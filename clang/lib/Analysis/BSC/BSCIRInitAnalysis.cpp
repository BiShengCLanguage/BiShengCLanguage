//===- BSCIRInitAnalysis.cpp - Initialization analysis on BSCIR -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P2795-style initialization analysis. In _Safe zones, ALL variables must be
// definitely initialized before use (not just _Owned types). This is a
// forward dataflow analysis computing definite initialization.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/Analysis/Analyses/BSC/BSCIRInitAnalysis.h"
#include "clang/Analysis/Analyses/BSC/BSCIRDataflow.h"
#include "clang/AST/Attr.h"
#include "clang/AST/BSC/DeclBSC.h"
#include "clang/Basic/Builtins.h"

using namespace clang;
using namespace clang::bscir;

/// Check if a type is the builtin va_list type.
static bool isVaListType(QualType Ty, const ASTContext &Ctx) {
  return Ctx.hasSameType(Ty, Ctx.getBuiltinVaListType());
}

/// Check if a local should be treated as implicitly initialized:
/// globals/statics or va_list type.
static bool isImplicitlyInitialized(const LocalDecl &LD, const Body &B) {
  if (LD.OriginalDecl) {
    if (LD.OriginalDecl->hasGlobalStorage())
      return true;
  }
  if (B.SourceFD)
    if (isVaListType(LD.Ty, B.SourceFD->getASTContext()))
      return true;
  if (InitAnalysis::isVacuouslyInitialized(LD.Ty))
    return true;
  return false;
}

//===----------------------------------------------------------------------===//
// Shared helpers for ensure_init / ensure_init_if_ret tracking
//===----------------------------------------------------------------------===//

/// Fold an integer-constant Operand to int64_t. Returns false for non-constant
/// operands and for constants wider than int64_t (e.g. a _BitInt(N>64) literal),
/// where APInt::getSExtValue() would assert.
static bool foldConstOperand(const Operand &Op, int64_t &Out) {
  if (Op.K == Operand::Constant && Op.getConstVal().isInt()) {
    const llvm::APSInt &I = Op.getConstVal().getInt();
    if (I.getSignificantBits() > 64)
      return false;
    Out = I.getSExtValue();
    return true;
  }
  return false;
}

/// Fold a unary operator over an already-extracted integer value. Handles the
/// operators that can appear on a constant in this analysis; returns false for
/// any other operator.
static bool foldUnary(UnaryOperatorKind Op, int64_t Sub, int64_t &Out) {
  switch (Op) {
  case UO_Minus: Out = -Sub; return true;
  case UO_Plus:  Out = Sub;  return true;
  case UO_LNot:  Out = !Sub; return true;
  default:       return false;
  }
}

/// The base local of a bare-local `copy(_n)` operand (no projections), or None.
static llvm::Optional<LocalId> asCopiedLocal(const Operand &Op) {
  if (Op.K == Operand::Copy && Op.getPlace().Projections.empty())
    return Op.getPlace().Base;
  return llvm::None;
}

/// The base local an argument passes on whole, whether copied or moved.
static llvm::Optional<LocalId> asPassedLocal(const Operand &Op) {
  if ((Op.K == Operand::Copy || Op.K == Operand::Move) &&
      Op.getPlace().Projections.empty())
    return Op.getPlace().Base;
  return llvm::None;
}

/// The base local an rvalue copies through `dst = src` / `dst = (cast)src`.
static llvm::Optional<LocalId> asCopiedLocal(const Rvalue &R) {
  if (R.K == Rvalue::Use)
    return asCopiedLocal(R.getUse().Op);
  if (R.K == Rvalue::Cast)
    return asCopiedLocal(R.getCast().Op);
  return llvm::None;
}

/// Intersect \p Src into \p Dst by value: keep only keys present in both with
/// equal values. Returns true if \p Dst changed.
template <class MapT>
static bool intersectMapByValue(const MapT &Src, MapT &Dst) {
  MapT Merged;
  for (const auto &E : Dst) {
    auto It = Src.find(E.first);
    if (It != Src.end() && It->second == E.second)
      Merged[E.first] = E.second;
  }
  if (Merged.size() == Dst.size())
    return false;
  Dst = std::move(Merged);
  return true;
}

//===----------------------------------------------------------------------===//
// InitAnalysis: Dataflow Implementation
//===----------------------------------------------------------------------===//

InitLattice InitAnalysis::entryState(const Body &B) const {
  InitLattice State;

  // Return slot (_0) starts uninitialized
  State.LocalStates[LocalId{0}] = InitState::Uninitialized;

  // Parameters (1..NumParams) start initialized, including all nested fields
  for (unsigned I = 1; I <= B.NumParams; ++I) {
    State.LocalStates[LocalId{I}] = InitState::Initialized;
    // Recursively mark all fields as initialized for struct params.
    const LocalDecl &LD = B.getLocal(LocalId{I});
    SmallVector<unsigned, 4> Prefix;
    markAllFieldsInit(State, LocalId{I}, LD.Ty, Prefix);
  }

  // All other locals start uninitialized, except globals/statics and
  // va_list types which are treated as fully initialized.
  for (unsigned I = B.NumParams + 1; I < B.Locals.size(); ++I) {
    const LocalDecl &LD = B.getLocal(LocalId{I});
    if (isImplicitlyInitialized(LD, B)) {
      State.LocalStates[LocalId{I}] = InitState::Initialized;
      SmallVector<unsigned, 4> Prefix;
      markAllFieldsInit(State, LocalId{I}, LD.Ty, Prefix);
    } else {
      State.LocalStates[LocalId{I}] = InitState::Uninitialized;
    }
  }

  // For callee-side ensure_init / ensure_init_if_ret: *param starts uninitialized
  if (B.SourceFD) {
    for (unsigned I = 0; I < B.SourceFD->getNumParams(); ++I) {
      const ParmVarDecl *PVD = B.SourceFD->getParamDecl(I);
      if (PVD->hasAttr<EnsureInitAttr>() ||
          PVD->hasAttr<EnsureInitIfRetAttr>()) {
        QualType PointeeTy = PVD->getType()->getPointeeType();
        InitState Seed = isVacuouslyInitialized(PointeeTy)
                             ? InitState::Initialized
                             : InitState::Uninitialized;
        State.EnsureInitDerefStates[LocalId{I + 1}] = Seed;
      }
    }
  }

  return State;
}

