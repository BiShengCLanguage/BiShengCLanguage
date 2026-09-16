//===--- SemaBSCOwnership.cpp - Semantic Analysis for BSC Ownership
//----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements dataflow analysis for BSC Ownership.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/AST/BSC/TypeBSC.h"
#include "clang/AST/Type.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Sema/Sema.h"

using namespace clang;
using namespace sema;

// for union fields/array/global variable type check
void Sema::CheckOwnedOrIndirectOwnedType(SourceLocation ErrLoc, QualType T, StringRef Env) {
  enum {
    ownedQualified,
    ownedTypedef,
    ownedFields
  };
  // Peel array types so arrays of owned / move-semantic element types are
  // caught too (e.g. `static struct A ls_arr[3]` where A has owned fields).
  while (const auto *AT = T->getAsArrayTypeUnsafe())
    T = AT->getElementType();
  bool Owned = T.isOwnedQualified() || T->isOwnedStruct();
  if (Owned && !T.getTypePtr()->getAs<TypedefType>()) {
    Diag(ErrLoc, diag::err_nested_owned_borrow_type_check)
        << ownedQualified << "_Owned" << Env;
  } else if (Owned && T.getTypePtr()->getAs<TypedefType>()) {
    Diag(ErrLoc, diag::err_nested_owned_borrow_type_check)
        << ownedTypedef << "_Owned" << Env << T;
  } else if (T->containsOwned(BSCLookThrough::NoPointer)) {
    Diag(ErrLoc, diag::err_nested_owned_borrow_type_check)
        << ownedFields << "_Owned" << Env << T;
  }
}

namespace {
bool IsOwnedRawPointerCastDisallowed(QualType LHSCanType, QualType RHSCanType) {
  const auto *LHSPtrType = LHSCanType->getAs<PointerType>();
  const auto *RHSPtrType = RHSCanType->getAs<PointerType>();
  if (!LHSPtrType || !RHSPtrType)
    return false;

  bool LHSOwned = LHSCanType.isOwnedPointer();
  bool RHSOwned = RHSCanType.isOwnedPointer();
  bool LHSRaw = LHSCanType.isRawPointer();
  bool RHSRaw = RHSCanType.isRawPointer();
  if ((LHSOwned && RHSRaw) || (RHSOwned && LHSRaw))
    return true;

  return false;
}
} // end anonymous namespace

static bool arrayElemCompatible(QualType Dst, QualType Src) {
  return Dst.getBSCPointerProperties().arrayElemMatches(
      Src.getBSCPointerProperties());
}


bool Sema::CheckOwnedQualTypeCStyleCast(QualType LHSType, QualType RHSType) {
  QualType RHSCanType = RHSType.getCanonicalType();
  QualType LHSCanType = LHSType.getCanonicalType();

  // Allow owned pointer to be cast from nullptr_t
  if (LHSCanType.isOwnedPointer() &&
      RHSCanType->isNullPtrType()) {
    return true;
  }

  bool IsSameType = (LHSCanType.getTypePtr() == RHSCanType.getTypePtr());
  const auto *LHSPtrType = LHSType->getAs<PointerType>();
  const auto *RHSPtrType = RHSType->getAs<PointerType>();
  bool IsPointer = LHSPtrType && RHSPtrType;

  if (RHSCanType->isDependentType()) {
    return true;
  }

  if (IsPointer) {
    bool LHSOwned = LHSCanType.isOwnedPointer();
    bool RHSOwned = RHSCanType.isOwnedPointer();
    bool LHSBorrow = LHSCanType.isBorrowQualified();
    bool RHSBorrow = RHSCanType.isBorrowQualified();
    bool LHSRaw = LHSCanType.isRawPointer();
    bool RHSRaw = RHSCanType.isRawPointer();
    // Disallow conversion between owned and borrow pointers
    if ((LHSBorrow && RHSOwned) || (LHSOwned && RHSBorrow)) {
      return false;
    }
    // Disallow conversion between owned and raw pointers
    if ((LHSRaw && RHSOwned) || (LHSOwned && RHSRaw)) {
      return false;
    }
    if (!arrayElemCompatible(LHSType, RHSType))
      return false;
    // Conversion between different raw pointers is allowed
    if (LHSRaw && RHSRaw) {
      return true;
    }
    // Allow conversion from/to void pointers
    // for conversion of owned->owned, we need to check
    // the inner pointer type recursively
    return LHSCanType.getTypePtr()->isVoidPointerType() ||
           RHSCanType.getTypePtr()->isVoidPointerType() ||
           CheckOwnedQualTypeCStyleCast(LHSPtrType->getPointeeType(),
                                        RHSPtrType->getPointeeType());
  }
  // owned pointer can be cast to integral type, but not the opposite
  if (LHSCanType->isIntegerType() && RHSCanType.isOwnedPointer()) {
    return true;
  }
  return IsSameType;
}

