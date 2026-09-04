//===--- SemaDeclBSC.cpp - Semantic Analysis for Declarations
//------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements semantic analysis for statements.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "TreeTransform.h"
#include "clang/AST/Attr.h"
#include "clang/AST/BSC/WalkerBSC.h"
#include "clang/Analysis/Analyses/BSC/BSCBorrowChecker.h"
#include "clang/Analysis/Analyses/BSC/BSCIR.h"
#include "clang/Analysis/Analyses/BSC/BSCIRBuilder.h"
#include "clang/Analysis/Analyses/BSC/BSCIRDump.h"
#include "clang/Analysis/Analyses/BSC/BSCIRInitAnalysis.h"
#include "clang/Analysis/Analyses/BSC/BSCNullabilityCheck.h"
#include "clang/Analysis/Analyses/BSC/BSCOwnership.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/SaveAndRestore.h"
#include <set>
#include <tuple>

using namespace clang;
using namespace sema;

namespace {

/// Returns true if \p Ty contains a record type that, when traversed by value
/// through fields/array elements, reaches itself again. Such cyclic record
/// types can only be created by error recovery (C does not allow a struct to
/// contain itself by value), and they make AST-walking analyses unsafe.
bool TypeHasCyclicRecord(QualType Ty,
                         llvm::DenseSet<const RecordDecl *> &Active) {
  if (Ty.isNull())
    return false;
  // getCanonicalType() strips all sugar (typedefs, elaborated types, etc.), so
  // the RecordDecl pointer used for cycle detection is unique per type.
  Ty = Ty.getCanonicalType();

  if (const RecordDecl *RD = Ty->getAsRecordDecl()) {
    if (!Active.insert(RD).second)
      return true;
    for (const FieldDecl *FD : RD->fields())
      if (TypeHasCyclicRecord(FD->getType(), Active))
        return true;
    Active.erase(RD);
    return false;
  }

  if (Ty->isArrayType()) {
    if (const ArrayType *AT = Ty->getAsArrayTypeUnsafe())
      return TypeHasCyclicRecord(AT->getElementType(), Active);
  }

  // _Atomic(T) stores T by value, so a cyclic record inside it is still a
  // by-value cycle and must be followed (C11 6.7.2.4p4).
  if (const AtomicType *AT = Ty->getAs<AtomicType>())
    return TypeHasCyclicRecord(AT->getValueType(), Active);

  // Pointers, references, scalars, and functions stop by-value recursion.
  return false;
}

bool TypeHasCyclicRecord(QualType Ty) {
  llvm::DenseSet<const RecordDecl *> Active;
  return TypeHasCyclicRecord(Ty, Active);
}

/// Set \p Loc to \p NewLoc when \p Loc is non-null, currently invalid, and
/// \p NewLoc is valid.
void setInvalidLocIfUnset(SourceLocation *Loc, SourceLocation NewLoc) {
  if (Loc && !Loc->isValid() && NewLoc.isValid())
    *Loc = NewLoc;
}

/// Recursively check whether a statement tree contains AST nodes that were
/// produced from errors, or references declarations/types that are invalid.
/// This is used to decide whether BSC dataflow analysis can safely run on the
/// function: error nodes coming from outside the current function can make the
/// AST unreliable even when no diagnostic was emitted inside the function.
/// If \p Loc is non-null, it is set to the first problematic node's location.
bool StmtTreeHasErrors(const Stmt *S, SourceLocation *Loc = nullptr) {
  if (!S)
    return false;

  if (const auto *E = dyn_cast<Expr>(S)) {
    if (E->containsErrors()) {
      setInvalidLocIfUnset(Loc, E->getExprLoc());
      return true;
    }
    if (TypeHasCyclicRecord(E->getType())) {
      setInvalidLocIfUnset(Loc, E->getExprLoc());
      return true;
    }
    if (const Type *T = E->getType().getTypePtrOrNull())
      if (T->containsErrors()) {
        setInvalidLocIfUnset(Loc, E->getExprLoc());
        return true;
      }
    if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
      if (DRE->getDecl() && DRE->getDecl()->isInvalidDecl()) {
        setInvalidLocIfUnset(Loc, DRE->getLocation());
        return true;
      }
    if (const auto *ME = dyn_cast<MemberExpr>(E))
      if (ME->getMemberDecl() && ME->getMemberDecl()->isInvalidDecl()) {
        setInvalidLocIfUnset(Loc, ME->getMemberLoc());
        return true;
      }
    if (const auto *CE = dyn_cast<CallExpr>(E))
      if (const FunctionDecl *Callee = CE->getDirectCallee())
        if (Callee->isInvalidDecl()) {
          setInvalidLocIfUnset(Loc, CE->getBeginLoc());
          return true;
        }
    if (const auto *CSCE = dyn_cast<CStyleCastExpr>(E)) {
      if (const Type *T = CSCE->getTypeAsWritten().getTypePtrOrNull())
        if (T->containsErrors()) {
          setInvalidLocIfUnset(Loc, CSCE->getBeginLoc());
          return true;
        }
    }
  }

  if (const auto *DS = dyn_cast<DeclStmt>(S)) {
    for (const Decl *D : DS->decls()) {
      if (D->isInvalidDecl()) {
        setInvalidLocIfUnset(Loc, D->getLocation());
        return true;
      }
      if (const auto *VD = dyn_cast<VarDecl>(D)) {
        if (TypeHasCyclicRecord(VD->getType())) {
          setInvalidLocIfUnset(Loc, VD->getLocation());
          return true;
        }
        if (const Type *T = VD->getType().getTypePtrOrNull())
          if (T->containsErrors()) {
            setInvalidLocIfUnset(Loc, VD->getLocation());
            return true;
          }
      }
    }
  }

  for (const Stmt *Child : S->children())
    if (StmtTreeHasErrors(Child, Loc))
      return true;

  return false;
}

} // namespace

bool Sema::HasInvalidAST(const FunctionDecl *FD,
                         SourceLocation *InvalidLoc) const {
  if (!FD)
    return false;

  auto SetLoc = [&](SourceLocation L) {
    setInvalidLocIfUnset(InvalidLoc, L);
  };

  if (FD->isInvalidDecl()) {
    SetLoc(FD->getLocation());
    return true;
  }
  if (const Type *T = FD->getType().getTypePtrOrNull())
    if (T->containsErrors()) {
      SetLoc(FD->getLocation());
      return true;
    }
  if (TypeHasCyclicRecord(FD->getReturnType())) {
    SetLoc(FD->getReturnTypeSourceRange().getBegin());
    SetLoc(FD->getLocation());
    return true;
  }
  if (const Type *RT = FD->getReturnType().getTypePtrOrNull())
    if (RT->containsErrors()) {
      SetLoc(FD->getReturnTypeSourceRange().getBegin());
      SetLoc(FD->getLocation());
      return true;
    }
  for (const ParmVarDecl *PVD : FD->parameters()) {
    if (TypeHasCyclicRecord(PVD->getType())) {
      SetLoc(PVD->getLocation());
      return true;
    }
    if (const Type *PT = PVD->getType().getTypePtrOrNull())
      if (PT->containsErrors()) {
        SetLoc(PVD->getLocation());
        return true;
      }
  }
  if (const Stmt *Body = FD->getBody())
    return StmtTreeHasErrors(Body, InvalidLoc);
  return false;
}

void Sema::CheckBSCConstexprFunction(FunctionDecl* FD) {
  assert(getLangOpts().BSC && FD->isConstexprSpecified());
  // BSC constexpr function can not be async.
  if (FD->isAsyncSpecified()) {
    Diag(FD->getBeginLoc(), diag::err_async_func_unsupported)
        << "constexpr";
  }
  // BSC constexpr function can not be variadic.
  if (FD->isVariadic()) {
    Diag(FD->getBeginLoc(), diag::err_constexpr_func_unsupported)
        << "variadic";
  }
  // The return type and parameter type of BSC constexpr function should be compile_time_calculated type.
  QualType RT = FD->getReturnType();
  if (!RT->isDependentType() && !RT->isBSCCalculatedTypeInCompileTime()) {
    Diag(FD->getBeginLoc(), diag::err_constexpr_func_unsupported_type) << RT;
  }
  for (ParmVarDecl* PVD: FD->parameters()) {
    QualType PT = PVD->getType();
    if (!PT->isDependentType() && !PT->isBSCCalculatedTypeInCompileTime()) {
      Diag(PVD->getLocation(), diag::err_constexpr_func_unsupported_type) << PT;
    }
  }
}

// The type of BSC constexpr variable should be compile_time_calculated type.
void Sema::CheckBSCConstexprVarType(VarDecl* VD) {
  assert(getLangOpts().BSC);
  QualType T = VD->getType();
  if (T->isDependentType())
    return;
  if (VD->isConstexpr() && !T->isBSCCalculatedTypeInCompileTime()) {
    Diag(VD->getLocation(), diag::err_constexpr_var_unsupported_type) << T;
    VD->setInvalidDecl();
    return;
  }
  if (FunctionDecl* FD = dyn_cast_or_null<FunctionDecl>(VD->getDeclContext())) {
    if (FD->isConstexpr() && !T->isBSCCalculatedTypeInCompileTime()) {
      Diag(VD->getLocation(), diag::err_constexpr_func_unsupported_type) << VD->getType();
      VD->setInvalidDecl();
      return;
    }
  }
}

bool HasDiffNullabilityQualifiers(QualType LHSType, QualType RHSType,
                                   ASTContext &Ctx) {
  if (LHSType.getDefNullability() != RHSType.getDefNullability())
    return true;
  if (LHSType->isPointerType() && RHSType->isPointerType()) {
    QualType LHSPType = LHSType->getPointeeType();
    QualType RHSPType = RHSType->getPointeeType();
    const auto *LHSFn = LHSPType->getAs<FunctionProtoType>();
    const auto *RHSFn = RHSPType->getAs<FunctionProtoType>();
    if (LHSFn && RHSFn && LHSFn->getNumParams() == RHSFn->getNumParams()) {
      if (HasDiffNullabilityQualifiers(LHSFn->getReturnType(),
                                       RHSFn->getReturnType(), Ctx))
        return true;
      for (unsigned I = 0, E = LHSFn->getNumParams(); I != E; ++I)
        if (HasDiffNullabilityQualifiers(LHSFn->getParamType(I),
                                         RHSFn->getParamType(I), Ctx))
          return true;
      return false;
    }
    return HasDiffNullabilityQualifiers(LHSPType, RHSPType, Ctx);
  }
  return false;
}