bool InitAnalysis::transferStatement(const Statement &S,
                                     InitLattice &State) const {
  bool Changed = false;

  switch (S.K) {
  case Statement::Assign: {
    // Assignment to a place: mark destination as initialized
    if (S.getAssign().Dest.isLocal()) {
      LocalId Dest = S.getAssign().Dest.Base;
      auto It = State.LocalStates.find(Dest);
      if (It == State.LocalStates.end() ||
          It->second != InitState::Initialized) {
        State.LocalStates[Dest] = InitState::Initialized;
        Changed = true;
      }
    } else if (auto FP = getFieldPath(S.getAssign().Dest)) {
      // Array-typed fields cannot be initialized element-by-element.
      // They must be initialized via {} or __assume_initialized.
      if (getFieldType(FP->Base, FP->Indices)->isArrayType())
        break;
      markFieldInit(State, *FP, Changed);
    }
    // Note: array element writes (arr[i] = ...) do NOT mark the array as
    // initialized. Arrays must be initialized via initializer list or
    // __assume_initialized.

    // Callee-side ensure_init: a write through *param. A re-pointed param does
    // not promote (the write hits the new pointee, not the tracked one).
    AddressedPlace Dest = classifyAddressedPlace(S.getAssign().Dest);
    if (Dest.Pointee && Dest.Recognised &&
        !State.ReassignedParams.count(Dest.Path.Base)) {
      if (Dest.Path.Indices.empty()) {
        markPointeeFullyInit(State, Dest.Path.Base, Changed);
      } else if (!getFieldType(Dest.Path.Base, Dest.Path.Indices)
                      ->isArrayType()) {
        // Writing one element does not initialize the array.
        markFieldInit(State, Dest.Path, Changed);
      }
    }

    // Detect re-point (`param = ...`): record the site; the at-return check
    // decides whether the path is acceptable and notes it.
    if (S.getAssign().Dest.isLocal()) {
      LocalId DestId = S.getAssign().Dest.Base;
      auto It = State.EnsureInitDerefStates.find(DestId);
      if (It != State.EnsureInitDerefStates.end() &&
          It->second != InitState::Initialized)
        State.ReassignedParams[DestId].push_back(S.Loc);
    }

    // Caller-side ensure_init_if_ret tracking. Order matters: alias-invalidate
    // first (the local may be mutated via the alias), then drop Dest's old
    // facts, then derive new ones from the RHS.
    if (S.getAssign().Dest.isLocal()) {
      LocalId DestId = S.getAssign().Dest.Base;
      const Rvalue &Src = S.getAssign().Src;

      auto invalidateLocal = [&](LocalId L) {
        llvm::erase_if(State.PendingCondInits,
                       [&](const InitLattice::PendingCondInit &P) {
                         return P.RetLocal == L || P.OutParamLocal == L;
                       });
        State.ComparisonFacts.erase(L);
        State.KnownConstants.erase(L);
        SmallVector<LocalId, 4> ToErase;
        for (const auto &Entry : State.ComparisonFacts)
          if (Entry.second.ComparedLocal == L)
            ToErase.push_back(Entry.first);
        for (LocalId K : ToErase)
          State.ComparisonFacts.erase(K);
      };
      // Only address-of or a mutable borrow can change the local; an
      // immutable (&_Const) borrow must not invalidate the association.
      if (Src.K == Rvalue::AddressOf) {
        const Place &P = Src.getAddrOf().P;
        if (P.Projections.empty())
          invalidateLocal(P.Base);
      } else if (Src.K == Rvalue::Ref &&
                 Src.getRef().BK == BorrowKind::Mut) {
        const Place &P = Src.getRef().P;
        if (P.Projections.empty())
          invalidateLocal(P.Base);
      }

      // DestId is overwritten: drop every fact about it (and any fact that
      // compares against it).
      invalidateLocal(DestId);

      // extractConstInt also looks through KnownConstants so comparisons
      // routed via a constant-holding temp (e.g. `_t = UnaryOp(-, const
      // 1)` for `-1`) match.
      auto extractConstInt = [&](const Operand &Op, int64_t &Out) -> bool {
        if (foldConstOperand(Op, Out))
          return true;
        if (auto L = asCopiedLocal(Op)) {
          auto It = State.KnownConstants.find(*L);
          if (It != State.KnownConstants.end()) {
            Out = It->second;
            return true;
          }
        }
        return false;
      };
      {
        int64_t CV = 0;
        if (Src.K == Rvalue::Use && extractConstInt(Src.getUse().Op, CV)) {
          State.KnownConstants[DestId] = CV;
        } else if (Src.K == Rvalue::Cast &&
                   extractConstInt(Src.getCast().Op, CV)) {
          State.KnownConstants[DestId] = CV;
        } else if (Src.K == Rvalue::UnaryOp) {
          const auto &UO = Src.getUnOp();
          int64_t Sub = 0, Folded = 0;
          if (extractConstInt(UO.Sub, Sub) && foldUnary(UO.Op, Sub, Folded))
            State.KnownConstants[DestId] = Folded;
        }
      }

      if (Src.K == Rvalue::BinaryOp) {
        const auto &BinOp = Src.getBinOp();
        if (BinOp.Op == BO_EQ || BinOp.Op == BO_NE) {
          auto tryExtract = [&](const Operand &Local, const Operand &Const,
                                LocalId &OutLocal, int64_t &OutValue) -> bool {
            auto L = asCopiedLocal(Local);
            int64_t CV = 0;
            if (!L || !extractConstInt(Const, CV))
              return false;
            OutLocal = *L;
            OutValue = CV;
            return true;
          };
          LocalId CompLocal;
          int64_t CompValue;
          if (tryExtract(BinOp.LHS, BinOp.RHS, CompLocal, CompValue) ||
              tryExtract(BinOp.RHS, BinOp.LHS, CompLocal, CompValue)) {
            InitLattice::ComparisonFact CF;
            CF.ComparedLocal = CompLocal;
            CF.ComparedValue = CompValue;
            CF.IsEq = (BinOp.Op == BO_EQ);
            State.ComparisonFacts[DestId] = CF;
          }
        }
      }

      // Propagate PCIs and ComparisonFacts through `dst = src` and
      // `dst = (cast)src`. 
      if (auto SrcOpt = asCopiedLocal(Src)) {
        LocalId SrcId = *SrcOpt;
        SmallVector<InitLattice::PendingCondInit, 2> NewPCIs;
        for (const auto &PCI : State.PendingCondInits) {
          if (PCI.RetLocal == SrcId) {
            InitLattice::PendingCondInit NewPCI = PCI;
            NewPCI.RetLocal = DestId;
            NewPCIs.push_back(NewPCI);
          }
        }
        for (const auto &PCI : NewPCIs)
          State.PendingCondInits.push_back(PCI);
        auto FactIt = State.ComparisonFacts.find(SrcId);
        if (FactIt != State.ComparisonFacts.end()) {
          // Copy out before the insert: ComparisonFacts[DestId] may rehash the
          // map (DestId was erased above, so this always inserts), invalidating
          // FactIt and turning FactIt->second into a use-after-free.
          InitLattice::ComparisonFact Fact = FactIt->second;
          State.ComparisonFacts[DestId] = Fact;
        }
      }
    }
    break;
  }

  case Statement::StorageLive: {
    // New variable comes into scope: starts uninitialized,
    // unless implicitly initialized (globals/statics, va_list).
    LocalId SL = S.getStorageLocal();
    const LocalDecl &SLD = B.getLocal(SL);
    if (isImplicitlyInitialized(SLD, B)) {
      auto It = State.LocalStates.find(SL);
      if (It == State.LocalStates.end() ||
          It->second != InitState::Initialized) {
        State.LocalStates[SL] = InitState::Initialized;
        SmallVector<unsigned, 4> Prefix;
        markAllFieldsInit(State, SL, SLD.Ty, Prefix);
        Changed = true;
      }
    } else {
      auto It = State.LocalStates.find(SL);
      if (It == State.LocalStates.end() ||
          It->second != InitState::Uninitialized) {
        State.LocalStates[SL] = InitState::Uninitialized;
        Changed = true;
      }
      clearFieldStates(State, SL, Changed);
    }
    break;
  }

  case Statement::StorageDead: {
    // Variable goes out of scope: mark uninitialized
    LocalId SL = S.getStorageLocal();
    auto It = State.LocalStates.find(SL);
    if (It == State.LocalStates.end() ||
        It->second != InitState::Uninitialized) {
      State.LocalStates[SL] = InitState::Uninitialized;
      Changed = true;
    }
    clearFieldStates(State, SL, Changed);
    break;
  }

  case Statement::Nop:
    break;
  }

  return Changed;
}

