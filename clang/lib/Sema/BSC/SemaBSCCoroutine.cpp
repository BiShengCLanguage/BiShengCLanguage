//===--- SemaBSCCoroutine.cpp - Semantic Analysis for BSC Coroutines
//----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements semantic analysis for BSC Coroutines.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include <algorithm>
#include <cstring>
#include <functional>

#include "TreeTransform.h"
#include "TypeLocBuilder.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/EvaluatedExprVisitor.h"
#include "clang/AST/Expr.h"
#include "clang/AST/NonTrivialTypeVisitor.h"
#include "clang/AST/Randstruct.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Sema/DeclSpec.h"
#include "clang/Sema/Initialization.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/ScopeInfo.h"
#include "clang/Sema/SemaInternal.h"
#include "clang/Sema/Template.h"
#include "llvm/ADT/SmallString.h"

using namespace clang;
using namespace sema;

static RecordDecl *buildAsyncDataRecord(ASTContext &C, StringRef Name,
                                        SourceLocation StartLoc,
                                        SourceLocation EndLoc,
                                        RecordDecl::TagKind TK) {
  RecordDecl *NewDecl = RecordDecl::Create(
      C, TK, C.getTranslationUnitDecl(), StartLoc, EndLoc, &C.Idents.get(Name));
  return NewDecl;
}

static void addAsyncRecordDecl(ASTContext &C, StringRef Name, QualType Type,
                               SourceLocation StartLoc, SourceLocation EndLoc,
                               RecordDecl *RD) {
  FieldDecl *Field = FieldDecl::Create(
      C, RD, StartLoc, EndLoc, &C.Idents.get(Name), Type,
      C.getTrivialTypeSourceInfo(Type, SourceLocation()),
      /*BW=*/nullptr, /*Mutable=*/false, /*InitStyle=*/ICIS_NoInit);
  Field->setAccess(AS_public);
  RD->addDecl(Field);
}

static FunctionDecl *buildAsyncFuncDecl(ASTContext &C, DeclContext *DC,
                                        SourceLocation StartLoc,
                                        SourceLocation NLoc, DeclarationName N,
                                        QualType T, TypeSourceInfo *TInfo) {
  FunctionDecl *NewDecl =
      FunctionDecl::Create(C, DC, StartLoc, NLoc, N, T, TInfo, SC_None);
  return NewDecl;
}

static BSCMethodDecl *
buildAsyncBSCMethodDecl(ASTContext &C, DeclContext *DC, SourceLocation StartLoc,
                        SourceLocation NLoc, SourceLocation EndLoc,
                        DeclarationName N, QualType T, TypeSourceInfo *TInfo,
                        StorageClass SC, QualType ET) {
  // TODO: inline should be passed.
  BSCMethodDecl *NewDecl = BSCMethodDecl::Create(
      C, DC, StartLoc, DeclarationNameInfo(N, NLoc), T, TInfo, SC, false, false,
      ConstexprSpecKind::Unspecified, EndLoc);
  if (auto RD = dyn_cast_or_null<RecordDecl>(DC)) {
    C.BSCDeclContextMap[RD->getTypeForDecl()] = DC;
    NewDecl->setHasThisParam(true); // bug
    NewDecl->setExtendedType(ET);
  }
  return NewDecl;
}

std::string GetPrefix(QualType T) {
  std::string ExtendedTypeStr = T.getCanonicalType().getAsString();
  for (int i = ExtendedTypeStr.length() - 1; i >= 0; i--) {
    if (ExtendedTypeStr[i] == ' ') {
      ExtendedTypeStr.replace(i, 1, "_");
    }
  }
  return ExtendedTypeStr;
}

static BSCMethodDecl *lookupBSCMethodInRecord(Sema &S, std::string FuncName,
                                              DeclContext *RecordDecl) {
  LookupResult Result(
      S,
      DeclarationNameInfo(&(S.Context.Idents).get(FuncName), SourceLocation()),
      S.LookupOrdinaryName);
  S.LookupQualifiedName(Result, RecordDecl, false, true);
  BSCMethodDecl *BSCMethod = nullptr;
  if (!Result.empty())
    BSCMethod = dyn_cast_or_null<BSCMethodDecl>(Result.getRepresentativeDecl());
  return BSCMethod;
}

static bool implementedFutureType(Sema &S, QualType Ty) {
  RecordDecl *FutureRD = nullptr;
  if (const auto *RT = Ty->getAs<RecordType>()) {
    FutureRD = RT->getAsRecordDecl();
  }

  if (!FutureRD)
    return false;
  BSCMethodDecl *PollFD = lookupBSCMethodInRecord(S, "poll", FutureRD);
  if (PollFD == nullptr)
    return false;

  BSCMethodDecl *FreeFD = lookupBSCMethodInRecord(S, "free", FutureRD);
  if (FreeFD == nullptr)
    return false;

  return true;
}

// Logic for generating the procedure to free a future object:
// @code
// if (FutureObj != 0) {
//    FreeFuncExpr(FutureObj);
//    FutureObj = (void*)0;
// }
// @endcode
static Stmt *
buildIfStmtForFreeFutureObj(Sema &S, Expr *PtrExpr, Expr *FreeFuncExpr,
                            SourceLocation Loc = SourceLocation()) {
  llvm::APInt ResultVal(S.Context.getTargetInfo().getIntWidth(), 0);
  Expr *IntegerExpr = IntegerLiteral::Create(S.Context, ResultVal,
                                             S.Context.IntTy, SourceLocation());
  // generating condition
  Expr *Comp = S.BuildBinOp(nullptr, SourceLocation(), BO_NE,
                            /*LHSExpr=*/PtrExpr, /*RHSExpr=*/IntegerExpr)
                   .get();
  Sema::ConditionResult IfCond = S.ActOnCondition(
      nullptr, SourceLocation(), Comp, Sema::ConditionKind::Boolean);

  // generating free call
  QualType Ty = S.Context.getPointerType(S.Context.VoidTy);
  Expr *FreeArg = PtrExpr;
  if (PtrExpr->getType() != Ty) {
    FreeArg = CStyleCastExpr::Create(
        S.Context, Ty, VK_PRValue, CK_BitCast, FreeArg, nullptr,
        FPOptionsOverride(),
        S.Context.getTrivialTypeSourceInfo(Ty, SourceLocation()),
        SourceLocation(), SourceLocation());
  }
  std::vector<Expr *> FreeArgs{FreeArg};
  Expr *FreeFuncCall = S.BuildCallExpr(nullptr, FreeFuncExpr, SourceLocation(),
                                       FreeArgs, SourceLocation())
                           .get();
  // generating null assignment
  Expr *RAssignExpr = CStyleCastExpr::Create(
      S.Context, Ty, VK_PRValue, CK_NullToPointer, IntegerExpr, nullptr,
      FPOptionsOverride(),
      S.Context.getTrivialTypeSourceInfo(Ty, SourceLocation()),
      SourceLocation(), SourceLocation());
  if (PtrExpr->getType() != Ty) {
    RAssignExpr = CStyleCastExpr::Create(
        S.Context, PtrExpr->getType(), VK_PRValue, CK_BitCast, RAssignExpr,
        nullptr, FPOptionsOverride(),
        S.Context.getTrivialTypeSourceInfo(PtrExpr->getType(),
                                           SourceLocation()),
        SourceLocation(), SourceLocation());
  }
  Expr *NullptrAssign =
      S.BuildBinOp(nullptr, SourceLocation(), BO_Assign,
                   /*LHSExpr=*/PtrExpr, /*RHSExpr=*/RAssignExpr)
          .get();

  SmallVector<Stmt *, 2> BodyStmts = {FreeFuncCall, NullptrAssign};
  Stmt *Body = CompoundStmt::Create(S.Context, BodyStmts, FPOptionsOverride(),
                                    SourceLocation(), SourceLocation());

  Stmt *If = S.BuildIfStmt(Loc, IfStatementKind::Ordinary,
                           /*LPL=*/Loc, /*Init=*/nullptr, IfCond,
                           /*RPL=*/Loc, Body, Loc, nullptr)
                 .get();
  return If;
}

/**
 * Is a BSC compatible future if and only if:
 * - Implements the Future trait
 * - is of the form *T, and T implements the Future trait
 * - is of the form trait Future<T>*
 */
bool Sema::IsBSCCompatibleFutureType(QualType Ty) {
  return implementedFutureType(*this, Ty) ||
         (isa<PointerType>(Ty.getTypePtr()) &&
          implementedFutureType(
              *this, cast<PointerType>(Ty.getTypePtr())->getPointeeType())) ||
         Ty.getTypePtr()->isBSCFutureType();
}

namespace {
class LocalVarFinder : public StmtVisitor<LocalVarFinder> {
  ASTContext &Context;
  std::vector<std::pair<DeclarationName, QualType>> LocalVarList;
  std::map<std::string, int> IdentifierNumber;

public:
  LocalVarFinder(ASTContext &Context) : Context(Context) {}

  void VisitStmt(Stmt *S) {
    for (auto *C : S->children()) {
      if (C) {
        if (auto DeclS = dyn_cast_or_null<DeclStmt>(C)) {
          for (const auto &DI : DeclS->decls()) {
            if (auto VD = dyn_cast_or_null<VarDecl>(DI)) {
              if (VD->isExternallyVisible() || VD->isConstexpr() ||
                  VD->isStaticLocal())
                continue;

              QualType QT = VD->getType();
              if (QT->hasTraitType())
                continue;

              Expr *Init = VD->getInit();
              // Do not need to transform constant variable with compile-time
              // constant initializier.
              const Expr *Culprit;
              if (QT.isConstQualified() && Init && !Init->isValueDependent() &&
                  Init->isConstantInitializer(Context, false, &Culprit))
                continue;

              std::string VDName = VD->getName().str();
              // May have several local variables which has same name, eg.
              // @code
              // async void f() {
              //    int c = 1;
              //    {
              //       int c = 2;
              //  }
              //   }
              // @endcode
              std::map<std::string, int>::iterator I =
                  IdentifierNumber.find(VDName);
              int VDNameNum = I != IdentifierNumber.end() ? I->second : 0;
              IdentifierNumber[VDName] = VDNameNum + 1;
              if (VDNameNum > 0) {
                VDName = VDName + "_" + std::to_string(VDNameNum);
                VD->setDeclName(&(Context.Idents).get(VDName));
              }

              LocalVarList.push_back(std::make_pair<DeclarationName, QualType>(
                  VD->getDeclName(), VD->getType()));
            }
          }
        }
        Visit(C);
      }
    }
  }
  std::vector<std::pair<DeclarationName, QualType>> GetLocalVarList() const {
    return LocalVarList;
  }
};

class AwaitExprFinder : public StmtVisitor<AwaitExprFinder> {
  int AwaitCount = 0;
  std::vector<Expr *> Args;

public:
  AwaitExprFinder() {}

  void VisitAwaitExpr(AwaitExpr *E) {
    Visit(E->getSubExpr());
    Args.push_back(E);
    AwaitCount++;
  }

  void VisitStmt(Stmt *S) {
    for (auto *C : S->children()) {
      if (C) {
        Visit(C);
      }
    }
  }

  int GetAwaitExprNum() const { return AwaitCount; }

  std::vector<Expr *> GetAwaitExpr() const { return Args; }
};

static bool hasAwaitExpr(Stmt *S) {
  if (S == nullptr) return false;

  if (isa<AwaitExpr>(S)) return true;

  for (auto *C : S->children()) {
    if (hasAwaitExpr(C)) {
      return true;
    }
  }

  return false;
}

/// Look for Illegal AwaitExpr
class IllegalAEFinder : public StmtVisitor<IllegalAEFinder> {
  Sema &SemaRef;
  bool IsIllegal;

public:
  IllegalAEFinder(Sema &SemaRef) : SemaRef(SemaRef) { IsIllegal = false; }

  bool CheckCondHasAwaitExpr(Stmt *S) {
    Expr *condE = nullptr;
    bool CHasIllegalAwait = false;
    std::string ArgString;

    switch (S->getStmtClass()) {
      case Stmt::IfStmtClass: {
        ArgString = "condition statement of if statement";
        condE = cast<IfStmt>(S)->getCond();
        CHasIllegalAwait = hasAwaitExpr(cast<Stmt>(condE));
        break;
      }

      case Stmt::WhileStmtClass: {
        ArgString = "condition statement of while loop";
        condE = cast<WhileStmt>(S)->getCond();
        CHasIllegalAwait = hasAwaitExpr(cast<Stmt>(condE));
        break;
      }

      case Stmt::DoStmtClass: {
        ArgString = "condition statement of do/while loop";
        condE = cast<DoStmt>(S)->getCond();
        CHasIllegalAwait = hasAwaitExpr(cast<Stmt>(condE));
        break;
      }

      case Stmt::ForStmtClass: {
        ArgString = "condition statement of for loop";
        Stmt *Init = cast<ForStmt>(S)->getInit();
        if (Init != nullptr) CHasIllegalAwait = hasAwaitExpr(Init);

        if (!CHasIllegalAwait) {
          condE = cast<ForStmt>(S)->getCond();
          CHasIllegalAwait = hasAwaitExpr(cast<Stmt>(condE));
        }

        break;
      }

      case Stmt::SwitchStmtClass: {
        ArgString = "condition statement of switch statement";
        condE = cast<SwitchStmt>(S)->getCond();
        CHasIllegalAwait = hasAwaitExpr(cast<Stmt>(condE));
        break;
      }

      default:
        break;
    }

    if (CHasIllegalAwait) {
      SemaRef.Diag(S->getBeginLoc(), diag::err_await_invalid_scope)
          << ArgString;
    }

    if (CHasIllegalAwait && !IsIllegal)
      IsIllegal = CHasIllegalAwait;

    return CHasIllegalAwait;
  }

  bool CheckAsyncFuncIllegalStmt(Stmt *S) {
    bool IsContinue = true;
    std::string ArgString;

    if (isa<BinaryOperator>(S)) {
      ArgString = "binary operator expression";
      BinaryOperator *BO = cast<BinaryOperator>(S);
      switch (BO->getOpcode()) {
        case BO_Comma:
        case BO_Assign:
          return false;

        default:
          break;
      }
    } else if (isa<ConditionalOperator>(S)) {
      ArgString = "condition operator expression";
    } else if (isa<CallExpr>(S)) {
      ArgString = "function parameters";
      CallExpr *CE = cast<CallExpr>(S);
      int AwaitNum = 0;
      int CallNum = 0;
      int Count = CE->getNumArgs();
      for (int i = 0; i < Count; i++) {
        CheckAwaitInFuncParams(CE->getArg(i), AwaitNum, CallNum);
      }

      if (AwaitNum >= 2 || (AwaitNum >= 1 && CallNum >= 1)) {
        SemaRef.Diag(S->getBeginLoc(), diag::err_await_invalid_scope)
            << ArgString;

        if (!IsIllegal) IsIllegal = true;
      }
      return IsContinue;
    } else if (DeclStmt *DS = dyn_cast<DeclStmt>(S)) {
      bool HasVar = false;
      for (auto *D : DS->decls()) {
        if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
          if (isa<VariableArrayType>(VD->getType())) {
            HasVar = true;
            break;
          }
        }
      }
      if (HasVar) {
        ArgString = "VariableArrayType";
        SemaRef.Diag(S->getBeginLoc(), diag::err_async_func_unsupported)
            << ArgString;
        if (!IsIllegal)
          IsIllegal = true;
        return IsContinue;
      }
      return false;
    }

    bool CHasIllegalAwait = hasAwaitExpr(S);
    if (CHasIllegalAwait && !IsIllegal)
      IsIllegal = CHasIllegalAwait;

    if (CHasIllegalAwait) {
      SemaRef.Diag(S->getBeginLoc(), diag::err_await_invalid_scope)
          << ArgString;
    }
    return IsContinue;
  }

  void VisitStmt(Stmt *S) {
    for (auto *C : S->children()) {
      if (C) {
        if (isa<IfStmt>(C) || isa<WhileStmt>(C) || isa<ForStmt>(C) ||
            isa<DoStmt>(C) || isa<SwitchStmt>(C)) {
          CheckCondHasAwaitExpr(C);
        } else if (isa<BinaryOperator>(C) || isa<ConditionalOperator>(C) ||
                   isa<CallExpr>(C) || isa<DeclStmt>(C)) {
          bool IsContinue = CheckAsyncFuncIllegalStmt(C);
          if (IsContinue) continue;
        }

        Visit(C);
      }
    }
  }

  void CheckAwaitInFuncParams(Stmt *S, int &AwaitNum, int &CallNum) {
    if (isa<AwaitExpr>(S)) {
      AwaitNum++;
      return;
    }

    if (isa<CallExpr>(S)) {
      CallNum++;
      return;
    }

    for (auto *C : S->children()) {
      if (C) {
        // In function parameters, if the number of await expr is greater than
        // or equal 2, or the function parameters have await expr and call expr,
        // then report error.
        if (AwaitNum >= 2 || (AwaitNum >= 1 && CallNum >= 1)) return;
        Visit(C);
      }
    }
  }

