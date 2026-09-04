//===- TypeBSC.cpp - Type representation and manipulation -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements the BSC type-related functionality.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Type.h"
#include "clang/AST/BSC/TypeBSC.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <algorithm>
#include <memory>

using namespace clang;

namespace clang {
namespace bsc_detail {

struct RegionLayoutGraphNode {
  const RecordDecl *Record = nullptr;
  const RecordDecl *Definition = nullptr;
  llvm::SmallVector<RegionLayoutGraphNode *, 4> Successors;
  bool Scanned = false;

  RegionLayoutGraphNode() = default;
  RegionLayoutGraphNode(const RecordDecl *Record,
                        const RecordDecl *Definition)
      : Record(Record), Definition(Definition) {}
};

} // namespace bsc_detail
} // namespace clang

namespace llvm {

template <> struct GraphTraits<clang::bsc_detail::RegionLayoutGraphNode *> {
  using NodeRef = clang::bsc_detail::RegionLayoutGraphNode *;
  using ChildIteratorType = llvm::SmallVectorImpl<NodeRef>::iterator;

  static NodeRef getEntryNode(NodeRef Node) { return Node; }
  static ChildIteratorType child_begin(NodeRef Node) {
    return Node->Successors.begin();
  }
  static ChildIteratorType child_end(NodeRef Node) {
    return Node->Successors.end();
  }
};

} // namespace llvm

namespace {

using RegionLayoutGraphNode = clang::bsc_detail::RegionLayoutGraphNode;

class RecordRegionLayoutBuilder {
  struct PendingFieldLayout {
    RecordRegionLayout::FieldRegionIndicesTy Indices;
    bool AppendSCCRegions = false;
  };

  struct PendingRecordLayout {
    RegionLayoutGraphNode *Node;
    llvm::DenseMap<const FieldDecl *, PendingFieldLayout> Fields;
  };

  const ASTContext &Ctx;
  RecordRegionLayoutMap &Layouts;
  RegionLayoutGraphNode Root;
  llvm::DenseMap<const RecordDecl *, RegionLayoutGraphNode *> RecordNodes;
  llvm::SmallVector<std::unique_ptr<RegionLayoutGraphNode>, 8> Nodes;

  static const RecordDecl *GetCanonicalRecord(const RecordDecl *Definition) {
    return cast<RecordDecl>(Definition->getCanonicalDecl());
  }

  RegionLayoutGraphNode *GetOrCreateNode(const RecordDecl *Record,
                                         const RecordDecl *Definition) {
    auto It = RecordNodes.find(Record);
    if (It != RecordNodes.end())
      return It->second;

    Nodes.push_back(
        std::make_unique<RegionLayoutGraphNode>(Record, Definition));
    RegionLayoutGraphNode *Node = Nodes.back().get();
    RecordNodes.try_emplace(Record, Node);
    Root.Successors.push_back(Node);
    return Node;
  }

  static void AddEdge(RegionLayoutGraphNode *From,
                      RegionLayoutGraphNode *To) {
    if (std::find(From->Successors.begin(), From->Successors.end(), To) ==
        From->Successors.end())
      From->Successors.push_back(To);
  }

  void CollectType(QualType Type, RegionLayoutGraphNode *Source = nullptr) {
    Type = Type.getCanonicalType();
    if (Type->isPointerType()) {
      if (Type.isBorrowQualified() || Type.isOwnedQualified())
        CollectType(Type->getPointeeType(), Source);
      return;
    }

    if (const ArrayType *AT = Ctx.getAsArrayType(Type)) {
      CollectType(AT->getElementType(), Source);
      return;
    }

    if (const RecordType *RT = Type->getAs<RecordType>()) {
      if (const RecordDecl *Definition = RT->getDecl()->getDefinition()) {
        const RecordDecl *Record = GetCanonicalRecord(Definition);
        if (Layouts.find(Record) == Layouts.end()) {
          RegionLayoutGraphNode *Target =
              GetOrCreateNode(Record, Definition);
          if (Source)
            AddEdge(Source, Target);
          if (!Target->Scanned) {
            Target->Scanned = true;
            for (const FieldDecl *FD : Definition->fields())
              CollectType(FD->getType(), Target);
          }
        }
      }
    }
  }

  unsigned AllocateRegion(unsigned &NextRegion, bool &HasFieldRegion) {
    if (!HasFieldRegion) {
      // The first tracked region of every field is the record's shared slot.
      HasFieldRegion = true;
      if (NextRegion == 0)
        NextRegion = 1;
      return 0;
    }
    return NextRegion++;
  }

