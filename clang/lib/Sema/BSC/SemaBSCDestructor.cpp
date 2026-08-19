#if ENABLE_BSC

#include <stack>

#include "TreeTransform.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/Template.h"

using namespace clang;
using namespace sema;

namespace {
/// Unwrap one SafeStmt wrapper (_Safe / _Unsafe regions in BSC).
Stmt *bscStripSafeStmt(Stmt *S) {
  if (auto *SS = dyn_cast<SafeStmt>(S))
    return SS->getSubStmt();
  return S;
}

CompoundStmt *bscGetBodyCompound(Stmt *Body) {
  return dyn_cast<CompoundStmt>(bscStripSafeStmt(Body));
}

BSCMethodDecl *buildBSCMethodDecl(ASTContext &C, DeclContext *DC,
                                         SourceLocation StartLoc,
                                         SourceLocation NLoc, DeclarationName N,
                                         QualType T, TypeSourceInfo *TInfo,
                                         StorageClass SC, QualType ET) {
  // TODO: inline should be passed.
  BSCMethodDecl *NewDecl = BSCMethodDecl::Create(
      C, DC, StartLoc, DeclarationNameInfo(N, NLoc), T, TInfo, SC, false, false,
      ConstexprSpecKind::Unspecified, NLoc);
  return NewDecl;
}

bool IsVarDeclWithOwnedStructureType(VarDecl *VD) {
  const Type *VDType = VD->getType().getCanonicalType().getTypePtr();
  if (VDType->isOwnedStructureType() && !VD->hasGlobalStorage())
    return true;
  return false;
}

// Collect owned struct type field that has destructor.
std::stack<FieldDecl *> CollectInstanceFieldWithDestructor(RecordDecl *RD) {
  std::stack<FieldDecl *> OwnedStructFields;
  for (RecordDecl::field_iterator FieldIt = RD->field_begin();
       FieldIt != RD->field_end(); ++FieldIt) {
    const Type *FieldType = FieldIt->getType().getCanonicalType().getTypePtr();
    if (FieldType->isOwnedStructureType()) {
      RecordDecl *RD = cast<RecordType>(FieldType)->getDecl();
      if (RD->getBSCDestructor() && !RD->getBSCDestructor()->isInvalidDecl()) {
        OwnedStructFields.push(*FieldIt);
      }
    }
  }
  return OwnedStructFields;
}
} // namespace

BSCMethodDecl *Sema::getOrInsertBSCDestructor(RecordDecl *RD) {
  if (RD->isInvalidDecl()) {
    return nullptr;
  }
  BSCMethodDecl *Destructor;
  if (RD->getBSCDestructor() == nullptr) {
    assert(RD->isOwnedDecl());
    QualType FuncRetType = getASTContext().VoidTy;
    QualType ParamType = getASTContext().getRecordType(RD);
    if (const InjectedClassNameType *ICT =
            dyn_cast<const InjectedClassNameType>(ParamType)) {
      ParamType = ICT->getInjectedSpecializationType();
    }
    ParamType.addOwned();
    SmallVector<QualType, 1> ParamTys;
    ParamTys.push_back(ParamType);
    QualType FuncType =
        getASTContext().getFunctionType(FuncRetType, ParamTys, {});
    DeclarationName Name = Context.DeclarationNames.getCXXDestructorName(
        Context.getCanonicalType(Context.getTypeDeclType(RD)));

    TypeSourceInfo *TInfo =
        Context.getTrivialTypeSourceInfo(FuncType, RD->getEndLoc());
    FunctionProtoTypeLoc ProtoLoc =
        TInfo->getTypeLoc().IgnoreParens().castAs<FunctionProtoTypeLoc>();
    Destructor = buildBSCMethodDecl(
        getASTContext(), RD, RD->getEndLoc(), RD->getEndLoc(), Name, FuncType,
        TInfo, SC_None, RD->getTypeForDecl()->getCanonicalTypeInternal());
    SmallVector<ParmVarDecl *, 1> ParmVarDecls;
    TypeSourceInfo *ParamTInfo =
        Context.getTrivialTypeSourceInfo(ParamType, SourceLocation());
    ParmVarDecl *PVD = ParmVarDecl::Create(
        getASTContext(), Destructor, RD->getEndLoc(), RD->getEndLoc(),
        &(getASTContext().Idents).get("this"), ParamType, ParamTInfo, SC_None,
        nullptr);
    ProtoLoc.setParam(0, PVD);

    ParmVarDecls.push_back(PVD);
    Destructor->setParams(ParmVarDecls);
    RD->addDecl(Destructor);
    Destructor->setLexicalDeclContext(getASTContext().getTranslationUnitDecl());
    Destructor->setDestructor(true);
    CompoundStmt *CS =
        CompoundStmt::Create(getASTContext(), {}, FPOptionsOverride(),
                             RD->getEndLoc(), RD->getEndLoc());
    Destructor->setBody(CS);
    PushOnScopeChains(Destructor, getCurScope(), false);
  } else {
    Destructor = RD->getBSCDestructor();
  }
  return Destructor;
}

