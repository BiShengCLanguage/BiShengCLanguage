//===- WalkerBSC.h - Classes for traversing BSC ASTs --*- BSC -*-=====//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Defines the BSC Walker utilities.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_WALKERBSC_H
#define LLVM_CLANG_AST_WALKERBSC_H

#if ENABLE_BSC

#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclBase.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/AST/DeclarationName.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeOrdering.h"
#include "clang/AST/TypeVisitor.h"
#include "clang/Basic/Builtins.h"

namespace clang {
class ASTContext;

static bool IsBSCDesugared(Decl *D, ASTContext *Context) {
  for (auto DD : Context->BSCDesugaredMap) {
    for (auto &DesugaredDecl : DD.second) {
      if (isa<NamedDecl>(D)) {
        NamedDecl *ND = cast<NamedDecl>(D);
        if (ND == DesugaredDecl) {
          return true;
        }
      }
    }
  }
  return false;
}

class BSCFeatureFinder : public StmtVisitor<BSCFeatureFinder, bool>,
                         public DeclVisitor<BSCFeatureFinder, bool>,
                         public TypeVisitor<BSCFeatureFinder, bool> {
public:
  using TypeVisitor<BSCFeatureFinder, bool>::Visit;
  using DeclVisitor<BSCFeatureFinder, bool>::Visit;
  using StmtVisitor<BSCFeatureFinder, bool>::Visit;

  BSCFeatureFinder(ASTContext &Context) : Context(Context) {}

  /*--------------Types--------------*/

  bool VisitType(const Type *T) {
    if (isa<PointerType>(T)) {
      if (VisitQualType(cast<PointerType>(T)->getPointeeType())) {
        return true;
      }
    }

    if (isa<ParenType>(T)) {
      if (VisitQualType(cast<ParenType>(T)->getInnerType())) {
        return true;
      }
    }

    if (const FunctionProtoType *FPT = dyn_cast<FunctionProtoType>(T)) {
      if (VisitQualType(FPT->getReturnType())) {
        return true;
      }
      for (auto &P : FPT->getParamTypes()) {
        if (VisitQualType(P)) {
          return true;
        }
      }
      if (FPT->hasExtParameterInfos()) {
        for (unsigned I = 0, N = FPT->getNumParams(); I != N; ++I) {
          FunctionProtoType::ExtParameterInfo EPI = FPT->getExtParameterInfo(I);
          if (EPI.isEnsureInit() || EPI.isEnsureInitIfRet()) {
            return true;
          }
        }
      }
    }

    if (T->isTraitType() || T->isTraitPointerType() || T->hasTraitType()) {
      return true;
    }
    return false;
  }

  bool VisitQualType(QualType QT) {
    if (QT.isOwnedQualified() || QT.isBorrowQualified() ||
        QT->hasBorrowFields() || QT->hasOwnedFields()) {
      return true;
    }
    if (QT.isNullableQualified() || QT.isNonnullQualified()) {
      return true;
    }
    if (IsDesugaredFromTraitType(QT)) {
      return true;
    }
    if (QT->getAs<TemplateSpecializationType>()) {
      return true;
    }
    if (auto *TT = QT->getAs<TypedefType>()) {
      if (auto *TD = TT->getDecl())
        if (isa<TypeAliasDecl>(TD) || isa<TypeAliasTemplateDecl>(TD))
          return true;
    }
    if (VisitType(QT.getTypePtr())) {
      return true;
    }
    return false;
  }

  /*--------------Decls--------------*/
  bool VisitBSCMethodDecl(BSCMethodDecl *MD) { return true; }

  bool VisitFunctionDecl(FunctionDecl *FD) {
    if (VisitQualType(FD->getType())) {
      return true;
    }

    // async related funcs
    if (IsBSCDesugared(FD, &Context) || FD->isAsyncSpecified()) {
      return true;
    }
    // safe / unsafe func
    if (FD->getSafeZoneSpecifier() != SZ_None) {
      return true;
    }
    // generic function
    if (FD->getParent() && isa<RecordDecl>(FD->getParent()) &&
        cast<RecordDecl>(FD->getParent())->getDescribedClassTemplate()) {
      return true;
    }
    if (FD->isTemplateInstantiation()) {
      return true;
    }

    if (FD->isConstexpr()) {
      return true;
    }

    for (auto *Parameter : FD->parameters()) {
      if (Visit(Parameter)) {
        return true;
      }
      if (VisitQualType(Parameter->getType())) {
        return true;
      }
    }

    if (FD->doesThisDeclarationHaveABody()) {
      if (Visit(FD->getBody())) {
        return true;
      }
    }

    if (FD->hasAttr<OperatorAttr>()) {
      return true;
    }
    return false;
  }
  bool VisitVarDecl(VarDecl *D) {
    if (VisitQualType(D->getType())) {
      return true;
    }
    if (D->isConstexpr()) {
      return true;
    }

    if (D->hasInit()) {
      if (Visit((D->getInit()))) {
        return true;
      }
      if (VisitQualType(D->getInit()->getType())) {
        return true;
      }
    }
    return false;
  }

  bool VisitRecordDecl(RecordDecl *RD) {
    if (IsBSCDesugared(RD, &Context)) {
      return true;
    }

    if (RD->isTrait() || RD->getDesugaredTraitDecl()) {
      return true;
    }
    if (RD->isOwnedDecl()) {
      return true;
    }
    for (auto Member : RD->fields()) {
      if (Visit(Member)) {
        return true;
      }
      if (VisitQualType(Member->getType())) {
        return true;
      }
    }
    return false;
  }

  bool VisitStaticAssertDecl(StaticAssertDecl *D) {
    return Visit(D->getAssertExpr());
  }

  bool VisitTypeAliasDecl(TypeAliasDecl *D) { return true; }

  bool VisitTypeAliasTemplateDecl(TypeAliasTemplateDecl *D) { return true; }

  bool VisitStmt(Stmt *S) {
    for (auto *C : S->children()) {
      if (C) {
        if (Visit(C)) {
          return true;
        }
      }
    }
    return false;
  }

  /*--------------Stmts--------------*/
  bool VisitDeclStmt(DeclStmt *DS) {
    for (auto *D : DS->decls()) {
      if (Visit(D)) {
        return true;
      }
    }
    return false;
  }

  bool VisitCompoundStmt(CompoundStmt *CS) {
    if (CS->getCompSafeZoneSpecifier() != SZ_None) {
      return true;
    }
    for (auto *S : CS->body()) {
      if (Visit(S)) {
        return true;
      }
    }
    return false;
  }

  bool VisitIfStmt(IfStmt *IS) {
    if (IS->isConstexpr()) {
      return true;
    }
    if (Visit(IS->getCond())) {
      return true;
    }
    if (IS->getConditionVariable()) {
      if (Visit(IS->getConditionVariable())) {
        return true;
      }
    }
    if (IS->getThen()) {
      if (Visit(IS->getThen())) {
        return true;
      }
    }
    if (IS->getElse()) {
      if (Visit(IS->getElse())) {
        return true;
      }
    }
    return false;
  }

  bool VisitSafeStmt(SafeStmt *SS) { return true; }

  bool VisitSafeExpr(SafeExpr *SE) { return true; }

  bool VisitInitListExpr(InitListExpr *ILE) {
    if (VisitQualType(ILE->getType())) {
      return true;
    }
    for (unsigned i = 0, e = ILE->getNumInits(); i != e; ++i) {
      if (ILE->getInit(i)) {
        if (Visit(ILE->getInit(i))) {
          return true;
        }
      }
    }
    return false;
  }

  bool VisitCStyleCastExpr(CStyleCastExpr *E) {
    if (VisitQualType(E->getType())) {
      return true;
    }
    if (VisitExplicitCastExpr(E)) {
      return true;
    }
    return false;
  }

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    if (VisitQualType(DRE->getType())) {
      return true;
    }
    if (IsBSCDesugared(DRE->getDecl(), &Context)) {
      return true;
    }

    if (DRE->getDecl()->getKind() == Decl::BSCMethod) {
      return true;
    }

    if (DRE->getDecl()->getKind() == Decl::Function) {
      FunctionDecl *FD = cast<FunctionDecl>(DRE->getDecl());
      if (FD->isTemplateInstantiation() || FD->isConstexpr() ||
          FD->hasAttr<OperatorAttr>()) {
        return true;
      }
      switch (FD->getBuiltinID()) {
      case Builtin::BI__move_to_raw:
      case Builtin::BI__take_from_raw:
      case Builtin::BI__move_array_to_raw:
      case Builtin::BI__take_array_from_raw:
      case Builtin::BI__assume_initialized:
      case Builtin::BI__assume_null:
      case Builtin::BI__forget:
        return true;
      default:
        break;
      }
    }
    return false;
  }