bool HasDiffBorrorOrOwnedQualifiers(QualType LHSType, QualType RHSType) {
  if (LHSType.isOwnedQualified() != RHSType.isOwnedQualified()) {
    return true;
  }
  if (LHSType.isBorrowQualified() != RHSType.isBorrowQualified()) {
    return true;
  }
  if (LHSType.isArrayElemQualified() != RHSType.isArrayElemQualified()) {
    return true;
  }
  if (LHSType->isPointerType() && RHSType->isPointerType()) {
    QualType LHSPType = LHSType->getPointeeType();
    QualType RHSPType = RHSType->getPointeeType();
    const auto *LHSFn = LHSPType->getAs<FunctionProtoType>();
    const auto *RHSFn = RHSPType->getAs<FunctionProtoType>();
    if (LHSFn && RHSFn && LHSFn->getNumParams() == RHSFn->getNumParams()) {
      if (HasDiffBorrorOrOwnedQualifiers(LHSFn->getReturnType(),
                                         RHSFn->getReturnType()))
        return true;
      for (unsigned I = 0, E = LHSFn->getNumParams(); I != E; ++I)
        if (HasDiffBorrorOrOwnedQualifiers(LHSFn->getParamType(I),
                                           RHSFn->getParamType(I)))
          return true;
      return false;
    }
    return HasDiffBorrorOrOwnedQualifiers(LHSPType, RHSPType);
  }
  return false;
}

bool Sema::HasDiffBorrowOrOwnedParamsTypeAtBothFunction(QualType LHS,
                                                        QualType RHS) {
  const FunctionProtoType *LSHFuncType = LHS->getAs<FunctionProtoType>();
  const FunctionProtoType *RSHFuncType = RHS->getAs<FunctionProtoType>();
  if (!LSHFuncType || !RSHFuncType) {
    return false;
  }

  QualType LHSRetType = LSHFuncType->getReturnType();
  QualType RHSRetType = RSHFuncType->getReturnType();
  if (HasDiffBorrorOrOwnedQualifiers(LHSRetType, RHSRetType)) {
    return true;
  }
  if (LSHFuncType->getNumParams() != RSHFuncType->getNumParams()) {
    return true;
  }
  for (unsigned i = 0; i < LSHFuncType->getNumParams(); i++) {
    QualType LHSParType =
        LSHFuncType->getParamType(i).getOnlyBSCQualifiedType(Context);
    QualType RHSParType =
        RSHFuncType->getParamType(i).getOnlyBSCQualifiedType(Context);
    if (HasDiffBorrorOrOwnedQualifiers(LHSParType, RHSParType)) {
      return true;
    }
  }
  return false;
}

bool Sema::HasDiffNullabilityParamsTypeAtBothFunction(QualType LHS,
                                                      QualType RHS) {
  const FunctionProtoType *LSHFuncType = LHS->getAs<FunctionProtoType>();
  const FunctionProtoType *RSHFuncType = RHS->getAs<FunctionProtoType>();
  if (!LSHFuncType || !RSHFuncType) {
    return false;
  }

  QualType LHSRetType = LSHFuncType->getReturnType();
  QualType RHSRetType = RSHFuncType->getReturnType();
  if (HasDiffNullabilityQualifiers(LHSRetType, RHSRetType, Context)) {
    return true;
  }
  if (LSHFuncType->getNumParams() != RSHFuncType->getNumParams()) {
    return true;
  }
  for (unsigned i = 0; i < LSHFuncType->getNumParams(); i++) {
    QualType LHSParType = LSHFuncType->getParamType(i);
    QualType RHSParType = RSHFuncType->getParamType(i);
    if (HasDiffNullabilityQualifiers(LHSParType, RHSParType, Context)) {
      return true;
    }
  }
  return false;
}

// Return true if any memory safe features are found in a FunctionDecl.
// Qualifiers: owned, borrow, fat
// AddrOp: &mut, &const
bool Sema::FindSafeFeatures(const FunctionDecl* FnDecl) {
  if (!FnDecl) return false;
  SafeFeatureFinder finder;
  if (finder.FindOwnedOrBorrow(const_cast<FunctionDecl*>(FnDecl))) {
    return true;
  }
  return false;
}

bool Sema::HasSafeZoneInStmt(const Stmt *CompStmt) {
  if (!CompStmt) {
    return false;
  }
  for (const Stmt *child: CompStmt->children()) {
    if (!child) {
      continue;
    }
    if (auto *CompChild = dyn_cast<CompoundStmt>(child)) {
      if (CompChild->getCompSafeZoneSpecifier() == SZ_Safe) {
        return true;
      }
    }
    if (auto *CompChild = dyn_cast<SafeStmt>(child)) {
      if (CompChild->getSafeZoneSpecifier() == SZ_Safe) {
        return true;
      }
    }
    if (auto *CompChild = dyn_cast<SafeExpr>(child)) {
      if (CompChild->getSafeZoneSpecifier() == SZ_Safe) {
        return true;
      }
    }
    if (HasSafeZoneInStmt(child)) {
      return true;
    }
  }
  return false;
}

void Sema::CheckBSCEnsureInitIfRetOnFunctionDecl(FunctionDecl *FD) {
  if (!FD)
    return;
  bool HasAttr = false;
  for (ParmVarDecl *PVD : FD->parameters()) {
    if (PVD->hasAttr<EnsureInitIfRetAttr>()) {
      HasAttr = true;
      break;
    }
  }
  if (!HasAttr)
    return;
  QualType RT = FD->getReturnType();
  // Dependent / compile-time return types are checked at instantiation, not on
  // the template pattern (like the other BSC return-type checks).
  if (RT->isDependentType() || RT->isBSCCalculatedTypeInCompileTime() ||
      RT->isIntegerType())
    return;
  // Emit once across redeclarations: skip if an earlier decl already carried
  // the attribute (and was thus already diagnosed).
  for (FunctionDecl *Prev = FD->getPreviousDecl(); Prev;
       Prev = Prev->getPreviousDecl())
    for (ParmVarDecl *PVD : Prev->parameters())
      if (PVD->hasAttr<EnsureInitIfRetAttr>())
        return;
  SourceLocation Loc = FD->getReturnTypeSourceRange().getBegin();
  if (!Loc.isValid())
    Loc = FD->getLocation();
  Diag(Loc, diag::err_ensure_init_if_ret_bad_return_type) << RT;
}

bool Sema::CheckBSCEnsureInitIfRetRedecl(FunctionDecl *Old, FunctionDecl *New) {
  if (!Old->hasPrototype() || !New->hasPrototype() ||
      Old->getNumParams() != New->getNumParams())
    return false;
  bool OldSafe = Old->getSafeZoneSpecifier() == SZ_Safe;
  bool NewSafe = New->getSafeZoneSpecifier() == SZ_Safe;
  bool SameSafety = (OldSafe == NewSafe);
  bool Failed = false;
  for (unsigned I = 0; I < Old->getNumParams(); ++I) {
    auto *OldA = Old->getParamDecl(I)->getAttr<EnsureInitIfRetAttr>();
    auto *NewA = New->getParamDecl(I)->getAttr<EnsureInitIfRetAttr>();
    if (SameSafety) {
      if (static_cast<bool>(OldA) != static_cast<bool>(NewA) ||
          (OldA && NewA && OldA->getCondValue() != NewA->getCondValue())) {
        Diag(New->getParamDecl(I)->getLocation(),
             diag::err_ensure_init_if_ret_redecl_mismatch)
            << New->getParamDecl(I) << New;
        Diag(Old->getParamDecl(I)->getLocation(),
             diag::note_previous_declaration);
        Failed = true;
      }
    } else {
      // Cross-safety: if the _Unsafe decl carries the attribute the _Safe one
      // must too with the same arg. Report on the new declaration with a note
      // at the previous one (standard redecl convention); %1 is the parameter
      // that actually carries the attribute (the _Unsafe one).
      auto *SafeA = OldSafe ? OldA : NewA;
      auto *UnsafeA = OldSafe ? NewA : OldA;
      ParmVarDecl *UnsafePVD = OldSafe ? New->getParamDecl(I)
                                       : Old->getParamDecl(I);
      if (UnsafeA &&
          (!SafeA || SafeA->getCondValue() != UnsafeA->getCondValue())) {
        Diag(New->getParamDecl(I)->getLocation(),
             diag::err_ensure_init_if_ret_unsafe_without_safe)
            << UnsafeA->getCondValue() << UnsafePVD;
        Diag(Old->getParamDecl(I)->getLocation(),
             diag::note_previous_declaration);
        Failed = true;
      }
    }
  }
  return Failed;
}

bool Sema::HasSafeZoneInFunction(const FunctionDecl* FnDecl) {
  if (!FnDecl || !FnDecl->getBody()) {
    return false;
  }
  if (FnDecl->getSafeZoneSpecifier() == SZ_Safe) {
    return true;
  }
  CompoundStmt *FuncBody = cast<CompoundStmt>(FnDecl->getBody());
  if (!FuncBody) {
    return false;
  }
  return HasSafeZoneInStmt(FuncBody);
}

