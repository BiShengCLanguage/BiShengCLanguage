//===- BSCOwnershipArrayLoopClassification.cpp -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The qualifying for-loop classifier for owned-element arrays (proposal 028).
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/Analysis/Analyses/BSC/BSCOwnership.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/STLExtras.h"
#include <algorithm>
#include <functional>

namespace clang {

// qualifying for-loop classifier. This AST pre-pass decides
// which for-loops may transfer ownership of owned-element arrays (3.2.1) and
// records the sites where element transfer is allowed. The actual ownership
// state transitions are performed by the CFG dataflow analysis in
// runOwnershipAnalysis; the classifier does not compute any ownership state
// itself.

// ---------------------------------------------------------------------------
// qualifying for-loop shape checks (pure AST predicates).

// incr must be i++ / ++i / i += 1 / i = i + 1 (step one, 3.2.1 cond 3).
// Whether E is a reference to the loop variable of the current qualifying
// loop (peeling casts). Used pervasively to decide whether an array index is
// the loop index (a[i] / w.arr[i] with i == LoopVar).
static bool IsLoopIndex(const Expr *E, const VarDecl *LoopVar) {
  if (const DeclRefExpr *DRE =
          dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts()))
    return DRE->getDecl() == LoopVar;
  return false;
}

static bool IsIncrementByOne(const ASTContext &Context, const Expr *Inc,
                             const VarDecl *LoopVar) {
  if (!Inc)
    return false;
  Inc = Inc->IgnoreParens();
  if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(Inc)) {
    if (UO->isIncrementOp()) {
      if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(
              UO->getSubExpr()->IgnoreParenImpCasts()))
        return DRE->getDecl() == LoopVar;
    }
    return false;
  }
  if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(Inc)) {
    if (BO->getOpcode() == BO_AddAssign) {
      if (const DeclRefExpr *LHS = dyn_cast<DeclRefExpr>(
              BO->getLHS()->IgnoreParenImpCasts())) {
        if (LHS->getDecl() != LoopVar)
          return false;
        if (const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts()) {
          if (RHS->isIntegerConstantExpr(Context))
            return RHS->EvaluateKnownConstInt(Context) == 1;
        }
      }
    }
    if (BO->getOpcode() == BO_Assign) {
      if (const DeclRefExpr *LHS = dyn_cast<DeclRefExpr>(
              BO->getLHS()->IgnoreParenImpCasts())) {
        if (LHS->getDecl() != LoopVar)
          return false;
        if (const BinaryOperator *Add =
                dyn_cast<BinaryOperator>(BO->getRHS()->IgnoreParenImpCasts())) {
          if (Add->getOpcode() != BO_Add)
            return false;
          auto isLoopVar = [&](const Expr *E) {
            if (const DeclRefExpr *DRE =
                    dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts()))
              return DRE->getDecl() == LoopVar;
            return false;
          };
          auto isOne = [&](const Expr *E) {
            E = E->IgnoreParenImpCasts();
            return E->isIntegerConstantExpr(Context) &&
                   E->EvaluateKnownConstInt(Context) == 1;
          };
          return (isLoopVar(Add->getLHS()) && isOne(Add->getRHS())) ||
                 (isLoopVar(Add->getRHS()) && isOne(Add->getLHS()));
        }
      }
    }
  }
  return false;
}

// Recognize `i < sizeof(arr)/sizeof(arr[0])` (also `sizeof(arr)/sizeof(*arr)`)
// as an array-length bound and return the array variable it refers to, or
// nullptr when the expression does not have that shape.
static const VarDecl *GetSizeofArrayBound(const ASTContext &Context,
                                          const Expr *E) {
  E = E->IgnoreParenImpCasts();
  const BinaryOperator *BO = dyn_cast<BinaryOperator>(E);
  if (!BO || BO->getOpcode() != BO_Div)
    return nullptr;
  const UnaryExprOrTypeTraitExpr *SizeOfArr =
      dyn_cast<UnaryExprOrTypeTraitExpr>(BO->getLHS()->IgnoreParenImpCasts());
  if (!SizeOfArr || SizeOfArr->getKind() != UETT_SizeOf ||
      SizeOfArr->isArgumentType())
    return nullptr;
  const Expr *ArrExpr = SizeOfArr->getArgumentExpr()->IgnoreParenImpCasts();
  const UnaryExprOrTypeTraitExpr *SizeOfElem =
      dyn_cast<UnaryExprOrTypeTraitExpr>(BO->getRHS()->IgnoreParenImpCasts());
  if (!SizeOfElem || SizeOfElem->getKind() != UETT_SizeOf ||
      SizeOfElem->isArgumentType())
    return nullptr;
  const Expr *ElemExpr = SizeOfElem->getArgumentExpr()->IgnoreParenImpCasts();
  const DeclRefExpr *ArrDRE = dyn_cast<DeclRefExpr>(ArrExpr);
  if (!ArrDRE)
    return nullptr;
  const Expr *ElemBase = nullptr;
  if (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(ElemExpr))
    ElemBase = ASE->getBase()->IgnoreParenImpCasts();
  else if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(ElemExpr))
    if (UO->getOpcode() == UO_Deref)
      ElemBase = UO->getSubExpr()->IgnoreParenImpCasts();
  const DeclRefExpr *ElemDRE = dyn_cast_or_null<DeclRefExpr>(ElemBase);
  if (!ElemDRE || ElemDRE->getDecl() != ArrDRE->getDecl())
    return nullptr;
  return dyn_cast<VarDecl>(ArrDRE->getDecl());
}

// For `i < sizeof(arr)/sizeof(arr[0])`, return the VLA length variable of
// `arr` (sizeof(arr)/sizeof(arr[0]) == that variable at runtime). Returns
// nullptr when the bound is not a sizeof-based array length or the VLA size
// is not a simple variable.
static const VarDecl *GetSizeofArrayLengthVar(const ASTContext &Context,
                                              const Expr *E) {
  const VarDecl *ArrVD = GetSizeofArrayBound(Context, E);
  if (!ArrVD)
    return nullptr;
  if (const auto *VAT = dyn_cast_or_null<VariableArrayType>(
          ArrVD->getType()->getAsArrayTypeUnsafe()))
    if (const DeclRefExpr *SizeDRE = dyn_cast<DeclRefExpr>(
            VAT->getSizeExpr()->IgnoreParenImpCasts()))
      return dyn_cast<VarDecl>(SizeDRE->getDecl());
  return nullptr;
}

// The loop bound must equal the array length: constant N vs constant
// array size, or VLA size variable n (3.2.1 cond 2). A sizeof-based bound
// is normalized to its VLA length variable by the caller.
static bool ArrayBoundMatches(const ASTContext &Context, const VarDecl *ArrVD,
                              bool IsConstantBound,
                              const llvm::APSInt &BoundVal,
                              const VarDecl *BoundVD) {
  QualType Ty = ArrVD->getType();
  if (IsConstantBound) {
    if (const auto *CAT = Context.getAsConstantArrayType(Ty))
      return CAT->getSize().getZExtValue() == BoundVal.getZExtValue();
    return false;
  }
  if (!BoundVD)
    return false;
  if (const auto *VAT =
          dyn_cast_or_null<VariableArrayType>(Ty->getAsArrayTypeUnsafe())) {
    if (const DeclRefExpr *SizeDRE = dyn_cast<DeclRefExpr>(
            VAT->getSizeExpr()->IgnoreParenImpCasts()))
      return SizeDRE->getDecl() == BoundVD;
  }
  return false;
}

// The loop variable must not be written to / have its address taken
// anywhere except the loop increment (3.2.1 conds 3 & 4). TouchedLoc is set
// to the exact statement that modifies or address-takes the loop variable,
// so the diagnostic note can point at it.
// Whether S (or a descendant) modifies / address-takes / mutably borrows the
// loop variable, recording the first offending statement's location.
static bool TouchesLoopVar(const Stmt *S, const VarDecl *LoopVar,
                           SourceLocation &TouchedLoc) {
  if (!S)
    return false;
  if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(S)) {
    if (BO->isAssignmentOp()) {
      if (const DeclRefExpr *LHS = dyn_cast<DeclRefExpr>(
              BO->getLHS()->IgnoreParenImpCasts()))
        if (IsLoopIndex(LHS, LoopVar)) {
          TouchedLoc = S->getSourceRange().getBegin();
          return true;
        }
    }
  }
  if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(S)) {
    if (UO->isIncrementDecrementOp() || UO->getOpcode() == UO_AddrOf ||
        UO->getOpcode() == UO_AddrMut ||
        UO->getOpcode() == UO_AddrConst) {
      if (const DeclRefExpr *Sub = dyn_cast<DeclRefExpr>(
              UO->getSubExpr()->IgnoreParenImpCasts()))
        if (IsLoopIndex(Sub, LoopVar)) {
          TouchedLoc = S->getSourceRange().getBegin();
          return true;
        }
    }
  }
  for (const Stmt *Child : S->children())
    if (TouchesLoopVar(Child, LoopVar, TouchedLoc))
      return true;
  return false;
}

static bool LoopVarUntouchedOutsideIncr(const ForStmt *FS,
                                        const VarDecl *LoopVar,
                                        SourceLocation &TouchedLoc) {
  return !TouchesLoopVar(FS->getCond(), LoopVar, TouchedLoc) &&
         !TouchesLoopVar(FS->getBody(), LoopVar, TouchedLoc);
}

