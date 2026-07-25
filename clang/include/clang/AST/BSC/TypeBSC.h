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

/// Returns the effective nullability of a BSC pointer type.
/// If the type carries an explicit _Nonnull or _Nullable annotation, returns it.
/// Otherwise fills in the BSC default: _Owned/_Borrow pointers default to
/// _Nonnull; raw pointers default to _Nullable. Non-pointer types return
/// NullabilityKind::Unspecified.
NullabilityKind getDefNullability(QualType QT, const ASTContext &Ctx);

/// Apply \p NK as outer nullability sugar on \p QT, idempotently.
/// If \p QT already has the same nullability (treating NullableResult as
/// Nullable), returns \p QT unchanged. Otherwise strips existing outer
/// nullability sugar and wraps with a fresh AttributedType.
QualType applyNullabilityToType(QualType QT, NullabilityKind NK,
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