/// BSC's dataflow analysis process is as follows:
/// ====================================================================
///                      __________________       ___________________
///                     |                  |     |                   |
/// FuncDecl--> CFG --> | NullabilityCheck | --> | OwnershipAnalysis | -->
///                     |__________________|     |___________________|
///       __________________
///      |                  |
///  --> |   BorrowCheck    | -->  FuncDecl  --> CodeGen
///      |__________________|
/// ====================================================================
void Sema::BSCDataflowAnalysis(const Decl *D) {
  AnalysisDeclContext AC(/* AnalysisDeclContextManager */ nullptr, D);

  AC.getCFGBuildOptions().PruneTriviallyFalseEdges = true;
  AC.getCFGBuildOptions().AddLifetime = true;
  AC.getCFGBuildOptions().BSCMode = true;
  AC.getCFGBuildOptions().setAllAlwaysAdd();

  const FunctionDecl *FD = cast<FunctionDecl>(D);

  // If D does not use memory safety features like "owned, borrow, &mut, &const",
  // we should not do borrow checking.
  bool RequireBorrowCheck = FindSafeFeatures(FD);
  // nullability-check happens in mode: {SafeOnly, All}.
  // For SafeOnly, do not build cfg when there is no SafeZone in Function.
  bool RequireNullabilityCheck = true;
  if (getLangOpts().getNullabilityCheck() == LangOptions::NC_SAFE) {
    if(HasSafeZoneInFunction(FD)) {
      RequireNullabilityCheck = true;
    } else {
      RequireNullabilityCheck = false;
    }
  }
  bool RequireCFGAnalysis = RequireNullabilityCheck || RequireBorrowCheck;
  bool DumpBSCIR = getLangOpts().DumpBSCIR;

  // -dump-bscir: dump BSCIR only, no analyses
  if (DumpBSCIR && FD && FD->getBody()) {
    bscir::BSCIRBuilder Builder(Context, *FD);
    auto Body = Builder.build();
    if (Body)
      bscir::dumpBody(*Body, llvm::outs());
    return;
  }

  // Init analysis (BSCIR-based, independent of CFG)
  bool RequireInitCheck = false;
  switch (getLangOpts().getUninitCheck()) {
  case LangOptions::UC_NONE:
    break;
  case LangOptions::UC_SAFE:
    if (FD) {
      bool HasEnsureInitParams = false;
      for (unsigned I = 0; I < FD->getNumParams(); ++I) {
        if (FD->getParamDecl(I)->hasAttr<EnsureInitAttr>() ||
            FD->getParamDecl(I)->hasAttr<EnsureInitIfRetAttr>()) {
          HasEnsureInitParams = true;
          break;
        }
      }
      RequireInitCheck = HasSafeZoneInFunction(FD) ||
                         FD->getSafeZoneSpecifier() == SZ_Safe ||
                         HasEnsureInitParams;
    }
    break;
  case LangOptions::UC_ALL:
    RequireInitCheck = true;
    break;
  }
  if (RequireInitCheck && FD && FD->getBody()) {
    bscir::BSCIRBuilder Builder(Context, *FD);
    auto Body = Builder.build();
    if (Body) {
      SmallVector<bscir::InitDiagInfo, 8> InitDiags;
      bool CheckAllZones =
          getLangOpts().getUninitCheck() == LangOptions::UC_ALL;
      bscir::runInitAnalysis(*Body, InitDiags, CheckAllZones);
      // Key on the variable name too: distinct parameters can fail the same
      // way at the same return site (multi-parameter contracts), and each
      // must be reported.
      std::set<std::tuple<unsigned, unsigned, std::string>> Seen;
      for (const auto &D : InitDiags) {
        unsigned RawLoc = D.Loc.getRawEncoding();
        unsigned KindVal = static_cast<unsigned>(D.Kind);
        if (!Seen.insert({RawLoc, KindVal, D.VarName}).second)
          continue;
        unsigned DiagId;
        switch (D.Kind) {
        case bscir::InitDiagKind::UseOfUninit:
          DiagId = diag::err_ownership_use_uninit;
          break;
        case bscir::InitDiagKind::UseOfMaybeUninit:
          DiagId = diag::err_ownership_use_possibly_uninit;
          break;
        case bscir::InitDiagKind::ReturnUninit:
        case bscir::InitDiagKind::ReturnMaybeUninit:
          DiagId = diag::err_return_uninit;
          break;
        case bscir::InitDiagKind::EnsureInitNotInit:
          DiagId = diag::err_ensure_init_not_init;
          break;
        case bscir::InitDiagKind::EnsureInitMaybeNotInit:
          DiagId = diag::err_ensure_init_maybe_not_init;
          break;
        case bscir::InitDiagKind::EnsureInitReassigned:
          DiagId = diag::err_ensure_init_reassigned;
          break;
        case bscir::InitDiagKind::EnsureInitPtrAliased:
          DiagId = diag::err_ensure_init_ptr_aliased;
          break;
        case bscir::InitDiagKind::EnsureInitDerefReadUninit:
          DiagId = diag::err_ensure_init_deref_read_uninit;
          break;
        case bscir::InitDiagKind::EnsureInitIfRetNotInit:
          DiagId = diag::err_ensure_init_if_ret_not_init;
          break;
        case bscir::InitDiagKind::EnsureInitIfRetMaybeNotInit:
          DiagId = diag::err_ensure_init_if_ret_maybe_not_init;
          break;
        case bscir::InitDiagKind::EnsureInitIfRetReassigned:
          DiagId = diag::err_ensure_init_if_ret_reassigned;
          break;
        case bscir::InitDiagKind::EnsureInitIfRetNonConstReturn:
          DiagId = diag::err_ensure_init_if_ret_non_const_return;
          break;
        }
        if (D.Kind == bscir::InitDiagKind::EnsureInitIfRetNotInit ||
            D.Kind == bscir::InitDiagKind::EnsureInitIfRetMaybeNotInit ||
            D.Kind == bscir::InitDiagKind::EnsureInitIfRetReassigned ||
            D.Kind == bscir::InitDiagKind::EnsureInitIfRetNonConstReturn)
          Diag(D.Loc, DiagId) << D.VarName << D.CondValue;
        else if (D.Kind == bscir::InitDiagKind::EnsureInitPtrAliased ||
                 D.Kind == bscir::InitDiagKind::EnsureInitDerefReadUninit)
          // %0 = param name, %1 = which attribute the param carries.
          Diag(D.Loc, DiagId) << D.VarName << D.AttrSelect;
        else if (D.Kind == bscir::InitDiagKind::ReturnUninit ||
                 D.Kind == bscir::InitDiagKind::ReturnMaybeUninit)
          Diag(D.Loc, DiagId)
              << D.VarName
              << (D.Kind == bscir::InitDiagKind::ReturnMaybeUninit ? 1 : 0);
        else
          Diag(D.Loc, DiagId) << D.VarName;
        for (SourceLocation NoteLoc : D.NoteLocs) {
          if (NoteLoc.isValid())
            Diag(NoteLoc, diag::note_ensure_init_ptr_reassigned_here)
                << D.VarName;
        }
        getDiagnostics().increaseInitCheckErrors();
      }
    }
  }

  // CFG-based analyses (nullability, borrow check)
  if (RequireCFGAnalysis && FD && AC.getCFG()) {
    // Step one: Run NullabilityCheck
    unsigned NumNullabilityCheckErrorsInCurrFD = 0;
    if (RequireNullabilityCheck) {
      NullabilityCheckDiagReporter NullabilityCheckReporter(*this);
      runNullabilityCheck(*FD, *AC.getCFG(), AC, NullabilityCheckReporter,
                          Context);
      NullabilityCheckReporter.flushDiagnostics();
      NumNullabilityCheckErrorsInCurrFD =
          NullabilityCheckReporter.getNumErrors();

    }
    // Step two: Run ownership analysis when there is no nullability errors in
    // current function.
    if (RequireBorrowCheck && !NumNullabilityCheckErrorsInCurrFD) {
      OwnershipDiagReporter OwnershipReporter(*this);
      runOwnershipAnalysis(*FD, *AC.getCFG(), AC, OwnershipReporter, Context);
      OwnershipReporter.flushDiagnostics();
      // Step three: Run borrow checker when there is no other ownership errors and
      // nullability in current function.
      if (!OwnershipReporter.getNumErrors()) {
        BSCBorrowChecker(const_cast<FunctionDecl *>(FD));
      }
    }
  }
}

struct ReplaceNodesMap {
  /// When we replace an AST node `p` with an AST node `q`, we use `q` as value
  /// and use `p` as key and insert into the map.
  /// When we execute BorrowCheckerEpilogue, when we find the key of an AST
  /// node, we replace it with the corresponding value.
  llvm::DenseMap<Expr *, Expr *> replacedExprsMap;
  llvm::DenseMap<Stmt *, Stmt *> replacedStmtsMap;

  bool Contains(Expr *E) const {
    return replacedExprsMap.find(E) != replacedExprsMap.end();
  }

  bool Contains(Stmt *S) const {
    return replacedStmtsMap.find(S) != replacedStmtsMap.end();
  }

  void Insert(Expr *Key, Expr *Value) {
    if (Key != Value)
      replacedExprsMap[Key] = Value;
  }

  void Insert(Stmt *Key, Stmt *Value) {
    if (Key != Value)
      replacedStmtsMap[Key] = Value;
  }

  Expr *Get(Expr *Key) { return replacedExprsMap[Key]; }

  Stmt *Get(Stmt *Key) { return replacedStmtsMap[Key]; }
};

/// Before running borrow checker, introduce some temporary variables to adjust
/// FunctionDecl in the AST, replacing nested function calls and complex
/// expressions.
///
/// The complexity for AST nodes presents significant challenges for
/// implementing the borrow checker. For example, scenarios like `foo(bar())`
/// are not convenient for analysis. To handle such cases, we use a temporary
/// variable to store the return value of `bar()` before passing it as an
/// argument to `foo()`, ensuring a semantically equivalent transformation.
class BorrowCheckerPrologue : public TreeTransform<BorrowCheckerPrologue> {
  typedef TreeTransform<BorrowCheckerPrologue> BaseTransform;
  typedef llvm::SmallVector<Stmt *, 8> StmtVector;

  FunctionDecl *FD;
  // Statements of the CompoundStmt currently being transformed.
  // Used to build replacement CompoundStmts during transformation.
  StmtVector Stmts;
  // Temporary declarations belong to the CompoundStmt being transformed,
  // independently of synthetic statement expressions used for evaluation.
  StmtVector TempDecls;
  unsigned TempVarCounter = 0;
  ReplaceNodesMap &replacedNodesMap;

  VarDecl *NewTempVar(QualType T) {
    std::string Name = "_borrowck_tmp_" + std::to_string(TempVarCounter++);
    VarDecl *VD = VarDecl::Create(
        getSema().Context, FD, SourceLocation(), SourceLocation(),
        &getSema().Context.Idents.get(Name), T, nullptr, SC_None);
    DeclStmt *DS = new (getSema().Context)
        DeclStmt(DeclGroupRef(VD), SourceLocation(), SourceLocation());
    TempDecls.push_back(DS);
    return VD;
  }

