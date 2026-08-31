//===--- SemaBSCOwnership.cpp - Semantic Analysis for BSC Ownership
//----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements dataflow analysis for BSC Ownership.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/AST/BSC/TypeBSC.h"
#include "clang/AST/Type.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Sema/Sema.h"

using namespace clang;
using namespace sema;

namespace {
QualType StripLocalArrayElemQualifier(ASTContext &Context, QualType QT) {
  if (QT.isNull() || !QT.isArrayElemQualified())
    return QT;
  QT.removeLocalArrayElem(Context);
  return QT;
}

bool CheckArrayElemQualifierRules(Sema &S, QualType T, SourceLocation Loc) {
  llvm::SmallVector<QualType, 4> Worklist;
  Worklist.push_back(T);

  while (!Worklist.empty()) {
    QualType Current = Worklist.pop_back_val();

    // Array types inherit _ArrayElem from their element the real qualifier
    // lives on the element pointer, so skip the array itself.
    if (Current.isArrayElemQualified() && !Current->isArrayType()) {
      if (!Current->isPointerType() && !Current->isDependentType()) {
        S.Diag(Loc, diag::err_owned_qualifier_non_pointer)
            << "_ArrayElem"
            << StripLocalArrayElemQualifier(S.Context, Current);
        return false;
      }

      if (!Current->isDependentType() && !Current.isOwnedQualified() &&
          !Current.isBorrowQualified()) {
        S.Diag(Loc, diag::err_arrayelem_requires_safe_pointer);
        return false;
      }
    }

    if (const auto *PT = Current->getAs<PointerType>())
      Worklist.push_back(PT->getPointeeType());
    if (const auto *AT = Current->getAsArrayTypeUnsafe())
      Worklist.push_back(AT->getElementType());
  }

  return true;
}
}

// for union fields/array/global variable type check
void Sema::CheckOwnedOrIndirectOwnedType(SourceLocation ErrLoc, QualType T, StringRef Env) {
  enum {
    ownedQualified,
    ownedTypedef,
    ownedFields
  };
  // Peel array types so arrays of owned / move-semantic element types are
  // caught too (e.g. `static struct A ls_arr[3]` where A has owned fields).
  while (const auto *AT = T->getAsArrayTypeUnsafe())
    T = AT->getElementType();
  if (T.getCanonicalType().isOwnedQualified() && !T.getTypePtr()->getAs<TypedefType>()) {
    Diag(ErrLoc, diag::err_nested_owned_borrow_type_check)
        << ownedQualified << "_Owned" << Env;
  } else if (T.getCanonicalType().isOwnedQualified() && T.getTypePtr()->getAs<TypedefType>()) {
    Diag(ErrLoc, diag::err_nested_owned_borrow_type_check)
        << ownedTypedef << "_Owned" << Env << T;
  } else if (T.getCanonicalType().getTypePtr()->isMoveSemanticType()) {
    Diag(ErrLoc, diag::err_nested_owned_borrow_type_check)
        << ownedFields << "_Owned" << Env << T;
  }
}

// Check that owned qualifiers on an instantiated type are valid.
// This is called after template instantiation to validate that template
// parameters which were qualified with 'owned' instantiate to pointer types
// (or other valid owned types).
//
// For example:
//   template<typename T> void f(owned T t);
//   f<int>(x);  // Error: 'int' cannot be qualified by 'owned'
//   f<int*>(x); // OK: 'int*' can be qualified by 'owned'
//
// Returns true if the type is valid, false if an error was reported.
bool Sema::CheckInstantiatedTypeOwnedQualifiers(QualType T, SourceLocation Loc) {
  if (!getLangOpts().BSC)
    return true;

  // Helper to check if a type can validly have owned qualifier
  auto isValidOwnedType = [](QualType Ty) {
    return (Ty->isPointerType() && !Ty->isFunctionPointerType()) ||
           Ty->isOwnedStructureType() ||
           Ty->isOwnedTemplateSpecializationType() ||
           Ty->isArrayType(); // arrays with _Owned element types are valid
  };

  // Check owned qualifier
  if (T.isOwnedQualified() && !isValidOwnedType(T)) {
    QualType UnqualType = T;
    UnqualType.removeLocalFastQualifiers(Qualifiers::Owned);
    Diag(Loc, diag::err_owned_qualifier_non_pointer) << "_Owned" << UnqualType;
    return false;
  }

  return true;
}

bool Sema::CheckInstantiatedTypeBorrowQualifiers(QualType T,
                                                 SourceLocation Loc) {
  if (T.isBorrowQualified() && T->isFunctionPointerType()) {
    QualType UnqualType = T;
    UnqualType.removeLocalBorrow();
    Diag(Loc, diag::err_owned_qualifier_non_pointer) << "_Borrow" << UnqualType;
    return false;
  }

  return true;
}

bool Sema::CheckInstantiatedTypeArrayElemQualifiers(QualType T,
                                                    SourceLocation Loc) {
  if (!getLangOpts().BSC)
    return true;
  return CheckArrayElemQualifierRules(*this, T, Loc);
}

// Check if 'owned' qualifier is applied to a non-pointer type
// The 'owned' qualifier is only valid on:
//   - Pointer types (e.g., int* owned, int** owned *)
//   - Owned structure types
//   - Owned template specialization types
//   - Dependent types (template parameters - checked at instantiation)
// Invalid examples:
//   - owned int x           (owned on primitive type)
//   - owned int * a[3]      (owned int inside array of pointers)
void Sema::CheckOwnedQualifierOnNonPointerType(const DeclSpec &DS, QualType T) {
  // Early exit if feature is disabled or owned not explicitly specified
  if (!getLangOpts().BSC || !DS.getOwnedSpecLoc().isValid())
    return;

  // Helper to check if a type can validly have owned qualifier
  // Returns true for pointers, owned structures, owned template types, and dependent types
  // Dependent types (like template parameters) are allowed - validity checked at instantiation
  auto isValidOwnedType = [](QualType Ty) {
    return Ty->isPointerType() || Ty->isOwnedStructureType() ||
           Ty->isOwnedTemplateSpecializationType() || Ty->isDependentType();
  };

  // Helper to emit diagnostic with unqualified type
  // Removes 'owned' qualifier before displaying the type in error message
  auto emitDiagnostic = [&](QualType Ty) {
    QualType UnqualType = Ty;
    UnqualType.removeLocalFastQualifiers(Qualifiers::Owned);
    Diag(DS.getOwnedSpecLoc(), diag::err_owned_qualifier_non_pointer)
        << "_Owned" << UnqualType;
  };

  // First Check: Deep type analysis
  // Strip all pointer and array levels to reach the innermost base type
  // Example: owned int **p → strip 2 levels → owned int (base type)
  QualType BaseType = T;
  while (const auto *PT = BaseType->getAs<PointerType>())
    BaseType = PT->getPointeeType();
  if (const auto *AT = BaseType->getAsArrayTypeUnsafe())
    BaseType = AT->getElementType();

  // Check if the innermost base type incorrectly has 'owned' on a non-pointer
  // This catches: owned int **p (where 'owned int' is at the base)
  if (BaseType.isOwnedQualified() && !isValidOwnedType(BaseType)) {
    emitDiagnostic(BaseType);
    return;
  }

  // Second Check: Top-level type analysis
  // Handle cases where the complete type declaration is invalid
  // Example: owned int x, or owned int * a[3]
  if (!isValidOwnedType(T)) {
    QualType CheckType = T;
    // Strip at most one array level, then one pointer level
    // For "owned int * a[3]": strip array → owned int*, then strip pointer → owned int
    if (const auto *AT = CheckType->getAsArrayTypeUnsafe())
      CheckType = AT->getElementType();
    if (const auto *PT = CheckType->getAs<PointerType>())
      CheckType = PT->getPointeeType();

    // If we found 'owned' after stripping, it's invalid
    if (CheckType.isOwnedQualified())
      emitDiagnostic(CheckType);
  }
}