static bool InnerLoopShapeQualifies(const ASTContext &Context,
                                    const ForStmt *InnerFS) {
  const Stmt *Init = InnerFS->getInit();
  const VarDecl *InnerVar = nullptr;
  if (const DeclStmt *DS = dyn_cast<DeclStmt>(Init)) {
    if (!DS->isSingleDecl())
      return false;
    InnerVar = dyn_cast<VarDecl>(DS->getSingleDecl());
    if (!InnerVar || !InnerVar->getInit())
      return false;
    if (!InnerVar->getInit()->isIntegerConstantExpr(Context) ||
        InnerVar->getInit()->EvaluateKnownConstInt(Context) != 0)
      return false;
  } else {
    return false;
  }
  const Expr *Cond = InnerFS->getCond();
  const BinaryOperator *CBO =
      Cond ? dyn_cast<BinaryOperator>(Cond->IgnoreParens()) : nullptr;
  if (!CBO || CBO->getOpcode() != BO_LT)
    return false;
  const DeclRefExpr *CLHS = dyn_cast<DeclRefExpr>(
      CBO->getLHS()->IgnoreParenImpCasts());
  if (!CLHS || CLHS->getDecl() != InnerVar)
    return false;
  if (!IsIncrementByOne(Context, InnerFS->getInc(), InnerVar))
    return false;
  SourceLocation DummyLoc;
  if (!LoopVarUntouchedOutsideIncr(InnerFS, InnerVar, DummyLoc))
    return false;
  return true;
}

// A nested for-loop that indexes one dimension must itself have the
// qualifying shape: init j = 0, cond j < bound, increment by one, and j
// untouched in its body (3.2.1 / 4.4).

// Fast function-level bail-out: the classifier is invoked for every function
// that uses owned/borrow, but most of those do not contain any owned-element
// array (e.g. a function with only a `_Borrow` parameter). Probe the body for
// any expression whose type is an owned-element array (`arr`, `w.arr`,
// `o.w[i].arr`, ...); when none exists the classifier has nothing to do and
// returns immediately (the dataflow then applies no array rules at all).
// The first of HostIdxs that is the same variable as FieldIdx (i.e. the index
// variable is reused across subscript levels: the diagonal s[i].arr[i] releases
// only s[0].arr[0], s[1].arr[1], ...), or nullptr. Shared by scanBody
// (coverage) and collectSites (site recording).
static const Expr *FindReusedHostIndex(const Expr *FieldIdx,
                                       llvm::ArrayRef<const Expr *> HostIdxs) {
  const DeclRefExpr *FD = dyn_cast<DeclRefExpr>(FieldIdx);
  if (!FD)
    return nullptr;
  for (const Expr *HIdx : HostIdxs) {
    const DeclRefExpr *HD = dyn_cast<DeclRefExpr>(HIdx);
    if (HD && HD->getDecl() == FD->getDecl())
      return HIdx;
  }
  return nullptr;
}

// The second occurrence of a variable that indexes more than one level of the
// same subscript chain (the diagonal a[i][i]), or nullptr.
static const Expr *FindReusedIndex(llvm::ArrayRef<const Expr *> Idxs) {
  for (size_t I = 0; I < Idxs.size(); ++I) {
    const DeclRefExpr *A = dyn_cast<DeclRefExpr>(Idxs[I]);
    if (!A)
      continue;
    for (size_t J = I + 1; J < Idxs.size(); ++J) {
      const DeclRefExpr *B = dyn_cast<DeclRefExpr>(Idxs[J]);
      if (B && A->getDecl() == B->getDecl())
        return Idxs[J];
    }
  }
  return nullptr;
}

// Why a field-array element chain is not usable as a full-range access.
enum class FieldChainIssue {
  None,                  // every enclosing level is iterated in full
  IndexReused,           // an enclosing level reuses the tracked level's index
  IndexNotLoop,          // an enclosing index is constant or not a loop variable
  EnclosingBoundMismatch,// the enclosing loop's bound != that level's length
  EnclosingBoundNotConstant, // the level is fixed length but the bound is not
                             // an integer constant expression
  EnclosingShape,        // the enclosing loop is not a full-range for-loop
};

// The reason to report for a rejected field-array element chain.
static NonQualifyingLoopReason ReasonForIssue(FieldChainIssue Issue) {
  switch (Issue) {
  case FieldChainIssue::IndexReused:
    return NonQualifyingLoopReason::IndexReused;
  case FieldChainIssue::EnclosingBoundMismatch:
    return NonQualifyingLoopReason::BoundMismatch;
  case FieldChainIssue::EnclosingBoundNotConstant:
    return NonQualifyingLoopReason::BoundNotConstant;
  case FieldChainIssue::EnclosingShape:
    return NonQualifyingLoopReason::NotQualifying;
  case FieldChainIssue::IndexNotLoop:
  case FieldChainIssue::None:
    return NonQualifyingLoopReason::ElemAccessMismatch;
  }
  llvm_unreachable("unknown field chain issue");
}

// Whether the loop \p F driving one enclosing subscript level iterates that
// level of type \p LevelTy in full (`for (T k = 0; k < N; ++k)` with N the
// constant level length). Reports the specific failure: a bound that does not
// equal the level length versus a bound that cannot equal it at all because the
// level is fixed length and the bound is not a constant expression, versus a
// loop that is not a full-range for-loop (non-zero init, non-unit step, counter
// modified, ...).
static FieldChainIssue CheckEnclosingLoop(const ASTContext &Context,
                                          const ForStmt *F, QualType LevelTy) {
  const Expr *Cond = F->getCond();
  const BinaryOperator *CBO =
      Cond ? dyn_cast<BinaryOperator>(Cond->IgnoreParens()) : nullptr;
  const auto *CAT = Context.getAsConstantArrayType(LevelTy);
  if (!CBO || CBO->getOpcode() != BO_LT || !CAT)
    return FieldChainIssue::EnclosingShape;
  if (!CBO->getRHS()->isIntegerConstantExpr(Context))
    return FieldChainIssue::EnclosingBoundNotConstant;
  if (CBO->getRHS()->EvaluateKnownConstInt(Context).getZExtValue() !=
      CAT->getSize().getZExtValue())
    return FieldChainIssue::EnclosingBoundMismatch;
  if (!InnerLoopShapeQualifies(Context, F))
    return FieldChainIssue::EnclosingShape;
  return FieldChainIssue::None;
}

// Validate the *enclosing* subscript levels of a field-array element chain
// whose tracked level is indexed by the current loop's variable \p LevelIdx.
// Every enclosing level is a (index expression, array type) pair and must be
// iterated in full by its own loop: a reused index (the diagonal s[i].arr[i])
// releases only the diagonal, while a constant / non-loop index (s[0].arr[j],
// s[k].arr[j]) or a loop that does not span the level
// (for (k = 0; k < 1; ++k) over S[2]) releases only part of the field array.
// OffendingIdx receives the index that failed, for the note location.
static FieldChainIssue ValidateEnclosingIdxs(
    const ASTContext &Context, const Expr *LevelIdx,
    llvm::ArrayRef<std::pair<const Expr *, QualType>> EnclosingLevels,
    const VarDecl *LoopVar,
    const llvm::DenseMap<const VarDecl *, const ForStmt *> &LoopOfCounter,
    const Expr *&OffendingIdx) {
  OffendingIdx = nullptr;
  llvm::SmallVector<const Expr *, 4> EnclosingIdxs;
  for (const auto &L : EnclosingLevels)
    EnclosingIdxs.push_back(L.first);
  if (const Expr *Reused = FindReusedHostIndex(LevelIdx, EnclosingIdxs)) {
    OffendingIdx = Reused;
    return FieldChainIssue::IndexReused;
  }
  for (const auto &L : EnclosingLevels) {
    const DeclRefExpr *IdxDRE = dyn_cast<DeclRefExpr>(L.first);
    const VarDecl *VD =
        IdxDRE ? dyn_cast<VarDecl>(IdxDRE->getDecl()) : nullptr;
    const ForStmt *F = VD ? LoopOfCounter.lookup(VD) : nullptr;
    if (!F) {
      OffendingIdx = L.first;
      return FieldChainIssue::IndexNotLoop;
    }
    FieldChainIssue Issue = CheckEnclosingLoop(Context, F, L.second);
    if (Issue != FieldChainIssue::None) {
      OffendingIdx = L.first;
      return Issue;
    }
  }
  return FieldChainIssue::None;
}