bool Sema::CheckOwnedQualTypeCStyleCast(QualType LHSType, QualType RHSType, SourceLocation RLoc) {
  if (!CheckOwnedQualTypeCStyleCast(LHSType, RHSType)) {
    QualType RHSCanType = RHSType.getCanonicalType();
    QualType LHSCanType = LHSType.getCanonicalType();
    if (IsOwnedRawPointerCastDisallowed(LHSCanType, RHSCanType)) {
      if (LHSType.isArrayElemQualified() || RHSType.isArrayElemQualified())
        Diag(RLoc, diag::err_owned_array_raw_cast_disallowed);
      else
        Diag(RLoc, diag::err_owned_raw_cast_disallowed);
    }
    else
      Diag(RLoc, diag::err_owned_qualcheck_incompatible) << RHSType << LHSType;
    return false;
  } else {
    return true;
  }
}

bool Sema::CheckOwnedQualTypeAssignment(QualType LHSType, QualType RHSType, SourceLocation RLoc) {
  QualType RHSCanType = RHSType.getCanonicalType();
  QualType LHSCanType = LHSType.getCanonicalType();
  const auto *LHSPtrType = LHSType->getAs<PointerType>();
  const auto *RHSPtrType = RHSType->getAs<PointerType>();
  bool IsPointer = LHSPtrType && RHSPtrType;
  bool IsSameType = (LHSCanType.getTypePtr() == RHSCanType.getTypePtr());
  bool IsTraitImplType = (LHSCanType->isTraitType() || RHSCanType->isTraitType());

  // _Bool <- T *_Owned // legal, doesn't consume ownership
  if (RHSCanType.isOwnedPointer() && LHSCanType->isBooleanType()) {
    return true;
  }

  // owned to owned cases:
  // int* owned  <->  int* owned   // legal
  // int* owned  <->  float* owned // illegal
  // const int** owned  <->  int** owned  // legal
  // trait T* owned <- [type : trait T] * owned // legal
  // unOwned to unOwned cases:
  // owned int* owned *  <->  owned int**  // illegal
  // owned int* const *  <->  owned int**  // legal
  // The decl is authoritative: one value type per owned struct.
  bool LHSOwned = LHSCanType.isOwnedPointerOrOwnedStruct();
  bool RHSOwned = RHSCanType.isOwnedPointerOrOwnedStruct();
  if (LHSOwned == RHSOwned ||
      (LHSCanType->isTraitType() && RHSCanType->isOwnedStruct())) {
    if (!arrayElemCompatible(LHSType, RHSType))
      return false;
    if (IsSameType) {
      return true;
    }
    if (IsTraitImplType) {
      return true;
    }
    if (!IsPointer) {
      return false;
    } else {
      // owned struct S* <-> void* //legal
      if (!LHSCanType.isOwnedPointer() &&
          (LHSPtrType->isVoidPointerType() || RHSPtrType->isVoidPointerType()))
        return true;
      return CheckOwnedQualTypeAssignment(LHSPtrType->getPointeeType(), RHSPtrType->getPointeeType(), RLoc);
    }
  }

  // trait T* owned <-> trait T* owned // legal
  if (LHSCanType.isOwnedPointerOrOwnedStruct() ||
      RHSCanType.isOwnedPointerOrOwnedStruct()) {
    TraitDecl *TD = TryDesugarTrait(LHSType);
    if (TD) {
      QualType QT = DesugarTraitToStructTrait(TD, LHSCanType, RLoc);
      if (QT.getCanonicalType() == RHSCanType) {
        return true;
      }
    }
  }

  // unOwned <-> owned
  // int* owned <-> int*  //illegal
  return false;
}

bool Sema::CheckOwnedQualTypeAssignment(QualType LHSType, Expr* RHSExpr) {
  QualType RHSCanType = RHSExpr->getType().getCanonicalType();
  QualType LHSCanType = LHSType.getCanonicalType();
  bool IsLiteral = false;
  Stmt::StmtClass RHSClass = RHSExpr->getStmtClass();
  if (RHSClass == Expr::IntegerLiteralClass
      || RHSClass == Expr::FloatingLiteralClass
      || RHSClass == Expr::CharacterLiteralClass) {
    IsLiteral = true;
  }
  SourceLocation ExprLoc = RHSExpr->getBeginLoc();
  // Owned pointer can be initialized by a null pointer constant
  if (LHSCanType.isOwnedPointer() &&
      RHSExpr->isNullPointerConstant(Context, Expr::NPC_ValueDependentIsNull))
    return true;

  bool Res = true;

  // unOwned to owned initialize cases:
  // int owned a = 10;        //legal even 10 is not owned type
  // int owned b = 10 + 10;   //ilegal
  // char owned c = 'c';      // legal even 'c' is int type
  // int owned d = (int)a;    // illegal
  if (LHSCanType.isOwnedPointerOrOwnedStruct() &&
      !RHSCanType.isOwnedPointerOrOwnedStruct() && IsLiteral) {
    if (LHSCanType.getTypePtr() != RHSCanType.getTypePtr()
        && !(LHSCanType.getTypePtr()->isCharType() && RHSCanType.getTypePtr()->isIntegerType())) {
      Res = false;
    }
  } else {
    Res = CheckOwnedQualTypeAssignment(LHSType, RHSCanType, ExprLoc);
  }
  return Res;
}

