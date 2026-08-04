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
      unsigned UnionDepth = 0;
      if (isUnionStructFieldPath(*FP, UnionDepth)) {
        // Writing a struct field within a union variant (e.g., u.s.x).
        // Track field-level init AND mark the union path as Initialized
        // so cross-variant reads (e.g., u.f) pass.
        markFieldInit(State, *FP, Changed);
        if (UnionDepth == 0) {
          // Top-level union local: mark whole local Initialized so that
          // cross-variant reads pass the whole-local check.
          auto &LS = State.LocalStates[FP->Base];
          if (LS != InitState::Initialized) {
            LS = InitState::Initialized;
            Changed = true;
          }
        }
        // For nested unions (UnionDepth > 0), do NOT mark the union field
        // path as Init here — that would bypass field-level tracking.
        // Cross-variant reads for nested unions are handled in checkOperand.
      } else if (isUnionVariantPath(*FP, UnionDepth)) {
        // Writing a whole union variant (e.g., u.f or u.s = {...}).
        // Clear any struct-field tracking and mark union Initialized.
        llvm::SmallVector<unsigned, 4> Prefix(FP->Indices.begin(),
                                              FP->Indices.begin() + UnionDepth);
        clearUnionFieldEntries(State, FP->Base, Prefix, Changed);
        if (UnionDepth == 0) {
          // Top-level union: mark whole local Initialized.
          auto &LS = State.LocalStates[FP->Base];
          if (LS != InitState::Initialized) {
            LS = InitState::Initialized;
            Changed = true;
          }
        } else {
          // Union nested in a struct: mark the union field path as Init.
          FieldPath UnionFP;
          UnionFP.Base = FP->Base;
          UnionFP.Indices.assign(FP->Indices.begin(),
                                 FP->Indices.begin() + UnionDepth);
          markFieldInit(State, UnionFP, Changed);
        }
      } else {
        // Normal struct field init (no union involved).
        markFieldInit(State, *FP, Changed);
      }
    }
    // Note: array element writes (arr[i] = ...) do NOT mark the array as
    // initialized. Arrays must be initialized via initializer list or
    // __assume_initialized.

    // Callee-side ensure_init: a write through *param. A re-pointed param does
    // not promote (the write hits the new pointee, not the tracked one).
    if (!S.getAssign().Dest.Projections.empty() &&
        S.getAssign().Dest.Projections[0].K == ProjectionElem::Deref) {
      LocalId Base = S.getAssign().Dest.Base;
      const auto &Projs = S.getAssign().Dest.Projections;
      if (!State.ReassignedParams.count(Base)) {
        if (Projs.size() == 1) {
          markPointeeFullyInit(State, Base, Changed);
        } else if (!getEnsureInitPointeeType(Base).isNull()) {
          // Struct pointee field write: reuse getFieldPath on the post-Deref place.
          Place SubPlace(Base, Projs.slice(1),
                         S.getAssign().Dest.Ty, S.getAssign().Dest.Loc);
          if (auto FP = getFieldPath(SubPlace)) {
            if (!getFieldType(FP->Base, FP->Indices)->isArrayType())
              markFieldInit(State, *FP, Changed);
          }
        }
        // else: non-struct element write (e.g. array index) — skip.
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
        const Place &ArgPlace = *CD.ArgPlaces[0];
        bool Changed = false;
        // A re-pointed param's `&*p`/`&p->f` denotes the new pointee, not the
        // tracked one — don't promote (mirrors the deref-write gating above).
        bool Repointed = Result.ReassignedParams.count(ArgPlace.Base);
        if (ArgPlace.Projections.empty()) {
          // &x: whole local, plus pointee if x is an ensure_init param.
          Result.LocalStates[ArgPlace.Base] = InitState::Initialized;
          SmallVector<unsigned, 4> Prefix;
          markAllFieldsInit(Result, ArgPlace.Base,
                            B.getLocal(ArgPlace.Base).Ty, Prefix);
          if (!Repointed)
            markPointeeFullyInit(Result, ArgPlace.Base, Changed);
        } else if (ArgPlace.Projections[0].K == ProjectionElem::Deref) {
          // &*p or &p->field... on an ensure_init pointer param.
          if (Repointed) {
            // skip: the contract-tracked pointee is no longer addressed here.
          } else if (ArgPlace.Projections.size() == 1) {
            markPointeeFullyInit(Result, ArgPlace.Base, Changed);
          } else if (!getEnsureInitPointeeType(ArgPlace.Base).isNull()) {
            Place SubPlace(ArgPlace.Base, ArgPlace.Projections.slice(1),
                           ArgPlace.Ty, ArgPlace.Loc);
            markFieldInit(Result, SubPlace, Changed);
          }
        } else {
          markFieldInit(Result, ArgPlace, Changed);
        }
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
            if (I < CD.ArgPlaces.size() && CD.ArgPlaces[I]) {
              // Caller-side `&x` / `&x.field`: ArgPlaces holds the place.
              const Place &ArgPlace = *CD.ArgPlaces[I];
              if (ArgPlace.Projections.empty()) {
                OutLocal = ArgPlace.Base;
                Recognised = true;
              } else if (auto FP = getFieldPath(ArgPlace)) {
                OutLocal = FP->Base;
                OutFields.assign(FP->Indices.begin(), FP->Indices.end());
                Recognised = true;
              }
            } else if (I < CD.Args.size()) {
              // Delegation: this function's own param passed directly (no
              // &-origin). Skip if re-pointed — the call passes a different
              // pointer than the contract-tracked one.
              if (auto ArgBase = asCopiedLocal(CD.Args[I]))
                if (getIfRetCondValue(*ArgBase) &&
                    !Result.ReassignedParams.count(*ArgBase)) {
                  OutLocal = *ArgBase;
                  Recognised = true;
                }
            }
            if (Recognised) {
              InitLattice::PendingCondInit PCI;
              PCI.OutParamLocal = OutLocal;
              PCI.OutFieldIndices = std::move(OutFields);
              PCI.RetLocal = CD.Dest.Base;
              PCI.CondValue = EIIRCondValue;
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

        // Callee-side delegation: if the argument is a direct pass of an
        // ensure_init param (Copy(_N) where _N is an ensure_init param),
        // mark the deref state as Initialized. Skip if the param has
        // been re-pointed — the call passes the new pointer, not the
        // original tracked one.
        if (I < CD.Args.size()) {
          if (auto ArgBase = asCopiedLocal(CD.Args[I])) {
            if (!Result.ReassignedParams.count(*ArgBase)) {
              auto DerefIt = Result.EnsureInitDerefStates.find(*ArgBase);
              if (DerefIt != Result.EnsureInitDerefStates.end())
                DerefIt->second = InitState::Initialized;
            }
          }
        }

        // Caller side: mark the addressed place as initialized
        if (I >= CD.ArgPlaces.size() || !CD.ArgPlaces[I])
          continue;

        const Place &ArgPlace = *CD.ArgPlaces[I];
        if (ArgPlace.Projections.empty()) {
          Result.LocalStates[ArgPlace.Base] = InitState::Initialized;
          SmallVector<unsigned, 4> Prefix;
          markAllFieldsInit(Result, ArgPlace.Base,
                            B.getLocal(ArgPlace.Base).Ty, Prefix);
        } else {
          bool Changed = false;
          markFieldInit(Result, ArgPlace, Changed);
        }
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
          if (PCI.OutFieldIndices.empty() &&
              getIfRetCondValue(PCI.OutParamLocal)) {
            // Delegation: matching branch => inner returned cond => *param
            // init. The pointee lives in EnsureInitDerefStates, not
            // LocalStates/FieldStates (which describe the pointer itself).
            Result.EnsureInitDerefStates[PCI.OutParamLocal] =
                InitState::Initialized;
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
  if (PointeeTy.isNull() || !PointeeTy->isRecordType() ||
      getNumFields(PointeeTy) == 0)
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
    unsigned I = 0;
    for (auto It = RD->field_begin(); It != RD->field_end(); ++It, ++I) {
      if (I == Idx) {
        Ty = It->getType();
        break;
      }
    }
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
  if (NumSiblings == 0) {
    // Check if parent is a union: promoting any single variant means the
    // whole union is fully covered (one complete variant covers all bytes).
    const RecordDecl *PRD = ParentTy->getAsRecordDecl();
    if (PRD && PRD->isUnion()) {
      if (Parent.Indices.empty()) {
        auto &LS = State.LocalStates[FP.Base];
        if (LS != InitState::Initialized) {
          LS = InitState::Initialized;
          Changed = true;
        }
      } else {
        auto &PS = State.FieldStates[Parent];
        if (PS != InitState::Initialized) {
          PS = InitState::Initialized;
          Changed = true;
        }
        tryPromoteParent(State, Parent, Changed);
      }
      // Clear field-level entries since the whole union is now promoted.
      clearUnionFieldEntries(State, FP.Base, Parent.Indices, Changed);
    }
    return;
  }

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
    if (FieldRD) {
      // Recurse into nested struct or union variants.
      markAllFieldsInit(State, Base, FieldTy, Prefix);
    }

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
    unsigned I = 0;
    for (auto It = RD->field_begin(); It != RD->field_end(); ++It, ++I) {
      if (I == Idx) {
        if (!It->isAnonymousStructOrUnion()) {
          Name += "." + It->getNameAsString();
        }
        CurTy = It->getType();
        break;
      }
    }
  }
  return Name;
}

bool InitAnalysis::isUnionStructFieldPath(const FieldPath &FP,
                                          unsigned &UnionDepth) const {
  QualType CurTy = B.getLocal(FP.Base).Ty;
  for (unsigned I = 0; I < FP.Indices.size(); ++I) {
    const RecordDecl *RD = CurTy->getAsRecordDecl();
    if (!RD)
      return false;
    if (RD->isUnion()) {
      // We're at a union level. The current index selects a variant.
      // Check if that variant is a struct and the path continues deeper.
      unsigned Idx = FP.Indices[I];
      unsigned F = 0;
      for (auto It = RD->field_begin(); It != RD->field_end(); ++It, ++F) {
        if (F == Idx) {
          QualType VariantTy = It->getType();
          const RecordDecl *VariantRD = VariantTy->getAsRecordDecl();
          if (VariantRD && !VariantRD->isUnion() && I + 1 < FP.Indices.size()) {
            // Found: union → struct variant → continues into struct fields.
            UnionDepth = I;
            return true;
          }
          // Variant is scalar, another union, or path ends here. Continue
          // walking to check deeper levels.
          CurTy = VariantTy;
          break;
        }
      }
      continue;
    }
    // Walk into the struct field.
    unsigned Idx = FP.Indices[I];
    unsigned F = 0;
    for (auto It = RD->field_begin(); It != RD->field_end(); ++It, ++F) {
      if (F == Idx) {
        CurTy = It->getType();
        break;
      }
    }
  }
  return false;
}

bool InitAnalysis::isUnionVariantPath(const FieldPath &FP,
                                      unsigned &UnionDepth) const {
  QualType CurTy = B.getLocal(FP.Base).Ty;
  for (unsigned I = 0; I < FP.Indices.size(); ++I) {
    const RecordDecl *RD = CurTy->getAsRecordDecl();
    if (!RD)
      return false;
    if (RD->isUnion()) {
      // The path enters this union. If it ends here (last index), it's a
      // union variant path (e.g., u.f or u.s as a whole).
      if (I + 1 == FP.Indices.size()) {
        UnionDepth = I;
        return true;
      }
      // It continues deeper — walk into the variant.
      unsigned Idx = FP.Indices[I];
      unsigned F = 0;
      for (auto It = RD->field_begin(); It != RD->field_end(); ++It, ++F) {
        if (F == Idx) {
          CurTy = It->getType();
          break;
        }
      }
      continue;
    }
    unsigned Idx = FP.Indices[I];
    unsigned F = 0;
    for (auto It = RD->field_begin(); It != RD->field_end(); ++It, ++F) {
      if (F == Idx) {
        CurTy = It->getType();
        break;
      }
    }
  }
  return false;
}

bool InitAnalysis::hasUnionFieldEntries(const InitLattice &State, LocalId Base,
                                        ArrayRef<unsigned> UnionPrefix) const {
  // Build a low key with the prefix and scan for entries that extend beyond it.
  FieldPath LowKey;
  LowKey.Base = Base;
  LowKey.Indices.assign(UnionPrefix.begin(), UnionPrefix.end());

  auto It = State.FieldStates.lower_bound(LowKey);
  while (It != State.FieldStates.end() && It->first.Base == Base) {
    const auto &Indices = It->first.Indices;
    if (Indices.size() > UnionPrefix.size()) {
      // Check prefix match.
      bool Match = true;
      for (unsigned I = 0; I < UnionPrefix.size(); ++I) {
        if (Indices[I] != UnionPrefix[I]) {
          Match = false;
          break;
        }
      }
      if (Match)
        return true;
    }
    ++It;
  }
  return false;
}

void InitAnalysis::clearUnionFieldEntries(InitLattice &State, LocalId Base,
                                          ArrayRef<unsigned> UnionPrefix,
                                          bool &Changed) const {
  FieldPath LowKey;
  LowKey.Base = Base;
  LowKey.Indices.assign(UnionPrefix.begin(), UnionPrefix.end());

  auto It = State.FieldStates.lower_bound(LowKey);
  while (It != State.FieldStates.end() && It->first.Base == Base) {
    const auto &Indices = It->first.Indices;
    // Check prefix match (strictly extending, not exact match).
    bool Match = true;
    if (Indices.size() > UnionPrefix.size()) {
      for (unsigned I = 0; I < UnionPrefix.size(); ++I) {
        if (Indices[I] != UnionPrefix[I]) {
          Match = false;
          break;
        }
      }
    } else {
      Match = false;
    }
    if (Match) {
      It = State.FieldStates.erase(It);
      Changed = true;
    } else if (Indices > LowKey.Indices) {
      break; // Past the prefix range due to ordering.
    } else {
      ++It;
    }
  }
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

  // If the whole local is Initialized, most field accesses are fine.
  // Exception: reading a struct field within a union variant when there is
  // active field-level tracking (meaning a partial struct write happened).
  if (IS == InitState::Initialized) {
    if (auto FP = getFieldPathPrefix(Op.getPlace())) {
      unsigned UnionDepth = 0;
      if (isUnionStructFieldPath(*FP, UnionDepth)) {
        llvm::SmallVector<unsigned, 4> Prefix(FP->Indices.begin(),
                                              FP->Indices.begin() + UnionDepth);
        if (hasUnionFieldEntries(State, Id, Prefix)) {
          InitState FS = getFieldInitState(State, *FP);
          if (FS == InitState::Initialized)
            return;
          const LocalDecl &LD = B.getLocal(Id);
          if (LD.IsTemp || LD.Name.empty())
            return;
          std::string FieldName = buildFieldName(*FP);
          if (FS == InitState::Uninitialized)
            Diags.emplace_back(InitDiagKind::UseOfUninit,
                               Loc.isValid() ? Loc : LD.DeclLoc, FieldName);
          else
            Diags.emplace_back(InitDiagKind::UseOfMaybeUninit,
                               Loc.isValid() ? Loc : LD.DeclLoc, FieldName);
          return;
        }
      }
    }
    return;
  }

  // Check field-level state if the operand has a field projection.
  if (auto FP = getFieldPathPrefix(Op.getPlace())) {
    if (isVacuouslyInitialized(getFieldType(FP->Base, FP->Indices)))
      return;

    InitState FS = getFieldInitState(State, *FP);
    if (FS == InitState::Initialized)
      return;

    // Cross-variant read of a nested union: if this path ends at a union
    // variant and there are any field entries under that union prefix,
    // some variant was written — cross-variant read is OK.
    {
      unsigned VUnionDepth = 0;
      if (isUnionVariantPath(*FP, VUnionDepth)) {
        llvm::SmallVector<unsigned, 4> Prefix(FP->Indices.begin(),
                                              FP->Indices.begin() + VUnionDepth);
        if (hasUnionFieldEntries(State, Id, Prefix))
          return;
      }
    }

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
    SmallVectorImpl<InitDiagInfo> &Diags) const {
  if (!S.getAssign().Dest.isLocal())
    return;

  LocalId DestId = S.getAssign().Dest.Base;

  // Re-pointing is deferred to the at-return check; ReassignedParams
  // gating keeps it sound, and the failing return gets the error + note.

  // Reject aliasing into a named variable before *param is init (the alias
  // can't be tracked across blocks); temp copies are tracked, not rejected.
  const Rvalue &Src = S.getAssign().Src;
  if (Src.K != Rvalue::Use)
    return;
  const Operand &Op = Src.getUse().Op;
  if (Op.K != Operand::Copy || !Op.getPlace().Projections.empty())
    return;
  LocalId SrcId = Op.getPlace().Base;
  // Check direct ensure_init param or transitive temp alias.
  LocalId ParamId = SrcId;
  auto AliasIt = TempToEnsureInitParam.find(SrcId);
  if (AliasIt != TempToEnsureInitParam.end())
    ParamId = AliasIt->second;
  auto DerefIt = State.EnsureInitDerefStates.find(ParamId);
  if (DerefIt == State.EnsureInitDerefStates.end() ||
      DerefIt->second == InitState::Initialized)
    return;
  const LocalDecl &DestLD = B.getLocal(DestId);
  if (DestLD.IsTemp) {
    // Track temp alias for same-block deref checking.
    TempToEnsureInitParam[DestId] = ParamId;
  } else if (!DestLD.Name.empty()) {
    // Named variable: reject aliasing before init.
    const LocalDecl &ParamLD = B.getLocal(ParamId);
    if (!ParamLD.Name.empty()) {
      Diags.emplace_back(InitDiagKind::EnsureInitPtrAliased,
                         S.Loc.isValid() ? S.Loc : DestLD.DeclLoc,
                         ParamLD.Name);
      Diags.back().AttrSelect = getIfRetCondValue(ParamId) ? 1 : 0;
    }
  }
}

void InitAnalysis::checkEnsureInitDerefReads(
    const Statement &S, const InitLattice &State,
    const llvm::DenseMap<LocalId, LocalId> &TempToEnsureInitParam,
    SmallVectorImpl<InitDiagInfo> &Diags) const {
  auto checkDerefRead = [&](const Operand &Op) {
    if (Op.K == Operand::Constant)
      return;
    const Place &P = Op.getPlace();
    if (P.Projections.empty() ||
        P.Projections[0].K != ProjectionElem::Deref)
      return;
    // Resolve temp alias to the original ensure_init param.
    LocalId Base = P.Base;
    auto AliasIt = TempToEnsureInitParam.find(Base);
    LocalId ParamId = (AliasIt != TempToEnsureInitParam.end())
                          ? AliasIt->second
                          : Base;
    auto DerefIt = State.EnsureInitDerefStates.find(ParamId);
    if (DerefIt == State.EnsureInitDerefStates.end())
      return;
    if (DerefIt->second != InitState::Initialized) {
      const LocalDecl &ParamLD = B.getLocal(ParamId);
      if (!ParamLD.Name.empty()) {
        Diags.emplace_back(InitDiagKind::EnsureInitDerefReadUninit,
                           S.Loc.isValid() ? S.Loc : ParamLD.DeclLoc,
                           ParamLD.Name);
        Diags.back().AttrSelect = getIfRetCondValue(ParamId) ? 1 : 0;
      }
    }
  };

  forEachRvalueOperand(S.getAssign().Src, checkDerefRead);
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
    if (classifyEnsureInit(CD.Decl, CD.CalleeProtoType, I, Cond) !=
        EnsureInitKind::None)
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
                PCI.CondValue == CondValue && PCI.RetLocal == RV.SourceLocal) {
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
          const Place &Dest = S.getAssign().Dest;
          if (!Dest.Projections.empty()) {
            for (unsigned I = 0; I < Dest.Projections.size(); ++I) {
              // Type of the prefix place ending just before projection I, i.e.
              // the operand that projection I reads from.
              QualType PrefixTy = I == 0 ? B.getLocal(Dest.Base).Ty
                                         : Dest.Projections[I - 1].ResultTy;
              // True when projection I dereferences the prefix to reach the
              // destination, so the prefix pointer must already be initialized:
              // always for Deref, and for an index only when the indexed operand
              // is a pointer (a real array is read in place, loading no pointer).
              bool LoadsPointer = false;
              switch (Dest.Projections[I].K) {
              case ProjectionElem::Deref:
                LoadsPointer = true;
                break;
              case ProjectionElem::Index:
              case ProjectionElem::ConstantIndex:
                LoadsPointer = !PrefixTy.isNull() && PrefixTy->isPointerType();
                break;
              case ProjectionElem::Field:
                break;
              }
              if (LoadsPointer) {
                Place Prefix(Dest.Base, Dest.Projections.slice(0, I), PrefixTy,
                             Dest.Loc);
                checkOperand(Operand::createCopy(Prefix), State, S.Loc, Diags);
              }
            }
          }
        }

        // Callee-side ensure_init checks (zone-independent).
        checkEnsureInitAssign(S, State, TempToEnsureInitParam, Diags);
        checkEnsureInitDerefReads(S, State, TempToEnsureInitParam, Diags);
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