namespace {
// Collect all owned struct type fields that has destructor,
// including nested owned struct type fields.
void CollectAllFieldsWithPendingInstantiatedDestructor(RecordDecl *RD,
                                                              std::stack<RecordDecl *> &OwnedStructFields) {
  for (RecordDecl::field_iterator FieldIt = RD->field_begin();
       FieldIt != RD->field_end(); ++FieldIt) {
    const Type *FieldType = FieldIt->getType().getCanonicalType().getTypePtr();
    if (FieldType->isOwnedStructureType()) {
      RecordDecl *RD = cast<RecordType>(FieldType)->getDecl();
      if (RD->getBSCDestructor() && !RD->getBSCDestructor()->isInvalidDecl()) {
        OwnedStructFields.push(RD);
        CollectAllFieldsWithPendingInstantiatedDestructor(RD, OwnedStructFields);
      }
    }
  }
}
} // namespace

void Sema::HandleBSCDestructorBody(RecordDecl *RD, BSCMethodDecl *Destructor,
                                   std::stack<FieldDecl *> InstanceFields) {
  if (InstanceFields.empty())
    return;
  Stmt *FuncBody = Destructor->getBody();
  if (FuncBody) {
    if (auto *CS = dyn_cast<CompoundStmt>(FuncBody)) {
      SmallVector<Stmt *> Stmts;
      for (auto *C : CS->children()) {
        Stmts.push_back(C);
      }
      while (!InstanceFields.empty()) {
        FieldDecl *Field = InstanceFields.top();
        InstanceFields.pop();

        const Type *FieldType = Field->getType().getCanonicalType().getTypePtr();
        if (FieldType->isOwnedStructureType()) {
          RecordDecl *ThisRD = cast<RecordType>(FieldType)->getDecl();
          BSCMethodDecl *DestructorToCall = ThisRD->getBSCDestructor();
          // If owned struct template field has a valid destructor declaration,
          // we instantiate it here to ensure the generated
          // destructor body (e.g., for A<T>) has access to the destructor
          // of its nested owned struct field (e.g., B<T>).
          // This is critical when dealing with nested owned structs, like:
          //   owned struct A<T> { B<T> buf; ~A(A<T> this) { /* call ~B<T> */ } }
          //   owned struct B<T> { ~B(B<T> this) { } }
          // so that ~B<T> is instantiated before being used in ~A<T>.
          if (DestructorToCall && !DestructorToCall->isInvalidDecl()) {
              SourceLocation PointOfInstantiation = DestructorToCall->getPointOfInstantiation();
              DestructorToCall->setInstantiationIsPending(true);
              InstantiateFunctionDefinition(PointOfInstantiation,
                                            DestructorToCall,
                                            /*Recursive=*/true,
                                            /*DefinitionRequired=*/true,
                                            /*AtEndOfTU=*/false);
          }  
          ParmVarDecl *PVD = Destructor->getParamDecl(0);
          QualType ParamType = getASTContext().getRecordType(RD);
          ParamType.addOwned();

          Expr *DRE =
              BuildDeclRefExpr(PVD, ParamType, VK_LValue, CS->getRBracLoc());
          if (!ParamType->getAsCXXRecordDecl()) {
            DRE = ImplicitCastExpr::Create(Context, ParamType, CK_LValueToRValue,
                                          DRE, nullptr, VK_PRValue,
                                          FPOptionsOverride());
          }
          Expr *MemberExpr = BuildMemberExpr(
              DRE, false, SourceLocation(), NestedNameSpecifierLoc(),
              SourceLocation(), Field,
              DeclAccessPair::make(RD, Field->getAccess()), false,
              DeclarationNameInfo(), Field->getType().getNonReferenceType(),
              VK_LValue, OK_Ordinary);
          SmallVector<Expr *, 1> Args;
          Args.push_back(MemberExpr);
          Expr *DestructorRef =
              BuildDeclRefExpr(DestructorToCall, DestructorToCall->getType(),
                              VK_LValue, SourceLocation());
          DestructorRef =
              ImpCastExprToType(DestructorRef,
                                Context.getPointerType(DestructorRef->getType()),
                                CK_FunctionToPointerDecay)
                  .get();
          Expr *CE = BuildCallExpr(nullptr, DestructorRef, SourceLocation(), Args,
                                  SourceLocation())
                        .get();
          Stmts.push_back(CE);

          // If a owned struct has nested owned structs which should be instantiated,
          // we add destructors of all nested owned structs to PendingInstantiations,
          // the functions in which will be instantiated in ActOnEndOfTranslationUnit().
          // For example:
          // @code
          //     owned struct A<T> {};
          //     owned struct B<T> { A<T> a; };
          //     owned struct C { B<int> b; };
          // @endcode
          // When desugar destructor of C, above we have added destructor of B<int> to PendingInstantiations,
          // here we add destructor of A<int> to PendingInstantiations.
          std::stack<RecordDecl *> OwnedStructFields;
          CollectAllFieldsWithPendingInstantiatedDestructor(ThisRD,
                                                            OwnedStructFields);
          while (!OwnedStructFields.empty()) {
            RecordDecl *RDOfOwnerStructField = OwnedStructFields.top();
            OwnedStructFields.pop();
            BSCMethodDecl *DestructorOfOwnerStructField =
                RDOfOwnerStructField->getBSCDestructor();
            SourceLocation PointOfInstantiation =
                DestructorOfOwnerStructField->getPointOfInstantiation();
            DestructorOfOwnerStructField->setInstantiationIsPending(true);
            PendingInstantiations.push_back(
                std::make_pair(DestructorOfOwnerStructField, PointOfInstantiation));
          }
        }
      }
      CompoundStmt *NewCS =
          CompoundStmt::Create(getASTContext(), Stmts, FPOptionsOverride(),
                              CS->getLBracLoc(), CS->getRBracLoc());
      Destructor->setBody(NewCS);
    }
  }
}