// False when a K&R side has no prototype to compare.
bool Sema::getBSCFunctionProtoPair(QualType LHSType, Expr *RHSExpr,
                                   const FunctionProtoType *&LHS,
                                   const FunctionProtoType *&RHS) {
  LHS = LHSType->getAs<PointerType>()
            ->getPointeeType()
            ->getAs<FunctionProtoType>();
  RHS = RHSExpr->getType()->isFunctionPointerType()
            ? RHSExpr->getType()
                  ->getAs<PointerType>()
                  ->getPointeeType()
                  ->getAs<FunctionProtoType>()
            : RHSExpr->getType()->getAs<FunctionProtoType>();
  return LHS && RHS;
}

Sema::AssignConvertType
Sema::CheckBSCQualTypeAssignment(QualType LHSType, ExprResult &RHS) {
  QualType LHSCan = LHSType.getCanonicalType();
  QualType RHSCan = RHS.get()->getType().getCanonicalType();

  bool MayHaveOwned = LHSCan.isOwnedPointerOrOwnedStruct() ||
                      RHSCan.isOwnedPointerOrOwnedStruct();
  bool MayHaveBorrow = LHSCan.isBorrowQualified() || RHSCan.isBorrowQualified();
  if (LHSCan->isPointerType() && RHSCan->isPointerType()) {
    MayHaveOwned = LHSCan.isOrContainsOwned(BSCLookThrough::AnyPointer) ||
                   RHSCan.isOrContainsOwned(BSCLookThrough::AnyPointer);
    MayHaveBorrow = LHSCan.isOrContainsBorrow(BSCLookThrough::AnyPointer) ||
                    RHSCan.isOrContainsBorrow(BSCLookThrough::AnyPointer);
  }

  // Check the destination's qualifier first for accurate diagnostics.
  if (LHSCan.isBorrowQualified()) {
    if (MayHaveBorrow && !CheckBorrowQualTypeAssignment(LHSType, RHS))
      return IncompatibleBorrowPointer;
    if (MayHaveOwned && !CheckOwnedQualTypeAssignment(LHSType, RHS.get()))
      return IncompatibleOwnedPointer;
  } else {
    if (MayHaveOwned && !CheckOwnedQualTypeAssignment(LHSType, RHS.get()))
      return IncompatibleOwnedPointer;
    if (MayHaveBorrow && !CheckBorrowQualTypeAssignment(LHSType, RHS))
      return IncompatibleBorrowPointer;
  }
  return Compatible;
}

Sema::AssignConvertType
Sema::CheckBSCFunctionPointerType(QualType LHSType, Expr *RHSExpr) {
  const FunctionProtoType *LHS, *RHS;
  if (!getBSCFunctionProtoPair(LHSType, RHSExpr, LHS, RHS))
    return Compatible;
  // A mixed _Safe/_Unsafe source resolves to the redeclaration that fits.
  if (FunctionDecl *FD = SelectFunctionDeclForPointerAssignment(RHSExpr, LHS)) {
    RHS = FD->getType()->getAs<FunctionProtoType>();
    if (!RHS)
      return Compatible;
  }
  switch (firstBSCFunctionTypeMismatch(Context, LHS, RHS)) {
  case BSCFunctionMismatch::None:
    return Compatible;
  case BSCFunctionMismatch::Owned:
    return IncompatibleOwnedPointer;
  case BSCFunctionMismatch::Borrow:
    return IncompatibleBorrowPointer;
  case BSCFunctionMismatch::Incompatible:
    return IncompatibleFunctionPointer;
  }
  llvm_unreachable("bad BSCFunctionMismatch");
}

bool Sema::CheckTemporaryVarMemoryLeak(Expr* E) {
  if (E == nullptr)
    return false;
  if (isUnevaluatedContext())
    return false;
  E = E->IgnoreParenCastsSafe();
  if (auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_LNot)
      return CheckTemporaryVarMemoryLeak(UO->getSubExpr());
  }
  if (auto *BO = dyn_cast<BinaryOperator>(E)) {
    // ActOnBinOp checks the comma LHS, which is always discarded. If the
    // comma expression itself is discarded, only its result-producing RHS
    // needs an additional check here.
    if (BO->getOpcode() == BO_Comma)
      return CheckTemporaryVarMemoryLeak(BO->getRHS());
  }
  if (auto *CO = dyn_cast<AbstractConditionalOperator>(E)) {
    // BinaryConditionalOperator (GNU `x ?: y`) reuses the common expression,
    // exposed via getCommon() rather than the OpaqueValueExpr getTrueExpr().
    Expr *TrueExpr = isa<BinaryConditionalOperator>(CO)
                         ? cast<BinaryConditionalOperator>(CO)->getCommon()
                         : CO->getTrueExpr();
    bool LeakCond = CheckTemporaryVarMemoryLeak(CO->getCond());
    bool LeakTrue = CheckTemporaryVarMemoryLeak(TrueExpr);
    bool LeakFalse = CheckTemporaryVarMemoryLeak(CO->getFalseExpr());
    return LeakCond || LeakTrue || LeakFalse;
  }
  if (auto *SE = dyn_cast<StmtExpr>(E)) {
    // A discarded StmtExpr discards its trailing expression's value; the
    // trailing statement itself skips this check (see ActOnExprStmt).
    const CompoundStmt *CS = SE->getSubStmt();
    if (CS->body_empty())
      return false;
    if (const ValueStmt *VS =
            dyn_cast<ValueStmt>(CS->getStmtExprResult()))
      if (const Expr *Tail = VS->getExprStmt())
        return CheckTemporaryVarMemoryLeak(
            const_cast<Expr *>(Tail));
    return false;
  }
  if (!isa<CallExpr>(E) && !isa<CompoundLiteralExpr>(E))
    return false;
  QualType RetType = E->getType().getCanonicalType();
  if (RetType.isOrContainsOwned(BSCLookThrough::NoPointer)) {
    std::string ExprString;
    llvm::raw_string_ostream ExprStream(ExprString);
    E->printPretty(ExprStream, nullptr, clang::PrintingPolicy(getLangOpts()));
    Diag(E->getBeginLoc(), diag::err_owned_temporary_memLeak) << ExprStream.str();
    return true;
  }
  return false;
}

