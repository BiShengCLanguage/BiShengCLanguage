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
                dyn_cast<BinaryOperator>(BO->getRHS()->IgnoreParens())) {
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

// The loop bound must equal the array length: constant N vs constant
// array size, or VLA size variable n (3.2.1 cond 2).
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
// True when the field-array index (e.g. arr's i in s[i].arr[i]) shares its
// variable with any host-level index (s's i): the loop releases only the
// diagonal, not the whole field array, so the field level must not be
// treated as covered / transferable. Shared by scanBody and collectSites.
static bool FieldIndexSharesHostIndex(const Expr *FieldIdx,
                                      llvm::ArrayRef<const Expr *> HostIdxs) {
  const DeclRefExpr *FD = dyn_cast<DeclRefExpr>(FieldIdx);
  if (!FD)
    return false;
  for (const Expr *HIdx : HostIdxs) {
    const DeclRefExpr *HD = dyn_cast<DeclRefExpr>(HIdx);
    if (HD && HD->getDecl() == FD->getDecl())
      return true;
  }
  return false;
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

// E is arr[LoopVar] / arr[LoopVar].field / w.arr[i] / o.w.arr[i] of ArrVD
// (single-level). If so, FieldName receives the field path ("" for plain
// arrays, "field" for s[i].field, "arr[]" / "w.arr[]" for field arrays).
static bool IsElemAccess(const Expr *E, const VarDecl *ArrVD,
                         const VarDecl *LoopVar, std::string &FieldName) {
  if (!E)
    return false;
  E = E->IgnoreParenImpCasts();
  if (const MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
    const Expr *Base = ME->getBase()->IgnoreParenImpCasts();
    if (const ArraySubscriptExpr *ASE =
            dyn_cast<ArraySubscriptExpr>(Base)) {
      if (const DeclRefExpr *B = dyn_cast<DeclRefExpr>(
              ASE->getBase()->IgnoreParenImpCasts())) {
        if (B->getDecl() == ArrVD) {
          if (const DeclRefExpr *Idx = dyn_cast<DeclRefExpr>(
                  ASE->getIdx()->IgnoreParenImpCasts())) {
            if (IsLoopIndex(Idx, LoopVar)) {
              FieldName = ME->getMemberNameInfo().getAsString();
              return true;
            }
          }
        }
      }
    }
  } else if (const ArraySubscriptExpr *ASE =
                  dyn_cast<ArraySubscriptExpr>(E)) {
    const Expr *B = ASE->getBase()->IgnoreParenImpCasts();
    if (const DeclRefExpr *B2 = dyn_cast<DeclRefExpr>(B)) {
      if (B2->getDecl() == ArrVD) {
        if (const DeclRefExpr *Idx = dyn_cast<DeclRefExpr>(
                ASE->getIdx()->IgnoreParenImpCasts())) {
          if (IsLoopIndex(Idx, LoopVar)) {
            FieldName = "";
            return true;
          }
        }
      }
    }
    // Struct-field array element: w.arr[i] / o.w.arr[i]. The base is a
    // member access; peel the host + "arr[]" path (PeelHostAndFieldPath,
    // shared with the CFG dataflow) so null-free if recognition
    // (`if (w.arr[i] != nullptr) { free }`) works like local arrays.
    if (dyn_cast<MemberExpr>(B)) {
      const VarDecl *HostVD = nullptr;
      std::string Path;
      if (PeelHostAndFieldPath(E, HostVD, Path) && HostVD == ArrVD &&
          !Path.empty() && Path.find("[]") != std::string::npos) {
        if (const DeclRefExpr *Idx = dyn_cast<DeclRefExpr>(
                ASE->getIdx()->IgnoreParenImpCasts())) {
          if (IsLoopIndex(Idx, LoopVar)) {
            FieldName = Path;
            return true;
          }
        }
      }
    }
  }
  return false;
}
// Per-array transfer summary of one branch (then/else): which covered arrays
// are moved out / assigned into, and the final order-sensitive state
// (0 = untouched, 1 = moved, 2 = owned, 3 = null). Shared by the null-free-if
// check (a move-out without a later assign-in frees the element) and the
// generic if path-consistency check.
struct BranchSummary {
  llvm::DenseMap<const VarDecl *, bool> HasMoveOut, HasAssignIn;
  llvm::DenseMap<const VarDecl *, unsigned> FinalStates;
};

static BranchSummary SummarizeBranch(
    ASTContext &Context,
    const llvm::SmallVectorImpl<const VarDecl *> &Covered,
    const VarDecl *LoopVar, const Stmt *Br) {
  BranchSummary Sum;
  std::function<void(const Stmt *)> scan = [&](const Stmt *X) {
    if (!X)
      return;
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(X)) {
      if (BO->getOpcode() == BO_Assign) {
        const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
        const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
        std::string F;
        for (const VarDecl *ArrVD : Covered) {
          if (IsElemAccess(LHS, ArrVD, LoopVar, F)) {
            Sum.HasAssignIn[ArrVD] = true;
            Sum.FinalStates[ArrVD] = IsNullExpr(Context, RHS) ? 3 : 2;
          }
          if (IsElemAccess(RHS, ArrVD, LoopVar, F)) {
            Sum.HasMoveOut[ArrVD] = true;
            Sum.FinalStates[ArrVD] = 1;
          }
        }
      }
    }
    if (const CallExpr *CE = dyn_cast<CallExpr>(X)) {
      for (const Expr *Arg : CE->arguments()) {
        std::string F;
        for (const VarDecl *ArrVD : Covered) {
          if (IsElemAccess(Arg->IgnoreParenCasts(), ArrVD, LoopVar, F)) {
            Sum.HasMoveOut[ArrVD] = true;
            Sum.FinalStates[ArrVD] = 1;
            break;
          }
        }
      }
    }
    if (const ReturnStmt *RS = dyn_cast<ReturnStmt>(X)) {
      if (const Expr *RV = RS->getRetValue()) {
        std::string F;
        for (const VarDecl *ArrVD : Covered) {
          if (IsElemAccess(RV->IgnoreParenCasts(), ArrVD, LoopVar, F)) {
            Sum.HasMoveOut[ArrVD] = true;
            Sum.FinalStates[ArrVD] = 1;
            break;
          }
        }
      }
    }
    for (const Stmt *C : X->children())
      scan(C);
  };
  scan(Br);
  return Sum;
}



// Collect every VLA bound variable used in any for-loop condition (3.2.1
// cond 2): they must not be modified / address-taken / mutably borrowed.
static void CollectBoundVarInStmt(
    const Stmt *S, llvm::SmallPtrSetImpl<const VarDecl *> &Out) {
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
          }
        }
      }
    }
  }
  for (const Stmt *Child : S->children())
    CollectBoundVarInStmt(Child, Out);
}

