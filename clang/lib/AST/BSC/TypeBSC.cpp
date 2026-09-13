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
      if (!Type.isRawPointer())
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
      } else if (Type.isOwnedPointer()) {
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

bool holdsBSC(QualType QT, BSCPointerKind Kind, BSCLookThrough LookThrough,
              bool ViaPointer,
              llvm::SmallPtrSetImpl<const RecordType *> &Visited);

BSCLookThrough lookThroughBitFor(QualType Pointer) {
  switch (Pointer.getBSCPointerProperties().Kind) {
  case BPK_Owned:
    return BSCLookThrough::OwnedPointers;
  case BPK_Borrow:
    return BSCLookThrough::BorrowPointers;
  case BPK_None:
    return BSCLookThrough::RawPointers;
  }
  llvm_unreachable("bad BSCPointerKind");
}

// The members of \p QT, looking through \p LookThrough pointers.
bool containsBSCImpl(QualType QT, BSCPointerKind Kind,
                     BSCLookThrough LookThrough,
                     llvm::SmallPtrSetImpl<const RecordType *> &Visited) {
  QT = QT.getCanonicalType();
  if (const auto *AT = dyn_cast<ArrayType>(QT))
    return holdsBSC(AT->getElementType(), Kind, LookThrough, false, Visited);
  if (QT->isPointerType()) {
    bool Cross =
        (LookThrough & lookThroughBitFor(QT)) != BSCLookThrough::NoPointer;
    return Cross &&
           holdsBSC(QT->getPointeeType(), Kind, LookThrough, true, Visited);
  }
  const auto *RT = dyn_cast<RecordType>(QT);
  if (!RT || !RT->getDecl() || !Visited.insert(RT).second)
    return false;
  // A union may not hold such a pointer, so only error-recovery ASTs get here.
  if (RT->getDecl()->isUnion())
    return false;
  for (FieldDecl *FD : RT->getDecl()->fields())
    if (holdsBSC(FD->getType(), Kind, LookThrough, false, Visited))
      return true;
  return false;
}

// \p QT itself, or its members.  A decl-owned struct is an owned member only
// when held by value, never when merely pointed to.
bool holdsBSC(QualType QT, BSCPointerKind Kind, BSCLookThrough LookThrough,
              bool ViaPointer,
              llvm::SmallPtrSetImpl<const RecordType *> &Visited) {
  QT = QT.getCanonicalType();
  // As written for either kind; ownership also comes from a declaration.
  bool Self = Kind == BPK_Owned
                  ? (QT.isOwnedQualified() || (!ViaPointer && QT->isOwnedStruct()))
                  : QT.isBorrowQualified();
  return Self || containsBSCImpl(QT, Kind, LookThrough, Visited);
}
} // namespace

bool Type::containsBSC(BSCPointerKind Kind, BSCLookThrough LookThrough) const {
  llvm::SmallPtrSet<const RecordType *, 16> Visited;
  return containsBSCImpl(QualType(this, 0), Kind, LookThrough, Visited);
}

bool QualType::isOrContainsBSC(BSCPointerKind Kind,
                               BSCLookThrough LookThrough) const {
  llvm::SmallPtrSet<const RecordType *, 16> Visited;
  return holdsBSC(*this, Kind, LookThrough, false, Visited);
}

bool QualType::isOrContainsOwned(BSCLookThrough LookThrough) const {
  return isOrContainsBSC(BPK_Owned, LookThrough);
}