  bool HasIllegalAwaitExpr() { return IsIllegal; }
};

static bool hasReturnStmt(Stmt *S) {
  if (S == nullptr) return false;

  if (isa<ReturnStmt>(S)) return true;

  for (auto *C : S->children()) {
    if (hasReturnStmt(C)) {
      return true;
    }
  }

  return false;
}

static bool isRefactorStmt(Stmt *S) {
  if (isa<CompoundStmt>(S) || isa<IfStmt>(S) || isa<WhileStmt>(S) ||
      isa<ForStmt>(S) || isa<DoStmt>(S) || isa<SwitchStmt>(S))
    return true;

  return false;
}

static QualType lookupGenericType(Sema &S, SourceLocation SLoc, QualType T,
                                  std::string GenericDeclName) {
  LookupResult Previous(S, &S.Context.Idents.get(GenericDeclName),
                        SourceLocation(), S.LookupTagName,
                        S.NotForRedeclaration);
  S.LookupName(Previous, S.TUScope);
  ClassTemplateDecl *CTD = nullptr;
  for (LookupResult::iterator I = Previous.begin(), E = Previous.end(); I != E;
       ++I) {
    if ((CTD = dyn_cast<ClassTemplateDecl>(*I))) {
      break;
    }
  }

  if (!CTD) {
    S.Diag(SLoc, diag::err_function_not_found)
        << GenericDeclName << "\"future.hbs\"";
    return QualType();
  }

  TemplateArgumentListInfo Args(SLoc, SLoc);
  Args.addArgument(TemplateArgumentLoc(
      TemplateArgument(T), S.Context.getTrivialTypeSourceInfo(T, SLoc)));
  QualType PollResultRecord =
      S.CheckTemplateIdType(TemplateName(CTD), SourceLocation(), Args);
  if (PollResultRecord.isNull()) return QualType();
  if (S.RequireCompleteType(SLoc, PollResultRecord,
                            diag::err_coroutine_type_missing_specialization))
    return QualType();
  return PollResultRecord;
}

}  // namespace

// build struct Future declaration for async function
static RecordDecl *buildOpaqueFutureRecordDecl(Sema &S, FunctionDecl *FD) {
  DeclarationName funcName = FD->getDeclName();
  SourceLocation SLoc = FD->getBeginLoc();
  SourceLocation ELoc = FD->getEndLoc();

  const std::string Recordname = "_Future" + funcName.getAsString();
  RecordDecl *RD = buildAsyncDataRecord(S.Context, Recordname, SLoc, ELoc,
                                        clang::TagDecl::TagKind::TTK_Struct);
  S.PushOnScopeChains(RD, S.getCurScope(), true);
  return RD;
}

// build struct Future for async function
static RecordDecl *buildFutureRecordDecl(
    Sema &S, FunctionDecl *FD, ArrayRef<Expr *> Args,
    std::vector<std::pair<DeclarationName, QualType>> LocalVarList) {
  std::vector<std::pair<DeclarationName, QualType>> paramList;
  DeclarationName funcName = FD->getDeclName();
  SourceLocation SLoc = FD->getBeginLoc();
  SourceLocation ELoc = FD->getEndLoc();
  // Create Record declaration
  const std::string Recordname = "_Future" + funcName.getAsString();
  RecordDecl *RD = buildAsyncDataRecord(S.Context, Recordname, SLoc, ELoc,
                                        clang::TagDecl::TagKind::TTK_Struct);
  RD->startDefinition();

  // Gather Function parameters
  for (FunctionDecl::param_const_iterator pi = FD->param_begin();
       pi != FD->param_end(); pi++) {
    paramList.push_back(std::make_pair<DeclarationName, QualType>(
        (*pi)->getDeclName(), (*pi)->getType()));
  }

  // Gather all awaited expressions
  for (unsigned I = 0; I != Args.size(); ++I) {
    auto *AE = cast<AwaitExpr>(Args[I])->getSubExpr();
    CallExpr *CE = dyn_cast<CallExpr>(AE);
    QualType AEType;
    FunctionDecl *AwaitFD = nullptr;
    if (CE) {
      AwaitFD = dyn_cast_or_null<FunctionDecl>(CE->getCalleeDecl());
      AEType =
          AwaitFD == nullptr
              ? AE->getType()
              : S.Context.getQualifiedType(
                    AE->getType(), AwaitFD->getReturnType().getQualifiers());
    } else AEType = AE->getType();

    if (AwaitFD && AwaitFD == FD) {
      // A recursive await
      assert(CE);
      // This only happen in the recursive case, and I'm building the thing
      // exactly now
      assert(AwaitFD->isAsyncSpecified());

      LocalVarList.push_back(std::make_pair<DeclarationName, QualType>(
          &(S.Context.Idents).get("Ft_" + std::to_string(I + 1)),
          S.Context.getPointerType(S.Context.getRecordType(RD))));
    } else {
      LocalVarList.push_back(std::make_pair<DeclarationName, QualType>(
          &(S.Context.Idents).get("Ft_" + std::to_string(I + 1)),
          AE->getType()));
    }
  }

  for (std::vector<std::pair<DeclarationName, QualType>>::iterator it =
           paramList.begin();
       it != paramList.end(); it++) {
    std::string VarName = it->first.getAsString();
    QualType VarType = it->second;
    addAsyncRecordDecl(S.Context, VarName, VarType, SLoc, ELoc, RD);
  }
  for (std::vector<std::pair<DeclarationName, QualType>>::iterator it =
           LocalVarList.begin();
       it != LocalVarList.end(); it++) {
    const std::string VarName = it->first.getAsString();
    QualType VarType = it->second;
    if (VarType.isConstQualified()) {
      VarType.removeLocalConst();
    }
    addAsyncRecordDecl(S.Context, VarName, VarType, SLoc, ELoc, RD);
  }

  const std::string FutureStateName = "__future_state";
  addAsyncRecordDecl(S.Context, FutureStateName, S.Context.IntTy, SLoc, ELoc,
                     RD);
  RD->completeDefinition();
  S.PushOnScopeChains(RD, S.getCurScope(), true);
  return RD;
}

static std::pair<RecordDecl *, bool>
generateVoidStruct(Sema &S, SourceLocation BLoc, SourceLocation ELoc) {
  std::string Recordname = "Void";
  RecordDecl *VoidRD = nullptr;
  bool IsExisted = false;
  LookupResult Previous(S, &S.Context.Idents.get(Recordname), SourceLocation(),
                        S.LookupTagName, S.NotForRedeclaration);
  S.LookupName(Previous, S.TUScope);
  for (LookupResult::iterator I = Previous.begin(), E = Previous.end(); I != E;
       ++I) {
    if ((VoidRD = dyn_cast<RecordDecl>(*I))) {
      IsExisted = true;
      break;
    }
  }
  if (!IsExisted) {
    VoidRD = buildAsyncDataRecord(S.Context, Recordname, BLoc, ELoc,
                                  clang::TagDecl::TagKind::TTK_Struct);
    VoidRD->startDefinition();
    VoidRD->completeDefinition();
    S.PushOnScopeChains(VoidRD, S.getCurScope(), true);
  }

  return std::make_pair(VoidRD, IsExisted);
}

// Future trait implementation
static VarDecl *buildVtableInitDecl(Sema &S, FunctionDecl *FD,
                                    QualType RecordType, QualType ReturnType,
                                    bool Initialize) {
  auto SLoc = FD->getBeginLoc();
  auto ELoc = FD->getEndLoc();

  LookupResult Previous(S, &S.Context.Idents.get("Future"), SourceLocation(),
                        S.LookupTagName, S.NotForRedeclaration);
  S.LookupName(Previous, S.TUScope);

  TraitTemplateDecl *TTD = nullptr;
  for (LookupResult::iterator I = Previous.begin(), E = Previous.end(); I != E;
       ++I) {
    if ((TTD = dyn_cast<TraitTemplateDecl>(*I))) {
      break;
    }
  }
  if (!TTD) {
    return nullptr;
  }

  TemplateArgumentListInfo Args(SLoc, SLoc);
  Args.addArgument(TemplateArgumentLoc(
      TemplateArgument(ReturnType),
      S.Context.getTrivialTypeSourceInfo(ReturnType, SLoc)));
  QualType InstantiatedTrait =
      S.CheckTemplateIdType(TemplateName(TTD), SourceLocation(), Args);

  if (InstantiatedTrait.isNull()) {
    return nullptr;
  }

  auto x = S.DesugarImplTrait(
      TTD->getTemplatedDecl(), SLoc, SLoc, ELoc, SourceRange(SLoc, ELoc),
      RecordType,
      S.Context.getElaboratedType(ETK_Trait, nullptr, InstantiatedTrait), SLoc,
      Initialize);

  return x;
}

static FunctionDecl *buildFutureInitFunctionDefinition(Sema &S, RecordDecl *RD,
                                                       FunctionDecl *FD,
                                                       FunctionDecl *FDecl) {
  FunctionDecl *NewFD = nullptr;
  SourceLocation SLoc = FD->getBeginLoc();
  SourceLocation NLoc = FD->getNameInfo().getLoc();
  SourceLocation ELoc = FD->getEndLoc();
  FunctionDecl::param_const_iterator pi;

  if (isa<BSCMethodDecl>(FD)) {
    BSCMethodDecl *BMD = cast<BSCMethodDecl>(FD);
    NewFD = buildAsyncBSCMethodDecl(
        S.Context, FDecl->getDeclContext(), SLoc, NLoc, ELoc,
        &(S.Context.Idents).get(FDecl->getName().str()), FDecl->getType(),
        FDecl->getTypeSourceInfo(), SC_None, BMD->getExtendedType());
  } else {
    NewFD = buildAsyncFuncDecl(S.Context, FDecl->getDeclContext(), SLoc, NLoc,
                               &(S.Context.Idents).get(FDecl->getName().str()),
                               FDecl->getType(), FDecl->getTypeSourceInfo());
  }

  SmallVector<ParmVarDecl *, 4> ParmVarDecls;
  for (const auto &I : FD->parameters()) {
    ParmVarDecl *PVD = ParmVarDecl::Create(
        S.Context, NewFD, SourceLocation(), SourceLocation(),
        &(S.Context.Idents).get(I->getName()), I->getType(), nullptr, SC_None,
        nullptr);
    ParmVarDecls.push_back(PVD);
  }
  NewFD->setParams(ParmVarDecls);
  NewFD->setLexicalDeclContext(S.Context.getTranslationUnitDecl());
  S.PushOnScopeChains(NewFD, S.getCurScope(), true);
  // Enter the scope of this instantiation. We don't use
  // PushDeclContext because we don't have a scope.
  Sema::ContextRAII savedContext(S, NewFD);
  LocalInstantiationScope Scope(S);

  S.PushFunctionScope();

  std::string IName = "data";
  VarDecl *VD = VarDecl::Create(
      S.Context, NewFD, SLoc, SLoc, &(S.Context.Idents).get(IName),
      S.Context.getPointerType(S.Context.getRecordType(RD)),
      S.Context.getTrivialTypeSourceInfo(
          S.Context.getPointerType(S.Context.getRecordType(RD)), SLoc),
      SC_None);

  DeclGroupRef DataDG(VD);
  DeclStmt *DataDS = new (S.Context) DeclStmt(DataDG, SLoc, ELoc);
  std::vector<Stmt *> Stmts;
  Stmts.push_back(DataDS);

  std::string CallocName = "calloc";
  LookupResult Previous(S, &S.Context.Idents.get(CallocName), SourceLocation(),
                        S.LookupOrdinaryName, S.NotForRedeclaration);
  S.LookupName(Previous, S.TUScope);
  FunctionDecl *CallocFunc = nullptr;
  for (LookupResult::iterator I = Previous.begin(), E = Previous.end(); I != E;
       ++I) {
    if ((CallocFunc = dyn_cast<FunctionDecl>(*I))) {
      break;
    }
  }
  if (!CallocFunc) {
    S.Diag(FD->getBeginLoc(), diag::err_function_not_found)
        << CallocName << "<stdlib.h>";
    return nullptr;
  }

  llvm::APInt OneResultVal(S.Context.getTargetInfo().getIntWidth(), 1);
  Expr *One = IntegerLiteral::Create(S.Context, OneResultVal, S.Context.IntTy,
                                     SourceLocation());

  Expr *CallocRef =
      S.BuildDeclRefExpr(CallocFunc, CallocFunc->getType(), VK_LValue, NLoc);
  CallocRef = S.ImpCastExprToType(
                   CallocRef, S.Context.getPointerType(CallocRef->getType()),
                   CK_FunctionToPointerDecay)
                  .get();
  // bsc: sizeof(struct __Futurex)
  Expr *SizeOfExpr =
      S.CreateUnaryExprOrTypeTraitExpr(
           S.Context.getTrivialTypeSourceInfo(S.Context.getRecordType(RD)),
           NLoc, UETT_SizeOf, SourceRange())
          .get();
  SmallVector<Expr *, 2> Args;
  Args.push_back(One);
  Args.push_back(SizeOfExpr);

  // bsc: calloc(1, sizeof(struct __Futurex))
  Expr *CE = S.BuildCallExpr(nullptr, CallocRef, SourceLocation(), Args,
                             SourceLocation())
                 .get();
  CE = S.ImpCastExprToType(
            CE, S.Context.getPointerType(S.Context.getRecordType(RD)),
            CK_BitCast)
           .get();
  VD->setInit(CE);
  Expr *DataRef = S.BuildDeclRefExpr(VD, VD->getType(), VK_LValue, NLoc);

  // bsc: if (data == 0)
  llvm::APInt ZeroResultVal(S.Context.getTargetInfo().getIntWidth(), 0);
  Expr *Zero = IntegerLiteral::Create(S.Context, ZeroResultVal, S.Context.IntTy,
                                      SourceLocation());
  Expr *Comp = S.BuildBinOp(nullptr, SourceLocation(), BO_EQ,
                            /*LHSExpr=*/DataRef, /*RHSExpr=*/Zero)
                   .get();

  Sema::ConditionResult IfCond = S.ActOnCondition(
      nullptr, SourceLocation(), Comp, Sema::ConditionKind::Boolean);

  // bsc: exit(1);
  std::string ExitName = "exit";
  LookupResult ExitPrevious(S, &S.Context.Idents.get(ExitName),
                            SourceLocation(), S.LookupOrdinaryName,
                            S.NotForRedeclaration);
  S.LookupName(ExitPrevious, S.TUScope);
  FunctionDecl *ExitFunc = nullptr;
  for (LookupResult::iterator I = ExitPrevious.begin(), E = ExitPrevious.end();
       I != E; ++I) {
    if ((ExitFunc = dyn_cast<FunctionDecl>(*I))) {
      break;
    }
  }
  if (!ExitFunc) {
    S.Diag(FD->getBeginLoc(), diag::err_function_not_found)
        << ExitName << "<stdlib.h>";
    return nullptr;
  }

  Expr *ExitRef =
      S.BuildDeclRefExpr(ExitFunc, ExitFunc->getType(), VK_LValue, NLoc);
  ExitRef =
      S.ImpCastExprToType(ExitRef, S.Context.getPointerType(ExitRef->getType()),
                          CK_FunctionToPointerDecay)
          .get();

  SmallVector<Expr *, 1> ExitArgs = {One};
  Expr *ExitCE = S.BuildCallExpr(nullptr, ExitRef, SourceLocation(), ExitArgs,
                                 SourceLocation())
                     .get();
  // Current code: if (data == 0) exit(1);
  // TODO: before exit(1) function, hope there is printf("calloc failed\n")
  // function to prompt for calloc failure. and it need to import header file
  // "stdio.h".
  SmallVector<Stmt *, 1> BodyStmts = {ExitCE};
  Stmt *Body = CompoundStmt::Create(S.Context, BodyStmts, FPOptionsOverride(),
                                    SourceLocation(), SourceLocation());
  Stmt *If = S.BuildIfStmt(SourceLocation(), IfStatementKind::Ordinary,
                           /*LPL=*/SourceLocation(), /*Init=*/nullptr, IfCond,
                           /*RPL=*/SourceLocation(), /*Body=*/Body,
                           SourceLocation(), nullptr)
                 .get();
  Stmts.push_back(If);

  DataRef = ImplicitCastExpr::Create(S.Context, DataRef->getType(),
                                     CK_LValueToRValue, DataRef, nullptr,
                                     VK_PRValue, FPOptionsOverride());
  pi = NewFD->param_begin();
  for (RecordDecl::field_iterator FieldIt = RD->field_begin();
       pi != NewFD->param_end(); ++pi, ++FieldIt) {
    Expr *LHSExpr = S.BuildMemberExpr(
        DataRef, true, SourceLocation(), NestedNameSpecifierLoc(),
        SourceLocation(), *FieldIt,
        DeclAccessPair::make(*pi, FieldIt->getAccess()), false,
        DeclarationNameInfo(), FieldIt->getType().getNonReferenceType(),
        VK_LValue, OK_Ordinary);

    Expr *RHSExpr =
        S.BuildDeclRefExpr(*pi, FieldIt->getType().getNonReferenceType(),
                           VK_LValue, (*pi)->getLocation());

    Expr *BO =
        S.CreateBuiltinBinOp((*pi)->getLocation(), BO_Assign, LHSExpr, RHSExpr)
            .get();
    Stmts.push_back(BO);
  }

  RecordDecl::field_iterator FutureStateField;
  for (RecordDecl::field_iterator FieldIt = RD->field_begin();
       FieldIt != RD->field_end(); ++FieldIt) {
    if (FieldIt->getDeclName().getAsString() == "__future_state") {
      FutureStateField = FieldIt;
    }
  }

  Expr *LHSExpr = S.BuildMemberExpr(
      DataRef, true, SourceLocation(), NestedNameSpecifierLoc(),
      SourceLocation(), *FutureStateField,
      DeclAccessPair::make(VD, FutureStateField->getAccess()), false,
      DeclarationNameInfo(), FutureStateField->getType().getNonReferenceType(),
      VK_LValue, OK_Ordinary);

  llvm::APInt ResultVal(S.Context.getTargetInfo().getIntWidth(), 0);
  Expr *RHSExpr = IntegerLiteral::Create(S.Context, ResultVal, S.Context.IntTy,
                                         SourceLocation());

  Expr *BO = S.CreateBuiltinBinOp((*FutureStateField)->getLocation(), BO_Assign,
                                  LHSExpr, RHSExpr)
                 .get();
  Stmts.push_back(BO);

  Expr *FutureRefExpr = S.BuildDeclRefExpr(VD, VD->getType(), VK_LValue, NLoc);
  Stmt *RS = S.BuildReturnStmt(NLoc, FutureRefExpr).get();
  Stmts.push_back(RS);

  CompoundStmt *CS =
      CompoundStmt::Create(S.Context, Stmts, FPOptionsOverride(), SLoc, ELoc);
  NewFD->setBody(CS);
  sema::AnalysisBasedWarnings::Policy *ActivePolicy = nullptr;
  S.PopFunctionScopeInfo(ActivePolicy, NewFD, QualType());
  return NewFD;
}