void Sema::CheckArrayElemQualifierOnType(const DeclSpec &DS, QualType T,
                                         SourceLocation DiagLoc) {
  if (!getLangOpts().BSC)
    return;

  if (DS.getArrayElemSpecLoc().isValid())
    DiagLoc = DS.getArrayElemSpecLoc();
  (void)CheckArrayElemQualifierRules(*this, T, DiagLoc);
}

namespace {
bool IsOwnedRawPointerCastDisallowed(QualType LHSCanType, QualType RHSCanType) {
  const auto *LHSPtrType = LHSCanType->getAs<PointerType>();
  const auto *RHSPtrType = RHSCanType->getAs<PointerType>();
  if (!LHSPtrType || !RHSPtrType)
    return false;

  bool LHSOwned = LHSCanType.isOwnedQualified();
  bool RHSOwned = RHSCanType.isOwnedQualified();
  bool LHSRaw = !LHSOwned && !LHSCanType.isBorrowQualified();
  bool RHSRaw = !RHSOwned && !RHSCanType.isBorrowQualified();
  if ((LHSOwned && RHSRaw) || (RHSOwned && LHSRaw))
    return true;

  return false;
}
} // end anonymous namespace

bool Sema::CheckOwnedQualTypeCStyleCast(QualType LHSType, QualType RHSType) {
  QualType RHSCanType = RHSType.getCanonicalType();
  QualType LHSCanType = LHSType.getCanonicalType();

  // Allow owned pointer to be cast from nullptr_t
  if (LHSCanType.isOwnedQualified() && LHSCanType->isPointerType() &&
      RHSCanType->isNullPtrType()) {
    return true;
  }

  bool IsSameType = (LHSCanType.getTypePtr() == RHSCanType.getTypePtr());
  const auto *LHSPtrType = LHSType->getAs<PointerType>();
  const auto *RHSPtrType = RHSType->getAs<PointerType>();
  bool IsPointer = LHSPtrType && RHSPtrType;

  if (RHSCanType->isDependentType()) {
    return true;
  }

  if (IsPointer) {
    bool LHSOwned = LHSCanType.isOwnedQualified();
    bool RHSOwned = RHSCanType.isOwnedQualified();
    bool LHSBorrow = LHSCanType.isBorrowQualified();
    bool RHSBorrow = RHSCanType.isBorrowQualified();
    bool LHSRaw = !LHSOwned && !LHSBorrow;
    bool RHSRaw = !RHSOwned && !RHSBorrow;
    // Disallow conversion between owned and borrow pointers
    if ((LHSBorrow && RHSOwned) || (LHSOwned && RHSBorrow)) {
      return false;
    }
    // Disallow conversion between owned and raw pointers
    if ((LHSRaw && RHSOwned) || (LHSOwned && RHSRaw)) {
      return false;
    }
    if (LHSType.isArrayElemQualified() != RHSType.isArrayElemQualified()) {
      // Allow `_Borrow _ArrayElem` to downgrade to plain `_Borrow`
      if (!(LHSBorrow && RHSBorrow &&
            !LHSType.isArrayElemQualified() &&
            RHSType.isArrayElemQualified()))
        return false;
    }
    // Conversion between different raw pointers is allowed
    if (LHSRaw && RHSRaw) {
      return true;
    }
    // Allow conversion from/to void pointers
    // for conversion of owned->owned, we need to check
    // the inner pointer type recursively
    return LHSCanType.getTypePtr()->isVoidPointerType() ||
           RHSCanType.getTypePtr()->isVoidPointerType() ||
           CheckOwnedQualTypeCStyleCast(LHSPtrType->getPointeeType(),
                                        RHSPtrType->getPointeeType());
  }
  // owned pointer can be cast to integral type, but not the opposite
  if (LHSCanType->isIntegerType() && RHSCanType.isOwnedQualified() &&
      RHSCanType->isPointerType()) {
    return true;
  }
  return IsSameType;
}

bool Sema::CheckOwnedQualTypeCStyleCast(QualType LHSType, QualType RHSType, SourceLocation RLoc) {
  if (!CheckOwnedQualTypeCStyleCast(LHSType, RHSType)) {
    QualType RHSCanType = RHSType.getCanonicalType();
    QualType LHSCanType = LHSType.getCanonicalType();
    if (IsOwnedRawPointerCastDisallowed(LHSCanType, RHSCanType)) {
      if (LHSType.isArrayElemQualified() || RHSType.isArrayElemQualified())
        Diag(RLoc, diag::err_owned_array_raw_cast_disallowed);
      else
        Diag(RLoc, diag::err_owned_raw_cast_disallowed);
    }
    else
      Diag(RLoc, diag::err_owned_qualcheck_incompatible) << RHSType << LHSType;
    return false;
  } else {
    return true;
  }
}