InitLattice InitAnalysis::transferTerminator(const Terminator &T,
                                             const InitLattice &StateBeforeTerm,
                                             BasicBlockId Target) const {
  // Call terminators: the destination is initialized after the call
  if (T.K == Terminator::Call) {
    InitLattice Result = StateBeforeTerm;
    const auto &CD = T.getCall();
    if (CD.Dest.isLocal()) {
      Result.LocalStates[CD.Dest.Base] = InitState::Initialized;
    }

    // __assume_initialized: mark the addressed memory as initialized.
    // Supported argument shapes:
    //   &x          — whole local (and pointee if x is an ensure_init param)
    //   &x.f...     — specific field of a local struct
    //   &*p         — whole pointee of an ensure_init pointer param
    //   &p->f...    — specific field of an ensure_init pointer's struct pointee
    if (CD.Decl &&
        CD.Decl->getBuiltinID() == Builtin::BI__assume_initialized) {
      if (!CD.ArgPlaces.empty() && CD.ArgPlaces[0]) {
        bool Changed = false;
        markAddressedPlaceInit(Result, classifyAddressedPlace(*CD.ArgPlaces[0]),
                               /*CreditPointeeOfLocal=*/true, Changed);
      }
      return Result;
    }

    // Caller side: mark *param init for ensure_init params (attr on the
    // FunctionDecl, or ExtParameterInfo for indirect calls).
    {
      unsigned NumParams = 0;
      if (CD.Decl)
        NumParams = CD.Decl->getNumParams();
      else if (CD.CalleeProtoType)
        NumParams = CD.CalleeProtoType->getNumParams();

      // Stale facts about the return slot from prior calls must be
      // invalidated exactly once, before this call's PCIs are added, so
      // that multi-arg calls don't drop their own sibling PCIs.
      bool InvalidatedForThisCall = false;
      auto invalidateForThisCall = [&]() {
        if (InvalidatedForThisCall || !CD.Dest.isLocal())
          return;
        InvalidatedForThisCall = true;
        llvm::erase_if(Result.PendingCondInits,
                       [&](const InitLattice::PendingCondInit &P) {
                         return P.RetLocal == CD.Dest.Base;
                       });
        Result.ComparisonFacts.erase(CD.Dest.Base);
      };

      for (unsigned I = 0; I < NumParams; ++I) {
        int EIIRCondValue = 0;
        EnsureInitKind Kind =
            classifyEnsureInit(CD.Decl, CD.CalleeProtoType, I, EIIRCondValue);

        if (Kind == EnsureInitKind::EnsureInitIfRet) {
          // Conditional contract: mark nothing here, just record a
          // PendingCondInit. It is credited on the matching SwitchInt edge
          // or at the return. Marking unconditionally is unsound
          // (`r = inner(out); return 0;` does not establish *out).
          if (CD.Dest.isLocal()) {
            LocalId OutLocal{0};
            SmallVector<unsigned, 2> OutFields;
            bool Recognised = false;
            bool Pointee = false;
            AddressedPlace Addressed = classifyContractArg(CD, I);
            // A re-pointed param addresses a different pointee.
            if (Addressed.Recognised &&
                !(Addressed.Pointee &&
                  Result.ReassignedParams.count(Addressed.Path.Base))) {
              OutLocal = Addressed.Path.Base;
              OutFields.assign(Addressed.Path.Indices.begin(),
                               Addressed.Path.Indices.end());
              Pointee = Addressed.Pointee;
              Recognised = true;
            }
            if (Recognised) {
              InitLattice::PendingCondInit PCI;
              PCI.OutParamLocal = OutLocal;
              PCI.OutFieldIndices = std::move(OutFields);
              PCI.RetLocal = CD.Dest.Base;
              PCI.CondValue = EIIRCondValue;
              PCI.Pointee = Pointee;
              invalidateForThisCall();
              llvm::erase_if(Result.PendingCondInits,
                             [&](const InitLattice::PendingCondInit &P) {
                               return P.OutParamLocal == PCI.OutParamLocal &&
                                      P.OutFieldIndices == PCI.OutFieldIndices;
                             });
              Result.PendingCondInits.push_back(std::move(PCI));
            }
          }
          continue;
        }

        if (Kind != EnsureInitKind::EnsureInit)
          continue;

        // The callee initializes whichever place the argument names.
        bool Changed = false;
        markAddressedPlaceInit(Result, classifyContractArg(CD, I),
                               /*CreditPointeeOfLocal=*/false, Changed);
      }
    }

    return Result;
  }

  // SwitchInt: resolve pending conditional inits on matching edges.
  if (T.K == Terminator::SwitchInt) {
    InitLattice Result = StateBeforeTerm;
    const auto &SW = T.getSwitchInt();
    if (Result.PendingCondInits.empty())
      return Result;

    if (SW.Discriminant.K != Operand::Copy ||
        !SW.Discriminant.getPlace().Projections.empty())
      return Result;

    LocalId DiscLocal = SW.Discriminant.getPlace().Base;

    // The BSCIR builder may emit either `[0: false, otherwise: true]` or
    // `[1: true, otherwise: false]` for a boolean comparison, so the
    // edge-to-truth-value mapping must be computed per-target rather
    // than assumed.
    auto classifyEdge = [&](BasicBlockId Tgt,
                            bool &IsTrueEdge, bool &IsFalseEdge) {
      IsTrueEdge = false;
      IsFalseEdge = false;
      bool ZeroInList = false;
      for (const auto &Pair : SW.Targets) {
        bool TgtIsZero = Pair.first.getZExtValue() == 0;
        if (TgtIsZero)
          ZeroInList = true;
        if (Pair.second == Tgt) {
          if (TgtIsZero)
            IsFalseEdge = true;
          else
            IsTrueEdge = true;
        }
      }
      if (Tgt == SW.Otherwise) {
        if (ZeroInList)
          IsTrueEdge = true;
        else
          IsFalseEdge = true;
      }
    };

    auto FactIt = Result.ComparisonFacts.find(DiscLocal);
    if (FactIt != Result.ComparisonFacts.end()) {
      LocalId CompLocal = FactIt->second.ComparedLocal;
      int64_t CompValue = FactIt->second.ComparedValue;
      bool IsEq = FactIt->second.IsEq;

      bool IsTrueEdge, IsFalseEdge;
      classifyEdge(Target, IsTrueEdge, IsFalseEdge);

      for (const auto &PCI : Result.PendingCondInits) {
        if (PCI.RetLocal != CompLocal || PCI.CondValue != CompValue)
          continue;
        // `CompLocal == CompValue` is proven on the true-edge of an EQ
        // tracker or on the false-edge of a NE tracker.
        if ((IsEq && IsTrueEdge) || (!IsEq && IsFalseEdge)) {
          if (PCI.OutFieldIndices.empty() && PCI.Pointee) {
            // Matching branch => inner returned cond => the pointee is init.
            auto DerefIt = Result.EnsureInitDerefStates.find(PCI.OutParamLocal);
            if (DerefIt != Result.EnsureInitDerefStates.end())
              DerefIt->second = InitState::Initialized;
          } else if (PCI.OutFieldIndices.empty()) {
            Result.LocalStates[PCI.OutParamLocal] = InitState::Initialized;
            SmallVector<unsigned, 4> Prefix;
            markAllFieldsInit(Result, PCI.OutParamLocal,
                              B.getLocal(PCI.OutParamLocal).Ty, Prefix);
          } else {
            FieldPath FP;
            FP.Base = PCI.OutParamLocal;
            FP.Indices.assign(PCI.OutFieldIndices.begin(),
                              PCI.OutFieldIndices.end());
            bool Changed = false;
            markFieldInit(Result, FP, Changed);
          }
        }
      }
      return Result;
    }

    // Patterns without a recognised ComparisonFact (e.g. `if (ok)`,
    // `if (!ok)`) are not one of the four spec-supported forms — *out
    // stays in its incoming state on every edge.
    return Result;
  }

  // For all other terminators, pass through unchanged
  return StateBeforeTerm;
}

/// Compute the meet of two init states (intersection semantics).
static InitState meetStates(InitState A, InitState B) {
  if (A == B)
    return A;
  // Any combination of different states produces MaybeInit:
  // Init + Uninit, Init + MaybeInit, or MaybeInit + Uninit.
  return InitState::MaybeInit;
}

bool InitAnalysis::merge(const InitLattice &Src, InitLattice &Dst) const {
  bool Changed = false;

  // Merge LocalStates
  for (const auto &Entry : Src.LocalStates) {
    auto It = Dst.LocalStates.find(Entry.first);
    if (It == Dst.LocalStates.end()) {
      Dst.LocalStates[Entry.first] = Entry.second;
      Changed = true;
    } else {
      InitState NewState = meetStates(Entry.second, It->second);
      if (NewState != It->second) {
        It->second = NewState;
        Changed = true;
      }
    }
  }

  // Merge FieldStates: missing entries on either side treated as Uninitialized.
  for (const auto &Entry : Src.FieldStates) {
    auto It = Dst.FieldStates.find(Entry.first);
    if (It == Dst.FieldStates.end()) {
      // Dst missing = Uninitialized on Dst side
      InitState NewState = meetStates(Entry.second, InitState::Uninitialized);
      Dst.FieldStates[Entry.first] = NewState;
      Changed = true;
    } else {
      InitState NewState = meetStates(Entry.second, It->second);
      if (NewState != It->second) {
        It->second = NewState;
        Changed = true;
      }
    }
  }

  // Handle field entries in Dst that are not in Src: treat Src as Uninitialized.
  for (auto &Entry : Dst.FieldStates) {
    if (Src.FieldStates.find(Entry.first) == Src.FieldStates.end()) {
      InitState NewState = meetStates(InitState::Uninitialized, Entry.second);
      if (NewState != Entry.second) {
        Entry.second = NewState;
        Changed = true;
      }
    }
  }

  // Merge ReassignedParams: union (a re-point on any path freezes promotion);
  // collect both sides' sites for the at-return note.
  for (const auto &Entry : Src.ReassignedParams) {
    auto &DstLocs = Dst.ReassignedParams[Entry.first];
    size_t Before = DstLocs.size();
    for (SourceLocation L : Entry.second)
      if (!llvm::is_contained(DstLocs, L))
        DstLocs.push_back(L);
    if (DstLocs.size() != Before)
      Changed = true;
  }

  // Merge EnsureInitDerefStates
  for (const auto &Entry : Src.EnsureInitDerefStates) {
    auto It = Dst.EnsureInitDerefStates.find(Entry.first);
    if (It == Dst.EnsureInitDerefStates.end()) {
      Dst.EnsureInitDerefStates[Entry.first] = Entry.second;
      Changed = true;
    } else {
      InitState NewState = meetStates(Entry.second, It->second);
      if (NewState != It->second) {
        It->second = NewState;
        Changed = true;
      }
    }
  }


  // Merge PendingCondInits: intersection (keep only those in both)
  {
    SmallVector<InitLattice::PendingCondInit, 2> Merged;
    for (const auto &PCI : Src.PendingCondInits)
      if (llvm::is_contained(Dst.PendingCondInits, PCI))
        Merged.push_back(PCI);
    if (Merged.size() != Dst.PendingCondInits.size() ||
        Merged != Dst.PendingCondInits) {
      Dst.PendingCondInits = std::move(Merged);
      Changed = true;
    }
  }

  // ComparisonFacts / KnownConstants: a fact holds only when every incoming
  // path agrees on it, so intersect by value.
  if (intersectMapByValue(Src.ComparisonFacts, Dst.ComparisonFacts))
    Changed = true;
  if (intersectMapByValue(Src.KnownConstants, Dst.KnownConstants))
    Changed = true;

  return Changed;
}

//===----------------------------------------------------------------------===//
// Diagnostic Helpers
//===----------------------------------------------------------------------===//

InitAnalysis::AddressedPlace
InitAnalysis::classifyAddressedPlace(const Place &P) const {
  AddressedPlace Addressed;
  Addressed.Path.Base = P.Base;

  if (P.Projections.empty()) {
    Addressed.Recognised = true;
    return Addressed;
  }

  if (P.Projections[0].K != ProjectionElem::Deref) {
    if (auto FP = getFieldPath(P)) {
      Addressed.Path = *FP;
      Addressed.Recognised = true;
    }
    return Addressed;
  }

  Addressed.Pointee = true;
  if (P.Projections.size() == 1) {
    Addressed.Recognised = true;
    return Addressed;
  }
  if (getEnsureInitPointeeType(P.Base).isNull())
    return Addressed;
  Place SubPlace(P.Base, P.Projections.slice(1), P.Ty, P.Loc);
  if (auto FP = getFieldPath(SubPlace)) {
    Addressed.Path = *FP;
    Addressed.Recognised = true;
  }
  return Addressed;
}