// True when E is an object/array expression rooted at a `_Borrow` (or
// `_Borrow _ArrayElem`) pointer, e.g. `a->p`, `arr[i].a`, `(*a).p`,
// `(*(a + 1)).p` and `((c ? a : b)[0]).p`. Only the three expression kinds
// that produce an object/array from a pointer are peeled (MemberExpr,
// ArraySubscriptExpr and UnaryOperator(Deref)); composite pointer operands
// are decided by their type alone. A non-borrow (raw) pointer base/operand
// stops the walk: an object reached through `**a` or `(*a)->p` (where `a` is
// `struct S ** _Borrow _ArrayElem`) is behind an untracked pointer and is not
// an element of the borrowed array. When RootDecl is provided and the
// expression has a single DeclRefExpr root, it receives that declaration (for
// diagnostics); composite roots leave it null.
static bool IsBorrowRoot(const Expr *E, const VarDecl **RootDecl = nullptr) {
  if (!E)
    return false;
  if (RootDecl)
    *RootDecl = nullptr;
  E = E->IgnoreParenImpCastsSafe();
  // Try to fill RootDecl from a borrow-qualified pointer expression when it is
  // a plain variable; composite pointer expressions have no single root.
  auto NoteRootIfSimple = [&](const Expr *PtrExpr) {
    if (RootDecl)
      IsBorrowRoot(PtrExpr, RootDecl);
  };

  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    const Expr *Base = ME->getBase()->IgnoreParenImpCastsSafe();
    QualType BaseTy = Base->getType();
    if (BaseTy->isPointerType()) {
      // `->`: the pointer base itself decides. Raw pointer bases (including
      // `(&a[0])->p` and `(*a)->p`) are intentionally not tracked.
      if (!BaseTy.isBorrowPointer())
        return false;
      NoteRootIfSimple(Base);
      return true;
    }
    // `.`: the object is a struct lvalue; its provenance comes from the base
    // expression (`a[0].f`, `(*a).f`). Composite expressions that produce the
    // object are followed on their result arms: `(c ? *a : *b).p` may take
    // ownership from either borrowed arm, and `(0, *a).p` takes it from the
    // comma's RHS. Such composite bases have no single root, so RootDecl is
    // intentionally left null.
    if (const auto *BO = dyn_cast<BinaryOperator>(Base)) {
      if (BO->getOpcode() == BO_Comma)
        return IsBorrowRoot(BO->getRHS());
    } else if (const auto *ACO = dyn_cast<AbstractConditionalOperator>(Base)) {
      if (IsBorrowRoot(ACO->getTrueExpr()) ||
          IsBorrowRoot(ACO->getFalseExpr()))
        return true;
      return false;
    }
    return IsBorrowRoot(Base, RootDecl);
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    const Expr *Base = ASE->getBase()->IgnoreParenImpCastsSafe();
    QualType BaseTy = Base->getType();
    if (BaseTy->isPointerType()) {
      // `p[i]`: the pointer base decides (composite pointer expressions are
      // covered by the type check).
      if (!BaseTy.isBorrowPointer())
        return false;
      NoteRootIfSimple(Base);
      return true;
    }
    // The base is an array lvalue (e.g. a member array of a struct element);
    // recurse into the expression that produced it.
    return IsBorrowRoot(Base, RootDecl);
  }

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_Deref) {
      const Expr *Operand = UO->getSubExpr()->IgnoreParenImpCastsSafe();
      if (!Operand->getType().isBorrowPointer())
        return false;
      NoteRootIfSimple(Operand);
      return true;
    }
    return false;
  }

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    QualType Ty = DRE->getType();
    if (Ty.isBorrowPointer()) {
      if (RootDecl)
        *RootDecl = dyn_cast<VarDecl>(DRE->getDecl());
      return true;
    }
  }
  return false;
}

