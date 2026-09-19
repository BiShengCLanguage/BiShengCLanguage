//===- StmtBSC.cpp - BSC Statement AST Node Implementation --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the BSC related Stmt classes.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Expr.h"

using namespace clang;

SafeZoneSpecifier SafeZoneAttr::getSafeZoneSpec(const Stmt *S) {
  if (const auto *AS = dyn_cast<AttributedStmt>(S))
    for (const Attr *A : AS->getAttrs())
      if (const auto *Zone = dyn_cast<SafeZoneAttr>(A))
        return Zone->isSafe() ? SZ_Safe : SZ_Unsafe;
  if (const auto *PE = dyn_cast<ParenExpr>(S))
    return PE->getSafeZoneSpec();
  return SZ_None;
}

#endif // ENABLE_BSC