/**
 * Build the Future init declaration
 */
static FunctionDecl *buildFutureInitFunctionDeclaration(Sema &S,
                                                        FunctionDecl *FD,
                                                        QualType FuncRetType) {
  SourceLocation SLoc = FD->getBeginLoc();
  SourceLocation NLoc = FD->getNameInfo().getLoc();
  SourceLocation ELoc = FD->getEndLoc();
  DeclarationName funcName = FD->getDeclName();
  SmallVector<QualType, 16> ParamTys;
  FunctionDecl::param_const_iterator pi;
  for (pi = FD->param_begin(); pi != FD->param_end(); pi++) {
    ParamTys.push_back((*pi)->getType());
  }
  QualType FuncType = S.Context.getFunctionType(FuncRetType, ParamTys, {});
  TypeSourceInfo *Tinfo = FD->getTypeSourceInfo();
  std::string FName = "__" + funcName.getAsString();

  FunctionDecl *NewFD = nullptr;
  if (isa<BSCMethodDecl>(FD)) {
    BSCMethodDecl *BMD = cast<BSCMethodDecl>(FD);
    NewFD =
        buildAsyncBSCMethodDecl(S.Context, FD->getDeclContext(), SLoc, NLoc,
                                ELoc, &(S.Context.Idents).get(FName), FuncType,
                                Tinfo, SC_None, BMD->getExtendedType());
  } else {
    NewFD = buildAsyncFuncDecl(S.Context, FD->getDeclContext(), SLoc, NLoc,
                               &(S.Context.Idents).get(FName), FuncType, Tinfo);
  }
  SmallVector<ParmVarDecl *, 4> ParmVarDecls;
  for (const auto &I : FD->parameters()) {
    ParmVarDecl *PVD = ParmVarDecl::Create(
        S.Context, NewFD, SourceLocation(), SourceLocation(),
        &(S.Context.Idents).get(I->getName()), I->getType(), nullptr, SC_None,
        nullptr);
    ParmVarDecls.push_back(PVD);
  }
  NewFD->setParams(ParmVarDecls);
  NewFD->setLexicalDeclContext(S.Context.getTranslationUnitDecl());
  S.PushOnScopeChains(NewFD, S.getCurScope(), true);
  return NewFD;
}

// build function struct Future for async function
static FunctionDecl *
buildFutureStructInitFunctionDefinition(Sema &S, RecordDecl *RD,
                                        FunctionDecl *OriginFD) {
  SourceLocation SLoc = OriginFD->getBeginLoc();
  SourceLocation NLoc = OriginFD->getNameInfo().getLoc();
  SourceLocation ELoc = OriginFD->getEndLoc();
  QualType FuncRetType = S.Context.getRecordType(RD);
  SmallVector<QualType, 16> ParamTys;
  FunctionDecl::param_const_iterator pi;
  for (pi = OriginFD->param_begin(); pi != OriginFD->param_end(); pi++) {
    ParamTys.push_back((*pi)->getType());
  }

  QualType FuncType = S.Context.getFunctionType(FuncRetType, ParamTys, {});
  FunctionDecl *NewFD = nullptr;

  std::string FuncName = "__" + OriginFD->getName().str();
  if (isa<BSCMethodDecl>(OriginFD)) {
    BSCMethodDecl *BMD = cast<BSCMethodDecl>(OriginFD);
    NewFD = buildAsyncBSCMethodDecl(
        S.Context, OriginFD->getDeclContext(), SLoc, NLoc, ELoc,
        &(S.Context.Idents).get(FuncName), FuncType,
        OriginFD->getTypeSourceInfo(), SC_None, BMD->getExtendedType());
  } else {
    NewFD = buildAsyncFuncDecl(
        S.Context, OriginFD->getDeclContext(), SLoc, NLoc,
        &(S.Context.Idents).get(FuncName), FuncType,
        OriginFD->getTypeSourceInfo());
  }

  SmallVector<ParmVarDecl *, 4> ParmVarDecls;
  for (const auto &I : OriginFD->parameters()) {
    ParmVarDecl *PVD = ParmVarDecl::Create(
        S.Context, NewFD, SourceLocation(), SourceLocation(),
        &(S.Context.Idents).get(I->getName()), I->getType(), nullptr, SC_None,
        nullptr);
    ParmVarDecls.push_back(PVD);
  }
  NewFD->setParams(ParmVarDecls);
  NewFD->setLexicalDeclContext(S.Context.getTranslationUnitDecl());
  S.PushOnScopeChains(NewFD, S.getCurScope(), true);

  // Enter the scope of this instantiation. We don't use
  // PushDeclContext because we don't have a scope.
  Sema::ContextRAII savedContext(S, NewFD);
  LocalInstantiationScope Scope(S);

  S.PushFunctionScope();

  // Instantiate the struct object and assign values to it
  std::string IName = "fi";
  VarDecl *VD = VarDecl::Create(S.Context, NewFD, SLoc, SLoc,
                                &(S.Context.Idents).get(IName), FuncRetType,
                                nullptr, SC_None);
  InitListExpr *ILE = new (S.Context)
      InitListExpr(S.Context, SourceLocation(), {}, SourceLocation());
  ILE->setType(FuncRetType);
  S.AddInitializerToDecl(VD, ILE, /*DirectInit=*/false);
  DeclGroupRef FutureDG(VD);
  DeclStmt *FutureDS = new (S.Context) DeclStmt(FutureDG, SLoc, ELoc);
  std::vector<Stmt *> Stmts;
  Stmts.push_back(FutureDS);

  Expr *StructFutureRef =
      S.BuildDeclRefExpr(VD, VD->getType(), VK_LValue, NLoc);

  RecordDecl::field_iterator FieldIt = RD->field_begin(),
                             Field_end = RD->field_end();
  llvm::SmallVector<Expr *, 4> InitExprs;
  for (pi = NewFD->param_begin();
       pi != NewFD->param_end() && FieldIt != Field_end; ++pi, ++FieldIt) {
    DeclarationName Name = FieldIt->getDeclName();
    DeclarationNameInfo MemberNameInfo(Name, FieldIt->getLocation());
    Expr *LHSExpr = S.BuildMemberExpr(
        StructFutureRef, false, SourceLocation(), NestedNameSpecifierLoc(),
        SourceLocation(), *FieldIt,
        DeclAccessPair::make(*FieldIt, FieldIt->getAccess()), false,
        MemberNameInfo, FieldIt->getType().getNonReferenceType(), VK_LValue,
        OK_Ordinary);
    ExprResult RHSER =
        S.BuildDeclRefExpr(*pi, FieldIt->getType().getNonReferenceType(),
                           VK_LValue, SourceLocation());
    if (RHSER.isInvalid())
      return nullptr;
    Expr *RHSExpr = RHSER.get();

    Expr *BO =
        S.CreateBuiltinBinOp((*pi)->getLocation(), BO_Assign, LHSExpr, RHSExpr)
            .get();
    Stmts.push_back(BO);
  }

  RecordDecl::field_iterator FutureStateField;
  for (RecordDecl::field_iterator FieldIt = RD->field_begin();
       FieldIt != RD->field_end(); ++FieldIt) {
    if (FieldIt->getDeclName().getAsString() == "__future_state") {
      FutureStateField = FieldIt;
    }
  }
  DeclarationName Name = FutureStateField->getDeclName();
  DeclarationNameInfo MemberNameInfo(Name, FutureStateField->getLocation());
  Expr *LHSExpr = S.BuildMemberExpr(
      StructFutureRef, false, SourceLocation(), NestedNameSpecifierLoc(),
      SourceLocation(), *FutureStateField,
      DeclAccessPair::make(*FutureStateField, FutureStateField->getAccess()),
      false, MemberNameInfo, FutureStateField->getType().getNonReferenceType(),
      VK_LValue, OK_Ordinary);

  llvm::APInt ResultVal(S.Context.getTargetInfo().getIntWidth(), 0);
  Expr *RHSExpr = IntegerLiteral::Create(S.Context, ResultVal, S.Context.IntTy,
                                         SourceLocation());
  Expr *BO = S.CreateBuiltinBinOp((*FutureStateField)->getLocation(), BO_Assign,
                                  LHSExpr, RHSExpr)
                 .get();
  Stmts.push_back(BO);

  // Generating:
  // @code
  // return fi;
  // @endcode
  // No need for ImplicitCastExpr since BuildReturnStmt will generate for us.
  Stmt *RS = S.BuildReturnStmt(NLoc, StructFutureRef).get();
  Stmts.push_back(RS);

  CompoundStmt *CS =
      CompoundStmt::Create(S.Context, Stmts, FPOptionsOverride(), SLoc, ELoc);
  NewFD->setBody(CS);
  sema::AnalysisBasedWarnings::Policy *ActivePolicy = nullptr;
  S.PopFunctionScopeInfo(ActivePolicy, NewFD, QualType());
  return NewFD;
}

static IfStmt *processAwaitExprStatus(Sema &S, int AwaitCount, RecordDecl *RD,
                                      Expr *ICE, ParmVarDecl *PVD,
                                      VarDecl *PollResultVar,
                                      VarDecl *AwaitResult,
                                      BSCMethodDecl *NewFD,
                                      Stmt *ThenBodyStmt) {
  std::vector<Stmt *> ElseStmts;
  Expr *IfCond = nullptr;
  SourceLocation BLoc = ThenBodyStmt->getBeginLoc();

  RecordDecl::field_iterator TheField;
  for (RecordDecl::field_iterator FieldIt = RD->field_begin();
       FieldIt != RD->field_end(); ++FieldIt) {
    if (FieldIt->getDeclName().getAsString() == "__future_state") {
      TheField = FieldIt;
      break;
    }
  }

  if (*TheField) {
    DeclarationName Name = TheField->getDeclName();
    DeclarationNameInfo MemberNameInfo(Name, TheField->getLocation());
    Expr *LHSExpr = S.BuildMemberExpr(
        ICE, true, SourceLocation(), NestedNameSpecifierLoc(), SourceLocation(),
        *TheField, DeclAccessPair::make(*TheField, TheField->getAccess()),
        false, MemberNameInfo, TheField->getType().getNonReferenceType(),
        VK_LValue, OK_Ordinary);

    llvm::APInt ResultVal(S.Context.getTargetInfo().getIntWidth(), AwaitCount);
    Expr *RHSExpr = IntegerLiteral::Create(S.Context, ResultVal,
                                           S.Context.IntTy, SourceLocation());

    Expr *BO = S.CreateBuiltinBinOp((*TheField)->getLocation(), BO_Assign,
                                    LHSExpr, RHSExpr)
                   .get();
    ElseStmts.push_back(BO);
  }

  RecordDecl *PollResultRD = PollResultVar->getType()->getAsRecordDecl();
  BSCMethodDecl *IsCompletedFD =
      lookupBSCMethodInRecord(S, "is_completed", PollResultRD);
  if (IsCompletedFD) {
    Expr *IsCompletedFDRef = S.BuildDeclRefExpr(
        IsCompletedFD, IsCompletedFD->getType(), VK_LValue, BLoc);
    IsCompletedFDRef->HasBSCScopeSpec = true;
    IsCompletedFDRef = S.ImpCastExprToType(IsCompletedFDRef,
                                           S.Context.getPointerType(
                                               IsCompletedFDRef->getType()),
                                           CK_FunctionToPointerDecay)
                           .get();

    if (PollResultRD) {
      Expr *PollResultVarRef = S.BuildDeclRefExpr(
          PollResultVar, PollResultVar->getType(), VK_LValue, BLoc);
      PollResultVarRef =
          S.CreateBuiltinUnaryOp(BLoc, UO_AddrOf, PollResultVarRef).get();
      Expr *AwaitResultRef = S.BuildDeclRefExpr(
          AwaitResult, AwaitResult->getType(), VK_LValue, BLoc);
      AwaitResultRef =
          S.CreateBuiltinUnaryOp(BLoc, UO_AddrOf, AwaitResultRef).get();

      SmallVector<Expr *, 2> Args;
      Args.push_back(PollResultVarRef);
      Args.push_back(AwaitResultRef);
      IfCond =
          S.BuildCallExpr(nullptr, IsCompletedFDRef, BLoc, Args, BLoc).get();
    }
  }

  RecordDecl *PollResultRDForPending =
      NewFD->getReturnType()->getAsRecordDecl();
  BSCMethodDecl *PendingFD =
      lookupBSCMethodInRecord(S, "pending", PollResultRDForPending);

  if (PendingFD) {
    Expr *PendingFDRef =
        S.BuildDeclRefExpr(PendingFD, PendingFD->getType(), VK_LValue, BLoc);
    PendingFDRef->HasBSCScopeSpec = true;
    PendingFDRef =
        S.ImpCastExprToType(PendingFDRef,
                            S.Context.getPointerType(PendingFDRef->getType()),
                            CK_FunctionToPointerDecay)
            .get();
    Expr *PendingCall =
        S.BuildCallExpr(nullptr, PendingFDRef, BLoc, {}, BLoc).get();
    Stmt *RS = S.BuildReturnStmt(BLoc, PendingCall).get();
    ElseStmts.push_back(RS);
  }

  std::vector<Stmt *> BodyStmts{ThenBodyStmt};

  Stmt *Body =
      CompoundStmt::Create(S.Context, BodyStmts, FPOptionsOverride(), BLoc, BLoc);
  Stmt *Else =
      CompoundStmt::Create(S.Context, ElseStmts, FPOptionsOverride(), BLoc, BLoc);
  IfStmt *If = IfStmt::Create(S.Context, BLoc, IfStatementKind::Ordinary,
                              /*Init=*/nullptr,
                              /*Var=*/nullptr, IfCond,
                              /*LPL=*/BLoc,
                              /*RPL=*/BLoc, Body, BLoc, Else);
  return If;
}

namespace {
  /**
   * Visit a Stmt and return true if there's a recursive call to the provided Decl
  */
class RecursiveCallVisitor : public ConstStmtVisitor<RecursiveCallVisitor, bool> {
public:
  RecursiveCallVisitor(const Decl *FD) : FD(FD) {}
  bool VisitCallExpr(const CallExpr *E) {
    if (E->getCalleeDecl() == FD)
      return true;

    return this->VisitStmt(static_cast<const Stmt*>(E));
  }
  bool VisitStmt(const Stmt *S) {
    for (auto *C : S->children()) {
      if (C) {
        if (Visit(C)) {
          return true;
        }
      }
    }
    return false;
  }

private:
  const Decl *FD;
};
} // namespace

namespace {
class TransformToReturnVoid : public TreeTransform<TransformToReturnVoid> {
  typedef TreeTransform<TransformToReturnVoid> BaseTransform;
  QualType ReturnTy = QualType();

public:
  TransformToReturnVoid(Sema &SemaRef) : BaseTransform(SemaRef) {}

  // make sure redo semantic analysis
  bool AlwaysRebuild() { return true; }

