//===- BSCNullCheckInfo.h - Nullability Check Results for Source CFGs --BSC---//
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

#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_BSCNULLCHECKINFO_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_BSCNULLCHECKINFO_H

#if ENABLE_BSC

#include "clang/Analysis/CFG.h"
#include "llvm/ADT/SmallPtrSet.h"

namespace clang {

/// Stores sub-expressions of Expr
/// that is certainly checked for null-ness or non-null-ness
class NullCheckInfo {
  // In most cases the condition expression only contains a few pointer
  // expressions, such as p && q, limit the size of the set to 2 is enough.
  constexpr static unsigned SetSize = 2;

  /// Mark whether `Cond` could be trivially evaluated as 0 or non-zero scalar
  /// value in compile-time, for example:
  /// NonTrivial: `p` `flag` `x > 0`
  /// ConstTrue: `1` `-1` `sizeof(int) > 0` `0 < 1`
  /// ConstFalse: `0` `0 > 1` `sizeof(int) < 0`
  enum Triviality {
    NonTrivial,
    ConstTrue,
    ConstFalse,
  } triviality;

  ASTContext &ctx;

  /// Helper of constructor. `Negate` carries a logical-not into the recursion
  /// via De Morgan's law.
  void init(const Expr *Cond, bool Negate);

  /// Synthesize two sub-expressions of && operator
  NullCheckInfo &operator&=(NullCheckInfo &&RHS);

  /// Synthesize two sub-expressions of || operator
  NullCheckInfo &operator|=(NullCheckInfo &&RHS);

  /// Extract the pointer expression being checked for null-ness or
  /// non-null-ness in a simple condition expression without top-level logical
  /// operators, such as `p`, `p = q`, `s.p != nullptr`, `++x, s->p == nullptr`
  /// Caller must ensure `Cond` is not nullptr.
  void extractAndInsert(const Expr *Cond, bool Negate);

  /// For infeasible conditions, obliviate those checked expressions to make
  /// sure that the analysis result is sound.
  /// If a condition expression distinguishes the same pointer both null and
  /// non-null, we can declare that the true branch is unreachable,
  /// and we find nothing useful about its nullability/ownership in the false
  /// branch, for example:
  /// if (p && !p) { unreachable } else { p is unknown }
  /// if (p || !p) { p is unknown } else { unreachable }
  void obliviateInfeasible();

public:
  llvm::SmallPtrSet<const Expr *, SetSize> nullCheckedExprs;
  llvm::SmallPtrSet<const Expr *, SetSize> presentCheckedExprs;

  /// Analyze `Cond` under `Ctx`, and extract the checked pointer expressions
  NullCheckInfo(const Expr *Cond, ASTContext &Ctx);

private:
  NullCheckInfo(ASTContext &Ctx) : triviality(NonTrivial), ctx(Ctx) {}

public:

  NullCheckInfo &operator=(NullCheckInfo &&RHS);
};
} // namespace clang

#endif // ENABLE_BSC

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_BSCNULLCHECKINFO_H