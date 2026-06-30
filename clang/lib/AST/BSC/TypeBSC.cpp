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

#include "clang/AST/Decl.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/SmallPtrSet.h"

using namespace clang;

namespace {
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

/// Check owned/borrow qualifier compatibility for heterogeneous redeclarations.
/// UnsafeType is the unsafe decl, SafeType is the _Safe redeclaration.
/// The _Safe redeclaration may only add qualifiers, not remove them.
/// Exception: _ArrayElem must not appear on the safe side when the unsafe
/// pointer is _Owned or _Borrow without _ArrayElem.
static bool AreOwnedBorrowQualifiersCompatible(QualType UnsafeType,
                                               QualType SafeType) {
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
  // owned ⟷ borrow is always incompatible.
  if ((SafeIsOwned && UnsafeIsBorrow) || (SafeIsBorrow && UnsafeIsOwned))
    return false;

  // Peel one pointer layer and recurse so a buried qualifier is not dropped:
  // into the pointee's function prototype if it has one, else into the pointee.
  if (UnsafeType->isPointerType() && SafeType->isPointerType()) {
    QualType UnsafePointee = UnsafeType->getPointeeType();
    QualType SafePointee = SafeType->getPointeeType();
    const auto *UnsafeFn = UnsafePointee->getAs<FunctionProtoType>();
    const auto *SafeFn = SafePointee->getAs<FunctionProtoType>();
    if (UnsafeFn && SafeFn &&
        UnsafeFn->getNumParams() == SafeFn->getNumParams()) {
      if (!AreOwnedBorrowQualifiersCompatible(UnsafeFn->getReturnType(),
                                              SafeFn->getReturnType()))
        return false;
      for (unsigned I = 0, E = UnsafeFn->getNumParams(); I != E; ++I)
        if (!AreOwnedBorrowQualifiersCompatible(UnsafeFn->getParamType(I),
                                                SafeFn->getParamType(I)))
          return false;
      return true;
    }
    return AreOwnedBorrowQualifiersCompatible(UnsafePointee, SafePointee);
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
    UnsafeT.removeLocalOwned();
    UnsafeT.removeLocalBorrow();
    UnsafeT.removeLocalArrayElem(Ctx);
    SafeT.removeLocalOwned();
    SafeT.removeLocalBorrow();
    SafeT.removeLocalArrayElem(Ctx);

    // Fast path: identical canonical unqualified types.
    if (UnsafeT.getCanonicalType().getUnqualifiedType() ==
        SafeT.getCanonicalType().getUnqualifiedType()) {
      return AreOwnedBorrowQualifiersCompatible(UnsafeTOrig, SafeTOrig);
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

    return AreOwnedBorrowQualifiersCompatible(UnsafeTOrig, SafeTOrig);
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
    : Type(Conditional, can,
           toTypeDependence(CondE->getDependence()) |
               (CondE->isInstantiationDependent() ? TypeDependence::Dependent
                                                  : TypeDependence::None) |
               (CondE->getType()->getDependence() &
                TypeDependence::VariablyModified) |
               T1->getDependence() | T2->getDependence()),
      CondResult(CondRes), CondExpr(CondE), Type1(T1), Type2(T2),
      UnderlyingType(can) {}

bool ConditionalType::isSugared() const {
  return !CondExpr->isInstantiationDependent();
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
  return result;
}

QualType QualType::removeConstForBorrow(const ASTContext &Context) {
  // Only applies to pointer types (e.g. const int * from dereferencing const int * borrow).
  // For non-pointer types (e.g. struct S from dereferencing struct S * borrow),
  // return unchanged - no const to remove.
  if (!getTypePtr()->isPointerType())
    return *this;
  QualType directPointee = getTypePtr()->getPointeeType();
  directPointee.removeLocalConst();
  QualType result = Context.getPointerType(directPointee);
  // Preserve owned/borrow qualifiers from the original pointer type.
  if (isOwnedQualified())
    result.addOwned();
  if (isBorrowQualified())
    result.addBorrow();
  return result;
}

#endif