void Sema::CheckMoveFromBorrow(Expr* E, SourceLocation SL) {
  if (E == nullptr)
    return;
  if (isUnevaluatedContext())
    return;
  E = E->IgnoreParenCastsSafe();
  // Recurse through result-producing paths of composite expressions so that
  // `T *_Owned tmp = (*p1, *p2)`, `q ?: null`, and `cond ? *p1 : *p2` don't
  // silently skip the check. Each arm is reported at its own location so that
  // `c ? a[0] : a[1]` yields one diagnostic per offending branch instead of
  // two identical diagnostics at the enclosing expression's location.
  if (auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_Comma) {
      // Only the RHS is the runtime-taken ownership move (LHS is discarded
      // without consuming ownership, matching `(void)*bp`-style discards).
      CheckMoveFromBorrow(BO->getRHS(), BO->getRHS()->getExprLoc());
      return;
    }
  } else if (auto *BCO = dyn_cast<BinaryConditionalOperator>(E)) {
    CheckMoveFromBorrow(BCO->getCommon(), BCO->getCommon()->getExprLoc());
    CheckMoveFromBorrow(BCO->getFalseExpr(), BCO->getFalseExpr()->getExprLoc());
    return;
  } else if (auto *CO = dyn_cast<ConditionalOperator>(E)) {
    CheckMoveFromBorrow(CO->getTrueExpr(), CO->getTrueExpr()->getExprLoc());
    CheckMoveFromBorrow(CO->getFalseExpr(), CO->getFalseExpr()->getExprLoc());
    return;
  }
  auto IsOwnedConsuming = [](QualType T) {
    return T.isOrContainsOwned(BSCLookThrough::NoPointer);
  };
  // Emit the move-through-borrow error and, when the offending value is rooted
  // at an array formal that was implicitly adjusted to `_Borrow _ArrayElem`,
  // add a note pointing at that parameter.
  auto DiagMoveBorrow = [&](const Expr *MoveExpr) {
    Diag(SL, diag::err_move_borrow);
    const VarDecl *Root = nullptr;
    if (IsBorrowRoot(MoveExpr, &Root)) {
      if (const auto *Parm = dyn_cast_or_null<ParmVarDecl>(Root)) {
        if (const TypeSourceInfo *TSI = Parm->getTypeSourceInfo()) {
          QualType WrittenTy = TSI->getType();
          if (WrittenTy->isArrayType() &&
              Parm->getType().isBorrowPointer() &&
              Parm->getType().isArrayElemQualified())
            Diag(Parm->getLocation(),
                 diag::note_array_param_adjusted_borrow_arrayelem)
                << Parm->getName();
        }
      }
    }
  };
  // Ownership must not leave through any `_Borrow` pointer, whether plain
  // `_Borrow` or `_Borrow _ArrayElem`. The direct borrow-qualified checks
  // cover `*p`, `p->f` and `a[i]`; `IsBorrowRoot` additionally covers
  // deref-then-member forms (`(*a).p`, `(*(a + 1)).p`,
  // `(c ? *a : *b).p`, `a[0].arr[0].p`). Forms that first go through a raw
  // pointer (e.g. `*(&a[1])`, `(&a[0])->p`, `**a`) are intentionally not
  // tracked.
  if (auto *UO = dyn_cast_or_null<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_Deref &&
        IsOwnedConsuming(UO->getType()) &&
        UO->getSubExpr()->getType().isBorrowQualified())
      DiagMoveBorrow(UO);
  } else if (auto *ME = dyn_cast_or_null<MemberExpr>(E)) {
    if (IsOwnedConsuming(ME->getType()) &&
        (ME->getBase()->getType().isBorrowQualified() ||
         IsBorrowRoot(ME)))
      DiagMoveBorrow(ME);
  } else if (auto *ASE =
                 dyn_cast_or_null<ArraySubscriptExpr>(E)) {
    if (IsOwnedConsuming(ASE->getType()) &&
        (ASE->getBase()->getType().isBorrowQualified() ||
         IsBorrowRoot(ASE)))
      DiagMoveBorrow(ASE);
  }
}

namespace {
/// Returns true if casting from RHS to LHS would cast away const qualifiers
/// (i.e. RHS has const that LHS doesn't at some level).
bool isCastingAwayConst(QualType LHS, QualType RHS) {
  if (LHS->isPointerType() && RHS->isPointerType()) {
    const auto *LHSPtr = LHS->getAs<PointerType>();
    const auto *RHSPtr = RHS->getAs<PointerType>();
    if (LHSPtr && RHSPtr)
      return isCastingAwayConst(LHSPtr->getPointeeType(), RHSPtr->getPointeeType());
  }
  // For non-pointer types: casting away const means RHS has const that LHS doesn't
  return RHS.isConstQualified() && !LHS.isConstQualified();
}
} // namespace