static void CollectLoopBoundVars(ASTContext &Context, const FunctionDecl *FD,
                                 llvm::SmallPtrSetImpl<const VarDecl *> &Out) {
  CollectBoundVarInStmt(FD->getBody(), Out);
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
  OwnershipDiagReporter &OwnershipReporter;

  // Function-wide state.
  llvm::DenseMap<const ArraySubscriptExpr *, SmallVector<const Expr *, 4>>
      SubscriptIdxsCache;
  llvm::DenseMap<const ArraySubscriptExpr *, const Expr *> SubscriptBaseCache;
  llvm::DenseMap<const VarDecl *, SourceLocation> BoundVarModLocs;

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
  llvm::SmallPtrSet<const VarDecl *, 4> MultiDimDone;
  llvm::SmallPtrSet<const VarDecl *, 4> FieldArrayDone;
  SmallVector<const VarDecl *, 4> Covered;
  bool Inconsistent = false;

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
      if (!BoundDRE)
        return reject(NonQualifyingLoopReason::CondNotMatch);
      BoundVD = dyn_cast<VarDecl>(BoundDRE->getDecl());
      if (!BoundVD || !BoundVD->getType()->isIntegerType())
        return reject(NonQualifyingLoopReason::CondNotMatch);
      auto ModIt = BoundVarModLocs.find(BoundVD);
      if (ModIt != BoundVarModLocs.end())
        return reject(NonQualifyingLoopReason::VlaBoundVarModified,
                      ModIt->second);
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
    } else {
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
                               BoundVD))
          return false;
      } else {
        if (const auto *CAT = Context.getAsConstantArrayType(DimTy)) {
          if (!CBO->getRHS()->isIntegerConstantExpr(Context) ||
              CBO->getRHS()->EvaluateKnownConstInt(Context).getZExtValue() !=
                  CAT->getSize().getZExtValue())
            return false;
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
        // Bound mismatch. For a VLA whose length variable is a *different*
        // variable than the loop bound variable, report that rule instead of
        // a generic bound mismatch.
        NonQualifyingLoopReason R = NonQualifyingLoopReason::BoundMismatch;
        if (!IsConstantBound) {
          if (const auto *VAT = dyn_cast_or_null<VariableArrayType>(
                  KV.first->getType()->getAsArrayTypeUnsafe())) {
            if (const DeclRefExpr *SizeDRE = dyn_cast<DeclRefExpr>(
                    VAT->getSizeExpr()->IgnoreParenImpCasts())) {
              if (SizeDRE->getDecl() != BoundVD)
                R = NonQualifyingLoopReason::VlaBoundVarMismatch;
            }
          }
        }
        LoopInfo.NonCoveredArrays[FS][KV.first] = {R, SourceLocation()};
      } else {
        LoopInfo.NonCoveredArrays[FS][KV.first] = {
            NonQualifyingLoopReason::ElemAccessMismatch, SourceLocation()};
      }
    }
    LoopInfo.LoopCoveredArrays[FS].assign(Covered.begin(), Covered.end());
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
            if (Idxs.size() == 1) {
              if (const DeclRefExpr *IdxDRE =
                      dyn_cast<DeclRefExpr>(Idxs.front()))
                GoodIdx = IsLoopIndex(IdxDRE, LoopVar);
            } else if (Idxs.size() > 1) {
              GoodIdx = matchNestedDimensions(FS, ArrVD->getType(), ArrVD,
                                        Idxs);
              if (GoodIdx)
                MultiDimDone.insert(ArrVD);
            }
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
              if (GoodIdx && FieldIdxOk)
                ArrayOk[ArrVD] = true;
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
              // Collect the array-field chain levels, innermost first:
              // (index expression, array-field type). The innermost level
              // is the outer ASE's Idx and FieldME's type; each outer
              // level is a subscripted member access (o.w[i].arr[j]: j is
              // the arr level, i is the w level). A loop covers the host
              // when it drives ANY level whose length matches the bound.
              llvm::SmallVector<std::pair<const Expr *, QualType>, 4>
                  Levels;
              Levels.push_back({Idxs.front(), FieldME->getType()});
              const Expr *LCur =
                  FieldME->getBase()->IgnoreParenImpCasts();
              while (const ArraySubscriptExpr *A =
                         dyn_cast<ArraySubscriptExpr>(LCur)) {
                const Expr *ABase = A->getBase()->IgnoreParenImpCasts();
                if (const MemberExpr *NextME =
                        dyn_cast<MemberExpr>(ABase)) {
                  Levels.push_back(
                      {A->getIdx()->IgnoreParenImpCasts(),
                       NextME->getType()});
                  LCur = NextME->getBase()->IgnoreParenImpCasts();
                } else {
                  break;
                }
              }
              for (const auto &L : Levels) {
                bool GoodIdx = false;
                const DeclRefExpr *IdxDRE =
                    dyn_cast<DeclRefExpr>(L.first);
                if (IdxDRE)
                  GoodIdx = IsLoopIndex(IdxDRE, LoopVar);
                if (!GoodIdx)
                  continue;
                // The field-array level (Levels.front()) must not share its
                // index variable with a host level: `s[i].arr[i]` releases
                // only the diagonal (s[0].arr[0], s[1].arr[1], ...), not the
                // whole arr field, so the aggregate must not be marked moved.
                if (&L == &Levels.front()) {
                  llvm::SmallVector<const Expr *, 4> HostIdxs;
                  for (size_t k = 1; k < Levels.size(); ++k)
                    HostIdxs.push_back(Levels[k].first);
                  if (FieldIndexSharesHostIndex(IdxDRE, HostIdxs)) {
                    GoodIdx = false;
                    continue;
                  }
                }
                bool BoundOk = false;
                if (IsConstantBound) {
                  if (const auto *CAT = Context.getAsConstantArrayType(
                          L.second))
                    BoundOk = CAT->getSize().getZExtValue() ==
                              BoundVal.getZExtValue();
                }
                if (BoundOk) {
                  ArrayOk[HostVD] = true;
                  FieldArrayDone.insert(HostVD);
                  break;
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
        if (const MemberExpr *FieldME = dyn_cast<MemberExpr>(Base)) {
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
    // Path-consistency and null-free if recognition: `if (a[i] != nullptr)
    // { free }` with no else is consistent; an if whose branches transfer
    // differently is reported (but does not disqualify the loop).
    if (const IfStmt *IS = dyn_cast<IfStmt>(S)) {
      if (!Inconsistent) {
        const Expr *CondE = IS->getCond()->IgnoreParenImpCasts();
        const Expr *Elem = nullptr;
        if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(CondE)) {
          if (BO->getOpcode() == BO_NE || BO->getOpcode() == BO_EQ) {
            const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
            const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
            if (IsNullExpr(Context, RHS))
              Elem = LHS;
            else if (IsNullExpr(Context, LHS))
              Elem = RHS;
          }
        } else if (const UnaryOperator *UO =
                       dyn_cast<UnaryOperator>(CondE)) {
          if (UO->getOpcode() == UO_LNot) {
            // if (!arr[i]) -- implicit null check (arr[i] == nullptr)
            const Expr *Inner = UO->getSubExpr()->IgnoreParenImpCasts();
            std::string F;
            for (const VarDecl *ArrVD : Covered)
              if (IsElemAccess(Inner, ArrVD, LoopVar, F)) {
                Elem = Inner;
                break;
              }
          }
        } else {
          // if (arr[i]) -- implicit null check (arr[i] != nullptr)
          std::string F;
          for (const VarDecl *ArrVD : Covered)
            if (IsElemAccess(CondE, ArrVD, LoopVar, F)) {
              Elem = CondE;
              break;
            }
        }
        const VarDecl *ElemArrVD = nullptr;
        if (Elem) {
          std::string F;
          for (const VarDecl *ArrVD : Covered) {
            if (IsElemAccess(Elem, ArrVD, LoopVar, F)) {
              ElemArrVD = ArrVD;
              break;
            }
          }
        }
        bool IsNullFree = false;
        if (ElemArrVD && IS->getElse() == nullptr) {
          BranchSummary S = SummarizeBranch(Context, Covered, LoopVar, IS->getThen());
          if (S.HasMoveOut[ElemArrVD] && !S.HasAssignIn[ElemArrVD])
            IsNullFree = true;
        }
        if (IsNullFree) {
          // Consistent null-free if: nothing to report; the site
          // collection below still scans the branches for transfers.
        }
        if (!IsNullFree) {
        // Generic if: then/else must end in the same per-(branch, array)
        // final state (0 = no transfer, 1 = moved, 2 = owned, 3 = null),
        // so a divergence in `b` does not taint `a` and a swapped-order
        // if/else (free-then-assign vs assign-then-free) is rejected.
        BranchSummary ThenS = SummarizeBranch(Context, Covered, LoopVar, IS->getThen());
        BranchSummary ElseS =
            IS->getElse() ? SummarizeBranch(Context, Covered, LoopVar,
                                            IS->getElse())
                          : BranchSummary();
        for (const VarDecl *ArrVD : Covered) {
          unsigned TS = ThenS.FinalStates.count(ArrVD)
                            ? ThenS.FinalStates[ArrVD]
                            : 0;
          unsigned ES = ElseS.FinalStates.count(ArrVD)
                            ? ElseS.FinalStates[ArrVD]
                            : 0;
          if (TS != ES) {
            Inconsistent = true;
            OwnershipDiagInfo DI(
                FS->getEndLoc(),
                OwnershipDiagKind::ArrayElemPathInconsistent,
                ArrVD->getNameAsString());
            OwnershipReporter.addDiagInfo(DI);
          }
        }
        }
      }
    }
    // MemberExpr root (w.arr[i].a / arr[i].in[j].a / s[i].a[j]): the
    // dataflow transfers on the MemberExpr itself, so record it (not just
    // its subscripted sub-expressions).
    if (const MemberExpr *ME = dyn_cast<MemberExpr>(S)) {
      const Expr *Cur2 = ME;
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
        // The innermost level carries the field-array index (w.arr[i] /
        // a[j] / arr[0].p); only a loop-variable index qualifies the site.
        // A constant inner index (s[i].a[0] / m[a].arr[0].p) must not be
        // allowed: the loop would only partially cover the field array and
        // a partial element free must not silently move the whole aggregate.
        if (IsLoopIndex(Levels.back().second, LoopVar))
          LoopInfo.AllowedTransferExprs.insert(ME);
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
                LoopInfo.AllowedTransferExprs.insert(ASE);
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
                  if (!FieldIndexSharesHostIndex(Idxs.front(), {MIdx})) {
                    LoopInfo.AllowedTransferExprs.insert(ASE);
                    LoopInfo.AllowedTransferExprs.insert(ME);
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
                  if (!FieldIndexSharesHostIndex(Idxs.front(), HostIdxs)) {
                    LoopInfo.AllowedTransferExprs.insert(ASE);
                    LoopInfo.AllowedTransferExprs.insert(FieldME);
                  }
                }
              }
            }
          }
        }
      }
    }
    if (const MemberExpr *ME = dyn_cast<MemberExpr>(S)) {
      const Expr *Base = ME->getBase()->IgnoreParenImpCasts();
      const Expr *Idx = nullptr;
      while (const ArraySubscriptExpr *ASE =
                 dyn_cast<ArraySubscriptExpr>(Base)) {
        Idx = ASE->getIdx()->IgnoreParenImpCasts();
        Base = ASE->getBase()->IgnoreParenImpCasts();
      }
      if (const DeclRefExpr *BaseDRE = dyn_cast<DeclRefExpr>(Base)) {
        if (const VarDecl *ArrVD =
                dyn_cast<VarDecl>(BaseDRE->getDecl())) {
          if (llvm::is_contained(Covered, ArrVD) && Idx) {
            if (const DeclRefExpr *IdxDRE = dyn_cast<DeclRefExpr>(Idx)) {
              if (IsLoopIndex(IdxDRE, LoopVar))
                LoopInfo.AllowedTransferExprs.insert(ME);
            }
          }
        }
      }
      // w.arr[i].a / w2.w[i].arr[j].a: the subscripted base is one or more
      // member-array fields of a covered host struct.
      if (const MemberExpr *FieldME = dyn_cast<MemberExpr>(Base)) {
        llvm::SmallVector<std::pair<const MemberExpr *, const Expr *>, 4>
            Fields;
        Fields.push_back({FieldME, Idx});
        const Expr *Cur = FieldME->getBase()->IgnoreParenImpCasts();
        while (const ArraySubscriptExpr *A =
                   dyn_cast<ArraySubscriptExpr>(Cur)) {
          const Expr *AIdx = A->getIdx()->IgnoreParenImpCasts();
          const Expr *ABase = A->getBase()->IgnoreParenImpCasts();
          if (const MemberExpr *NextME = dyn_cast<MemberExpr>(ABase)) {
            Fields.push_back({NextME, AIdx});
            Cur = NextME->getBase()->IgnoreParenImpCasts();
          } else {
            break;
          }
        }
        if (const DeclRefExpr *HostDRE = dyn_cast<DeclRefExpr>(Cur)) {
          if (const VarDecl *HostVD =
                  dyn_cast<VarDecl>(HostDRE->getDecl())) {
            if (llvm::is_contained(Covered, HostVD)) {
              for (const auto &F : Fields) {
                if (!F.second)
                  continue;
                if (const DeclRefExpr *IdxDRE =
                        dyn_cast<DeclRefExpr>(F.second)) {
                  if (IsLoopIndex(IdxDRE, LoopVar)) {
                    LoopInfo.AllowedTransferExprs.insert(ME);
                    break;
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
    MultiDimDone.clear();
    FieldArrayDone.clear();
    Covered.clear();
    Inconsistent = false;
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
                           OwnedArrayLoopInfo &LI,
                           OwnershipDiagReporter &R)
      : Context(Ctx), FD(F), LoopInfo(LI), OwnershipReporter(R) {}

  void run() {
    if (!BodyUsesOwnedElementArray(FD))
      return;
    llvm::SmallPtrSet<const VarDecl *, 4> AllBoundVars;
    CollectLoopBoundVars(Context, FD, AllBoundVars);
    FindBoundVarModifications(Context, FD, AllBoundVars,
                             BoundVarModLocs);
    scanForLoops(FD->getBody());
  }
};

void classifyOwnedArrayLoops(ASTContext &Context,
                             const FunctionDecl *FD,
                             OwnedArrayLoopInfo &LoopInfo,
                             OwnershipDiagReporter &OwnershipReporter) {
  OwnedArrayLoopClassifier Classifier(Context, FD, LoopInfo,
                                       OwnershipReporter);
  Classifier.run();
}


} // namespace clang

#endif // ENABLE_BSC