  FunctionDecl *TransformFunctionDecl(FunctionDecl *D) {
    FunctionDecl *NewFD = D;
    if (D->getReturnType()->isVoidType()) {
      LookupResult ExitPrevious(SemaRef, &SemaRef.Context.Idents.get("Void"),
                                SourceLocation(), SemaRef.LookupTagName,
                                SemaRef.NotForRedeclaration);
      SemaRef.LookupName(ExitPrevious, SemaRef.TUScope);
      RecordDecl *ReturnDecl = nullptr;
      for (LookupResult::iterator I = ExitPrevious.begin(),
                                  E = ExitPrevious.end();
           I != E; ++I) {
        if ((ReturnDecl = dyn_cast<RecordDecl>(*I))) {
          break;
        }
      }

      assert(ReturnDecl && "struct Void not generated");
      ReturnTy = SemaRef.Context.getRecordType(ReturnDecl);
      SmallVector<QualType, 16> ParamTys;
      FunctionDecl::param_const_iterator pi;
      for (pi = D->param_begin(); pi != D->param_end(); pi++) {
        ParamTys.push_back((*pi)->getType());
      }
      QualType FuncType =
          SemaRef.Context.getFunctionType(ReturnTy, ParamTys, {});

      SourceLocation SLoc = D->getBeginLoc();
      SourceLocation NLoc = D->getNameInfo().getLoc();
      SourceLocation ELoc = D->getEndLoc();
      TypeSourceInfo *Tinfo = D->getTypeSourceInfo();
      std::string FName = std::string(D->getIdentifier()->getName());

      if (isa<BSCMethodDecl>(D)) {
        BSCMethodDecl *BMD = cast<BSCMethodDecl>(D);
        NewFD = buildAsyncBSCMethodDecl(
            SemaRef.Context, D->getDeclContext(), SLoc, NLoc, ELoc,
            &(SemaRef.Context.Idents).get(FName), FuncType, Tinfo, SC_None,
            BMD->getExtendedType());
      } else {
        NewFD = buildAsyncFuncDecl(SemaRef.Context, D->getDeclContext(), SLoc,
                                   NLoc, &(SemaRef.Context.Idents).get(FName),
                                   FuncType, Tinfo);
      }
      SmallVector<ParmVarDecl *, 4> ParmVarDecls;
      for (const auto &I : D->parameters()) {
        ParmVarDecl *PVD = ParmVarDecl::Create(
            SemaRef.Context, NewFD, SourceLocation(), SourceLocation(),
            &(SemaRef.Context.Idents).get(I->getName()), I->getType(), nullptr,
            SC_None, nullptr);
        ParmVarDecls.push_back(PVD);
      }
      NewFD->setParams(ParmVarDecls);
      NewFD->setLexicalDeclContext(SemaRef.Context.getTranslationUnitDecl());

      CompoundStmt *body =
          llvm::dyn_cast_if_present<CompoundStmt>(D->getBody());
      // If transforming a declaration without body this may not be present
      if (body) {
        Stmt *LastStmt = body->body_back();
        if (!LastStmt || !dyn_cast<ReturnStmt>(LastStmt)) {
          std::vector<Stmt *> Stmts;
          for (auto *C : body->children()) {
            Stmts.push_back(C);
          }
          ReturnStmt *RS = ReturnStmt::Create(SemaRef.Context, SourceLocation(),
                                              nullptr, nullptr);
          Stmts.push_back(RS);
          Sema::CompoundScopeRAII CompoundScope(SemaRef);
          body = BaseTransform::RebuildCompoundStmt(SourceLocation(), Stmts,
                                                    SourceLocation(), false)
                     .getAs<CompoundStmt>();
        }

        Stmt *FuncBody = BaseTransform::TransformStmt(body).getAs<Stmt>();
        NewFD->setBody(FuncBody);
      }
    }
    return NewFD;
  }

  StmtResult TransformReturnStmt(ReturnStmt *S) {
    assert(!S->getRetValue() && "void function should not return a value");
    InitListExpr *ILE = new (SemaRef.Context)
        InitListExpr(SemaRef.Context, SourceLocation(), {}, SourceLocation());
    QualType Ty = SemaRef.Context.getElaboratedType(ETK_Struct, nullptr,
                                                    ReturnTy, nullptr);
    ILE->setType(Ty);
    TypeSourceInfo *superTInfo = SemaRef.Context.getTrivialTypeSourceInfo(Ty);
    CompoundLiteralExpr *CLE = new (SemaRef.Context) CompoundLiteralExpr(
        SourceLocation(), superTInfo, Ty, VK_LValue, ILE, false);
    ImplicitCastExpr *ICE =
        ImplicitCastExpr::Create(SemaRef.Context, Ty, CK_LValueToRValue, CLE,
                                 nullptr, VK_PRValue, FPOptionsOverride());
    ReturnStmt *RS =
        ReturnStmt::Create(SemaRef.Context, SourceLocation(), ICE, nullptr);
    return RS;
  }
};
}  // namespace

namespace {
class TransformToAP : public TreeTransform<TransformToAP> {
  typedef TreeTransform<TransformToAP> BaseTransform;
  Expr *PDRE;
  RecordDecl *FutureRD;
  BSCMethodDecl *FD;
  std::vector<Stmt *> DeclStmts;
  int DIndex;
  llvm::DenseMap<Stmt *, std::tuple<int, int>> DMap;
  std::map<std::string, VarDecl *> ArrayPointerMap;
  std::map<std::string, VarDecl *> ArrayAssignedPointerMap;

public:
  TransformToAP(Sema &SemaRef, Expr *PDRE, RecordDecl *FutureRD,
                BSCMethodDecl *FD)
      : BaseTransform(SemaRef), PDRE(PDRE), FutureRD(FutureRD), FD(FD) {
    DIndex = 0;
  }

  // make sure redo semantic analysis
  bool AlwaysRebuild() { return true; }

  ExprResult TransformDeclRefExpr(DeclRefExpr *E) {
    Expr *RE = E;
    RecordDecl::field_iterator TheField, FieldIt;
    for (FieldIt = FutureRD->field_begin(); FieldIt != FutureRD->field_end();
         ++FieldIt) {
      if (FieldIt->getDeclName().getAsString() ==
          cast<DeclRefExpr>(RE)->getDecl()->getName()) {
        TheField = FieldIt;
        break;
      }
    }

    if (FieldIt != FutureRD->field_end()) {
      DeclarationName Name = TheField->getDeclName();
      DeclarationNameInfo MemberNameInfo(Name, TheField->getLocation());
      RE = BaseTransform::RebuildMemberExpr(
               PDRE, SourceLocation(), true, NestedNameSpecifierLoc(),
               SourceLocation(), MemberNameInfo, *TheField,
               DeclAccessPair::make(*TheField, TheField->getAccess()).getDecl(),
               nullptr, nullptr)
               .getAs<Expr>();
    }

    return RE;
  }

  StmtResult TransformDeclStmt(DeclStmt *S) {
    Stmt *Result = nullptr;
    DeclStmt *StmDecl = S;
    int CIndex = 0;

    std::vector<BinaryOperator *> BOStmts;
    std::vector<Stmt *> NullStmts;
    for (auto *SD : StmDecl->decls()) {
      if (VarDecl *VD = dyn_cast<VarDecl>(SD)) {
        Expr *Init = VD->getInit();
        Expr *LE = nullptr;
        Expr *RE = nullptr;
        QualType QT = VD->getType();

        if (VD->isExternallyVisible() || VD->isConstexpr() ||
            VD->isStaticLocal())
          return S;

        // Do not need to transform constant variable with compile-time constant
        // initializier.
        const Expr *Culprit;
        if (QT.isConstQualified() && Init && !Init->isValueDependent() &&
            Init->isConstantInitializer(SemaRef.Context, false, &Culprit))
          return S;

        RecordDecl::field_iterator LField, FieldIt;
        for (FieldIt = FutureRD->field_begin();
             FieldIt != FutureRD->field_end(); ++FieldIt) {
          if (FieldIt->getDeclName().getAsString() == VD->getName()) {
            LField = FieldIt;
            break;
          }
        }

        if (FieldIt != FutureRD->field_end()) {
          DeclarationName Name = LField->getDeclName();
          DeclarationNameInfo MemberNameInfo(Name, LField->getLocation());
          LE = BaseTransform::RebuildMemberExpr(
                   PDRE, SourceLocation(), true, NestedNameSpecifierLoc(),
                   SourceLocation(), MemberNameInfo, *LField,
                   DeclAccessPair::make(*LField, LField->getAccess()).getDecl(),
                   nullptr, nullptr)
                   .getAs<Expr>();
        }

        if (Init && (QT->isArrayType() || QT->isRecordType())) {
          Expr *CInit = BaseTransform::TransformExpr(Init).get();

          std::string ArgName = VD->getName().str();
          VarDecl *ArgVDNew = VarDecl::Create(
              SemaRef.Context, FD, SourceLocation(), SourceLocation(),
              &(SemaRef.Context.Idents).get(ArgName),
              VD->getType().getNonReferenceType(), nullptr, SC_None);

          SemaRef.AddInitializerToDecl(ArgVDNew, CInit, /*DirectInit=*/false);
          DeclStmt *DSNew =
              SemaRef
                  .ActOnDeclStmt(SemaRef.ConvertDeclToDeclGroup(ArgVDNew),
                                 SourceLocation(), SourceLocation())
                  .getAs<DeclStmt>();

          DeclStmts.push_back(DSNew);

          RE = SemaRef.BuildDeclRefExpr(ArgVDNew,
                                        VD->getType().getNonReferenceType(),
                                        VK_LValue, SourceLocation());
          // No need for ImplicitCastExpr which will be generated
          // by RebuildBinaryOperator in future.
          DIndex++;
          CIndex++;

          if (const ConstantArrayType *CA = dyn_cast<ConstantArrayType>(QT)) {
            int Elements = SemaRef.Context.getConstantArrayElementCount(CA);
            QualType SubQT = SemaRef.Context.getBaseElementType(QT);

            QualType Pty = SemaRef.Context.getPointerType(SubQT);
            Expr *AssignedRVExpr = SemaRef.BuildDeclRefExpr(
                ArgVDNew, ArgVDNew->getType(), VK_LValue, SourceLocation());
            TypeSourceInfo *AssignedType =
                SemaRef.Context.getTrivialTypeSourceInfo(Pty);
            Expr *AssignedCCE = BaseTransform::RebuildCStyleCastExpr(
                                    SourceLocation(), AssignedType,
                                    SourceLocation(), AssignedRVExpr)
                                    .get();

            std::string AssignedPtrName =
                "__ASSIGNED_ARRAY_PTR_" + GetPrefix(SubQT);
            VarDecl *AssignedPtrVar =
                GetArrayAssignedPointerMap(AssignedPtrName);
            if (AssignedPtrVar == nullptr) {
              AssignedPtrVar = VarDecl::Create(
                  SemaRef.Context, FD, SourceLocation(), SourceLocation(),
                  &(SemaRef.Context.Idents).get(AssignedPtrName),
                  Pty.getNonReferenceType(), nullptr, SC_None);

              SemaRef.AddInitializerToDecl(AssignedPtrVar, AssignedCCE,
                                           /*DirectInit=*/false);

              DeclStmt *AssignedDS =
                  SemaRef
                      .ActOnDeclStmt(
                          SemaRef.ConvertDeclToDeclGroup(AssignedPtrVar),
                          SourceLocation(), SourceLocation())
                      .getAs<DeclStmt>();

              DeclStmts.push_back(AssignedDS);
              SetArrayAssignedPointerMap(AssignedPtrName, AssignedPtrVar);
            } else {
              Expr *AssignedDRE = SemaRef.BuildDeclRefExpr(
                  AssignedPtrVar, AssignedPtrVar->getType(), VK_LValue,
                  SourceLocation());
              Stmt *AssignedBO =
                  BaseTransform::RebuildBinaryOperator(
                      SourceLocation(), BO_Assign, AssignedDRE, AssignedCCE)
                      .getAs<Stmt>();
              DeclStmts.push_back(AssignedBO);
            }
            DIndex++;
            CIndex++;

            TypeSourceInfo *ArrayType =
                SemaRef.Context.getTrivialTypeSourceInfo(Pty);
            Expr *ArrayCCE =
                BaseTransform::RebuildCStyleCastExpr(
                    SourceLocation(), ArrayType, SourceLocation(), LE)
                    .get();

            std::string ArrayPtrName = "__ARRAY_PTR_" + GetPrefix(SubQT);
            VarDecl *ArrayPtrVar = GetArrayPointerMap(ArrayPtrName);
            if (ArrayPtrVar == nullptr) {
              ArrayPtrVar = VarDecl::Create(
                  SemaRef.Context, FD, SourceLocation(), SourceLocation(),
                  &(SemaRef.Context.Idents).get(ArrayPtrName),
                  Pty.getNonReferenceType(), nullptr, SC_None);

              SemaRef.AddInitializerToDecl(ArrayPtrVar, ArrayCCE,
                                           /*DirectInit=*/false);
              DeclStmt *ArrayDS =
                  SemaRef
                      .ActOnDeclStmt(
                          SemaRef.ConvertDeclToDeclGroup(ArrayPtrVar),
                          SourceLocation(), SourceLocation())
                      .getAs<DeclStmt>();

              DeclStmts.push_back(ArrayDS);
              SetArrayPointerMap(ArrayPtrName, ArrayPtrVar);
            } else {
              Expr *ArrayDRE =
                  SemaRef.BuildDeclRefExpr(ArrayPtrVar, ArrayPtrVar->getType(),
                                           VK_LValue, SourceLocation());
              Stmt *ArrayBO =
                  BaseTransform::RebuildBinaryOperator(
                      SourceLocation(), BO_Assign, ArrayDRE, ArrayCCE)
                      .getAs<Stmt>();
              DeclStmts.push_back(ArrayBO);
            }
            DIndex++;
            CIndex++;

            if (Elements > 1) {
              VarDecl *IArgVDNew = VarDecl::Create(
                  SemaRef.Context, FD, SourceLocation(), SourceLocation(),
                  &(SemaRef.Context.Idents).get("I"), SemaRef.Context.IntTy,
                  nullptr, SC_None);
              llvm::APInt Zero(
                  SemaRef.Context.getTypeSize(SemaRef.Context.IntTy), 0);
              Expr *IInit = IntegerLiteral::Create(SemaRef.Context, Zero,
                                                   SemaRef.Context.IntTy,
                                                   SourceLocation());
              IArgVDNew->setInit(IInit);
              DeclGroupRef IDG(IArgVDNew);
              Stmt *FInit = new (SemaRef.Context)
                  DeclStmt(IDG, SourceLocation(), SourceLocation());

              Expr *IDRE =
                  SemaRef.BuildDeclRefExpr(IArgVDNew, SemaRef.Context.IntTy,
                                           VK_LValue, SourceLocation());
              llvm::APInt ISize(
                  SemaRef.Context.getTypeSize(SemaRef.Context.IntTy), Elements);
              Expr *IRE = IntegerLiteral::Create(SemaRef.Context, ISize,
                                                 SemaRef.Context.IntTy,
                                                 SourceLocation());

              Expr *Cond = BaseTransform::RebuildBinaryOperator(
                               SourceLocation(), BO_LT, IDRE, IRE)
                               .getAs<Expr>();

              Expr *Inc = BaseTransform::RebuildUnaryOperator(SourceLocation(),
                                                              UO_PreInc, IDRE)
                              .getAs<Expr>();

              llvm::SmallVector<Stmt *, 1> Stmts;

              Expr *LHS =
                  SemaRef.BuildDeclRefExpr(ArrayPtrVar, ArrayPtrVar->getType(),
                                           VK_LValue, SourceLocation());
              LHS = SemaRef
                        .CreateBuiltinUnaryOp(SourceLocation(), UO_PostInc, LHS)
                        .get();
              LHS =
                  SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_Deref, LHS)
                      .get();

              Expr *RHS = SemaRef.BuildDeclRefExpr(AssignedPtrVar,
                                                   AssignedPtrVar->getType(),
                                                   VK_LValue, SourceLocation());
              RHS = SemaRef
                        .CreateBuiltinUnaryOp(SourceLocation(), UO_PostInc, RHS)
                        .get();
              RHS =
                  SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_Deref, RHS)
                      .get();
              Expr *BO = BaseTransform::RebuildBinaryOperator(
                             SourceLocation(), BO_Assign, LHS, RHS)
                             .getAs<Expr>();

              Stmts.push_back(BO);
              Sema::CompoundScopeRAII CompoundScope(SemaRef);
              Stmt *Body = BaseTransform::RebuildCompoundStmt(
                               SourceLocation(), Stmts, SourceLocation(), false)
                               .getAs<CompoundStmt>();
              ForStmt *For = new (SemaRef.Context)
                  ForStmt(SemaRef.Context, FInit, Cond, nullptr, Inc, Body,
                          SourceLocation(), SourceLocation(), SourceLocation());

              DeclStmts.push_back(For);
              DIndex++;
              CIndex++;

              LE = SemaRef.BuildDeclRefExpr(ArrayPtrVar, ArrayPtrVar->getType(),
                                            VK_LValue, SourceLocation());
              RE = new (SemaRef.Context) ImplicitValueInitExpr(LE->getType());
            }
          }
        }

        if (Init == nullptr) {
          std::string ArgName = VD->getName().str();
          VarDecl *ArgVDNew = VarDecl::Create(
              SemaRef.Context, FD, SourceLocation(), SourceLocation(),
              &(SemaRef.Context.Idents).get(ArgName),
              VD->getType().getNonReferenceType(), nullptr, SC_None);
          SemaRef.ActOnUninitializedDecl(ArgVDNew);
          DeclStmt *DSNew =
              SemaRef
                  .ActOnDeclStmt(SemaRef.ConvertDeclToDeclGroup(ArgVDNew),
                                 SourceLocation(), SourceLocation())
                  .getAs<DeclStmt>();

          DeclStmts.push_back(DSNew);
          DIndex++;
          CIndex++;
        }

        if (LE && Init) {
          if (RE == nullptr)
            RE = BaseTransform::TransformExpr(const_cast<Expr *>(Init))
                     .getAs<Expr>();
          BinaryOperator *BO = BaseTransform::RebuildBinaryOperator(
                                   LField->getLocation(), BO_Assign, LE, RE)
                                   .getAs<BinaryOperator>();
          BOStmts.push_back(BO);
        }
      }
    }