bool Sema::CheckBorrowQualTypeCStyleCast(QualType LHSType, QualType RHSType) {
  // Keep Owned/Borrow/ArrayElem, drop CVR, ignore nullability (checked
  // separately by the nullability checker).
  LHSType = Context.getTypeWithoutCVRAndNullability(LHSType);
  RHSType = Context.getTypeWithoutCVRAndNullability(RHSType);

  QualType RHSCanType = RHSType.getCanonicalType();
  QualType LHSCanType = LHSType.getCanonicalType();

  // Allow borrow pointer to be cast from nullptr_t
  if (LHSCanType.isBorrowPointer() &&
      RHSCanType->isNullPtrType()) {
    return true;
  }

  // Defer borrow compatibility checks for dependent types to instantiation,
  // where concrete types are available for a precise check.
  if (RHSCanType->isDependentType() || LHSCanType->isDependentType()) {
    return true;
  }

  if (Context.hasSameType(LHSType, RHSType)) {
    return true;
  }
  const auto *LHSPtrType = LHSType->getAs<PointerType>();
  const auto *RHSPtrType = RHSType->getAs<PointerType>();
  bool IsPointer = LHSPtrType && RHSPtrType;
  bool IsUnqualifiedTypeMatch = Context.hasSameUnqualifiedType(LHSCanType, RHSCanType);

  if (LHSCanType->isIntegerType() && RHSCanType.isBorrowPointer()) {
    return true;
  }
  if (!IsPointer)
    return IsUnqualifiedTypeMatch;
  // Check borrow qualifier compatibility first - prevent casting between
  // mutable and const borrows to avoid aliasing (must run before hasSameType
  // which may treat types as equivalent)
  if (RHSCanType.isBorrowPointer() && LHSCanType.isBorrowPointer() &&
      (RHSCanType.isConstBorrow() != LHSCanType.isConstBorrow()))
    return false;
  if (LHSCanType->isVoidPointerType())
    return true;
  if (RHSCanType->isVoidPointerType() && !IsInEvaluatedSafeZone())
    return true;
  if (RHSCanType.isBorrowPointer() && LHSCanType.isBorrowPointer() &&
      (RHSType.isArrayElemQualified() != LHSType.isArrayElemQualified())) {
    if (!LHSType.isArrayElemQualified() && RHSType.isArrayElemQualified()) {
      QualType LHSPointee = LHSCanType->getPointeeType();
      QualType RHSPointee = RHSCanType->getPointeeType();
      if (Context.hasSameType(LHSPointee, RHSPointee))
        return true;
    }
    return false;
  }
  if (IsUnqualifiedTypeMatch)
    return true;
  if (TryDesugarTrait(RHSType))
    return true;

  // Reject casting away const (e.g. const int *const * -> int *const *)
  QualType LHSPointee = LHSPtrType->getPointeeType();
  QualType RHSPointee = RHSPtrType->getPointeeType();
  if (isCastingAwayConst(LHSPointee, RHSPointee))
    return false;
  return CheckBorrowQualTypeCStyleCast(LHSPointee, RHSPointee);
}

bool Sema::CheckBorrowQualTypeCStyleCast(QualType LHSType, QualType RHSType, SourceLocation RLoc) {
  if (!CheckBorrowQualTypeCStyleCast(LHSType, RHSType)) {
    Diag(RLoc, diag::err_borrow_qualcheck_incompatible) << CompleteTraitType(RHSType) << CompleteTraitType(LHSType);
    return false;
  } else {
    return true;
  }
}

bool Sema::CheckBorrowQualTypeAssignment(QualType LHSType, QualType RHSType, SourceLocation RLoc) {
  // Keep Owned/Borrow/ArrayElem, drop CVR, ignore nullability (checked
  // separately by the nullability checker).
  LHSType = Context.getTypeWithoutCVRAndNullability(LHSType);
  RHSType = Context.getTypeWithoutCVRAndNullability(RHSType);

  QualType RHSCanType = RHSType.getCanonicalType();
  QualType LHSCanType = LHSType.getCanonicalType();
  const auto *LHSPtrType = LHSType->getAs<PointerType>();
  const auto *RHSPtrType = RHSType->getAs<PointerType>();
  bool IsPointer = LHSPtrType && RHSPtrType;

  if (LHSCanType.isBorrowQualified() == RHSCanType.isBorrowQualified()) {
    if (!arrayElemCompatible(LHSType, RHSType))
      return false;
    if (TraitDecl *TD = TryDesugarTrait(LHSCanType)) {
      if (TD->getTypeImpledVarDecl(RHSCanType->getPointeeType()))
        return true;
    }
    if (Context.hasSameType(LHSCanType, RHSCanType)) {
      return true;
    }
    if (LHSCanType->isVoidPointerType()) {
      if (LHSCanType->getPointeeType().isConstQualified() == RHSCanType->getPointeeType().isConstQualified())
        return true;
    }
    if (!IsPointer) {
      return false;
    } else {
      return CheckBorrowQualTypeAssignment(LHSPtrType->getPointeeType(), RHSPtrType->getPointeeType(), RLoc);
    }
  }

  return false;
}

// An implicit reborrow keeps the source's _ArrayElem; a written `&_X *p` does
// not (manual 3.2.1.2).
static ExprResult CreateImplicitReborrow(Sema &S, SourceLocation Loc,
                                         UnaryOperatorKind Opc, Expr *Src) {
  ExprResult R = S.CreateBuiltinUnaryOp(Loc, Opc, Src);
  if (R.isInvalid() || !Src->getType().isArrayElemQualified())
    return R;
  Expr *E = R.get();
  BSCPointerProperties P = E->getType().getBSCPointerProperties();
  P.ArrayElem = true;
  E->setType(S.Context.getTypeWithBSCProperties(E->getType(), P));
  return R;
}

