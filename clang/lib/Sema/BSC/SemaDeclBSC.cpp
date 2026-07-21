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
#include "llvm/Support/SaveAndRestore.h"
#include <set>
#include <tuple>

using namespace clang;
using namespace sema;

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
  if (getDefNullability(LHSType, Ctx) != getDefNullability(RHSType, Ctx))
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
    QualType LHSParType = LSHFuncType->getParamType(i).getUnqualifiedType();
    QualType RHSParType = RSHFuncType->getParamType(i).getUnqualifiedType();
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
          DiagId = diag::err_return_uninit;
          break;
        case bscir::InitDiagKind::ReturnMaybeUninit:
          DiagId = diag::err_return_possibly_uninit;
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
  unsigned TempVarCounter = 0;
  ReplaceNodesMap &replacedNodesMap;

  VarDecl *NewTempVar(QualType T, Expr *E = nullptr) {
    std::string Name = "_borrowck_tmp_" + std::to_string(TempVarCounter++);
    VarDecl *VD = VarDecl::Create(
        getSema().Context, FD, SourceLocation(), SourceLocation(),
        &getSema().Context.Idents.get(Name), T, nullptr, SC_None);
    VD->setInit(E);
    DeclStmt *DS = new (getSema().Context)
        DeclStmt(DeclGroupRef(VD), SourceLocation(), SourceLocation());
    Stmts.push_back(DS);
    return VD;
  }

  // Replace the given expression with a new temporary variable, and return the
  // corresponding DeclRefExpr.
  Expr *ReplaceWithRefToNewTempVar(Expr *E, QualType T = QualType{}) {
    if (T.isNull()) {
      T = E->getType();
    }
    VarDecl *VD = NewTempVar(T, E);
    DeclRefExpr *DRE = DeclRefExpr::Create(
        getSema().Context, NestedNameSpecifierLoc(), SourceLocation(), VD,
        false, E->getBeginLoc(), T, VK_LValue);
    if (E->isPRValue())
        return getSema().DefaultLvalueConversion(DRE).get();
    return DRE;
  }

  // Ensure the given statement is wrapped with a CompoundStmt. If not, create
  // a CompoundStmt to wrap it.
  CompoundStmt *EnsureWrappedWithCompoundStmt(Stmt *S) {
    if (isa<CompoundStmt>(S))
      return cast<CompoundStmt>(S);
    return CompoundStmt::Create(SemaRef.Context, S, FPOptionsOverride(),
                                S->getBeginLoc(), S->getEndLoc());
  }

  ExprResult TransformStringLiteralLike(Expr *E) {
    QualType PtrTy = getSema().Context.getArrayDecayedType(E->getType());
    Expr *DRE = ReplaceWithRefToNewTempVar(E, PtrTy);
    replacedNodesMap.Insert(DRE, E);
    return DRE;
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
      { return getDerived().TransformExpr(cast<Expr>(S)).get(); }
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
    // Traverse and transform all statements in the compound statement.
    for (Stmt *S : CS->body()) {
      StmtResult Res = getDerived().TransformStmt(S);
      Stmts.push_back(Res.getAs<Stmt>());
    }
    CompoundStmt *NewCS = CompoundStmt::Create(
        SemaRef.Context, Stmts, FPOptionsOverride(), CS->getLBracLoc(),
        CS->getRBracLoc(), CS->getCompSafeZoneSpecifier());
    replacedNodesMap.Insert(NewCS, CS);

    return NewCS;
  }

  StmtResult TransformDeclStmt(DeclStmt *DS) {
    for (Decl *D : DS->decls()) {
      if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
        if (VD->hasInit()) {
          ExprResult Res = getDerived().TransformExpr(VD->getInit());
          VD->setInit(Res.get());
        }
      }
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
    CompoundStmt *CS = CompoundStmt::Create(
        SemaRef.Context, Cond, FPOptionsOverride(), Cond->getBeginLoc(),
        Cond->getEndLoc(), SafeZoneSpecifier::SZ_None);
    StmtResult ResCS = getDerived().TransformStmt(CS);
    StmtExpr *SE = new (SemaRef.Context)
        StmtExpr(cast<CompoundStmt>(ResCS.get()), Cond->getType(),
                 Cond->getBeginLoc(), Cond->getEndLoc(), 0);
    DS->setCond(SE);
    replacedNodesMap.Insert(SE, Cond);

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
      CompoundStmt *CS = CompoundStmt::Create(
          SemaRef.Context, Cond, FPOptionsOverride(), Cond->getBeginLoc(),
          Cond->getEndLoc(), SafeZoneSpecifier::SZ_None);
      StmtResult ResCS = getDerived().TransformStmt(CS);
      StmtExpr *SE = new (SemaRef.Context)
          StmtExpr(cast<CompoundStmt>(ResCS.get()), Cond->getType(),
                   Cond->getBeginLoc(), Cond->getEndLoc(), 0);
      FS->setCond(SE);
      replacedNodesMap.Insert(SE, Cond);
    }

    if (Expr *Inc = FS->getInc()) {
      CompoundStmt *CS = CompoundStmt::Create(
          SemaRef.Context, Inc, FPOptionsOverride(), Inc->getBeginLoc(),
          Inc->getEndLoc(), SafeZoneSpecifier::SZ_None);
      StmtResult ResCS = getDerived().TransformStmt(CS);
      StmtExpr *SE = new (SemaRef.Context)
          StmtExpr(cast<CompoundStmt>(ResCS.get()), Inc->getType(),
                   Inc->getBeginLoc(), Inc->getEndLoc(), 0);
      FS->setInc(SE);
      replacedNodesMap.Insert(SE, Inc);
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
    ExprResult ResCond = getDerived().TransformExpr(Cond);
    IS->setCond(ResCond.get());
    replacedNodesMap.Insert(ResCond.get(), Cond);

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
    if (Expr *RV = RS->getRetValue()) {
      ExprResult Res = getDerived().TransformExpr(RV);
      Expr *E = Res.get();
      RS->setRetValue(E);
    }

    return RS;
  }

  StmtResult TransformSafeStmt(SafeStmt *SS) {
    StmtResult Res = getDerived().TransformStmt(SS->getSubStmt());
    SS->setSubStmt(Res.get());

    return SS;
  }

  StmtResult TransformSwitchStmt(SwitchStmt *SS) {
    Expr *Cond = SS->getCond();
    ExprResult ResCond = getDerived().TransformExpr(Cond);
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
    CompoundStmt *CS = CompoundStmt::Create(
        SemaRef.Context, Cond, FPOptionsOverride(), Cond->getBeginLoc(),
        Cond->getEndLoc(), SafeZoneSpecifier::SZ_None);
    StmtResult ResCS = getDerived().TransformStmt(CS);
    StmtExpr *SE = new (SemaRef.Context)
        StmtExpr(cast<CompoundStmt>(ResCS.get()), Cond->getType(),
                 Cond->getBeginLoc(), Cond->getEndLoc(), 0);
    WS->setCond(SE);
    replacedNodesMap.Insert(SE, Cond);

    Stmt *Body = WS->getBody();
    CompoundStmt *CSBody = EnsureWrappedWithCompoundStmt(Body);
    StmtResult ResBody = getDerived().TransformStmt(CSBody);
    WS->setBody(ResBody.get());
    replacedNodesMap.Insert(ResBody.get(), Body);

    return WS;
  }

  ExprResult TransformArraySubscriptExpr(ArraySubscriptExpr *ASE) {
    ExprResult ResLHS = getDerived().TransformExpr(ASE->getLHS());
    ASE->setLHS(ResLHS.get());

    ExprResult ResRHS = getDerived().TransformExpr(ASE->getRHS());
    ASE->setRHS(ResRHS.get());

    return ASE;
  }

  ExprResult TransformAwaitExpr(AwaitExpr *AE) {
    return AE;
  }

  // Note: don't replace LHS and RHS with temporary variables directly in this
  // function, because it may cause incorrect transformation results.
  ExprResult TransformBinaryOperator(BinaryOperator *BO) {
    ExprResult ResLHS = getDerived().TransformExpr(BO->getLHS());
    Expr *ELHS = ResLHS.get();
    BO->setLHS(ELHS);

    ExprResult ResRHS = getDerived().TransformExpr(BO->getRHS());
    Expr *ERHS = ResRHS.get();
    BO->setRHS(ERHS);

    // For logical operators (&&, ||), don't wrap in a temporary variable.
    // The CFG builder handles them via VisitLogicalOperator which creates
    // short-circuit blocks. Wrapping them would destroy the short-circuit
    // semantics and cause incorrect borrow-checker lifetimes.
    if (BO->isLogicalOp())
      return BO;

    Expr *DRE = ReplaceWithRefToNewTempVar(BO);
    replacedNodesMap.Insert(DRE, BO);
    return DRE;
  }

  ExprResult TransformCallExpr(CallExpr *CE) {
    for (unsigned i = 0; i < CE->getNumArgs(); ++i) {
      ExprResult Res = getDerived().TransformExpr(CE->getArg(i));
      Expr *E = Res.get();

      Expr *DRE = ReplaceWithRefToNewTempVar(E);
      CE->setArg(i, DRE);
      replacedNodesMap.Insert(DRE, E);
    }

    // If the call expression has a non-void return type, replace it with a
    // temporary variable. Otherwise, return the original call expression.
    if (!CE->getCallReturnType(SemaRef.Context)->isVoidType()) {
      Expr *DRE = ReplaceWithRefToNewTempVar(CE);
      replacedNodesMap.Insert(DRE, CE);
      return DRE;
    }
    return CE;
  }

  ExprResult TransformCompoundLiteralExpr(CompoundLiteralExpr *CLE) {
    ExprResult Res = getDerived().TransformExpr(CLE->getInitializer());
    Expr *E = Res.get();
    CLE->setInitializer(E);

    Expr *DRE = ReplaceWithRefToNewTempVar(CLE);
    replacedNodesMap.Insert(DRE, CLE);
    return DRE;
  }

  // Transform ConditionalOperator into a IfStmt and a VarDecl. The result of
  // the ConditionalOperator is the value of the VarDecl.
  ExprResult TransformConditionalOperator(ConditionalOperator *CO) {
    Expr *Cond = CO->getCond();
    Expr *TrueExpr = CO->getTrueExpr();
    Expr *FalseExpr = CO->getFalseExpr();

    // Create a new temporary variable.
    VarDecl *TempVD = NewTempVar(CO->getType());

    // Build true branch of the IfStmt.
    DeclRefExpr *TempRef1 = DeclRefExpr::Create(
        getSema().Context, NestedNameSpecifierLoc(), SourceLocation(), TempVD,
        false, CO->getBeginLoc(), CO->getType(), VK_LValue);
    BinaryOperator *TrueAssign = BinaryOperator::Create(
        getSema().Context, TempRef1, TrueExpr, BO_Assign, CO->getType(),
        VK_PRValue, OK_Ordinary, SourceLocation(), FPOptionsOverride());

    // Build false branch of the IfStmt.
    DeclRefExpr *TempRef2 = DeclRefExpr::Create(
        getSema().Context, NestedNameSpecifierLoc(), SourceLocation(), TempVD,
        false, CO->getBeginLoc(), CO->getType(), VK_LValue);
    BinaryOperator *FalseAssign = BinaryOperator::Create(
        getSema().Context, TempRef2, FalseExpr, BO_Assign, CO->getType(),
        VK_PRValue, OK_Ordinary, SourceLocation(), FPOptionsOverride());

    // Build the IfStmt.
    IfStmt *IS = IfStmt::Create(getSema().Context, SourceLocation(),
                                IfStatementKind::Ordinary, nullptr, nullptr,
                                Cond, SourceLocation(), SourceLocation(),
                                TrueAssign, SourceLocation(), FalseAssign);

    StmtResult Res = getDerived().TransformStmt(IS);
    Stmts.push_back(Res.get());

    DeclRefExpr *DRE = DeclRefExpr::Create(
        getSema().Context, NestedNameSpecifierLoc(), SourceLocation(), TempVD,
        false, CO->getBeginLoc(), CO->getType(), VK_LValue);
    replacedNodesMap.Insert(DRE, CO);
    return DRE;
  }

  ExprResult TransformCStyleCastExpr(CStyleCastExpr *CSCE) {
    ExprResult Res = getDerived().TransformExpr(CSCE->getSubExpr());
    CSCE->setSubExpr(Res.get());

    Expr *DRE = ReplaceWithRefToNewTempVar(CSCE);
    replacedNodesMap.Insert(DRE, CSCE);
    return DRE;
  }

  ExprResult TransformDeclRefExpr(DeclRefExpr *DRE) { return DRE; }

  ExprResult TransformImplicitCastExpr(ImplicitCastExpr *ICE) {
    ExprResult Res = getDerived().TransformExpr(ICE->getSubExpr());
    ICE->setSubExpr(Res.get());

    return ICE;
  }

  ExprResult TransformInitListExpr(InitListExpr *ILE) {
    for (unsigned i = 0; i < ILE->getNumInits(); ++i) {
      ExprResult Res = getDerived().TransformExpr(ILE->getInit(i));
      Expr *E = Res.get();

      Expr *DRE = ReplaceWithRefToNewTempVar(E);
      ILE->setInit(i, DRE);
      replacedNodesMap.Insert(DRE, E);
    }

    Expr *DRE = ReplaceWithRefToNewTempVar(ILE);
    replacedNodesMap.Insert(DRE, ILE);
    return ILE;
  }

  ExprResult TransformMemberExpr(MemberExpr *ME) {
    ExprResult Res = getDerived().TransformExpr(ME->getBase());
    ME->setBase(Res.get());

    return ME;
  }

  ExprResult TransformParenExpr(ParenExpr *PE) {
    ExprResult Res = getDerived().TransformExpr(PE->getSubExpr());
    PE->setSubExpr(Res.get());

    return PE;
  }

  ExprResult TransformPredefinedExpr(PredefinedExpr *PE) {
    return TransformStringLiteralLike(PE);
  }

  ExprResult TransformSafeExpr(SafeExpr *SE) {
    ExprResult Res = getDerived().TransformExpr(SE->getSubExpr());
    SE->setSubExpr(Res.get());

    return SE;
  }

  ExprResult TransformStmtExpr(StmtExpr *SE) {
    StmtResult Res = getDerived().TransformStmt(SE->getSubStmt());
    SE->setSubStmt(Res.getAs<CompoundStmt>());
    return SE;
  }

  ExprResult TransformUnaryOperator(UnaryOperator *UO) {
    ExprResult Res = getDerived().TransformExpr(UO->getSubExpr());
    UO->setSubExpr(Res.get());

    // Special handling for post-increment and post-decrement operators because
    // they are l-values.
    if (!UO->isLValue() ||
        (UO->getOpcode() >= UO_PostInc && UO->getOpcode() <= UO_PreDec)) {
      Expr *DRE = ReplaceWithRefToNewTempVar(UO);
      replacedNodesMap.Insert(DRE, UO);
      return DRE;
    }
    return UO;
  }

  ExprResult TransformStringLiteral(StringLiteral *SL) {
    return TransformStringLiteralLike(SL);
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
  AC.getCFGBuildOptions()
      .setAlwaysAdd(Stmt::BinaryOperatorClass)
      .setAlwaysAdd(Stmt::BreakStmtClass)
      .setAlwaysAdd(Stmt::CompoundStmtClass)
      .setAlwaysAdd(Stmt::DeclStmtClass)
      .setAlwaysAdd(Stmt::DoStmtClass)
      .setAlwaysAdd(Stmt::ForStmtClass)
      .setAlwaysAdd(Stmt::IfStmtClass)
      .setAlwaysAdd(Stmt::ReturnStmtClass)
      .setAlwaysAdd(Stmt::SwitchStmtClass)
      .setAlwaysAdd(Stmt::WhileStmtClass);

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