  void BuildFieldLayout(
      QualType Type,
      const llvm::DenseSet<RegionLayoutGraphNode *> &SCCNodes,
      unsigned &NextRegion, bool &HasFieldRegion,
      PendingFieldLayout &FieldLayout) {
    Type = Type.getCanonicalType();
    if (Type->isPointerType()) {
      if (Type.isBorrowQualified()) {
        FieldLayout.Indices.push_back(
            AllocateRegion(NextRegion, HasFieldRegion));
        BuildFieldLayout(Type->getPointeeType(), SCCNodes, NextRegion,
                         HasFieldRegion, FieldLayout);
      } else if (Type.isOwnedQualified()) {
        BuildFieldLayout(Type->getPointeeType(), SCCNodes, NextRegion,
                         HasFieldRegion, FieldLayout);
      }
      return;
    }

    if (const ArrayType *AT = Ctx.getAsArrayType(Type)) {
      BuildFieldLayout(AT->getElementType(), SCCNodes, NextRegion,
                       HasFieldRegion, FieldLayout);
      return;
    }

    if (Type.isBorrowQualified()) {
      FieldLayout.Indices.push_back(
          AllocateRegion(NextRegion, HasFieldRegion));
      return;
    }

    if (const RecordType *RT = Type->getAs<RecordType>()) {
      if (const RecordDecl *Definition = RT->getDecl()->getDefinition()) {
        const RecordDecl *Record = GetCanonicalRecord(Definition);
        auto NodeIt = RecordNodes.find(Record);
        if (NodeIt != RecordNodes.end() && SCCNodes.count(NodeIt->second)) {
          // A record in the same SCC is a recursive tail. Its complete
          // parameter list is appended after the SCC's region count becomes
          // known.
          FieldLayout.AppendSCCRegions = true;
        } else {
          auto LayoutIt = Layouts.find(Record);
          assert(LayoutIt != Layouts.end() &&
                 "dependency layout should be computed before its user");
          for (unsigned I = 0; I < LayoutIt->second.getNumRegions(); ++I) {
            FieldLayout.Indices.push_back(
                AllocateRegion(NextRegion, HasFieldRegion));
          }
        }
      }
    }
  }

  void BuildSCC(llvm::ArrayRef<RegionLayoutGraphNode *> SCC) {
    llvm::DenseSet<RegionLayoutGraphNode *> SCCNodes(SCC.begin(), SCC.end());
    llvm::SmallVector<PendingRecordLayout, 4> PendingLayouts;
    unsigned NumSCCRegions = 0;

    // Records in a recursive SCC use one common formal-region space. Each
    // record computes its non-recursive needs independently, and the SCC uses
    // the largest such layout so recursive edges can map parameters by index.
    for (RegionLayoutGraphNode *Node : SCC) {
      unsigned NextRegion = 0;
      llvm::DenseMap<const FieldDecl *, PendingFieldLayout> Fields;
      for (const FieldDecl *FD : Node->Definition->fields()) {
        PendingFieldLayout FieldLayout;
        bool HasFieldRegion = false;
        BuildFieldLayout(FD->getType(), SCCNodes, NextRegion, HasFieldRegion,
                         FieldLayout);
        Fields.try_emplace(FD, std::move(FieldLayout));
      }
      NumSCCRegions = std::max(NumSCCRegions, NextRegion);
      PendingLayouts.push_back({Node, std::move(Fields)});
    }

    for (PendingRecordLayout &Pending : PendingLayouts) {
      RecordRegionLayout::FieldRegionMapTy Fields;
      for (const FieldDecl *FD : Pending.Node->Definition->fields()) {
        PendingFieldLayout &FieldLayout = Pending.Fields.find(FD)->second;
        if (FieldLayout.AppendSCCRegions) {
          for (unsigned I = 0; I < NumSCCRegions; ++I)
            FieldLayout.Indices.push_back(I);
        }
        Fields.try_emplace(FD, std::move(FieldLayout.Indices));
      }
      Layouts.try_emplace(Pending.Node->Record, NumSCCRegions,
                          std::move(Fields));
    }
  }

public:
  RecordRegionLayoutBuilder(const ASTContext &Ctx,
                            RecordRegionLayoutMap &Layouts)
      : Ctx(Ctx), Layouts(Layouts) {}

  void Build(const RecordDecl *RD) {
    const RecordDecl *Definition = RD->getDefinition();
    assert(Definition && "record layout requires a complete definition");
    const RecordDecl *Record = GetCanonicalRecord(Definition);
    if (Layouts.find(Record) != Layouts.end())
      return;

    CollectType(Ctx.getRecordType(Definition));
    for (auto I = llvm::scc_begin(&Root), E = llvm::scc_end(&Root); I != E;
         ++I) {
      const auto &SCC = *I;
      if (SCC.size() == 1 && SCC.front() == &Root)
        continue;
      BuildSCC(SCC);
    }

    assert(Layouts.find(Record) != Layouts.end() &&
           "record layout should have been built");
  }
};

bool withBorrowFieldsImpl(QualType QT,
                          llvm::SmallPtrSetImpl<const RecordType *> &Visited) {
  if (QT.isBorrowQualified())
    return true;

  if (QT->isPointerType() && QT.isOwnedQualified())
    QT = QT->getPointeeType();

  QT = QT.getCanonicalType();
  const auto *RT = QT->getAs<RecordType>();
  if (!RT)
    return false;

  // Avoid revisiting records in self-referential owned-pointer graphs.
  if (!Visited.insert(RT).second)
    return false;

  RecordDecl *RD = RT->getDecl();
  if (!RD)
    return false;

  for (FieldDecl *FD : RD->fields()) {
    if (withBorrowFieldsImpl(FD->getType(), Visited))
      return true;
  }

  return false;
}
} // namespace

// hasOwnedFields is used to determine whether a type has a field
// that is directly or indirectly qualified by owned.
// If you want to determine whether a type is a move semantic type,
// use isMoveSemanticType instead.
bool PointerType::hasOwnedFields() const {
  QualType R = getPointeeType();
  if (R.isOwnedQualified()) {
    return true;
  }
  if (R.getTypePtr()->hasOwnedFields()) {
    return true;
  }
  return false;
}