bool Sema::CheckBorrowQualTypeAssignment(QualType LHSType, ExprResult &RHS) {
  Expr *RHSExpr = RHS.get();
  QualType RHSCanType = Context.getTypeWithoutCVRAndNullability(
      RHSExpr->getType().getCanonicalType());
  QualType LHSCanType =
      Context.getTypeWithoutCVRAndNullability(LHSType.getCanonicalType());

  SourceLocation ExprLoc = RHSExpr->getBeginLoc();
  bool Res = true;

  // Defer borrow compatibility checks for dependent types to instantiation,
  // where concrete types are available for a precise check.
  if (RHSCanType->isDependentType() || LHSCanType->isDependentType())
    return true;

  if (LHSCanType.isBorrowQualified() || RHSCanType.isBorrowQualified()) {
    if (TraitDecl *TD = TryDesugarTrait(LHSCanType)) {
      if (RHSCanType->isPointerType()) {
        QualType ImplType = Context.getTypeWithoutBSCProperties(
            RHSCanType->getPointeeType().getUnqualifiedType()
                .getCanonicalType());
        if (TD->getTypeImpledVarDecl(ImplType))
          return true;
      }
      // trait T* borrow <-> trait T* borrow // legal
      if (TD) {
        QualType QT = DesugarTraitToStructTrait(TD, LHSCanType, RHSExpr->getExprLoc());
        if (QT.getCanonicalType() == RHSCanType) {
          return true;
        }
      }
    }

    // Borrow pointer can be initialized by a null pointer constant
    if (LHSCanType.isBorrowPointer() &&
        RHSExpr->isNullPointerConstant(Context, Expr::NPC_ValueDependentIsNull))
      return true;

    if (LHSCanType->isVoidPointerType()) {
      if (RHSCanType->isArrayType()) {
        if (LHSCanType.isBorrowQualified()) {
          QualType RHSElementType =
              RHSCanType->getAsArrayTypeUnsafe()->getElementType();
          if (!(RHSElementType.isConstQualified() &&
              !LHSCanType->getPointeeType().isConstQualified())) {
            // this conversion does not drop const on the element type
            // unsafe zone: allow conversion unconditionally;
            // safe zone: require trivial element type
            if (!IsInEvaluatedSafeZone() || RHSElementType->isTrivialDataType())
              return true;
          }
          Res = false;
        } else {
          // Check const compatibility between void* and array element type.
          QualType RHSElementType = RHSCanType->getAsArrayTypeUnsafe()->getElementType();
          if (LHSCanType->getPointeeType().isConstQualified() == RHSElementType.isConstQualified())
            return true;
        }
      } else if (RHSCanType->isPointerType()) {
        // Pointer-to-pointer: check const and borrow qualifiers.
        if (LHSCanType->getPointeeType().isConstQualified() == RHSCanType->getPointeeType().isConstQualified() &&
            LHSCanType.isBorrowQualified() == RHSCanType.isBorrowQualified())
          return true;
      }
    }

    // Allow mutable borrow downgrading to immutable borrow (re-borrow)
    // Allow `_Borrow _ArrayElem` to downgrade to plain `_Borrow` as well
    if (LHSCanType.isBorrowPointer() &&
        RHSCanType.isBorrowPointer()) {
      if (!LHSType.isArrayElemQualified() &&
          RHSExpr->getType().isArrayElemQualified()) {
        QualType LHSPointee = LHSCanType->getPointeeType();
        QualType RHSPointee = RHSCanType->getPointeeType();
        if (Context.hasSameType(LHSPointee, RHSPointee)) {
          return true;
        }
      }
      QualType LHSPointee = LHSCanType->getPointeeType();
      QualType RHSPointee = RHSCanType->getPointeeType();
      if (LHSPointee.isConstQualified() && !RHSPointee.isConstQualified()) {
        LHSPointee.removeLocalConst();
        if ((LHSPointee == RHSPointee) || // T*_Borrow -> const T*_Borrow
            (LHSPointee->isVoidType() &&  // T*_Borrow -> const void*_Borrow
                (!IsInEvaluatedSafeZone() || RHSPointee->isTrivialDataType()))) {
          ExprResult ReBorrowExpr = CreateImplicitReborrow(
              *this, ExprLoc, UO_AddrConstDeref, RHSExpr);
          if (!ReBorrowExpr.isInvalid()) {
            RHS = ReBorrowExpr;
            return true;
          }
          Res = false;
        }
      }
    }

    if (!Context.hasSameType(LHSCanType, RHSCanType))
      Res = false;

    // _Bool <- T *_Borrow is allowed
    if (RHSCanType.isBorrowPointer() &&
        LHSCanType->isBooleanType()) {
      Res = true;
    }
  } else {
    Res = CheckBorrowQualTypeAssignment(LHSType, RHSCanType, ExprLoc);
  }
  return Res;
}