  bool VisitMemberExpr(MemberExpr *ME) {
    if (VisitQualType(ME->getType())) {
      return true;
    }
    if (Visit(ME->getBase())) {
      return true;
    }
    if (IsBSCDesugared(ME->getMemberDecl(), &Context)) {
      return true;
    }
    if (ME->getMemberDecl()->getKind() == Decl::BSCMethod) {
      return true;
    }
    if (ME->getMemberDecl()->getKind() == Decl::Function) {
      FunctionDecl *FD = cast<FunctionDecl>(ME->getMemberDecl());
      if (FD->isTemplateInstantiation() || FD->isConstexpr()) {
        return true;
      }
    }
    return false;
  }
  
  // sizeof / _Alignof / __alignof — the operand may be a BSC-qualified type
  // whose only appearance is here; without this the walker reports "no BSC
  // features" and the rewriter copies the source verbatim, leaking qualifiers.
  bool VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *E) {
    if (E->isArgumentType())
      return VisitQualType(E->getArgumentType());
    return Visit(E->getArgumentExpr());
  }

  // _Generic association types can carry BSC-qualified types without any other
  // BSC syntax in the expression.
  bool VisitGenericSelectionExpr(GenericSelectionExpr *E) {
    if (Visit(E->getControllingExpr()))
      return true;
    for (const GenericSelectionExpr::Association Assoc : E->associations()) {
      if (!Assoc.getType().isNull() && VisitQualType(Assoc.getType()))
        return true;
      if (Visit(Assoc.getAssociationExpr()))
        return true;
    }
    return false;
  }

  // nullptr visit
  bool VisitCXXNullPtrLiteralExpr(CXXNullPtrLiteralExpr *E) {
      return true;
  }


