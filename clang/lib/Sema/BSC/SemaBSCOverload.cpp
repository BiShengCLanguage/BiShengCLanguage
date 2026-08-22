#if ENABLE_BSC
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaDiagnostic.h"

using namespace clang;
using namespace sema;

OverloadedOperatorKind Sema::getOperatorKindByDeclarator(Declarator &D) {
  for (ParsedAttr &AL : D.getMutableDeclSpec().getAttributes()) {
    if (AL.getKind() == ParsedAttr::AT_Operator) {
      return AL.getOperatorTypeBuffer().Kind;
    }
  }
  return OO_None;
}

bool Sema::IsDesugarNeededOperatorKind(OverloadedOperatorKind Op) {
  if (Op == OO_Star || Op == OO_Arrow || Op == OO_Subscript) {
    return true;
  }
  return false;
}

// For the operators *, ->, and [], the first input parameter of the overloaded
// function must be of pointer type. Through this function,
// we take the address of the first argument of the overloading to match the
// overloaded function.
Expr *Sema::DesugarOperatorFirstArg(FunctionDecl *FD, ArrayRef<Expr *> Args) {
  if (const auto *FPT = FD->getType()->castAs<FunctionProtoType>()) {
    assert(FPT->getNumParams() != 0 && "The function must have at least one parameter.");
    QualType FirstParamTy = FPT->getParamType(0);
    if (FirstParamTy->isPointerType() && !Args[0]->getType()->isPointerType()) {
      QualType QT = Args[0]->getType();
      Expr *argExpr;
      UnaryOperator::Opcode UO = UO_AddrOf;
      if (FirstParamTy.isConstBorrow()) {
        QT.addConst();
        UO = UO_AddrConst;
      }
      QT = Context.getPointerType(QT);
      if (FirstParamTy.isBorrowQualified()) {
        QT.addBorrow();
        UO = (UO == UO_AddrConst ? UO_AddrConst : UO_AddrMut);
      }
      argExpr = UnaryOperator::Create(Context, Args[0], UO, QT, VK_PRValue,
                                      OK_Ordinary, Args[0]->getExprLoc(), false,
                                      CurFPFeatureOverrides());
      return argExpr;
    }
  }
  return Args[0];
}

// For the overloading of the * and [] operators, if we have already taken the
// address of the input parameter, we then need to dereference the function
// result. This is to ensure that the return value meets the expectations.
Expr *Sema::DesugarOperatorCallExpr(FunctionDecl *FD, Expr *TheCall) {
  QualType ReturnType = FD->getReturnType();
  if (ReturnType->isPointerType()) {
    Expr *argExpr =
        CreateBuiltinUnaryOp(TheCall->getExprLoc(), UO_Deref, TheCall).get();
    ParenExpr *PE = new (Context)
        ParenExpr(TheCall->getExprLoc(), TheCall->getExprLoc(), argExpr);
    return PE;
  }

  return TheCall;
}

bool Sema::CheckComparisonKindOperatorFunReturnType(FunctionDecl *FnDecl) {
  OverloadedOperatorKind Op = FnDecl->getOverloadedOperator();
  DefaultedComparisonKind DCK = DefaultedComparisonKind::None;
  switch (Op) {
  case OO_Less:
  case OO_Greater:
  case OO_LessEqual:
  case OO_GreaterEqual:
    DCK = DefaultedComparisonKind::Relational;
    break;
  case OO_EqualEqual:
    DCK = DefaultedComparisonKind::Equal;
    break;
  case OO_ExclaimEqual:
    DCK = DefaultedComparisonKind::Equal;
    break;

  default:
    break;
  }
  if (DCK != DefaultedComparisonKind::None &&
      !FnDecl->getReturnType()->isBooleanType()) {
    Diag(FnDecl->getLocation(),
         diag::err_defaulted_comparison_return_type_not_bool)
        << (int)DCK << FnDecl->getDeclaredReturnType() << Context.BoolTy
        << FnDecl->getReturnTypeSourceRange();
    return true;
  }
  return false;
}