// hasOwnedFields is used to determine whether a type has a field
// that is directly or indirectly qualified by owned.
// If you want to determine whether a type is a move semantic type,
// use isMoveSemanticType instead.
bool Type::hasOwnedFields() const {
  if (const auto *RecTy = dyn_cast<RecordType>(CanonicalType)) {
    return RecTy->hasOwnedFields();
  } else if (const auto *PointerTy = dyn_cast<PointerType>(CanonicalType)) {
    return PointerTy->hasOwnedFields();
  } else if (const auto *ArrTy = dyn_cast<ArrayType>(CanonicalType)) {
    return ArrTy->getElementType().getTypePtr()->hasOwnedFields();
  }
  return false;
}

bool PointerType::hasBorrowFields() const {
  QualType R = getPointeeType();
  if (R.isBorrowQualified()) {
    return true;
  }
  if (R.getTypePtr()->hasBorrowFields()) {
    return true;
  }
  return false;
}

bool Type::hasBorrowFields() const {
  if (const auto *RecTy = dyn_cast<RecordType>(CanonicalType)) {
    return RecTy->hasBorrowFields();
  } else if (const auto *PointerTy = dyn_cast<PointerType>(CanonicalType)) {
    return PointerTy->hasBorrowFields();
  } else if (const auto *ArrTy = dyn_cast<ArrayType>(CanonicalType)) {
    return ArrTy->getElementType().getTypePtr()->hasBorrowFields();
  }
  return false;
}

bool Type::withBorrowFields() const {
  if (!isa<RecordType>(CanonicalType))
    return false;

  llvm::SmallPtrSet<const RecordType *, 16> Visited;
  return withBorrowFieldsImpl(CanonicalType, Visited);
}

bool FunctionProtoType::hasOwnedRetOrParams() const {
  if (getReturnType().isOwnedQualified()) {
    return true;
  }
  for (auto ParamType : getParamTypes()) {
    if (ParamType.isOwnedQualified()) {
      return true;
    }
  }
  return false;
}

bool FunctionProtoType::hasBorrowRetOrParams() const {
  if (getReturnType().hasBorrow()) {
    return true;
  }
  for (auto ParamType : getParamTypes()) {
    if (ParamType.hasBorrow()) {
      return true;
    }
  }
  return false;
}

bool Type::checkFunctionProtoType(SafeZoneSpecifier SZS) const {
  const FunctionProtoType *FPT = nullptr;
  if (isFunctionType()) {
    FPT = getAs<FunctionProtoType>();
  } else if (isFunctionPointerType()) {
    FPT = getPointeeType()->getAs<FunctionProtoType>();
  }
  if (FPT) {
    FunctionProtoType::ExtProtoInfo EPI = FPT->getExtProtoInfo();
    return EPI.SafeZoneSpec == SZS;
  }
  return false;
}