    int BOSize = BOStmts.size();
    if (BOSize == 0 && DeclStmts.empty())
      return S;
    if (BOSize == 0) {
      Result = DeclStmts[DeclStmts.size() - 1];
      DeclStmts.erase(DeclStmts.end() - 1);
      DIndex--;
      CIndex--;
    } else if (BOSize == 1) {
      Result = cast<Stmt>(BOStmts.front());
    } else {
      BinaryOperator *PreBO = BOStmts.front();
      for (int I = 1; I < BOSize; I++) {
        BinaryOperator *BO = BaseTransform::RebuildBinaryOperator(
                                 SourceLocation(), BO_Comma, PreBO, BOStmts[I])
                                 .getAs<BinaryOperator>();
        PreBO = BO;
      }
      Result = cast<Stmt>(PreBO);
    }
    if (CIndex > 0)
      SetDMap(Result, std::make_tuple(DIndex - CIndex, CIndex));
    return Result;
  }

  void SetDMap(Stmt *S, std::tuple<int, int> val) {
    assert(S && "Passed null DeclStmt");
    DMap[S] = val;
  }

  std::tuple<int, int> GetDMap(Stmt *S) {
    llvm::DenseMap<Stmt *, std::tuple<int, int>>::iterator I = DMap.find(S);
    if (I != DMap.end()) return I->second;
    return {-1, -1};
  }

  std::vector<Stmt *> GetDeclStmts() { return DeclStmts; }

  void SetArrayPointerMap(std::string APName, VarDecl *VD) {
    assert(VD && "Passed null array pointers variable");
    ArrayPointerMap[APName] = VD;
  }

  VarDecl *GetArrayPointerMap(std::string APName) {
    std::map<std::string, VarDecl *>::iterator I = ArrayPointerMap.find(APName);
    if (I != ArrayPointerMap.end())
      return I->second;
    return nullptr;
  }

  void SetArrayAssignedPointerMap(std::string AAPName, VarDecl *VD) {
    assert(VD && "Passed null array pointers variable");
    ArrayAssignedPointerMap[AAPName] = VD;
  }

  VarDecl *GetArrayAssignedPointerMap(std::string AAPName) {
    std::map<std::string, VarDecl *>::iterator I =
        ArrayAssignedPointerMap.find(AAPName);
    if (I != ArrayAssignedPointerMap.end())
      return I->second;
    return nullptr;
  }
};
}  // namespace

namespace {
/**
 * Transform the function body so it manages a single state and the return
 * statement wraps the results in PollResult
 */
class TransformToHasSingleStateAndReturnStatements
    : public TreeTransform<TransformToHasSingleStateAndReturnStatements> {
  typedef TreeTransform<TransformToHasSingleStateAndReturnStatements>
      BaseTransform;
  TransformToAP DT;
  Expr *PDRE;
  RecordDecl *FutureRD;
  BSCMethodDecl *FD;
  std::map<std::string, int> IdentifierNumber;

public:
  TransformToHasSingleStateAndReturnStatements(Sema &SemaRef, TransformToAP DT,
                                               Expr *PDRE, RecordDecl *FutureRD,
                                               BSCMethodDecl *FD)
      : BaseTransform(SemaRef), DT(DT), PDRE(PDRE), FutureRD(FutureRD), FD(FD) {
  }

  // make sure redo semantic analysis
  bool AlwaysRebuild() { return true; }

  StmtResult TransformIfStmt(IfStmt *S) {
    bool HasStatement = false;
    bool HasStatementElse = false;
    IfStmt *If = S;
    Stmt *TS = If->getThen();
    Stmt *ES = If->getElse();
    if (TS != nullptr) HasStatement = (hasAwaitExpr(TS) || hasReturnStmt(TS));
    if (ES != nullptr)
      HasStatementElse = (hasAwaitExpr(ES) || hasReturnStmt(ES));

    if (HasStatementElse && !isRefactorStmt(ES)) {
      std::vector<Stmt *> Stmts;
      Stmts.push_back(ES);
      Sema::CompoundScopeRAII CompoundScope(SemaRef);
      ES = BaseTransform::RebuildCompoundStmt(If->getBeginLoc(), Stmts,
                                              If->getEndLoc(), false)
               .getAs<CompoundStmt>();
      If->setElse(ES);
    }

    if (HasStatement && !isRefactorStmt(TS)) {
      std::vector<Stmt *> Stmts;
      Stmts.push_back(TS);
      Sema::CompoundScopeRAII CompoundScope(SemaRef);
      TS = BaseTransform::RebuildCompoundStmt(If->getBeginLoc(), Stmts,
                                              If->getEndLoc(), false)
               .getAs<CompoundStmt>();
      If->setThen(TS);
    }

    return BaseTransform::TransformIfStmt(If);
  }

  StmtResult TransformWhileStmt(WhileStmt *S) {
    bool HasStatement = false;
    WhileStmt *WS = S;
    Stmt *Body = WS->getBody();

    if (Body != nullptr)
      HasStatement = (hasAwaitExpr(Body) || hasReturnStmt(Body));

    if (HasStatement && !isRefactorStmt(Body)) {
      std::vector<Stmt *> Stmts;
      Stmts.push_back(Body);
      Sema::CompoundScopeRAII CompoundScope(SemaRef);
      Body = BaseTransform::RebuildCompoundStmt(Body->getBeginLoc(), Stmts,
                                                Body->getEndLoc(), false)
                 .getAs<CompoundStmt>();
      WS->setBody(Body);
    }
    return BaseTransform::TransformWhileStmt(WS);
  }

  StmtResult TransformDoStmt(DoStmt *S) {
    bool HasStatement = false;
    DoStmt *DS = S;
    Stmt *Body = DS->getBody();

    if (Body != nullptr)
      HasStatement = (hasAwaitExpr(Body) || hasReturnStmt(Body));

    if (HasStatement && !isRefactorStmt(Body)) {
      std::vector<Stmt *> Stmts;
      Stmts.push_back(Body);
      Sema::CompoundScopeRAII CompoundScope(SemaRef);
      Body = BaseTransform::RebuildCompoundStmt(Body->getBeginLoc(), Stmts,
                                                Body->getEndLoc(), false)
                 .getAs<CompoundStmt>();
      DS->setBody(Body);
    }
    return BaseTransform::TransformDoStmt(DS);
  }

  StmtResult TransformForStmt(ForStmt *S) {
    bool HasStatement = false;
    ForStmt *FS = S;
    Stmt *Body = FS->getBody();

    if (Body != nullptr)
      HasStatement = (hasAwaitExpr(Body) || hasReturnStmt(Body));
    if (HasStatement && !isRefactorStmt(Body)) {
      std::vector<Stmt *> Stmts;
      Stmts.push_back(Body);
      Sema::CompoundScopeRAII CompoundScope(SemaRef);
      Body = BaseTransform::RebuildCompoundStmt(Body->getBeginLoc(), Stmts,
                                                Body->getEndLoc(), false)
                 .getAs<CompoundStmt>();
      FS->setBody(Body);
    }
    return BaseTransform::TransformForStmt(FS);
  }

  StmtResult TransformCaseStmt(CaseStmt *S) {
    bool HasStatement = false;
    CaseStmt *CS = S;
    Stmt *SS = CS->getSubStmt();
    if (SS != nullptr) HasStatement = (hasAwaitExpr(SS) || hasReturnStmt(SS));
    if (HasStatement && !isRefactorStmt(SS)) {
      std::vector<Stmt *> Stmts;
      Stmts.push_back(SS);
      Sema::CompoundScopeRAII CompoundScope(SemaRef);
      SS = BaseTransform::RebuildCompoundStmt(SS->getBeginLoc(), Stmts,
                                              SS->getEndLoc(), false)
               .getAs<CompoundStmt>();
      CS->setSubStmt(SS);
    }
    return BaseTransform::TransformCaseStmt(CS);
  }

  StmtResult TransformDefaultStmt(DefaultStmt *S) {
    bool HasStatement = false;
    DefaultStmt *DS = S;
    Stmt *SS = DS->getSubStmt();
    if (SS != nullptr) HasStatement = (hasAwaitExpr(SS) || hasReturnStmt(SS));
    if (HasStatement && !isRefactorStmt(SS)) {
      std::vector<Stmt *> Stmts;
      Stmts.push_back(SS);
      Sema::CompoundScopeRAII CompoundScope(SemaRef);
      SS = BaseTransform::RebuildCompoundStmt(SS->getBeginLoc(), Stmts,
                                              SS->getEndLoc(), false)
               .getAs<CompoundStmt>();
      DS->setSubStmt(SS);
    }
    return BaseTransform::TransformDefaultStmt(DS);
  }

  StmtResult TransformCompoundStmt(CompoundStmt *S) {
    if (S == nullptr) return S;

    std::vector<Stmt *> Statements;
    for (auto *C : S->children()) {
      Stmt *SS = const_cast<Stmt *>(C);
      std::tuple<int, int> DRes = DT.GetDMap(SS);
      int DIndex = std::get<0>(DRes);
      if (DIndex != -1) {
        int DNum = std::get<1>(DRes);
        std::vector<Stmt *> DeclStmts = DT.GetDeclStmts();
        int size = DeclStmts.size();
        for (int i = DIndex; i < DIndex + DNum; i++) {
          if (i < size) Statements.push_back(DeclStmts[i]);
        }
      }
      SS = BaseTransform::TransformStmt(SS).getAs<Stmt>();
      Statements.push_back(SS);
    }
    Sema::CompoundScopeRAII CompoundScope(SemaRef);
    CompoundStmt *CS = BaseTransform::RebuildCompoundStmt(
                           S->getBeginLoc(), Statements, S->getEndLoc(), false)
                           .getAs<CompoundStmt>();
    return CS;
  }

  StmtResult TransformCompoundStmt(CompoundStmt *S, bool IsStmtExpr) {
    return S;
  }

  StmtResult TransformReturnStmt(ReturnStmt *S) {
    std::vector<Stmt *> ReturnStmts;
    Expr *RHSExpr = S->getRetValue();
    SourceLocation BLoc = FD->getBeginLoc();
    SourceLocation ELoc = FD->getEndLoc();

    RecordDecl::field_iterator FutureStateField = FutureRD->field_end();
    for (RecordDecl::field_iterator TheFieldIt = FutureRD->field_begin();
         TheFieldIt != FutureRD->field_end(); ++TheFieldIt) {
      if (TheFieldIt->getDeclName().getAsString() == "__future_state") {
        FutureStateField = TheFieldIt;
        break;
      }
    }
    if (FutureStateField != FutureRD->field_end()) {
      DeclarationName Name = FutureStateField->getDeclName();
      DeclarationNameInfo MemberNameInfo(Name, FutureStateField->getLocation());
      Expr *LHSExpr = SemaRef.BuildMemberExpr(
          PDRE, true, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *FutureStateField,
          DeclAccessPair::make(*FutureStateField,
                               FutureStateField->getAccess()),
          false, MemberNameInfo,
          FutureStateField->getType().getNonReferenceType(), VK_LValue,
          OK_Ordinary);

      llvm::APInt ResultVal(SemaRef.Context.getTargetInfo().getIntWidth(), 1);
      Expr *FutureStateVal = IntegerLiteral::Create(
          SemaRef.Context, ResultVal, SemaRef.Context.IntTy, SourceLocation());

      Expr *Unop =
          SemaRef
              .CreateBuiltinUnaryOp(SourceLocation(), UO_Minus, FutureStateVal)
              .get();
      Expr *BO =
          SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, LHSExpr, Unop)
              .get();
      ReturnStmts.push_back(BO);
    }

    std::string ResReturnName = "__RES_RETURN";
    // May have several return stmts in the same scope, eg.
    // @code
    // async f() {
    //    goto ERR;
    //    return 0;
    // ERR:
    //    return 0;
    // }
    // @endcode
    std::map<std::string, int>::iterator I =
        IdentifierNumber.find(ResReturnName);
    int ResReturnNum = I != IdentifierNumber.end() ? I->second : 0;
    IdentifierNumber[ResReturnName] = ResReturnNum + 1;
    if (ResReturnNum > 0) {
      ResReturnName = ResReturnName + "_" + std::to_string(ResReturnNum);
    }
    VarDecl *ResReturnVD =
        VarDecl::Create(SemaRef.Context, FD, SourceLocation(), SourceLocation(),
                        &(SemaRef.Context.Idents).get(ResReturnName),
                        RHSExpr->getType(), nullptr, SC_None);

    ResReturnVD->setInit(RHSExpr);
    DeclGroupRef ResReturnDG(ResReturnVD);
    DeclStmt *ResReturnDS = new (SemaRef.Context)
        DeclStmt(ResReturnDG, SourceLocation(), SourceLocation());
    ReturnStmts.push_back(ResReturnDS);

    Expr *ResExpr = SemaRef.BuildDeclRefExpr(
        ResReturnVD, ResReturnVD->getType(), VK_LValue, SourceLocation());

    ResExpr = ImplicitCastExpr::Create(SemaRef.Context, ResReturnVD->getType(),
                                       CK_LValueToRValue, ResExpr, nullptr,
                                       VK_PRValue, FPOptionsOverride());

    SmallVector<Expr *, 1> Args;
    Args.push_back(ResExpr);

    QualType PollResultType = FD->getReturnType();
    RecordDecl *PollResultRD = PollResultType->getAsRecordDecl();

    BSCMethodDecl *CompletedFD =
        lookupBSCMethodInRecord(SemaRef, "completed", PollResultRD);

    Expr *CompletedRef = SemaRef.BuildDeclRefExpr(
        CompletedFD, CompletedFD->getType(), VK_LValue, BLoc);
    CompletedRef->HasBSCScopeSpec = true;
    CompletedRef =
        SemaRef
            .ImpCastExprToType(
                CompletedRef,
                SemaRef.Context.getPointerType(CompletedRef->getType()),
                CK_FunctionToPointerDecay)
            .get();

    Expr *CE =
        SemaRef.BuildCallExpr(nullptr, CompletedRef, BLoc, Args, ELoc).get();
    Stmt *RS = SemaRef.BuildReturnStmt(BLoc, CE).get();
    ReturnStmts.push_back(RS);
    CompoundStmt *CS = CompoundStmt::Create(SemaRef.Context, ReturnStmts,
                                            FPOptionsOverride(), BLoc, ELoc);

    return CS;
  }
};
}  // namespace

namespace {
class TransformAEToCS : public TreeTransform<TransformAEToCS> {
  typedef TreeTransform<TransformAEToCS> BaseTransform;
  int AwaitIndex;
  std::vector<AwaitExpr *> AEStmts;
  std::vector<LabelDecl *> &LabelDecls;
  int AwaitCount = 0;

  ParmVarDecl *PVD;
  Expr *PDRE;
  RecordDecl *FutureRD;
  std::vector<Stmt *> AwaitStmts;
  BSCMethodDecl *FD;

public:
  TransformAEToCS(Sema &SemaRef, std::vector<LabelDecl *> &LabelDecls,
                  ParmVarDecl *PVD, Expr *PDRE, RecordDecl *FutureRD,
                  BSCMethodDecl *FD)
      : BaseTransform(SemaRef), LabelDecls(LabelDecls), PVD(PVD), PDRE(PDRE),
        FutureRD(FutureRD), FD(FD) {
    AwaitIndex = 1;
  }

  // make sure redo semantic analysis
  bool AlwaysRebuild() { return true; }