// The innermost subscript of a field-array element chain (e.g. the `arr[i]` in
// s[i].arr[i].p) when the enclosing levels make it a full-range transfer for
// the current loop, or nullptr otherwise. The innermost subscript indexes the
// tracked field array, so the current loop must drive it; every enclosing
// subscript (host levels such as s[i], outer field levels such as w[i] in
// w2.w[i].arr[j]) must be driven in full by its own loop (ValidateEnclosingIdxs).
// Otherwise the transfer would move the whole aggregate and hide the leak of
// the unreleased elements.
static const Expr *FullRangeFieldChainInnermost(
    const ASTContext &Context, const Expr *Site, const VarDecl *LoopVar,
    const llvm::DenseMap<const VarDecl *, const ForStmt *> &LoopOfCounter) {
  const Expr *Innermost = nullptr;
  llvm::SmallVector<std::pair<const Expr *, QualType>, 4> OuterLevels;
  for (const Expr *Cur = Site ? Site->IgnoreParenImpCasts() : nullptr; Cur;) {
    if (const ArraySubscriptExpr *A = dyn_cast<ArraySubscriptExpr>(Cur)) {
      const Expr *Idx = A->getIdx()->IgnoreParenImpCasts();
      if (!Innermost)
        Innermost = Idx;
      else
        OuterLevels.push_back(
            {Idx, A->getBase()->IgnoreParenImpCasts()->getType()});
      Cur = A->getBase()->IgnoreParenImpCasts();
    } else if (const MemberExpr *M = dyn_cast<MemberExpr>(Cur)) {
      Cur = M->getBase()->IgnoreParenImpCasts();
    } else {
      break;
    }
  }
  if (!Innermost || !IsLoopIndex(Innermost, LoopVar))
    return nullptr;
  const Expr *OffendingIdx = nullptr;
  if (ValidateEnclosingIdxs(Context, Innermost, OuterLevels, LoopVar,
                            LoopOfCounter, OffendingIdx) !=
      FieldChainIssue::None)
    return nullptr;
  return Innermost;
}

// Peel `w.arr[i]` / `o.w[i].arr[j]` / `w.arr[i].a` to the host variable plus
// the array-field levels [(MemberExpr, index)] outermost-first. Only *field*
// array levels are collected (host-array indexes such as s[i] in s[i].a[j]
// are not levels). Shared by scanBody (coverage, needs the field length) and
// collectSites (site recording, needs the level index).
static bool PeelFieldIndexLevels(
    const Expr *E, const VarDecl *&HostVD,
    SmallVectorImpl<std::pair<const MemberExpr *, const Expr *>> &Levels) {
  Levels.clear();
  llvm::SmallVector<std::pair<const MemberExpr *, const Expr *>, 4> Rev;
  const Expr *Cur = E ? E->IgnoreParenImpCasts() : nullptr;
  const MemberExpr *LastME = nullptr;
  while (Cur) {
    if (const ArraySubscriptExpr *A = dyn_cast<ArraySubscriptExpr>(Cur)) {
      const Expr *Base = A->getBase()->IgnoreParenImpCasts();
      // Every subscript level is recorded, associated with the enclosing
      // member access (nullptr for a host-array level such as s[i] in
      // s[i].p). This lets collectSites recognise s[i].p / w.arr[i] /
      // o.w[i].arr[j] alike; scanBody filters the host levels out.
      Rev.push_back({LastME, A->getIdx()->IgnoreParenImpCasts()});
      if (const MemberExpr *M = dyn_cast<MemberExpr>(Base))
        LastME = M;
      Cur = Base;
    } else if (const MemberExpr *M = dyn_cast<MemberExpr>(Cur)) {
      LastME = M;
      Cur = M->getBase()->IgnoreParenImpCasts();
    } else {
      break;
    }
  }
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Cur))
    HostVD = dyn_cast<VarDecl>(DRE->getDecl());
  else
    return false;
  if (!HostVD)
    return false;
  for (auto It = Rev.rbegin(); It != Rev.rend(); ++It)
    Levels.push_back(*It);
  return true;
}

// Collect every VLA bound variable used in any for-loop condition (3.2.1
// cond 2): they must not be modified / address-taken / mutably borrowed.
static void CollectBoundVarInStmt(
    const ASTContext &Context, const Stmt *S,
    llvm::SmallPtrSetImpl<const VarDecl *> &Out) {
  if (!S)
    return;
  if (const ForStmt *FS = dyn_cast<ForStmt>(S)) {
    if (const Expr *Cond = FS->getCond()) {
      if (const BinaryOperator *BO =
              dyn_cast<BinaryOperator>(Cond->IgnoreParens())) {
        if (BO->getOpcode() == BO_LT) {
          if (const DeclRefExpr *R = dyn_cast<DeclRefExpr>(
                  BO->getRHS()->IgnoreParenImpCasts())) {
            if (const VarDecl *V = dyn_cast<VarDecl>(R->getDecl())) {
              if (V->getType()->isIntegerType())
                Out.insert(V);
            }
          } else if (const VarDecl *BoundVar =
                         GetSizeofArrayLengthVar(Context, BO->getRHS())) {
            // `sizeof(arr)/sizeof(arr[0])`: for a VLA the length variable is
            // still a bound variable and must not be modified.
            Out.insert(BoundVar);
          }
        }
      }
    }
  }
  for (const Stmt *Child : S->children())
    CollectBoundVarInStmt(Context, Child, Out);
}

static void CollectLoopBoundVars(ASTContext &Context, const FunctionDecl *FD,
                                 llvm::SmallPtrSetImpl<const VarDecl *> &Out) {
  CollectBoundVarInStmt(Context, FD->getBody(), Out);
}

// Collect the counter variable of every for-loop in Stmt (the variable declared
// by the loop init) and the loop it belongs to. Used to require that the
// enclosing subscript levels of a field-array element chain are driven by loops
// that iterate those levels in full.
static void CollectLoopCounterVars(
    const Stmt *S, llvm::DenseMap<const VarDecl *, const ForStmt *> &Out) {
  if (!S)
    return;
  if (const ForStmt *FS = dyn_cast<ForStmt>(S)) {
    if (const DeclStmt *DS = dyn_cast_or_null<DeclStmt>(FS->getInit()))
      if (DS->isSingleDecl())
        if (const VarDecl *VD = dyn_cast<VarDecl>(DS->getSingleDecl()))
          Out[VD] = FS;
  }
  for (const Stmt *Child : S->children())
    CollectLoopCounterVars(Child, Out);
}

// First statement in FD that modifies / address-takes / mutably borrows
// BoundVD, or an invalid location when BoundVD is never touched.
// Record, for every VLA bound variable, the first statement in FD that
// modifies / address-takes / mutably borrows it (invalid location when the
// variable is never touched). A single AST walk covers all bound variables
// instead of one full walk per variable.
static void CheckBoundVarModification(
    const Stmt *S,
    const llvm::SmallPtrSetImpl<const VarDecl *> &AllBoundVars,
    llvm::DenseMap<const VarDecl *, SourceLocation> &ModLocs) {
  if (!S || ModLocs.size() == AllBoundVars.size())
    return;
  if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(S)) {
    if (BO->isAssignmentOp()) {
      if (const DeclRefExpr *LHS = dyn_cast<DeclRefExpr>(
              BO->getLHS()->IgnoreParenImpCasts())) {
        if (const VarDecl *VD = dyn_cast<VarDecl>(LHS->getDecl())) {
          if (AllBoundVars.count(VD))
            ModLocs.try_emplace(VD, S->getSourceRange().getBegin());
        }
      }
    }
  }
  if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(S)) {
    if (UO->isIncrementDecrementOp() || UO->getOpcode() == UO_AddrOf ||
        UO->getOpcode() == UO_AddrMut ||
        UO->getOpcode() == UO_AddrConst) {
      if (const DeclRefExpr *Sub = dyn_cast<DeclRefExpr>(
              UO->getSubExpr()->IgnoreParenImpCasts())) {
        if (const VarDecl *VD = dyn_cast<VarDecl>(Sub->getDecl())) {
          if (AllBoundVars.count(VD))
            ModLocs.try_emplace(VD, S->getSourceRange().getBegin());
        }
      }
    }
  }
  for (const Stmt *Child : S->children())
    CheckBoundVarModification(Child, AllBoundVars, ModLocs);
}

static void FindBoundVarModifications(
    ASTContext &Context, const FunctionDecl *FD,
    const llvm::SmallPtrSetImpl<const VarDecl *> &AllBoundVars,
    llvm::DenseMap<const VarDecl *, SourceLocation> &ModLocs) {
  CheckBoundVarModification(FD->getBody(), AllBoundVars, ModLocs);
}

static bool StmtUsesOwnedElementArray(const Stmt *S) {
  if (!S)
    return false;
  if (const Expr *E = dyn_cast<Expr>(S))
    if (IsOwnedElementArrayType(E->getType()))
      return true;
  for (const Stmt *Child : S->children())
    if (StmtUsesOwnedElementArray(Child))
      return true;
  return false;
}

// Memoized decomposition of an array-subscript chain: a[i][j] ->
// (Idxs=[i,j], Base=a). The cache is shared across every loop of the
// function (the coverage scan and the site collection reuse the same peeling
// result for a given AST node).
static void PeelSubscriptChain(
    const ArraySubscriptExpr *ASE, SmallVectorImpl<const Expr *> &Idxs,
    const Expr *&Base,
    llvm::DenseMap<const ArraySubscriptExpr *, SmallVector<const Expr *, 4>>
        &IdxsCache,
    llvm::DenseMap<const ArraySubscriptExpr *, const Expr *> &BaseCache) {
  auto It = IdxsCache.find(ASE);
  if (It != IdxsCache.end()) {
    Idxs.assign(It->second.begin(), It->second.end());
    Base = BaseCache[ASE];
    return;
  }
  Base = ASE;
  Idxs.clear();
  while (const ArraySubscriptExpr *A = dyn_cast<ArraySubscriptExpr>(Base)) {
    Idxs.push_back(A->getIdx()->IgnoreParenImpCasts());
    Base = A->getBase()->IgnoreParenImpCasts();
  }
  std::reverse(Idxs.begin(), Idxs.end());
  IdxsCache[ASE].assign(Idxs.begin(), Idxs.end());
  BaseCache[ASE] = Base;
}