namespace clang {

const RecordRegionLayout &
GetOrCreateRecordRegionLayout(const ASTContext &Ctx, const RecordDecl *RD,
                              RecordRegionLayoutMap &Layouts) {
  const RecordDecl *Definition = RD->getDefinition();
  assert(Definition && "record layout requires a complete definition");
  const RecordDecl *Record =
      cast<RecordDecl>(Definition->getCanonicalDecl());
  auto It = Layouts.find(Record);
  if (It == Layouts.end()) {
    RecordRegionLayoutBuilder(Ctx, Layouts).Build(Definition);
    It = Layouts.find(Record);
  }
  assert(It != Layouts.end() && "record layout should have been built");
  return It->second;
}

unsigned ComputeNumRegions(const ASTContext &Ctx, QualType Type) {
  RecordRegionLayoutMap Layouts;
  return ComputeNumRegions(Ctx, Type, Layouts);
}

unsigned ComputeNumRegions(const ASTContext &Ctx, QualType Type,
                           RecordRegionLayoutMap &Layouts) {
  Type = Type.getCanonicalType();

  if (Type->isPointerType()) {
    if (Type.isBorrowQualified())
      return ComputeNumRegions(Ctx, Type->getPointeeType(), Layouts) + 1;
    if (Type.isOwnedQualified())
      return ComputeNumRegions(Ctx, Type->getPointeeType(), Layouts);
    return 0;
  }

  if (const ArrayType *AT = Ctx.getAsArrayType(Type))
    return ComputeNumRegions(Ctx, AT->getElementType(), Layouts);

  if (const RecordType *RT = Type->getAs<RecordType>()) {
    if (const RecordDecl *Definition = RT->getDecl()->getDefinition())
      return GetOrCreateRecordRegionLayout(Ctx, Definition, Layouts)
          .getNumRegions();
  }

  return 0;
}

/// Check that SafeType is a valid _Safe-side refinement of UnsafeType
/// for heterogeneous redeclarations.  The _Safe redeclaration may add
/// qualifiers (_Owned, _Borrow, _ArrayElem) but must not drop them.
/// Additionally: the _Unsafe side must not be _Nonnull when the _Safe
/// side is _Nullable.
static bool AreTypesCompatibleForUnsafeToSafeRefinement(QualType UnsafeType,
                                                       QualType SafeType,
                                                       ASTContext &Ctx) {
  bool UnsafeIsOwned =
      UnsafeType->isPointerType() && UnsafeType.isOwnedQualified();
  bool UnsafeIsBorrow =
      UnsafeType->isPointerType() && UnsafeType.isBorrowQualified();
  bool UnsafeIsArrayElem =
      UnsafeType->isPointerType() && UnsafeType.isArrayElemQualified();
  bool SafeIsOwned = SafeType->isPointerType() && SafeType.isOwnedQualified();
  bool SafeIsBorrow = SafeType->isPointerType() && SafeType.isBorrowQualified();
  bool SafeIsArrayElem =
      SafeType->isPointerType() && SafeType.isArrayElemQualified();

  // Safe redecl must not drop a qualifier present in the unsafe decl.
  if (UnsafeIsOwned && !SafeIsOwned)
    return false;
  if (UnsafeIsBorrow && !SafeIsBorrow)
    return false;
  if (UnsafeIsArrayElem && !SafeIsArrayElem)
    return false;
  if ((UnsafeIsOwned || UnsafeIsBorrow) &&
      !UnsafeIsArrayElem && SafeIsArrayElem)
    return false;

  // Nullability check:
  // A (_Unsafe) being _Nonnull while B (_Safe) is _Nullable is forbidden.
  if (UnsafeType->isPointerType() && SafeType->isPointerType()) {
    if (UnsafeType.getDefNullability() == NullabilityKind::NonNull &&
        SafeType.getDefNullability() == NullabilityKind::Nullable)
      return false;
  }

  // Peel one pointer layer and recurse so a buried qualifier is not dropped:
  // into the pointee's function prototype if it has one, else into the pointee.
  if (UnsafeType->isPointerType() && SafeType->isPointerType()) {
    QualType UnsafePointee = UnsafeType->getPointeeType();
    QualType SafePointee = SafeType->getPointeeType();
    const auto *UnsafeFn = UnsafePointee->getAs<FunctionProtoType>();
    const auto *SafeFn = SafePointee->getAs<FunctionProtoType>();
    if (UnsafeFn && SafeFn &&
        UnsafeFn->getNumParams() == SafeFn->getNumParams()) {
      if (!AreTypesCompatibleForUnsafeToSafeRefinement(UnsafeFn->getReturnType(),
                                              SafeFn->getReturnType(), Ctx))
        return false;
      for (unsigned I = 0, E = UnsafeFn->getNumParams(); I != E; ++I)
        if (!AreTypesCompatibleForUnsafeToSafeRefinement(UnsafeFn->getParamType(I),
                                                SafeFn->getParamType(I), Ctx))
          return false;
      return true;
    }
    return AreTypesCompatibleForUnsafeToSafeRefinement(UnsafePointee, SafePointee, Ctx);
  }

  // Arrays: recurse into element types so BSC qualifiers inside array elements
  // (e.g. int *_Owned arr[10]) are checked too.
  if (UnsafeType->isArrayType() && SafeType->isArrayType()) {
    return AreTypesCompatibleForUnsafeToSafeRefinement(
        UnsafeType->getAsArrayTypeUnsafe()->getElementType(),
        SafeType->getAsArrayTypeUnsafe()->getElementType(), Ctx);
  }

  return true;
}

/// Check if two function types are compatible for heterogeneous redeclarations
/// where one is declared safe and the other unsafe.
///
/// Strategy:
/// 1. Use Clang's typesAreCompatible (which automatically strips owned/borrow
///    via mergeTypes, while preserving const/volatile/restrict checking)
/// 2. Add BSC-specific check: ensure owned and borrow are not mixed
bool areFunctionTypesCompatibleForHeterogeneousRedecl(
    ASTContext &Ctx, QualType Type1, QualType Type2,
    SafeZoneSpecifier SZS1, SafeZoneSpecifier SZS2,
    HeterogeneousRedeclMismatchInfo *MismatchOut) {
  using Kind = HeterogeneousRedeclMismatchInfo::Kind;
  auto Report = [&](Kind K, QualType T1, QualType T2, unsigned Idx = 0) {
    if (MismatchOut) {
      MismatchOut->MismatchKind = K;
      MismatchOut->ParamIndex = Idx;
      MismatchOut->Type1 = T1;
      MismatchOut->Type2 = T2;
    }
  };

  bool Type1IsSafe = (SZS1 == SZ_Safe);
  bool Type2IsSafe = (SZS2 == SZ_Safe);
  if (Type1IsSafe == Type2IsSafe) {
    Report(Kind::Other, Type1, Type2);
    return false;
  }

  const FunctionProtoType *FPT1 = Type1->getAs<FunctionProtoType>();
  const FunctionProtoType *FPT2 = Type2->getAs<FunctionProtoType>();
  if (!FPT1 || !FPT2) {
    Report(Kind::Other, Type1, Type2);
    return false;
  }

  if (FPT1->getNumParams() != FPT2->getNumParams()) {
    Report(Kind::ParamCount, Type1, Type2);
    return false;
  }
  if (FPT1->isVariadic() != FPT2->isVariadic()) {
    Report(Kind::Variadic, Type1, Type2);
    return false;
  }

  const FunctionProtoType *UnsafeFPT = Type1IsSafe ? FPT2 : FPT1;
  const FunctionProtoType *SafeFPT = Type1IsSafe ? FPT1 : FPT2;

  // Helper lambda: check if two (possibly function-pointer) types are compatible
  // in the context of a heterogeneous redeclaration. For function pointer types
  // that differ only in SafeZoneSpecifier, we apply the heterogeneous check
  // recursively instead of relying on typesAreCompatible (which rejects
  // safe/unsafe mismatches unconditionally).
  auto AreParamTypesCompatible = [&](QualType UnsafeT, QualType SafeT) -> bool {
    // Save originals for owned/borrow check.
    QualType UnsafeTOrig = UnsafeT;
    QualType SafeTOrig = SafeT;

    // Strip nullability and owned/borrow for base type compatibility checking.
    AttributedType::stripOuterNullability(UnsafeT);
    AttributedType::stripOuterNullability(SafeT);
    UnsafeT.removeLocalNullability(Ctx);
    SafeT.removeLocalNullability(Ctx);
    UnsafeT.removeLocalOwned();
    UnsafeT.removeLocalBorrow();
    UnsafeT.removeLocalArrayElem(Ctx);
    SafeT.removeLocalOwned();
    SafeT.removeLocalBorrow();
    SafeT.removeLocalArrayElem(Ctx);

    // Fast path: identical canonical unqualified types.
    if (UnsafeT.getCanonicalType().getUnqualifiedType() ==
        SafeT.getCanonicalType().getUnqualifiedType()) {
      return AreTypesCompatibleForUnsafeToSafeRefinement(UnsafeTOrig, SafeTOrig, Ctx);
    }

    // If both are function pointer types, check heterogeneous compatibility
    // recursively so that e.g. `func` and `func_safe` are accepted as a
    // compatible pair when used as parameters in a heterogeneous redeclaration.
    if (UnsafeT->isFunctionPointerType() && SafeT->isFunctionPointerType()) {
      QualType UnsafePointee = UnsafeT->getPointeeType();
      QualType SafePointee = SafeT->getPointeeType();
      const FunctionProtoType *UnsafeFP =
          UnsafePointee->getAs<FunctionProtoType>();
      const FunctionProtoType *SafeFP =
          SafePointee->getAs<FunctionProtoType>();
      if (UnsafeFP && SafeFP) {
        SafeZoneSpecifier UnsafeFPSZS = UnsafeFP->getFunSafeZoneSpecifier();
        SafeZoneSpecifier SafeFPSZS = SafeFP->getFunSafeZoneSpecifier();
        // Only treat as a heterogeneous function-pointer pair when one side is
        // safe and the other is not.
        bool UnsafeFPIsSafe = (UnsafeFPSZS == SZ_Safe);
        bool SafeFPIsSafe = (SafeFPSZS == SZ_Safe);
        if (UnsafeFPIsSafe != SafeFPIsSafe) {
          return areFunctionTypesCompatibleForHeterogeneousRedecl(
              Ctx, UnsafePointee, SafePointee, UnsafeFPSZS, SafeFPSZS,
              /*MismatchOut=*/nullptr);
        }
      }
    }

    // General case: use Clang's standard type compatibility check.
    if (!Ctx.typesAreCompatible(UnsafeT, SafeT))
      return false;

    return AreTypesCompatibleForUnsafeToSafeRefinement(UnsafeTOrig, SafeTOrig, Ctx);
  };

  if (!AreParamTypesCompatible(UnsafeFPT->getReturnType(),
                               SafeFPT->getReturnType())) {
    Report(Kind::ReturnType, FPT1->getReturnType(), FPT2->getReturnType());
    return false;
  }

  for (unsigned I = 0, N = FPT1->getNumParams(); I < N; ++I) {
    if (!AreParamTypesCompatible(UnsafeFPT->getParamType(I),
                                 SafeFPT->getParamType(I))) {
      Report(Kind::Parameter, FPT1->getParamType(I), FPT2->getParamType(I),
             I + 1);
      return false;
    }
  }

  return true;
}

QualType applyNullabilityToType(QualType QT, NullabilityKind NK,
                                ASTContext &Ctx) {
  if (NK != NullabilityKind::Nullable && NK != NullabilityKind::NonNull)
    return QT;

  // Prefer QualType ExtQuals bits (BSC's representation) over AttributedType.
  if ((NK == NullabilityKind::Nullable && QT.isNullableQualified()) ||
      (NK == NullabilityKind::NonNull && QT.isNonnullQualified()))
    return QT;

  // Also accept legacy AttributedType sugar with the same kind.
  if (Optional<NullabilityKind> Current = QT->getNullability(Ctx)) {
    if (*Current == NK ||
        (*Current == NullabilityKind::NullableResult &&
         NK == NullabilityKind::Nullable))
      return QT;
  }

  QualType BaseTy = QT;
  BaseTy.removeLocalNullability(Ctx);
  while (BaseTy->getNullability(Ctx))
    BaseTy = BaseTy.getSingleStepDesugaredType(Ctx);

  Qualifiers Qs = BaseTy.getQualifiers();
  Qs.removeNullable();
  Qs.removeNonnull();
  if (NK == NullabilityKind::Nullable)
    Qs.addNullable();
  else
    Qs.addNonnull();
  return Ctx.getQualifiedType(BaseTy.getTypePtr(), Qs);
}

QualType transferExplicitNullability(QualType Src, QualType Dest,
                                     ASTContext &Ctx) {
  if (Optional<NullabilityKind> NK = Src.getExplicitNullability())
    return applyNullabilityToType(Dest, *NK, Ctx);
  return Dest;
}

QualType stripAllNullabilityQualifiers(QualType T, ASTContext &Ctx) {
  T.removeLocalNullability(Ctx);

  if (const auto *PT = T->getAs<PointerType>()) {
    QualType OldPointee = PT->getPointeeType();
    QualType NewPointee = stripAllNullabilityQualifiers(OldPointee, Ctx);
    if (NewPointee.getAsOpaquePtr() == OldPointee.getAsOpaquePtr())
      return T;
    Qualifiers Qs = T.getQualifiers();
    Qs.removeNullable();
    Qs.removeNonnull();
    return Ctx.getQualifiedType(Ctx.getPointerType(NewPointee).getTypePtr(),
                                Qs);
  }

  // Recurse into array element types so pointer nullability inside arrays
  // (e.g. int *_Nullable arr[10]) is stripped too.
  if (const auto *AT = T->getAsArrayTypeUnsafe()) {
    QualType OldElem = AT->getElementType();
    QualType NewElem = stripAllNullabilityQualifiers(OldElem, Ctx);
    if (NewElem.getAsOpaquePtr() == OldElem.getAsOpaquePtr())
      return T;
    if (const auto *CAT = dyn_cast<ConstantArrayType>(AT))
      return Ctx.getConstantArrayType(NewElem, CAT->getSize(),
                                      CAT->getSizeExpr(), CAT->getSizeModifier(),
                                      CAT->getIndexTypeCVRQualifiers());
    if (const auto *VAT = dyn_cast<VariableArrayType>(AT))
      return Ctx.getVariableArrayType(NewElem, VAT->getSizeExpr(),
                                      VAT->getSizeModifier(),
                                      VAT->getIndexTypeCVRQualifiers(),
                                      VAT->getBracketsRange());
    if (const auto *IAT = dyn_cast<IncompleteArrayType>(AT))
      return Ctx.getIncompleteArrayType(NewElem, IAT->getSizeModifier(),
                                        IAT->getIndexTypeCVRQualifiers());
    if (const auto *DSAT = dyn_cast<DependentSizedArrayType>(AT))
      return Ctx.getDependentSizedArrayType(NewElem, DSAT->getSizeExpr(),
                                            DSAT->getSizeModifier(),
                                            DSAT->getIndexTypeCVRQualifiers(),
                                            DSAT->getBracketsRange());
    return T;
  }

  // Recurse into function return/parameter types so pointer nullability inside
  // function types is stripped too.
  if (const auto *FPT = T->getAs<FunctionProtoType>()) {
    QualType OldRet = FPT->getReturnType();
    QualType NewRet = stripAllNullabilityQualifiers(OldRet, Ctx);
    SmallVector<QualType, 4> NewParams;
    bool ParamsChanged = false;
    for (QualType P : FPT->getParamTypes()) {
      QualType NP = stripAllNullabilityQualifiers(P, Ctx);
      NewParams.push_back(NP);
      if (NP.getAsOpaquePtr() != P.getAsOpaquePtr())
        ParamsChanged = true;
    }
    if (NewRet.getAsOpaquePtr() == OldRet.getAsOpaquePtr() && !ParamsChanged)
      return T;
    return Ctx.getFunctionType(NewRet, NewParams, FPT->getExtProtoInfo());
  }

  if (const auto *FNPT = T->getAs<FunctionNoProtoType>()) {
    QualType OldRet = FNPT->getReturnType();
    QualType NewRet = stripAllNullabilityQualifiers(OldRet, Ctx);
    if (NewRet.getAsOpaquePtr() == OldRet.getAsOpaquePtr())
      return T;
    return Ctx.getFunctionNoProtoType(NewRet, FNPT->getExtInfo());
  }

  return T;
}

QualType getOnlyBSCQualifiedTypeWithoutNullability(QualType T,
                                                    ASTContext &Ctx) {
  return stripAllNullabilityQualifiers(T.getOnlyBSCQualifiedType(Ctx), Ctx);
}

/// Recursively check that LHS and RHS have the same effective nullability
/// at every pointer level where both sides are pointer types. Function
/// pointer pointees are traversed too, so nullability inside their return
/// types and parameters is compared recursively.
static bool areTypesNullabilityCompatibleRec(QualType LHS, QualType RHS,
                                             ASTContext &Ctx) {
  if (LHS->isPointerType() && RHS->isPointerType()) {
    if (LHS.getDefNullability() != RHS.getDefNullability())
      return false;
    QualType LPointee = LHS->getPointeeType();
    QualType RPointee = RHS->getPointeeType();
    const auto *LFn = LPointee->getAs<FunctionProtoType>();
    const auto *RFn = RPointee->getAs<FunctionProtoType>();
    if (LFn && RFn) {
      if (LFn->getNumParams() != RFn->getNumParams())
        return false;
      if (!areTypesNullabilityCompatibleRec(LFn->getReturnType(),
                                            RFn->getReturnType(), Ctx))
        return false;
      for (unsigned I = 0, N = LFn->getNumParams(); I != N; ++I)
        if (!areTypesNullabilityCompatibleRec(LFn->getParamType(I),
                                              RFn->getParamType(I), Ctx))
          return false;
      return true;
    }
    return areTypesNullabilityCompatibleRec(LPointee, RPointee, Ctx);
  }
  return true;
}

bool AreFunctionTypesNullabilityCompatible(const FunctionProtoType *LHS,
                                            const FunctionProtoType *RHS,
                                            ASTContext &Ctx) {
  if (!areTypesNullabilityCompatibleRec(LHS->getReturnType(),
                                        RHS->getReturnType(), Ctx))
    return false;
  for (unsigned I = 0, N = LHS->getNumParams();
       I < N && I < RHS->getNumParams(); ++I)
    if (!areTypesNullabilityCompatibleRec(LHS->getParamType(I),
                                          RHS->getParamType(I), Ctx))
      return false;
  return true;
}

} // namespace clang