  ExprResult TransformAwaitExpr(AwaitExpr *E) {
    auto TransformedSE = BaseTransform::TransformExpr(E->getSubExpr());
    AwaitCount++;
    auto *AE = TransformedSE.get();
    RecordDecl::field_iterator FtField, FtFieldIt;
    for (FtFieldIt = FutureRD->field_begin();
         FtFieldIt != FutureRD->field_end(); ++FtFieldIt) {
      if (FtFieldIt->getDeclName().getAsString() ==
          "Ft_" + std::to_string(AwaitCount)) {
        FtField = FtFieldIt;
        break;
      }
    }

    Expr *LHSExpr = SemaRef.BuildMemberExpr(
        PDRE, true, SourceLocation(), NestedNameSpecifierLoc(),
        SourceLocation(), *FtField,
        DeclAccessPair::make(PVD, FtField->getAccess()), false,
        DeclarationNameInfo(), FtField->getType().getNonReferenceType(),
        VK_LValue, OK_Ordinary);
    Expr *RHSExpr = AE;

    bool IsOptimization = false;
    // Handle nested call
    if (CallExpr *CE = dyn_cast<CallExpr>(AE)) {
      FunctionDecl *FutureInitFunc = CE->getDirectCallee();
      if (FutureInitFunc) {
        IsOptimization =
            !(CE->getType().getTypePtr()->isBSCFutureType()) &&
            (implementedFutureType(SemaRef, CE->getType()) ||
             (isa<PointerType>(CE->getType().getTypePtr()) &&
              implementedFutureType(
                  SemaRef, cast<PointerType>(CE->getType().getTypePtr())
                               ->getPointeeType())));
      }
    }

    Expr *ResultStmt = SemaRef
                           .CreateBuiltinBinOp((*FtField)->getLocation(),
                                               BO_Assign, LHSExpr, RHSExpr)
                           .get();
    AwaitStmts.push_back(ResultStmt);

    Expr *PollFuncExpr = nullptr;
    RecordDecl *PollResultRD = nullptr;
    std::vector<Expr *> PollArgs;
    Stmt *ThenBodyStmt = nullptr;
    if (IsOptimization) {
      CallExpr *CE = dyn_cast<CallExpr>(AE);
      QualType AEType;
      if (CE) {
        // TODO: improve this
        FunctionDecl *AwaitFD = dyn_cast_or_null<FunctionDecl>(CE->getCalleeDecl());
        AEType =
            AwaitFD == nullptr
                ? AE->getType()
                : SemaRef.Context.getQualifiedType(
                      AE->getType(), AwaitFD->getReturnType().getQualifiers());
      } else AEType = AE->getType();

      // Remove all pointers from type
      while (AEType->isPointerType()) {
        AEType = AEType->getPointeeType();
      }

      const RecordType *FutureType = dyn_cast<RecordType>(
          AEType.getDesugaredType(SemaRef.Context));
      assert(FutureType != nullptr &&
             "struct future of async function is null");

      RecordDecl *FutureStructRD = FutureType->getDecl();
      assert(FutureStructRD != nullptr &&
             "struct future of async function is null");

      BSCMethodDecl *PollFD =
          lookupBSCMethodInRecord(SemaRef, "poll", FutureStructRD);
      assert(PollFD != nullptr && "poll function of async function is null");
      const RecordType *PollResultRT = dyn_cast<RecordType>(
          SemaRef.getASTContext().getCanonicalType(PollFD->getReturnType()));
      if (!PollResultRT)
        return ExprError();
      PollResultRD = cast<RecordDecl>(PollResultRT->getDecl());
      assert(PollResultRD != nullptr &&
             "the return type of poll function is null");
      QualType ParamType = SemaRef.getASTContext().getCanonicalType(
          PollFD->getParamDecl(0)->getType());

      DeclarationName Name = FtField->getDeclName();
      DeclarationNameInfo MemberNameInfo(Name, FtField->getLocation());
      Expr *FtExpr = SemaRef.BuildMemberExpr(
          PDRE, true, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *FtField,
          DeclAccessPair::make(*FtField, FtField->getAccess()), false,
          MemberNameInfo, FtField->getType().getNonReferenceType(),
          VK_LValue, OK_Ordinary);
      if (!FtField->getType()->isPointerType()) {
        // Only do it if it's not a pointer already
        FtExpr = UnaryOperator::Create(
            SemaRef.Context, FtExpr, UO_AddrOf, ParamType, VK_PRValue,
            OK_Ordinary, SourceLocation(), false, FPOptionsOverride());
      }
      PollArgs.push_back(FtExpr);

      PollFuncExpr = SemaRef.BuildDeclRefExpr(
          PollFD, PollFD->getType().getNonReferenceType(), VK_LValue,
          PollFD->getLocation());
      PollFuncExpr->HasBSCScopeSpec = true;
      PollFuncExpr = SemaRef
                         .ImpCastExprToType(
                             PollFuncExpr,
                             SemaRef.Context.getPointerType(PollFD->getType()),
                             CK_FunctionToPointerDecay)
                         .get();
      PollFuncExpr->HasBSCScopeSpec = true;

      BSCMethodDecl *FreeFD =
          lookupBSCMethodInRecord(SemaRef, "free", FutureStructRD);
      assert(FreeFD != nullptr && "free function of async function is null");

      Expr *FreeFuncExpr = SemaRef.BuildDeclRefExpr(
          FreeFD, FreeFD->getType().getNonReferenceType(), VK_LValue,
          E->getBeginLoc());
      FreeFuncExpr->HasBSCScopeSpec = true;
      std::vector<Expr *> FreeArgs{FtExpr};
      Expr *FreeFuncCall =
          SemaRef
              .BuildCallExpr(nullptr, FreeFuncExpr, SourceLocation(), FreeArgs,
                             SourceLocation())
              .get();

      ThenBodyStmt = FreeFuncCall;

      // If the future is a pointer type, we need to set it back to 0
      if (FtField->getType()->isPointerType()) {
        llvm::APInt ResultVal(SemaRef.Context.getTargetInfo().getIntWidth(), 0);
        Expr *IntegerExpr =
            IntegerLiteral::Create(SemaRef.Context, ResultVal,
                                   SemaRef.Context.IntTy, SourceLocation());

        Expr *RAssignExpr = CStyleCastExpr::Create(
            SemaRef.Context, FtField->getType(), VK_PRValue, CK_NullToPointer,
            IntegerExpr, nullptr, FPOptionsOverride(),
            SemaRef.Context.getTrivialTypeSourceInfo(FtField->getType(),
                                                     SourceLocation()),
            SourceLocation(), SourceLocation());
        Expr *NullptrAssign =
            SemaRef
                .BuildBinOp(nullptr, SourceLocation(), BO_Assign,
                            /* LHSExpr=*/FtExpr, /* RHSExpr=*/RAssignExpr)
                .get();

        SmallVector<Stmt *, 2> BodyStmts = {FreeFuncCall, NullptrAssign};
        ThenBodyStmt = CompoundStmt::Create(SemaRef.Context, BodyStmts,
                                            FPOptionsOverride(),
                                            SourceLocation(), SourceLocation());
      }

    } else {
      RecordDecl *FatPointerRD =
          dyn_cast<RecordType>(
              LHSExpr->getType().getDesugaredType(SemaRef.Context))
              ->getDecl();
      assert(isa<ClassTemplateSpecializationDecl>(FatPointerRD));
      ClassTemplateSpecializationDecl *CTSD =
          cast<ClassTemplateSpecializationDecl>(FatPointerRD);
      const TemplateArgumentList &args = CTSD->getTemplateArgs();
      assert(args.size() == 1);
      // Make sure these three generic types are fully instantiated.
      (void)lookupGenericType(SemaRef, FD->getBeginLoc(), args[0].getAsType(),
                              "PollResult");
      (void)lookupGenericType(SemaRef, FD->getBeginLoc(), args[0].getAsType(),
                              "__Trait_Future_Vtable");
      (void)lookupGenericType(SemaRef, FD->getBeginLoc(), args[0].getAsType(),
                              "__Trait_Future");
      RecordDecl::field_iterator PtrField, VtableField, FPFieldIt;
      for (FPFieldIt = FatPointerRD->field_begin();
          FPFieldIt != FatPointerRD->field_end(); ++FPFieldIt) {
        if (FPFieldIt->getDeclName().getAsString() == "data") {
          PtrField = FPFieldIt;
        } else if (FPFieldIt->getDeclName().getAsString() == "vtable") {
          VtableField = FPFieldIt;
        }
      }

      // this.Ft_<x>.vtable
      Expr *VtableExpr = SemaRef.BuildMemberExpr(
          LHSExpr, false, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *VtableField,
          DeclAccessPair::make(FatPointerRD, VtableField->getAccess()), false,
          DeclarationNameInfo(), VtableField->getType(), VK_LValue, OK_Ordinary);
      VtableExpr = ImplicitCastExpr::Create(
          SemaRef.Context, VtableExpr->getType(), CK_NoOp, VtableExpr, nullptr,
          VK_PRValue, FPOptionsOverride());

      RecordDecl::field_iterator PollFuncField, VtableFieldIt, FreeFuncField;
      const RecordType *RT = dyn_cast<RecordType>(
          VtableField->getType()->getPointeeType().getDesugaredType(
              SemaRef.Context));
      RecordDecl *VtableRD = RT->getDecl();
      for (VtableFieldIt = VtableRD->field_begin();
          VtableFieldIt != VtableRD->field_end(); ++VtableFieldIt) {
        if (VtableFieldIt->getDeclName().getAsString() == "poll") {
          PollFuncField = VtableFieldIt;
        }
        if (VtableFieldIt->getDeclName().getAsString() == "free") {
          FreeFuncField = VtableFieldIt;
        }
      }
      const FunctionType *FT = dyn_cast<FunctionType>(
          PollFuncField->getType()->getPointeeType().getDesugaredType(
              SemaRef.Context));
      const RecordType *PollResultRT = dyn_cast<RecordType>(
          SemaRef.Context.getCanonicalType(FT->getReturnType()));
      if (!PollResultRT)
        return ExprError();
      PollResultRD = cast<RecordDecl>(PollResultRT->getDecl());

      PollFuncExpr = SemaRef.BuildMemberExpr(
          VtableExpr, true, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *PollFuncField,
          DeclAccessPair::make(VtableRD, PollFuncField->getAccess()), false,
          DeclarationNameInfo(), PollFuncField->getType(), VK_LValue,
          OK_Ordinary);
      Expr *PtrExpr = SemaRef.BuildMemberExpr(
          LHSExpr, false, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *PtrField,
          DeclAccessPair::make(FatPointerRD, PtrField->getAccess()), false,
          DeclarationNameInfo(), PtrField->getType(), VK_LValue, OK_Ordinary);

      PollArgs.push_back(PtrExpr);
      Expr *FreeFuncExpr = SemaRef.BuildMemberExpr(
          VtableExpr, true, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *FreeFuncField,
          DeclAccessPair::make(VtableRD, FreeFuncField->getAccess()), false,
          DeclarationNameInfo(), FreeFuncField->getType(), VK_LValue,
          OK_Ordinary);
      ThenBodyStmt = buildIfStmtForFreeFutureObj(SemaRef, PtrExpr, FreeFuncExpr,
                                                 E->getBeginLoc());
    }
    Expr *PollFuncCall =
        SemaRef
            .BuildCallExpr(nullptr, PollFuncExpr, SourceLocation(), PollArgs,
                           SourceLocation())
            .get();

    RecordDecl::field_iterator ResField, FieldIt;
    for (FieldIt = PollResultRD->field_begin();
         FieldIt != PollResultRD->field_end(); ++FieldIt) {
      if (FieldIt->getDeclName().getAsString() == "res") {
        ResField = FieldIt;
        break;
      }
    }
    std::string AwaitResultVDName = "Res_" + std::to_string(AwaitCount);
    VarDecl *AwaitResultVD =
        VarDecl::Create(SemaRef.Context, FD, SourceLocation(), SourceLocation(),
                        &(SemaRef.Context.Idents).get(AwaitResultVDName),
                        ResField->getType(), nullptr, SC_None);

    DeclGroupRef AwaitResultDG(AwaitResultVD);
    DeclStmt *AwaitResultDS = new (SemaRef.Context)
        DeclStmt(AwaitResultDG, SourceLocation(), SourceLocation());

    Stmt::EmptyShell Empty;
    NullStmt *Null = new (SemaRef.Context) NullStmt(Empty);
    Null->setSemiLoc(SourceLocation());

    LabelStmt *LS =
        BaseTransform::RebuildLabelStmt(
            SourceLocation(), cast<LabelDecl>(LabelDecls[AwaitIndex++]),
            SourceLocation(), Null)
            .getAs<LabelStmt>();
    AwaitStmts.push_back(LS);

    AwaitStmts.push_back(AwaitResultDS);

    std::string PollResultVDName = "PR_" + std::to_string(AwaitCount);
    VarDecl *PollResultVD =
        VarDecl::Create(SemaRef.Context, FD, SourceLocation(), SourceLocation(),
                        &(SemaRef.Context.Idents).get(PollResultVDName),
                        PollFuncCall->getType(), nullptr, SC_None);

    PollResultVD->setInit(PollFuncCall);
    DeclGroupRef PollResultDG(PollResultVD);
    DeclStmt *PollResultDS = new (SemaRef.Context)
        DeclStmt(PollResultDG, SourceLocation(), SourceLocation());
    AwaitStmts.push_back(PollResultDS);

    IfStmt *If =
        processAwaitExprStatus(SemaRef, AwaitCount, FutureRD, PDRE, PVD,
                               PollResultVD, AwaitResultVD, FD, ThenBodyStmt);
    if (If != nullptr) AwaitStmts.push_back(If);

    Expr *AwaitResultRef = SemaRef.BuildDeclRefExpr(
        AwaitResultVD, AwaitResultVD->getType(), VK_LValue, SourceLocation());
    AwaitResultRef = ImplicitCastExpr::Create(
        SemaRef.Context, AwaitResultVD->getType(), CK_LValueToRValue,
        AwaitResultRef, nullptr, VK_PRValue, FPOptionsOverride());
    return AwaitResultRef;
  }

  Decl *TransformDefinition(SourceLocation Loc, Decl *D) {
    if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
      Expr *Init = const_cast<Expr *>(VD->getInit());
      bool HasAwait = hasAwaitExpr(Init);
      if (HasAwait && Init != nullptr) {
        Init = BaseTransform::TransformExpr(Init).get();

        VarDecl *NewVD = VarDecl::Create(
            SemaRef.Context, VD->getDeclContext(), VD->getBeginLoc(),
            VD->getEndLoc(), VD->getIdentifier(), VD->getType(),
            VD->getTypeSourceInfo(), VD->getStorageClass());

        SemaRef.AddInitializerToDecl(NewVD, Init, /*DirectInit=*/false);
        transformedLocalDecl(VD, NewVD);
        return NewVD;
      }
    }

    return BaseTransform::TransformDefinition(Loc, D);
  }

  StmtResult TransformCompoundStmt(CompoundStmt *S) {
    return this->TransformCompoundStmt(S, false);
  }

  StmtResult TransformCompoundStmt(CompoundStmt *S, bool IsStmtExpr) {
    if (S == nullptr)
      return S;

    std::vector<Stmt *> Statements;
    for (auto *SS : S->children()) {
      auto prevSize = AwaitStmts.size();
      SS = BaseTransform::TransformStmt(SS).getAs<Stmt>();
      for (size_t i = prevSize; i < AwaitStmts.size(); ++i) {
        Statements.push_back(AwaitStmts[i]);
      }
      AwaitStmts.resize(prevSize);
      Statements.push_back(SS);
    }
    Sema::CompoundScopeRAII CompoundScope(SemaRef);
    CompoundStmt *CS =
        BaseTransform::RebuildCompoundStmt(S->getBeginLoc(), Statements,
                                           S->getEndLoc(), IsStmtExpr)
            .getAs<CompoundStmt>();
    return CS;
  }
};
} // namespace

// Build only the declaration of the free function
static BSCMethodDecl *buildFreeFunctionDeclaration(Sema &S, RecordDecl *RD,
                                                   FunctionDecl *FD) {
  SourceLocation SLoc = FD->getBeginLoc();
  SourceLocation NLoc = FD->getNameInfo().getLoc();
  SourceLocation ELoc = FD->getEndLoc();

  std::string FName = "free";
  QualType FuncRetType = S.Context.VoidTy;
  QualType ParamType = S.Context.getPointerType(S.Context.getRecordType(RD));
  SmallVector<QualType, 1> ParamTys;
  ParamTys.push_back(ParamType);

  QualType FuncType = S.Context.getFunctionType(FuncRetType, ParamTys, {});

  BSCMethodDecl *NewFD = buildAsyncBSCMethodDecl(
      S.Context, RD, SLoc, NLoc, ELoc, &(S.Context.Idents).get(FName), FuncType,
      nullptr, SC_None, RD->getTypeForDecl()->getCanonicalTypeInternal());
  NewFD->setLexicalDeclContext(S.Context.getTranslationUnitDecl());
  S.PushOnScopeChains(NewFD, S.getCurScope(), true);
  S.Context.BSCDeclContextMap[RD->getTypeForDecl()] = RD;

  SmallVector<ParmVarDecl *, 1> ParmVarDecls;
  ParmVarDecl *PVD = ParmVarDecl::Create(
      S.Context, NewFD, SourceLocation(), SourceLocation(),
      &(S.Context.Idents).get("this"), ParamType, nullptr, SC_None, nullptr);
  ParmVarDecls.push_back(PVD);
  NewFD->setParams(ParmVarDecls);
  S.PushFunctionScope();

  sema::AnalysisBasedWarnings::Policy *ActivePolicy = nullptr;
  S.PopFunctionScopeInfo(ActivePolicy, NewFD, QualType());
  return NewFD;
}

