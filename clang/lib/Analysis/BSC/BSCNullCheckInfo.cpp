//===- BSCNullCheckInfo.cpp - Nullability Check Results for Source CFGs --BSC-//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the nullability distinguish logic for both BSC
// nullability and ownership analysis.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/Analysis/Analyses/BSC/BSCNullCheckInfo.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/SmallVector.h"

using namespace clang;

namespace {

llvm::FoldingSetNodeID getSemanticExprID(const Expr *E, ASTContext &Ctx) {
  llvm::FoldingSetNodeID ID;
  E->IgnoreParenImpCasts()->Profile(ID, Ctx, true);
  return ID;
}

// Return whether LHS and RHS are semantically the same expression under `Ctx`
// For example, for `p` and `q` that are different DeclRefExpr but both refer
// to the same VarDecl, they are semantically the same expression.
bool semanticEqual(const Expr *LHS, const Expr *RHS, ASTContext &Ctx) {
  return getSemanticExprID(LHS, Ctx) == getSemanticExprID(RHS, Ctx);
}

template <unsigned N>
bool semanticContains(const llvm::SmallPtrSet<const Expr *, N> &Set,
                      const Expr *Candidate, ASTContext &Ctx) {
  for (const Expr *Existing : Set) {
    if (semanticEqual(Existing, Candidate, Ctx)) {
      return true;
    }
  }
  return false;
}

template <unsigned N>
void semanticUnion(llvm::SmallPtrSet<const Expr *, N> &LHS,
                   const llvm::SmallPtrSet<const Expr *, N> &RHS,
                   ASTContext &Ctx) {
  for (const Expr *E : RHS) {
    if (!semanticContains(LHS, E, Ctx)) {
      LHS.insert(E);
    }
  }
}

template <unsigned N>
void semanticIntersect(llvm::SmallPtrSet<const Expr *, N> &LHS,
                       const llvm::SmallPtrSet<const Expr *, N> &RHS,
                       ASTContext &Ctx) {
  llvm::SmallVector<const Expr *, N> Kept;
  for (const Expr *E : LHS) {
    if (semanticContains(RHS, E, Ctx)) {
      Kept.push_back(E);
    }
  }
  LHS.clear();
  for (const Expr *E : Kept) {
    LHS.insert(E);
  }
}

/// Return whether the evaluation process of E contains any
/// builtin array access such as [], or its equivalent form *(+) *(-)
bool containsArrayAccess(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  if (isa<ArraySubscriptExpr>(E)) {
    return true;
  }
  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    return containsArrayAccess(ME->getBase());
  }
  if (auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_Deref) {
      Expr *SubE = UO->getSubExpr()->IgnoreParenImpCasts();
      if (auto *BO = dyn_cast<BinaryOperator>(SubE)) {
        if (BO->isAdditiveOp())
          return true;
      }
    }
  }
  return false;
}

/// This function is identity only when E is a trackable pointer expression.
/// Otherwise return nullptr.
/// Caller must ensure Cond is not nullptr, and Cond is a simple expression
const Expr *getTrackablePtr(const Expr *Cond) {
  Cond = Cond->IgnoreParenImpCastsSafe();
  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    // for condition like (x, p), rhs may be trackable
    if (BO->getOpcode() == BO_Comma) {
      return getTrackablePtr(BO->getRHS());
    }
    // for condition like (p = foo()), lhs may be trackable
    if (BO->isAssignmentOp()) {
      return getTrackablePtr(BO->getLHS());
    }
  }
  // Trackable restriction: non-volatile, non-atomic lvalue pointer expression
  // without built-in array subscription or pointer-arithmetic-dereference.
  if (!Cond->isLValue()) {
    return nullptr;
  }
  QualType QT = Cond->getType();
  if (!QT.getCanonicalType()->isPointerType()) {
    return nullptr;
  }
  if (QT.isVolatileQualified() || QT->isAtomicType()) {
    return nullptr;
  }
  if (containsArrayAccess(Cond)) {
    return nullptr;
  }
  return Cond;
}