InitAnalysis::AddressedPlace
InitAnalysis::classifyContractArg(const Terminator::CallData &CD,
                                  unsigned I) const {
  if (I < CD.ArgPlaces.size() && CD.ArgPlaces[I])
    return classifyAddressedPlace(*CD.ArgPlaces[I]);

  AddressedPlace Addressed;
  // The pointer is handed on, so the callee's contract covers our pointee.
  if (I < CD.Args.size())
    if (auto ArgBase = asPassedLocal(CD.Args[I])) {
      Addressed.Recognised = true;
      Addressed.Pointee = true;
      Addressed.Path.Base = *ArgBase;
    }
  return Addressed;
}

void InitAnalysis::markAddressedPlaceInit(InitLattice &State,
                                          const AddressedPlace &Addressed,
                                          bool CreditPointeeOfLocal,
                                          bool &Changed) const {
  LocalId Base = Addressed.Path.Base;
  // A re-pointed param's `&*p` / `&p->f` denotes the new pointee.
  bool Repointed = State.ReassignedParams.count(Base);
  if (!Addressed.Recognised || (Addressed.Pointee && Repointed))
    return;

  if (!Addressed.Path.Indices.empty()) {
    markFieldInit(State, Addressed.Path, Changed);
  } else if (Addressed.Pointee) {
    markPointeeFullyInit(State, Base, Changed);
  } else {
    State.LocalStates[Base] = InitState::Initialized;
    SmallVector<unsigned, 4> Prefix;
    markAllFieldsInit(State, Base, B.getLocal(Base).Ty, Prefix);
    if (CreditPointeeOfLocal && !Repointed)
      markPointeeFullyInit(State, Base, Changed);
  }
}

QualType InitAnalysis::getEnsureInitPointeeType(LocalId Id) const {
  if (Id.Index < 1 || Id.Index > B.NumParams)
    return QualType();
  if (!B.SourceFD)
    return QualType();
  const ParmVarDecl *PVD = B.SourceFD->getParamDecl(Id.Index - 1);
  if (!PVD->hasAttr<EnsureInitAttr>() &&
      !PVD->hasAttr<EnsureInitIfRetAttr>())
    return QualType();
  QualType PointeeTy = PVD->getType()->getPointeeType();
  if (PointeeTy.isNull() || !PointeeTy->isRecordType())
    return QualType();
  // getNumFields is 0 for a union by convention, not for want of fields.
  const RecordDecl *PointeeRD = PointeeTy->getAsRecordDecl();
  if (!PointeeRD->isUnion() && getNumFields(PointeeTy) == 0)
    return QualType();
  return PointeeTy;
}

llvm::Optional<int> InitAnalysis::getIfRetCondValue(LocalId Id) const {
  if (Id.Index < 1 || Id.Index > B.NumParams || !B.SourceFD)
    return llvm::None;
  const ParmVarDecl *PVD = B.SourceFD->getParamDecl(Id.Index - 1);
  if (auto *A = PVD->getAttr<EnsureInitIfRetAttr>())
    return A->getCondValue();
  return llvm::None;
}

InitState InitAnalysis::getInitState(const InitLattice &State,
                                     LocalId Id) const {
  auto It = State.LocalStates.find(Id);
  if (It == State.LocalStates.end())
    return InitState::Uninitialized;
  return It->second;
}

void InitAnalysis::markInit(InitLattice &State, LocalId Id) const {
  State.LocalStates[Id] = InitState::Initialized;
}

void InitAnalysis::markUninit(InitLattice &State, LocalId Id) const {
  State.LocalStates[Id] = InitState::Uninitialized;
}

//===----------------------------------------------------------------------===//
// Field-Level Init Tracking Helpers (Recursive)
//===----------------------------------------------------------------------===//

/// Field declaration at index \p Idx of \p RD, or null if out of range.
static const FieldDecl *getFieldAt(const RecordDecl *RD, unsigned Idx) {
  unsigned I = 0;
  for (auto It = RD->field_begin(); It != RD->field_end(); ++It, ++I)
    if (I == Idx)
      return *It;
  return nullptr;
}

unsigned InitAnalysis::getNumFields(QualType Ty) {
  const RecordDecl *RD = Ty->getAsRecordDecl();
  if (!RD || RD->isUnion())
    return 0;
  unsigned Count = 0;
  for (auto It = RD->field_begin(); It != RD->field_end(); ++It)
    ++Count;
  return Count;
}

bool InitAnalysis::isVacuouslyInitialized(QualType Ty) {
  const RecordDecl *RD = Ty->getAsRecordDecl();
  if (!RD || RD->isUnion())
    return false;
  for (auto It = RD->field_begin(); It != RD->field_end(); ++It) {
    if (!isVacuouslyInitialized(It->getType()))
      return false;
  }
  return true;
}

QualType InitAnalysis::getFieldType(LocalId Id,
                                    ArrayRef<unsigned> Path) const {
  // For ensure_init struct pointees, use the pointee type.
  QualType PointeeTy = getEnsureInitPointeeType(Id);
  QualType Ty = PointeeTy.isNull() ? B.getLocal(Id).Ty : PointeeTy;
  for (unsigned Idx : Path) {
    const RecordDecl *RD = Ty->getAsRecordDecl();
    assert(RD && "getFieldType: expected record type along path");
    if (const FieldDecl *FD = getFieldAt(RD, Idx))
      Ty = FD->getType();
  }
  return Ty;
}

llvm::Optional<FieldPath>
InitAnalysis::getFieldPath(const Place &P) const {
  auto FP = getFieldPathPrefix(P);
  if (!FP || FP->Indices.size() != P.Projections.size())
    return llvm::None;
  return FP;
}

llvm::Optional<FieldPath>
InitAnalysis::getFieldPathPrefix(const Place &P) const {
  if (P.Projections.empty())
    return llvm::None;
  if (P.Projections[0].K != ProjectionElem::Field)
    return llvm::None;

  FieldPath FP;
  FP.Base = P.Base;

  // Walk the local's type through the field/index chain.
  // For ensure_init struct pointees, use the pointee type.
  QualType PointeeTy = getEnsureInitPointeeType(P.Base);
  QualType CurTy = PointeeTy.isNull() ? B.getLocal(P.Base).Ty : PointeeTy;

  for (const auto &Proj : P.Projections) {
    if (Proj.K == ProjectionElem::Field) {
      const RecordDecl *RD = CurTy->getAsRecordDecl();
      if (!RD)
        break;
      // Continue through unions into their variants.
      FP.Indices.push_back(Proj.FieldIndex);
      CurTy = Proj.ResultTy;
    } else {
      break;
    }
  }

  if (FP.Indices.empty())
    return llvm::None;

  return FP;
}

InitState InitAnalysis::getFieldInitState(const InitLattice &State,
                                          const FieldPath &FP) const {
  auto It = State.FieldStates.find(FP);
  if (It == State.FieldStates.end())
    return InitState::Uninitialized;
  return It->second;
}

void InitAnalysis::markFieldInit(InitLattice &State, const FieldPath &FP,
                                 bool &Changed) const {
  // Only a whole-variant write to the outermost union on the path covers it.
  if (auto FirstU = firstUnionDepth(FP)) {
    if (*FirstU + 1 != FP.Indices.size())
      return;
    if (*FirstU == 0) {
      // A contract pointee's state lives in EnsureInitDerefStates.
      auto DerefIt = State.EnsureInitDerefStates.find(FP.Base);
      InitState &Target = (DerefIt != State.EnsureInitDerefStates.end())
                              ? DerefIt->second
                              : State.LocalStates[FP.Base];
      if (Target != InitState::Initialized) {
        Target = InitState::Initialized;
        Changed = true;
      }
      return;
    }
    FieldPath UnionFP;
    UnionFP.Base = FP.Base;
    UnionFP.Indices.assign(FP.Indices.begin(), FP.Indices.begin() + *FirstU);
    markFieldInit(State, UnionFP, Changed);
    return;
  }

  auto &FS = State.FieldStates[FP];
  if (FS != InitState::Initialized) {
    FS = InitState::Initialized;
    Changed = true;
  }
  tryPromoteParent(State, FP, Changed);
}

void InitAnalysis::markFieldInit(InitLattice &State, const Place &P,
                                 bool &Changed) const {
  if (auto FP = getFieldPath(P))
    markFieldInit(State, *FP, Changed);
}