bool QualType::isOrContainsBorrow(BSCLookThrough LookThrough) const {
  return isOrContainsBSC(BPK_Borrow, LookThrough);
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
        if (FQT->isOwnedStruct())
          return false;
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
  if (CanonicalType->isOwnedStruct())
    return false;
  llvm::SmallPtrSet<const RecordType *, 8> Visited;
  return isTrivialDataTypeImpl(CanonicalType, Visited);
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
    if (Type.isOwnedPointer())
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

using TypePairPred = llvm::function_ref<bool(QualType, QualType)>;

// Applies Ok here, then through array elements, pointees and prototype return
// and parameter slots, so a qualifier buried under a pointer is never skipped.
static bool holdsAtEveryPointerLevel(QualType L, QualType R, TypePairPred Ok) {
  if (!Ok(L, R))
    return false;
  if (L->isArrayType() && R->isArrayType())
    return holdsAtEveryPointerLevel(
        L->getAsArrayTypeUnsafe()->getElementType(),
        R->getAsArrayTypeUnsafe()->getElementType(), Ok);
  const auto *LFn = L->getAs<FunctionProtoType>();
  const auto *RFn = R->getAs<FunctionProtoType>();
  if (LFn && RFn) {
    if (LFn->getNumParams() != RFn->getNumParams())
      return true;
    if (!holdsAtEveryPointerLevel(LFn->getReturnType(), RFn->getReturnType(),
                                  Ok))
      return false;
    for (unsigned I = 0, E = LFn->getNumParams(); I != E; ++I)
      if (!holdsAtEveryPointerLevel(LFn->getParamType(I), RFn->getParamType(I),
                                    Ok))
        return false;
    return true;
  }
  if (!L->isPointerType() || !R->isPointerType())
    return true;
  return holdsAtEveryPointerLevel(L->getPointeeType(), R->getPointeeType(), Ok);
}

QualType mergeNullabilityAtEveryPointerLevel(const ASTContext &Ctx, QualType T,
                                             QualType L, QualType R) {
  const auto *TP = T->getAs<PointerType>();
  const auto *LP = L->getAs<PointerType>();
  const auto *RP = R->getAs<PointerType>();
  if (!TP || !LP || !RP)
    return T;
  QualType Pointee = mergeNullabilityAtEveryPointerLevel(
      Ctx, TP->getPointeeType(), LP->getPointeeType(), RP->getPointeeType());
  BSCPointerProperties P = T.getBSCPointerProperties();
  // A level that already reads as nullable keeps the spelling it was given.
  if ((L.getDefNullability() == NullabilityKind::Nullable ||
       R.getDefNullability() == NullabilityKind::Nullable) &&
      T.getDefNullability() != NullabilityKind::Nullable)
    P.Nullability = BWN_Nullable;
  if (Pointee == TP->getPointeeType() && P == T.getBSCPointerProperties())
    return T;
  return Ctx.getQualifiedType(Ctx.getPointerType(Pointee, P),
                              T.getQualifiers());
}

// Manual 3.8.3 rule 3: same qualifiers once nullability defaults are filled.
static bool stickyQualifiersEqual(QualType L, QualType R) {
  return L.getBSCPointerProperties().stickyMatches(
             R.getBSCPointerProperties()) &&
         L.getDefNullability() == R.getDefNullability();
}

// Manual 3.6.5.4 rule 2: _Unsafe may omit a sticky qualifier, else must match;
// _Unsafe _Nonnull against _Safe _Nullable is not allowed.
static bool unsafeSafeRefinementAtLevel(QualType Unsafe, QualType Safe) {
  BSCPointerProperties U = Unsafe.getBSCPointerProperties();
  BSCPointerProperties S = Safe.getBSCPointerProperties();
  if (U.Kind != BPK_None && !U.stickyMatches(S))
    return false;
  return !(Unsafe.getDefNullability() == NullabilityKind::NonNull &&
           Safe.getDefNullability() == NullabilityKind::Nullable);
}

bool areBSCTypesCompatible(QualType L, QualType R) {
  return holdsAtEveryPointerLevel(L, R, stickyQualifiersEqual);
}

bool satisfiesUnsafeSafeRefinement(QualType Unsafe, QualType Safe) {
  return holdsAtEveryPointerLevel(Unsafe, Safe, unsafeSafeRefinementAtLevel);
}

static bool carriesOwnership(const FunctionProtoType *F) {
  auto Carries = [](QualType T) {
    return T.isOrContainsOwned(BSCLookThrough::AnyPointer) ||
           T.isOrContainsBorrow(BSCLookThrough::AnyPointer);
  };
  return Carries(F->getReturnType()) || llvm::any_of(F->param_types(), Carries);
}

BSCFunctionMismatch firstBSCFunctionTypeMismatch(const ASTContext &Ctx,
                                                 const FunctionProtoType *L,
                                                 const FunctionProtoType *R) {
  if (L->getNumParams() != R->getNumParams())
    return carriesOwnership(L) || carriesOwnership(R)
               ? BSCFunctionMismatch::Incompatible
               : BSCFunctionMismatch::None;
  BSCFunctionMismatch M = BSCFunctionMismatch::None;
  holdsAtEveryPointerLevel(QualType(L, 0), QualType(R, 0), [&](QualType A,
                                                                QualType B) {
    bool HasOwned =
        A.isOwnedPointerOrOwnedStruct() || B.isOwnedPointerOrOwnedStruct();
    BSCPointerProperties PA = A.getBSCPointerProperties();
    BSCPointerProperties PB = B.getBSCPointerProperties();
    if (!PA.stickyMatches(PB))
      M = HasOwned ? BSCFunctionMismatch::Owned : BSCFunctionMismatch::Borrow;
    else if (A->isPointerType() && B->isPointerType() &&
             A.getDefNullability() != B.getDefNullability())
      M = BSCFunctionMismatch::Incompatible;
    else if ((HasOwned || PA.Kind == BPK_Borrow) &&
             Ctx.getTypeWithoutCVRAndNullability(A).getCanonicalType() !=
                 Ctx.getTypeWithoutCVRAndNullability(B).getCanonicalType())
      M = BSCFunctionMismatch::Incompatible;
    return M == BSCFunctionMismatch::None;
  });
  return M;
}

/// Manual 3.6.5.4, unsafe-safe refinement relation: the _Safe type, with the
/// BiSheng C safety features removed, is compatible with the _Unsafe type.
bool functionTypeSatisfiesUnsafeSafeRefinement(
    ASTContext &Ctx, QualType Type1, QualType Type2,
    SafeZoneSpecifier SZS1, SafeZoneSpecifier SZS2,
    UnsafeSafeRefinementMismatchInfo *MismatchOut) {
  using Kind = UnsafeSafeRefinementMismatchInfo::Kind;
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

  // Function-pointer parameters differing only in _Safe recurse into the
  // relation; typesAreCompatible would reject the _Safe mismatch outright.
  auto AreParamTypesCompatible = [&](QualType UnsafeT, QualType SafeT) -> bool {
    // Save originals for owned/borrow check.
    QualType UnsafeTOrig = UnsafeT;
    QualType SafeTOrig = SafeT;

    // C compatibility treats a Kind mismatch as incompatible, but the
    // refinement lets the _Unsafe side omit a Kind at any pointer level, so
    // strip the properties everywhere before asking; the relation itself is
    // checked on the originals below.
    AttributedType::stripOuterNullability(UnsafeT);
    AttributedType::stripOuterNullability(SafeT);
    auto Erase = [](BSCPointerProperties) { return BSCPointerProperties(); };
    UnsafeT = Ctx.mapBSCPropertiesAtEveryPointerLevel(UnsafeT, Erase);
    SafeT = Ctx.mapBSCPropertiesAtEveryPointerLevel(SafeT, Erase);

    // Fast path: identical canonical unqualified types.
    if (UnsafeT.getCanonicalType().getUnqualifiedType() ==
        SafeT.getCanonicalType().getUnqualifiedType()) {
      return satisfiesUnsafeSafeRefinement(UnsafeTOrig, SafeTOrig);
    }

    // Manual 3.6.5.4 rule 1 applied to a function-pointer parameter pair.
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
        // Only a pair with exactly one _Safe side is a refinement pair.
        bool UnsafeFPIsSafe = (UnsafeFPSZS == SZ_Safe);
        bool SafeFPIsSafe = (SafeFPSZS == SZ_Safe);
        if (UnsafeFPIsSafe != SafeFPIsSafe) {
          return functionTypeSatisfiesUnsafeSafeRefinement(
              Ctx, UnsafePointee, SafePointee, UnsafeFPSZS, SafeFPSZS,
              /*MismatchOut=*/nullptr);
        }
      }
    }

    // General case: use Clang's standard type compatibility check.
    if (!Ctx.typesAreCompatible(UnsafeT, SafeT))
      return false;

    return satisfiesUnsafeSafeRefinement(UnsafeTOrig, SafeTOrig);
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

} // namespace clang