static BSCMethodDecl *buildFreeFunctionDefinition(Sema &S, RecordDecl *RD,
                                                  FunctionDecl *FD,
                                                  bool IsOptimization) {
  SourceLocation SLoc = FD->getBeginLoc();
  SourceLocation NLoc = FD->getNameInfo().getLoc();
  SourceLocation ELoc = FD->getEndLoc();

  std::string FName = "free";
  QualType FuncRetType = S.Context.VoidTy;
  QualType ParamType = S.Context.getPointerType(S.Context.getRecordType(RD));
  SmallVector<QualType, 1> ParamTys;
  ParamTys.push_back(ParamType);

  QualType FuncType = S.Context.getFunctionType(FuncRetType, ParamTys, {});

  BSCMethodDecl *NewFD = buildAsyncBSCMethodDecl(
      S.Context, RD, SLoc, NLoc, ELoc, &(S.Context.Idents).get(FName), FuncType,
      nullptr, SC_None, RD->getTypeForDecl()->getCanonicalTypeInternal());
  NewFD->setLexicalDeclContext(S.Context.getTranslationUnitDecl());
  S.PushOnScopeChains(NewFD, S.getCurScope(), true);

  S.Context.BSCDeclContextMap[RD->getTypeForDecl()] = RD;

  SmallVector<ParmVarDecl *, 1> ParmVarDecls;
  ParmVarDecl *PVD = ParmVarDecl::Create(
      S.Context, NewFD, SourceLocation(), SourceLocation(),
      &(S.Context.Idents).get("this"), ParamType, nullptr, SC_None, nullptr);
  ParmVarDecls.push_back(PVD);
  NewFD->setParams(ParmVarDecls);
  S.PushFunctionScope();

  std::vector<Stmt *> Stmts;

  auto StartsWith = [](const std::string &str, const std::string &prefix) {
    if (str.length() >= prefix.length()) {
      return 0 == str.compare(0, prefix.length(), prefix);
    }
    return false;
  };

  std::stack<RecordDecl::field_iterator> Futures;
  for (RecordDecl::field_iterator FieldIt = RD->field_begin();
       FieldIt != RD->field_end(); ++FieldIt) {
    if (StartsWith(FieldIt->getDeclName().getAsString(), "Ft_")) {
      auto FieldTy = FieldIt->getType();
      auto maybePtrTy = dyn_cast<PointerType>(FieldTy.getTypePtr());
      // Free trait pointer or known pointers
      if (FieldTy.getTypePtr()->isBSCFutureType() ||
          (maybePtrTy &&
            implementedFutureType(
            S, maybePtrTy->getPointeeType())) ||
          (maybePtrTy &&
            isa<RecordType>(maybePtrTy->getPointeeType()) &&
            cast<RecordType>(maybePtrTy->getPointeeType())->getDecl() == RD)) {
              Futures.push(FieldIt);
            }
    }
  }

  while (!Futures.empty()) {
    RecordDecl::field_iterator FtField = Futures.top();
    Futures.pop();
    Expr *DataExpr = nullptr;
    Expr *FreeFuncExpr = nullptr;
    if (FtField->getType().getTypePtr()->isBSCFutureType()) {
      // If its BSCFutureType (the trait) call the free function from the vtable
      RecordDecl *FatPointerRD =
          dyn_cast<RecordType>(FtField->getType().getDesugaredType(S.Context))
              ->getDecl();

      assert(isa<ClassTemplateSpecializationDecl>(FatPointerRD));
      ClassTemplateSpecializationDecl *CTSD =
          cast<ClassTemplateSpecializationDecl>(FatPointerRD);
      const TemplateArgumentList &args = CTSD->getTemplateArgs();
      assert(args.size() == 1);

      // Make sure these three generic types are fully instantiated.
      (void)lookupGenericType(S, FD->getBeginLoc(), args[0].getAsType(),
                              "PollResult");
      (void)lookupGenericType(S, FD->getBeginLoc(), args[0].getAsType(),
                              "__Trait_Future_Vtable");
      (void)lookupGenericType(S, FD->getBeginLoc(), args[0].getAsType(),
                              "__Trait_Future");

      Expr *DRE =
          S.BuildDeclRefExpr(PVD, ParamType, VK_LValue, SourceLocation());
      Expr *PDRE =
          ImplicitCastExpr::Create(S.Context, ParamType, CK_LValueToRValue, DRE,
                                   nullptr, VK_PRValue, FPOptionsOverride());

      // Generating `FutureExpr` as followed:
      // @code
      // this.Ft_<x>
      // @endcode
      Expr *FutureExpr = S.BuildMemberExpr(
          PDRE, true, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *FtField,
          DeclAccessPair::make(PVD, FtField->getAccess()), false,
          DeclarationNameInfo(), FtField->getType().getNonReferenceType(),
          VK_LValue, OK_Ordinary);

      RecordDecl::field_iterator PtrField, VtableField, FPFieldIt;
      for (FPFieldIt = FatPointerRD->field_begin();
           FPFieldIt != FatPointerRD->field_end(); ++FPFieldIt) {
        if (FPFieldIt->getDeclName().getAsString() == "data") {
          PtrField = FPFieldIt;
        } else if (FPFieldIt->getDeclName().getAsString() == "vtable") {
          VtableField = FPFieldIt;
        }
      }

      // Generating `VtableExpr` as followed:
      // @code
      // this.Ft_<x>.vtable
      // @endcode
      Expr *VtableExpr = S.BuildMemberExpr(
          FutureExpr, false, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *VtableField,
          DeclAccessPair::make(FatPointerRD, VtableField->getAccess()), false,
          DeclarationNameInfo(), VtableField->getType(), VK_LValue,
          OK_Ordinary);
      VtableExpr = ImplicitCastExpr::Create(S.Context, VtableExpr->getType(),
                                            CK_NoOp, VtableExpr, nullptr,
                                            VK_PRValue, FPOptionsOverride());

      RecordDecl::field_iterator FreeFuncField, FreeFuncFieldIt;
      const RecordType *RT = dyn_cast<RecordType>(
          VtableField->getType()->getPointeeType().getDesugaredType(S.Context));
      RecordDecl *VtableRD = RT->getDecl();
      for (FreeFuncFieldIt = VtableRD->field_begin();
           FreeFuncFieldIt != VtableRD->field_end(); ++FreeFuncFieldIt) {
        if (FreeFuncFieldIt->getDeclName().getAsString() == "free") {
          FreeFuncField = FreeFuncFieldIt;
        }
      }

      DataExpr = S.BuildMemberExpr(
          FutureExpr, false, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *PtrField,
          DeclAccessPair::make(FatPointerRD, PtrField->getAccess()), false,
          DeclarationNameInfo(), PtrField->getType(), VK_LValue, OK_Ordinary);

      FreeFuncExpr = S.BuildMemberExpr(
          VtableExpr, true, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *FreeFuncField,
          DeclAccessPair::make(FatPointerRD, FreeFuncField->getAccess()), false,
          DeclarationNameInfo(), FreeFuncField->getType(), VK_LValue,
          OK_Ordinary);
    } else {
      // If not, the free function can be obtained directly
      auto AEType = FtField->getType();

      // Remove all pointers from type
      while (AEType->isPointerType()) {
        AEType = AEType->getPointeeType();
      }

      const RecordType *FutureType =
          dyn_cast<RecordType>(AEType.getDesugaredType(S.Context));
      assert(FutureType != nullptr &&
             "struct future of async function is null");

      RecordDecl *FutureStructRD = FutureType->getDecl();
      assert(FutureStructRD != nullptr &&
             "struct future of async function is null");

      BSCMethodDecl *FreeFD =
          lookupBSCMethodInRecord(S, "free", FutureStructRD);
      assert(FreeFD != nullptr && "free function of async function is null");

      FreeFuncExpr = S.BuildDeclRefExpr(
          FreeFD, FreeFD->getType().getNonReferenceType(), VK_LValue, SLoc);

      FreeFuncExpr->HasBSCScopeSpec = true;

      Expr *DRE =
          S.BuildDeclRefExpr(PVD, ParamType, VK_LValue, SourceLocation());
      Expr *PDRE =
          ImplicitCastExpr::Create(S.Context, ParamType, CK_LValueToRValue, DRE,
                                   nullptr, VK_PRValue, FPOptionsOverride());

      DataExpr = S.BuildMemberExpr(
          PDRE, true, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *FtField,
          DeclAccessPair::make(PVD, FtField->getAccess()), false,
          DeclarationNameInfo(), FtField->getType().getNonReferenceType(),
          VK_LValue, OK_Ordinary);
    }
    assert(DataExpr);
    assert(FreeFuncExpr);
    Stmt *If = buildIfStmtForFreeFutureObj(S, DataExpr, FreeFuncExpr);
    Stmts.push_back(If);
  }

  if (!IsOptimization) {
    std::string FreeName = "free";
    LookupResult ExitPrevious(S, &S.Context.Idents.get(FreeName),
                              SourceLocation(), S.LookupOrdinaryName,
                              S.NotForRedeclaration);
    S.LookupName(ExitPrevious, S.TUScope);
    FunctionDecl *FreeFunc = nullptr;
    for (LookupResult::iterator I = ExitPrevious.begin(),
                                E = ExitPrevious.end();
         I != E; ++I) {
      if ((FreeFunc = dyn_cast<FunctionDecl>(*I))) {
        break;
      }
    }
    if (!FreeFunc) {
      S.Diag(FD->getBeginLoc(), diag::err_function_not_found)
          << FreeName << "<stdlib.h>";
      return nullptr;
    }

    Expr *FreeFuncRef =
        S.BuildDeclRefExpr(FreeFunc, FreeFunc->getType(), VK_LValue, NLoc);
    Expr *FutureObj =
        S.BuildDeclRefExpr(PVD, ParamType, VK_LValue, SourceLocation());
    Stmt *If = buildIfStmtForFreeFutureObj(S, FutureObj, FreeFuncRef);
    Stmts.push_back(If);
  }

  CompoundStmt *CS =
      CompoundStmt::Create(S.Context, Stmts, FPOptionsOverride(), SLoc, ELoc);
  NewFD->setBody(CS);
  sema::AnalysisBasedWarnings::Policy *ActivePolicy = nullptr;
  S.PopFunctionScopeInfo(ActivePolicy, NewFD, QualType());

  return NewFD;
}

static BSCMethodDecl *buildPollFunctionDeclaration(Sema &S, RecordDecl *RD,
                                                   RecordDecl *PollResultRD,
                                                   FunctionDecl *FD) {
  SourceLocation SLoc = FD->getBeginLoc();
  SourceLocation NLoc = FD->getNameInfo().getLoc();
  SourceLocation ELoc = FD->getEndLoc();
  QualType Ty = FD->getDeclaredReturnType();

  S.PushFunctionScope();
  FunctionDecl *TransformedFD = nullptr;
  {
    Sema::ContextRAII savedContext(S, FD);
    LocalInstantiationScope Scope(S);

    TransformedFD = TransformToReturnVoid(S).TransformFunctionDecl(FD);
  }
  S.PopFunctionScopeInfo();

  std::string FName = "poll";

  QualType FuncRetType = S.Context.getRecordType(PollResultRD);
  QualType ParamType = S.Context.getPointerType(S.Context.getRecordType(RD));
  SmallVector<QualType, 1> ParamTys;
  ParamTys.push_back(ParamType);

  QualType FuncType = S.Context.getFunctionType(FuncRetType, ParamTys, {});
  QualType OriginType = S.Context.getFunctionType(Ty, ParamTys, {});

  BSCMethodDecl *NewFD = buildAsyncBSCMethodDecl(
      S.Context, RD, SLoc, NLoc, ELoc, &(S.Context.Idents).get(FName),
      OriginType, nullptr, SC_None,
      RD->getTypeForDecl()->getCanonicalTypeInternal());
  NewFD->setLexicalDeclContext(S.Context.getTranslationUnitDecl());
  S.PushOnScopeChains(NewFD, S.getCurScope(), true);

  // Enter the scope of this instantiation. We don't use
  // PushDeclContext because we don't have a scope.
  Sema::ContextRAII savedContext(S, NewFD);
  LocalInstantiationScope Scope(S);

  S.Context.BSCDeclContextMap[RD->getTypeForDecl()] = RD;
  S.PushFunctionScope();

  SmallVector<ParmVarDecl *, 1> ParmVarDecls;
  ParmVarDecl *PVD = ParmVarDecl::Create(
      S.Context, NewFD, SourceLocation(), SourceLocation(),
      &(S.Context.Idents).get("this"), ParamType, nullptr, SC_None, nullptr);
  ParmVarDecls.push_back(PVD);
  NewFD->setParams(ParmVarDecls);

  NewFD->setType(
      S.Context.getFunctionType(TransformedFD->getReturnType(), ParamTys, {}));

  NewFD->setType(FuncType);

  sema::AnalysisBasedWarnings::Policy *ActivePolicy = nullptr;
  S.PopFunctionScopeInfo(ActivePolicy, NewFD, QualType());
  return NewFD->isInvalidDecl() ? nullptr : NewFD;
}

static BSCMethodDecl *buildPollFunctionDefinition(Sema &S, RecordDecl *RD,
                                                  RecordDecl *PollResultRD,
                                                  FunctionDecl *FD,
                                                  RecordDecl *FatPointerRD,
                                                  int FutureStateNumber) {
  SourceLocation SLoc = FD->getBeginLoc();
  SourceLocation NLoc = FD->getNameInfo().getLoc();
  SourceLocation ELoc = FD->getEndLoc();
  QualType Ty = FD->getDeclaredReturnType();

  S.PushFunctionScope();

  // Enter the scope of this instantiation. We don't use
  // PushDeclContext because we don't have a scope.
  FunctionDecl *TransformedFD = nullptr;
  {
    Sema::ContextRAII savedContext(S, FD);
    LocalInstantiationScope Scope(S);

    TransformedFD = TransformToReturnVoid(S).TransformFunctionDecl(FD);
  }
  S.PopFunctionScopeInfo();

  std::string FName = "poll";

  QualType FuncRetType = S.Context.getRecordType(PollResultRD);
  QualType ParamType = S.Context.getPointerType(S.Context.getRecordType(RD));
  SmallVector<QualType, 1> ParamTys;
  ParamTys.push_back(ParamType);

  QualType FuncType = S.Context.getFunctionType(FuncRetType, ParamTys, {});
  QualType OriginType = S.Context.getFunctionType(Ty, ParamTys, {});

  BSCMethodDecl *NewFD = buildAsyncBSCMethodDecl(
      S.Context, RD, SLoc, NLoc, ELoc, &(S.Context.Idents).get(FName),
      OriginType, nullptr, SC_None,
      RD->getTypeForDecl()->getCanonicalTypeInternal());
  NewFD->setLexicalDeclContext(S.Context.getTranslationUnitDecl());
  S.PushOnScopeChains(NewFD, S.getCurScope(), true);

  // Enter the scope of this instantiation. We don't use
  // PushDeclContext because we don't have a scope.
  Sema::ContextRAII savedContext(S, NewFD);
  LocalInstantiationScope Scope(S);

  S.Context.BSCDeclContextMap[RD->getTypeForDecl()] = RD;
  S.PushFunctionScope();

  SmallVector<ParmVarDecl *, 1> ParmVarDecls;
  ParmVarDecl *PVD = ParmVarDecl::Create(
      S.Context, NewFD, SourceLocation(), SourceLocation(),
      &(S.Context.Idents).get("this"), ParamType, nullptr, SC_None, nullptr);
  ParmVarDecls.push_back(PVD);
  NewFD->setParams(ParmVarDecls);

  std::vector<Stmt *> Stmts;

  RecordDecl::field_iterator FutureStateField;
  for (RecordDecl::field_iterator FieldIt = RD->field_begin();
       FieldIt != RD->field_end(); ++FieldIt) {
    if (FieldIt->getDeclName().getAsString() == "__future_state") {
      FutureStateField = FieldIt;
    }
  }

  Expr *FutureObj =
      S.BuildDeclRefExpr(PVD, ParamType, VK_LValue, SourceLocation());
  FutureObj = ImplicitCastExpr::Create(S.Context, ParamType, CK_LValueToRValue,
                                       FutureObj, nullptr, VK_PRValue,
                                       FPOptionsOverride());
  // Creating Switch Stmt
  DeclarationName FutureName = FutureStateField->getDeclName();
  DeclarationNameInfo MemberNameInfo(FutureName,
                                     FutureStateField->getLocation());

  // @code
  // this.__future_state
  // @endcode
  Expr *ConditionVariable = S.BuildMemberExpr(
      FutureObj, true, SourceLocation(), NestedNameSpecifierLoc(),
      SourceLocation(), *FutureStateField,
      DeclAccessPair::make(*FutureStateField, FutureStateField->getAccess()),
      false, MemberNameInfo, FutureStateField->getType().getNonReferenceType(),
      VK_LValue, OK_Ordinary);
  S.setFunctionHasBranchIntoScope();
  SwitchStmt *SS =
      SwitchStmt::Create(S.Context, nullptr, nullptr, ConditionVariable,
                         SourceLocation(), SourceLocation());

  S.getCurFunction()->SwitchStack.push_back(
      FunctionScopeInfo::SwitchInfo(SS, false));

  std::vector<Stmt *> CaseStmts;
  std::vector<LabelDecl *> LabelDecls;
  for (int i = 0; i < FutureStateNumber; i++) {
    llvm::APInt ResultVal(S.Context.getTargetInfo().getIntWidth(), i);
    Expr *LHSExpr = IntegerLiteral::Create(S.Context, ResultVal,
                                           S.Context.IntTy, SourceLocation());
    CaseStmt *CS =
        CaseStmt::Create(S.Context, LHSExpr, nullptr, SourceLocation(),
                         SourceLocation(), SourceLocation());
    LabelDecl *LD =
        LabelDecl::Create(S.Context, NewFD, SourceLocation(),
                          &(S.Context.Idents).get("__L" + std::to_string(i)));
    LabelDecls.push_back(LD);
    Stmt *RHSExpr =
        new (S.Context) GotoStmt(LD, SourceLocation(), SourceLocation());
    CS->setSubStmt(RHSExpr);
    CaseStmts.push_back(CS);
    SS->addSwitchCase(CS);
  }

  CompoundStmt *SSBody = CompoundStmt::Create(S.Context, CaseStmts,
                                              FPOptionsOverride(), SLoc, ELoc);

  SS->setBody(SSBody);
  Stmts.push_back(SS);

  int StmtSize = Stmts.size();

  NewFD->setType(
      S.Context.getFunctionType(TransformedFD->getReturnType(), ParamTys, {}));

  TransformToAP DT = TransformToAP(S, FutureObj, RD, NewFD);
  StmtResult MemberChangeRes = DT.TransformStmt(TransformedFD->getBody());
  Stmt *FuncBody = MemberChangeRes.get();

  NewFD->setType(FuncType);

  StmtResult SingleStateRes =
      TransformToHasSingleStateAndReturnStatements(S, DT, FutureObj, RD, NewFD)
          .TransformStmt(FuncBody);
  FuncBody = SingleStateRes.get();

  StmtResult AEToCSRes = TransformAEToCS(S, LabelDecls, NewFD->getParamDecl(0),
                                         FutureObj, RD, NewFD)
                             .TransformStmt(FuncBody);

  for (auto *C : AEToCSRes.get()->children()) {
    Stmts.push_back(C);
  }
  Stmt::EmptyShell Empty;
  NullStmt *Null = new (S.Context) NullStmt(Empty);
  Null->setSemiLoc(SourceLocation());
  int CurStmtSize = Stmts.size();
  if (CurStmtSize > StmtSize) {
    LabelStmt *LS =
        new (S.Context) LabelStmt(SourceLocation(), LabelDecls[0], Null);
    Stmts.insert(Stmts.begin() + StmtSize, LS);
  } else {
    LabelStmt *LS =
        new (S.Context) LabelStmt(SourceLocation(), LabelDecls[0], Null);
    Stmts.push_back(LS);
  }

  CompoundStmt *CS =
      CompoundStmt::Create(S.Context, Stmts, FPOptionsOverride(), SLoc, ELoc);
  NewFD->setBody(CS);
  sema::AnalysisBasedWarnings::Policy *ActivePolicy = nullptr;
  S.PopFunctionScopeInfo(ActivePolicy, NewFD, QualType());
  return NewFD->isInvalidDecl() ? nullptr : NewFD;
}