// Whether S (or a descendant) contains an array-subscript expression.
static bool HasSubscriptInBody(const Stmt *S) {
  if (!S)
    return false;
  if (isa<ArraySubscriptExpr>(S))
    return true;
  for (const Stmt *Child : S->children())
    if (HasSubscriptInBody(Child))
      return true;
  return false;
}

static bool BodyUsesOwnedElementArray(const FunctionDecl *FD) {
  if (!FD->getBody())
    return false;
  for (const ParmVarDecl *PVD : FD->parameters())
    if (IsOwnedElementArrayType(PVD->getType()))
      return true;
  return StmtUsesOwnedElementArray(FD->getBody());
}

// Stateful classifier for one function: turns the qualifying-loop
// rules into per-loop coverage / allow-sites. The per-loop state
// lives in the class so the recursive body / site walkers need no
// capture lists.
class OwnedArrayLoopClassifier {
  ASTContext &Context;
  const FunctionDecl *FD;
  OwnedArrayLoopInfo &LoopInfo;

  // Function-wide state.
  llvm::DenseMap<const ArraySubscriptExpr *, SmallVector<const Expr *, 4>>
      SubscriptIdxsCache;
  llvm::DenseMap<const ArraySubscriptExpr *, const Expr *> SubscriptBaseCache;
  llvm::DenseMap<const VarDecl *, SourceLocation> BoundVarModLocs;
  // Counter variable of every for-loop in the function -> that loop: an
  // enclosing subscript level of a field-array element chain must be driven by
  // one of these loops, and the loop must iterate that level in full.
  llvm::DenseMap<const VarDecl *, const ForStmt *> LoopOfCounter;

  // Per-loop state (reset by classifyForLoop).
  const ForStmt *FS = nullptr;
  const VarDecl *LoopVar = nullptr;
  const VarDecl *BoundVD = nullptr;
  llvm::APSInt BoundVal;
  bool IsConstantBound = false;
  const Stmt *Body = nullptr;
  bool HasEarlyExit = false;
  SourceLocation EarlyExitLoc;
  llvm::DenseMap<const VarDecl *, bool> ArrayOk;
  // Per-array reason for an accessed-but-not-covered array (e.g. a reused loop
  // index); falls back to the generic index-misuse reason when unset.
  llvm::DenseMap<const VarDecl *, LoopFailure> ArrayFailures;
  llvm::SmallPtrSet<const VarDecl *, 4> MultiDimDone;
  llvm::SmallPtrSet<const VarDecl *, 4> FieldArrayDone;
  SmallVector<const VarDecl *, 4> Covered;

  bool reject(NonQualifyingLoopReason R,
              SourceLocation Loc = SourceLocation()) {
    LoopInfo.NonQualifyingLoops[FS] = {R, Loc};
    return false;
  }

  void setCovered(const VarDecl *VD, bool GoodIdx) {
    if (!ArrayOk.count(VD))
      ArrayOk[VD] = true;
    if (!GoodIdx)
      ArrayOk[VD] = false;
  }

  // Record that VD is accessed but not fully covered, with the reason and the
  // location to report in the note (e.g. a reused loop index).
  void setNotCovered(const VarDecl *VD, NonQualifyingLoopReason R,
                     SourceLocation Loc = SourceLocation()) {
    setCovered(VD, /*GoodIdx=*/false);
    ArrayFailures[VD] = {R, Loc};
  }

  // Parse and validate the for-loop head (`init; cond`) for the
  // qualifying shape (3.2.1 conds 1-2): zero init, `i < N` / `i < n`
  // bound that fits the loop-variable type, and an untouched bound
  // variable. Records the failure reason and returns false when the
  // head does not qualify.
  bool parseQualifyingLoopHead() {
    // init syntax: T i = ...  or  i = ...  The value and the type-vs-bound
    // check are deferred until after cond, which provides the bound (needed
    // to distinguish "type too small" from "initial value is not zero").
    const Expr *InitVal = nullptr;
    const Stmt *InitStmt = FS->getInit();
    if (!InitStmt)
      return reject(NonQualifyingLoopReason::InitNotZero);
    if (const DeclStmt *DS = dyn_cast<DeclStmt>(InitStmt)) {
      if (!DS->isSingleDecl())
        return reject(NonQualifyingLoopReason::InitNotZero);
      LoopVar = dyn_cast<VarDecl>(DS->getSingleDecl());
      if (!LoopVar || !LoopVar->getInit())
        return reject(NonQualifyingLoopReason::InitNotZero);
      InitVal = LoopVar->getInit();
    } else if (const BinaryOperator *BO =
                   dyn_cast<BinaryOperator>(InitStmt)) {
      if (BO->getOpcode() != BO_Assign)
        return reject(NonQualifyingLoopReason::InitNotZero);
      if (const DeclRefExpr *LHS = dyn_cast<DeclRefExpr>(
              BO->getLHS()->IgnoreParenImpCasts())) {
        LoopVar = dyn_cast<VarDecl>(LHS->getDecl());
        if (!LoopVar)
          return reject(NonQualifyingLoopReason::InitNotZero);
        InitVal = BO->getRHS();
      } else {
        return reject(NonQualifyingLoopReason::InitNotZero);
      }
    } else {
      return reject(NonQualifyingLoopReason::InitNotZero);
    }
    if (!LoopVar->getType()->isIntegerType())
      return reject(NonQualifyingLoopReason::InitNotZero);

    const Expr *Cond = FS->getCond();
    if (!Cond)
      return reject(NonQualifyingLoopReason::CondNotMatch);
    const BinaryOperator *CondBO =
        dyn_cast<BinaryOperator>(Cond->IgnoreParens());
    if (!CondBO || CondBO->getOpcode() != BO_LT)
      return reject(NonQualifyingLoopReason::CondNotMatch);
    const DeclRefExpr *CondLHS = dyn_cast<DeclRefExpr>(
        CondBO->getLHS()->IgnoreParenImpCasts());
    if (!CondLHS || CondLHS->getDecl() != LoopVar)
      return reject(NonQualifyingLoopReason::CondNotMatch);

    if (CondBO->getRHS()->isIntegerConstantExpr(Context)) {
      BoundVal = CondBO->getRHS()->EvaluateKnownConstInt(Context);
      IsConstantBound = true;
    } else {
      const DeclRefExpr *BoundDRE = dyn_cast<DeclRefExpr>(
          CondBO->getRHS()->IgnoreParenImpCasts());
      if (BoundDRE) {
        BoundVD = dyn_cast<VarDecl>(BoundDRE->getDecl());
        if (!BoundVD || !BoundVD->getType()->isIntegerType())
          return reject(NonQualifyingLoopReason::CondNotMatch);
      } else {
        // `i < sizeof(arr)/sizeof(arr[0])`: for a VLA this is a runtime
        // value, so it is not a constant expression. Normalize it to the
        // array's VLA length variable and treat it like `i < n`.
        BoundVD = GetSizeofArrayLengthVar(Context, CondBO->getRHS());
        if (!BoundVD)
          return reject(NonQualifyingLoopReason::CondNotMatch);
      }
      if (BoundVD) {
        auto ModIt = BoundVarModLocs.find(BoundVD);
        if (ModIt != BoundVarModLocs.end())
          return reject(NonQualifyingLoopReason::VlaBoundVarModified,
                        ModIt->second);
      }
    }

    // init value: must be zero (3.2.1 cond 1)...
    if (!InitVal->isIntegerConstantExpr(Context) ||
        InitVal->EvaluateKnownConstInt(Context) != 0)
      return reject(NonQualifyingLoopReason::InitNotZero);
    // ...and the loop variable's type must be able to hold the loop bound;
    // otherwise the loop would wrap / never terminate (3.2.1 cond 2).
    // For a constant bound, compare the bound's value against the loop
    // variable's positive range.  For a variable bound, the loop variable
    // must be able to represent every value the bound variable can take,
    // approximated by the maximum positive value of its type.
    if (IsConstantBound) {
      unsigned Bits = Context.getTypeSize(LoopVar->getType());
      unsigned MaxBits =
          Bits - (LoopVar->getType()->isSignedIntegerType() ? 1 : 0);
      if (BoundVal.getActiveBits() > MaxBits)
        return reject(NonQualifyingLoopReason::InitTypeTooSmall);
    } else if (BoundVD) {
      unsigned LoopBits = Context.getTypeSize(LoopVar->getType());
      unsigned LoopMaxBits =
          LoopBits - (LoopVar->getType()->isSignedIntegerType() ? 1 : 0);
      unsigned BoundBits = Context.getTypeSize(BoundVD->getType());
      unsigned BoundMaxBits =
          BoundBits - (BoundVD->getType()->isSignedIntegerType() ? 1 : 0);
      if (BoundMaxBits > LoopMaxBits)
        return reject(NonQualifyingLoopReason::InitTypeTooSmall);
    }
    return true;
  }