bool Sema::CheckOwnedQualTypeAssignment(QualType LHSType, QualType RHSType, SourceLocation RLoc) {
  QualType RHSCanType = RHSType.getCanonicalType();
  QualType LHSCanType = LHSType.getCanonicalType();
  const auto *LHSPtrType = LHSType->getAs<PointerType>();
  const auto *RHSPtrType = RHSType->getAs<PointerType>();
  bool IsPointer = LHSPtrType && RHSPtrType;
  bool IsSameType = (LHSCanType.getTypePtr() == RHSCanType.getTypePtr());
  bool IsTraitImplType = (LHSCanType->isTraitType() || RHSCanType->isTraitType());

  // _Bool <- T *_Owned // legal, doesn't consume ownership
  if (RHSPtrType && RHSCanType.isOwnedQualified() &&
      LHSCanType->isBooleanType()) {
    return true;
  }

  // owned to owned cases:
  // int* owned  <->  int* owned   // legal
  // int* owned  <->  float* owned // illegal
  // const int** owned  <->  int** owned  // legal
  // trait T* owned <- [type : trait T] * owned // legal
  // unOwned to unOwned cases:
  // owned int* owned *  <->  owned int**  // illegal
  // owned int* const *  <->  owned int**  // legal
  if (LHSCanType.isOwnedQualified() == RHSCanType.isOwnedQualified() ||
      (LHSCanType->isTraitType() && RHSCanType->isOwnedStructureType())) {
    if (LHSType.isArrayElemQualified() != RHSType.isArrayElemQualified()) {
      // Allow `_Borrow _ArrayElem` to downgrade to plain `_Borrow`
      if (!(LHSCanType.isBorrowQualified() && RHSCanType.isBorrowQualified() &&
            !LHSType.isArrayElemQualified() &&
            RHSType.isArrayElemQualified()))
        return false;
    }
    if (IsSameType) {
      return true;
    }
    if (IsTraitImplType) {
      return true;
    }
    if (!IsPointer) {
      return false;
    } else {
      // owned struct S* <-> void* //legal
      if (!LHSCanType.isOwnedQualified() && (LHSPtrType->isVoidPointerType() || RHSPtrType->isVoidPointerType()))
        return true;
      return CheckOwnedQualTypeAssignment(LHSPtrType->getPointeeType(), RHSPtrType->getPointeeType(), RLoc);
    }
  }

  // trait T* owned <-> trait T* owned // legal
  if (LHSCanType.isOwnedQualified() || RHSCanType.isOwnedQualified()) {
    TraitDecl *TD = TryDesugarTrait(LHSType);
    if (TD) {
      QualType QT = DesugarTraitToStructTrait(TD, LHSCanType, RLoc);
      if (QT.getCanonicalType() == RHSCanType) {
        return true;
      }
    }
  }

  // unOwned <-> owned
  // int* owned <-> int*  //illegal
  return false;
}

bool Sema::CheckOwnedQualTypeAssignment(QualType LHSType, Expr* RHSExpr) {
  QualType RHSCanType = RHSExpr->getType().getCanonicalType();
  QualType LHSCanType = LHSType.getCanonicalType();
  bool IsLiteral = false;
  Stmt::StmtClass RHSClass = RHSExpr->getStmtClass();
  if (RHSClass == Expr::IntegerLiteralClass
      || RHSClass == Expr::FloatingLiteralClass
      || RHSClass == Expr::CharacterLiteralClass) {
    IsLiteral = true;
  }
  SourceLocation ExprLoc = RHSExpr->getBeginLoc();
  // Owned pointer can be inited by nullptr.
  if (LHSCanType.isOwnedQualified() && LHSCanType->isPointerType() &&
      isa<CXXNullPtrLiteralExpr>(RHSExpr->IgnoreParens()))
    return true;

  bool Res = true;

  // unOwned to owned initialize cases:
  // int owned a = 10;        //legal even 10 is not owned type
  // int owned b = 10 + 10;   //ilegal
  // char owned c = 'c';      // legal even 'c' is int type
  // int owned d = (int)a;    // illegal
  if (LHSCanType.isOwnedQualified() && !RHSCanType.isOwnedQualified() && IsLiteral) {
    if (LHSCanType.getTypePtr() != RHSCanType.getTypePtr()
        && !(LHSCanType.getTypePtr()->isCharType() && RHSCanType.getTypePtr()->isIntegerType())) {
      Res = false;
    }
  } else {
    Res = CheckOwnedQualTypeAssignment(LHSType, RHSCanType, ExprLoc);
  }
  return Res;
}

bool Sema::CheckOwnedFunctionPointerType(QualType LHSType, Expr* RHSExpr) {
  const FunctionProtoType* LHSFuncType = LHSType->getAs<PointerType>()->getPointeeType()->getAs<FunctionProtoType>();
  const FunctionProtoType* RHSFuncType = RHSExpr->getType()->isFunctionPointerType()?
    RHSExpr->getType()->getAs<PointerType>()->getPointeeType()->getAs<FunctionProtoType>():
    RHSExpr->getType()->getAs<FunctionProtoType>();

  // K&R-style functions use FunctionNoProtoType. BSC function qualifier checks
  // only apply when both sides are prototype function types.
  if (!LHSFuncType || !RHSFuncType) {
    return true;
  }

  // For heterogeneous redeclarations, select the best matching declaration.
  if (FunctionDecl *SelectedFD =
          SelectFunctionDeclForPointerAssignment(RHSExpr, LHSFuncType))
    RHSFuncType = SelectedFD->getType()->getAs<FunctionProtoType>();

  if (!RHSFuncType) {
    return true;
  }

  // return if no 'owned' in both side
  if (!LHSFuncType->hasOwnedRetOrParams() && !RHSFuncType->hasOwnedRetOrParams()) {
    return true;
  }

  // Mismatched pointee base types are reported by the general function pointer
  // checks, which give a better diagnostic; only compare cv here.
  auto OwnedPointeeCVMatch = [](QualType Dest, QualType Src) -> bool {
    if (!Dest.isOwnedQualified() || !Src.isOwnedQualified() ||
        !Dest->isPointerType() || !Src->isPointerType())
      return true;
    return Dest->getPointeeType().getCanonicalType().getLocalCVRQualifiers() ==
           Src->getPointeeType().getCanonicalType().getLocalCVRQualifiers();
  };

  if ((LHSFuncType->getReturnType().isOwnedQualified() && !RHSFuncType->getReturnType().isOwnedQualified())
       || (!LHSFuncType->getReturnType().isOwnedQualified() && RHSFuncType->getReturnType().isOwnedQualified())) {
    return false;
  }
  if (!OwnedPointeeCVMatch(LHSFuncType->getReturnType(),
                           RHSFuncType->getReturnType())) {
    return false;
  }
  if (LHSFuncType->getNumParams() != RHSFuncType->getNumParams()) {
    return false;
  }
  for (unsigned i = 0; i < LHSFuncType->getNumParams(); i++) {
    if ((LHSFuncType->getParamType(i).isOwnedQualified() && !RHSFuncType->getParamType(i).isOwnedQualified())
         || (!LHSFuncType->getParamType(i).isOwnedQualified() && RHSFuncType->getParamType(i).isOwnedQualified())) {
      return false;
    }
    if (!OwnedPointeeCVMatch(LHSFuncType->getParamType(i),
                             RHSFuncType->getParamType(i))) {
      return false;
    }
  }
  return true;
}