namespace {

class DeclRefFinder : public RecursiveASTVisitor<DeclRefFinder> {
public:
  DeclRefFinder() {}

  bool VisitCallExpr(CallExpr *CE) {
    for (auto it = CE->arg_begin(), ei = CE->arg_end(); it != ei; ++it) {
      RecursiveASTVisitor<DeclRefFinder>::TraverseStmt(*it);
    }
    return false;
  }
  bool VisitDeclRefExpr(DeclRefExpr *E) {
    if (isa<VarDecl>(E->getDecl()) &&
        IsVarDeclWithOwnedStructureType(cast<VarDecl>(E->getDecl()))) {
      MovedDecls.push_back(cast<VarDecl>(E->getDecl()));
    }
    return true;
  }
  bool VisitBinaryOperator(const BinaryOperator *BOp) {
    if (isa<DeclRefExpr>(BOp->getLHS())) {
      auto E = cast<DeclRefExpr>(BOp->getLHS());
      if (isa<VarDecl>(E->getDecl()) &&
          IsVarDeclWithOwnedStructureType(cast<VarDecl>(E->getDecl()))) {
        ReAssignedDecls.push_back(cast<VarDecl>(E->getDecl()));
      }
    }
    if (BOp->getRHS()) {
      RecursiveASTVisitor<DeclRefFinder>::TraverseStmt(BOp->getRHS());
    }
    return false;
  }

  bool VisitUnaryOperator(const UnaryOperator *UOp) { return false; }

  bool VisitMemberExpr(const MemberExpr *MA) { return false; }
  bool VisitInitListExpr(InitListExpr *ILE) {
    for (auto *Init : ILE->inits()) {
      RecursiveASTVisitor<DeclRefFinder>::TraverseStmt(Init);
    }
    return false;
  }