void InitAnalysis::tryPromoteParent(InitLattice &State, const FieldPath &FP,
                                    bool &Changed) const {
  if (FP.Indices.empty())
    return;

  // Build the parent path (everything except the last index).
  FieldPath Parent;
  Parent.Base = FP.Base;
  Parent.Indices.assign(FP.Indices.begin(), FP.Indices.end() - 1);

  // Get the parent type to count siblings.
  // getFieldType handles ensure_init pointee types for empty paths.
  QualType ParentTy = getFieldType(FP.Base, Parent.Indices);

  unsigned NumSiblings = getNumFields(ParentTy);
  // Non-record parents and unions never promote from below.
  if (NumSiblings == 0)
    return;

  // Check if all siblings at this level are Initialized.
  for (unsigned I = 0; I < NumSiblings; ++I) {
    FieldPath Sibling;
    Sibling.Base = FP.Base;
    Sibling.Indices = Parent.Indices;
    Sibling.Indices.push_back(I);
    if (getFieldInitState(State, Sibling) == InitState::Initialized)
      continue;
    if (isVacuouslyInitialized(getFieldType(FP.Base, Sibling.Indices)))
      continue;
    return; // Not all siblings initialized yet.
  }

  // All siblings initialized. Promote parent.
  if (Parent.Indices.empty()) {
    // Promote the whole local, or EnsureInitDerefStates for pointee tracking.
    auto DerefIt = State.EnsureInitDerefStates.find(FP.Base);
    InitState &Target = (DerefIt != State.EnsureInitDerefStates.end())
                            ? DerefIt->second
                            : State.LocalStates[FP.Base];
    if (Target != InitState::Initialized) {
      Target = InitState::Initialized;
      Changed = true;
    }
  } else {
    // Mark the parent field path as Initialized.
    auto &PS = State.FieldStates[Parent];
    if (PS != InitState::Initialized) {
      PS = InitState::Initialized;
      Changed = true;
    }
    // Recurse upward.
    tryPromoteParent(State, Parent, Changed);
  }
}

void InitAnalysis::clearFieldStates(InitLattice &State, LocalId Id,
                                    bool &Changed) const {
  // Erase all entries with matching Base using std::map's ordered iteration.
  // FieldPath ordering is Base first, so we can use lower_bound.
  FieldPath LowKey;
  LowKey.Base = Id;
  // Empty Indices sorts before any non-empty Indices.

  auto It = State.FieldStates.lower_bound(LowKey);
  while (It != State.FieldStates.end() && It->first.Base == Id) {
    It = State.FieldStates.erase(It);
    Changed = true;
  }
}

void InitAnalysis::markPointeeFullyInit(InitLattice &State, LocalId Base,
                                        bool &Changed) const {
  auto It = State.EnsureInitDerefStates.find(Base);
  if (It == State.EnsureInitDerefStates.end())
    return;
  if (It->second != InitState::Initialized) {
    It->second = InitState::Initialized;
    Changed = true;
  }
  QualType PointeeTy = getEnsureInitPointeeType(Base);
  if (!PointeeTy.isNull()) {
    SmallVector<unsigned, 4> Prefix;
    markAllFieldsInit(State, Base, PointeeTy, Prefix);
  }
}

void InitAnalysis::markAllFieldsInit(InitLattice &State, LocalId Base,
                                     QualType Ty,
                                     SmallVector<unsigned, 4> &Prefix) const {
  const RecordDecl *RD = Ty->getAsRecordDecl();
  if (!RD)
    return;

  unsigned FIdx = 0;
  for (auto It = RD->field_begin(); It != RD->field_end(); ++It, ++FIdx) {
    Prefix.push_back(FIdx);

    QualType FieldTy = It->getType();
    const RecordDecl *FieldRD = FieldTy->getAsRecordDecl();
    // Recurse into nested structs; unions are covered whole, not per-variant.
    if (FieldRD && !FieldRD->isUnion())
      markAllFieldsInit(State, Base, FieldTy, Prefix);

    // Mark this field path as initialized (leaf or intermediate).
    FieldPath FP;
    FP.Base = Base;
    FP.Indices.assign(Prefix.begin(), Prefix.end());
    State.FieldStates[FP] = InitState::Initialized;

    Prefix.pop_back();
  }
}

std::string InitAnalysis::buildFieldName(const FieldPath &FP) const {
  const LocalDecl &LD = B.getLocal(FP.Base);
  std::string Name = LD.Name.str();

  QualType CurTy = LD.Ty;
  for (unsigned Idx : FP.Indices) {
    const RecordDecl *RD = CurTy->getAsRecordDecl();
    if (!RD)
      break;
    const FieldDecl *FD = getFieldAt(RD, Idx);
    if (!FD)
      break;
    if (!FD->isAnonymousStructOrUnion())
      Name += "." + FD->getNameAsString();
    CurTy = FD->getType();
  }
  return Name;
}

llvm::Optional<unsigned>
InitAnalysis::firstUnionDepth(const FieldPath &FP) const {
  QualType CurTy = getFieldType(FP.Base, {});
  for (unsigned I = 0; I < FP.Indices.size(); ++I) {
    const RecordDecl *RD = CurTy->getAsRecordDecl();
    if (!RD)
      break;
    if (RD->isUnion())
      return I;
    const FieldDecl *FD = getFieldAt(RD, FP.Indices[I]);
    if (!FD)
      break;
    CurTy = FD->getType();
  }
  return llvm::None;
}

void InitAnalysis::checkOperand(const Operand &Op, const InitLattice &State,
                                SourceLocation Loc,
                                SmallVectorImpl<InitDiagInfo> &Diags) const {
  if (Op.K == Operand::Constant)
    return;

  // Array-element reads of owned-element arrays are validated by the
  // ownership dataflow (elements are initialized through a qualifying
  // for-loop); the init analysis tracks whole locals / fields only, so a
  // loop-initialized owned array would be misreported as uninitialized
  // here (e.g. `for (i) a[i] = safe_malloc(i); for (i) safe_free(a[i])`).
  // Ordinary (non-owned) arrays keep the uninit check.
  const Place &P = Op.getPlace();
  if (llvm::any_of(P.Projections, [](const ProjectionElem &E) {
        return E.K == ProjectionElem::Index ||
               E.K == ProjectionElem::ConstantIndex;
      })) {
    if (P.Ty.isOwnedQualified() || P.Ty->isMoveSemanticType())
      return;
  }

  if (P.Loc.isValid())
    Loc = P.Loc;

  LocalId Id = P.Base;
  InitState IS = getInitState(State, Id);

  // An Initialized local (for unions: covered) makes every field access fine.
  if (IS == InitState::Initialized)
    return;

  // Check field-level state if the operand has a field projection.
  if (auto FP = getFieldPathPrefix(Op.getPlace())) {
    if (isVacuouslyInitialized(getFieldType(FP->Base, FP->Indices)))
      return;

    InitState FS = getFieldInitState(State, *FP);
    if (FS == InitState::Initialized)
      return;

    // Check if any ancestor field path is initialized (e.g., whole struct
    // or union field assigned covers all sub-paths).
    {
      FieldPath Ancestor;
      Ancestor.Base = FP->Base;
      bool AncestorInit = false;
      for (unsigned i = 0; i + 1 < FP->Indices.size(); ++i) {
        Ancestor.Indices.push_back(FP->Indices[i]);
        if (getFieldInitState(State, Ancestor) == InitState::Initialized) {
          AncestorInit = true;
          break;
        }
      }
      if (AncestorInit)
        return;
    }

    const LocalDecl &LD = B.getLocal(Id);
    if (LD.IsTemp || LD.Name.empty())
      return;

    // Nothing inside an uncovered union is readable; name that union.
    if (auto FirstU = firstUnionDepth(*FP)) {
      FieldPath UnionFP;
      UnionFP.Base = FP->Base;
      UnionFP.Indices.assign(FP->Indices.begin(),
                             FP->Indices.begin() + *FirstU);
      InitState US =
          *FirstU == 0 ? IS : getFieldInitState(State, UnionFP);
      Diags.emplace_back(US == InitState::MaybeInit
                             ? InitDiagKind::UseOfMaybeUninit
                             : InitDiagKind::UseOfUninit,
                         Loc.isValid() ? Loc : LD.DeclLoc,
                         buildFieldName(UnionFP));
      return;
    }

    // Build field-qualified name: "o.inner.b"
    std::string FieldName = buildFieldName(*FP);

    if (FS == InitState::Uninitialized) {
      Diags.emplace_back(InitDiagKind::UseOfUninit,
                         Loc.isValid() ? Loc : LD.DeclLoc, FieldName);
    } else {
      Diags.emplace_back(InitDiagKind::UseOfMaybeUninit,
                         Loc.isValid() ? Loc : LD.DeclLoc, FieldName);
    }
    return;
  }

  // Whole-local check (existing logic).
  if (IS == InitState::Uninitialized) {
    const LocalDecl &LD = B.getLocal(Id);
    if (!LD.IsTemp && !LD.Name.empty()) {
      Diags.emplace_back(InitDiagKind::UseOfUninit,
                         Loc.isValid() ? Loc : LD.DeclLoc, LD.Name);
    }
  } else if (IS == InitState::MaybeInit) {
    const LocalDecl &LD = B.getLocal(Id);
    if (!LD.IsTemp && !LD.Name.empty()) {
      Diags.emplace_back(InitDiagKind::UseOfMaybeUninit,
                         Loc.isValid() ? Loc : LD.DeclLoc, LD.Name);
    }
  }
}

//===----------------------------------------------------------------------===//
// Ensure-Init Helpers
//===----------------------------------------------------------------------===//