bool Type::isOwnedStructureType() const {
  if (const auto *RT = getAs<RecordType>())
    return RT->getDecl()->isStruct() && RT->getDecl()->isOwnedDecl();
  return false;
}

bool Type::isOwnedTemplateSpecializationType() const {
  if (const auto *RT = getAs<TemplateSpecializationType>()) {
    if (RT->getTemplateName().getAsTemplateDecl() &&
        RT->getTemplateName().getAsTemplateDecl()->getTemplatedDecl()) {
      if (auto RD = dyn_cast<RecordDecl>(
              RT->getTemplateName().getAsTemplateDecl()->getTemplatedDecl()))
        return RD->isOwnedDecl();
    }
  }
  return false;
}

// Return true when a type is move semantic type,
// including owned pointer(int *owned, int **owned, ...),
// owned struct and struct which has owned fields, for example:
// @code
//     owned struct S1 { };
//     struct S2 { int* owned p; };
//     struct S3 { S1 s; };
//     struct S4 { struct S2 s; };
// @endcode
// These types are not move semantic:
// @code
//     struct S5 { S1* s};
//     struct S6 { int *owned * p};
// @endcode
namespace {
bool isMoveSemanticTypeImpl(QualType QT, llvm::SmallPtrSetImpl<const RecordType *> &Visited) {
  // Owned pointer or owned struct is owned qualified.
  if (QT.isOwnedQualified())
    return true;
  if (const auto *RecTy = dyn_cast<RecordType>(QT)) {
    // Every element in Visited is either:
    // 1. `T t2`   in `struct S { T t1; T t2; };`
    //    In this case, T t1 is visited means T is not move semantic. It is safe to return false for `T t2`.
    // 2. `S s`    in `struct S { S s; };`
    //    In this case, it is a faulty C program. Return something to prevent infinite loop.
    if (!Visited.insert(RecTy).second)
      return false;
    RecordDecl *RD = RecTy->getDecl();
    if (!RD)
      return false;
    for (FieldDecl *FD : RD->fields()) {
      QualType FQT = FD->getType().getCanonicalType();
      if (FQT.isOwnedQualified())
        return true;
      if (const auto *AT = dyn_cast<ArrayType>(FQT)) {
        if (isMoveSemanticTypeImpl(AT->getElementType(), Visited))
          return true;
        continue;
      }
      if (isa<RecordType>(FQT)) {
        if (isMoveSemanticTypeImpl(FQT, Visited))
          return true;
      }
    }
  }
  return false;
}
} // namespace