// BSC extensions for await keyword
ExprResult Sema::BuildAwaitExpr(SourceLocation AwaitLoc, Expr *E) {
  assert(E && "null expression");

  if (E->getType()->isDependentType()) {
    Expr *Res = new (Context) AwaitExpr(AwaitLoc, E, Context.DependentTy);
    return Res;
  }

  QualType AwaitReturnTy = E->getType();

  bool IsCall = isa<CallExpr>(E);
  if (IsCall) {
    Decl *AwaitDecl = (dyn_cast<CallExpr>(E))->getCalleeDecl();
    FunctionDecl *FDecl = dyn_cast_or_null<FunctionDecl>(AwaitDecl);
    if (FDecl) {
      if (!FDecl->isAsyncSpecified() &&
              !IsBSCCompatibleFutureType(AwaitReturnTy)) {
        Diag(E->getExprLoc(), PDiag(diag::err_not_a_async_call)
                                  << getExprRange(E));
        return ExprError();
      }
    } else {
      if (!IsBSCCompatibleFutureType(AwaitReturnTy)) {
        Diag(E->getExprLoc(), PDiag(diag::err_not_a_async_call)
                                  << getExprRange(E));
        return ExprError();
      }
    }
  } else {
    if (!IsBSCCompatibleFutureType(AwaitReturnTy)) {
      Diag(E->getExprLoc(), PDiag(diag::err_not_a_async_call)
                                << getExprRange(E));
      return ExprError();
    }
  }

  if (AwaitReturnTy.getTypePtr()->isBSCFutureType()) {
    const RecordType *FatPointerType =
        dyn_cast<RecordType>(AwaitReturnTy.getDesugaredType(Context));
    if (!FatPointerType)
      return ExprError();
    RecordDecl *FatPointer = FatPointerType->getDecl();
    assert(isa<ClassTemplateSpecializationDecl>(FatPointer));
    ClassTemplateSpecializationDecl *CTSD =
        cast<ClassTemplateSpecializationDecl>(FatPointer);
    const TemplateArgumentList &args = CTSD->getTemplateArgs();
    assert(args.size() == 1);
    AwaitReturnTy = args[0].getAsType();
  } else if (implementedFutureType(*this, AwaitReturnTy)) {
    const RecordType *FutureType =
        dyn_cast<RecordType>(AwaitReturnTy.getDesugaredType(Context));
    if (!FutureType)
      return ExprError();
    RecordDecl *FutureRD = FutureType->getDecl();

    BSCMethodDecl *PollFD = lookupBSCMethodInRecord(*this, "poll", FutureRD);
    if (PollFD != nullptr) {
      const RecordType *PollResultType = dyn_cast<RecordType>(
          PollFD->getReturnType().getDesugaredType(Context));
      if (PollResultType) {
        RecordDecl *PollResult = PollResultType->getDecl();
        for (RecordDecl::field_iterator FieldIt = PollResult->field_begin(),
                                        Field_end = PollResult->field_end();
             FieldIt != Field_end; ++FieldIt) {
          if (FieldIt->getDeclName().getAsString() == "res") {
            AwaitReturnTy = FieldIt->getType();
          }
        }
      }
    }
  } else if ((isa<PointerType>(AwaitReturnTy.getTypePtr()) &&
              implementedFutureType(
                  *this, cast<PointerType>(AwaitReturnTy.getTypePtr())
                             ->getPointeeType()))) {
    auto AwaitReturnTy2 =
        cast<PointerType>(AwaitReturnTy.getTypePtr())->getPointeeType();
    const RecordType *FutureType =
        dyn_cast<RecordType>(AwaitReturnTy2.getDesugaredType(Context));
    if (!FutureType)
      return ExprError();
    RecordDecl *FutureRD = FutureType->getDecl();

    BSCMethodDecl *PollFD = lookupBSCMethodInRecord(*this, "poll", FutureRD);
    if (PollFD != nullptr) {
      const RecordType *PollResultType = dyn_cast<RecordType>(
          PollFD->getReturnType().getDesugaredType(Context));
      if (PollResultType) {
        RecordDecl *PollResult = PollResultType->getDecl();
        for (RecordDecl::field_iterator FieldIt = PollResult->field_begin(),
                                        Field_end = PollResult->field_end();
             FieldIt != Field_end; ++FieldIt) {
          if (FieldIt->getDeclName().getAsString() == "res") {
            AwaitReturnTy = FieldIt->getType();
          }
        }
      }
    }
  }

  // build AwaitExpr
  AwaitExpr *Res = new (Context) AwaitExpr(AwaitLoc, E, AwaitReturnTy);
  return Res;
}

ExprResult Sema::ActOnAwaitExpr(SourceLocation AwaitLoc, Expr *E) {
  if (!getLangOpts().BSCExperimental)
    Diag(AwaitLoc, diag::err_bsc_exp_sema) << "coroutine";
  if (FunctionScopes.size() < 1 ||
      getCurFunction()->CompoundScopes.size() < 1) {
    Diag(AwaitLoc, diag::err_await_invalid_scope) << "this scope";
    return ExprError();
  }

  // Correct typos for await expr.
  ExprResult CorrectVal =
      CorrectDelayedTyposInExpr(E, nullptr, /*RecoverUncorrectedTypos=*/true);
  if (CorrectVal.isInvalid())
    return ExprError();
  E = CorrectVal.get();
  return BuildAwaitExpr(AwaitLoc, E);
}

SmallVector<Decl *, 8> Sema::ActOnAsyncFunctionDeclaration(FunctionDecl *FD) {
  if (FD->isAsyncSpecified() && !getLangOpts().BSCExperimental)
    Diag(FD->getBeginLoc(), diag::err_bsc_exp_sema) << "coroutine";
  SmallVector<Decl *, 8> Decls;
  if (!IsBSCCompatibleFutureType(FD->getReturnType())) {
    QualType ReturnTy = FD->getReturnType();
    ReturnTy.removeLocalConst(); // TODO: need to reconsider.
    if (ReturnTy->isVoidType()) {
      std::pair<RecordDecl *, bool> VoidRD =
          generateVoidStruct(*this, FD->getBeginLoc(), FD->getEndLoc());
      if (!std::get<1>(VoidRD)) {
        Decls.push_back(std::get<0>(VoidRD));
        Context.BSCDesugaredMap[FD].push_back(std::get<0>(VoidRD));
      }
      ReturnTy = Context.getRecordType(std::get<0>(VoidRD));
    }
    QualType PollResultType =
        lookupGenericType(*this, FD->getBeginLoc(), ReturnTy, "PollResult");
    if (PollResultType.isNull()) {
      return Decls;
    }
    QualType VtableType = lookupGenericType(*this, FD->getBeginLoc(), ReturnTy,
                                            "__Trait_Future_Vtable");
    if (VtableType.isNull()) {
      return Decls;
    }
    QualType FatPointerType =
        lookupGenericType(*this, FD->getBeginLoc(), ReturnTy, "__Trait_Future");
    if (FatPointerType.isNull()) {
      return Decls;
    }
    RecordDecl *RD = buildOpaqueFutureRecordDecl(*this, FD);

    Decls.push_back(RD);
    Context.BSCDesugaredMap[FD].push_back(RD);

    QualType PointerStructTy =
        Context.getPointerType(Context.getRecordType(RD));

    RecordDecl *PollResultRD = PollResultType->getAsRecordDecl();

    buildPollFunctionDeclaration(*this, RD, PollResultRD, FD);
    buildFreeFunctionDeclaration(*this, RD, FD);

    VarDecl *VtableDecl = buildVtableInitDecl(
        *this, FD, Context.getRecordType(RD), ReturnTy, false);
    if (!VtableDecl) {
      return Decls;
    }
    Decls.push_back(VtableDecl);
    Context.BSCDesugaredMap[FD].push_back(VtableDecl);

    if (!FD->isStatic()) {
      FunctionDecl *FutureInitDef =
          buildFutureInitFunctionDeclaration(*this, FD, PointerStructTy);
      if (FutureInitDef) {
        Decls.push_back(FutureInitDef);
        Context.BSCDesugaredMap[FD].push_back(FutureInitDef);
      }
    }
  }
  return Decls;
}

SmallVector<Decl *, 8> Sema::ActOnAsyncFunctionDefinition(FunctionDecl *FD) {
  if (FD->isAsyncSpecified() && !getLangOpts().BSCExperimental)
    Diag(FD->getBeginLoc(), diag::err_bsc_exp_sema) << "coroutine";
  SmallVector<Decl *, 8> Decls;
  Decls.push_back(FD);
  // clangd does not analyze the function bodies of functions defined in header
  // files, such as inline functions
  if (!FD->getBody()) {
    return Decls;
  }
  AwaitExprFinder AwaitFinder = AwaitExprFinder();
  AwaitFinder.Visit(FD->getBody());

  // Report if await expression appear in non-async functions.
  if (!FD->isAsyncSpecified()) {
    if (AwaitFinder.GetAwaitExprNum() != 0) {
      Diag(FD->getBeginLoc(), diag::err_await_invalid_scope)
          << "non-_Async function";
    }
    return Decls;
  }

  // For leaf nodes, should not be modified async.
  if (IsBSCCompatibleFutureType(FD->getReturnType())) {
    Diag(FD->getBeginLoc(), diag::err_invalid_async_function);
    return Decls;
  }

  // Do not process desugar if we already met errors.
  if (Diags.hasErrorOccurred()) {
    return Decls;
  }

  IllegalAEFinder IAEFinder = IllegalAEFinder(*this);
  IAEFinder.Visit(FD->getBody());
  if (IAEFinder.HasIllegalAwaitExpr()) return Decls;

  QualType ReturnTy = FD->getReturnType();
  ReturnTy.removeLocalConst();
  if (ReturnTy->isVoidType()) {
    std::pair<RecordDecl *, bool> VoidRD =
        generateVoidStruct(*this, FD->getBeginLoc(), FD->getEndLoc());
    if (!std::get<1>(VoidRD)) {
      Decls.push_back(std::get<0>(VoidRD));
      Context.BSCDesugaredMap[FD].push_back(std::get<0>(VoidRD));
    }
    ReturnTy = Context.getRecordType(std::get<0>(VoidRD));
  }
  QualType PollResultType =
      lookupGenericType(*this, FD->getBeginLoc(), ReturnTy, "PollResult");
  if (PollResultType.isNull()) {
    return Decls;
  }
  RecordDecl *PollResultRD = PollResultType->getAsRecordDecl();
  QualType VtableType = lookupGenericType(*this, FD->getBeginLoc(), ReturnTy,
                                          "__Trait_Future_Vtable");
  if (VtableType.isNull()) {
    return Decls;
  }

  QualType FatPointerType =
      lookupGenericType(*this, FD->getBeginLoc(), ReturnTy, "__Trait_Future");
  if (FatPointerType.isNull()) {
    return Decls;
  }

  RecordDecl *FatPointerRD = FatPointerType->getAsRecordDecl();

  LocalVarFinder VarFinder = LocalVarFinder(Context);
  VarFinder.Visit(FD->getBody());

  bool IsRecursiveCall = RecursiveCallVisitor(FD).VisitStmt(FD->getBody());
  bool IsOptimization = FD->isStatic() && !IsRecursiveCall;
  RecordDecl *RD = buildFutureRecordDecl(*this, FD, AwaitFinder.GetAwaitExpr(),
                                         VarFinder.GetLocalVarList());
  auto RDType = Context.getRecordType(RD);
  QualType PointerStructTy = Context.getPointerType(RDType);

  if (!RD) {
    return Decls;
  }
  // Handle declaration first.
  FunctionDecl *FutureInitDef = buildFutureInitFunctionDeclaration(
      *this, FD, IsOptimization ? RDType : PointerStructTy);
  if (!FutureInitDef) {
    return Decls;
  }

  Decls.push_back(FutureInitDef);
  Context.BSCDesugaredMap[FD].push_back(FutureInitDef);

  const int FutureStateNumber = AwaitFinder.GetAwaitExprNum() + 1;

  Decls.push_back(RD);
  Context.BSCDesugaredMap[FD].push_back(RD);

  FunctionDecl *FutureInit = nullptr;
  if (IsOptimization) {
    FutureInit = buildFutureStructInitFunctionDefinition(*this, RD, FD);
  } else {
    FutureInit =
        buildFutureInitFunctionDefinition(*this, RD, FD, FutureInitDef);
  }

  if (!FutureInit) {
    return Decls;
  }
  Decls.push_back(FutureInit);
  Context.BSCDesugaredMap[FD].push_back(FutureInit);

  BSCMethodDecl *FreeDecl =
      buildFreeFunctionDefinition(*this, RD, FD, IsOptimization);
  if (!FreeDecl) {
    return Decls;
  }
  Decls.push_back(FreeDecl);
  Context.BSCDesugaredMap[FD].push_back(FreeDecl);

  BSCMethodDecl *PollDecl = buildPollFunctionDefinition(
      *this, RD, PollResultRD, FD, FatPointerRD, FutureStateNumber);
  if (!PollDecl) {
    return Decls;
  }
  Decls.push_back(PollDecl);
  Context.BSCDesugaredMap[FD].push_back(PollDecl);

  VarDecl *VtableDecl =
      buildVtableInitDecl(*this, FD, Context.getRecordType(RD), ReturnTy, true);
  if (!VtableDecl) {
    return Decls;
  }
  Decls.push_back(VtableDecl);
  Context.BSCDesugaredMap[FD].push_back(VtableDecl);

  return Decls;
}

#endif // ENABLE_BSC