  Expr *CreateTempRef(VarDecl *VD, SourceLocation Loc, bool AsRValue) {
    DeclRefExpr *DRE = DeclRefExpr::Create(
        getSema().Context, NestedNameSpecifierLoc(), SourceLocation(), VD,
        false, Loc, VD->getType(), VK_LValue);
    if (AsRValue)
      return getSema().DefaultLvalueConversion(DRE).get();
    return DRE;
  }

  // Preserve array-to-pointer decay when required by the original AST.
  ExprResult MaybeDecayArrayToPointer(Expr *E, bool NeedDecay) {
    if (!NeedDecay)
      return E;

    QualType DecayedTy =
        getSema().Context.getArrayDecayedType(E->getType());
    return getSema().ImpCastExprToType(E, DecayedTy,
                                       CK_ArrayToPointerDecay);
  }

  /// Normalize E as a stable place. NeedDecay preserves an enclosing
  /// array-to-pointer conversion after the place itself has been normalized.
  ExprResult AsPlace(Expr *E, bool NeedDecay = false) {
    switch (E->getStmtClass()) {
    case Stmt::DeclRefExprClass:
      return MaybeDecayArrayToPointer(E, NeedDecay);
    case Stmt::MemberExprClass: {
      MemberExpr *ME = cast<MemberExpr>(E);
      ExprResult Base = AsPlace(ME->getBase());
      ME->setBase(Base.get());
      return MaybeDecayArrayToPointer(ME, NeedDecay);
    }
    case Stmt::ArraySubscriptExprClass: {
      ArraySubscriptExpr *ASE = cast<ArraySubscriptExpr>(E);
      ExprResult Base = AsPlace(ASE->getLHS());
      ASE->setLHS(Base.get());
      ExprResult Index = AsTemp(ASE->getRHS());
      ASE->setRHS(Index.get());
      return MaybeDecayArrayToPointer(ASE, NeedDecay);
    }
    case Stmt::UnaryOperatorClass: {
      UnaryOperator *UO = cast<UnaryOperator>(E);
      if (UO->getOpcode() == UO_Extension) {
        ExprResult SubExpr = AsPlace(UO->getSubExpr());
        UO->setSubExpr(SubExpr.get());
        return MaybeDecayArrayToPointer(UO, NeedDecay);
      }

      if (UO->getOpcode() != UO_Deref) {
        return AsTemp(UO);
      }

      ExprResult SubExpr = AsPlace(UO->getSubExpr());
      UO->setSubExpr(SubExpr.get());
      return MaybeDecayArrayToPointer(UO, NeedDecay);
    }
    case Stmt::BinaryOperatorClass: {
      BinaryOperator *BO = cast<BinaryOperator>(E);
      if (!BO->getType().isBorrowQualified() ||
          (BO->getOpcode() != BO_Add && BO->getOpcode() != BO_Sub)) {
        return AsTemp(BO);
      }

      // Pointer arithmetic preserves the abstract place rooted at its pointer
      // operand. Materialize the offset so its access has a separate action.
      if (BO->getLHS()->getType()->isPointerType()) {
        ExprResult LHS = AsPlace(BO->getLHS());
        BO->setLHS(LHS.get());
        ExprResult RHS = AsTemp(BO->getRHS());
        BO->setRHS(RHS.get());
      } else {
        ExprResult LHS = AsTemp(BO->getLHS());
        BO->setLHS(LHS.get());
        ExprResult RHS = AsPlace(BO->getRHS());
        BO->setRHS(RHS.get());
      }
      // Binary operators cannot produce arrays; their operands handle decay.
      return BO;
    }
    case Stmt::ParenExprClass: {
      ParenExpr *PE = cast<ParenExpr>(E);
      ExprResult SubExpr = AsPlace(PE->getSubExpr());
      PE->setSubExpr(SubExpr.get());
      return MaybeDecayArrayToPointer(PE, NeedDecay);
    }
    case Stmt::ImplicitCastExprClass: {
      ImplicitCastExpr *ICE = cast<ImplicitCastExpr>(E);
      if (ICE->getCastKind() == CK_ArrayToPointerDecay) {
        ExprResult Result = AsPlace(ICE->getSubExpr(), true);
        replacedNodesMap.Insert(Result.get(), ICE);
        return Result;
      }
      if (ICE->getCastKind() == CK_NullToPointer) {
        // Null-to-pointer creates a pointer value from an expression that does
        // not denote a place, so materialize the conversion as a whole.
        return AsTemp(ICE);
      }
      ExprResult SubExpr = AsPlace(ICE->getSubExpr());
      ICE->setSubExpr(SubExpr.get());
      return MaybeDecayArrayToPointer(ICE, NeedDecay);
    }
    case Stmt::SafeExprClass: {
      SafeExpr *SE = cast<SafeExpr>(E);
      ExprResult SubExpr = AsPlace(SE->getSubExpr());
      SE->setSubExpr(SubExpr.get());
      return MaybeDecayArrayToPointer(SE, NeedDecay);
    }
    case Stmt::SubstNonTypeTemplateParmExprClass: {
      // Transparent wrapper around the substituted value; lower the inner
      // expression directly.
      SubstNonTypeTemplateParmExpr *SNTTP =
          cast<SubstNonTypeTemplateParmExpr>(E);
      return AsPlace(SNTTP->getReplacement(), NeedDecay);
    }
    default: {
      ExprResult Result = MaybeDecayArrayToPointer(E, NeedDecay);
      return AsTemp(Result.get());
    }
    }
  }

  // Normalize a discarded expression without producing a destination value.
  void AsDiscarded(Expr *E) { ExprIntoDest(nullptr, E); }

  void PushAssignOrExpr(Expr *Dest, Expr *E) {
    // A discarded expression has no destination, so emit it directly instead
    // of creating a synthetic assignment.
    if (!Dest) {
      Stmts.push_back(E);
      return;
    }

    BinaryOperator *Assign = BinaryOperator::Create(
        getSema().Context, Dest, E, BO_Assign, Dest->getType(), VK_PRValue,
        OK_Ordinary, E->getExprLoc(), FPOptionsOverride());
    Stmts.push_back(Assign);
  }