Sema::AssignConvertType
Sema::CheckBSCQualTypeAssignment(QualType LHSType, ExprResult &RHS) {
  QualType LHSCan = LHSType.getCanonicalType();
  QualType RHSCan = RHS.get()->getType().getCanonicalType();

  bool MayHaveOwned = LHSCan.isOwnedQualified() || RHSCan.isOwnedQualified();
  bool MayHaveBorrow = LHSCan.isBorrowQualified() || RHSCan.isBorrowQualified();
  if (const auto *LHSPtr = LHSType->getAs<PointerType>()) {
    if (const auto *RHSPtr = RHS.get()->getType()->getAs<PointerType>()) {
      MayHaveOwned |= LHSPtr->hasOwnedFields() || RHSPtr->hasOwnedFields();
      MayHaveBorrow |= LHSPtr->hasBorrowFields() || RHSPtr->hasBorrowFields();
    }
  }

  // Check the destination's qualifier first for accurate diagnostics.
  if (LHSCan.isBorrowQualified()) {
    if (MayHaveBorrow && !CheckBorrowQualTypeAssignment(LHSType, RHS))
      return IncompatibleBorrowPointer;
    if (MayHaveOwned && !CheckOwnedQualTypeAssignment(LHSType, RHS.get()))
      return IncompatibleOwnedPointer;
  } else {
    if (MayHaveOwned && !CheckOwnedQualTypeAssignment(LHSType, RHS.get()))
      return IncompatibleOwnedPointer;
    if (MayHaveBorrow && !CheckBorrowQualTypeAssignment(LHSType, RHS))
      return IncompatibleBorrowPointer;
  }
  return Compatible;
}

Sema::AssignConvertType
Sema::CheckBSCFunctionPointerType(QualType LHSType, Expr *RHSExpr) {
  const FunctionProtoType *LHSFuncType =
      LHSType->getAs<PointerType>()->getPointeeType()->getAs<FunctionProtoType>();
  const FunctionProtoType *RHSFuncType =
      RHSExpr->getType()->isFunctionPointerType()
          ? RHSExpr->getType()
                ->getAs<PointerType>()
                ->getPointeeType()
                ->getAs<FunctionProtoType>()
          : RHSExpr->getType()->getAs<FunctionProtoType>();

  // K&R-style functions use FunctionNoProtoType. BSC-specific function
  // qualifier checks only apply when both sides are prototype function types.
  if (!LHSFuncType || !RHSFuncType)
    return Compatible;

  if (!CheckOwnedFunctionPointerType(LHSType, RHSExpr))
    return IncompatibleOwnedPointer;
  if (!CheckBorrowFunctionPointerType(LHSType, RHSExpr))
    return IncompatibleBorrowPointer;
  // Nullability compatibility.
  if (!AreFunctionTypesNullabilityCompatible(LHSFuncType, RHSFuncType, Context))
    return IncompatibleFunctionPointer;
  return Compatible;
}

bool Sema::CheckTemporaryVarMemoryLeak(Expr* E) {
  if (E == nullptr)
    return false;
  if (isUnevaluatedContext())
    return false;
  E = E->IgnoreParenCastsSafe();
  if (auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_LNot)
      return CheckTemporaryVarMemoryLeak(UO->getSubExpr());
  }
  if (auto *BO = dyn_cast<BinaryOperator>(E)) {
    // ActOnBinOp checks the comma LHS, which is always discarded. If the
    // comma expression itself is discarded, only its result-producing RHS
    // needs an additional check here.
    if (BO->getOpcode() == BO_Comma)
      return CheckTemporaryVarMemoryLeak(BO->getRHS());
  }
  if (auto *CO = dyn_cast<AbstractConditionalOperator>(E)) {
    // BinaryConditionalOperator (GNU `x ?: y`) reuses the common expression,
    // exposed via getCommon() rather than the OpaqueValueExpr getTrueExpr().
    Expr *TrueExpr = isa<BinaryConditionalOperator>(CO)
                         ? cast<BinaryConditionalOperator>(CO)->getCommon()
                         : CO->getTrueExpr();
    bool LeakCond = CheckTemporaryVarMemoryLeak(CO->getCond());
    bool LeakTrue = CheckTemporaryVarMemoryLeak(TrueExpr);
    bool LeakFalse = CheckTemporaryVarMemoryLeak(CO->getFalseExpr());
    return LeakCond || LeakTrue || LeakFalse;
  }
  if (!isa<CallExpr>(E) && !isa<CompoundLiteralExpr>(E))
    return false;
  QualType RetType = E->getType().getCanonicalType();
  if (RetType.isOwnedQualified() || RetType->isMoveSemanticType()) {
    std::string ExprString;
    llvm::raw_string_ostream ExprStream(ExprString);
    E->printPretty(ExprStream, nullptr, clang::PrintingPolicy(getLangOpts()));
    Diag(E->getBeginLoc(), diag::err_owned_temporary_memLeak) << ExprStream.str();
    return true;
  }
  return false;
}