/// Extract a trackable pointer that is distinguished null-ness or non-null-ness
/// sub-expression from Cond.
/// Caller must ensure Cond is not nullptr, and Cond is a simple expression.
/// NullNess is an output parameter indicating whether the extracted expression
/// is checked for null-ness
/// true => Distinguished null-ness, such as p == nullptr
/// false => Distinguished non-null-ness, such as p != nullptr
const Expr *extractDistinguishedTrackablePtr(const Expr *Cond, ASTContext &Ctx,
                                             bool &NullNess) {
  Cond = Cond->IgnoreParenImpCasts();
  // Generic pointer-valued condition, e.g. `if (*p)` / `if (**p)`.
  if (const Expr *PtrExpr = getTrackablePtr(Cond)) {
    NullNess = false;
    return PtrExpr;
  }

  // if (p) { p is present }
  if (const auto *DRE = dyn_cast<DeclRefExpr>(Cond)) {
    if (const Expr *PtrExpr = getTrackablePtr(DRE)) {
      NullNess = false;
      return PtrExpr;
    }
  }
  // if (s.p) { s.p is present }
  // if (s->p) { s->p is present }
  if (const auto *ME = dyn_cast<MemberExpr>(Cond)) {
    if (const Expr *PtrExpr = getTrackablePtr(ME)) {
      NullNess = false;
      return PtrExpr;
    }
  }
  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    // comma expression's condition value is RHS, keep extracting from RHS.
    // for example: if (flag1, flag2, p) { p is present }
    if (BO->getOpcode() == BO_Comma) {
      return extractDistinguishedTrackablePtr(BO->getRHS(), Ctx, NullNess);
    }
    // assignment expression's condition value is the assigned value, and it is
    // also stored into LHS. Track LHS first so we can reason about
    // `if (p = foo(...))` as a check on `p`.
    if (BO->isAssignmentOp()) {
      if (const Expr *PtrExpr = getTrackablePtr(BO->getLHS())) {
        NullNess = false;
        return PtrExpr;
      }
      // Fallback: if LHS is not trackable, turn for RHS's value.
      return extractDistinguishedTrackablePtr(BO->getRHS(), Ctx, NullNess);
    }
    // if (p == nullptr) { p is null }
    // if (p != nullptr) { p is present }
    if (BO->isEqualityOp()) {
      bool NullLHS = BO->getLHS()->isNullExpr(Ctx);
      bool NullRHS = BO->getRHS()->isNullExpr(Ctx);
      if (NullLHS == NullRHS) {
        // XNOR: not a null check, or both sides are trivially semantic null
        return nullptr;
      }
      const Expr *Candidate = NullLHS ? BO->getRHS() : BO->getLHS();
      if (const Expr *PtrExpr = getTrackablePtr(Candidate)) {
        NullNess = BO->getOpcode() == BO_EQ;
        return PtrExpr;
      }
    }
  }
  return nullptr;
}
} // namespace

void NullCheckInfo::init(const Expr *Cond, bool Negate) {
  bool EvaluationResult;
  if (Cond->EvaluateAsBooleanCondition(EvaluationResult, ctx)) {
    triviality = (EvaluationResult != Negate) ? ConstTrue : ConstFalse;
    return;
  }
  Cond = Cond->IgnoreParenImpCasts();
  // recursively process logical operators
  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot) {
      init(UO->getSubExpr(), !Negate);
      return;
    }
  }
  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    if (BO->isLogicalOp()) {
      NullCheckInfo LHSInfo(ctx);
      LHSInfo.init(BO->getLHS(), Negate);
      LHSInfo.obliviateInfeasible();
      NullCheckInfo RHSInfo(ctx);
      RHSInfo.init(BO->getRHS(), Negate);
      RHSInfo.obliviateInfeasible();
      *this = std::move(LHSInfo);
      if ((BO->getOpcode() == BO_LAnd) != Negate)
        *this &= std::move(RHSInfo);
      else
        *this |= std::move(RHSInfo);
      return;
    }
  }
  // basic case: extract and register pointer expression
  extractAndInsert(Cond, Negate);
}

NullCheckInfo &NullCheckInfo::operator&=(NullCheckInfo &&RHS) {
  if (triviality == ConstFalse ||
      RHS.triviality == ConstFalse) {
    // The ObliviateInfeasible function called later handles infeasibility,
    // Additionally, we also need handle constant-evaluated trivial condition:
    // if (p && 0 < -1) { unreachable } else { p is unknown }
    //
    // for && operator, if either branch's triviality is ConstEvaluatedFalse,
    // the whole condition is ConstEvaluatedFalse, and nothing useful derived.
    triviality = ConstFalse;
    nullCheckedExprs.clear();
    presentCheckedExprs.clear();
    return *this;
  }
  // triviality transfer: if one branch is trivially evaluated as true,
  // use another branch's triviality because in && operation,
  // ConstEvaluatedTrue is the lowest level that could be overriden by
  // any other triviality
  if (triviality == ConstTrue) {
    triviality = RHS.triviality;
  }
  // When combining two branches of && operators, we synthesize the
  // null-checked expressions as the union of the two branches,
  // because if either branch checks an expression for null-ness,
  // the whole condition is checking that expression for null-ness,
  // for example:
  // if (p && q)
  // *this: {present: {p}, null: {}}
  // RHS: {present: {q}, null: {}}
  // result: {present: {p, q}, null: {}}
  //
  // if (p && !q)
  // *this: {present: {p}, null: {}}
  // RHS: {present: {}, null: {q}}
  // result: {present: {p}, null: {q}}
  //
  // if (p && x > 1)
  // *this: {present: {p}, null: {}}
  // RHS: {present: {}, null: {}}
  // result: {present: {p}, null: {}}
  //
  // if (p && 0 < 1)
  // *this: {present: {p}, null: {}}
  // RHS: {present: {}, null: {}}
  // result: {present: {p}, null: {}}
  semanticUnion(nullCheckedExprs, RHS.nullCheckedExprs, ctx);
  semanticUnion(presentCheckedExprs, RHS.presentCheckedExprs, ctx);
  return *this;
}

