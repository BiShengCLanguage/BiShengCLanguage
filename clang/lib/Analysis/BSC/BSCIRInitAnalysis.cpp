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
/// operands and for values that do not fit (e.g. a _BitInt(N>64) literal).
static bool foldConstOperand(const Operand &Op, int64_t &Out) {
  if (Op.K == Operand::Constant && Op.getConstVal().isInt()) {
    const llvm::APSInt &I = Op.getConstVal().getInt();
    if (I.isUnsigned() ? I.getActiveBits() > 63 : I.getSignificantBits() > 64)
      return false;
    Out = I.isUnsigned() ? (int64_t)I.getZExtValue() : I.getSExtValue();
    return true;
  }
  return false;
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

/// The place `&`, `&_Mut` or `&_Const` takes, or null for any other rvalue.
static const Place *addressedPlace(const Rvalue &Src) {
  if (Src.K == Rvalue::AddressOf)
    return &Src.getAddrOf().P;
  if (Src.K == Rvalue::Ref)
    return &Src.getRef().P;
  return nullptr;
}

/// What `_0 = Src` makes the return value.
static InitLattice::ReturnValue classifyReturned(const Rvalue &Src) {
  InitLattice::ReturnValue RV;
  int64_t CV = 0;
  if (Src.K == Rvalue::Use && foldConstOperand(Src.getUse().Op, CV)) {
    RV.K = InitLattice::ReturnValue::Constant;
    RV.Const = CV;
  } else if (auto L = asCopiedLocal(Src)) {
    RV.K = InitLattice::ReturnValue::Local;
    RV.Src = *L;
  } else {
    RV.K = InitLattice::ReturnValue::Unknown;
  }
  return RV;
}

static bool isNamedLocal(const LocalDecl &LD) {
  return !LD.IsTemp && !LD.Name.empty();
}

static unsigned calleeNumParams(const Terminator::CallData &CD) {
  if (CD.Decl)
    return CD.Decl->getNumParams();
  return CD.CalleeProtoType ? CD.CalleeProtoType->getNumParams() : 0;
}

/// The first \p N indices of \p FP.
static FieldPath pathPrefix(const FieldPath &FP, unsigned N) {
  FieldPath Prefix;
  Prefix.Base = FP.Base;
  Prefix.Indices.assign(FP.Indices.begin(), FP.Indices.begin() + N);
  return Prefix;
}

/// Compute the meet of two init states (intersection semantics).
static InitState meetStates(InitState A, InitState B) {
  if (A == B)
    return A;
  // Any combination of different states produces MaybeInit:
  // Init + Uninit, Init + MaybeInit, or MaybeInit + Uninit.
  return InitState::MaybeInit;
}

/// Meet \p Src into \p Dst key by key; a key only \p Src has is copied.
/// Returns true if \p Dst changed.
template <class MapT>
static bool meetMapInto(const MapT &Src, MapT &Dst) {
  bool Changed = false;
  for (const auto &E : Src) {
    auto It = Dst.find(E.first);
    if (It == Dst.end()) {
      Dst[E.first] = E.second;
      Changed = true;
      continue;
    }
    InitState New = meetStates(E.second, It->second);
    if (New != It->second) {
      It->second = New;
      Changed = true;
    }
  }
  return Changed;
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

/// Drop every fact that names \p L.
static void invalidateLocal(InitLattice &State, LocalId L) {
  llvm::erase_if(State.PendingCondInits,
                 [&](const InitLattice::PendingCondInit &P) {
                   return P.RetLocal == L || P.Out.Path.Base == L;
                 });
  State.ComparisonFacts.erase(L);
  SmallVector<LocalId, 4> ToErase;
  for (const auto &Entry : State.ComparisonFacts)
    if (Entry.second.ComparedLocal == L)
      ToErase.push_back(Entry.first);
  for (LocalId K : ToErase)
    State.ComparisonFacts.erase(K);
}

//===----------------------------------------------------------------------===//
// InitAnalysis: Dataflow Implementation
//===----------------------------------------------------------------------===//

InitLattice InitAnalysis::entryState(const Body &B) const {
  InitLattice State;

  // Return slot (_0) starts uninitialized
  setInitState(State, LocalId{0}, InitState::Uninitialized);

  bool Ignored = false;
  // Parameters (1..NumParams) start initialized, including all nested fields;
  // the pointee of a contract parameter starts uninitialized.
  for (unsigned I = 1; I <= B.NumParams; ++I) {
    markLocalFullyInit(State, LocalId{I}, Ignored);
    if (const ParmVarDecl *PVD = contractParamDecl(LocalId{I})) {
      QualType PointeeTy = PVD->getType()->getPointeeType();
      State.EnsureInitDerefStates[LocalId{I}] =
          isVacuouslyInitialized(PointeeTy) ? InitState::Initialized
                                            : InitState::Uninitialized;
    }
  }

  // All other locals start uninitialized, except globals/statics and
  // va_list types which are treated as fully initialized.
  for (unsigned I = B.NumParams + 1; I < B.Locals.size(); ++I) {
    if (isImplicitlyInitialized(B.getLocal(LocalId{I}), B))
      markLocalFullyInit(State, LocalId{I}, Ignored);
    else
      setInitState(State, LocalId{I}, InitState::Uninitialized);
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
      setInitState(State, Dest, InitState::Initialized, &Changed);
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
      auto DS = getDerefState(State, DestId);
      if (DS && *DS != InitState::Initialized)
        State.ReassignedParams[DestId].push_back(S.Loc);
    }

    // Caller-side ensure_init_if_ret tracking. Order matters: alias-invalidate
    // first (the local may be mutated via the alias), then drop Dest's old
    // facts, then derive new ones from the RHS.
    if (S.getAssign().Dest.isLocal()) {
      LocalId DestId = S.getAssign().Dest.Base;
      const Rvalue &Src = S.getAssign().Src;

      // Only address-of or a mutable borrow can change the local; an
      // immutable (&_Const) borrow must not invalidate the association.
      if (const Place *P = addressedPlace(Src))
        if (P->Projections.empty() &&
            (Src.K == Rvalue::AddressOf || Src.getRef().BK == BorrowKind::Mut))
          invalidateLocal(State, P->Base);

      // DestId is overwritten: drop every fact about it (and any fact that
      // compares against it).
      invalidateLocal(State, DestId);

      if (DestId == LocalId{0}) {
        InitLattice::ReturnValue RV = classifyReturned(Src);
        RV.Loc = S.Loc;
        if (!(State.RetValue == RV)) {
          State.RetValue = RV;
          Changed = true;
        }
      }

      if (Src.K == Rvalue::BinaryOp) {
        const auto &BinOp = Src.getBinOp();
        if (BinOp.Op == BO_EQ || BinOp.Op == BO_NE) {
          auto tryExtract = [&](const Operand &Local, const Operand &Const,
                                LocalId &OutLocal, int64_t &OutValue) -> bool {
            auto L = asCopiedLocal(Local);
            int64_t CV = 0;
            if (!L || !foldConstOperand(Const, CV))
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
        QualType CastTo =
            Src.K == Rvalue::Cast ? Src.getCast().Ty : QualType();
        QualType CastFrom = Src.K == Rvalue::Cast &&
                                    Src.getCast().Op.K == Operand::Copy
                                ? Src.getCast().Op.getPlace().Ty
                                : QualType();
        SmallVector<InitLattice::PendingCondInit, 2> NewPCIs;
        for (const auto &PCI : State.PendingCondInits) {
          if (PCI.RetLocal != SrcId)
            continue;
          InitLattice::PendingCondInit NewPCI = PCI;
          if (!CastTo.isNull() && !recordFollowsCast(NewPCI, CastFrom, CastTo))
            continue;
          NewPCI.RetLocal = DestId;
          NewPCIs.push_back(NewPCI);
        }
        for (const auto &PCI : NewPCIs)
          State.PendingCondInits.push_back(PCI);
        // A comparison reaches its branch through the builder's own temps.
        // Storing it in a named variable leaves the recognised guard forms.
        auto FactIt = State.ComparisonFacts.find(SrcId);
        if (FactIt != State.ComparisonFacts.end() &&
            B.getLocal(DestId).IsTemp) {
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
      markLocalFullyInit(State, SL, Changed);
    } else {
      setInitState(State, SL, InitState::Uninitialized, &Changed);
      clearFieldStates(State, SL, Changed);
    }
    invalidateLocal(State, SL);
    break;
  }

  case Statement::StorageDead: {
    // Variable goes out of scope: mark uninitialized
    LocalId SL = S.getStorageLocal();
    setInitState(State, SL, InitState::Uninitialized, &Changed);
    clearFieldStates(State, SL, Changed);
    invalidateLocal(State, SL);
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
      setInitState(Result, CD.Dest.Base, InitState::Initialized);
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
      unsigned NumParams = calleeNumParams(CD);

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
            AddressedPlace Addressed = classifyContractArg(CD, I);
            // A re-pointed param addresses a different pointee.
            if (Addressed.Recognised &&
                !(Addressed.Pointee &&
                  Result.ReassignedParams.count(Addressed.Path.Base))) {
              InitLattice::PendingCondInit PCI;
              PCI.Out = Addressed;
              PCI.RetLocal = CD.Dest.Base;
              PCI.CondValue = EIIRCondValue;
              PCI.RetTy = B.getLocal(CD.Dest.Base).Ty;
              invalidateForThisCall();
              llvm::erase_if(Result.PendingCondInits,
                             [&](const InitLattice::PendingCondInit &P) {
                               return P.Out.Path == PCI.Out.Path;
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

    // Only the two-way branch a recognised guard lowers to proves anything:
    // one listed value plus `otherwise`. A written `switch` is not one of the
    // guard forms, and its arms may share a target, which proves nothing.
    auto classifyEdge = [&](BasicBlockId Tgt,
                            bool &IsTrueEdge, bool &IsFalseEdge) {
      IsTrueEdge = false;
      IsFalseEdge = false;
      // An edge proves the comparison only when one discriminant value reaches
      // it; arms that share a target are reachable either way.
      if (SW.Targets.size() != 1)
        return;
      bool ListedIsZero = SW.Targets[0].first.getZExtValue() == 0;
      if (SW.Targets[0].second == Tgt) {
        IsFalseEdge = ListedIsZero;
        IsTrueEdge = !ListedIsZero;
      } else if (Tgt == SW.Otherwise) {
        IsTrueEdge = ListedIsZero;
        IsFalseEdge = !ListedIsZero;
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
        // Matching branch => inner returned cond => the place is init.
        if ((IsEq && IsTrueEdge) || (!IsEq && IsFalseEdge)) {
          bool Ignored = false;
          markAddressedPlaceInit(Result, PCI.Out, /*CreditPointeeOfLocal=*/false,
                                 Ignored);
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

bool InitAnalysis::recordFollowsCast(InitLattice::PendingCondInit &PCI,
                                     QualType From, QualType To) const {
  if (From.isNull() || To.isNull() || !B.SourceFD)
    return false;
  if (!From->isIntegralOrEnumerationType() || !To->isIntegralOrEnumerationType())
    return false;
  ASTContext &Ctx = B.SourceFD->getASTContext();
  // Integer conversions reduce mod 2^width (bool aside): only a lost width merges values.
  bool Identifiable =
      Ctx.hasSameUnqualifiedType(From, To) ||
      (!To->isBooleanType() && Ctx.getIntWidth(To) >= Ctx.getIntWidth(From)) ||
      (!PCI.RetTy.isNull() &&
       To.getCanonicalType() == PCI.RetTy.getCanonicalType());
  if (!Identifiable)
    return false;
  PCI.CondValue = convertedTo(PCI.CondValue, To);
  return true;
}

int64_t InitAnalysis::convertedTo(int64_t V, QualType Ty) const {
  if (!B.SourceFD || Ty.isNull() || !Ty->isIntegralOrEnumerationType())
    return V;
  // A conversion to bool yields 0 or 1, not the low bit.
  if (Ty->isBooleanType())
    return V != 0;
  unsigned Width = B.SourceFD->getASTContext().getIntWidth(Ty);
  if (Width == 0 || Width >= 64)
    return V;
  llvm::APInt Converted = llvm::APInt(64, (uint64_t)V).trunc(Width);
  return Ty->isSignedIntegerOrEnumerationType()
             ? Converted.getSExtValue()
             : (int64_t)Converted.getZExtValue();
}

int64_t InitAnalysis::asReturnedValue(int64_t V) const {
  return B.Locals.empty() ? V : convertedTo(V, B.Locals[0].Ty);
}

bool InitAnalysis::merge(const InitLattice &Src, InitLattice &Dst) const {
  bool Changed = meetMapInto(Src.LocalStates, Dst.LocalStates);
  Changed |= meetMapInto(Src.EnsureInitDerefStates, Dst.EnsureInitDerefStates);

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

  // ComparisonFacts: a fact holds only when every incoming path agrees on
  // it, so intersect by value.
  if (intersectMapByValue(Src.ComparisonFacts, Dst.ComparisonFacts))
    Changed = true;

  // The return value must agree on every incoming path; a path that never
  // wrote _0 disagrees with every value.
  if (!(Src.RetValue == Dst.RetValue) &&
      Dst.RetValue.K != InitLattice::ReturnValue::Unknown) {
    Dst.RetValue = InitLattice::ReturnValue();
    Dst.RetValue.K = InitLattice::ReturnValue::Unknown;
    Changed = true;
  }

  return Changed;
}

//===----------------------------------------------------------------------===//
// Diagnostic Helpers
//===----------------------------------------------------------------------===//

AddressedPlace InitAnalysis::classifyAddressedPlace(const Place &P) const {
  AddressedPlace Addressed;
  Addressed.Path.Base = P.Base;

  if (P.Projections.empty()) {
    Addressed.Recognised = true;
    return Addressed;
  }

  // `p[0]` names the pointee, exactly as `*p` does. Over an array it is a real
  // element, and one element never initializes the array.
  bool NamesPointee =
      P.Projections[0].K == ProjectionElem::Deref ||
      (P.Projections[0].K == ProjectionElem::ConstantIndex &&
       P.Projections[0].ConstIdx == 0 &&
       B.getLocal(P.Base).Ty->isPointerType());
  if (!NamesPointee) {
    if (auto FP = getFieldPath(P)) {
      Addressed.Path = *FP;
      Addressed.Recognised = true;
    }
    return Addressed;
  }

  Addressed.Pointee = true;
  if (auto Param = surelyHoldsParam(P.Base))
    Addressed.Path.Base = *Param;
  if (P.Projections.size() == 1) {
    Addressed.Recognised = true;
    return Addressed;
  }
  if (getEnsureInitPointeeType(Addressed.Path.Base).isNull())
    return Addressed;
  Place SubPlace(Addressed.Path.Base, P.Projections.slice(1), P.Ty, P.Loc);
  if (auto FP = getFieldPath(SubPlace)) {
    Addressed.Path = *FP;
    Addressed.Recognised = true;
  }
  return Addressed;
}

AddressedPlace InitAnalysis::classifyContractArg(const Terminator::CallData &CD,
                                  unsigned I) const {
  if (I < CD.ArgPlaces.size() && CD.ArgPlaces[I])
    return classifyAddressedPlace(*CD.ArgPlaces[I]);

  AddressedPlace Addressed;
  // The pointer is handed on, so the callee's contract covers our pointee.
  if (I < CD.Args.size())
    if (auto ArgBase = asPassedLocal(CD.Args[I])) {
      Addressed.Recognised = true;
      Addressed.Pointee = true;
      Addressed.Path.Base = surelyHoldsParam(*ArgBase).value_or(*ArgBase);
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
    markLocalFullyInit(State, Base, Changed);
    if (CreditPointeeOfLocal && !Repointed)
      markPointeeFullyInit(State, Base, Changed);
  }
}

const ParmVarDecl *InitAnalysis::contractParamDecl(LocalId Id) const {
  if (Id.Index < 1 || Id.Index > B.NumParams || !B.SourceFD)
    return nullptr;
  const ParmVarDecl *PVD = B.SourceFD->getParamDecl(Id.Index - 1);
  return PVD->hasAttr<EnsureInitAttr>() || PVD->hasAttr<EnsureInitIfRetAttr>()
             ? PVD
             : nullptr;
}

QualType InitAnalysis::getEnsureInitPointeeType(LocalId Id) const {
  const ParmVarDecl *PVD = contractParamDecl(Id);
  if (!PVD)
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
  if (const ParmVarDecl *PVD = contractParamDecl(Id))
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

void InitAnalysis::setInitState(InitLattice &State, LocalId Id, InitState S,
                                bool *Changed) const {
  auto It = State.LocalStates.find(Id);
  if (It != State.LocalStates.end() && It->second == S)
    return;
  State.LocalStates[Id] = S;
  if (Changed)
    *Changed = true;
}

llvm::Optional<InitState>
InitAnalysis::getDerefState(const InitLattice &State, LocalId Id) const {
  auto It = State.EnsureInitDerefStates.find(Id);
  if (It == State.EnsureInitDerefStates.end())
    return llvm::None;
  return It->second;
}

void InitAnalysis::markLocalFullyInit(InitLattice &State, LocalId Id,
                                      bool &Changed) const {
  setInitState(State, Id, InitState::Initialized, &Changed);
  markAllFieldsInit(State, Id, B.getLocal(Id).Ty, Changed);
}

void InitAnalysis::markWholeObjectInit(InitLattice &State, LocalId Base,
                                       bool &Changed) const {
  if (getDerefState(State, Base))
    markPointeeFullyInit(State, Base, Changed);
  else
    markLocalFullyInit(State, Base, Changed);
}

void InitAnalysis::reportContract(const InitLattice &State, LocalId ParamId,
                                  InitDiagKind K, SourceLocation Loc,
                                  int CondValue,
                                  SmallVectorImpl<InitDiagInfo> &Diags) const {
  Diags.emplace_back(K, Loc, B.getLocal(ParamId).Name, CondValue);
  auto RpIt = State.ReassignedParams.find(ParamId);
  if (RpIt != State.ReassignedParams.end())
    Diags.back().NoteLocs.assign(RpIt->second.begin(), RpIt->second.end());
}

/// The failure kind for a contract pointee in state \p DS, blaming a re-point
/// on this path when there is one.
static InitDiagKind contractFailureKind(bool IfRet, bool Reassigned,
                                        InitState DS) {
  if (Reassigned)
    return IfRet ? InitDiagKind::EnsureInitIfRetReassigned
                 : InitDiagKind::EnsureInitReassigned;
  if (DS == InitState::Uninitialized)
    return IfRet ? InitDiagKind::EnsureInitIfRetNotInit
                 : InitDiagKind::EnsureInitNotInit;
  return IfRet ? InitDiagKind::EnsureInitIfRetMaybeNotInit
               : InitDiagKind::EnsureInitMaybeNotInit;
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
  return std::distance(RD->field_begin(), RD->field_end());
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

QualType InitAnalysis::trackedRootType(LocalId Id) const {
  QualType PointeeTy = getEnsureInitPointeeType(Id);
  return PointeeTy.isNull() ? B.getLocal(Id).Ty : PointeeTy;
}

template <class Fn>
void InitAnalysis::walkFieldPath(LocalId Base, ArrayRef<unsigned> Path,
                                 Fn F) const {
  QualType Ty = trackedRootType(Base);
  for (unsigned I = 0; I < Path.size(); ++I) {
    const RecordDecl *RD = Ty->getAsRecordDecl();
    if (!RD)
      return;
    const FieldDecl *FD = getFieldAt(RD, Path[I]);
    if (!F(RD, FD, I) || !FD)
      return;
    Ty = FD->getType();
  }
}

QualType InitAnalysis::getFieldType(LocalId Id,
                                    ArrayRef<unsigned> Path) const {
  QualType Ty = trackedRootType(Id);
  walkFieldPath(Id, Path, [&](const RecordDecl *, const FieldDecl *FD, unsigned) {
    if (FD)
      Ty = FD->getType();
    return true;
  });
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

  QualType CurTy = trackedRootType(P.Base);

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

bool InitAnalysis::isFieldPathCovered(const InitLattice &State,
                                      const FieldPath &FP) const {
  if (isVacuouslyInitialized(getFieldType(FP.Base, FP.Indices)))
    return true;
  if (getFieldInitState(State, FP) == InitState::Initialized)
    return true;
  // A whole-struct or union write covers every path below it.
  for (unsigned I = 1; I < FP.Indices.size(); ++I)
    if (getFieldInitState(State, pathPrefix(FP, I)) == InitState::Initialized)
      return true;
  return false;
}

/// Set one field path Initialized, flagging \p Changed when it moves.
static void setFieldInit(InitLattice &State, const FieldPath &FP,
                         bool &Changed) {
  auto &FS = State.FieldStates[FP];
  if (FS != InitState::Initialized) {
    FS = InitState::Initialized;
    Changed = true;
  }
}

void InitAnalysis::markFieldInit(InitLattice &State, const FieldPath &FP,
                                 bool &Changed) const {
  // Only a whole-variant write to the outermost union on the path covers it.
  if (auto FirstU = firstUnionDepth(FP)) {
    if (*FirstU + 1 != FP.Indices.size())
      return;
    if (*FirstU == 0) {
      markWholeObjectInit(State, FP.Base, Changed);
      return;
    }
    markFieldInit(State, pathPrefix(FP, *FirstU), Changed);
    return;
  }

  setFieldInit(State, FP, Changed);
  tryPromoteParent(State, FP, Changed);
}

void InitAnalysis::tryPromoteParent(InitLattice &State, const FieldPath &FP,
                                    bool &Changed) const {
  if (FP.Indices.empty())
    return;

  FieldPath Parent = pathPrefix(FP, FP.Indices.size() - 1);

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
    markWholeObjectInit(State, FP.Base, Changed);
  } else {
    setFieldInit(State, Parent, Changed);
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
  if (!PointeeTy.isNull())
    markAllFieldsInit(State, Base, PointeeTy, Changed);
}

/// Mark \p Path and every field below \p Ty under it initialized.
static void markFieldsBelow(InitLattice &State, FieldPath &Path, QualType Ty,
                            bool &Changed) {
  const RecordDecl *RD = Ty->getAsRecordDecl();
  if (!RD)
    return;
  unsigned FIdx = 0;
  for (const FieldDecl *FD : RD->fields()) {
    Path.Indices.push_back(FIdx++);
    // Unions are covered whole, not per variant.
    const RecordDecl *FieldRD = FD->getType()->getAsRecordDecl();
    if (FieldRD && !FieldRD->isUnion())
      markFieldsBelow(State, Path, FD->getType(), Changed);
    setFieldInit(State, Path, Changed);
    Path.Indices.pop_back();
  }
}

void InitAnalysis::markAllFieldsInit(InitLattice &State, LocalId Base,
                                     QualType Ty, bool &Changed) const {
  FieldPath Path;
  Path.Base = Base;
  markFieldsBelow(State, Path, Ty, Changed);
}

std::string InitAnalysis::buildFieldName(const FieldPath &FP) const {
  std::string Name = B.getLocal(FP.Base).Name.str();
  walkFieldPath(FP.Base, FP.Indices,
                [&](const RecordDecl *, const FieldDecl *FD, unsigned) {
                  if (FD && !FD->isAnonymousStructOrUnion())
                    Name += "." + FD->getNameAsString();
                  return true;
                });
  return Name;
}

llvm::Optional<unsigned>
InitAnalysis::firstUnionDepth(const FieldPath &FP) const {
  llvm::Optional<unsigned> Depth;
  walkFieldPath(FP.Base, FP.Indices,
                [&](const RecordDecl *RD, const FieldDecl *, unsigned I) {
                  if (RD->isUnion())
                    Depth = I;
                  return !Depth;
                });
  return Depth;
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
    if (P.Ty.isOrContainsOwned(BSCLookThrough::NoPointer))
      return;
  }

  if (P.Loc.isValid())
    Loc = P.Loc;

  LocalId Id = P.Base;
  InitState IS = getInitState(State, Id);

  // An Initialized local (for unions: covered) makes every field access fine.
  if (IS == InitState::Initialized)
    return;
  const LocalDecl &LD = B.getLocal(Id);
  if (!isNamedLocal(LD))
    return;
  auto report = [&](InitState S, StringRef Name) {
    Diags.emplace_back(S == InitState::MaybeInit ? InitDiagKind::UseOfMaybeUninit
                                                 : InitDiagKind::UseOfUninit,
                       Loc.isValid() ? Loc : LD.DeclLoc, Name);
  };

  // Check field-level state if the operand has a field projection.
  if (auto FP = getFieldPathPrefix(P)) {
    if (isFieldPathCovered(State, *FP))
      return;
    // Nothing inside an uncovered union is readable; name that union.
    if (auto FirstU = firstUnionDepth(*FP)) {
      FieldPath UnionFP = pathPrefix(*FP, *FirstU);
      report(*FirstU == 0 ? IS : getFieldInitState(State, UnionFP),
             buildFieldName(UnionFP));
      return;
    }
    report(getFieldInitState(State, *FP), buildFieldName(*FP));
    return;
  }

  report(IS, LD.Name);
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

llvm::DenseSet<unsigned>
InitAnalysis::contractArgIndices(const Terminator::CallData &CD) const {
  llvm::DenseSet<unsigned> Indices;
  if (CD.Decl && CD.Decl->getBuiltinID() == Builtin::BI__assume_initialized) {
    for (unsigned I = 0; I < CD.Args.size(); ++I)
      Indices.insert(I);
    return Indices;
  }
  unsigned NumParams = std::min<size_t>(calleeNumParams(CD), CD.Args.size());
  for (unsigned I = 0; I < NumParams; ++I) {
    int Cond = 0;
    if (classifyEnsureInit(CD.Decl, CD.CalleeProtoType, I, Cond) ==
        EnsureInitKind::None)
      continue;
    if (I < CD.ArgPlaces.size() && CD.ArgPlaces[I])
      Indices.insert(I);
  }
  return Indices;
}

llvm::DenseSet<LocalId> InitAnalysis::contractArgTemps(
    const Terminator::CallData &CD,
    const llvm::DenseSet<unsigned> &ContractArgs) const {
  llvm::DenseSet<LocalId> Temps;
  for (unsigned I : ContractArgs)
    if (auto L = asPassedLocal(CD.Args[I]))
      Temps.insert(*L);
  return Temps;
}

llvm::Optional<LocalId> InitAnalysis::pointerHandedOn(const Rvalue &Src) const {
  const Operand *Op = nullptr;
  switch (Src.K) {
  case Rvalue::Use:
    Op = &Src.getUse().Op;
    break;
  case Rvalue::Cast:
    if (Src.getCast().Ty->isPointerType())
      Op = &Src.getCast().Op;
    break;
  case Rvalue::Ref:
  case Rvalue::AddressOf: {
    const Place &P = *addressedPlace(Src);
    // A reborrow's place is the pointer, but the result carries its value.
    if (P.Projections.empty())
      return Src.K == Rvalue::Ref && Src.getRef().IsReborrow
                 ? llvm::Optional<LocalId>(P.Base)
                 : llvm::None;
    // `&p->f` and `&p[0]` point into the pointee just as `&*p` does.
    if (projectionLoadsPointer(P, 0, B.getLocal(P.Base).Ty))
      return P.Base;
    return llvm::None;
  }
  default:
    return llvm::None;
  }
  if (!Op || Op->K != Operand::Copy || !Op->getPlace().Projections.empty())
    return llvm::None;
  return Op->getPlace().Base;
}

InitAnalysis::ContractPointerFlow
InitAnalysis::flowOf(const Rvalue &Src) const {
  ContractPointerFlow Held;
  auto add = [](SmallVectorImpl<LocalId> &To, ArrayRef<LocalId> From) {
    for (LocalId L : From)
      if (!llvm::is_contained(To, L))
        To.push_back(L);
  };
  auto addLocal = [&](LocalId L) {
    if (isContractParam(L)) {
      add(Held.Params, L);
      return;
    }
    auto It = TempFlows.find(L);
    if (It == TempFlows.end()) {
      Held.Other = true;
      return;
    }
    add(Held.Params, It->second.Params);
    add(Held.ParamAddresses, It->second.ParamAddresses);
    Held.Other |= It->second.Other;
  };

  // `&p` is the parameter's own address, not its pointer.
  if (const Place *P = addressedPlace(Src))
    if (P->Projections.empty() &&
        !(Src.K == Rvalue::Ref && Src.getRef().IsReborrow)) {
      if (isContractParam(P->Base))
        add(Held.ParamAddresses, P->Base);
      else
        Held.Other = true;
      return Held;
    }
  // `*a` loads p's pointer where a holds `&p`, and something else wherever a
  // may hold any other address.
  if (Src.K == Rvalue::Use && Src.getUse().Op.K == Operand::Copy) {
    const Place &P = Src.getUse().Op.getPlace();
    if (P.Projections.size() == 1 &&
        P.Projections[0].K == ProjectionElem::Deref) {
      auto It = TempFlows.find(P.Base);
      if (It == TempFlows.end()) {
        Held.Other = true;
        return Held;
      }
      add(Held.Params, It->second.ParamAddresses);
      Held.Other = It->second.Other || !It->second.Params.empty() ||
                   It->second.ParamAddresses.empty();
      return Held;
    }
  }
  // An aggregate carries whatever its elements hold, but is none of them.
  if (Src.K == Rvalue::Aggregate || Src.K == Rvalue::Array) {
    for (const Operand &Op : Src.K == Rvalue::Aggregate
                                 ? Src.getAgg().Fields
                                 : Src.getArray().Elements)
      if (Op.K == Operand::Copy && Op.getPlace().Projections.empty())
        addLocal(Op.getPlace().Base);
    Held.Other = true;
    return Held;
  }
  if (auto L = pointerHandedOn(Src)) {
    addLocal(*L);
    return Held;
  }
  // A truth value carries no pointer.
  if ((Src.K == Rvalue::BinaryOp &&
       BinaryOperator::isComparisonOp(Src.getBinOp().Op)) ||
      (Src.K == Rvalue::UnaryOp && Src.getUnOp().Op == UO_LNot) ||
      (Src.K == Rvalue::Cast && Src.getCast().Ty->isBooleanType())) {
    Held.Other = true;
    return Held;
  }
  // An unrecognised form still carries whatever its operands hold, or the
  // guards would go silent on it rather than conservative.
  forEachRvalueOperand(Src, [&](const Operand &Op) {
    if ((Op.K == Operand::Copy || Op.K == Operand::Move) &&
        Op.getPlace().Projections.empty())
      addLocal(Op.getPlace().Base);
  });
  Held.Other = true;
  return Held;
}

void InitAnalysis::collectContractPointerFlow() {
  // A temp belongs to one expression, so any path reaching it reaches its reader.
  for (const BasicBlock &BB : B.Blocks)
    if (BB.Term.K == Terminator::Call && BB.Term.getCall().Dest.isLocal() &&
        B.getLocal(BB.Term.getCall().Dest.Base).IsTemp)
      TempFlows[BB.Term.getCall().Dest.Base].Other = true;
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (const BasicBlock &BB : B.Blocks) {
      for (const Statement &S : BB.Statements) {
        if (S.K != Statement::Assign || !S.getAssign().Dest.isLocal())
          continue;
        LocalId DestId = S.getAssign().Dest.Base;
        if (!B.getLocal(DestId).IsTemp)
          continue;
        ContractPointerFlow Held = flowOf(S.getAssign().Src);
        ContractPointerFlow &Flow = TempFlows[DestId];
        for (LocalId P : Held.Params)
          if (!llvm::is_contained(Flow.Params, P)) {
            Flow.Params.push_back(P);
            Changed = true;
          }
        for (LocalId P : Held.ParamAddresses)
          if (!llvm::is_contained(Flow.ParamAddresses, P)) {
            Flow.ParamAddresses.push_back(P);
            Changed = true;
          }
        if (Held.Other && !Flow.Other) {
          Flow.Other = true;
          Changed = true;
        }
      }
    }
  }
}

SmallVector<LocalId, 1> InitAnalysis::mayHoldParam(LocalId L) const {
  if (isContractParam(L))
    return {L};
  auto It = TempFlows.find(L);
  if (It == TempFlows.end())
    return {};
  return It->second.Params;
}

llvm::Optional<LocalId> InitAnalysis::surelyHoldsParam(LocalId L) const {
  if (isContractParam(L))
    return L;
  auto It = TempFlows.find(L);
  if (It == TempFlows.end() || It->second.Other ||
      !It->second.ParamAddresses.empty() || It->second.Params.size() != 1)
    return llvm::None;
  return It->second.Params.front();
}

void InitAnalysis::checkEnsureInitAssign(
    const Statement &S, const InitLattice &State,
    SmallVectorImpl<InitDiagInfo> &Diags) const {
  // Re-pointing is deferred to the at-return check; ReassignedParams
  // gating keeps it sound, and the failing return gets the error + note.

  // A temp is followed by ContractPointerFlow; storing into a named variable,
  // or into a field or element of one, is the escape.
  const LocalDecl &DestLD = B.getLocal(S.getAssign().Dest.Base);
  if (!isNamedLocal(DestLD))
    return;
  ContractPointerFlow Held = flowOf(S.getAssign().Src);
  auto noteAlias = [&](LocalId ParamId) {
    // Writing the parameter itself re-points it; that is the return check's.
    if (S.getAssign().Dest.Projections.empty() &&
        S.getAssign().Dest.Base == ParamId)
      return;
    auto DS = getDerefState(State, ParamId);
    if (!DS || *DS == InitState::Initialized)
      return;
    const LocalDecl &ParamLD = B.getLocal(ParamId);
    if (ParamLD.Name.empty())
      return;
    Diags.emplace_back(InitDiagKind::EnsureInitPtrAliased,
                       S.Loc.isValid() ? S.Loc : DestLD.DeclLoc, ParamLD.Name);
    Diags.back().AttrSelect = getIfRetCondValue(ParamId) ? 1 : 0;
  };
  for (LocalId P : Held.Params)
    noteAlias(P);
  for (LocalId P : Held.ParamAddresses)
    noteAlias(P);
}

void InitAnalysis::checkEnsureInitPointeeRead(
    const Place &P, const InitLattice &State, SourceLocation Loc,
    SmallVectorImpl<InitDiagInfo> &Diags) const {
  // Only a projection that dereferences the base reads the pointee.
  if (P.Projections.empty() ||
      !projectionLoadsPointer(P, 0, B.getLocal(P.Base).Ty))
    return;
  // A terminator carries no location of its own; point at the read.
  if (P.Loc.isValid())
    Loc = P.Loc;
  for (LocalId ParamId : mayHoldParam(P.Base)) {
    auto DS = getDerefState(State, ParamId);
    if (!DS || *DS == InitState::Initialized)
      continue;
    // A read of one field only needs that field, not the whole pointee.
    if (P.Projections.size() > 1 &&
        P.Projections[0].K == ProjectionElem::Deref) {
      Place SubPlace(ParamId, P.Projections.slice(1), P.Ty, P.Loc);
      if (auto FP = getFieldPathPrefix(SubPlace))
        if (isFieldPathCovered(State, *FP))
          continue;
    }
    const LocalDecl &ParamLD = B.getLocal(ParamId);
    if (ParamLD.Name.empty())
      continue;
    Diags.emplace_back(InitDiagKind::EnsureInitDerefReadUninit,
                       Loc.isValid() ? Loc : ParamLD.DeclLoc, ParamLD.Name);
    Diags.back().AttrSelect = getIfRetCondValue(ParamId) ? 1 : 0;
  }
}

void InitAnalysis::checkEnsureInitDerefReads(
    const Statement &S, const InitLattice &State,
    const llvm::DenseSet<LocalId> &EnsureInitArgTemps,
    SmallVectorImpl<InitDiagInfo> &Diags) const {
  // Addressing the pointee for a contract callee delegates it, not reads it.
  if (S.getAssign().Dest.isLocal() &&
      EnsureInitArgTemps.count(S.getAssign().Dest.Base))
    return;

  forEachRvalueOperand(S.getAssign().Src, [&](const Operand &Op) {
    if (Op.K == Operand::Constant)
      return;
    checkEnsureInitPointeeRead(Op.getPlace(), State, S.Loc, Diags);
  });
}

void InitAnalysis::checkEnsureInitAtReturn(
    const Terminator &T, const InitLattice &State,
    SmallVectorImpl<InitDiagInfo> &Diags) const {
  for (const auto &Entry : State.EnsureInitDerefStates) {
    // Skip ensure_init_if params — checked separately per return path.
    if (getIfRetCondValue(Entry.first))
      continue;
    if (Entry.second == InitState::Initialized)
      continue;
    SourceLocation DiagLoc = T.Loc.isValid()
        ? T.Loc
        : (B.SourceFD ? B.SourceFD->getBodyRBrace() : SourceLocation());
    reportContract(State, Entry.first,
                   contractFailureKind(/*IfRet=*/false,
                                       State.ReassignedParams.count(Entry.first),
                                       Entry.second),
                   DiagLoc, 0, Diags);
  }
}

SmallVector<InitAnalysis::ReturnArm, 2>
InitAnalysis::collectReturnArms(const DataflowResult<InitLattice> &Result,
                                const InitLattice &PredState) const {
  SmallVector<ReturnArm, 2> Arms;
  auto whole = [&] {
    Arms.clear();
    Arms.push_back({PredState.RetValue, PredState});
    // The local _0 copied may be dead by now; _0 carries its records.
    Arms.back().RV.Src = LocalId{0};
    return Arms;
  };
  const InitLattice::ReturnValue &Returned = PredState.RetValue;
  if (Returned.K != InitLattice::ReturnValue::Local ||
      !B.getLocal(Returned.Src).IsTemp)
    return whole();

  struct Cast {
    QualType From, To;
  };
  struct Pending {
    LocalId Temp;
    // Innermost first.
    SmallVector<Cast, 2> Casts;
  };
  SmallVector<Pending, 4> Worklist;
  llvm::DenseSet<LocalId> Seen;
  Worklist.push_back({Returned.Src, {}});
  Seen.insert(Returned.Src);

  while (!Worklist.empty()) {
    Pending P = Worklist.pop_back_val();
    auto addArm = [&](InitLattice::ReturnValue RV, InitLattice State) {
      RV.Loc = Returned.Loc;
      if (RV.K == InitLattice::ReturnValue::Constant)
        for (const Cast &C : P.Casts)
          RV.Const = convertedTo(RV.Const, C.To);
      if (RV.K == InitLattice::ReturnValue::Local) {
        SmallVector<InitLattice::PendingCondInit, 2> Kept;
        for (InitLattice::PendingCondInit PCI : State.PendingCondInits)
          if (PCI.RetLocal != RV.Src ||
              llvm::all_of(P.Casts, [&](const Cast &C) {
                return recordFollowsCast(PCI, C.From, C.To);
              }))
            Kept.push_back(PCI);
        State.PendingCondInits = std::move(Kept);
      }
      Arms.push_back({RV, std::move(State)});
    };

    bool Defined = false;
    for (const BasicBlock &BB : B.Blocks) {
      auto EntryIt = Result.EntryStates.find(BB.Id);
      if (EntryIt == Result.EntryStates.end())
        continue;
      InitLattice State = EntryIt->second;
      for (const Statement &S : BB.Statements) {
        transferStatement(S, State);
        if (S.K != Statement::Assign || !S.getAssign().Dest.isLocal() ||
            S.getAssign().Dest.Base != P.Temp)
          continue;
        Defined = true;
        const Rvalue &Src = S.getAssign().Src;
        InitLattice::ReturnValue RV = classifyReturned(Src);
        if (RV.K == InitLattice::ReturnValue::Local &&
            B.getLocal(RV.Src).IsTemp) {
          if (!Seen.insert(RV.Src).second)
            return whole();
          Pending Next{RV.Src, P.Casts};
          if (Src.K == Rvalue::Cast)
            Next.Casts.insert(Next.Casts.begin(),
                              {Src.getCast().Op.getPlace().Ty,
                               Src.getCast().Ty});
          Worklist.push_back(std::move(Next));
          continue;
        }
        // The temp carries the records copied from a named source.
        if (RV.K == InitLattice::ReturnValue::Local)
          RV.Src = P.Temp;
        addArm(RV, State);
      }
      if (BB.Term.K != Terminator::Call)
        continue;
      const auto &CD = BB.Term.getCall();
      if (!CD.Dest.isLocal() || CD.Dest.Base != P.Temp)
        continue;
      Defined = true;
      InitLattice::ReturnValue RV;
      RV.K = InitLattice::ReturnValue::Local;
      RV.Src = P.Temp;
      addArm(RV, transferTerminator(BB.Term, State, CD.Successor));
    }
    if (!Defined)
      return whole();
  }
  return Arms;
}

void InitAnalysis::checkEnsureInitIfRetAtReturn(
    const DataflowResult<InitLattice> &Result,
    SmallVectorImpl<InitDiagInfo> &Diags) const {
  // ensure_init_if_ret params are a per-decl constant; collect them once.
  SmallVector<std::pair<LocalId, int>, 2> IfRetParams;
  for (unsigned I = 1; I <= B.NumParams; ++I)
    if (auto Cond = getIfRetCondValue(LocalId{I}))
      IfRetParams.push_back({LocalId{I}, *Cond});
  if (IfRetParams.empty())
    return;

  // Returns true when the arm breaks the contract of \p ParamId.
  auto checkArm = [&](const ReturnArm &Arm, LocalId ParamId, int CondValue) {
    const InitLattice::ReturnValue &RV = Arm.RV;
    SourceLocation DiagLoc = RV.Loc.isValid() ? RV.Loc
                             : B.SourceFD    ? B.SourceFD->getBodyRBrace()
                                             : SourceLocation();
    InitState DS =
        getDerefState(Arm.State, ParamId).value_or(InitState::Uninitialized);

    // Delegation credit: returning an inner ensure_init_if_ret call's
    // result with the same cond inits *param on this path.
    if (DS != InitState::Initialized &&
        RV.K == InitLattice::ReturnValue::Local) {
      for (const auto &PCI : Arm.State.PendingCondInits) {
        if (PCI.Out.Pointee && PCI.Out.Path.Base == ParamId &&
            PCI.Out.Path.Indices.empty() && PCI.CondValue == CondValue &&
            PCI.RetLocal == RV.Src) {
          DS = InitState::Initialized;
          break;
        }
      }
    }
    if (DS == InitState::Initialized)
      return false;

    if (RV.K == InitLattice::ReturnValue::Constant) {
      if (asReturnedValue(RV.Const) != CondValue)
        return false;
      reportContract(Arm.State, ParamId,
                     contractFailureKind(
                         /*IfRet=*/true,
                         Arm.State.ReassignedParams.count(ParamId), DS),
                     DiagLoc, CondValue, Diags);
    } else {
      // The runtime value may equal arg, so *p must already be init.
      reportContract(Arm.State, ParamId,
                     InitDiagKind::EnsureInitIfRetNonConstReturn, DiagLoc,
                     CondValue, Diags);
    }
    return true;
  };

  // Check every predecessor of a return block, whatever its terminator: a
  // Drop can precede the return.
  for (const BasicBlock &BB : B.Blocks) {
    if (BB.Term.K != Terminator::Return)
      continue;
    for (BasicBlockId PredId : B.getPredecessors(BB.Id)) {
      auto ExitIt = Result.ExitStates.find(PredId);
      if (ExitIt == Result.ExitStates.end())
        continue;
      SmallVector<ReturnArm, 2> Arms =
          collectReturnArms(Result, ExitIt->second);
      for (const auto &IRP : IfRetParams)
        for (const ReturnArm &Arm : Arms)
          if (checkArm(Arm, IRP.first, IRP.second))
            break;
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

    // Arguments a contract callee or __assume_initialized takes the address
    // for are delegated, not read.
    llvm::DenseSet<unsigned> ContractArgs;
    llvm::DenseSet<LocalId> EnsureInitArgTemps;
    if (BB.Term.K == Terminator::Call) {
      ContractArgs = contractArgIndices(BB.Term.getCall());
      EnsureInitArgTemps = contractArgTemps(BB.Term.getCall(), ContractArgs);
    }

    for (const Statement &S : BB.Statements) {
      // Check operands used in this statement
      if (S.K == Statement::Assign) {
        // Only check uses in _Safe zones
        if (CheckAllZones || S.IsSafe) {
          const Rvalue &Src = S.getAssign().Src;
          if (const Place *P = addressedPlace(Src)) {
            // Taking the address for a contract callee delegates, not reads.
            if (!S.getAssign().Dest.isLocal() ||
                !EnsureInitArgTemps.count(S.getAssign().Dest.Base))
              checkOperand(Operand::createCopy(*P), State, S.Loc, Diags);
          } else {
            forEachRvalueOperand(Src, [&](const Operand &Op) {
              checkOperand(Op, State, S.Loc, Diags);
            });
          }
        }

        // Callee-side ensure_init checks (zone-independent).
        checkEnsureInitAssign(S, State, Diags);
        checkEnsureInitDerefReads(S, State, EnsureInitArgTemps, Diags);

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
          if (CheckAllZones || S.IsSafe)
            checkOperand(Operand::createCopy(Prefix), State, S.Loc, Diags);
          // The prefix before projection 0 is the parameter itself.
          if (I > 0)
            checkEnsureInitPointeeRead(Prefix, State, S.Loc, Diags);
        }
      }

      // Apply transfer function to update state
      transferStatement(S, State);
    }

    // Terminator operands are read like assignment operands: the uninit check
    // in checked zones, the pointee read check everywhere.
    const Terminator &T = BB.Term;
    bool Checked = CheckAllZones || T.IsSafe;
    auto readOperand = [&](const Operand &Op) {
      if (Checked)
        checkOperand(Op, State, T.Loc, Diags);
      if (Op.K != Operand::Constant)
        checkEnsureInitPointeeRead(Op.getPlace(), State, T.Loc, Diags);
    };
    if (T.K == Terminator::Call) {
      const auto &CD = T.getCall();
      // An indirect call loads its callee, so that load reads the pointee too.
      readOperand(CD.Callee);
      for (unsigned I = 0; I < CD.Args.size(); ++I)
        if (!ContractArgs.count(I))
          readOperand(CD.Args[I]);
    } else if (T.K == Terminator::SwitchInt) {
      readOperand(T.getSwitchInt().Discriminant);
    }

    // Check return slot at Return terminator
    if (T.K == Terminator::Return) {
      if ((CheckAllZones || T.IsSafe) && !B.Locals[0].Ty->isVoidType()) {
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
      // The contract binds in every zone, unlike the return slot above.
      checkEnsureInitAtReturn(T, State, Diags);
    }
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
