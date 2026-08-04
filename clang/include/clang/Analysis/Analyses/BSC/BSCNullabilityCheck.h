//===- BSCNullabilityCheck.h - Nullability Check for Source CFGs -*- BSC ---*//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements BSC Pointer Nullability Check for source-level CFGs.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_BSCNULLABILITYCHECK_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_BSCNULLABILITYCHECK_H

#if ENABLE_BSC

#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/AST/BSC/TypeBSC.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Sema/Sema.h"

namespace clang {
bool FindNonnull(QualType QT, const ASTContext &Ctx);
Expr *NormalizeInitExpr(Expr *E);

enum NullabilityCheckDiagKind {
  NonnullAssignedByNullable,
  PassNullableArgument,
  ReturnNullable,
  NullableCastNonnull,
  NullablePointerDereference,
  NullablePointerAccessMember,
  NonnullInitByDefault,
  NestedNullabilityMismatch,
  NullabilityMaxDiagKind
};

const unsigned NullabilityDiagIdList[] = {
    diag::err_nonnull_assigned_by_nullable,
    diag::err_pass_nullable_argument,
    diag::err_return_nullable,
    diag::err_nullable_cast_nonnull,
    diag::err_nullable_pointer_dereference,
    diag::err_nullable_pointer_access_member,
    diag::err_nonnull_init_by_default,
    diag::err_nested_nullability_mismatch};

struct NullabilityCheckDiagInfo {
  SourceLocation Loc;
  NullabilityCheckDiagKind Kind;
  std::string Name;
  QualType Types[4]; // for NestedNullabilityMismatch: OrigRHS, OrigLHS, InnerRHS, InnerLHS
  NullabilityCheckDiagInfo(SourceLocation Loc, NullabilityCheckDiagKind Kind)
      : Loc(Loc), Kind(Kind) {}
  NullabilityCheckDiagInfo(SourceLocation Loc, NullabilityCheckDiagKind Kind,
                           std::string Name)
      : Loc(Loc), Kind(Kind), Name(Name) {}
  NullabilityCheckDiagInfo(SourceLocation Loc, NullabilityCheckDiagKind Kind,
                           QualType OrigRHS, QualType OrigLHS,
                           QualType InnerRHS, QualType InnerLHS)
      : Loc(Loc), Kind(Kind) {
    Types[0] = OrigRHS; Types[1] = OrigLHS;
    Types[2] = InnerRHS; Types[3] = InnerLHS;
  }

  bool operator==(const NullabilityCheckDiagInfo &other) const {
    return Loc == other.Loc && Kind == other.Kind && Name == other.Name;
  }
};

class NullabilityCheckDiagReporter {
  Sema &S;
  std::vector<NullabilityCheckDiagInfo> DIV;

public:
  NullabilityCheckDiagReporter(Sema &S) : S(S) {}

  void addDiags(llvm::SmallVector<NullabilityCheckDiagInfo, 3> &diags) {
    for (auto it = diags.begin(), ei = diags.end(); it != ei; ++it) {
      addDiagInfo(*it);
    }
  }

  void addDiagInfo(NullabilityCheckDiagInfo &DI) {
    for (auto it = DIV.begin(), ei = DIV.end(); it != ei; ++it) {
      if (DI == *it)
        return;
    }
    if (S.getDiagnostics().getDiagnosticLevel(getNullabilityDiagID(DI.Kind),
                                              DI.Loc) ==
        DiagnosticsEngine::Ignored) {
      return;
    }
    DIV.push_back(DI);
  }

  unsigned getNullabilityDiagID(NullabilityCheckDiagKind Kind) {
    unsigned index = static_cast<unsigned>(Kind);
    assert(index < static_cast<unsigned>(
                       NullabilityCheckDiagKind::NullabilityMaxDiagKind) &&
           "Unknown error type");
    return NullabilityDiagIdList[index];
  }

  void flushDiagnostics() {
    // Sort the diag info by SourceLocation. While not strictly
    // guaranteed to produce them in line/column order, this will provide
    // a stable ordering.
    std::sort(DIV.begin(), DIV.end(),
              [this](const NullabilityCheckDiagInfo &a,
                     const NullabilityCheckDiagInfo &b) {
                return S.getSourceManager().isBeforeInTranslationUnit(a.Loc,
                                                                      b.Loc);
              });

    for (const NullabilityCheckDiagInfo &DI : DIV) {
      switch (DI.Kind) {
      case NonnullAssignedByNullable:
      case PassNullableArgument:
      case ReturnNullable:
      case NullableCastNonnull:
      case NullablePointerDereference:
      case NullablePointerAccessMember:
        S.Diag(DI.Loc, getNullabilityDiagID(DI.Kind));
        break;
      case NestedNullabilityMismatch:
        S.Diag(DI.Loc, getNullabilityDiagID(DI.Kind))
            << DI.Types[0] << DI.Types[1] << DI.Types[2] << DI.Types[3];
        break;
      case NonnullInitByDefault:
        S.Diag(DI.Loc, getNullabilityDiagID(DI.Kind)) << DI.Name;
        S.Diag(DI.Loc, diag::note_nonnull_init_reason) << DI.Name;
        break;
      default:
        llvm_unreachable("Unknown error type");
        break;
      }

      S.getDiagnostics().increaseNullabilityCheckErrors();
    }
  }

  unsigned getNumErrors() { return DIV.size(); }
};

void runNullabilityCheck(const FunctionDecl &fd, const CFG &cfg,
                         AnalysisDeclContext &ac,
                         NullabilityCheckDiagReporter &reporter,
                         ASTContext &ctx);

} // end namespace clang

#endif // ENABLE_BSC

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_BSCNULLABILITYCHECK_H