protected:
  ASTContext &Context;

private:
  bool IsDesugaredFromTraitType(QualType T) {
    while (T->isPointerType())
      T = T->getPointeeType();
    if (RecordDecl *RD = T->getAsRecordDecl()) {
      if (auto *TST = dyn_cast_or_null<TemplateSpecializationType>(T)) {
        TemplateDecl *TempT = TST->getTemplateName().getAsTemplateDecl();
        RD = dyn_cast_or_null<RecordDecl>(TempT->getTemplatedDecl());
      }
      if (RD && RD->getDesugaredTraitDecl())
        return true;
    }
    return false;
  }
};

/// Whether a FunctionDecl has any "safe" features in it.
/// TODO: add Unit Test
class SafeFeatureFinder : public StmtVisitor<SafeFeatureFinder, bool>,
                          public DeclVisitor<SafeFeatureFinder, bool> {
public:
  using DeclVisitor<SafeFeatureFinder, bool>::Visit;
  using StmtVisitor<SafeFeatureFinder, bool>::Visit;

  // TODO: Is this enough? Do we need VisitType API?
  bool VisitQualType(QualType QT) {
    if (QT.isOwnedQualified() || QT.isBorrowQualified() ||
        QT->hasBorrowFields() || QT->hasOwnedFields()) {
      return true;
    }
    // Recurse into array element types to detect arrays of owned/borrow
    // pointers (e.g., int *_Owned arr[10]).
    if (const auto *AT = QT->getAsArrayTypeUnsafe())
      return VisitQualType(AT->getElementType());
    return false;
  }

  bool VisitFunctionDecl(FunctionDecl *FD) {
    // Return Type has owned, borrow
    if (VisitQualType(FD->getType())) {
      return true;
    }

    // Function Parameters has owned, borrow
    for (auto *Param : FD->parameters()) {
      if (VisitQualType(Param->getType())) {
        return true;
      }
    }

    // FuncBody has owned, borrow
    if (FD->doesThisDeclarationHaveABody()) {
      // Dispatch to VisitCompoundStmt
      if (Visit(FD->getBody())) {
        return true;
      }
    }

    return false;
  }

  bool VisitVarDecl(VarDecl *D) {
    if (VisitQualType(D->getType())) {
      return true;
    }

    if (D->hasInit()) {
      if (Visit((D->getInit()))) {
        return true;
      }
      if (VisitQualType(D->getInit()->getType())) {
        return true;
      }
    }
    return false;
  }

  // TODO: Not sure other Decls should be override or not.
  // RecordDecl, StaticAssertDecl, TypeAliasDecl, BSCMethodDecl etc.

  // If VisitXXXStmt is not implemented, fallback to VisitStmt
  bool VisitStmt(Stmt *S) {
    // Children() is the common interface implemented by XStmt
    for (auto *C : S->children()) {
      if (C) {
        if (Visit(C)) {
          return true;
        }
      }
    }
    return false;
  }

  bool VisitDeclStmt(DeclStmt *DS) {
    for (auto *D : DS->decls()) {
      if (Visit(D))
        return true;
    }
    return false;
  }

  bool VisitCStyleCastExpr(CStyleCastExpr *E) {
    if (VisitQualType(E->getType())) {
      return true;
    }
    if (VisitExplicitCastExpr(E)) {
      return true;
    }
    return false;
  }

  bool VisitUnaryOperator(UnaryOperator *UO) {
    return UO->getOpcode() >= UO_AddrMut && UO->getOpcode() <= UO_AddrConstDeref;
  }

  bool FindOwnedOrBorrow(FunctionDecl *FD) { return VisitFunctionDecl(FD); }
};

} // namespace clang

#endif // ENABLE_BSC

#endif // LLVM_CLANG_AST_WALKERBSC_H