bool Type::isMoveSemanticType() const {
  llvm::SmallPtrSet<const RecordType *, 8> Visited;
  return isMoveSemanticTypeImpl(CanonicalType, Visited);
}

namespace {
bool isTrivialDataTypeImpl(QualType QT, llvm::SmallPtrSetImpl<const RecordType *> &Visited) {
  if (QT->isFunctionType()) {
    return false;
  }
  if (QT->isPointerType()) {
    return false;
  }
  if (const auto *ArrTy = dyn_cast<ArrayType>(QT)) {
    QualType ET = ArrTy->getElementType().getCanonicalType();
    return isTrivialDataTypeImpl(ET, Visited);
  }
  if (QT->isIncompleteType())
    return false;

  if (const auto *RecTy = dyn_cast<RecordType>(QT)) {
    // Every element in Visited is either:
    // 1. `T t2`   in `struct S { T t1; T t2; };`
    //    In this case, T t1 is visited means T is trivial data. It is safe to return true for `T t2`.
    // 2. `S s`    in `struct S { struct S s; };`
    //    In this case, it is a faulty C program. Return something to prevent infinite loop.
    if (!Visited.insert(RecTy).second)
      return true;
    if (RecordDecl *RD = RecTy->getDecl()) {
      for (FieldDecl *FD : RD->fields()) {
        QualType FQT = FD->getType().getCanonicalType();
        if (FQT.isBorrowQualified() || FQT.isOwnedQualified()) {
          return false;
        }
        if (!isTrivialDataTypeImpl(FQT, Visited)) {
          return false;
        }
      }
    }
  }
  return true;
}
} // namespace

