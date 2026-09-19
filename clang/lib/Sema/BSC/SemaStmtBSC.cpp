//===--- SemaStmtBSC.cpp - Semantic Analysis for Statements
//------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements semantic analysis for statements.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaDiagnostic.h"

using namespace clang;
using namespace sema;

// Check if BSC constexpr if condition expression satisfy:
// 1. type is bool, integral or char;
// 2. constant expression which can be calculated in compile time.
ExprResult Sema::CheckBSCConstexprCondition(SourceLocation Loc, Expr *CondExpr, bool IsConstexpr) {
  if (!CondExpr->getType()->isBSCCalculatedTypeInCompileTime()) {
    Diag(Loc, diag::err_constexpr_if_cond_expr_unsupported_type) << CondExpr->getType();
    return ExprError();
  }
  return CheckCXXBooleanCondition(CondExpr, IsConstexpr);
}

StmtResult Sema::ActOnSafeStmt(SourceLocation SafeZoneLoc,
                               SafeZoneSpecifier safeZoneSpec, Stmt *SubStmt) {
  auto *Zone = SafeZoneAttr::Create(Context, SafeZoneLoc,
                                    AttributeCommonInfo::AS_Keyword,
                                    safeZoneSpec == SZ_Safe
                                        ? SafeZoneAttr::Keyword_Safe
                                        : SafeZoneAttr::Keyword_Unsafe);
  return AttributedStmt::Create(Context, SafeZoneLoc, Zone, SubStmt);
}

ExprResult Sema::ActOnSafeExpr(SafeZoneSpecifier safeZoneSpec,
                               SourceLocation LParen, SourceLocation RParen,
                               Expr *SubExpr) {
  // ActOnParenExpr may have folded the parentheses into __arithmetic_fence.
  auto *PE = dyn_cast<ParenExpr>(SubExpr);
  if (!PE)
    PE = new (Context) ParenExpr(LParen, RParen, SubExpr);
  PE->setSafeZoneSpec(safeZoneSpec);
  return PE;
}
#endif