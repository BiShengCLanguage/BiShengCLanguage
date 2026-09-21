//===- ExprBSC.cpp - BSC Expression AST Node Implementation --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the BSC related Expr classes.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/AST/BSC/ExprBSC.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/Linkage.h"
#include "clang/Basic/Builtins.h"

using namespace clang;

/// If current expr is equal to 0, return true.
/// Such as : nullptr, 0, (void*)0, ((void*)0), (int*)0,
///           (int* borrow)(void*)0, (int* owned)(void*)0,
/// also include constant value which equals to 0.
bool Expr::isNullExpr(ASTContext &Ctx) const {
  if (getType()->isNullPtrType()) {
    return true;
  } else if (const IntegerLiteral *IL = dyn_cast<IntegerLiteral>(this)) {
    if (IL->getValue().getZExtValue() == 0)
      return true;
  } else if (const CStyleCastExpr *CSCE = dyn_cast<CStyleCastExpr>(this)) {
    return CSCE->getSubExpr()->isNullExpr(Ctx);
  } else if (const ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(this)) {
    return ICE->getSubExpr()->isNullExpr(Ctx);
  } else if (const ParenExpr *PE = dyn_cast<ParenExpr>(this)) {
    return PE->getSubExpr()->isNullExpr(Ctx);
  } else if (const SafeExpr *SE = dyn_cast<SafeExpr>(this)) {
    return SE->getSubExpr()->isNullExpr(Ctx);
  } else if (const CallExpr *CE = dyn_cast<CallExpr>(this)) {
    if (const FunctionDecl *FD = CE->getDirectCallee()) {
      if (CE->getNumArgs() == 1) {
        auto BuiltinID = FD->getBuiltinID();
        if (BuiltinID == Builtin::BI__take_from_raw ||
            BuiltinID == Builtin::BI__move_to_raw ||
            BuiltinID == Builtin::BI__take_array_from_raw ||
            BuiltinID == Builtin::BI__move_array_to_raw) {
          return CE->getArg(0)->isNullExpr(Ctx);
        }
      }
    }
  } else if (Optional<llvm::APSInt> I = this->getIntegerConstantExpr(Ctx)) {
    if (*I == 0)
      return true;
  }
  return false;
}

/// Whether this expression matches the trackable grammar:
///   trackable_expr ::= identifier
///                    | (trackable_expr)
///                    | trackable_expr . identifier
///                    | trackable_expr -> identifier
///                    | * trackable_expr
bool Expr::isNullabilityTrackableExpr() const {
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(this)) {
    return isa<VarDecl>(DRE->getDecl());
  } else if (const ParenExpr *PE = dyn_cast<ParenExpr>(this)) {
    return PE->getSubExpr()->isNullabilityTrackableExpr();
  } else if (const MemberExpr *ME = dyn_cast<MemberExpr>(this)) {
    if (!isa<FieldDecl>(ME->getMemberDecl()))
      return false;
    return ME->getBase()->isNullabilityTrackableExpr();
  } else if (const ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(this)) {
    return ICE->getSubExpr()->isNullabilityTrackableExpr();
  } else if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(this)) {
    if (UO->getOpcode() == UO_Deref)
      return UO->getSubExpr()->isNullabilityTrackableExpr();
  }
  return false;
}

/// Whether this expression matches the __forget trackable grammar: the
/// nullability trackable grammar plus array subscripts:
///   trackable_expr ::= identifier
///                    | (trackable_expr)
///                    | trackable_expr . identifier
///                    | trackable_expr -> identifier
///                    | * trackable_expr
///                    | trackable_expr [ expr ]
bool Expr::isForgetTrackableExpr() const {
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(this)) {
    return isa<VarDecl>(DRE->getDecl());
  } else if (const ParenExpr *PE = dyn_cast<ParenExpr>(this)) {
    return PE->getSubExpr()->isForgetTrackableExpr();
  } else if (const MemberExpr *ME = dyn_cast<MemberExpr>(this)) {
    if (!isa<FieldDecl>(ME->getMemberDecl()))
      return false;
    return ME->getBase()->isForgetTrackableExpr();
  } else if (const ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(this)) {
    return ICE->getSubExpr()->isForgetTrackableExpr();
  } else if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(this)) {
    if (UO->getOpcode() == UO_Deref)
      return UO->getSubExpr()->isForgetTrackableExpr();
  } else if (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(this)) {
    return ASE->getBase()->isForgetTrackableExpr();
  }
  return false;
}
#endif // ENABLE_BSC