  llvm::SmallVector<VarDecl *> MovedDecls;
  llvm::SmallVector<VarDecl *> ReAssignedDecls;
};
llvm::DenseMap<CompoundStmt *, CompoundStmt *> ReplaceCompoundMap;

class InsertDestructorCallStmt
    : public RecursiveASTVisitor<InsertDestructorCallStmt> {
  Sema &SemaRef;
  llvm::DenseMap<VarDecl *, VarDecl *> FlagMap;
  FunctionDecl *FD;

  /// Append move-flag updates for BinaryOperator / DeclStmt / CallExpr Inner.
  void appendMoveFlagUpdates(SmallVectorImpl<Stmt *> &Out, Stmt *Inner) {
    if (!isa<BinaryOperator>(Inner) && !isa<DeclStmt>(Inner) &&
        !isa<CallExpr>(Inner))
      return;
    DeclRefFinder Finder;
    Finder.TraverseStmt(Inner);
    for (auto *D : Finder.ReAssignedDecls) {
      if (Stmt *Update = MoveFlagStatusUpdate(D, 0))
        Out.push_back(Update);
    }
    for (auto *D : Finder.MovedDecls) {
      if (Stmt *Update = MoveFlagStatusUpdate(D))
        Out.push_back(Update);
    }
  }

  /// Emit destructor-before-reassign + statement + move-flag updates for a
  /// BinaryOperator that reassigns an owned struct.
  void emitReassignSequence(SmallVectorImpl<Stmt *> &Out, Stmt *S, Stmt *Inner) {
    DeclRefFinder Finder;
    Finder.TraverseStmt(Inner);
    for (auto *D : Finder.ReAssignedDecls) {
      if (Stmt *IfStmt = AddIfStmt(D))
        Out.push_back(IfStmt);
    }
    Out.push_back(S);
    for (auto *D : Finder.ReAssignedDecls) {
      if (Stmt *Update = MoveFlagStatusUpdate(D, 0))
        Out.push_back(Update);
    }
    for (auto *D : Finder.MovedDecls) {
      if (Stmt *Update = MoveFlagStatusUpdate(D))
        Out.push_back(Update);
    }
  }

public:
  explicit InsertDestructorCallStmt(Sema &SemaRef) : SemaRef(SemaRef) {}

  Stmt *AddIfStmt(VarDecl *VD) {
    assert(IsVarDeclWithOwnedStructureType(VD));
    const Type *VDType = VD->getType().getCanonicalType().getTypePtr();
    RecordDecl *RD = cast<RecordType>(VDType)->getDecl();
    BSCMethodDecl *DestructorToCall = SemaRef.getOrInsertBSCDestructor(RD);
    if (!DestructorToCall) {
      return nullptr;
    }
    Expr *IDRE = SemaRef.BuildDeclRefExpr(VD, VD->getType().getCanonicalType(),
                                          VK_LValue, SourceLocation());
    SmallVector<Expr *, 1> Args;
    Args.push_back(IDRE);
    Expr *DestructorRef =
        SemaRef.BuildDeclRefExpr(DestructorToCall, DestructorToCall->getType(),
                                 VK_LValue, SourceLocation());
    DestructorRef = SemaRef
                        .ImpCastExprToType(DestructorRef,
                                           SemaRef.Context.getPointerType(
                                               DestructorRef->getType()),
                                           CK_FunctionToPointerDecay)
                        .get();
    Expr *CE = SemaRef
                   .BuildCallExpr(nullptr, DestructorRef, SourceLocation(),
                                  Args, SourceLocation())
                   .get();
    if (!CE) {
      return nullptr;
    }
    // VD may be from an enclosing scope that bypassed CreateMoveFlag.
    if (!FlagMap.count(VD))
      return nullptr;
    Expr *FlagRefExpr = SemaRef.BuildDeclRefExpr(
        FlagMap[VD], FlagMap[VD]->getType(), VK_LValue, SourceLocation());
    FlagRefExpr =
        SemaRef.CreateBuiltinUnaryOp(SourceLocation(), UO_LNot, FlagRefExpr)
            .get();
    Sema::ConditionResult IfCond = SemaRef.ActOnCondition(
        nullptr, SourceLocation(), FlagRefExpr, Sema::ConditionKind::Boolean);
    Stmt *If = SemaRef
                   .BuildIfStmt(
                       SourceLocation(), IfStatementKind::Ordinary,
                       /*LPL=*/SourceLocation(), /*Init=*/nullptr, IfCond,
                       /*RPL=*/SourceLocation(), CE, SourceLocation(), nullptr)
                   .get();
    return If;
  }

  // Add if stmt for owned struct type vardecl.
  SmallVector<Stmt *> AddIfStmts(Stmt *S) {
    SmallVector<Stmt *> IfStmts;
    SmallVector<VarDecl *> VarDecls = SemaRef.Context.DestructMap[FD][S];
    for (auto *VD : VarDecls) {
      Stmt *If = AddIfStmt(VD);
      IfStmts.push_back(If);
    }
    return IfStmts;
  }
  // Create _Bool xx_is_moved = 0;
  DeclStmt *CreateMoveFlag(VarDecl *D) {
    std::string IName = D->getName().str() + "_is_moved";
    VarDecl *VD =
        VarDecl::Create(SemaRef.Context, FD, D->getLocation(), D->getLocation(),
                        &(SemaRef.Context.Idents).get(IName),
                        SemaRef.Context.BoolTy, nullptr, SC_None);
    FD->addDecl(VD);
    llvm::APInt Zero(SemaRef.Context.getTypeSize(SemaRef.Context.IntTy), 0);
    Expr *IInit = IntegerLiteral::Create(
        SemaRef.Context, Zero, SemaRef.Context.IntTy, SourceLocation());
    IInit = SemaRef
                .ImpCastExprToType(IInit, SemaRef.Context.BoolTy,
                                   CK_IntegralToBoolean)
                .get();
    VD->setInit(IInit);
    DeclGroupRef DataDG(VD);
    DeclStmt *DataDS = new (SemaRef.Context)
        DeclStmt(DataDG, D->getLocation(), D->getLocation());
    FlagMap[D] = VD;
    return DataDS;
  }

  // Create xx_is_moved = 1;
  Stmt *MoveFlagStatusUpdate(VarDecl *D, uint64_t Value = 1) {
    // Skip variables not declared in the current function (e.g., captured from
    // an enclosing scope in a nested _Owned struct defined in function scope).
    if (FlagMap.find(D) == FlagMap.end())
      return nullptr;
    Expr *LHS = SemaRef.BuildDeclRefExpr(FlagMap[D], FlagMap[D]->getType(),
                                         VK_LValue, SourceLocation());
    llvm::APInt One(SemaRef.Context.getTypeSize(SemaRef.Context.IntTy), Value);
    Expr *IInit = IntegerLiteral::Create(
        SemaRef.Context, One, SemaRef.Context.IntTy, SourceLocation());
    IInit = SemaRef
                .ImpCastExprToType(IInit, SemaRef.Context.BoolTy,
                                   CK_IntegralToBoolean)
                .get();
    Expr *BinOpExpr =
        SemaRef.CreateBuiltinBinOp(SourceLocation(), BO_Assign, LHS, IInit)
            .get();
    return BinOpExpr;
  }

  bool VisitCompoundStmt(CompoundStmt *CS) {
    SmallVector<Stmt *> Statements;
    if (CS == FD->getBody()) {
      for (ParmVarDecl *PVD : FD->parameters()) {
        if (IsVarDeclWithOwnedStructureType(PVD)) {
          Statements.push_back(CreateMoveFlag(PVD));
        }
      }
    }
    bool HasControlTransferExpr = false;
    for (auto *C : CS->children()) {
      Stmt *S = const_cast<Stmt *>(C);
      Stmt *Inner = bscStripSafeStmt(S);
      // Add destructor call if-stmt for all defined vardecls before exit current block
      if (isa<ReturnStmt>(Inner) || isa<BreakStmt>(Inner) || isa<ContinueStmt>(Inner)) {
        HasControlTransferExpr = true;
        SmallVector<Stmt *> IfStmts = AddIfStmts(Inner);
        Statements.insert(Statements.end(), IfStmts.begin(), IfStmts.end());
      }
      RecursiveASTVisitor<InsertDestructorCallStmt>::TraverseStmt(C);
      if (isa<CompoundStmt>(S)) {
        Statements.push_back(ReplaceCompoundMap.lookup(cast<CompoundStmt>(S)));
      } else if (auto *SS = dyn_cast<SafeStmt>(S)) {
        if (auto *InnerCS = dyn_cast<CompoundStmt>(SS->getSubStmt())) {
          if (CompoundStmt *NewC = ReplaceCompoundMap.lookup(InnerCS))
            SS->setSubStmt(NewC);
          Statements.push_back(S);
        } else if (isa<BinaryOperator>(Inner)) {
          emitReassignSequence(Statements, S, Inner);
        } else {
          Statements.push_back(S);
          appendMoveFlagUpdates(Statements, Inner);
        }
      } else if (isa<LabelStmt>(S)) {
        auto *labelStmt = cast<LabelStmt>(S);
        if (!isa<CompoundStmt>(labelStmt->getSubStmt())) {
          auto stmts = CreateStatements(labelStmt->getSubStmt());
          labelStmt->setSubStmt(stmts[0]);
          Statements.push_back(labelStmt);
          Statements.insert(Statements.end(), stmts.begin() + 1, stmts.end());
        } else {
          Statements.push_back(C);
        }
      } else {
        if (isa<BinaryOperator>(Inner)) {
          emitReassignSequence(Statements, C, Inner);
        } else {
          Statements.push_back(C);
        }
      }
      if (isa<DeclStmt>(Inner)) {
        // add _Bool xx_is_moved = 0;
        for (auto *D : cast<DeclStmt>(Inner)->decls()) {
          if (isa<VarDecl>(D)) {
            VarDecl *VD = cast<VarDecl>(D);
            if (IsVarDeclWithOwnedStructureType(VD)) {
              Statements.push_back(CreateMoveFlag(VD));
            }
          }
        }
      }

      // change reassigned_decl_is_moved = 0, and moved_decl_is_moved = 1
      if (!isa<BinaryOperator>(Inner))
        appendMoveFlagUpdates(Statements, Inner);
    }
    if (!HasControlTransferExpr) {
      // If no return/break/continue stmts, create destructor call if-stmt for all defined vardecls before exit
      auto IfStmts = AddIfStmts(CS);
      Statements.insert(Statements.end(), IfStmts.begin(), IfStmts.end());
    }
    auto NewCPStmt = CompoundStmt::Create(SemaRef.getASTContext(), Statements,
                                          FPOptionsOverride(),
                                          CS->getLBracLoc(), CS->getRBracLoc(),
                                          CS->getCompSafeZoneSpecifier());
    ReplaceCompoundMap[CS] = NewCPStmt;
    return false;
  }

  SmallVector<Stmt *> CreateStatements(Stmt *S) {
    SmallVector<Stmt *> Statements;
    Stmt *Inner = bscStripSafeStmt(S);
    if (isa<ReturnStmt>(Inner) || isa<BreakStmt>(Inner) || isa<ContinueStmt>(Inner)) {
      SmallVector<Stmt *> IfStmts = AddIfStmts(Inner);
      Statements.insert(Statements.end(), IfStmts.begin(), IfStmts.end());
    }
    if (isa<BinaryOperator>(Inner)) {
      emitReassignSequence(Statements, S, Inner);
      return Statements;
    }
    Statements.push_back(S);
    if (isa<DeclStmt>(Inner)) {
      for (auto *D : cast<DeclStmt>(Inner)->decls()) {
        if (isa<VarDecl>(D)) {
          VarDecl *VD = cast<VarDecl>(D);
          if (IsVarDeclWithOwnedStructureType(VD)) {
            Statements.push_back(CreateMoveFlag(VD));
            Statements.push_back(AddIfStmt(VD));
          }
        }
      }
    }
    appendMoveFlagUpdates(Statements, Inner);
    return Statements;
  }

  Stmt *CreateNewCompoundStmt(Stmt *S) {
    auto Stmts = CreateStatements(S);
    if (Stmts.size() <= 1)
      return S;
    return CompoundStmt::Create(SemaRef.getASTContext(), Stmts,
                                FPOptionsOverride(), S->getBeginLoc(),
                                S->getEndLoc());
  }

  // Replace a body stmt: if compound, look up in ReplaceCompoundMap;
  // otherwise synthesize a compound via CreateNewCompoundStmt.
  // Handles SafeStmt wrapping transparently.
  template <typename SetBodyFn>
  void ReplaceBody(Stmt *Body, SetBodyFn SetBody) {
    SafeStmt *SS = dyn_cast<SafeStmt>(Body);
    Stmt *Inner = bscStripSafeStmt(Body);
    if (auto *CS = dyn_cast<CompoundStmt>(Inner)) {
      if (auto *NewCompound = ReplaceCompoundMap.lookup(CS)) {
        if (SS)
          SS->setSubStmt(NewCompound);
        else
          SetBody(NewCompound);
      }
    } else {
      SetBody(CreateNewCompoundStmt(Body));
    }
  }

  // Traverse a loop/branch body, replacing its CompoundStmt via the map.
  // Handles the case where the body is wrapped in a SafeStmt.
  template <typename SetBodyFn>
  void TraverseAndReplaceBody(Stmt *Body, SetBodyFn SetBody) {
    RecursiveASTVisitor<InsertDestructorCallStmt>::TraverseStmt(Body);
    ReplaceBody(Body, SetBody);
  }

  bool TraverseWhileStmt(WhileStmt *WS) {
    if (WS->getBody()) {
      TraverseAndReplaceBody(WS->getBody(),
                             [WS](Stmt *S) { WS->setBody(S); });
    }
    return false;
  }

  bool TraverseIfStmt(IfStmt *IS) {
    if (IS->getThen()) {
      TraverseAndReplaceBody(IS->getThen(),
                             [IS](Stmt *S) { IS->setThen(S); });
    }
    if (IS->getElse()) {
      TraverseAndReplaceBody(IS->getElse(),
                             [IS](Stmt *S) { IS->setElse(S); });
    }
    return false;
  }

  bool TraverseDoStmt(DoStmt *DS) {
    if (DS->getBody()) {
      TraverseAndReplaceBody(DS->getBody(),
                             [DS](Stmt *S) { DS->setBody(S); });
    }
    return false;
  }
  bool TraverseForStmt(ForStmt *FS) {
    if (FS->getBody()) {
      TraverseAndReplaceBody(FS->getBody(),
                             [FS](Stmt *S) { FS->setBody(S); });
    }
    return false;
  }

  bool TraverseLabelStmt(LabelStmt *LS) {
    RecursiveASTVisitor<InsertDestructorCallStmt>::TraverseLabelStmt(LS);
    ReplaceBody(LS->getSubStmt(), [LS](Stmt *S) { LS->setSubStmt(S); });
    return false;
  }

  bool TraverseSwitchStmt(SwitchStmt *SS) {
    if (SS->getBody()) {
      TraverseAndReplaceBody(SS->getBody(),
                             [SS](Stmt *S) { SS->setBody(S); });
    }
    return false;
  }

  bool TraverseDefaultStmt(DefaultStmt *DS) {
    RecursiveASTVisitor<InsertDestructorCallStmt>::TraverseDefaultStmt(DS);
    ReplaceBody(DS->getSubStmt(), [DS](Stmt *S) { DS->setSubStmt(S); });
    return false;
  }

  bool TraverseCaseStmt(CaseStmt *CS) {
    RecursiveASTVisitor<InsertDestructorCallStmt>::TraverseCaseStmt(CS);
    ReplaceBody(CS->getSubStmt(), [CS](Stmt *S) { CS->setSubStmt(S); });
    return false;
  }

  bool TraverseFunctionDecl(FunctionDecl *D) {
    FD = D;
    RecursiveASTVisitor<InsertDestructorCallStmt>::TraverseFunctionDecl(D);
    if (D->getBody() != nullptr) {
      if (auto *CS = dyn_cast<CompoundStmt>(D->getBody())) {
        if (auto *NewCompound = ReplaceCompoundMap.lookup(CS))
          D->setBody(NewCompound);
      }
    }
    return false;
  }
};

class CalcDestructorMapForIns
    : public RecursiveASTVisitor<CalcDestructorMapForIns> {
  Sema &SemaRef;
  FunctionDecl *FD;
  bool IsDestructor = false;
  std::stack<CompoundStmt *> VisitCompoundStmtStack;
  llvm::DenseMap<CompoundStmt *, SmallVector<VarDecl *>> CS2Vars;
  std::stack<CompoundStmt *> VisitLoopCompoundStmtStack;

public:
  explicit CalcDestructorMapForIns(Sema &SemaRef) : SemaRef(SemaRef) {}

  bool VisitReturnStmt(ReturnStmt *RS) {
    if (!VisitCompoundStmtStack.empty()) {
      DeclRefFinder Finder = DeclRefFinder();
      Finder.TraverseStmt(RS);
      SmallVector<VarDecl *> MovedDecls = Finder.MovedDecls;
      std::stack<CompoundStmt *> tempStack = VisitCompoundStmtStack;
      SmallVector<VarDecl *> newVarDecls;
      while (!tempStack.empty()) {
        auto CompoundStmt = tempStack.top();
        for (auto iter = CS2Vars[CompoundStmt].begin();
             iter != CS2Vars[CompoundStmt].end(); iter++) {
          VarDecl *D = *iter;
          if (std::find(MovedDecls.begin(), MovedDecls.end(), D) ==
              MovedDecls.end()) {
            newVarDecls.insert(newVarDecls.end(), D);
          }
        }
        tempStack.pop();
      }
      if (newVarDecls.size() != 0) {
        SemaRef.Context.DestructMap[FD][RS] = newVarDecls;
      }
    }
    return false;
  }

  void CalcDestructMapForBreakAndContinue(Stmt *S) {
    if (!VisitLoopCompoundStmtStack.empty()) {
      std::stack<CompoundStmt *> tempStack = VisitCompoundStmtStack;
      SmallVector<VarDecl *> newVarDecls;
      while (!tempStack.empty() &&
             tempStack.top() != VisitLoopCompoundStmtStack.top()) {
        auto CompoundStmt = tempStack.top();
        newVarDecls.insert(newVarDecls.begin(), CS2Vars[CompoundStmt].begin(),
                           CS2Vars[CompoundStmt].end());
        tempStack.pop();
      }
      if (!tempStack.empty() &&
          tempStack.top() == VisitLoopCompoundStmtStack.top()) {
        auto CompoundStmt = tempStack.top();
        newVarDecls.insert(newVarDecls.begin(), CS2Vars[CompoundStmt].begin(),
                           CS2Vars[CompoundStmt].end());
        tempStack.pop();
      }
      if (newVarDecls.size() != 0) {
        SemaRef.Context.DestructMap[FD][S] = newVarDecls;
      }
    }
  }

  bool VisitBreakStmt(BreakStmt *BS) {
    CalcDestructMapForBreakAndContinue(BS);
    return false;
  }
  bool VisitContinueStmt(ContinueStmt *CS) {
    CalcDestructMapForBreakAndContinue(CS);
    return false;
  }

  bool TraverseCompoundStmt(CompoundStmt *CS) {
    VisitCompoundStmtStack.push(CS);
    SmallVector<VarDecl *> &VarDecls = CS2Vars[CS];
    if (CS == FD->getBody() && !IsDestructor) {
      for (ParmVarDecl *PVD : FD->parameters()) {
        if (IsVarDeclWithOwnedStructureType(PVD)) {
          VarDecls.insert(VarDecls.begin(), PVD);
        }
      }
    }

    for (auto *C : CS->children()) {
      Stmt *S = const_cast<Stmt *>(C);
      if (DeclStmt *StmtDecl = dyn_cast<DeclStmt>(S)) {
        for (auto *SD : StmtDecl->decls()) {
          if (auto VD = dyn_cast<VarDecl>(SD)) {
            bool InTopLevelSwitchBlock = VD->isDefInTopLevelSwitchBlock();
            if (IsVarDeclWithOwnedStructureType(VD) && !InTopLevelSwitchBlock) {
              VarDecls.insert(VarDecls.begin(), VD);
            }
          }
        }
      }
      RecursiveASTVisitor<CalcDestructorMapForIns>::TraverseStmt(C);
    }
    if (VarDecls.size() != 0) {
      SemaRef.Context.DestructMap[FD][CS] = VarDecls;
    }
    VisitCompoundStmtStack.pop();
    return true;
  }

  bool TraverseSwitchStmt(SwitchStmt *SS) {
    CompoundStmt *BodyCS = bscGetBodyCompound(SS->getBody());
    if (BodyCS)
      VisitLoopCompoundStmtStack.push(BodyCS);
    RecursiveASTVisitor<CalcDestructorMapForIns>::TraverseSwitchStmt(SS);
    if (BodyCS)
      VisitLoopCompoundStmtStack.pop();
    return false;
  }

  bool TraverseWhileStmt(WhileStmt *WS) {
    CompoundStmt *BodyCS = bscGetBodyCompound(WS->getBody());
    if (BodyCS)
      VisitLoopCompoundStmtStack.push(BodyCS);
    RecursiveASTVisitor<CalcDestructorMapForIns>::TraverseWhileStmt(WS);
    if (BodyCS)
      VisitLoopCompoundStmtStack.pop();
    return false;
  }

  bool TraverseDoStmt(DoStmt *DS) {
    CompoundStmt *BodyCS = bscGetBodyCompound(DS->getBody());
    if (BodyCS)
      VisitLoopCompoundStmtStack.push(BodyCS);
    RecursiveASTVisitor<CalcDestructorMapForIns>::TraverseDoStmt(DS);
    if (BodyCS)
      VisitLoopCompoundStmtStack.pop();
    return false;
  }

  bool TraverseForStmt(ForStmt *FS) {
    CompoundStmt *BodyCS = bscGetBodyCompound(FS->getBody());
    if (BodyCS)
      VisitLoopCompoundStmtStack.push(BodyCS);
    RecursiveASTVisitor<CalcDestructorMapForIns>::TraverseForStmt(FS);
    if (BodyCS)
      VisitLoopCompoundStmtStack.pop();
    return false;
  }
  bool TraverseFunctionDecl(FunctionDecl *D) {
    FD = D;
    if (auto MD = dyn_cast<BSCMethodDecl>(D)) {
      IsDestructor = MD->isDestructor();
    }
    return RecursiveASTVisitor<CalcDestructorMapForIns>::TraverseFunctionDecl(
        D);
  }
};
} // namespace