// True when E is an object/array expression rooted at a `_Borrow` (or
// `_Borrow _ArrayElem`) pointer, e.g. `a->p`, `arr[i].a`, `(*a).p`,
// `(*(a + 1)).p` and `((c ? a : b)[0]).p`. Only the three expression kinds
// that produce an object/array from a pointer are peeled (MemberExpr,
// ArraySubscriptExpr and UnaryOperator(Deref)); composite pointer operands
// are decided by their type alone. A non-borrow (raw) pointer base/operand
// stops the walk: an object reached through `**a` or `(*a)->p` (where `a` is
// `struct S ** _Borrow _ArrayElem`) is behind an untracked pointer and is not
// an element of the borrowed array. When RootDecl is provided and the
// expression has a single DeclRefExpr root, it receives that declaration (for
// diagnostics); composite roots leave it null.
static bool IsBorrowRoot(const Expr *E, const VarDecl **RootDecl = nullptr) {
  if (!E)
    return false;
  if (RootDecl)
    *RootDecl = nullptr;
  E = E->IgnoreParenImpCastsSafe();
  auto IsBorrowPtr = [](QualType T) {
    return T->isPointerType() && T.isBorrowQualified();
  };
  // Try to fill RootDecl from a borrow-qualified pointer expression when it is
  // a plain variable; composite pointer expressions have no single root.
  auto NoteRootIfSimple = [&](const Expr *PtrExpr) {
    if (RootDecl)
      IsBorrowRoot(PtrExpr, RootDecl);
  };

  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    const Expr *Base = ME->getBase()->IgnoreParenImpCastsSafe();
    QualType BaseTy = Base->getType();
    if (BaseTy->isPointerType()) {
      // `->`: the pointer base itself decides. Raw pointer bases (including
      // `(&a[0])->p` and `(*a)->p`) are intentionally not tracked.
      if (!IsBorrowPtr(BaseTy))
        return false;
      NoteRootIfSimple(Base);
      return true;
    }
    // `.`: the object is a struct lvalue; its provenance comes from the base
    // expression (`a[0].f`, `(*a).f`). Composite expressions that produce the
    // object are followed on their result arms: `(c ? *a : *b).p` may take
    // ownership from either borrowed arm, and `(0, *a).p` takes it from the
    // comma's RHS. Such composite bases have no single root, so RootDecl is
    // intentionally left null.
    if (const auto *BO = dyn_cast<BinaryOperator>(Base)) {
      if (BO->getOpcode() == BO_Comma)
        return IsBorrowRoot(BO->getRHS());
    } else if (const auto *ACO = dyn_cast<AbstractConditionalOperator>(Base)) {
      if (IsBorrowRoot(ACO->getTrueExpr()) ||
          IsBorrowRoot(ACO->getFalseExpr()))
        return true;
      return false;
    }
    return IsBorrowRoot(Base, RootDecl);
  }

  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    const Expr *Base = ASE->getBase()->IgnoreParenImpCastsSafe();
    QualType BaseTy = Base->getType();
    if (BaseTy->isPointerType()) {
      // `p[i]`: the pointer base decides (composite pointer expressions are
      // covered by the type check).
      if (!IsBorrowPtr(BaseTy))
        return false;
      NoteRootIfSimple(Base);
      return true;
    }
    // The base is an array lvalue (e.g. a member array of a struct element);
    // recurse into the expression that produced it.
    return IsBorrowRoot(Base, RootDecl);
  }

  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_Deref) {
      const Expr *Operand = UO->getSubExpr()->IgnoreParenImpCastsSafe();
      if (!IsBorrowPtr(Operand->getType()))
        return false;
      NoteRootIfSimple(Operand);
      return true;
    }
    return false;
  }

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    QualType Ty = DRE->getType();
    if (IsBorrowPtr(Ty)) {
      if (RootDecl)
        *RootDecl = dyn_cast<VarDecl>(DRE->getDecl());
      return true;
    }
  }
  return false;
}

void Sema::CheckMoveFromBorrow(Expr* E, SourceLocation SL) {
  if (E == nullptr)
    return;
  if (isUnevaluatedContext())
    return;
  E = E->IgnoreParenCastsSafe();
  // Recurse through result-producing paths of composite expressions so that
  // `T *_Owned tmp = (*p1, *p2)`, `q ?: null`, and `cond ? *p1 : *p2` don't
  // silently skip the check. Each arm is reported at its own location so that
  // `c ? a[0] : a[1]` yields one diagnostic per offending branch instead of
  // two identical diagnostics at the enclosing expression's location.
  if (auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_Comma) {
      // Only the RHS is the runtime-taken ownership move (LHS is discarded
      // without consuming ownership, matching `(void)*bp`-style discards).
      CheckMoveFromBorrow(BO->getRHS(), BO->getRHS()->getExprLoc());
      return;
    }
  } else if (auto *BCO = dyn_cast<BinaryConditionalOperator>(E)) {
    CheckMoveFromBorrow(BCO->getCommon(), BCO->getCommon()->getExprLoc());
    CheckMoveFromBorrow(BCO->getFalseExpr(), BCO->getFalseExpr()->getExprLoc());
    return;
  } else if (auto *CO = dyn_cast<ConditionalOperator>(E)) {
    CheckMoveFromBorrow(CO->getTrueExpr(), CO->getTrueExpr()->getExprLoc());
    CheckMoveFromBorrow(CO->getFalseExpr(), CO->getFalseExpr()->getExprLoc());
    return;
  }
  auto IsOwnedConsuming = [](QualType T) {
    return T.isOwnedQualified() || T->isMoveSemanticType();
  };
  // Emit the move-through-borrow error and, when the offending value is rooted
  // at an array formal that was implicitly adjusted to `_Borrow _ArrayElem`,
  // add a note pointing at that parameter.
  auto DiagMoveBorrow = [&](const Expr *MoveExpr) {
    Diag(SL, diag::err_move_borrow);
    const VarDecl *Root = nullptr;
    if (IsBorrowRoot(MoveExpr, &Root)) {
      if (const auto *Parm = dyn_cast_or_null<ParmVarDecl>(Root)) {
        if (const TypeSourceInfo *TSI = Parm->getTypeSourceInfo()) {
          QualType WrittenTy = TSI->getType();
          if (WrittenTy->isArrayType() &&
              Parm->getType().isBorrowQualified() &&
              Parm->getType().isArrayElemQualified())
            Diag(Parm->getLocation(),
                 diag::note_array_param_adjusted_borrow_arrayelem)
                << Parm->getName();
        }
      }
    }
  };
  // Ownership must not leave through any `_Borrow` pointer, whether plain
  // `_Borrow` or `_Borrow _ArrayElem`. The direct borrow-qualified checks
  // cover `*p`, `p->f` and `a[i]`; `IsBorrowRoot` additionally covers
  // deref-then-member forms (`(*a).p`, `(*(a + 1)).p`,
  // `(c ? *a : *b).p`, `a[0].arr[0].p`). Forms that first go through a raw
  // pointer (e.g. `*(&a[1])`, `(&a[0])->p`, `**a`) are intentionally not
  // tracked.
  if (auto *UO = dyn_cast_or_null<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_Deref &&
        IsOwnedConsuming(UO->getType()) &&
        UO->getSubExpr()->getType().isBorrowQualified())
      DiagMoveBorrow(UO);
  } else if (auto *ME = dyn_cast_or_null<MemberExpr>(E)) {
    if (IsOwnedConsuming(ME->getType()) &&
        (ME->getBase()->getType().isBorrowQualified() ||
         IsBorrowRoot(ME)))
      DiagMoveBorrow(ME);
  } else if (auto *ASE =
                 dyn_cast_or_null<ArraySubscriptExpr>(E)) {
    if (IsOwnedConsuming(ASE->getType()) &&
        (ASE->getBase()->getType().isBorrowQualified() ||
         IsBorrowRoot(ASE)))
      DiagMoveBorrow(ASE);
  }
}

namespace {
/// Returns true if casting from RHS to LHS would cast away const qualifiers
/// (i.e. RHS has const that LHS doesn't at some level).
bool isCastingAwayConst(QualType LHS, QualType RHS) {
  if (LHS->isPointerType() && RHS->isPointerType()) {
    const auto *LHSPtr = LHS->getAs<PointerType>();
    const auto *RHSPtr = RHS->getAs<PointerType>();
    if (LHSPtr && RHSPtr)
      return isCastingAwayConst(LHSPtr->getPointeeType(), RHSPtr->getPointeeType());
  }
  // For non-pointer types: casting away const means RHS has const that LHS doesn't
  return RHS.isConstQualified() && !LHS.isConstQualified();
}
} // namespace