/// Visit all operands in an Rvalue, calling F on each.
template <typename Fn>
static void forEachRvalueOperand(const Rvalue &Src, Fn &&F) {
  switch (Src.K) {
  case Rvalue::Use:
    F(Src.getUse().Op);
    break;
  case Rvalue::BinaryOp:
    F(Src.getBinOp().LHS);
    F(Src.getBinOp().RHS);
    break;
  case Rvalue::UnaryOp:
    F(Src.getUnOp().Sub);
    break;
  case Rvalue::Cast:
    F(Src.getCast().Op);
    break;
  case Rvalue::Aggregate:
    for (const Operand &Field : Src.getAgg().Fields)
      F(Field);
    break;
  case Rvalue::Array:
    for (const Operand &El : Src.getArray().Elements)
      F(El);
    break;
  default:
    break;
  }
}

/// True when projection \p I dereferences the prefix built before it.
static bool projectionLoadsPointer(const Place &P, unsigned I,
                                   QualType BaseTy) {
  QualType PrefixTy = I == 0 ? BaseTy : P.Projections[I - 1].ResultTy;
  switch (P.Projections[I].K) {
  case ProjectionElem::Deref:
    return true;
  case ProjectionElem::Index:
  case ProjectionElem::ConstantIndex:
    return !PrefixTy.isNull() && PrefixTy->isPointerType();
  case ProjectionElem::Field:
    return false;
  }
  return false;
}

llvm::DenseSet<LocalId>
InitAnalysis::collectEnsureInitArgTemps(const BasicBlock &BB) const {
  llvm::DenseSet<LocalId> Result;
  if (BB.Term.K != Terminator::Call)
    return Result;

  const auto &CD = BB.Term.getCall();
  llvm::DenseSet<LocalId> ExemptArgBases;
  if (CD.Decl &&
      CD.Decl->getBuiltinID() == Builtin::BI__assume_initialized) {
    for (const Operand &Arg : CD.Args)
      if (Arg.K == Operand::Copy || Arg.K == Operand::Move)
        ExemptArgBases.insert(Arg.getPlace().Base);
  }
  unsigned NumParams = CD.Decl
                           ? CD.Decl->getNumParams()
                           : (CD.CalleeProtoType
                                  ? CD.CalleeProtoType->getNumParams()
                                  : 0);
  for (unsigned I = 0; I < NumParams && I < CD.Args.size(); ++I) {
    int Cond = 0;
    bool IsAI = classifyEnsureInit(CD.Decl, CD.CalleeProtoType, I, Cond) !=
                EnsureInitKind::None;
    if (IsAI && (CD.Args[I].K == Operand::Copy ||
                 CD.Args[I].K == Operand::Move))
      ExemptArgBases.insert(CD.Args[I].getPlace().Base);
  }
  // Find statements that are AddressOf/Ref producing these temps.
  for (const Statement &S : BB.Statements) {
    if (S.K != Statement::Assign || !S.getAssign().Dest.isLocal())
      continue;
    LocalId DestId = S.getAssign().Dest.Base;
    if (!ExemptArgBases.count(DestId))
      continue;
    const Rvalue &Src = S.getAssign().Src;
    if (Src.K == Rvalue::AddressOf || Src.K == Rvalue::Ref)
      Result.insert(DestId);
  }
  return Result;
}

void InitAnalysis::checkEnsureInitAssign(
    const Statement &S, const InitLattice &State,
    llvm::DenseMap<LocalId, LocalId> &TempToEnsureInitParam,
    llvm::DenseMap<LocalId, LocalId> &TempToEnsureInitParamAddr,
    SmallVectorImpl<InitDiagInfo> &Diags) const {
  // A store into an aggregate escapes the pointer just as a named copy does.
  bool DestIsWholeLocal = S.getAssign().Dest.isLocal();
  LocalId DestId = S.getAssign().Dest.Base;

  // Re-pointing is deferred to the at-return check; ReassignedParams
  // gating keeps it sound, and the failing return gets the error + note.

  // Reject aliasing into a named variable before *param is init (the alias
  // can't be tracked across blocks); temp copies are tracked, not rejected.
  auto noteAlias = [&](LocalId ParamId, bool IsAddress) {
    auto DerefIt = State.EnsureInitDerefStates.find(ParamId);
    if (DerefIt == State.EnsureInitDerefStates.end() ||
        DerefIt->second == InitState::Initialized)
      return;
    const LocalDecl &DestLD = B.getLocal(DestId);
    if (DestLD.IsTemp) {
      if (!DestIsWholeLocal)
        return;
      if (IsAddress)
        TempToEnsureInitParamAddr[DestId] = ParamId;
      else
        TempToEnsureInitParam[DestId] = ParamId;
      return;
    }
    const LocalDecl &ParamLD = B.getLocal(ParamId);
    if (DestLD.Name.empty() || ParamLD.Name.empty())
      return;
    Diags.emplace_back(InitDiagKind::EnsureInitPtrAliased,
                       S.Loc.isValid() ? S.Loc : DestLD.DeclLoc, ParamLD.Name);
    Diags.back().AttrSelect = getIfRetCondValue(ParamId) ? 1 : 0;
  };

  const Rvalue &Src = S.getAssign().Src;

  // `&*p` is p itself, so it aliases exactly what `q = p` does.
  if (Src.K == Rvalue::AddressOf || Src.K == Rvalue::Ref) {
    const Place &SrcPlace =
        Src.K == Rvalue::AddressOf ? Src.getAddrOf().P : Src.getRef().P;
    // A reborrow's place is the pointer, but the result carries its value.
    bool IsReborrow = Src.K == Rvalue::Ref && Src.getRef().IsReborrow;
    if (SrcPlace.Projections.empty())
      noteAlias(SrcPlace.Base, /*IsAddress=*/!IsReborrow);
    else if (SrcPlace.Projections.size() == 1 &&
             SrcPlace.Projections[0].K == ProjectionElem::Deref)
      noteAlias(SrcPlace.Base, /*IsAddress=*/false);
    return;
  }

  if (Src.K != Rvalue::Use)
    return;
  const Operand &Op = Src.getUse().Op;
  if (Op.K != Operand::Copy)
    return;
  const Place &SrcPlace = Op.getPlace();

  // `t = *a` where a holds `&p`: t now carries p's own pointer value.
  if (SrcPlace.Projections.size() == 1 &&
      SrcPlace.Projections[0].K == ProjectionElem::Deref) {
    auto AddrIt = TempToEnsureInitParamAddr.find(SrcPlace.Base);
    if (AddrIt != TempToEnsureInitParamAddr.end())
      noteAlias(AddrIt->second, /*IsAddress=*/false);
    return;
  }
  if (!SrcPlace.Projections.empty())
    return;

  // A direct contract param, or a temp alias of its value or address.
  LocalId SrcId = SrcPlace.Base;
  auto AddrIt = TempToEnsureInitParamAddr.find(SrcId);
  if (AddrIt != TempToEnsureInitParamAddr.end()) {
    noteAlias(AddrIt->second, /*IsAddress=*/true);
    return;
  }
  LocalId ParamId = SrcId;
  auto AliasIt = TempToEnsureInitParam.find(SrcId);
  if (AliasIt != TempToEnsureInitParam.end())
    ParamId = AliasIt->second;
  noteAlias(ParamId, /*IsAddress=*/false);
}

void InitAnalysis::checkEnsureInitPointeeRead(
    const Place &P, const InitLattice &State,
    const llvm::DenseMap<LocalId, LocalId> &TempToEnsureInitParam,
    SourceLocation Loc, SmallVectorImpl<InitDiagInfo> &Diags) const {
  // Only a projection that dereferences the base reads the pointee.
  if (P.Projections.empty() ||
      !projectionLoadsPointer(P, 0, B.getLocal(P.Base).Ty))
    return;
  // A terminator carries no location of its own; point at the read.
  if (P.Loc.isValid())
    Loc = P.Loc;
  // Resolve temp alias to the original ensure_init param.
  auto AliasIt = TempToEnsureInitParam.find(P.Base);
  LocalId ParamId =
      (AliasIt != TempToEnsureInitParam.end()) ? AliasIt->second : P.Base;
  auto DerefIt = State.EnsureInitDerefStates.find(ParamId);
  if (DerefIt == State.EnsureInitDerefStates.end() ||
      DerefIt->second == InitState::Initialized)
    return;

  // A read of one field only needs that field, not the whole pointee.
  if (P.Projections.size() > 1 &&
      P.Projections[0].K == ProjectionElem::Deref) {
    Place SubPlace(ParamId, P.Projections.slice(1), P.Ty, P.Loc);
    if (auto FP = getFieldPathPrefix(SubPlace)) {
      if (isVacuouslyInitialized(getFieldType(FP->Base, FP->Indices)))
        return;
      if (getFieldInitState(State, *FP) == InitState::Initialized)
        return;
      // A whole-struct write covers every path below it.
      FieldPath Ancestor;
      Ancestor.Base = FP->Base;
      for (unsigned I = 0; I + 1 < FP->Indices.size(); ++I) {
        Ancestor.Indices.push_back(FP->Indices[I]);
        if (getFieldInitState(State, Ancestor) == InitState::Initialized)
          return;
      }
    }
  }
  const LocalDecl &ParamLD = B.getLocal(ParamId);
  if (ParamLD.Name.empty())
    return;
  Diags.emplace_back(InitDiagKind::EnsureInitDerefReadUninit,
                     Loc.isValid() ? Loc : ParamLD.DeclLoc, ParamLD.Name);
  Diags.back().AttrSelect = getIfRetCondValue(ParamId) ? 1 : 0;
}