void Sema::DesugarDestructorCall(FunctionDecl *FD) {
  if (!getLangOpts().BSC)
    return;
  // Skip function template and class template.
  if (auto RD = dyn_cast<RecordDecl>(FD->getParent())) {
    if (RD->getDescribedClassTemplate() != nullptr)
      return;
    if (RD->isInvalidDecl())
      return;
  }
  if (FD->getTemplatedKind() == FunctionDecl::TK_FunctionTemplate)
    return;
  CollDestructMapInFuncInstantiation(FD);
  // Insert destructor function calls.
  InsertDestructorCallStmt IDCS(*this);
  IDCS.TraverseFunctionDecl(FD);
}

void Sema::DesugarDestructor(RecordDecl *RD) {
  if (!getLangOpts().BSC || !RD->isOwnedDecl() || !RD->isCompleteDefinition()) {
    return;
  }
  BSCMethodDecl *Destructor = getOrInsertBSCDestructor(RD);
  if (!Destructor || Destructor->isInvalidDecl())
    return;
  std::stack<FieldDecl *> Fields = CollectInstanceFieldWithDestructor(RD);
  BSCDataflowAnalysisFlag = true;
  unsigned NumUncompilableErrorsBeforeBodyBuild =
      getDiagnostics().getNumUncompilableErrors();
  HandleBSCDestructorBody(RD, Destructor, Fields);
  if (getLangOpts().getBSCDiag() == LangOptions::BSCDiagExhaustive) {
    // The user-written destructor body may already have reported errors while it
    // was parsed; those are recorded in BSCFunctionsWithLocalErrors. Errors
    // emitted while synthesizing the remaining body are also local errors.
    // Warnings upgraded by -Werror do not invalidate the AST, so they are not
    // treated as local errors here.
    bool HasLocalErrors =
        Destructor->isInvalidDecl() ||
        BSCFunctionsWithLocalErrors.count(Destructor) ||
        getDiagnostics().getNumUncompilableErrors() >
            NumUncompilableErrorsBeforeBodyBuild;
    if (HasLocalErrors) {
      // Already reported errors inside this destructor; skip analysis.
    } else {
      SourceLocation InvalidLoc;
      if (HasInvalidAST(Destructor, &InvalidLoc)) {
        // The destructor body itself is clean, but earlier errors elsewhere have
        // produced an unreliable AST. Tell the user why no BSC analysis was run,
        // pointing at the problematic AST inside the destructor when possible.
        SourceLocation DiagLoc = InvalidLoc;
        if (DiagLoc.isInvalid() ||
            !getSourceManager().isPointWithin(DiagLoc, Destructor->getBeginLoc(),
                                              Destructor->getEndLoc()))
          DiagLoc = Destructor->getLocation();
        Diag(DiagLoc, diag::warn_bsc_analysis_skipped) << Destructor;
      } else {
        BSCDataflowAnalysis(Destructor);
      }
    }
  } else if (getDiagnostics().areAllErrorsFromBSCAnalyses()) {
    BSCDataflowAnalysis(Destructor);
  }
  BSCDataflowAnalysisFlag = false;
}