  /// Normalize E and emit an explicit assignment into Dest. Nested comma and
  /// assignment expressions emit their preceding writes into Stmts first, so
  /// every write is represented by its own CFG action site.
  void ExprIntoDest(Expr *Dest, Expr *E) {
    switch (E->getStmtClass()) {
    case Stmt::ArraySubscriptExprClass: {
      ArraySubscriptExpr *ASE = cast<ArraySubscriptExpr>(E);
      ExprResult Source = AsPlace(ASE);
      PushAssignOrExpr(Dest, Source.get());
      return;
    }
    case Stmt::MemberExprClass: {
      MemberExpr *ME = cast<MemberExpr>(E);
      ExprResult Source = AsPlace(ME);
      PushAssignOrExpr(Dest, Source.get());
      return;
    }
    case Stmt::BinaryConditionalOperatorClass: {
      BinaryConditionalOperator *BCO = cast<BinaryConditionalOperator>(E);
      PushAssignOrExpr(Dest, BCO);
      return;
    }
    case Stmt::BinaryOperatorClass: {
      BinaryOperator *BO = cast<BinaryOperator>(E);

      if (BO->getOpcode() == BO_Comma) {
        // Evaluate the LHS for its effects; only the RHS yields the result.
        AsDiscarded(BO->getLHS());
        ExprIntoDest(Dest, BO->getRHS());
        return;
      }

      if (BO->getOpcode() == BO_Assign) {
        ExprResult LHS = AsPlace(BO->getLHS());

        // A discarded assignment only performs the write; its result does not
        // need to be forwarded through a temporary.
        if (!Dest) {
          ExprResult RHS = AsOperand(BO->getRHS());
          PushAssignOrExpr(LHS.get(), RHS.get());
          return;
        }

        // Lower `Dest = (LHS = RHS)` to
        // `Tmp = RHS; LHS = Tmp; Dest = Tmp`.
        VarDecl *TempVD = NewTempVar(BO->getType());
        Expr *LValue = CreateTempRef(TempVD, BO->getBeginLoc(), false);
        ExprIntoDest(LValue, BO->getRHS());

        Expr *LHSRValue =
            CreateTempRef(TempVD, BO->getBeginLoc(), true);
        PushAssignOrExpr(LHS.get(), LHSRValue);

        Expr *DestRValue =
            CreateTempRef(TempVD, BO->getBeginLoc(), true);
        PushAssignOrExpr(Dest, DestRValue);
        return;
      }

      if (BO->isLogicalOp()) {
        auto BuildOperand = [&](Expr *Operand) {
          llvm::SaveAndRestore<StmtVector> StmtsRestore(Stmts, StmtVector());
          ExprResult Result = AsOperand(Operand);
          Stmts.push_back(Result.get());

          CompoundStmt *CS = CompoundStmt::Create(
              SemaRef.Context, Stmts, FPOptionsOverride(),
              Operand->getBeginLoc(), Operand->getEndLoc(),
              SafeZoneSpecifier::SZ_None);
          StmtExpr *SE = new (SemaRef.Context)
              StmtExpr(CS, Result.get()->getType(), Operand->getBeginLoc(),
                       Operand->getEndLoc(), 0);
          replacedNodesMap.Insert(SE, Operand);
          return SE;
        };

        BO->setLHS(BuildOperand(BO->getLHS()));
        BO->setRHS(BuildOperand(BO->getRHS()));
        PushAssignOrExpr(Dest, BO);
        return;
      }

      if (BO->isMultiplicativeOp() || BO->isAdditiveOp() ||
          BO->isShiftOp() || BO->isRelationalOp() ||
          BO->isEqualityOp() || BO->isBitwiseOp()) {
        ExprResult LHS = AsOperand(BO->getLHS());
        BO->setLHS(LHS.get());

        ExprResult RHS = AsOperand(BO->getRHS());
        BO->setRHS(RHS.get());

        PushAssignOrExpr(Dest, BO);
        return;
      }

      llvm_unreachable("unexpected binary operator");
    }
    case Stmt::CompoundAssignOperatorClass: {
      CompoundAssignOperator *CAO = cast<CompoundAssignOperator>(E);
      ExprResult LHS = AsPlace(CAO->getLHS());
      ExprResult RHS = AsOperand(CAO->getRHS());

      // Lower compound assignment to `Tmp = LHS op RHS; LHS = Tmp` and
      // forward Tmp into Dest only when the enclosing expression consumes the
      // result. AsPlace materializes side-effecting components, so the same
      // stable place can be reused.
      ExprResult Computation = getSema().BuildBinOp(
          nullptr, CAO->getOperatorLoc(),
          BinaryOperator::getOpForCompoundAssignment(CAO->getOpcode()),
          LHS.get(), RHS.get());
      ExprResult Result = getSema().PerformImplicitConversion(
          Computation.get(), CAO->getType(), Sema::AA_Assigning);

      VarDecl *TempVD = NewTempVar(CAO->getType());
      Expr *TempLValue = CreateTempRef(TempVD, CAO->getBeginLoc(), false);
      PushAssignOrExpr(TempLValue, Result.get());

      Expr *LHSRValue = CreateTempRef(TempVD, CAO->getBeginLoc(), true);
      PushAssignOrExpr(LHS.get(), LHSRValue);

      if (Dest) {
        Expr *DestRValue = CreateTempRef(TempVD, CAO->getBeginLoc(), true);
        PushAssignOrExpr(Dest, DestRValue);
      }
      return;
    }
    case Stmt::CallExprClass: {
      CallExpr *CE = cast<CallExpr>(E);
      if (!CE->getDirectCallee()) {
        ExprResult Callee = AsOperand(CE->getCallee());
        CE->setCallee(Callee.get());
      }

      for (unsigned I = 0; I < CE->getNumArgs(); ++I) {
        ExprResult Arg = AsOperand(CE->getArg(I));
        CE->setArg(I, Arg.get());
      }
      PushAssignOrExpr(Dest, CE);
      return;
    }
    case Stmt::CompoundLiteralExprClass: {
      CompoundLiteralExpr *CLE = cast<CompoundLiteralExpr>(E);
      ExprIntoDest(Dest, CLE->getInitializer());
      return;
    }
    case Stmt::ConditionalOperatorClass: {
      ConditionalOperator *CO = cast<ConditionalOperator>(E);
      ExprResult Cond = AsCondition(CO->getCond());
      VarDecl *TempVD = Dest ? NewTempVar(CO->getType()) : nullptr;

      auto BuildBranch = [&](Expr *Branch) {
        llvm::SaveAndRestore<StmtVector> StmtsRestore(Stmts, StmtVector());
        Expr *BranchDest = nullptr;
        if (TempVD)
          BranchDest =
              CreateTempRef(TempVD, Branch->getBeginLoc(), false);
        ExprIntoDest(BranchDest, Branch);
        return CompoundStmt::Create(
            SemaRef.Context, Stmts, FPOptionsOverride(),
            Branch->getBeginLoc(), Branch->getEndLoc(),
            SafeZoneSpecifier::SZ_None);
      };

      CompoundStmt *TrueCS = BuildBranch(CO->getTrueExpr());
      CompoundStmt *FalseCS = BuildBranch(CO->getFalseExpr());
      IfStmt *IS = IfStmt::Create(
          SemaRef.Context, SourceLocation(), IfStatementKind::Ordinary,
          nullptr, nullptr, Cond.get(), SourceLocation(), SourceLocation(),
          TrueCS, SourceLocation(), FalseCS);
      Stmts.push_back(IS);

      if (Dest) {
        Expr *Result = CreateTempRef(TempVD, CO->getExprLoc(), true);
        PushAssignOrExpr(Dest, Result);
      }
      return;
    }
    case Stmt::CStyleCastExprClass: {
      CStyleCastExpr *CSCE = cast<CStyleCastExpr>(E);

      // A void operand has no value for AsOperand; the outer cast only
      // discards its evaluation.
      if (CSCE->getSubExpr()->getType()->isVoidType()) {
        AsDiscarded(CSCE->getSubExpr());
        return;
      }

      // A discarded borrow cast still needs a destination type for its region
      // constraints.
      if (!Dest && CSCE->getType().isBorrowQualified()) {
        AsTemp(CSCE);
        return;
      }

      ExprResult SubExpr = AsPlace(CSCE->getSubExpr());
      CSCE->setSubExpr(SubExpr.get());
      PushAssignOrExpr(Dest, CSCE);
      return;
    }
    case Stmt::InitListExprClass: {
      InitListExpr *ILE = cast<InitListExpr>(E);
      for (unsigned I = 0; I < ILE->getNumInits(); ++I) {
        ExprResult Init = AsOperand(ILE->getInit(I));
        ILE->setInit(I, Init.get());
      }
      PushAssignOrExpr(Dest, ILE);
      return;
    }
    case Stmt::ParenExprClass: {
      ParenExpr *PE = cast<ParenExpr>(E);
      ExprIntoDest(Dest, PE->getSubExpr());
      return;
    }
    case Stmt::ImplicitCastExprClass: {
      ImplicitCastExpr *ICE = cast<ImplicitCastExpr>(E);
      ExprResult SubExpr = AsOperand(ICE->getSubExpr());
      ICE->setSubExpr(SubExpr.get());
      PushAssignOrExpr(Dest, ICE);
      return;
    }
    case Stmt::SafeExprClass: {
      SafeExpr *SE = cast<SafeExpr>(E);
      ExprIntoDest(Dest, SE->getSubExpr());
      return;
    }
    case Stmt::SubstNonTypeTemplateParmExprClass: {
      // Transparent wrapper around the substituted value; lower the inner
      // expression directly.
      SubstNonTypeTemplateParmExpr *SNTTP =
          cast<SubstNonTypeTemplateParmExpr>(E);
      ExprIntoDest(Dest, SNTTP->getReplacement());
      return;
    }
    case Stmt::StmtExprClass: {
      StmtExpr *SE = cast<StmtExpr>(E);
      StmtResult Res =
          getDerived().TransformCompoundStmt(SE->getSubStmt(), true);
      SE->setSubStmt(Res.getAs<CompoundStmt>());
      PushAssignOrExpr(Dest, SE);
      return;
    }
    case Stmt::UnaryOperatorClass: {
      UnaryOperator *UO = cast<UnaryOperator>(E);

      if (UO->getOpcode() == UO_Extension) {
        ExprIntoDest(Dest, UO->getSubExpr());
        return;
      }

      if (UO->getOpcode() == UO_Deref) {
        ExprResult Source = AsPlace(UO);
        PushAssignOrExpr(Dest, Source.get());
        return;
      }

      if (UO->getOpcode() == UO_AddrOf ||
          UO->getOpcode() == UO_AddrMut ||
          UO->getOpcode() == UO_AddrConst ||
          UO->getOpcode() == UO_AddrMutDeref ||
          UO->getOpcode() == UO_AddrConstDeref) {
        // A borrow action needs a destination type for its region constraints.
        // Materialize a discarded borrow without keeping its value live.
        if (!Dest && UO->getType().isBorrowQualified()) {
          AsTemp(UO);
          return;
        }

        ExprResult SubExpr = AsPlace(UO->getSubExpr());
        UO->setSubExpr(SubExpr.get());
        PushAssignOrExpr(Dest, UO);
        return;
      }

      if (UO->isArithmeticOp()) {
        ExprResult SubExpr = AsOperand(UO->getSubExpr());
        UO->setSubExpr(SubExpr.get());
        PushAssignOrExpr(Dest, UO);
        return;
      }

      if (UO->isIncrementDecrementOp()) {
        ExprResult SubExpr = AsPlace(UO->getSubExpr());
        UO->setSubExpr(SubExpr.get());
        PushAssignOrExpr(Dest, UO);
        return;
      }

      llvm_unreachable("unexpected unary operator");
    }
    case Stmt::VAArgExprClass: {
      VAArgExpr *VAE = cast<VAArgExpr>(E);
      ExprResult SubExpr = AsOperand(VAE->getSubExpr());
      VAE->setSubExpr(SubExpr.get());
      PushAssignOrExpr(Dest, VAE);
      return;
    }
    case Stmt::AtomicExprClass: {
      AtomicExpr *AE = cast<AtomicExpr>(E);
      for (unsigned I = 0; I < AE->getNumSubExprs(); ++I) {
        ExprResult Sub = AsOperand(AE->getSubExprs()[I]);
        AE->getSubExprs()[I] = Sub.get();
      }
      PushAssignOrExpr(Dest, AE);
      return;
    }
    case Stmt::CharacterLiteralClass:
    case Stmt::CXXNullPtrLiteralExprClass:
    case Stmt::DeclRefExprClass:
    case Stmt::FloatingLiteralClass:
    case Stmt::GNUNullExprClass:
    case Stmt::ImplicitValueInitExprClass:
    case Stmt::IntegerLiteralClass:
    case Stmt::PredefinedExprClass:
    case Stmt::StringLiteralClass:
    case Stmt::UnaryExprOrTypeTraitExprClass:
      PushAssignOrExpr(Dest, E);
      return;
    case Stmt::AwaitExprClass:
      llvm_unreachable("await expression is not implemented yet");
    default:
      llvm_unreachable("unsupported expression");
    }
  }

  ExprResult AsTemp(Expr *E) {
    VarDecl *VD = NewTempVar(E->getType());

    Expr *Dest = CreateTempRef(VD, E->getBeginLoc(), false);
    ExprIntoDest(Dest, E);

    Expr *Ref = CreateTempRef(VD, E->getBeginLoc(), E->isPRValue());
    replacedNodesMap.Insert(Ref, E);
    return Ref;
  }

  ExprResult AsOperand(Expr *E) {
    switch (E->getStmtClass()) {
    case Stmt::CXXNullPtrLiteralExprClass:
    case Stmt::ImplicitValueInitExprClass:
    case Stmt::IntegerLiteralClass:
    case Stmt::CharacterLiteralClass:
    case Stmt::FloatingLiteralClass:
    case Stmt::PredefinedExprClass:
    case Stmt::StringLiteralClass:
    case Stmt::UnaryExprOrTypeTraitExprClass:
      return E;
    case Stmt::ParenExprClass: {
      ParenExpr *PE = cast<ParenExpr>(E);
      ExprResult SubExpr = AsOperand(PE->getSubExpr());
      PE->setSubExpr(SubExpr.get());
      return PE;
    }
    case Stmt::ImplicitCastExprClass: {
      ImplicitCastExpr *ICE = cast<ImplicitCastExpr>(E);
      if (ICE->getCastKind() == CK_ArrayToPointerDecay) {
        // Don't reuse the original ICE; forward the pre-decay sub-expr to
        // build a fresh decay replacement.
        ExprResult Decayed =
            MaybeDecayArrayToPointer(ICE->getSubExpr(), true);
        ExprResult Result = AsTemp(Decayed.get());
        replacedNodesMap.Insert(Result.get(), ICE);
        return Result;
      }
      ExprResult SubExpr = AsOperand(ICE->getSubExpr());
      ICE->setSubExpr(SubExpr.get());
      return ICE;
    }
    case Stmt::SafeExprClass: {
      SafeExpr *SE = cast<SafeExpr>(E);
      ExprResult SubExpr = AsOperand(SE->getSubExpr());
      SE->setSubExpr(SubExpr.get());
      return SE;
    }
    case Stmt::SubstNonTypeTemplateParmExprClass: {
      // Transparent wrapper around the substituted value; lower the inner
      // expression directly.
      SubstNonTypeTemplateParmExpr *SNTTP =
          cast<SubstNonTypeTemplateParmExpr>(E);
      return AsOperand(SNTTP->getReplacement());
    }
    default:
      return AsTemp(E);
    }
  }

