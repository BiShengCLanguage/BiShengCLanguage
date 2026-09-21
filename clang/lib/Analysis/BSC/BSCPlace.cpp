//===- BSCPlace.cpp - Shared place infrastructure for BSC analyses ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/Analysis/Analyses/BSC/BSCPlace.h"
#include "clang/AST/Expr.h"
#include "clang/AST/BSC/ExprBSC.h"

using namespace clang;

namespace clang {
namespace bsc {

constexpr uint64_t Place::NullBaseSeed;

Ty *Ty::Create(const ASTContext &Ctx, QualType QT) {
  QT = QT.getCanonicalType();

  if (QT->isPointerType())
    return Ty::CreatePointer(Ctx, QT,
                             Ty::Create(Ctx, QT->getPointeeType()));

  if (QT->isArrayType())
    return Ty::CreateArray(Ctx, QT, Ty::Create(Ctx, QT->getAsArrayTypeUnsafe()
                                                        ->getElementType()));

  if (QT->getAs<RecordType>())
    return Ty::CreateStruct(Ctx, QT);

  return Ty::CreateBase(Ctx, QT);
}

void Place::print(llvm::raw_ostream &OS) const {
  switch (K) {
  case Kind::Var:
    OS << Name;
    break;
  case Kind::Field:
    assert(Base && "field place should have a base");
    if (Base->getKind() == Kind::Deref)
      OS << '(';
    Base->print(OS);
    if (Base->getKind() == Kind::Deref)
      OS << ')';
    OS << '.' << Name;
    break;
  case Kind::Deref:
    assert(Base && "deref place should have a base");
    OS << '*';
    Base->print(OS);
    break;
  case Kind::Index:
    assert(Base && "index place should have a base");
    Base->print(OS);
    OS << '[' << Name << ']';
    break;
  }
}

const Place *PlaceBuilder::Build(const VarDecl *VD, SourceLocation Loc) {
  return Place::CreateVar(Ctx, VD, Ty::Create(Ctx, VD->getType()), Loc);
}

const Place *PlaceBuilder::BuildField(const Place *Base, const FieldDecl *FD,
                                      SourceLocation Loc) {
  return Place::CreateField(Ctx, Base, FD, Ty::Create(Ctx, FD->getType()), Loc);
}

const Place *PlaceBuilder::BuildDeref(const Place *Base, QualType PointeeTy,
                                      SourceLocation Loc) {
  return Place::CreateDeref(Ctx, Base, Ty::Create(Ctx, PointeeTy), Loc);
}

const Place *PlaceBuilder::BuildIndex(const Place *Base, QualType ElemTy,
                                      llvm::StringRef Discr,
                                      SourceLocation Loc) {
  return Place::CreateIndex(Ctx, Base, Ty::Create(Ctx, ElemTy), Loc, Discr);
}

const Place *PlaceBuilder::Build(const Expr *E) {
  if (!E)
    return nullptr;

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      return Build(VD, DRE->getLocation());
    return nullptr;
  }

  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
    if (!FD)
      return nullptr;
    const Place *FieldBase = Build(ME->getBase());
    // p->f dereferences p before indexing the field.
    if (ME->isArrow())
      FieldBase = BuildDeref(FieldBase, ME->getBase()->getType()
                                             ->getPointeeType(),
                             ME->getOperatorLoc());
    // Untrackable base (array element, call result, ...): root the field cell
    // at null so same-named fields of such bases share one cell, matching the
    // historical string-path fallback.
    return BuildField(FieldBase, FD, ME->getMemberLoc());
  }

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() != UO_Deref)
      return nullptr;
    const Place *Sub = Build(UO->getSubExpr());
    if (!Sub)
      return nullptr;
    return BuildDeref(Sub, UO->getType(), UO->getOperatorLoc());
  }

  if (const auto *ICE = dyn_cast<ImplicitCastExpr>(E))
    return Build(ICE->getSubExpr());

  if (const auto *PE = dyn_cast<ParenExpr>(E))
    return Build(PE->getSubExpr());

  if (const auto *SE = dyn_cast<SafeExpr>(E))
    return Build(SE->getSubExpr());

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    const Place *Base = Build(ASE->getBase());
    if (!Base)
      return nullptr;
    // The discriminant is not part of borrow identity: any index of a borrow
    // root addresses the same pointee.
    return BuildIndex(Base, ASE->getType(), "", ASE->getRBracketLoc());
  }

  return nullptr;
}

} // namespace bsc
} // namespace clang
#endif // ENABLE_BSC