void Sema::CheckBSCDestructorDeclarator(FunctionDecl *NewFD) {
  RecordDecl *Record = cast<RecordDecl>(NewFD->getParent());
  if (Record->isInvalidDecl())
    return;
  QualType ClassType = Context.getTypeDeclType(Record);
  DeclarationName Name = Context.DeclarationNames.getCXXDestructorName(
      Context.getCanonicalType(ClassType));
  if (NewFD->getDeclName() != Name) {
    Diag(NewFD->getLocation(), diag::err_owned_struct_destructor_name);
    NewFD->setInvalidDecl();
    return;
  }

  std::string TypeName;
  if (isa<InjectedClassNameType>(ClassType)) {
    // handle generic owned struct type
    TypeName = ClassType.getAsString();
  } else {
    TypeName = ClassType.getBaseTypeIdentifier()->getName().str();
  }

  if (NewFD->getNumParams() == 0) {
    Diag(NewFD->getLocation(), diag::invalid_param_for_destructor) << TypeName;
    NewFD->setInvalidDecl();
    return;
  }

  bool IsEqualType = true;
  if (isa<InjectedClassNameType>(ClassType)) {
    IsEqualType = ("_Owned " + ClassType.getAsString()) ==
                  NewFD->getParamDecl(0)->getType().getAsString();
  } else {
    auto ParamType = NewFD->getParamDecl(0)->getType().getTypePtrOrNull();
    if (auto CT = ClassType.getTypePtrOrNull()) {
      IsEqualType = CT->getCanonicalTypeUnqualified().getTypePtrOrNull() ==
                    ParamType->getCanonicalTypeUnqualified().getTypePtrOrNull();
    }
  }

  if ((!IsEqualType || NewFD->getParamDecl(0)->getName() != "this")) {
    Diag(NewFD->getParamDecl(0)->getLocation(),
         diag::invalid_param_for_destructor)
        << TypeName;
    NewFD->setInvalidDecl();
    return;
  }

  if (NewFD->getNumParams() > 1) {
    Diag(NewFD->getLocation(), diag::invalid_param_num_for_destructor);
    NewFD->setInvalidDecl();
    return;
  }
}