  ExprResult AsCondition(Expr *E) {
    llvm::SaveAndRestore<StmtVector> StmtsRestore(Stmts, StmtVector());
    ExprResult Result = AsOperand(E);

    Stmts.push_back(Result.get());
    CompoundStmt *CS = CompoundStmt::Create(
        SemaRef.Context, Stmts, FPOptionsOverride(), E->getBeginLoc(),
        E->getEndLoc(), SafeZoneSpecifier::SZ_None);
    StmtExpr *SE = new (SemaRef.Context)
        StmtExpr(CS, Result.get()->getType(), E->getBeginLoc(), E->getEndLoc(),
                 0);
    replacedNodesMap.Insert(SE, E);
    return SE;
  }

  ExprResult AsLoopIncrement(Expr *E) {
    llvm::SaveAndRestore<StmtVector> StmtsRestore(Stmts, StmtVector());
    AsDiscarded(E);

    CompoundStmt *CS = CompoundStmt::Create(
        SemaRef.Context, Stmts, FPOptionsOverride(), E->getBeginLoc(),
        E->getEndLoc(), SafeZoneSpecifier::SZ_None);
    StmtExpr *SE = new (SemaRef.Context)
        StmtExpr(CS, E->getType(), E->getBeginLoc(), E->getEndLoc(), 0);
    replacedNodesMap.Insert(SE, E);
    return SE;
  }

  // Ensure the given statement is wrapped with a CompoundStmt. If not, create
  // a CompoundStmt to wrap it.
  CompoundStmt *EnsureWrappedWithCompoundStmt(Stmt *S) {
    if (isa<CompoundStmt>(S))
      return cast<CompoundStmt>(S);
    return CompoundStmt::Create(SemaRef.Context, S, FPOptionsOverride(),
                                S->getBeginLoc(), S->getEndLoc());
  }

public:
  BorrowCheckerPrologue(Sema &SemaRef, FunctionDecl *FD,
                        ReplaceNodesMap &replacedNodesMap)
      : BaseTransform(SemaRef), FD(FD), replacedNodesMap(replacedNodesMap) {}

  // Don't redo semantic analysis to ensure that AST nodes are not rebuilt to
  // affect destructor insertion and AST recovery.
  bool AlwaysRebuild() { return false; }

  void applyTransform() {
    StmtResult Res = BaseTransform::TransformStmt(FD->getBody());
    FD->setBody(Res.get());
  }

  // Avoid Exprs redo semantic checking.
  StmtResult TransformStmt(Stmt *S, StmtDiscardKind SDK = SDK_Discarded) {
    if (!S)
      return S;

    switch (S->getStmtClass()) {
    case Stmt::NoStmtClass:
      break;

// Transform individual statement nodes
// Pass SDK into statements that can produce a value
#define STMT(Node, Parent)                                                     \
  case Stmt::Node##Class:                                                      \
    return getDerived().Transform##Node(cast<Node>(S));
#define VALUESTMT(Node, Parent)                                                \
  case Stmt::Node##Class:                                                      \
    return getDerived().Transform##Node(cast<Node>(S), SDK);
#define ABSTRACT_STMT(Node)
#define EXPR(Node, Parent)
#include "clang/AST/StmtNodes.inc"

// Transform expressions by calling TransformExpr.
#define STMT(Node, Parent)
#define ABSTRACT_STMT(Stmt)
#define EXPR(Node, Parent) case Stmt::Node##Class:
#include "clang/AST/StmtNodes.inc"
      {
        AsDiscarded(cast<Expr>(S));
        return Stmts.pop_back_val();
      }
    }

    return S;
  }

  // Create a NullStmt to replace the sub stmt of CaseStmt, and add
  // it to the Stmts vector.
  // Note: No need to handle LHS and RHS of CaseStmt, because it's always a
  // constant expression.
  StmtResult TransformCaseStmt(CaseStmt *CS) {
    Stmt *Sub = CS->getSubStmt();
    NullStmt *NS = new (getSema().Context) NullStmt(Sub->getBeginLoc());
    CS->setSubStmt(NS);
    replacedNodesMap.Insert(NS, Sub);
    Stmts.push_back(CS);

    StmtResult ResSub = getDerived().TransformStmt(Sub);
    return ResSub;
  }

  StmtResult TransformCompoundStmt(CompoundStmt *CS) {
    return TransformCompoundStmt(CS, false);
  }

  // Transform each stmt in the CompoundStmt.
  StmtResult TransformCompoundStmt(CompoundStmt *CS, bool IsStmtExpr) {
    if (CS == nullptr)
      return CS;

    llvm::SaveAndRestore<StmtVector> StmtsRestore(Stmts, StmtVector());
    llvm::SaveAndRestore<StmtVector> TempDeclsRestore(TempDecls,
                                                     StmtVector());
    // Traverse and transform all statements in the compound statement.
    unsigned Index = 0;
    for (Stmt *S : CS->body()) {
      bool IsLast = ++Index == CS->size();
      Expr *E = dyn_cast<Expr>(S);
      if (IsStmtExpr && IsLast && E && !E->getType()->isVoidType()) {
        ExprResult Res = AsOperand(E);
        Stmts.push_back(Res.get());
        continue;
      }

      StmtResult Res = getDerived().TransformStmt(S);
      Stmts.push_back(Res.getAs<Stmt>());
    }
    Stmts.insert(Stmts.begin(), TempDecls.begin(), TempDecls.end());
    CompoundStmt *NewCS = CompoundStmt::Create(
        SemaRef.Context, Stmts, FPOptionsOverride(), CS->getLBracLoc(),
        CS->getRBracLoc(), CS->getCompSafeZoneSpecifier());
    replacedNodesMap.Insert(NewCS, CS);

    return NewCS;
  }

  StmtResult TransformDeclStmt(DeclStmt *DS) {
    for (Decl *D : DS->decls()) {
      VarDecl *VD = dyn_cast<VarDecl>(D);
      if (!VD || !VD->hasInit())
        continue;

      Expr *Init = VD->getInit();
      llvm::SaveAndRestore<StmtVector> StmtsRestore(Stmts, StmtVector());

      ExprResult Result = AsOperand(Init);
      Stmts.push_back(Result.get());

      CompoundStmt *CS = CompoundStmt::Create(
          SemaRef.Context, Stmts, FPOptionsOverride(), Init->getBeginLoc(),
          Init->getEndLoc(), SafeZoneSpecifier::SZ_None);
      StmtExpr *SE = new (SemaRef.Context)
          StmtExpr(CS, Result.get()->getType(), Init->getBeginLoc(),
                   Init->getEndLoc(), 0);
      replacedNodesMap.Insert(SE, Init);
      VD->setInit(SE);
    }

    return DS;
  }

  // Create a NullStmt to replace the sub stmt of LabelStmt, and add it to the
  // Stmts vector.
  StmtResult TransformDefaultStmt(DefaultStmt *DS) {
    Stmt *Sub = DS->getSubStmt();
    NullStmt *NS = new (getSema().Context) NullStmt(Sub->getBeginLoc());
    DS->setSubStmt(NS);
    replacedNodesMap.Insert(NS, Sub);
    Stmts.push_back(DS);

    StmtResult ResSub = getDerived().TransformStmt(Sub);
    return ResSub;
  }

  // Transform DoStmt which has the form `do body while (cond)` into
  // `do { body } while (({x = cond; x;}))`. After the transformation, `cond`
  // is ensured to be a StmtExpr and `body` is ensured to be a CompoundStmt,
  // so that the DoStmt can be transformed correctly by the prologue.
  StmtResult TransformDoStmt(DoStmt *DS) {
    Stmt *Body = DS->getBody();
    CompoundStmt *CSBody = EnsureWrappedWithCompoundStmt(Body);
    StmtResult ResBody = getDerived().TransformStmt(CSBody);
    DS->setBody(ResBody.get());
    replacedNodesMap.Insert(ResBody.get(), Body);

    Expr *Cond = DS->getCond();
    ExprResult ResCond = AsCondition(Cond);
    DS->setCond(ResCond.get());

    return DS;
  }

  // Transform ForStmt which has the form `for (init-opt; cond-opt; inc-opt)
  // body` into `for (init-opt; ({x = cond; x;})-opt; ({y = inc; y;})-opt) {
  // body }`. After the transformation, `cond` and `inc` are ensured to be a
  // StmtExpr and `body` is ensured to be a CompoundStmt, so that the ForStmt
  // can be transformed correctly by the prologue.
  StmtResult TransformForStmt(ForStmt *FS) {
    if (Stmt *Init = FS->getInit()) {
      StmtResult ResInit = getDerived().TransformStmt(Init);
      FS->setInit(ResInit.get());
      replacedNodesMap.Insert(ResInit.get(), Init);
    }

    if (Expr *Cond = FS->getCond()) {
      ExprResult ResCond = AsCondition(Cond);
      FS->setCond(ResCond.get());
    }

    if (Expr *Inc = FS->getInc()) {
      ExprResult ResInc = AsLoopIncrement(Inc);
      FS->setInc(ResInc.get());
    }

    Stmt *Body = FS->getBody();
    CompoundStmt *CSBody = EnsureWrappedWithCompoundStmt(Body);
    StmtResult ResBody = getDerived().TransformStmt(CSBody);
    FS->setBody(ResBody.get());
    replacedNodesMap.Insert(ResBody.get(), Body);

    return FS;
  }