void InitAnalysis::checkEnsureInitDerefReads(
    const Statement &S, const InitLattice &State,
    const llvm::DenseSet<LocalId> &EnsureInitArgTemps,
    const llvm::DenseMap<LocalId, LocalId> &TempToEnsureInitParam,
    SmallVectorImpl<InitDiagInfo> &Diags) const {
  // Addressing the pointee for a contract callee delegates it, not reads it.
  if (S.getAssign().Dest.isLocal() &&
      EnsureInitArgTemps.count(S.getAssign().Dest.Base))
    return;

  forEachRvalueOperand(S.getAssign().Src, [&](const Operand &Op) {
    if (Op.K == Operand::Constant)
      return;
    checkEnsureInitPointeeRead(Op.getPlace(), State, TempToEnsureInitParam,
                               S.Loc, Diags);
  });
}


llvm::DenseSet<unsigned>
InitAnalysis::collectExemptArgIndices(
    const Terminator::CallData &CD) const {
  llvm::DenseSet<unsigned> ExemptArgIndices;
  if (CD.Decl &&
      CD.Decl->getBuiltinID() == Builtin::BI__assume_initialized) {
    ExemptArgIndices.insert(0);
  }
  unsigned NumParams = CD.Decl
                           ? CD.Decl->getNumParams()
                           : (CD.CalleeProtoType
                                  ? CD.CalleeProtoType->getNumParams()
                                  : 0);
  for (unsigned I = 0; I < NumParams; ++I) {
    int Cond = 0;
    if (classifyEnsureInit(CD.Decl, CD.CalleeProtoType, I, Cond) ==
        EnsureInitKind::None)
      continue;
    if (I < CD.ArgPlaces.size() && CD.ArgPlaces[I])
      ExemptArgIndices.insert(I);
  }
  return ExemptArgIndices;
}

void InitAnalysis::checkEnsureInitAtReturn(
    const Terminator &T, const InitLattice &State,
    SmallVectorImpl<InitDiagInfo> &Diags) const {
  for (const auto &Entry : State.EnsureInitDerefStates) {
    // Skip ensure_init_if params — checked separately per return path.
    if (getIfRetCondValue(Entry.first))
      continue;
    const LocalDecl &LD = B.getLocal(Entry.first);
    SourceLocation DiagLoc = T.Loc.isValid()
        ? T.Loc
        : (B.SourceFD ? B.SourceFD->getBodyRBrace() : SourceLocation());
    // When a re-point on this path is the cause, attribute the failure to it
    // (clearer than "*param not initialized at return", since *param now
    // denotes the new pointee). Keep the accurate not-init message otherwise.
    bool Reassigned = State.ReassignedParams.count(Entry.first);
    InitDiagInfo *Emitted = nullptr;
    if (Entry.second == InitState::Uninitialized ||
        Entry.second == InitState::MaybeInit) {
      InitDiagKind K =
          Reassigned ? InitDiagKind::EnsureInitReassigned
          : Entry.second == InitState::Uninitialized
              ? InitDiagKind::EnsureInitNotInit
              : InitDiagKind::EnsureInitMaybeNotInit;
      Diags.emplace_back(K, DiagLoc, LD.Name);
      Emitted = &Diags.back();
    }
    if (Emitted) {
      auto RpIt = State.ReassignedParams.find(Entry.first);
      if (RpIt != State.ReassignedParams.end())
        Emitted->NoteLocs.assign(RpIt->second.begin(), RpIt->second.end());
    }
  }
}

InitAnalysis::ReturnValueInfo
InitAnalysis::analyzeReturnValue(BasicBlockId PredId) const {
  ReturnValueInfo RV;

  llvm::SmallDenseSet<unsigned, 8> Visited;
  BasicBlockId Cur = PredId;
  while (Cur.Index < B.Blocks.size() && Visited.insert(Cur.Index).second) {
    const BasicBlock &Blk = B.getBlock(Cur);
    bool FoundAssign = false;
    for (auto It = Blk.Statements.rbegin(); It != Blk.Statements.rend(); ++It) {
      if (It->K != Statement::Assign || !It->getAssign().Dest.isLocal() ||
          It->getAssign().Dest.Base != LocalId{0})
        continue;
      FoundAssign = true;
      RV.Loc = It->Loc;
      const Rvalue &Src = It->getAssign().Src;
      int64_t V = 0;
      if (Src.K == Rvalue::Use && foldConstOperand(Src.getUse().Op, V)) {
        RV.IsConstant = true;
        RV.ConstVal = V;
      } else if (Src.K == Rvalue::Use) {
        if (llvm::Optional<LocalId> TmpId = asCopiedLocal(Src.getUse().Op)) {
          // `_0 = copy(_t)`: record the source local (for delegation) and
          // trace one level for a folded constant.
          RV.SourceLocal = *TmpId;
          RV.HasSourceLocal = true;
          for (auto It2 = It; It2 != Blk.Statements.rend(); ++It2) {
            if (It2->K != Statement::Assign ||
                !It2->getAssign().Dest.isLocal() ||
                It2->getAssign().Dest.Base != *TmpId)
              continue;
            const Rvalue &TmpSrc = It2->getAssign().Src;
            int64_t Folded = 0;
            if ((TmpSrc.K == Rvalue::Use &&
                 foldConstOperand(TmpSrc.getUse().Op, V)) ||
                (TmpSrc.K == Rvalue::Cast &&
                 foldConstOperand(TmpSrc.getCast().Op, V))) {
              RV.IsConstant = true;
              RV.ConstVal = V;
            } else if (TmpSrc.K == Rvalue::UnaryOp &&
                       foldConstOperand(TmpSrc.getUnOp().Sub, V) &&
                       foldUnary(TmpSrc.getUnOp().Op, V, Folded)) {
              RV.IsConstant = true;
              RV.ConstVal = Folded;
            }
            break;
          }
        }
      }
      break; // handled the last write to _0 in this block
    }
    if (FoundAssign)
      return RV;
    // _0 not assigned here: walk back through the cleanup-block chain.
    // Stop at a join (>1 pred) — the value is then unknown (conservative).
    auto Preds = B.getPredecessors(Cur);
    if (Preds.size() != 1)
      return RV;
    Cur = Preds[0];
  }
  return RV;
}