void Sema::CheckBSCDestructorBody(FunctionDecl *NewFD) {
  if (NewFD->getBody() == nullptr) {
    Diag(NewFD->getLocation(), diag::err_owned_struct_destructor_body);
    NewFD->setInvalidDecl();
  }
}

// Collect the destructor map of non-instantiated functions.
void Sema::CollectDestructMap(StmtResult Res, Scope *BeginScope,
                              Scope *EndScope) {
  if (!getLangOpts().BSC || Res.isInvalid() || !Res.get())
    return;
  llvm::SmallVector<VarDecl *> MovedDecls;
  if (isa<ReturnStmt>(Res.get())) {
    ReturnStmt *Value = cast<ReturnStmt>(Res.get());
    // should find var
    DeclRefFinder Finder = DeclRefFinder();
    Finder.TraverseStmt(Value);
    MovedDecls = Finder.MovedDecls;
  }
  SmallVector<VarDecl *> ReturnStmts;
  while (BeginScope != EndScope->getParent()) {
    std::stack<VarDecl *> VarDeclStack =
        BeginScope->DeclsInScopeToEmitDestructorCall;
    while (!VarDeclStack.empty()) {
      VarDecl *D = VarDeclStack.top();
      bool IsTopLevelSwitchBlock =
          BeginScope->getParent()->getFlags() & Scope::SwitchScope;
      VarDeclStack.pop();
      bool IsDestructorParam = false;
      if (auto MD = dyn_cast<BSCMethodDecl>(getCurFunctionOrMethodDecl())) {
        IsDestructorParam = MD->isDestructor() && isa<ParmVarDecl>(D);
      }
      if ((std::find(MovedDecls.begin(), MovedDecls.end(), D) ==
           MovedDecls.end()) &&
          !IsTopLevelSwitchBlock && !IsDestructorParam) {
        ReturnStmts.push_back(D);
      }
    }
    BeginScope = BeginScope->getParent();
  }

  if (ReturnStmts.size() != 0) {
    Context.DestructMap[getCurFunctionOrMethodDecl()][Res.get()] = ReturnStmts;
  }
}

// Collect destructor map during function instantiation,the destructor map of
// non-instantiated functions has already been collected in the parser phase.
void Sema::CollDestructMapInFuncInstantiation(FunctionDecl *FD) {
  if (FD->getTemplatedKind() == FunctionDecl::TK_NonTemplate)
    return;
  CalcDestructorMapForIns CalcDestructorMapForInsObj(*this);
  CalcDestructorMapForInsObj.TraverseFunctionDecl(FD);
}

#endif