bool Type::isOwnedStruct() const {
  if (const auto *RT = getAs<RecordType>())
    return RT->getDecl()->isStruct() && RT->getDecl()->isOwnedDecl();
  if (const auto *TST = getAs<TemplateSpecializationType>()) {
    if (TST->isTypeAlias())
      return TST->getAliasedType()->isOwnedStruct();
    if (TemplateDecl *TD = TST->getTemplateName().getAsTemplateDecl())
      if (const auto *RD = dyn_cast_or_null<RecordDecl>(TD->getTemplatedDecl()))
        return RD->isOwnedDecl();
  }
  return false;
}

QualType clang::getInnermostPointeeType(QualType T) {
  while (T->isPointerType())
    T = T->getPointeeType();
  return T;
}

QualType clang::stripTypedefsAndAliasTemplates(QualType T) {
  while (true) {
    if (const auto *TST = dyn_cast<TemplateSpecializationType>(T)) {
      if (!TST->isTypeAlias())
        return T;
      T = TST->getAliasedType();
    } else if (const auto *TT = dyn_cast<TypedefType>(T)) {
      T = TT->desugar();
    } else {
      return T;
    }
  }
}

RecordDecl *clang::getRecordDeclThroughSpecialization(QualType T) {
  RecordDecl *RD = T->getAsRecordDecl();
  if (!RD)
    return nullptr;
  if (const auto *TST = dyn_cast<TemplateSpecializationType>(T))
    if (TemplateDecl *TD = TST->getTemplateName().getAsTemplateDecl())
      RD = dyn_cast_or_null<RecordDecl>(TD->getTemplatedDecl());
  return RD;
}