bool Type::isTrivialDataType() const {
  if (CanonicalType.isBorrowQualified() || CanonicalType.isOwnedQualified()) {
    return false;
  }
  llvm::SmallPtrSet<const RecordType *, 8> Visited;
  return isTrivialDataTypeImpl(CanonicalType, Visited);
}

// hasOwnedFields is used to determine whether a type has a field
// that is directly or indirectly qualified by owned.
// If you want to determine whether a type is a move semantic type,
// use isMoveSemanticType instead.
bool RecordType::hasOwnedFields() const {
  llvm::SmallPtrSet<const RecordType *, 16> Visited;
  llvm::SmallVector<const RecordType *, 16> Queue;
  Queue.push_back(this);
  Visited.insert(this);
  for (unsigned i = 0; i < Queue.size(); ++i) {
    // traverse all fields
    for (FieldDecl *FD : Queue[i]->getDecl()->fields()) {
      // basic case
      QualType FieldTy = FD->getType().getCanonicalType();
      if (FieldTy.isOwnedQualified() || FieldTy->isOwnedStructureType()) {
        return true;
      }
      while (const auto *AT = dyn_cast<ArrayType>(FieldTy))
        FieldTy = AT->getElementType().getCanonicalType();
      if (FieldTy.isOwnedQualified() || FieldTy->isOwnedStructureType()) {
        return true;
      }
      // pointer: dereference to the final pointee
      QualType TempQT = FieldTy;
      for (const Type *TempT = TempQT.getTypePtr(); TempT->isPointerType();
           TempT = TempQT.getTypePtr()) {
        TempQT = TempT->getPointeeType().getCanonicalType();
        if (TempQT.isOwnedQualified() && !TempQT->isOwnedStructureType()) {
          return true;
        }
      }
      FieldTy = TempQT.getCanonicalType();
      if (const auto *FieldRecTy = FieldTy->getAs<RecordType>()) {
        if (Visited.insert(FieldRecTy).second) {
          Queue.push_back(FieldRecTy);
        }
      }
    }
  }
  return false;
}