void InitAnalysis::checkEnsureInitIfRetAtReturn(
    const DataflowResult<InitLattice> &Result,
    SmallVectorImpl<InitDiagInfo> &Diags) const {
  if (!B.SourceFD)
    return;
  // ensure_init_if_ret params are a per-decl constant; collect them once.
  SmallVector<std::pair<LocalId, int>, 2> IfRetParams;
  for (unsigned PI = 0; PI < B.SourceFD->getNumParams(); ++PI)
    if (auto *A = B.SourceFD->getParamDecl(PI)->getAttr<EnsureInitIfRetAttr>())
      IfRetParams.push_back({LocalId{PI + 1}, A->getCondValue()});
  if (IfRetParams.empty())
    return;

  // Check each predecessor of each return block. Do NOT filter on the
  // terminator kind: a Drop (resource cleanup) can precede the return,
  // and filtering on Goto skipped the contract for resource-owning fns.
  for (const BasicBlock &BB : B.Blocks) {
    if (BB.Term.K != Terminator::Return)
      continue;

    for (BasicBlockId PredId : B.getPredecessors(BB.Id)) {
      auto ExitIt = Result.ExitStates.find(PredId);
      if (ExitIt == Result.ExitStates.end())
        continue;
      // ExitStates[PredId] is pre-terminator; cleanup terminators don't
      // touch deref states, so it is the deref state at the return.
      const InitLattice &PredState = ExitIt->second;

      ReturnValueInfo RV = analyzeReturnValue(PredId);
      SourceLocation DiagLoc = RV.Loc.isValid()
          ? RV.Loc
          : B.SourceFD->getBodyRBrace();

      for (const auto &IRP : IfRetParams) {
        LocalId ParamId = IRP.first;
        int CondValue = IRP.second;

        auto DerefIt = PredState.EnsureInitDerefStates.find(ParamId);
        InitState DS = (DerefIt != PredState.EnsureInitDerefStates.end())
                           ? DerefIt->second
                           : InitState::Uninitialized;

        // Delegation credit: returning an inner ensure_init_if_ret call's
        // result with the same cond inits *param on this path. Apply only
        // when the returned value IS that inner result (the recorded PCI).
        if (DS != InitState::Initialized && RV.HasSourceLocal) {
          for (const auto &PCI : PredState.PendingCondInits) {
            if (PCI.OutParamLocal == ParamId && PCI.OutFieldIndices.empty() &&
                PCI.Pointee && PCI.CondValue == CondValue &&
                PCI.RetLocal == RV.SourceLocal) {
              DS = InitState::Initialized;
              break;
            }
          }
        }

        // When a re-point on this path is the cause, attribute the failure to
        // it instead of the misleading "*p not initialized at return" (after a
        // re-point *p denotes the new pointee). Accurate message otherwise.
        bool Reassigned = PredState.ReassignedParams.count(ParamId);
        InitDiagInfo *Emitted = nullptr;
        if (RV.IsConstant) {
          if (RV.ConstVal != CondValue)
            continue;
          if (DS == InitState::Uninitialized || DS == InitState::MaybeInit) {
            InitDiagKind K =
                Reassigned ? InitDiagKind::EnsureInitIfRetReassigned
                : DS == InitState::Uninitialized
                    ? InitDiagKind::EnsureInitIfRetNotInit
                    : InitDiagKind::EnsureInitIfRetMaybeNotInit;
            Diags.emplace_back(K, DiagLoc, B.getLocal(ParamId).Name, CondValue);
            Emitted = &Diags.back();
          }
        } else {
          // Non-constant return: the runtime value may equal arg, so *p
          // must already be init on this path. Over-fulfilment (always
          // init) silently satisfies the contract.
          if (DS != InitState::Initialized) {
            Diags.emplace_back(InitDiagKind::EnsureInitIfRetNonConstReturn,
                               DiagLoc, B.getLocal(ParamId).Name, CondValue);
            Emitted = &Diags.back();
          }
        }
        if (Emitted) {
          auto RpIt = PredState.ReassignedParams.find(ParamId);
          if (RpIt != PredState.ReassignedParams.end())
            Emitted->NoteLocs.assign(RpIt->second.begin(),
                                     RpIt->second.end());
        }
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// Run Analysis with Diagnostic Collection
//===----------------------------------------------------------------------===//

void InitAnalysis::run(SmallVectorImpl<InitDiagInfo> &Diags) const {
  // Run forward dataflow analysis
  DataflowResult<InitLattice> Result = runForwardAnalysis(B, *this);

  // Walk through all blocks and check uses against computed states
  for (const BasicBlock &BB : B.Blocks) {
    auto EntryIt = Result.EntryStates.find(BB.Id);
    if (EntryIt == Result.EntryStates.end())
      continue;

    InitLattice State = EntryIt->second;

    // Collect ensure_init/assume_initialized exempt arg temps for this block.
    llvm::DenseSet<LocalId> EnsureInitArgTemps =
        collectEnsureInitArgTemps(BB);

    // Track block-local temp aliases of ensure_init params for deref checking.
    llvm::DenseMap<LocalId, LocalId> TempToEnsureInitParam;
    llvm::DenseMap<LocalId, LocalId> TempToEnsureInitParamAddr;

    for (const Statement &S : BB.Statements) {
      // Check operands used in this statement
      if (S.K == Statement::Assign) {
        // Only check uses in _Safe zones
        if (CheckAllZones || S.SafeZone == SZ_Safe) {
          // Check the source operands
          const Rvalue &Src = S.getAssign().Src;
          switch (Src.K) {
          case Rvalue::Use:
            checkOperand(Src.getUse().Op, State, S.Loc, Diags);
            break;
          case Rvalue::BinaryOp:
            checkOperand(Src.getBinOp().LHS, State, S.Loc, Diags);
            checkOperand(Src.getBinOp().RHS, State, S.Loc, Diags);
            break;
          case Rvalue::UnaryOp:
            checkOperand(Src.getUnOp().Sub, State, S.Loc, Diags);
            break;
          case Rvalue::Cast:
            checkOperand(Src.getCast().Op, State, S.Loc, Diags);
            break;
          case Rvalue::Aggregate:
            for (const Operand &Field : Src.getAgg().Fields)
              checkOperand(Field, State, S.Loc, Diags);
            break;
          case Rvalue::Array:
            for (const Operand &El : Src.getArray().Elements)
              checkOperand(El, State, S.Loc, Diags);
            break;
          case Rvalue::Ref: {
            const Place &P = Src.getRef().P;
            // Exempt if dest temp feeds an ensure_init/__assume_initialized arg.
            if (!S.getAssign().Dest.isLocal() ||
                !EnsureInitArgTemps.count(S.getAssign().Dest.Base))
              checkOperand(Operand::createCopy(P), State, S.Loc, Diags);
            break;
          }
          case Rvalue::AddressOf: {
            const Place &P = Src.getAddrOf().P;
            if (!S.getAssign().Dest.isLocal() ||
                !EnsureInitArgTemps.count(S.getAssign().Dest.Base))
              checkOperand(Operand::createCopy(P), State, S.Loc, Diags);
            break;
          }
          case Rvalue::NullPtr:
          case Rvalue::SizeOf:
            break;
          }

          // Check destination for implicit reads through Deref/Index
          // projections. Writing to (*_1.p) or _1.p[i] reads _1.p to compute the
          // address; the prefix before such a projection must be initialized.
          // Indexing a real array (not a pointer) reads no pointer, so it is
          // exempt.
        }

        // Callee-side ensure_init checks (zone-independent).
        checkEnsureInitAssign(S, State, TempToEnsureInitParam,
                              TempToEnsureInitParamAddr, Diags);
        checkEnsureInitDerefReads(S, State, EnsureInitArgTemps,
                                  TempToEnsureInitParam, Diags);

        // Each prefix a later projection dereferences is itself read.
        const Place &Dest = S.getAssign().Dest;
        QualType DestBaseTy = B.getLocal(Dest.Base).Ty;
        for (unsigned I = 0; I < Dest.Projections.size(); ++I) {
          if (!projectionLoadsPointer(Dest, I, DestBaseTy))
            continue;
          QualType PrefixTy =
              I == 0 ? DestBaseTy : Dest.Projections[I - 1].ResultTy;
          Place Prefix(Dest.Base, Dest.Projections.slice(0, I), PrefixTy,
                       Dest.Loc);
          if (CheckAllZones || S.SafeZone == SZ_Safe)
            checkOperand(Operand::createCopy(Prefix), State, S.Loc, Diags);
          // The prefix before projection 0 is the parameter itself.
          if (I > 0)
            checkEnsureInitPointeeRead(Prefix, State, TempToEnsureInitParam,
                                       S.Loc, Diags);
        }
      }

      // Apply transfer function to update state
      transferStatement(S, State);
    }

    // Check terminator operands
    const Terminator &T = BB.Term;
    if (T.K == Terminator::Call && (CheckAllZones || T.SafeZone == SZ_Safe)) {
      const auto &CD = T.getCall();
      checkOperand(CD.Callee, State, T.Loc, Diags);
      llvm::DenseSet<unsigned> ExemptArgIndices = collectExemptArgIndices(CD);
      for (unsigned I = 0; I < CD.Args.size(); ++I) {
        if (!ExemptArgIndices.count(I))
          checkOperand(CD.Args[I], State, T.Loc, Diags);
      }
    }

    if (T.K == Terminator::SwitchInt && (CheckAllZones || T.SafeZone == SZ_Safe))
      checkOperand(T.getSwitchInt().Discriminant, State, T.Loc, Diags);

    // A terminator operand reads the pointee just as an assignment does.
    if (T.K == Terminator::Call) {
      const auto &CD = T.getCall();
      llvm::DenseSet<unsigned> ExemptArgIndices = collectExemptArgIndices(CD);
      for (unsigned I = 0; I < CD.Args.size(); ++I) {
        if (ExemptArgIndices.count(I) || CD.Args[I].K == Operand::Constant)
          continue;
        checkEnsureInitPointeeRead(CD.Args[I].getPlace(), State,
                                   TempToEnsureInitParam, T.Loc, Diags);
      }
    } else if (T.K == Terminator::SwitchInt) {
      const Operand &Disc = T.getSwitchInt().Discriminant;
      if (Disc.K != Operand::Constant)
        checkEnsureInitPointeeRead(Disc.getPlace(), State,
                                   TempToEnsureInitParam, T.Loc, Diags);
    }

    // Check return slot at Return terminator
    if (T.K == Terminator::Return && (CheckAllZones || T.SafeZone == SZ_Safe)) {
      if (!B.Locals[0].Ty->isVoidType()) {
        InitState RetState = getInitState(State, LocalId{0});
        // The implicit fall-off return has no source location; anchor the
        // diagnostic at the function's closing brace instead.
        SourceLocation RetLoc = T.Loc;
        if (RetLoc.isInvalid() && B.SourceFD)
          RetLoc = B.SourceFD->getEndLoc();
        if (RetState == InitState::Uninitialized) {
          Diags.emplace_back(InitDiagKind::ReturnUninit, RetLoc,
                             B.SourceFD ? B.SourceFD->getNameAsString()
                                        : "<return>");
        } else if (RetState == InitState::MaybeInit) {
          Diags.emplace_back(InitDiagKind::ReturnMaybeUninit, RetLoc,
                             B.SourceFD ? B.SourceFD->getNameAsString()
                                        : "<return>");
        }
      }
      checkEnsureInitAtReturn(T, State, Diags);
    }

    // Also check ensure_init contract at Return outside _Safe zones
    if (T.K == Terminator::Return && T.SafeZone != SZ_Safe)
      checkEnsureInitAtReturn(T, State, Diags);
  }

  // Check ensure_init_if contract per return-path predecessor.
  checkEnsureInitIfRetAtReturn(Result, Diags);
}

//===----------------------------------------------------------------------===//
// Entry Point
//===----------------------------------------------------------------------===//

void bscir::runInitAnalysis(const Body &B,
                            SmallVectorImpl<InitDiagInfo> &Diags,
                            bool CheckAllZones) {
  InitAnalysis Analysis(B, CheckAllZones);
  Analysis.run(Diags);
}

#endif // ENABLE_BSC