  StmtResult TransformIfStmt(IfStmt *IS) {
    Expr *Cond = IS->getCond();
    if (!isa<ConstantExpr>(Cond)) {
      ExprResult ResCond = AsCondition(Cond);
      IS->setCond(ResCond.get());
    }

    Stmt *Then = IS->getThen();
    CompoundStmt *CSThen = EnsureWrappedWithCompoundStmt(Then);
    StmtResult ResThen = getDerived().TransformStmt(CSThen);
    IS->setThen(ResThen.get());
    replacedNodesMap.Insert(ResThen.get(), Then);

    if (Stmt *Else = IS->getElse()) {
      CompoundStmt *CSElse = EnsureWrappedWithCompoundStmt(Else);
      StmtResult ResElse = getDerived().TransformStmt(CSElse);
      IS->setElse(ResElse.get());
      replacedNodesMap.Insert(ResElse.get(), Else);
    }

    return IS;
  }

  // Create a NullStmt to replace the sub stmt of LabelStmt, and add it to the
  // Stmts vector.
  StmtResult TransformLabelStmt(LabelStmt *LS, StmtDiscardKind SDK) {
    Stmt *Sub = LS->getSubStmt();
    NullStmt *NS = new (getSema().Context) NullStmt(Sub->getBeginLoc());
    LS->setSubStmt(NS);
    replacedNodesMap.Insert(NS, Sub);
    Stmts.push_back(LS);

    StmtResult ResSub = getDerived().TransformStmt(Sub);
    return ResSub;
  }

  StmtResult TransformReturnStmt(ReturnStmt *RS) {
    Expr *RV = RS->getRetValue();
    if (!RV)
      return RS;

    if (RV->getType()->isVoidType()) {
      AsDiscarded(RV);

      ReturnStmt *NewRS = ReturnStmt::Create(
          getSema().Context, RS->getReturnLoc(), nullptr,
          /*NRVOCandidate=*/nullptr);
      replacedNodesMap.Insert(NewRS, RS);
      return NewRS;
    }

    ExprResult Res = AsOperand(RV);
    RS->setRetValue(Res.get());
    return RS;
  }

  StmtResult TransformSafeStmt(SafeStmt *SS) {
    StmtResult Res = getDerived().TransformStmt(SS->getSubStmt());
    SS->setSubStmt(Res.get());

    return SS;
  }

  StmtResult TransformSwitchStmt(SwitchStmt *SS) {
    Expr *Cond = SS->getCond();
    ExprResult ResCond = AsOperand(Cond);
    SS->setCond(ResCond.get());

    Stmt *Body = SS->getBody();
    Body = EnsureWrappedWithCompoundStmt(Body);
    StmtResult ResBody = getDerived().TransformStmt(Body);
    SS->setBody(ResBody.get());
    replacedNodesMap.Insert(ResBody.get(), Body);

    return SS;
  }

  // Transform WhileStmt which has the form `while (cond) body` into
  // `while (({ x = cond; x; })) { body }`. After the transformation, `cond` is
  // ensured to be a StmtExpr and `body` is ensured to be a CompoundStmt,
  // so that the WhileStmt can be transformed correctly by the prologue.
  StmtResult TransformWhileStmt(WhileStmt *WS) {
    Expr *Cond = WS->getCond();
    ExprResult ResCond = AsCondition(Cond);
    WS->setCond(ResCond.get());

    Stmt *Body = WS->getBody();
    CompoundStmt *CSBody = EnsureWrappedWithCompoundStmt(Body);
    StmtResult ResBody = getDerived().TransformStmt(CSBody);
    WS->setBody(ResBody.get());
    replacedNodesMap.Insert(ResBody.get(), Body);

    return WS;
  }
};

/// After running borrow checker, restore the AST to its original form to avoid
/// any impact on other compiler phases caused by AST transformations.
class BorrowCheckerEpilogue : public TreeTransform<BorrowCheckerEpilogue> {
  typedef TreeTransform<BorrowCheckerEpilogue> BaseTransform;

  FunctionDecl *FD;
  ReplaceNodesMap &replacedNodesMap;

public:
  BorrowCheckerEpilogue(Sema &SemaRef, FunctionDecl *FD,
                        ReplaceNodesMap &replacedNodesMap)
      : BaseTransform(SemaRef), FD(FD), replacedNodesMap(replacedNodesMap) {}

  // Don't redo semantic analysis to ensure that AST nodes are not rebuilt to
  // affect destructor insertion.
  bool AlwaysRebuild() { return false; }

  ExprResult TransformConstantExpr(ConstantExpr *E) { return E; }

  void applyTransform() {
    StmtResult Res = BaseTransform::TransformStmt(FD->getBody());
    FD->setBody(Res.get());
  }

  // Avoid Exprs redo semantic checking.
  StmtResult TransformStmt(Stmt *S, StmtDiscardKind SDK = SDK_Discarded) {
    if (!S)
      return S;

    switch (S->getStmtClass()) {
    case Stmt::NoStmtClass:
      break;

// Transform individual statement nodes
// Pass SDK into statements that can produce a value
#define STMT(Node, Parent)                                                     \
  case Stmt::Node##Class:                                                      \
    return getDerived().Transform##Node(cast<Node>(S));
#define VALUESTMT(Node, Parent)                                                \
  case Stmt::Node##Class:                                                      \
    return getDerived().Transform##Node(cast<Node>(S), SDK);
#define ABSTRACT_STMT(Node)
#define EXPR(Node, Parent)
#include "clang/AST/StmtNodes.inc"

// Transform expressions by calling TransformExpr.
#define STMT(Node, Parent)
#define ABSTRACT_STMT(Stmt)
#define EXPR(Node, Parent) case Stmt::Node##Class:
#include "clang/AST/StmtNodes.inc"
      { return getDerived().TransformExpr(cast<Expr>(S)).get(); }
    }

    return S;
  }

  StmtResult TransformCaseStmt(CaseStmt *CS) {
    Stmt *Sub = CS->getSubStmt();
    if (replacedNodesMap.Contains(Sub)) {
      Sub = replacedNodesMap.Get(Sub);
    }
    StmtResult ResSub = getDerived().TransformStmt(Sub);
    CS->setSubStmt(ResSub.get());
    return CS;
  }

  StmtResult TransformCompoundStmt(CompoundStmt *CS) {
    return TransformCompoundStmt(CS, false);
  }

  // Transform each stmt in the CompoundStmt.
  StmtResult TransformCompoundStmt(CompoundStmt *CS, bool IsStmtExpr) {
    if (CS == nullptr)
      return CS;

    if (replacedNodesMap.Contains(CS)) {
      CS = cast<CompoundStmt>(replacedNodesMap.Get(CS));
    }

    // Traverse and transform all statements in the compound statement.
    for (Stmt *S : CS->body()) {
      BaseTransform::TransformStmt(S);
    }

    return CS;
  }

  StmtResult TransformDeclStmt(DeclStmt *DS) {
    for (Decl *D : DS->decls()) {
      if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
        if (VD->hasInit()) {
          Expr *Init = VD->getInit();
          if (replacedNodesMap.Contains(Init)) {
            Init = replacedNodesMap.Get(Init);
          }
          getDerived().TransformExpr(Init);
          VD->setInit(Init);
        }
      }
    }