  // Validate a multi-dimensional subscript chain a[i][j] / s[i].a[j]
  // against the enclosing loop nest: every dimension must be indexed
  // by the loop variable of the corresponding loop, and each
  // dimension's length must match the loop bound. Returns true when
  // the whole nest matches.
  bool matchNestedDimensions(
      const ForStmt *OuterFS, QualType ArrTy, const VarDecl *ArrVD,
      const llvm::SmallVectorImpl<const Expr *> &Idxs) {
    // Locate the index this loop's variable drives. In a nested nest
    // (a[i][j]) the inner loop drives j at position 1; the leading indices
    // belong to enclosing loops and are skipped.
    unsigned StartD = Idxs.size();
    for (unsigned D = 0; D < Idxs.size(); ++D) {
      if (const DeclRefExpr *IdxDRE = dyn_cast<DeclRefExpr>(Idxs[D]))
        if (IsLoopIndex(IdxDRE, LoopVar)) {
          StartD = D;
          break;
        }
    }
    if (StartD == Idxs.size())
      return false;
    QualType DimTy = ArrTy;
    for (unsigned i = 0; i < StartD; ++i) {
      const auto *AT = DimTy->getAsArrayTypeUnsafe();
      if (!AT)
        return false;
      DimTy = AT->getElementType();
    }
    const ForStmt *CurFS = OuterFS;
    for (unsigned D = StartD; D < Idxs.size(); ++D) {
      const DeclRefExpr *Wanted = dyn_cast<DeclRefExpr>(Idxs[D]);
      if (!Wanted)
        return false;
      const VarDecl *CurVar = nullptr;
      if (D == StartD) {
        CurVar = LoopVar;
      } else {
        if (!InnerLoopShapeQualifies(Context, CurFS))
          return false;
        const Stmt *Init = CurFS->getInit();
        if (const DeclStmt *DS = dyn_cast<DeclStmt>(Init))
          if (DS->isSingleDecl())
            CurVar = dyn_cast<VarDecl>(DS->getSingleDecl());
        if (!CurVar)
          return false;
      }
      if (Wanted->getDecl() != CurVar)
        return false;
      const Expr *Cond = CurFS->getCond();
      const BinaryOperator *CBO =
          Cond ? dyn_cast<BinaryOperator>(Cond->IgnoreParens()) : nullptr;
      if (!CBO || CBO->getOpcode() != BO_LT)
        return false;
      if (StartD == 0 && D == 0) {
        if (!ArrayBoundMatches(Context, ArrVD, IsConstantBound, BoundVal,
                               BoundVD)) {
          // A constant bound that does not equal the length of this dimension is
          // a bound mismatch; a non-constant bound can only match a VLA length
          // variable, so on a fixed-length dimension it is reported as such.
          // Either way the note names the rule instead of falling back to the
          // generic index-misuse reason.
          if (IsConstantBound)
            setNotCovered(ArrVD, NonQualifyingLoopReason::BoundMismatch,
                          CurFS->getBeginLoc());
          else if (Context.getAsConstantArrayType(DimTy))
            setNotCovered(ArrVD, NonQualifyingLoopReason::BoundNotConstant,
                          CurFS->getBeginLoc());
          return false;
        }
      } else {
        if (const auto *CAT = Context.getAsConstantArrayType(DimTy)) {
          if (!CBO->getRHS()->isIntegerConstantExpr(Context)) {
            // This level has a fixed length, so only an integer constant
            // expression can iterate it in full.
            setNotCovered(ArrVD, NonQualifyingLoopReason::BoundNotConstant,
                          CurFS->getBeginLoc());
            return false;
          }
          if (CBO->getRHS()->EvaluateKnownConstInt(Context).getZExtValue() !=
              CAT->getSize().getZExtValue()) {
            // An inner loop whose constant bound does not equal this level's
            // length (e.g. `for (j = 0; j < 1; ++j)` over `a[2][2]`) releases
            // only part of the array, exactly like a short bound on the
            // outermost dimension.
            setNotCovered(ArrVD, NonQualifyingLoopReason::BoundMismatch,
                          CurFS->getBeginLoc());
            return false;
          }
        } else {
          return false;
        }
      }
      if (const auto *AT = DimTy->getAsArrayTypeUnsafe())
        DimTy = AT->getElementType();
      else
        return false;
      if (D + 1 < Idxs.size()) {
        // The nested loop for the next dimension is the one whose loop
        // variable matches the next index (Idxs[D+1]) -- not simply the
        // first ForStmt in the body: an outer body may contain several
        // inner loops (e.g. a k-loop before the j-loop that drives a[i][j]),
        // and picking the first one would fail the multi-dim match.
        const ForStmt *Next = nullptr;
        const DeclRefExpr *WantedDRE =
            dyn_cast<DeclRefExpr>(Idxs[D + 1]->IgnoreParenImpCasts());
        const VarDecl *WantedVar =
            WantedDRE ? dyn_cast<VarDecl>(WantedDRE->getDecl()) : nullptr;
        std::function<void(const Stmt *)> findInner;
        findInner = [&](const Stmt *X) {
          if (!X || Next)
            return;
          if (const ForStmt *F = dyn_cast<ForStmt>(X)) {
            const Stmt *Init = F->getInit();
            const DeclStmt *DS = dyn_cast<DeclStmt>(Init);
            const VarDecl *FVar = DS && DS->isSingleDecl()
                                      ? dyn_cast<VarDecl>(DS->getSingleDecl())
                                      : nullptr;
            if (FVar && WantedVar && FVar == WantedVar &&
                InnerLoopShapeQualifies(Context, F)) {
              Next = F;
              return;
            }
            // Not the loop driving this dimension -- keep looking.
          }
          for (const Stmt *C : X->children())
            findInner(C);
        };
        findInner(CurFS->getBody());
        if (!Next)
          return false;
        CurFS = Next;
      }
    }
    return true;
  }

  // Assemble the per-loop covered-array list from the coverage scan
  // results: an array is covered when its bound matches the loop
  // bound (or matchNestedDimensions / the field branch already
  // validated every dimension). Non-covered arrays are recorded
  // per-array for the diagnostic note.
  void collectCoveredArrays() {
    // Coverage is per-array: a loop may qualify for one array (bound matches)
    // while another array inside the same body is not covered (bound mismatch
    // or index misuse). Non-covered arrays are recorded per-loop for the
    // diagnostic note; they do not make the whole loop non-qualifying.
    for (auto &KV : ArrayOk) {
      if (KV.second) {
        // Multi-dim arrays: matchNested already validated every dimension
        // (current-loop bound + inner-loop bounds), so no bound match here.
        if (MultiDimDone.count(KV.first) || FieldArrayDone.count(KV.first)) {
          Covered.push_back(KV.first);
          continue;
        }
        if (ArrayBoundMatches(Context, KV.first, IsConstantBound, BoundVal,
                              BoundVD)) {
          Covered.push_back(KV.first);
          continue;
        }
        // Bound mismatch. The allowed forms differ by array kind: a VLA is
        // iterated with its length variable (report a different variable than
        // the one declaring the length), while a fixed-length array is only
        // matched by an integer constant expression equal to its length, so a
        // non-constant bound cannot equal it (report that rule rather than the
        // misleading "bound must equal the array length").
        NonQualifyingLoopReason R = NonQualifyingLoopReason::BoundMismatch;
        if (!IsConstantBound) {
          if (const auto *VAT = dyn_cast_or_null<VariableArrayType>(
                  KV.first->getType()->getAsArrayTypeUnsafe())) {
            if (const DeclRefExpr *SizeDRE = dyn_cast<DeclRefExpr>(
                    VAT->getSizeExpr()->IgnoreParenImpCasts())) {
              if (SizeDRE->getDecl() != BoundVD)
                R = NonQualifyingLoopReason::VlaBoundVarMismatch;
            }
          } else {
            R = NonQualifyingLoopReason::BoundNotConstant;
          }
        }
        LoopInfo.NonCoveredArrays[FS][KV.first] = {R, SourceLocation()};
      } else {
        // A specific reason (e.g. a reused loop index) when the coverage scan
        // recorded one, otherwise the generic index-misuse reason.
        auto FIt = ArrayFailures.find(KV.first);
        LoopInfo.NonCoveredArrays[FS][KV.first] =
            FIt != ArrayFailures.end()
                ? FIt->second
                : LoopFailure{NonQualifyingLoopReason::ElemAccessMismatch,
                              SourceLocation()};
      }
    }
    LoopInfo.LoopCoveredArrays[FS].assign(Covered.begin(), Covered.end());
  }