NullCheckInfo &NullCheckInfo::operator|=(NullCheckInfo &&RHS) {
  // Cannot use De Morgan's law: !(LHS || RHS) === !LHS && !RHS
  // Because the underlying set operations are not as simple as a boolean value

  // for || operator, if either branch's triviality is ConstEvaluatedTrue,
  // the whole condition is ConstEvaluatedTrue, and nothing useful derived.
  if (triviality == ConstTrue ||
      RHS.triviality == ConstTrue) {
    // if (p || 1 > 0) { p is unknown }
    // if (1 > 0 || p) { p is unknown }
    triviality = ConstTrue;
    nullCheckedExprs.clear();
    presentCheckedExprs.clear();
    return *this;
  }
  // Additionally, if one of the branches is trivially evaluated as false,
  // we can simply derive another branch's result, instead of intersection.
  if (triviality == ConstFalse) {
    // if (p || 0 > 1) { p is present }
    triviality = RHS.triviality;
    return *this = std::move(RHS);
  }
  if (RHS.triviality == ConstFalse) {
    // if (0 > 1 || p) { p is present }
    return *this;
  }
  // When combining two branches of || operators, we synthesize the null-checked
  // expressions as the intersection of the two branches,
  // because only when both branches check an expression for null-ness,
  // the whole condition is checking that expression for null-ness,
  // for example:
  // if (p || q)
  // *this: {present: {p}, null: {}}
  // RHS: {present: {q}, null: {}}
  // result: {present: {}, null: {}}
  //
  // if (p || (p && q))
  // *this: {present: {p}, null: {}}
  // RHS: {present: {p, q}, null: {}}
  // result: {present: {p}, null: {}}
  //
  // if (p || x > 1)
  // *this: {present: {p}, null: {}}
  // RHS: {present: {}, null: {}}
  // result: {present: {}, null: {}}
  semanticIntersect(nullCheckedExprs, RHS.nullCheckedExprs, ctx);
  semanticIntersect(presentCheckedExprs, RHS.presentCheckedExprs, ctx);
  return *this;
}

void NullCheckInfo::extractAndInsert(const Expr *Cond, bool Negate) {
  bool NullNess = false;
  if (const Expr *PtrExpr =
          extractDistinguishedTrackablePtr(Cond, ctx, NullNess)) {
    if (NullNess != Negate) {
      nullCheckedExprs.insert(PtrExpr);
    } else {
      presentCheckedExprs.insert(PtrExpr);
    }
  }
}

void NullCheckInfo::obliviateInfeasible() {
  // semanticIntersect is not intuitively symmetric,
  // because nullCheckedExpr could contain a `p` from a AST node,
  // and presentCheckedExprs could contain a semantically equivalent but
  // syntactically different `p` from another AST node,
  // and we should obliviate both of them
  llvm::SmallPtrSet<const Expr *, SetSize> NullIntersection(nullCheckedExprs);
  llvm::SmallPtrSet<const Expr *, SetSize> PresentIntersection(
      presentCheckedExprs);
  semanticIntersect(NullIntersection, presentCheckedExprs, ctx);
  semanticIntersect(PresentIntersection, nullCheckedExprs, ctx);
  for (const Expr *E : NullIntersection) {
    nullCheckedExprs.erase(E);
  }
  for (const Expr *E : PresentIntersection) {
    presentCheckedExprs.erase(E);
  }
}

NullCheckInfo::NullCheckInfo(const Expr *Cond, ASTContext &Ctx)
    : triviality(NonTrivial), ctx(Ctx) {
  if (!Cond)
    return;
  init(Cond, /*Negate=*/false);
  obliviateInfeasible();
}

NullCheckInfo &NullCheckInfo::operator=(NullCheckInfo &&RHS) {
  triviality = RHS.triviality;
  nullCheckedExprs = std::move(RHS.nullCheckedExprs);
  presentCheckedExprs = std::move(RHS.presentCheckedExprs);
  return *this;
}

#endif // ENABLE_BSC