bool Sema::CheckBorrowQualTypeCStyleCast(QualType LHSType, QualType RHSType) {
  // Keep Owned/Borrow/ArrayElem, drop CVR, ignore nullability (checked
  // separately by the nullability checker).
  LHSType = getOnlyBSCQualifiedTypeWithoutNullability(LHSType, Context);
  RHSType = getOnlyBSCQualifiedTypeWithoutNullability(RHSType, Context);

  QualType RHSCanType = RHSType.getCanonicalType();
  QualType LHSCanType = LHSType.getCanonicalType();

  // Allow borrow pointer to be cast from nullptr_t
  if (LHSCanType.isBorrowQualified() && LHSCanType->isPointerType() &&
      RHSCanType->isNullPtrType()) {
    return true;
  }

  // Defer borrow compatibility checks for dependent types to instantiation,
  // where concrete types are available for a precise check.
  if (RHSCanType->isDependentType() || LHSCanType->isDependentType()) {
    return true;
  }

  if (Context.hasSameType(LHSType, RHSType)) {
    return true;
  }
  const auto *LHSPtrType = LHSType->getAs<PointerType>();
  const auto *RHSPtrType = RHSType->getAs<PointerType>();
  bool IsPointer = LHSPtrType && RHSPtrType;
  bool IsUnqualifiedTypeMatch = Context.hasSameUnqualifiedType(LHSCanType, RHSCanType);

  if (LHSCanType->isIntegerType() && RHSCanType.isBorrowQualified() &&
      RHSCanType->isPointerType()) {
    return true;
  }
  if (!IsPointer)
    return IsUnqualifiedTypeMatch;
  // Check borrow qualifier compatibility first - prevent casting between
  // mutable and const borrows to avoid aliasing (must run before hasSameType
  // which may treat types as equivalent)
  if (RHSCanType.isBorrowQualified() && LHSCanType.isBorrowQualified() &&
      (RHSCanType.isConstBorrow() != LHSCanType.isConstBorrow()))
    return false;
  if (LHSCanType->isVoidPointerType())
    return true;
  if (RHSCanType->isVoidPointerType() && !IsInEvaluatedSafeZone())
    return true;
  if (RHSCanType.isBorrowQualified() && LHSCanType.isBorrowQualified() &&
      (RHSType.isArrayElemQualified() != LHSType.isArrayElemQualified())) {
    if (!LHSType.isArrayElemQualified() && RHSType.isArrayElemQualified()) {
      QualType LHSPointee = LHSCanType->getPointeeType();
      QualType RHSPointee = RHSCanType->getPointeeType();
      if (Context.hasSameType(LHSPointee, RHSPointee))
        return true;
    }
    return false;
  }
  if (IsUnqualifiedTypeMatch)
    return true;
  if (TryDesugarTrait(RHSType))
    return true;

  // Reject casting away const (e.g. const int *const * -> int *const *)
  QualType LHSPointee = LHSPtrType->getPointeeType();
  QualType RHSPointee = RHSPtrType->getPointeeType();
  if (isCastingAwayConst(LHSPointee, RHSPointee))
    return false;
  return CheckBorrowQualTypeCStyleCast(LHSPointee, RHSPointee);
}

bool Sema::CheckBorrowQualTypeCStyleCast(QualType LHSType, QualType RHSType, SourceLocation RLoc) {
  if (!CheckBorrowQualTypeCStyleCast(LHSType, RHSType)) {
    Diag(RLoc, diag::err_borrow_qualcheck_incompatible) << CompleteTraitType(RHSType) << CompleteTraitType(LHSType);
    return false;
  } else {
    return true;
  }
}

bool Sema::CheckBorrowQualTypeAssignment(QualType LHSType, QualType RHSType, SourceLocation RLoc) {
  // Keep Owned/Borrow/ArrayElem, drop CVR, ignore nullability (checked
  // separately by the nullability checker).
  LHSType = getOnlyBSCQualifiedTypeWithoutNullability(LHSType, Context);
  RHSType = getOnlyBSCQualifiedTypeWithoutNullability(RHSType, Context);

  QualType RHSCanType = RHSType.getCanonicalType();
  QualType LHSCanType = LHSType.getCanonicalType();
  const auto *LHSPtrType = LHSType->getAs<PointerType>();
  const auto *RHSPtrType = RHSType->getAs<PointerType>();
  bool IsPointer = LHSPtrType && RHSPtrType;

  if (LHSCanType.isBorrowQualified() == RHSCanType.isBorrowQualified()) {
    if (LHSType.isArrayElemQualified() != RHSType.isArrayElemQualified()) {
      if (!(LHSCanType.isBorrowQualified() && RHSCanType.isBorrowQualified() &&
            !LHSType.isArrayElemQualified() &&
            RHSType.isArrayElemQualified())) {
        return false;
      }
    }
    if (TraitDecl *TD = TryDesugarTrait(LHSCanType)) {
      if (TD->getTypeImpledVarDecl(RHSCanType->getPointeeType()))
        return true;
    }
    if (Context.hasSameType(LHSCanType, RHSCanType)) {
      return true;
    }
    if (LHSCanType->isVoidPointerType()) {
      if (LHSCanType->getPointeeType().isConstQualified() == RHSCanType->getPointeeType().isConstQualified())
        return true;
    }
    if (!IsPointer) {
      return false;
    } else {
      return CheckBorrowQualTypeAssignment(LHSPtrType->getPointeeType(), RHSPtrType->getPointeeType(), RLoc);
    }
  }

  return false;
}