bool RecordType::hasBorrowFields() const {
  llvm::SmallPtrSet<const RecordType *, 16> Visited;
  llvm::SmallVector<const RecordType *, 16> Queue;
  Queue.push_back(this);
  Visited.insert(this);
  for (unsigned i = 0; i < Queue.size(); ++i) {
    // traverse all fields
    for (FieldDecl *FD : Queue[i]->getDecl()->fields()) {
      // basic case
      QualType FieldTy = FD->getType();
      if (FieldTy.isBorrowQualified()) {
        return true;
      }
      // pointer: dereference to the final pointee
      QualType TempQT = FieldTy;
      for (const Type *TempT = TempQT.getTypePtr(); TempT->isPointerType();
           TempT = TempQT.getTypePtr()) {
        TempQT = TempT->getPointeeType();
        if (TempQT.isBorrowQualified()) {
          return true;
        }
        TempQT = TempQT.getCanonicalType();
      }
      FieldTy = TempQT.getCanonicalType();
      // extend the bfs frontier
      if (const auto *FieldRecTy = FieldTy->getAs<RecordType>()) {
        if (Visited.insert(FieldRecTy).second)
          Queue.push_back(FieldRecTy);
      }
    }
  }
  return false;
}

bool RecordType::withBorrowFields() const {
  llvm::SmallPtrSet<const RecordType *, 16> Visited;
  return withBorrowFieldsImpl(QualType(this, 0), Visited);
}

bool Type::isBSCFutureType() const {
  if (const auto *RT = getAs<RecordType>()) {
    RecordDecl *RD = RT->getAsRecordDecl();
    if (isa<ClassTemplateSpecializationDecl>(RD)) {
      return RD->getNameAsString() == "__Trait_Future";
    }
  }
  return false;
}

bool Type::isBSCTemplateRecordType() const {
  if (const auto *RT = getAs<RecordType>()) {
    RecordDecl *RD = RT->getAsRecordDecl();
    return isa<ClassTemplateSpecializationDecl>(RD);
  }
  return false;
}

ConditionalType::ConditionalType(llvm::Optional<bool> CondRes, Expr *CondE,
                                 QualType T1, QualType T2, QualType can)
    : Type(Conditional, can.isNull() ? QualType(this, 0) : can,
           toTypeDependence(CondE->getDependence()) |
               (CondE->isInstantiationDependent() ? TypeDependence::Dependent
                                                  : TypeDependence::None) |
               (CondE->getType()->getDependence() &
                TypeDependence::VariablyModified) |
               T1->getDependence() | T2->getDependence()),
      CondResult(CondRes), CondExpr(CondE), Type1(T1), Type2(T2),
      UnderlyingType(can) {}

bool ConditionalType::isSugared() const {
  // A ConditionalType whose condition could not be resolved has no
  // underlying type to desugar to; treat it as unsugared so that
  // desugar() never yields a NULL QualType.
  return !CondExpr->isInstantiationDependent() && !getUnderlyingType().isNull();
}

QualType ConditionalType::desugar() const {
  if (isSugared())
    return getUnderlyingType();

  return QualType(this, 0);
}

bool QualType::hasOwned() const {
  if (isOwnedQualified())
    return true;
  return getTypePtr()->hasOwnedFields();
}

bool QualType::hasBorrow() const {
  if (isBorrowQualified())
    return true;
  return getTypePtr()->hasBorrowFields();
}

bool QualType::isConstBorrow() const {
  if (!isBorrowQualified())
    return false;
  if (!getTypePtr()->isPointerType())
    return false;
  QualType directPointee = getTypePtr()->getPointeeType();
  return directPointee.isConstQualified();
}

bool QualType::isConstPointee() const {
  QualType QT = QualType(getTypePtr(), getLocalFastQualifiers());
  while (QT->isPointerType()) {
      QT = QT->getPointeeType();
  }
  if (QT.isLocalConstQualified())
      return true;
  return false;
}

QualType QualType::addConstBorrow(const ASTContext &Context) {
  QualType pointee;
  if (getTypePtr()->isPointerType()) {
    // Use the pointee type as stored (preserve sugar) so the result type prints
    // without an extra tag (e.g. "const s<int> *_Borrow" not "const struct s<int> *_Borrow").
    pointee = getTypePtr()->getPointeeType();
  } else {
    // Non-pointer: &_Const applied to a value of type T (e.g. *a with type s<T>)
    // yields const T* _Borrow. Use the type as-is so printing matches the
    // operand (e.g. "s<int>" not "struct s<int>").
    pointee = *this;
  }
  pointee.addConst();  // Add const to the (direct) pointee (the borrowed object)
  QualType result = Context.getPointerType(pointee);
  result.addBorrow();
  // Preserve BSC semantic qualifiers that describe the borrow itself:
  // _ArrayElem and explicit nullability (_Nullable/_Nonnull) survive a
  // mutable-to-const reborrow. _Owned is intentionally not carried over:
  // a const borrow is not an owned pointer (callers strip _Owned before
  // invoking this helper).
  Qualifiers Qs = result.getQualifiers();
  if (isArrayElemQualified())
    Qs.addArrayElem();
  if (isNullableQualified())
    Qs.addNullable();
  if (isNonnullQualified())
    Qs.addNonnull();
  return Context.getQualifiedType(result.getTypePtr(), Qs);
}

#endif