ExprResult Sema::MaybeCreateImplicitMutableReborrow(QualType DestType,
                                                    Expr *Source) {
  QualType SrcType = Source->getType();
  if (!DestType.isBorrowPointer() || DestType.isConstBorrow() ||
      !SrcType.isBorrowPointer() || SrcType.isConstBorrow()) {
    return Source;
  }

  // Explicit borrow operators already carry the reborrow represented by this
  // conversion. Look through syntax-only wrappers to avoid nesting another
  // implicit &_Mut * around them.
  Expr *Core = Source->IgnoreParenImpCastsSafe();
  const auto *UO = dyn_cast<UnaryOperator>(Core);
  bool HasExplicitReborrow =
      UO && (UO->getOpcode() == UO_AddrMut ||
             UO->getOpcode() == UO_AddrMutDeref);
  if (HasExplicitReborrow)
    return Source;

  return CreateImplicitReborrow(*this, Source->getExprLoc(), UO_AddrMutDeref,
                                Source);
}

bool Sema::CheckBorrowQualTypeCompare(QualType LHSType, QualType RHSType) {
  QualType RHSCanType = RHSType.getCanonicalType();
  QualType LHSCanType = LHSType.getCanonicalType();
  bool LHSBorrow = LHSCanType.isBorrowQualified();
  bool RHSBorrow = RHSCanType.isBorrowQualified();
  if (LHSBorrow != RHSBorrow)
    return false;
  if (!LHSBorrow)
    return true;

  return LHSCanType->getPointeeType().getUnqualifiedType() ==
         RHSCanType->getPointeeType().getUnqualifiedType();
}

bool Sema::CheckBorrowFunctionType(QualType ReturnTy,
                                   ArrayRef<QualType> ParamTys,
                                   SourceLocation SL) {
  if (ReturnTy->isDependentType()) {
    return true;
  }
  if (ComputeNumRegions(Context, ReturnTy) > 1) {
    Diag(SL, diag::err_typecheck_multi_level_borrow_func);
    return false;
  }
  if (ReturnTy.isOrContainsBorrow(BSCLookThrough::AnyPointer)) {
    bool HasBorrowParam = false;
    for (QualType PT : ParamTys) {
      if (PT->isDependentType()) {
        return true;
      }
      if (PT.isOrContainsBorrow(BSCLookThrough::AnyPointer)) {
        HasBorrowParam = true;
        break;
      }
    }
    if (!HasBorrowParam) {
      Diag(SL, diag::err_typecheck_borrow_func);
      return false;
    }
  }
  return true;
}

bool Sema::CheckEnsureInitFunctionPointerType(
    const FunctionProtoType *LHSFuncType, const FunctionProtoType *RHSFuncType,
    SourceLocation Loc) {
  if (LHSFuncType->getNumParams() != RHSFuncType->getNumParams())
    return true; // Param count mismatch handled elsewhere

  // Asymmetric compat: target having the attribute is a contract callers
  // rely on, so source must carry it too. Reverse direction (source has
  // more, target has less) silently weakens and is allowed.
  for (unsigned I = 0; I < LHSFuncType->getNumParams(); ++I) {
    FunctionProtoType::ExtParameterInfo LHSExt =
        LHSFuncType->hasExtParameterInfos()
            ? LHSFuncType->getExtParameterInfo(I)
            : FunctionProtoType::ExtParameterInfo();
    FunctionProtoType::ExtParameterInfo RHSExt =
        RHSFuncType->hasExtParameterInfos()
            ? RHSFuncType->getExtParameterInfo(I)
            : FunctionProtoType::ExtParameterInfo();

    if (LHSExt.isEnsureInit() && !RHSExt.isEnsureInit()) {
      Diag(Loc,
           diag::err_ensure_init_funcptr_incompatible)
          << I;
      return false;
    }

    if (LHSExt.isEnsureInitIfRet()) {
      if (!RHSExt.isEnsureInitIfRet()) {
        Diag(Loc,
             diag::err_ensure_init_if_ret_funcptr_missing)
            << I;
        return false;
      }
      if (LHSExt.getEnsureInitIfRetCondValue() !=
          RHSExt.getEnsureInitIfRetCondValue()) {
        Diag(Loc,
             diag::err_ensure_init_if_ret_funcptr_cond_mismatch)
            << LHSExt.getEnsureInitIfRetCondValue() << I
            << RHSExt.getEnsureInitIfRetCondValue();
        return false;
      }
    }
  }
  return true;
}

// for global borrow variable type check
void Sema::CheckBorrowOrIndirectBorrowType(SourceLocation ErrLoc, QualType T,
                                           StringRef Env) {
  enum { BorrowQualified, BorrowTypedef, BorrowFields };
  if (T.isBorrowQualified() && !T.getTypePtr()->getAs<TypedefType>()) {
    Diag(ErrLoc, diag::err_nested_owned_borrow_type_check)
        << BorrowQualified << "_Borrow" << Env;
  } else if (T.isBorrowQualified() && T.getTypePtr()->getAs<TypedefType>()) {
    Diag(ErrLoc, diag::err_nested_owned_borrow_type_check)
        << BorrowTypedef << "_Borrow" << Env << T;
  } else if (T->containsBorrow(BSCLookThrough::AnyPointer)) {
    Diag(ErrLoc, diag::err_nested_owned_borrow_type_check)
        << BorrowFields << "_Borrow" << Env << T;
  }
}
#endif