bool isRcPointerType(QualType QT) {
  if (QT->isRecordType()) {
    const RecordType *RT = QT->getAs<RecordType>();
    if (RT && RT->getDecl()->getDeclName().getAsString() == "Rc") {
      return true;
    }
  }
  if (QT->isDependentType()) {
    if (const auto *TST = QT->getAs<TemplateSpecializationType>()) {
      if (TST->getTemplateName().getAsTemplateDecl()->getNameAsString() ==
          "Rc") {
        return true;
      }
    }
  }
  return false;
}
// check whether the parameters and return values of the overloaded function are
// non-compliance. If they are non-compliance, return true; if they are
// compliance, return false.
bool Sema::CheckBSCOverloadedOperatorDeclaration(FunctionDecl *FnDecl) {
  unsigned NumParams = FnDecl->getNumParams();
  OverloadedOperatorKind Op = FnDecl->getOverloadedOperator();

  if ((Op == OO_Star && NumParams == 1) || Op == OO_Arrow ||
      Op == OO_Subscript) {
    const auto *FPT = FnDecl->getType()->castAs<FunctionProtoType>();
    if (NumParams >= 1) {
      QualType FirstParamTy = FPT->getParamType(0);
      if (!FirstParamTy->isPointerType() ||
          !FirstParamTy->getPointeeType()->isOverloadableType()) {
        Diag(FnDecl->getLocation(), diag::err_operator_overload_needs_point)
            << FnDecl->getDeclName();
        return true;
      }
    } else {
      Diag(FnDecl->getLocation(), diag::err_operator_overload_needs_point)
          << FnDecl->getDeclName();
      return true;
    }
    QualType returnType = FnDecl->getReturnType();
    if (!returnType->isPointerType() && !isRcPointerType(returnType)) {
      Diag(FnDecl->getLocation(), diag::err_operator_overload_needs_point)
          << FnDecl->getDeclName();
      return true;
    }
  } else {
    bool ClassOrEnumParam = false;
    for (auto Param : FnDecl->parameters()) {
      QualType ParamType = Param->getType().getNonReferenceType();
      if (ParamType->isOverloadableType()) {
        ClassOrEnumParam = true;
        break;
      }
    }

    if (!ClassOrEnumParam) {
      Diag(FnDecl->getLocation(),
           diag::err_operator_overload_needs_class_or_enum)
          << FnDecl->getDeclName();
      return true;
    }

    if (CheckComparisonKindOperatorFunReturnType(FnDecl)) {
      return true;
    }
  }

  static const bool OperatorUses[NUM_OVERLOADED_OPERATORS][3] = {
      {false, false, false}
#define OVERLOADED_OPERATOR(Name, Spelling, Token, Unary, Binary, MemberOnly)  \
  , { Unary, Binary, MemberOnly }
#include "clang/Basic/OperatorKinds.def"
  };

  bool CanBeUnaryOperator = OperatorUses[Op][0];
  bool CanBeBinaryOperator = OperatorUses[Op][1];
  // Address operator overload is not allowed
  if (Op == OO_Amp && NumParams == 1) {
    Diag(FnDecl->getLocation(), diag::err_operator_overload_must_be)
        << FnDecl->getDeclName() << NumParams << 1;
    return true;
  }
  if ((NumParams == 1 && !CanBeUnaryOperator) ||
      (NumParams == 2 && !CanBeBinaryOperator) || (NumParams < 1) ||
      (NumParams > 2)) {
    unsigned ErrorKind;
    if (CanBeUnaryOperator && CanBeBinaryOperator) {
      ErrorKind = 2; // 2 -> unary or binary.
    } else if (CanBeUnaryOperator) {
      ErrorKind = 0; // 0 -> unary
    } else {
      ErrorKind = 1; // 1 -> binary
    }
    Diag(FnDecl->getLocation(), diag::err_operator_overload_must_be)
        << FnDecl->getDeclName() << NumParams << ErrorKind;
    return true;
  }

  return false;
}

bool Sema::CheckIsUnsafeOverloadCall(Expr *Fn) {
  if (!IsInEvaluatedSafeZone()) {
    return false;
  }
  if (!Fn->getType()->checkFunctionProtoType(SZ_Safe)) {
    Diag(Fn->getBeginLoc(), diag::err_unsafe_action)
        << "overload _Unsafe function";
    return true;
  }
  return false;
}

#endif