//===- TypeBSC.h - BSC type utilities ---------------------------*- BSC -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// BSC type utility functions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_BSC_TYPEBSC_H
#define LLVM_CLANG_AST_BSC_TYPEBSC_H

#if ENABLE_BSC

#include "clang/AST/Type.h"

namespace clang {

/// Apply \p NK as outer BSC nullability on \p QT, idempotently.
/// BSC stores _Nullable/_Nonnull as non-fast qualifier bits (like
/// _ArrayElem). If \p QT already has the same nullability (treating
/// NullableResult as Nullable), returns \p QT unchanged. Otherwise strips
/// existing outer nullability and re-applies \p NK as qualifier bits.
QualType applyNullabilityToType(QualType QT, NullabilityKind NK,
                                ASTContext &Ctx);

/// Copy explicit nullability from \p Src onto \p Dest, if any.
QualType transferExplicitNullability(QualType Src, QualType Dest,
                                     ASTContext &Ctx);

/// Strip _Nullable/_Nonnull at every pointer level of \p T.
/// Used when comparing pointer kinds (SafeZone / Ownership) so that
/// nullability differences — handled by the nullability checker — do not
/// make otherwise-compatible pointer types look distinct.
QualType stripAllNullabilityQualifiers(QualType T, ASTContext &Ctx);

/// \c getOnlyBSCQualifiedType followed by \c stripAllNullabilityQualifiers.
/// Prefer this over ad-hoc \c getUnqualifiedType + nullability stripping when
/// comparing BSC pointer kinds: Owned/Borrow/ArrayElem are kept consistently,
/// CVR is dropped, and nullability is removed at every pointer level.
QualType getOnlyBSCQualifiedTypeWithoutNullability(QualType T,
                                                    ASTContext &Ctx);

/// Returns true when LHS and RHS function types have the same effective
/// nullability on every corresponding pair of parameters and return types.
/// Returns false if any mismatch is found, e.g. a _Nonnull source parameter
/// assigned to a _Nullable destination parameter.
bool AreFunctionTypesNullabilityCompatible(const FunctionProtoType *LHS,
                                           const FunctionProtoType *RHS,
                                           ASTContext &Ctx);

} // namespace clang

#endif // ENABLE_BSC

#endif // LLVM_CLANG_AST_BSC_TYPEBSC_H