bool Sema::CheckBorrowQualTypeAssignment(QualType LHSType, ExprResult &RHS) {
  Expr *RHSExpr = RHS.get();
  QualType RHSCanType = getOnlyBSCQualifiedTypeWithoutNullability(
      RHSExpr->getType().getCanonicalType(), Context);
  QualType LHSCanType =
      getOnlyBSCQualifiedTypeWithoutNullability(LHSType.getCanonicalType(), Context);

  SourceLocation ExprLoc = RHSExpr->getBeginLoc();
  bool Res = true;

  // Defer borrow compatibility checks for dependent types to instantiation,
  // where concrete types are available for a precise check.
  if (RHSCanType->isDependentType() || LHSCanType->isDependentType())
    return true;

  if (LHSCanType.isBorrowQualified() || RHSCanType.isBorrowQualified()) {
    if (TraitDecl *TD = TryDesugarTrait(LHSCanType)) {
      if (RHSCanType->isPointerType()) {
        QualType ImplType = RHSCanType->getPointeeType().getUnqualifiedType().getCanonicalType();
        ImplType.removeLocalOwned();
        if (TD->getTypeImpledVarDecl(ImplType))
          return true;
      }
      // trait T* borrow <-> trait T* borrow // legal
      if (TD) {
        QualType QT = DesugarTraitToStructTrait(TD, LHSCanType, RHSExpr->getExprLoc());
        if (QT.getCanonicalType() == RHSCanType) {
          return true;
        }
      }
    }

    // Borrow pointer can be inited by nullptr.
    if (LHSCanType.isBorrowQualified() && LHSCanType->isPointerType() &&
        isa<CXXNullPtrLiteralExpr>(RHSExpr->IgnoreParens()))
      return true;

    if (LHSCanType->isVoidPointerType()) {
      if (RHSCanType->isArrayType()) {
        if (LHSCanType.isBorrowQualified()) {
          QualType RHSElementType =
              RHSCanType->getAsArrayTypeUnsafe()->getElementType();
          if (!(RHSElementType.isConstQualified() &&
              !LHSCanType->getPointeeType().isConstQualified())) {
            // this conversion does not drop const on the element type
            // unsafe zone: allow conversion unconditionally;
            // safe zone: require trivial element type
            if (!IsInEvaluatedSafeZone() || RHSElementType->isTrivialDataType())
              return true;
          }
          Res = false;
        } else {
          // Check const compatibility between void* and array element type.
          QualType RHSElementType = RHSCanType->getAsArrayTypeUnsafe()->getElementType();
          if (LHSCanType->getPointeeType().isConstQualified() == RHSElementType.isConstQualified())
            return true;
        }
      } else if (RHSCanType->isPointerType()) {
        // Pointer-to-pointer: check const and borrow qualifiers.
        if (LHSCanType->getPointeeType().isConstQualified() == RHSCanType->getPointeeType().isConstQualified() &&
            LHSCanType.isBorrowQualified() == RHSCanType.isBorrowQualified())
          return true;
      }
    }

    // Allow mutable borrow downgrading to immutable borrow (re-borrow)
    // Allow `_Borrow _ArrayElem` to downgrade to plain `_Borrow` as well
    if (LHSCanType->isPointerType() && LHSCanType.isBorrowQualified() &&
        RHSCanType->isPointerType() && RHSCanType.isBorrowQualified()) {
      if (!LHSType.isArrayElemQualified() &&
          RHSExpr->getType().isArrayElemQualified()) {
        QualType LHSPointee = LHSCanType->getPointeeType();
        QualType RHSPointee = RHSCanType->getPointeeType();
        if (Context.hasSameType(LHSPointee, RHSPointee)) {
          return true;
        }
      }
      QualType LHSPointee = LHSCanType->getPointeeType();
      QualType RHSPointee = RHSCanType->getPointeeType();
      if (LHSPointee.isConstQualified() && !RHSPointee.isConstQualified()) {
        LHSPointee.removeLocalConst();
        if ((LHSPointee == RHSPointee) || // T*_Borrow -> const T*_Borrow
            (LHSPointee->isVoidType() &&  // T*_Borrow -> const void*_Borrow
                (!IsInEvaluatedSafeZone() || RHSPointee->isTrivialDataType()))) {
          ExprResult ReBorrowExpr =
              CreateBuiltinUnaryOp(ExprLoc, UO_AddrConstDeref, RHSExpr);
          if (!ReBorrowExpr.isInvalid()) {
            RHS = ReBorrowExpr;
            return true;
          }
          Res = false;
        }
      }
    }

    if (!Context.hasSameType(LHSCanType, RHSCanType))
      Res = false;

    // _Bool <- T *_Borrow is allowed
    if (RHSCanType->isPointerType() && RHSCanType.isBorrowQualified() &&
        LHSCanType->isBooleanType()) {
      Res = true;
    }
  } else {
    Res = CheckBorrowQualTypeAssignment(LHSType, RHSCanType, ExprLoc);
  }
  return Res;
}

static bool isMutableBorrowPointer(QualType Type) {
  Type = Type.getCanonicalType();
  return Type->isPointerType() && Type.isBorrowQualified() &&
         !Type.isConstBorrow();
}

ExprResult Sema::MaybeCreateImplicitMutableReborrow(QualType DestType,
                                                    Expr *Source) {
  if (!isMutableBorrowPointer(DestType) ||
      !isMutableBorrowPointer(Source->getType())) {
    return Source;
  }

  // Explicit borrow operators already carry the reborrow represented by this
  // conversion. Look through syntax-only wrappers to avoid nesting another
  // implicit &_Mut * around them.
  Expr *Core = Source->IgnoreParenImpCastsSafe();
  const auto *UO = dyn_cast<UnaryOperator>(Core);
  bool HasExplicitReborrow =
      UO && (UO->getOpcode() == UO_AddrMut ||
             UO->getOpcode() == UO_AddrMutDeref);
  if (HasExplicitReborrow)
    return Source;

  return CreateBuiltinUnaryOp(Source->getExprLoc(), UO_AddrMutDeref, Source);
}

bool Sema::CheckBorrowQualTypeCompare(QualType LHSType, QualType RHSType) {
  QualType RHSCanType = RHSType.getCanonicalType();
  QualType LHSCanType = LHSType.getCanonicalType();
  bool LHSBorrow = LHSCanType.isBorrowQualified();
  bool RHSBorrow = RHSCanType.isBorrowQualified();
  if (LHSBorrow != RHSBorrow)
    return false;
  if (!LHSBorrow)
    return true;

  return LHSCanType->getPointeeType().getUnqualifiedType() ==
         RHSCanType->getPointeeType().getUnqualifiedType();
}

bool Sema::CheckBorrowFunctionType(QualType ReturnTy,
                                   ArrayRef<QualType> ParamTys,
                                   SourceLocation SL) {
  if (ReturnTy->isDependentType()) {
    return true;
  }
  if (ComputeNumRegions(Context, ReturnTy) > 1) {
    Diag(SL, diag::err_typecheck_multi_level_borrow_func);
    return false;
  }
  if (ReturnTy.hasBorrow()) {
    bool HasBorrowParam = false;
    for (QualType PT : ParamTys) {
      if (PT->isDependentType()) {
        return true;
      }
      if (PT.hasBorrow()) {
        HasBorrowParam = true;
        break;
      }
    }
    if (!HasBorrowParam) {
      Diag(SL, diag::err_typecheck_borrow_func);
      return false;
    }
  }
  return true;
}

