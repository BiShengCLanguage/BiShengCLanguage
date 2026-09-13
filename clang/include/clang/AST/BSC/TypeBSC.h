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

/// \p T with every pointer level removed: `T **` gives `T`, a non-pointer
/// gives itself. Sugar on the innermost type is kept.
QualType getInnermostPointeeType(QualType T);

/// Look through typedefs and alias-template specializations. Stops at a
/// class-template specialization, so its arguments stay visible, and at any
/// other type.
QualType stripTypedefsAndAliasTemplates(QualType T);

/// The record declaration of \p T, taking the primary template's record for a
/// template specialization; null when \p T is not a record.
RecordDecl *getRecordDeclThroughSpecialization(QualType T);

/// Behind any pointer levels, \p T is a struct desugared from a `_Trait`.
bool isDesugaredFromTraitType(QualType T);

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

unsigned ComputeNumRegions(const ASTContext &Ctx, QualType Type);
unsigned ComputeNumRegions(const ASTContext &Ctx, QualType Type,
                           RecordRegionLayoutMap &Layouts);

/// Manual 3.8.3: compatible, with the same qualifiers at every pointer level;
/// a function type compares its return and parameter slots.
bool areBSCTypesCompatible(QualType L, QualType R);

/// Manual 3.4.4: either operand may supply the value, so every pointer level
/// of \p T is nullable as soon as one of \p L / \p R is.
QualType mergeNullabilityAtEveryPointerLevel(const ASTContext &Ctx, QualType T,
                                             QualType L, QualType R);

/// Manual 3.8.3 for a function-pointer assignment; Incompatible is what C
/// would only warn about but ownership must not let pass.
enum class BSCFunctionMismatch { None, Owned, Borrow, Incompatible };
BSCFunctionMismatch firstBSCFunctionTypeMismatch(const ASTContext &Ctx,
                                                 const FunctionProtoType *L,
                                                 const FunctionProtoType *R);

/// Manual 3.6.5.4: Unsafe satisfies the unsafe-safe refinement relation to
/// Safe.
bool satisfiesUnsafeSafeRefinement(QualType Unsafe, QualType Safe);

struct UnsafeSafeRefinementMismatchInfo {
  enum class Kind {
    ReturnType,
    Parameter,
    ParamCount,
    Variadic,
    Other,
  };
  Kind MismatchKind = Kind::Other;
  // 1-based when MismatchKind == Parameter; 0 otherwise.
  unsigned ParamIndex = 0;
  QualType Type1;
  QualType Type2;
};

/// Manual 3.6.5.4: the _Unsafe function type satisfies the unsafe-safe
/// refinement relation with respect to the _Safe one.
bool functionTypeSatisfiesUnsafeSafeRefinement(
    ASTContext &Ctx, QualType Type1, QualType Type2,
    SafeZoneSpecifier SZS1, SafeZoneSpecifier SZS2,
    UnsafeSafeRefinementMismatchInfo *MismatchOut);

} // namespace clang

#endif // ENABLE_BSC

#endif // LLVM_CLANG_AST_BSC_TYPEBSC_H
