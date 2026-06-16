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
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include <utility>

namespace clang {

class FieldDecl;

/// Describes how the implicit region parameters of a record are projected to
/// its fields. Field entries contain indices into a concrete record instance's
/// region vector and may repeat an index for recursive projections.
class RecordRegionLayout {
public:
  using FieldRegionIndicesTy = llvm::SmallVector<unsigned, 2>;
  using FieldRegionMapTy =
      llvm::DenseMap<const FieldDecl *, FieldRegionIndicesTy>;

private:
  unsigned NumRegions = 0;
  FieldRegionMapTy FieldRegionIndices;

public:
  RecordRegionLayout() = default;
  RecordRegionLayout(unsigned NumRegions,
                     FieldRegionMapTy FieldRegionIndices)
      : NumRegions(NumRegions),
        FieldRegionIndices(std::move(FieldRegionIndices)) {}

  unsigned getNumRegions() const { return NumRegions; }

  llvm::ArrayRef<unsigned>
  getFieldRegionIndices(const FieldDecl *FD) const {
    auto It = FieldRegionIndices.find(FD);
    if (It == FieldRegionIndices.end())
      return {};
    return It->second;
  }
};

using RecordRegionLayoutMap =
    llvm::DenseMap<const RecordDecl *, RecordRegionLayout>;

/// Return the fixed implicit-region layout of \p RD, building it and any
/// dependent record layouts on demand.
const RecordRegionLayout &
GetOrCreateRecordRegionLayout(const ASTContext &Ctx, const RecordDecl *RD,
                              RecordRegionLayoutMap &Layouts);

/// Compute the number of borrow regions required to represent \p Type.
unsigned ComputeNumRegions(const ASTContext &Ctx, QualType Type);

/// Compute the number of borrow regions required to represent \p Type,
/// building record layouts in \p Layouts on demand.
unsigned ComputeNumRegions(const ASTContext &Ctx, QualType Type,
                           RecordRegionLayoutMap &Layouts);

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
