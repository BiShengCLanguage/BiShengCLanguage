//===- ExprBSC.h - Classes for representing BSC expressions ---*- BSC -*-=====//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Defines the BSC Expr subclasses
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_EXPRBSC_H
#define LLVM_CLANG_AST_EXPRBSC_H

#if ENABLE_BSC

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"

namespace clang {
  class ASTContext;

class AwaitExpr final : public Expr {
public:
  explicit AwaitExpr(SourceLocation AwaitLoc, Expr *Se, QualType Ty)
      : Expr(AwaitExprClass, Ty, VK_PRValue, OK_Ordinary), AwaitLoc(AwaitLoc) {
    SubExpr = Se;
    setDependence(Se->getDependence());
  }

  explicit AwaitExpr(EmptyShell Empty) : Expr(AwaitExprClass, Empty) {}

  SourceLocation getBeginLoc() const { return AwaitLoc; }
  SourceLocation getEndLoc() const { return SubExpr->getEndLoc(); }

  const Expr *getSubExpr() const { return cast<Expr>(SubExpr); }
  Expr *getSubExpr() { return cast<Expr>(SubExpr); }

  static bool classof(const Stmt *T) {
    return T->getStmtClass() == AwaitExprClass;
  }

  // Iterators
  child_range children() { return child_range(&SubExpr, &SubExpr + 1); }
  const_child_range children() const {
    return const_child_range(&SubExpr, &SubExpr + 1);
  }

protected:
  Stmt *SubExpr;

private:
  SourceLocation AwaitLoc;
  friend class ASTStmtReader;
  friend class ASTStmtWriter;
};

} // namespace clang

#endif // ENABLE_BSC

#endif // LLVM_CLANG_AST_EXPRBSC_H