    return DS;
  }

  StmtResult TransformDefaultStmt(DefaultStmt *DS) {
    Stmt *Sub = DS->getSubStmt();
    if (replacedNodesMap.Contains(Sub)) {
      Sub = replacedNodesMap.Get(Sub);
    }
    StmtResult ResSub = getDerived().TransformStmt(Sub);
    DS->setSubStmt(ResSub.get());
    return DS;
  }

  StmtResult TransformDoStmt(DoStmt *DS) {
    Stmt *Body = DS->getBody();
    if (replacedNodesMap.Contains(Body)) {
      Body = replacedNodesMap.Get(Body);
    }
    StmtResult ResBody = getDerived().TransformStmt(Body);
    DS->setBody(ResBody.get());

    Expr *Cond = DS->getCond();
    if (replacedNodesMap.Contains(Cond)) {
      Cond = replacedNodesMap.Get(Cond);
    }
    ExprResult ResCond = getDerived().TransformExpr(Cond);
    DS->setCond(ResCond.get());

    return DS;
  }

  StmtResult TransformForStmt(ForStmt *FS) {
    if (Stmt *Init = FS->getInit()) {
      if (replacedNodesMap.Contains(Init)) {
        Init = replacedNodesMap.Get(Init);
      }
      StmtResult ResInit = getDerived().TransformStmt(Init);
      FS->setInit(ResInit.get());
    }

    if (Expr *Cond = FS->getCond()) {
      if (replacedNodesMap.Contains(Cond)) {
        Cond = replacedNodesMap.Get(Cond);
      }
      ExprResult ResCond = getDerived().TransformExpr(Cond);
      FS->setCond(ResCond.get());
    }

    if (Expr *Inc = FS->getInc()) {
      if (replacedNodesMap.Contains(Inc)) {
        Inc = replacedNodesMap.Get(Inc);
      }
      ExprResult ResInc = getDerived().TransformExpr(Inc);
      FS->setInc(ResInc.get());
    }

    Stmt *Body = FS->getBody();
    if (replacedNodesMap.Contains(Body)) {
      Body = replacedNodesMap.Get(Body);
    }
    StmtResult ResBody = getDerived().TransformStmt(Body);
    FS->setBody(ResBody.get());

    return FS;
  }

  StmtResult TransformIfStmt(IfStmt *IS) {
    Expr *Cond = IS->getCond();
    if (replacedNodesMap.Contains(Cond)) {
      Cond = replacedNodesMap.Get(Cond);
    }
    ExprResult ResCond = getDerived().TransformExpr(Cond);
    IS->setCond(ResCond.get());

    Stmt *Then = IS->getThen();
    if (replacedNodesMap.Contains(Then)) {
      Then = replacedNodesMap.Get(Then);
    }
    StmtResult ResThen = getDerived().TransformStmt(Then);
    IS->setThen(ResThen.get());

    if (Stmt *Else = IS->getElse()) {
      if (replacedNodesMap.Contains(Else)) {
        Else = replacedNodesMap.Get(Else);
      }
      StmtResult ResElse = getDerived().TransformStmt(Else);
      IS->setElse(ResElse.get());
    }

    return IS;
  }

  StmtResult TransformLabelStmt(LabelStmt *LS, StmtDiscardKind SDK) {
    Stmt *Sub = LS->getSubStmt();
    if (replacedNodesMap.Contains(Sub)) {
      Sub = replacedNodesMap.Get(Sub);
    }
    StmtResult ResSub = getDerived().TransformStmt(Sub);
    LS->setSubStmt(ResSub.get());
    return LS;
  }

  StmtResult TransformReturnStmt(ReturnStmt *RS) {
    if (replacedNodesMap.Contains(RS))
      return replacedNodesMap.Get(RS);

    if (!RS->getRetValue())
      return RS;

    if (replacedNodesMap.Contains(RS->getRetValue())) {
      RS->setRetValue(replacedNodesMap.Get(RS->getRetValue()));
    }

    ExprResult Res = getDerived().TransformExpr(RS->getRetValue());
    Expr *E = Res.get();

    RS->setRetValue(E);
    return RS;
  }

  StmtResult TransformSafeStmt(SafeStmt *SS) {
    Stmt *Sub = SS->getSubStmt();
    if (replacedNodesMap.Contains(Sub)) {
      Sub = replacedNodesMap.Get(Sub);
    }
    StmtResult ResSub = getDerived().TransformStmt(Sub);
    SS->setSubStmt(ResSub.get());
    return SS;
  }

  StmtResult TransformSwitchStmt(SwitchStmt *SS) {
    SS->setCond(getDerived().TransformExpr(SS->getCond()).get());
    Stmt *Body = SS->getBody();
    if (replacedNodesMap.Contains(Body)) {
      Body = replacedNodesMap.Get(Body);
    }
    StmtResult ResBody = getDerived().TransformStmt(Body);
    SS->setBody(ResBody.get());
    return SS;
  }

  StmtResult TransformWhileStmt(WhileStmt *WS) {
    Expr *Cond = WS->getCond();
    if (replacedNodesMap.Contains(Cond)) {
      Cond = replacedNodesMap.Get(Cond);
    }
    ExprResult ResCond = getDerived().TransformExpr(Cond);
    WS->setCond(ResCond.get());

    Stmt *Body = WS->getBody();
    if (replacedNodesMap.Contains(Body)) {
      Body = replacedNodesMap.Get(Body);
    }
    StmtResult Res = getDerived().TransformStmt(Body);
    WS->setBody(Res.get());

    return WS;
  }

  ExprResult TransformArraySubscriptExpr(ArraySubscriptExpr *ASE) {
    Expr *LHS = ASE->getLHS();
    if (replacedNodesMap.Contains(LHS)) {
      LHS = replacedNodesMap.Get(LHS);
    }
    ExprResult ResLHS = getDerived().TransformExpr(LHS);
    ASE->setLHS(ResLHS.get());

    Expr *RHS = ASE->getRHS();
    if (replacedNodesMap.Contains(RHS)) {
      RHS = replacedNodesMap.Get(RHS);
    }
    ExprResult ResRHS = getDerived().TransformExpr(RHS);
    ASE->setRHS(ResRHS.get());

    return ASE;
  }

  ExprResult TransformAwaitExpr(AwaitExpr *AE) {
    return AE;
  }

  ExprResult TransformBinaryOperator(BinaryOperator *BO) {
    Expr *LHS = BO->getLHS();
    if (replacedNodesMap.Contains(LHS)) {
      LHS = replacedNodesMap.Get(LHS);
    }
    ExprResult ResLHS = getDerived().TransformExpr(LHS);
    BO->setLHS(ResLHS.get());

    Expr *RHS = BO->getRHS();
    if (replacedNodesMap.Contains(RHS)) {
      RHS = replacedNodesMap.Get(RHS);
    }
    ExprResult ResRHS = getDerived().TransformExpr(RHS);
    BO->setRHS(ResRHS.get());

    return BO;
  }

  ExprResult TransformCallExpr(CallExpr *CE) {
    Expr *Callee = CE->getCallee();
    if (replacedNodesMap.Contains(Callee)) {
      Callee = replacedNodesMap.Get(Callee);
    }
    ExprResult ResCallee = getDerived().TransformExpr(Callee);
    CE->setCallee(ResCallee.get());

    for (unsigned i = 0; i < CE->getNumArgs(); ++i) {
      Expr *Arg = CE->getArg(i);
      if (replacedNodesMap.Contains(Arg)) {
        Arg = replacedNodesMap.Get(Arg);
      }
      ExprResult Res = getDerived().TransformExpr(Arg);
      Expr *E = Res.get();

      CE->setArg(i, E);
    }
    return CE;
  }

  ExprResult TransformAtomicExpr(AtomicExpr *AE) {
    for (unsigned I = 0; I < AE->getNumSubExprs(); ++I) {
      Expr *Sub = AE->getSubExprs()[I];
      if (replacedNodesMap.Contains(Sub))
        Sub = replacedNodesMap.Get(Sub);
      ExprResult Res = getDerived().TransformExpr(Sub);
      AE->getSubExprs()[I] = Res.get();
    }
    return AE;
  }

  ExprResult TransformVAArgExpr(VAArgExpr *VAE) {
    Expr *Sub = VAE->getSubExpr();
    if (replacedNodesMap.Contains(Sub))
      Sub = replacedNodesMap.Get(Sub);
    ExprResult Res = getDerived().TransformExpr(Sub);
    VAE->setSubExpr(Res.get());
    return VAE;
  }

  ExprResult TransformCompoundLiteralExpr(CompoundLiteralExpr *CLE) {
    Expr *Initializer = CLE->getInitializer();
    if (replacedNodesMap.Contains(Initializer)) {
      Initializer = replacedNodesMap.Get(Initializer);
    }
    ExprResult Res = getDerived().TransformExpr(Initializer);
    CLE->setInitializer(Res.get());

    return CLE;
  }

  ExprResult TransformCStyleCastExpr(CStyleCastExpr *CSCE) {
    Expr *Sub = CSCE->getSubExpr();
    if (replacedNodesMap.Contains(Sub)) {
      Sub = replacedNodesMap.Get(Sub);
    }
    ExprResult Res = getDerived().TransformExpr(Sub);
    CSCE->setSubExpr(Res.get());

    return CSCE;
  }

  ExprResult TransformDeclRefExpr(DeclRefExpr *DRE) {
    if (replacedNodesMap.Contains(DRE)) {
      Expr *E = replacedNodesMap.Get(DRE);
      ExprResult Res = getDerived().TransformExpr(E);
      return Res.get();
    }
    return DRE;
  }

  ExprResult TransformImplicitCastExpr(ImplicitCastExpr *ICE) {
    Expr *Sub = ICE->getSubExpr();
    if (replacedNodesMap.Contains(Sub)) {
      Sub = replacedNodesMap.Get(Sub);
    }
    ExprResult Res = getDerived().TransformExpr(Sub);
    ICE->setSubExpr(Res.get());
    if (replacedNodesMap.Contains(ICE)) {
      Expr *E = replacedNodesMap.Get(ICE);
      ExprResult ResE = getDerived().TransformExpr(E);
      return ResE.get();
    }

    return ICE;
  }

  ExprResult TransformInitListExpr(InitListExpr *ILE) {
    for (unsigned i = 0; i < ILE->getNumInits(); ++i) {
      Expr *Init = ILE->getInit(i);
      if (replacedNodesMap.Contains(Init)) {
        Init = replacedNodesMap.Get(Init);
      }
      ExprResult Res = getDerived().TransformExpr(Init);
      ILE->setInit(i, Res.get());
    }

    return ILE;
  }

  ExprResult TransformMemberExpr(MemberExpr *ME) {
    Expr *Base = ME->getBase();
    if (replacedNodesMap.Contains(Base)) {
      Base = replacedNodesMap.Get(Base);
    }
    ExprResult Res = getDerived().TransformExpr(Base);
    ME->setBase(Res.get());

    return ME;
  }

  ExprResult TransformParenExpr(ParenExpr *PE) {
    Expr *Sub = PE->getSubExpr();
    if (replacedNodesMap.Contains(Sub)) {
      Sub = replacedNodesMap.Get(Sub);
    }
    ExprResult Res = getDerived().TransformExpr(Sub);
    PE->setSubExpr(Res.get());

    return PE;
  }

  ExprResult TransformSafeExpr(SafeExpr *SE) {
    Expr *Sub = SE->getSubExpr();
    if (replacedNodesMap.Contains(Sub)) {
      Sub = replacedNodesMap.Get(Sub);
    }
    ExprResult Res = getDerived().TransformExpr(Sub);
    SE->setSubExpr(Res.get());

    return SE;
  }

  ExprResult TransformStmtExpr(StmtExpr *SE) {
    StmtResult Res = getDerived().TransformStmt(SE->getSubStmt());
    SE->setSubStmt(Res.getAs<CompoundStmt>());
    return SE;
  }

  ExprResult TransformUnaryOperator(UnaryOperator *UO) {
    Expr *Sub = UO->getSubExpr();
    if (replacedNodesMap.Contains(Sub)) {
      Sub = replacedNodesMap.Get(Sub);
    }
    ExprResult Res = getDerived().TransformExpr(Sub);
    UO->setSubExpr(Res.get());

    return UO;
  }
};

void Sema::BSCBorrowChecker(FunctionDecl *FD) {
  ReplaceNodesMap replacedNodesMap;
  BorrowCheckerPrologue BCP(*this, FD, replacedNodesMap);
  BCP.applyTransform();

  AnalysisDeclContext AC(/* AnalysisDeclContextManager */ nullptr, FD);
  AC.getCFGBuildOptions().PruneTriviallyFalseEdges = true;
  AC.getCFGBuildOptions().AddLifetime = true;
  AC.getCFGBuildOptions().BSCMode = true;
  AC.getCFGBuildOptions().BSCBorrowCk = true;

  if (AC.getCFG()) {
#if DEBUG_PRINT
    AC.getCFG()->dump(LangOpts, true);
#endif
    borrow::BorrowDiagReporter Reporter(*this);
    borrow::runBorrowChecker(*FD, *AC.getCFG(), Context, Reporter);
  }

  BorrowCheckerEpilogue BCE(*this, FD, replacedNodesMap);
  BCE.applyTransform();
}

#endif