bool Sema::CheckBorrowFunctionPointerType(QualType LHSType, Expr *RHSExpr) {
  const FunctionProtoType *LHSFuncType = LHSType->getAs<PointerType>()
                                             ->getPointeeType()
                                             ->getAs<FunctionProtoType>();
  const FunctionProtoType *RHSFuncType =
      RHSExpr->getType()->isFunctionPointerType()
          ? RHSExpr->getType()
                ->getAs<PointerType>()
                ->getPointeeType()
                ->getAs<FunctionProtoType>()
          : RHSExpr->getType()->getAs<FunctionProtoType>();

  // K&R-style functions use FunctionNoProtoType. BSC function qualifier checks
  // only apply when both sides are prototype function types.
  if (!LHSFuncType || !RHSFuncType)
    return true;

  // For heterogeneous redeclarations, select the best matching declaration.
  if (FunctionDecl *SelectedFD =
          SelectFunctionDeclForPointerAssignment(RHSExpr, LHSFuncType))
    RHSFuncType = SelectedFD->getType()->getAs<FunctionProtoType>();

  if (!RHSFuncType)
    return true;

  // return if no 'borrow' in both side
  if (!LHSFuncType->hasBorrowRetOrParams() &&
      !RHSFuncType->hasBorrowRetOrParams()) {
    return true;
  }
  
  auto BorrowParamTypesMatch = [&](QualType Dest, QualType Src) -> bool {
    Dest = getOnlyBSCQualifiedTypeWithoutNullability(Dest, Context);
    Src = getOnlyBSCQualifiedTypeWithoutNullability(Src, Context);
    if (!DoPointerTypesSatisfyAssignmentConstraintsStrict(Dest, Src))
      return false;
    // For pointer params, additionally require that the pointee types match
    // exactly including const/volatile (not just unqualified base type).
    if (Dest->isPointerType() && Src->isPointerType()) {
      QualType DestPointee = Dest->getPointeeType().getCanonicalType();
      QualType SrcPointee = Src->getPointeeType().getCanonicalType();
      // Strip BSC qualifiers (_Borrow/_Owned) from the pointee for comparison;
      // keep all standard qualifiers (const, volatile, restrict).
      DestPointee.removeLocalOwned();
      DestPointee.removeLocalBorrow();
      SrcPointee.removeLocalOwned();
      SrcPointee.removeLocalBorrow();
      if (DestPointee != SrcPointee)
        return false;
    }
    return true;
  };

  bool Compatible = true;
  if (LHSFuncType->getNumParams() != RHSFuncType->getNumParams()) {
    Compatible = false;
  } else {
    if (!BorrowParamTypesMatch(LHSFuncType->getReturnType(),
                               RHSFuncType->getReturnType()))
      Compatible = false;
    for (unsigned I = 0, N = LHSFuncType->getNumParams(); Compatible && I < N; ++I) {
      if (!BorrowParamTypesMatch(LHSFuncType->getParamType(I),
                                 RHSFuncType->getParamType(I)))
        Compatible = false;
    }
  }

  return Compatible;
}

bool Sema::CheckEnsureInitFunctionPointerType(QualType LHSType, Expr *RHSExpr) {
  const FunctionProtoType *LHSFuncType = LHSType->getAs<PointerType>()
                                             ->getPointeeType()
                                             ->getAs<FunctionProtoType>();
  const FunctionProtoType *RHSFuncType =
      RHSExpr->getType()->isFunctionPointerType()
          ? RHSExpr->getType()
                ->getAs<PointerType>()
                ->getPointeeType()
                ->getAs<FunctionProtoType>()
          : RHSExpr->getType()->getAs<FunctionProtoType>();

  if (!LHSFuncType || !RHSFuncType)
    return true;

  // For heterogeneous redeclarations, select the best matching declaration.
  if (FunctionDecl *SelectedFD =
          SelectFunctionDeclForPointerAssignment(RHSExpr, LHSFuncType))
    RHSFuncType = SelectedFD->getType()->getAs<FunctionProtoType>();

  if (LHSFuncType->getNumParams() != RHSFuncType->getNumParams())
    return true; // Param count mismatch handled elsewhere

  // Asymmetric compat: target having the attribute is a contract callers
  // rely on, so source must carry it too. Reverse direction (source has
  // more, target has less) silently weakens and is allowed.
  for (unsigned I = 0; I < LHSFuncType->getNumParams(); ++I) {
    FunctionProtoType::ExtParameterInfo LHSExt =
        LHSFuncType->hasExtParameterInfos()
            ? LHSFuncType->getExtParameterInfo(I)
            : FunctionProtoType::ExtParameterInfo();
    FunctionProtoType::ExtParameterInfo RHSExt =
        RHSFuncType->hasExtParameterInfos()
            ? RHSFuncType->getExtParameterInfo(I)
            : FunctionProtoType::ExtParameterInfo();

    if (LHSExt.isEnsureInit() && !RHSExt.isEnsureInit()) {
      Diag(RHSExpr->getBeginLoc(),
           diag::err_ensure_init_funcptr_incompatible)
          << I;
      return false;
    }

    if (LHSExt.isEnsureInitIfRet()) {
      if (!RHSExt.isEnsureInitIfRet()) {
        Diag(RHSExpr->getBeginLoc(),
             diag::err_ensure_init_if_ret_funcptr_missing)
            << I;
        return false;
      }
      if (LHSExt.getEnsureInitIfRetCondValue() !=
          RHSExt.getEnsureInitIfRetCondValue()) {
        Diag(RHSExpr->getBeginLoc(),
             diag::err_ensure_init_if_ret_funcptr_cond_mismatch)
            << LHSExt.getEnsureInitIfRetCondValue() << I
            << RHSExt.getEnsureInitIfRetCondValue();
        return false;
      }
    }
  }
  return true;
}

// for global borrow variable type check
void Sema::CheckBorrowOrIndirectBorrowType(SourceLocation ErrLoc, QualType T,
                                           StringRef Env) {
  enum { BorrowQualified, BorrowTypedef, BorrowFields };
  if (T.getCanonicalType().isBorrowQualified() &&
      !T.getTypePtr()->getAs<TypedefType>()) {
    Diag(ErrLoc, diag::err_nested_owned_borrow_type_check)
        << BorrowQualified << "_Borrow" << Env;
  } else if (T.getCanonicalType().isBorrowQualified() &&
             T.getTypePtr()->getAs<TypedefType>()) {
    Diag(ErrLoc, diag::err_nested_owned_borrow_type_check)
        << BorrowTypedef << "_Borrow" << Env << T;
  } else if (T.getCanonicalType().getTypePtr()->hasBorrowFields()) {
    Diag(ErrLoc, diag::err_nested_owned_borrow_type_check)
        << BorrowFields << "_Borrow" << Env << T;
  }
}
#endif