// Recursively determine whether a record contains a pointer field with the
// given nullability, reachable through embedded struct/array fields but NOT
// through pointer pointees (which live in separate allocations and are not
// reachable from the record itself).
static bool hasFieldWithNullability(const RecordDecl *RD,
                                    NullabilityKind Kind) {
  if (!RD)
    return false;
  for (const FieldDecl *FD : RD->fields()) {
    QualType FT = FD->getType();
    if (FT->isPointerType()) {
      if (FT.getDefNullability() == Kind)
        return true;
    } else if (FT->isArrayType()) {
      QualType ElemTy = FT;
      while (const auto *AT = dyn_cast<ArrayType>(ElemTy))
        ElemTy = AT->getElementType();
      if (ElemTy->isPointerType()) {
        if (ElemTy.getDefNullability() == Kind)
          return true;
      } else if (const RecordType *RT = ElemTy->getAs<RecordType>()) {
        if (hasFieldWithNullability(RT->getDecl(), Kind))
          return true;
      }
    } else if (const RecordType *RT = FT->getAs<RecordType>()) {
      if (hasFieldWithNullability(RT->getDecl(), Kind))
        return true;
    }
  }
  return false;
}

bool RecordType::hasNonnullFields() const {
  return hasFieldWithNullability(getDecl(), NullabilityKind::NonNull);
}

bool RecordType::hasNullableFields() const {
  return hasFieldWithNullability(getDecl(), NullabilityKind::Nullable);
}

bool clang::isDesugaredFromTraitType(QualType T) {
  RecordDecl *RD =
      getRecordDeclThroughSpecialization(getInnermostPointeeType(T));
  return RD && RD->getDesugaredTraitDecl();
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

bool QualType::isConstBorrow() const {
  return isBorrowPointer() &&
         getTypePtr()->getPointeeType().isConstQualified();
}




#endif