  // The for-loop inside Outer's body whose declared counter is the variable
  // used as the subscript Idx, or nullptr (the index is constant / not a loop
  // counter / its loop is not nested in Outer).
  const ForStmt *findNestedLoopForIndex(const ForStmt *Outer, const Expr *Idx) {
    const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Idx);
    const VarDecl *Wanted = DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
    if (!Wanted || !Outer->getBody())
      return nullptr;
    const ForStmt *Found = nullptr;
    std::function<void(const Stmt *)> Find = [&](const Stmt *S) {
      if (!S || Found)
        return;
      if (const ForStmt *F = dyn_cast<ForStmt>(S)) {
        const DeclStmt *DS = dyn_cast_or_null<DeclStmt>(F->getInit());
        const VarDecl *FV = DS && DS->isSingleDecl()
                                ? dyn_cast<VarDecl>(DS->getSingleDecl())
                                : nullptr;
        if (FV == Wanted) {
          Found = F;
          return;
        }
      }
      for (const Stmt *Child : S->children())
        Find(Child);
    };
    Find(Outer->getBody());
    return Found;
  }

  // Validate the level chain of an owned-element array access (outermost level
  // first) for the loop currently being classified: this loop must drive one
  // level, every level outside it must be iterated in full by an enclosing loop
  // and every level inside it by a nested full-range for-loop, in the declared
  // dimension order (manual 3.3.3.1 cond 5). The covered aggregate is HostVD; a
  // failing level records the specific per-array reason so the note names the
  // rule that failed. Returns true when the whole chain is covered.
  bool validateLevelChain(
      llvm::ArrayRef<std::pair<const Expr *, QualType>> Chain,
      const VarDecl *HostVD) {
    unsigned StartD = Chain.size();
    for (unsigned D = 0; D < Chain.size(); ++D) {
      const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Chain[D].first);
      if (DRE && IsLoopIndex(DRE, LoopVar)) {
        StartD = D;
        break;
      }
    }
    if (StartD == Chain.size())
      return false; // this loop does not drive the chain

    // Levels outside this loop's level: host levels such as the k of
    // s[k].arr[j] and the field's own outer levels such as the i of
    // v.arr[i][j] when j drives the transfer. Each must be driven by its own
    // enclosing loop, iterating it in full.
    llvm::SmallVector<std::pair<const Expr *, QualType>, 4> Enclosing(
        Chain.begin(), Chain.begin() + StartD);
    const Expr *OffendingIdx = nullptr;
    FieldChainIssue Issue = ValidateEnclosingIdxs(
        Context, Chain[StartD].first, Enclosing, LoopVar, LoopOfCounter,
        OffendingIdx);
    if (Issue != FieldChainIssue::None) {
      setNotCovered(HostVD, ReasonForIssue(Issue),
                    Issue == FieldChainIssue::IndexReused
                        ? OffendingIdx->getExprLoc()
                        : SourceLocation());
      return false;
    }

    // A loop index reused by several levels of this chain (the diagonal
    // v.arr[i][i]) releases only the diagonal, never the whole aggregate. Reuse
    // between an enclosing level and this one is reported by
    // ValidateEnclosingIdxs above.
    llvm::SmallVector<const Expr *, 4> SubIdxs;
    for (unsigned D = StartD; D < Chain.size(); ++D)
      SubIdxs.push_back(Chain[D].first);
    if (const Expr *Reused = FindReusedIndex(SubIdxs)) {
      setNotCovered(HostVD, NonQualifyingLoopReason::IndexReused,
                    Reused->getExprLoc());
      return false;
    }

    // The level this loop drives and every level inside it: each must be a
    // full-range for-loop over its own length, and the loop of level D+1 must
    // be nested inside the loop of level D (a transposed nest such as
    // `for (j) for (i) v.arr[i][j]` releases only part of the aggregate).
    const ForStmt *CurFS = FS;
    for (unsigned D = StartD; D < Chain.size(); ++D) {
      // A level whose length is not a compile-time constant (a flexible array
      // member, or a variably modified level) can never be proven covered, but
      // a loop driving it can still be well formed: leave the reason to the
      // per-loop fallback instead of reporting a loop-shape failure.
      if (D == StartD && !Context.getAsConstantArrayType(Chain[D].second))
        return false;
      if (D > StartD) {
        const ForStmt *Next = findNestedLoopForIndex(CurFS, Chain[D].first);
        if (!Next) {
          setNotCovered(HostVD, NonQualifyingLoopReason::ElemAccessMismatch,
                        SourceLocation());
          return false;
        }
        CurFS = Next;
      }
      FieldChainIssue LevelIssue =
          CheckEnclosingLoop(Context, CurFS, Chain[D].second);
      if (LevelIssue != FieldChainIssue::None) {
        setNotCovered(HostVD, ReasonForIssue(LevelIssue),
                      CurFS->getBeginLoc());
        return false;
      }
    }
    return true;
  }

  void scanBody(const Stmt *S, bool DirectBody) {
    if (!S || HasEarlyExit)
      return;
    // --- early-exit detection ---
    if (isa<ReturnStmt>(S) || isa<IndirectGotoStmt>(S)) {
      HasEarlyExit = true;
      EarlyExitLoc = S->getSourceRange().getBegin();
      return;
    }
    if (const GotoStmt *GS = dyn_cast<GotoStmt>(S)) {
      // Manual rule 4: a goto whose target label is inside the loop body
      // is allowed (it does not leave the loop).
      const SourceManager &SM = Context.getSourceManager();
      SourceLocation TargetLoc = GS->getLabel()->getLocation();
      bool TargetInBody =
          SM.isBeforeInTranslationUnit(Body->getBeginLoc(), TargetLoc) &&
          SM.isBeforeInTranslationUnit(TargetLoc, Body->getEndLoc());
      if (!TargetInBody) {
        HasEarlyExit = true;
        EarlyExitLoc = S->getSourceRange().getBegin();
      }
      return;
    }
    if (isa<BreakStmt>(S)) {
      if (DirectBody) {
        HasEarlyExit = true;
        EarlyExitLoc = S->getSourceRange().getBegin();
      }
      return;
    }
    // --- coverage scan ---
    if (const ArraySubscriptExpr *ASE =
            dyn_cast<ArraySubscriptExpr>(S)) {
      SmallVector<const Expr *, 4> Idxs;
      const Expr *Base = nullptr;
      PeelSubscriptChain(ASE, Idxs, Base, SubscriptIdxsCache,
                        SubscriptBaseCache);
      if (const DeclRefExpr *BaseDRE = dyn_cast<DeclRefExpr>(Base)) {
        if (const VarDecl *ArrVD =
                dyn_cast<VarDecl>(BaseDRE->getDecl())) {
          if (IsOwnedElementArrayType(ArrVD->getType())) {
            bool GoodIdx = false;
            const Expr *ReusedIdx = nullptr;
            if (Idxs.size() == 1) {
              if (const DeclRefExpr *IdxDRE =
                      dyn_cast<DeclRefExpr>(Idxs.front()))
                GoodIdx = IsLoopIndex(IdxDRE, LoopVar);
            } else if (Idxs.size() > 1) {
              GoodIdx = matchNestedDimensions(FS, ArrVD->getType(), ArrVD,
                                        Idxs);
              if (GoodIdx)
                MultiDimDone.insert(ArrVD);
              else
                // The same loop index indexing several dimensions (a[i][i])
                // releases only the diagonal: distinguish it from the other
                // index-misuse failures.
                ReusedIdx = FindReusedIndex(Idxs);
            }
            if (ReusedIdx)
              setNotCovered(ArrVD, NonQualifyingLoopReason::IndexReused,
                            ReusedIdx->getExprLoc());
            else
              setCovered(ArrVD, GoodIdx);
          }
        }
      }
      // s[i].a[j]: the base is a MemberExpr whose base is the struct
      // array s. Recognize s (GoodIdx = s[i]'s index == LoopVar), same as
      // the standalone s[i].field case.
      if (const MemberExpr *ME = dyn_cast<MemberExpr>(Base)) {
        const Expr *MBase = ME->getBase()->IgnoreParenImpCasts();
        const Expr *MIdx = nullptr;
        while (const ArraySubscriptExpr *A =
                   dyn_cast<ArraySubscriptExpr>(MBase)) {
          MIdx = A->getIdx()->IgnoreParenImpCasts();
          MBase = A->getBase()->IgnoreParenImpCasts();
        }
        if (const DeclRefExpr *BaseDRE = dyn_cast<DeclRefExpr>(MBase)) {
          if (const VarDecl *ArrVD =
                  dyn_cast<VarDecl>(BaseDRE->getDecl())) {
            if (IsOwnedElementArrayType(ArrVD->getType())) {
              bool GoodIdx = false;
              if (const DeclRefExpr *IdxDRE =
                      dyn_cast<DeclRefExpr>(MIdx))
                GoodIdx = IsLoopIndex(IdxDRE, LoopVar);
              // The subscripted member may itself be an array field
              // (s[i].a[j]); its inner index must be a *variable* driven by
              // some (possibly nested) loop. A constant inner index
              // (s[i].a[0]) cannot be driven by any enclosing loop, so the
              // loop would only partially cover the field array -- a
              // partial element free must not silently mark the whole field
              // aggregate as moved (manual rule 5 allows only a[i] /
              // a[i][j] / a[i].f).
              bool FieldIdxOk = true;
              if (ME->getType()->isArrayType())
                FieldIdxOk = dyn_cast<DeclRefExpr>(Idxs.front()) != nullptr;
              if (GoodIdx && FieldIdxOk) {
                // The field-array index must not be the same loop variable as
                // the host index: s[i].a[i] releases only the diagonal.
                if (FindReusedHostIndex(Idxs.front(), {MIdx}))
                  setNotCovered(ArrVD, NonQualifyingLoopReason::IndexReused,
                                MIdx->getExprLoc());
                else
                  ArrayOk[ArrVD] = true;
              }
              // A host-level mismatch (s[i].a[j] where i != LoopVar) does
              // NOT reject the array: the field-level branch below may
              // still cover it with the field index.
            }
          }
        }
      }
      if (const MemberExpr *FieldME = dyn_cast<MemberExpr>(Base)) {
        // Struct-field owned-element array: w.arr[i] (and nested
        // o.w.arr[i]). The subscripted base is a member-array field of a
        // tracked host struct; it is covered like a local array when the
        // bound matches the field's length and the (outermost) index is the
        // loop variable.
        const Expr *Cur = FieldME;
        std::string FieldPath;
        {
          // Peel the member chain to the root host, skipping host-array
          // subscripts (arr[i].a[j]: i belongs to the host) and recording
          // field-array levels as `[]` (o.w[i].arr[j]: i is the w field
          // level, j is the arr field level).
          llvm::SmallVector<std::string, 4> chain;
          while (Cur) {
            if (const MemberExpr *M = dyn_cast<MemberExpr>(Cur)) {
              chain.push_back(M->getMemberNameInfo().getAsString());
              Cur = M->getBase()->IgnoreParenImpCasts();
            } else if (const ArraySubscriptExpr *A =
                           dyn_cast<ArraySubscriptExpr>(Cur)) {
              const Expr *B = A->getBase()->IgnoreParenImpCasts();
              bool IsFieldIndex = dyn_cast<MemberExpr>(B) != nullptr;
              if (const ArraySubscriptExpr *Nested =
                      dyn_cast<ArraySubscriptExpr>(B)) {
                const Expr *BB =
                    Nested->getBase()->IgnoreParenImpCasts();
                while (const ArraySubscriptExpr *BA =
                           dyn_cast<ArraySubscriptExpr>(BB))
                  BB = BA->getBase()->IgnoreParenImpCasts();
                IsFieldIndex = dyn_cast<MemberExpr>(BB) != nullptr;
              }
              if (IsFieldIndex)
                chain.push_back("[]");
              Cur = B;
            } else {
              break;
            }
          }
          for (auto It = chain.rbegin(); It != chain.rend(); ++It) {
            if (*It == "[]")
              FieldPath += "[]";
            else if (FieldPath.empty())
              FieldPath = *It;
            else
              FieldPath += "." + *It;
          }
        }
        if (const DeclRefExpr *HostDRE = dyn_cast<DeclRefExpr>(Cur)) {
          if (const VarDecl *HostVD =
                  dyn_cast<VarDecl>(HostDRE->getDecl())) {
            std::string FieldName = FieldPath;
            if (IsTrackedType(HostVD->getType()) &&
                IsOwnedArrayField(HostVD, FieldName)) {
              // Build the level chain of this access, outermost level first:
              // the host subscript levels (e.g. the k of s[k].arr[j]) followed
              // by the field's own array levels, each with that level's array
              // type (e.g. `int *_Owned[2][2]` then `int *_Owned[2]` for
              // v.arr[i][j]). validateLevelChain then checks that this loop
              // drives one level and that every level outside / inside it is
              // iterated in full by its own loop, in the declared dimension
              // order.
              llvm::SmallVector<std::pair<const Expr *, QualType>, 4> Chain;
              for (const Expr *HCur = FieldME->getBase()->IgnoreParenImpCasts();
                   const ArraySubscriptExpr *A =
                       dyn_cast<ArraySubscriptExpr>(HCur);) {
                Chain.push_back({A->getIdx()->IgnoreParenImpCasts(),
                                 A->getBase()
                                     ->IgnoreParenImpCasts()
                                     ->getType()});
                HCur = A->getBase()->IgnoreParenImpCasts();
              }
              std::reverse(Chain.begin(), Chain.end());
              // The field's own levels are the subscripts this expression peels
              // (`Idxs`; PeelSubscriptChain stops at the member access), one per
              // array level of the field type, outermost first. A partial access
              // (fewer indices than levels) is not an element transfer.
              QualType DimTy = FieldME->getType();
              unsigned FieldLevels = 0;
              while (const ArrayType *AT = Context.getAsArrayType(DimTy)) {
                ++FieldLevels;
                DimTy = AT->getElementType();
              }
              if (FieldLevels > 0 && Idxs.size() == FieldLevels) {
                DimTy = FieldME->getType();
                for (unsigned D = 0; D < FieldLevels; ++D) {
                  const ArrayType *AT = Context.getAsArrayType(DimTy);
                  Chain.push_back({Idxs[D], DimTy});
                  DimTy = AT->getElementType();
                }
                if (validateLevelChain(Chain, HostVD)) {
                  ArrayOk[HostVD] = true;
                  FieldArrayDone.insert(HostVD);
                }
              }
            }
          }
        }
      }
      // The whole subscript chain (a[i][j] / s[i].a[j]) was consumed by
      // PeelSubscriptChain; do not recurse into its sub-expressions
      // (the inner a[i] / s[i].a would be re-examined with a mismatched
      // index and wrongly invalidate the array).
      return;
    }
    // s[i].field
    if (const MemberExpr *ME = dyn_cast<MemberExpr>(S)) {
      const Expr *Base = ME->getBase()->IgnoreParenImpCasts();
      if (const ArraySubscriptExpr *ASE =
              dyn_cast<ArraySubscriptExpr>(Base)) {
        const Expr *Idx = ASE->getIdx()->IgnoreParenImpCasts();
        Base = ASE->getBase()->IgnoreParenImpCasts();
        if (const DeclRefExpr *BaseDRE = dyn_cast<DeclRefExpr>(Base)) {
          if (const VarDecl *ArrVD =
                  dyn_cast<VarDecl>(BaseDRE->getDecl())) {
            if (IsOwnedElementArrayType(ArrVD->getType())) {
              bool GoodIdx = IsLoopIndex(Idx, LoopVar);
              setCovered(ArrVD, GoodIdx);
            }
          }
        }
        // w.arr[i].a / w2.w[i].arr[j].a: the subscripted base is one or more
        // member-array fields (w.arr / w2.w[i].arr) of a tracked host struct.
        // They are covered the same way as s[i].a / a[i][j]; the bound must
        // match the length of the array field whose index is the loop var.
        if (isa<MemberExpr>(Base)) {
          // Collect the array-field chain (shared PeelFieldIndexLevels).
          const VarDecl *HostVD = nullptr;
          llvm::SmallVector<std::pair<const MemberExpr *, const Expr *>, 4>
              Fields;
          if (PeelFieldIndexLevels(ASE, HostVD, Fields) && HostVD) {
            if (IsTrackedType(HostVD->getType())) {
                for (const auto &F : Fields) {
                  // Host-array levels (s[i] in s[i].p) carry no field
                  // length; only field levels participate in the bound
                  // check.
                  if (!F.first)
                    continue;
                  bool GoodIdx = false;
                  if (const DeclRefExpr *IdxDRE =
                          dyn_cast<DeclRefExpr>(F.second))
                    GoodIdx = IsLoopIndex(IdxDRE, LoopVar);
                  if (!GoodIdx)
                    continue;
                  bool BoundOk = false;
                  if (IsConstantBound) {
                    if (const auto *CAT = Context.getAsConstantArrayType(
                            F.first->getType()))
                      BoundOk = CAT->getSize().getZExtValue() ==
                                BoundVal.getZExtValue();
                  }
                  if (BoundOk) {
                    setCovered(HostVD, /*GoodIdx=*/true);
                    FieldArrayDone.insert(HostVD);
                    break;
                  }
                }
            }
          }
        }
      }
    }
    if (const ForStmt *InnerFS = dyn_cast<ForStmt>(S)) {
      // Nested loop: only scan its body. The init/cond/incr expressions
      // carry no element-transfer semantics and the inner loop is
      // classified independently by scanForLoops (avoids re-scanning them
      // once per enclosing loop level). A break inside the inner loop
      // targets the inner loop, so DirectBody = false.
      scanBody(InnerFS->getBody(), false);
      return;
    }
    if (isa<WhileStmt>(S) || isa<DoStmt>(S) || isa<SwitchStmt>(S)) {
      // A break inside a nested while/do/switch does not exit the outer
      // for.
      for (const Stmt *Child : S->children())
        scanBody(Child, false);
      return;
    }
    for (const Stmt *Child : S->children())
      scanBody(Child, DirectBody);
  }

  void collectSites(const Stmt *S) {
    if (!S)
      return;
    // MemberExpr root (w.arr[i].a / arr[i].in[j].a / s[i].a[j]): the
    // dataflow transfers on the MemberExpr itself, so record it (not just
    // its subscripted sub-expressions).
    if (const MemberExpr *ME = dyn_cast<MemberExpr>(S)) {
      const VarDecl *HostVD = nullptr;
      llvm::SmallVector<std::pair<const MemberExpr *, const Expr *>, 4>
          Levels;
      if (PeelFieldIndexLevels(ME, HostVD, Levels) && HostVD &&
          llvm::is_contained(Covered, HostVD) && !Levels.empty() &&
          // A member whose type is itself an array (s[i].arr -- the
          // subscripted field array) is not a transfer site by itself: the
          // element transfer happens on the enclosing ArraySubscriptExpr,
          // handled by the ASE field branch below. Collecting it here would
          // let the dataflow chain-check (Site -> base) allow a
          // field-element transfer whose field index shares the host index
          // variable (s[i].arr[i] releases only the diagonal).
          !ME->getType()->isArrayType()) {
        // The innermost subscript indexes the tracked field array, so this
        // loop must drive it; every enclosing subscript level must be iterated
        // in full by its own loop. This rejects a partial release (the diagonal
        // s[i].arr[i], a constant host index s[0].arr[j], a non-loop host index
        // s[k].arr[j], a partially iterating enclosing loop, or a constant
        // inner index m[a].arr[0]): the transfer would otherwise silently move
        // the whole field aggregate and hide the leak of the unreleased
        // elements.
        if (FullRangeFieldChainInnermost(Context, ME, LoopVar, LoopOfCounter))
          LoopInfo.AllowedTransferExprs[FS].insert(ME);
      }
      for (const Stmt *Child : S->children())
        collectSites(Child);
      return;
    }
    if (const ArraySubscriptExpr *ASE =
            dyn_cast<ArraySubscriptExpr>(S)) {
      SmallVector<const Expr *, 4> Idxs;
      const Expr *Base = nullptr;
      PeelSubscriptChain(ASE, Idxs, Base, SubscriptIdxsCache,
                        SubscriptBaseCache);
      if (const DeclRefExpr *BaseDRE = dyn_cast<DeclRefExpr>(Base)) {
        if (const VarDecl *ArrVD =
                dyn_cast<VarDecl>(BaseDRE->getDecl())) {
          if (llvm::is_contained(Covered, ArrVD) && !Idxs.empty()) {
            if (const DeclRefExpr *IdxDRE =
                    dyn_cast<DeclRefExpr>(Idxs.front())) {
              if (IsLoopIndex(IdxDRE, LoopVar))
                LoopInfo.AllowedTransferExprs[FS].insert(ASE);
            }
          }
        }
      }
      // s[i].a[j]
      if (const MemberExpr *ME = dyn_cast<MemberExpr>(Base)) {
        const Expr *MBase = ME->getBase()->IgnoreParenImpCasts();
        const Expr *MIdx = nullptr;
        while (const ArraySubscriptExpr *A =
                   dyn_cast<ArraySubscriptExpr>(MBase)) {
          MIdx = A->getIdx()->IgnoreParenImpCasts();
          MBase = A->getBase()->IgnoreParenImpCasts();
        }
        if (const DeclRefExpr *BaseDRE = dyn_cast<DeclRefExpr>(MBase)) {
          if (const VarDecl *ArrVD =
                  dyn_cast<VarDecl>(BaseDRE->getDecl())) {
            if (llvm::is_contained(Covered, ArrVD) && MIdx) {
              if (const DeclRefExpr *IdxDRE =
                      dyn_cast<DeclRefExpr>(MIdx)) {
                if (IsLoopIndex(IdxDRE, LoopVar)) {
                  // Mirror of the field branch below: when the field-level
                  // index (Idxs.front(), e.g. arr's i in s[i].arr[i]) shares
                  // the host-level index variable (MIdx, s's i), only the
                  // diagonal is released -- do not allow the transfer.
                  if (!FindReusedHostIndex(Idxs.front(), {MIdx})) {
                    LoopInfo.AllowedTransferExprs[FS].insert(ASE);
                    LoopInfo.AllowedTransferExprs[FS].insert(ME);
                  }
                }
              }
            }
          }
        }
      }
      // Struct-field owned-element array: w.arr[i] (and nested o.w.arr[i]).
      if (const MemberExpr *FieldME = dyn_cast<MemberExpr>(Base)) {
        const Expr *HostBase = FieldME;
        llvm::SmallVector<const Expr *, 4> HostIdxs;
        while (true) {
          if (const MemberExpr *M = dyn_cast<MemberExpr>(HostBase))
            HostBase = M->getBase()->IgnoreParenImpCasts();
          else if (const ArraySubscriptExpr *A =
                       dyn_cast<ArraySubscriptExpr>(HostBase)) {
            HostIdxs.push_back(A->getIdx()->IgnoreParenImpCasts());
            HostBase = A->getBase()->IgnoreParenImpCasts();
          } else
            break;
        }
        if (const DeclRefExpr *HostDRE = dyn_cast<DeclRefExpr>(HostBase)) {
          if (const VarDecl *HostVD =
                  dyn_cast<VarDecl>(HostDRE->getDecl())) {
            if (llvm::is_contained(Covered, HostVD) && !Idxs.empty()) {
              if (const DeclRefExpr *IdxDRE =
                      dyn_cast<DeclRefExpr>(Idxs.front())) {
                if (IsLoopIndex(IdxDRE, LoopVar)) {
                  // Mirror of scanBody: if the field-array index (Idxs.front)
                  // shares a variable with a host index (s[i].arr[i]), only
                  // the diagonal is released -- do not allow the transfer.
                  if (!FindReusedHostIndex(Idxs.front(), HostIdxs)) {
                    LoopInfo.AllowedTransferExprs[FS].insert(ASE);
                    LoopInfo.AllowedTransferExprs[FS].insert(FieldME);
                  }
                }
              }
            }
          }
        }
      }
    }
    if (const ForStmt *InnerFS = dyn_cast<ForStmt>(S)) {
      collectSites(InnerFS->getBody());
      return;
    }
    for (const Stmt *Child : S->children())
      collectSites(Child);
  }

  bool classifyForLoop(const ForStmt *F,
                       SmallVectorImpl<const VarDecl *> &OutCovered) {
    FS = F;
    LoopVar = nullptr;
    BoundVD = nullptr;
    BoundVal = llvm::APSInt();
    IsConstantBound = false;
    Body = F->getBody();
    HasEarlyExit = false;
    EarlyExitLoc = SourceLocation();
    ArrayOk.clear();
    ArrayFailures.clear();
    MultiDimDone.clear();
    FieldArrayDone.clear();
    Covered.clear();
    // Record why this for-loop is not qualifying (for a diagnostic note) and
    // reject it. Loc (when valid) is the exact statement to point the note
    // at (e.g. a return/break or a loop-variable modification site).

    // Per-loop fast bail-out: a loop whose body contains no array subscript
    // at all can never transfer an array element, so skip the full
    // classification (early-exit / coverage / path / sites scans). The
    // non-qualifying note uses the generic index-misuse reason.
    if (!HasSubscriptInBody(FS->getBody()))
      return reject(NonQualifyingLoopReason::ElemAccessMismatch);

    if (!parseQualifyingLoopHead())
      return false;

    if (!IsIncrementByOne(Context, FS->getInc(), LoopVar))
      return reject(NonQualifyingLoopReason::IncrNotByOne);
    SourceLocation LoopVarTouchedLoc;
    if (!LoopVarUntouchedOutsideIncr(FS, LoopVar, LoopVarTouchedLoc))
      return reject(NonQualifyingLoopReason::LoopVarModified,
                    LoopVarTouchedLoc);

    if (!Body)
      return reject(NonQualifyingLoopReason::NotQualifying);

    // No return/goto in the body; a break counts only when it targets
    // this loop directly (break inside a nested loop is fine). EarlyExitLoc
    // points the note at the exiting statement itself.
    // Early-exit detection is fused into the coverage scan below (one
    // traversal does both): see scanBody.

    // Collect covered arrays (body must use only a[i] / a[i][j] / s[i].f
    // with the loop index; multi-dim nests must match dimension order).
    // Mark VD as covered for this loop: any access sets true, an access with
    // an index that is not the loop variable sets false.
    // Arrays whose multi-dim nest fully validated inside matchNested (bound of
    // the current loop and all inner dimensions); exempt from the trailing
    // ArrayBoundMatches, which only checks the first (outermost) dimension.
    // Host structs whose array field (w.arr) was covered: the bound was
    // validated against the field's length inside scanBody, so the trailing
    // ArrayBoundMatches (which only understands the host's own type) is
    // skipped.
    scanBody(Body, true);
    if (HasEarlyExit)
      return reject(NonQualifyingLoopReason::EarlyExit, EarlyExitLoc);

    collectCoveredArrays();
    if (Covered.empty()) {
      // No array is covered by this loop: it is not a qualifying loop (and
      // must not enter the dataflow's special loop handling, which would
      // defer the exit block waiting for a back-edge state that may stay
      // empty). Report the first recorded per-array reason so the note is
      // accurate (bound mismatch vs index misuse).
      auto NC = LoopInfo.NonCoveredArrays.find(FS);
      if (NC != LoopInfo.NonCoveredArrays.end() && !NC->second.empty())
        return reject(NC->second.begin()->second.Reason,
                      NC->second.begin()->second.Loc);
      return reject(NonQualifyingLoopReason::BoundMismatch);
    }



    // Record transfer sites in this loop body (including nested loops, so
    // the outer loop of a multi-dim nest records a[i][j]).
    collectSites(Body);

    LoopInfo.QualifyingLoops.insert(FS);

    OutCovered = Covered;
    return true;
  }

  void scanForLoops(const Stmt *S) {
    if (!S)
      return;
    if (const ForStmt *FS = dyn_cast<ForStmt>(S)) {
      SmallVector<const VarDecl *, 4> Covered;
      classifyForLoop(FS, Covered);
    }
    for (const Stmt *Child : S->children())
      scanForLoops(Child);
  }

public:
  OwnedArrayLoopClassifier(ASTContext &Ctx, const FunctionDecl *F,
                           OwnedArrayLoopInfo &LI)
      : Context(Ctx), FD(F), LoopInfo(LI) {}

  void run() {
    if (!BodyUsesOwnedElementArray(FD))
      return;
    llvm::SmallPtrSet<const VarDecl *, 4> AllBoundVars;
    CollectLoopBoundVars(Context, FD, AllBoundVars);
    FindBoundVarModifications(Context, FD, AllBoundVars,
                             BoundVarModLocs);
    CollectLoopCounterVars(FD->getBody(), LoopOfCounter);
    scanForLoops(FD->getBody());
  }
};

void classifyOwnedArrayLoops(ASTContext &Context, const FunctionDecl *FD,
                             OwnedArrayLoopInfo &LoopInfo) {
  OwnedArrayLoopClassifier Classifier(Context, FD, LoopInfo);
  Classifier.run();
}


} // namespace clang

#endif // ENABLE_BSC
