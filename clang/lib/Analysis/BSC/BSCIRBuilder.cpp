//===- BSCIRBuilder.cpp - AST to BSCIR lowering ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Recursive AST lowering into BSCIR. Creates blocks in encounter order and
// threads control flow directly from AST structure, then runs SimplifyCfg.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/Analysis/Analyses/BSC/BSCIRBuilder.h"
#include "clang/AST/Attr.h"
#include "clang/AST/BSC/ExprBSC.h"
#include "clang/AST/BSC/StmtBSC.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/TargetInfo.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::bscir;

//===----------------------------------------------------------------------===//
// Constructor
//===----------------------------------------------------------------------===//

BSCIRBuilder::BSCIRBuilder(ASTContext &Ctx, const FunctionDecl &FD)
    : Ctx(Ctx), FD(FD) {
  TheBody = std::make_unique<Body>();
  TheBody->SourceFD = &FD;
  TheBody->FuncSafeZone = FD.getSafeZoneSpecifier();
}

//===----------------------------------------------------------------------===//
// Safe Zone Computation
//===----------------------------------------------------------------------===//

SafeZoneSpecifier BSCIRBuilder::currentSafeZone() const {
  if (!SafeZoneStack.empty())
    return SafeZoneStack.back();
  return TheBody->FuncSafeZone;
}

//===----------------------------------------------------------------------===//
// Block Management
//===----------------------------------------------------------------------===//

BasicBlockId BSCIRBuilder::createBlock() { return TheBody->addBlock(); }

BasicBlockId BSCIRBuilder::switchToBlock(BasicBlockId NewBlock) {
  BasicBlockId Old = CurrentBlock;
  CurrentBlock = NewBlock;
  return Old;
}

void BSCIRBuilder::emit(Statement S) {
  TheBody->getBlock(CurrentBlock).Statements.push_back(std::move(S));
}

void BSCIRBuilder::setTerminator(Terminator T) {
  TheBody->getBlock(CurrentBlock).Term = std::move(T);
}

//===----------------------------------------------------------------------===//
// Local Variable Management
//===----------------------------------------------------------------------===//

LocalId BSCIRBuilder::getOrCreateLocal(const VarDecl *VD) {
  auto It = VarMap.find(VD);
  if (It != VarMap.end())
    return It->second;

  LocalId Id = TheBody->addLocal(VD->getType(), VD, VD->getName(), false,
                                 VD->getLocation());
  VarMap[VD] = Id;
  return Id;
}

//===----------------------------------------------------------------------===//
// Lowering Helpers
//===----------------------------------------------------------------------===//

bool BSCIRBuilder::shouldMove(const Expr *E) const {
  QualType Ty = E->getType();
  if (Ty.isOwnedQualified())
    return true;
  if (Ty->isRecordType()) {
    if (Ty.getTypePtr()->isOwnedStructureType() ||
        Ty->isMoveSemanticType())
      return true;
  }
  return false;
}

Place BSCIRBuilder::lowerToPlace(const Expr *E) {
  E = E->IgnoreParenImpCasts();

  if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      LocalId Id = getOrCreateLocal(VD);
      return Place(Id, VD->getType(), DRE->getLocation());
    }
  }

  if (auto *ME = dyn_cast<MemberExpr>(E)) {
    if (auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      Place Base = lowerToPlace(ME->getBase());
      if (ME->isArrow()) {
        Base = Base.project(ProjectionElem::createDeref(
            ME->getBase()->getType()->getPointeeType()),
            TheBody->getAllocator());
      }
      // Pass the member-access location so diagnostics point to the
      // leaf field (e.g. `y` in `a.y`), not the base (`a`).
      return Base.project(
          ProjectionElem::createField(FD->getFieldIndex(), FD, FD->getType()),
          TheBody->getAllocator(), ME->getMemberLoc());
    }
  }

  if (auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_Deref) {
      Place Base = lowerToPlace(UO->getSubExpr());
      return Base.project(
          ProjectionElem::createDeref(UO->getType()),
          TheBody->getAllocator(), UO->getOperatorLoc());
    }
  }

  if (auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    Place Base = lowerToPlace(ASE->getBase());
    Operand Idx = lowerToOperand(ASE->getIdx());
    if (Idx.K == Operand::Constant) {
      if (Idx.getConstVal().isInt()) {
        uint64_t CI = Idx.getConstVal().getInt().getZExtValue();
        return Base.project(
            ProjectionElem::createConstantIndex(CI, ASE->getType()),
            TheBody->getAllocator(), ASE->getExprLoc());
      }
    }
    LocalId IdxLocal = TheBody->addTemp(ASE->getIdx()->getType(),
                                        ASE->getIdx()->getExprLoc());
    Place IdxPlace(IdxLocal, ASE->getIdx()->getType(),
                   ASE->getIdx()->getExprLoc());
    emit(Statement::createAssign(IdxPlace, Rvalue::createUse(Idx),
                                 currentSafeZone()));
    return Base.project(
        ProjectionElem::createIndex(IdxLocal, ASE->getType()),
        TheBody->getAllocator(), ASE->getExprLoc());
  }

  // Fallback: create a temporary for complex lvalue expressions
  Operand Op = Visit(const_cast<Expr *>(E));
  LocalId Tmp = TheBody->addTemp(E->getType(), E->getExprLoc());
  Place TmpPlace(Tmp, E->getType(), E->getExprLoc());
  emit(Statement::createAssign(TmpPlace, Rvalue::createUse(Op),
                               currentSafeZone()));
  return TmpPlace;
}

Operand BSCIRBuilder::lowerToOperand(const Expr *E) {
  return Visit(const_cast<Expr *>(E));
}

//===----------------------------------------------------------------------===//
// Parameter Lowering
//===----------------------------------------------------------------------===//

void BSCIRBuilder::lowerParams() {
  // Local 0 = return slot
  QualType RetTy = FD.getReturnType();
  TheBody->addLocal(RetTy, nullptr, "_0", false, FD.getLocation());

  // Locals 1..N = parameters
  for (unsigned I = 0; I < FD.getNumParams(); ++I) {
    const ParmVarDecl *PD = FD.getParamDecl(I);
    LocalId Id = TheBody->addLocal(PD->getType(), PD, PD->getName(), false,
                                   PD->getLocation());
    VarMap[PD] = Id;
  }
  TheBody->NumParams = FD.getNumParams();
}

//===----------------------------------------------------------------------===//
// Label/Goto Support
//===----------------------------------------------------------------------===//

BasicBlockId BSCIRBuilder::getOrCreateLabelBlock(LabelDecl *LD) {
  auto It = LabelMap.find(LD);
  if (It != LabelMap.end())
    return It->second;
  BasicBlockId BB = createBlock();
  LabelMap[LD] = BB;
  return BB;
}

//===----------------------------------------------------------------------===//
// Scope Cleanup
//===----------------------------------------------------------------------===//

/// Check whether a type needs an explicit Drop terminator (owned pointer or
/// struct with owned fields / move semantics).
/// NOTE: file-local for now. Promote to a header if other passes need it.
static bool needsDrop(QualType Ty) {
  if (Ty->isPointerType() && Ty.isOwnedQualified())
    return true;
  if (Ty->isRecordType() &&
      (Ty.getTypePtr()->isOwnedStructureType() || Ty->isMoveSemanticType()))
    return true;
  return false;
}

void BSCIRBuilder::emitLocalsCleanup(ArrayRef<LocalId> Locals,
                                      SourceLocation Loc, CleanupKind CK) {
  for (LocalId L : llvm::reverse(Locals)) {
    const LocalDecl &LD = TheBody->getLocal(L);
    if (needsDrop(LD.Ty)) {
      SourceLocation DropLoc = Loc.isValid() ? Loc : LD.DeclLoc;
      Place P(L, LD.Ty, DropLoc);
      BasicBlockId SuccBB = createBlock();
      setTerminator(Terminator::createDrop(P, SuccBB, currentSafeZone()));
      TheBody->getBlock(CurrentBlock).Term.Loc = DropLoc;
      switchToBlock(SuccBB);
    }
    if (CK == CleanupKind::DropAndStorageDead)
      emit(Statement::createStorageDead(L, currentSafeZone()));
  }
}

void BSCIRBuilder::emitScopeCleanup(unsigned TargetDepth,
                                     SourceLocation ScopeEndLoc) {
  // Emit Drop + StorageDead for all locals in scopes above TargetDepth,
  // top-down (reverse declaration order).
  // Does NOT pop scopes — used by break/continue/goto/return where other
  // code paths still need the scope stack intact.
  for (unsigned I = ScopeStack.size(); I > TargetDepth; --I)
    emitLocalsCleanup(ScopeStack[I - 1].Locals, ScopeEndLoc);
}

void BSCIRBuilder::emitScopeExit(SourceLocation ScopeEndLoc) {
  // Emit Drop + StorageDead for the topmost scope, then pop it.
  // Used at normal scope end (compound '}', for-loop exit, switch exit).
  emitLocalsCleanup(ScopeStack.back().Locals, ScopeEndLoc);
  ScopeStack.pop_back();
}

//===----------------------------------------------------------------------===//
// Recursive Statement Lowering
//===----------------------------------------------------------------------===//

void BSCIRBuilder::lowerStmt(const Stmt *S) {
  if (!S)
    return;

  // SafeStmt: push safe zone, lower body, pop
  if (auto *SS = dyn_cast<SafeStmt>(S)) {
    SafeZoneStack.push_back(SS->getSafeZoneSpecifier());
    lowerStmt(SS->getSubStmt());
    SafeZoneStack.pop_back();
    return;
  }

  if (auto *CS = dyn_cast<CompoundStmt>(S)) {
    lowerCompoundStmt(CS);
    return;
  }

  if (auto *IS = dyn_cast<IfStmt>(S)) {
    lowerIfStmt(IS);
    return;
  }

  if (auto *WS = dyn_cast<WhileStmt>(S)) {
    lowerWhileStmt(WS);
    return;
  }

  if (auto *FS = dyn_cast<ForStmt>(S)) {
    lowerForStmt(FS);
    return;
  }

  if (auto *DS = dyn_cast<DoStmt>(S)) {
    lowerDoWhileStmt(DS);
    return;
  }

  if (auto *SS = dyn_cast<SwitchStmt>(S)) {
    lowerSwitchStmt(SS);
    return;
  }

  if (auto *BS = dyn_cast<BreakStmt>(S)) {
    lowerBreakStmt(BS);
    return;
  }

  if (auto *CS = dyn_cast<ContinueStmt>(S)) {
    lowerContinueStmt(CS);
    return;
  }

  if (auto *GS = dyn_cast<GotoStmt>(S)) {
    lowerGotoStmt(GS);
    return;
  }

  if (auto *LS = dyn_cast<LabelStmt>(S)) {
    lowerLabelStmt(LS);
    return;
  }

  if (auto *RS = dyn_cast<ReturnStmt>(S)) {
    lowerReturnStmt(RS);
    return;
  }

  if (auto *DS = dyn_cast<DeclStmt>(S)) {
    VisitDeclStmt(const_cast<DeclStmt *>(DS));
    return;
  }

  // NullStmt: bare semicolons — skip silently.
  if (isa<NullStmt>(S))
    return;

  if (auto *AS = dyn_cast<AttributedStmt>(S)) {
    lowerStmt(AS->getSubStmt());
    return;
  }

  // Expression statements
  if (auto *E = dyn_cast<Expr>(S)) {
    Visit(const_cast<Expr *>(E));
    return;
  }

  // Fallback: emit nop
  emit(Statement::createNop(currentSafeZone(), S->getBeginLoc()));
}

void BSCIRBuilder::lowerCompoundStmt(const CompoundStmt *CS) {
  // Push safe zone if this compound has one
  bool PushedSafeZone = false;
  SafeZoneSpecifier CompSZ = CS->getCompSafeZoneSpecifier();
  if (CompSZ != SZ_None) {
    SafeZoneStack.push_back(CompSZ);
    PushedSafeZone = true;
  }

  // Push scope for StorageDead tracking
  ScopeStack.push_back({});

  for (const Stmt *S : CS->body())
    lowerStmt(S);

  emitScopeExit(CS->getRBracLoc());

  if (PushedSafeZone)
    SafeZoneStack.pop_back();
}

void BSCIRBuilder::lowerIfStmt(const IfStmt *IS) {
  // For constexpr if (or any if with a compile-time constant condition),
  // only emit the taken branch to avoid false positives in init analysis.
  Expr::EvalResult ConstResult;
  if (IS->getCond()->EvaluateAsInt(ConstResult, Ctx)) {
    bool CondIsTrue = ConstResult.Val.getInt().getBoolValue();
    if (CondIsTrue) {
      lowerStmt(IS->getThen());
    } else if (IS->getElse()) {
      lowerStmt(IS->getElse());
    }
    return;
  }

  // Lower condition
  Operand Cond = lowerToOperand(IS->getCond());

  BasicBlockId ThenBB = createBlock();
  BasicBlockId ElseBB = createBlock();
  BasicBlockId JoinBB = createBlock();

  emitBoolSwitch(std::move(Cond), ThenBB, ElseBB);

  // Then branch
  switchToBlock(ThenBB);
  lowerStmt(IS->getThen());
  // If then didn't terminate (return/break/etc), goto join
  if (TheBody->getBlock(CurrentBlock).Term.K == Terminator::Unreachable)
    setTerminator(Terminator::createGoto(JoinBB, currentSafeZone()));

  // Else branch
  switchToBlock(ElseBB);
  if (IS->getElse())
    lowerStmt(IS->getElse());
  if (TheBody->getBlock(CurrentBlock).Term.K == Terminator::Unreachable)
    setTerminator(Terminator::createGoto(JoinBB, currentSafeZone()));

  // Continue from join
  switchToBlock(JoinBB);
}

void BSCIRBuilder::emitBoolSwitch(Operand Cond, BasicBlockId TrueBB,
                                  BasicBlockId FalseBB) {
  SmallVector<std::pair<llvm::APInt, BasicBlockId>, 4> Targets;
  Targets.push_back({llvm::APInt(1, 0), FalseBB});
  setTerminator(Terminator::createSwitchInt(std::move(Cond),
                                            std::move(Targets), TrueBB,
                                            currentSafeZone()));
}

void BSCIRBuilder::emitCondBranch(const Expr *Cond, BasicBlockId BodyBB,
                                      BasicBlockId ExitBB) {
  Expr::EvalResult Result;
  if (Cond->EvaluateAsInt(Result, Ctx)) {
    // Constant condition — emit direct goto, skip switchInt.
    BasicBlockId Target =
        Result.Val.getInt().getBoolValue() ? BodyBB : ExitBB;
    setTerminator(Terminator::createGoto(Target, currentSafeZone()));
  } else {
    Operand CondOp = lowerToOperand(Cond);
    emitBoolSwitch(std::move(CondOp), BodyBB, ExitBB);
  }
}

void BSCIRBuilder::lowerWhileStmt(const WhileStmt *WS) {
  BasicBlockId CondBB = createBlock();
  BasicBlockId BodyBB = createBlock();
  BasicBlockId ExitBB = createBlock();

  // Goto cond
  setTerminator(Terminator::createGoto(CondBB, currentSafeZone()));

  // Condition block
  switchToBlock(CondBB);
  emitCondBranch(WS->getCond(), BodyBB, ExitBB);

  // Body block
  // FIXME if cond always false no need to build body part. 
  // currently handled by simplify dead block removal. 
  switchToBlock(BodyBB);
  BreakableScopes.push_back(
      {ExitBB, CondBB, /*HasContinue=*/true,
       static_cast<unsigned>(ScopeStack.size())});
  lowerStmt(WS->getBody());
  BreakableScopes.pop_back();
  if (TheBody->getBlock(CurrentBlock).Term.K == Terminator::Unreachable)
    setTerminator(Terminator::createGoto(CondBB, currentSafeZone()));

  // Exit
  switchToBlock(ExitBB);
}

void BSCIRBuilder::lowerForStmt(const ForStmt *FS) {
  bool PushedInitScope = false;
  if (isa_and_nonnull<DeclStmt>(FS->getInit())) {
    ScopeStack.push_back({});
    PushedInitScope = true;
  }

  // Lower init in current block
  if (FS->getInit())
    lowerStmt(FS->getInit());

  BasicBlockId CondBB = createBlock();
  BasicBlockId BodyBB = createBlock();
  BasicBlockId IncrBB = createBlock();
  BasicBlockId ExitBB = createBlock();

  // Goto cond
  setTerminator(Terminator::createGoto(CondBB, currentSafeZone()));

  // Condition block
  switchToBlock(CondBB);
  if (!FS->getCond()) {
    // No condition = always true (for(;;))
    setTerminator(Terminator::createGoto(BodyBB, currentSafeZone()));
  } else {
    emitCondBranch(FS->getCond(), BodyBB, ExitBB);
  }

  // Body block
  switchToBlock(BodyBB);
  BreakableScopes.push_back(
      {ExitBB, IncrBB, /*HasContinue=*/true,
       static_cast<unsigned>(ScopeStack.size())});
  lowerStmt(FS->getBody());
  BreakableScopes.pop_back();
  if (TheBody->getBlock(CurrentBlock).Term.K == Terminator::Unreachable)
    setTerminator(Terminator::createGoto(IncrBB, currentSafeZone()));

  // Increment block
  switchToBlock(IncrBB);
  if (FS->getInc())
    Visit(const_cast<Expr *>(FS->getInc()));
  setTerminator(Terminator::createGoto(CondBB, currentSafeZone()));

  // Exit
  switchToBlock(ExitBB);

  // Emit Drop + StorageDead for init-declaration variables at loop exit
  if (PushedInitScope)
    emitScopeExit(FS->getRParenLoc());
}

void BSCIRBuilder::lowerDoWhileStmt(const DoStmt *DS) {
  BasicBlockId BodyBB = createBlock();
  BasicBlockId CondBB = createBlock();
  BasicBlockId ExitBB = createBlock();

  // Goto body
  setTerminator(Terminator::createGoto(BodyBB, currentSafeZone()));

  // Body block
  switchToBlock(BodyBB);
  BreakableScopes.push_back(
      {ExitBB, CondBB, /*HasContinue=*/true,
       static_cast<unsigned>(ScopeStack.size())});
  lowerStmt(DS->getBody());
  BreakableScopes.pop_back();
  if (TheBody->getBlock(CurrentBlock).Term.K == Terminator::Unreachable)
    setTerminator(Terminator::createGoto(CondBB, currentSafeZone()));

  // Condition block
  switchToBlock(CondBB);
  emitCondBranch(DS->getCond(), BodyBB, ExitBB);

  // Exit
  switchToBlock(ExitBB);
}

void BSCIRBuilder::lowerSwitchStmt(const SwitchStmt *SS) {
  // Lower discriminant
  Operand Discr = lowerToOperand(SS->getCond());

  BasicBlockId ExitBB = createBlock();

  // A case region groups one or more case/default labels with the body
  // statements that follow them until the next case/default label.
  struct CaseRegion {
    SmallVector<llvm::APInt, 2> CaseValues; // integer values for case labels
    bool IsDefault = false;                  // true if this region has a default
    SmallVector<const Stmt *, 4> Body;       // statements in this region
  };

  SmallVector<CaseRegion, 8> Regions;

  // Helper: unwrap nested CaseStmt/DefaultStmt labels and collect their values,
  // returning the innermost non-case/default sub-statement.
  std::function<const Stmt *(const Stmt *, CaseRegion &)> unwrapLabels =
      [&](const Stmt *S, CaseRegion &R) -> const Stmt * {
    if (auto *CS = dyn_cast<CaseStmt>(S)) {
      Expr::EvalResult Result;
      if (CS->getLHS()->EvaluateAsInt(Result, Ctx))
        R.CaseValues.push_back(Result.Val.getInt());
      return unwrapLabels(CS->getSubStmt(), R);
    }
    if (auto *DS = dyn_cast<DefaultStmt>(S)) {
      R.IsDefault = true;
      return unwrapLabels(DS->getSubStmt(), R);
    }
    return S; // not a label
  };

  const Stmt *Body = SS->getBody();
  SmallVector<const Stmt *, 8> BodyStmts;
  if (auto *CS = dyn_cast<CompoundStmt>(Body))
    BodyStmts.append(CS->body_begin(), CS->body_end());
  else if (Body)
    BodyStmts.push_back(Body);

  for (const Stmt *S : BodyStmts) {
    if (isa<CaseStmt>(S) || isa<DefaultStmt>(S)) {
      // Start a new region
      Regions.push_back({});
      CaseRegion &R = Regions.back();
      const Stmt *Inner = unwrapLabels(S, R);
      // The innermost sub-statement is the first body statement
      if (Inner)
        R.Body.push_back(Inner);
    } else {
      // Non-label statement: add to current region's body
      if (!Regions.empty())
        Regions.back().Body.push_back(S);
      // else: statements before any case label — unreachable in valid C
    }
  }

  // Create a block for each region + find default
  SmallVector<BasicBlockId, 8> RegionBlocks;
  BasicBlockId DefaultBB = ExitBB; // fallback if no default label
  SmallVector<std::pair<llvm::APInt, BasicBlockId>, 4> SwitchTargets;

  for (unsigned I = 0; I < Regions.size(); ++I) {
    BasicBlockId BB = createBlock();
    RegionBlocks.push_back(BB);

    for (auto &Val : Regions[I].CaseValues)
      SwitchTargets.push_back({Val, BB});

    if (Regions[I].IsDefault)
      DefaultBB = BB;
  }

  // Set switch terminator on the current block
  setTerminator(Terminator::createSwitchInt(std::move(Discr),
                                            std::move(SwitchTargets), DefaultBB,
                                            currentSafeZone()));

  // Push a scope for the switch body. Declarations in case regions (without
  // explicit braces) are scoped to the switch body; StorageDead for these
  // locals must appear at switch exit, not function exit.
  ScopeStack.push_back({});

  // Lower each region's body
  for (unsigned I = 0; I < Regions.size(); ++I) {
    switchToBlock(RegionBlocks[I]);
    BreakableScopes.push_back(
        {ExitBB, {0}, /*HasContinue=*/false,
         static_cast<unsigned>(ScopeStack.size())});

    for (const Stmt *S : Regions[I].Body)
      lowerStmt(S);

    BreakableScopes.pop_back();

    // If unterminated, fall through to next region (or exit for last)
    if (TheBody->getBlock(CurrentBlock).Term.K == Terminator::Unreachable) {
      BasicBlockId FallTarget =
          (I + 1 < Regions.size()) ? RegionBlocks[I + 1] : ExitBB;
      setTerminator(Terminator::createGoto(FallTarget, currentSafeZone()));
    }
  }

  // Exit: emit Drop + StorageDead for switch-body locals, then pop the scope.
  switchToBlock(ExitBB);
  emitScopeExit(SS->getEndLoc());
}

void BSCIRBuilder::lowerBreakStmt(const BreakStmt *BS) {
  if (BreakableScopes.empty())
    return;
  emitScopeCleanup(BreakableScopes.back().ScopeDepth, BS->getBreakLoc());
  setTerminator(Terminator::createGoto(BreakableScopes.back().BreakTarget,
                                       currentSafeZone()));
  // Create fresh unreachable block for dead code after break
  BasicBlockId DeadBB = createBlock();
  switchToBlock(DeadBB);
}

void BSCIRBuilder::lowerContinueStmt(const ContinueStmt *CS) {
  // Walk up the breakable scope stack to find the nearest loop (HasContinue).
  // Switch statements push scopes with HasContinue=false, so we must skip them.
  for (int I = BreakableScopes.size() - 1; I >= 0; --I) {
    const auto &Scope = BreakableScopes[I];
    if (Scope.HasContinue) {
      emitScopeCleanup(Scope.ScopeDepth, CS->getContinueLoc());
      setTerminator(Terminator::createGoto(Scope.ContinueTarget,
                                           currentSafeZone()));
      break;
    }
  }
  // Create fresh unreachable block for dead code after continue
  BasicBlockId DeadBB = createBlock();
  switchToBlock(DeadBB);
}

void BSCIRBuilder::lowerGotoStmt(const GotoStmt *GS) {
  BasicBlockId TargetBB = getOrCreateLabelBlock(GS->getLabel());
  // Emit StorageDead for locals between current scope and the label's scope.
  // prescanLabels() has already recorded every label's depth, so the lookup
  // always succeeds for valid programs.
  auto It = LabelScopeDepth.find(GS->getLabel());
  unsigned TargetDepth = (It != LabelScopeDepth.end())
                             ? It->second
                             : ScopeStack.size(); // no cleanup if unknown
  emitScopeCleanup(TargetDepth, GS->getGotoLoc());
  setTerminator(Terminator::createGoto(TargetBB, currentSafeZone()));
  // Create fresh block for dead code after goto
  BasicBlockId DeadBB = createBlock();
  switchToBlock(DeadBB);
}

void BSCIRBuilder::enterLabelBlock(const LabelStmt *LS) {
  BasicBlockId LabelBB = getOrCreateLabelBlock(LS->getDecl());
  // Record scope depth at the label for goto cleanup
  LabelScopeDepth[LS->getDecl()] = ScopeStack.size();
  // Terminate current block with goto to label block
  if (TheBody->getBlock(CurrentBlock).Term.K == Terminator::Unreachable)
    setTerminator(Terminator::createGoto(LabelBB, currentSafeZone()));
  switchToBlock(LabelBB);
}

void BSCIRBuilder::lowerLabelStmt(const LabelStmt *LS) {
  enterLabelBlock(LS);
  // Lower the sub-statement
  lowerStmt(LS->getSubStmt());
}

void BSCIRBuilder::lowerReturnStmt(const ReturnStmt *RS) {
  // Lower return value and assign to _0
  VisitReturnStmt(const_cast<ReturnStmt *>(RS));
  // Emit Drop + StorageDead for all locals in all scopes.
  // Use the return statement's location for drop diagnostics.
  emitScopeCleanup(0, RS->getReturnLoc());
  // Emit Drop for owned parameters (not tracked in ScopeStack).
  // Drops are emitted unconditionally; if the parameter was already moved
  // (e.g., returned via `_0 = move(_1)`), the ownership analysis sees the
  // Moved state at the drop point and treats it as a no-op.
  SmallVector<LocalId, 4> ParamLocals;
  for (unsigned I = TheBody->NumParams; I >= 1; --I)
    ParamLocals.push_back(LocalId{I});
  emitLocalsCleanup(ParamLocals, RS->getReturnLoc(), CleanupKind::DropOnly);
  // Goto return block
  setTerminator(Terminator::createGoto(ReturnBlock, currentSafeZone()));
  // Create fresh block for dead code after return
  BasicBlockId DeadBB = createBlock();
  switchToBlock(DeadBB);
}

//===----------------------------------------------------------------------===//
// StmtVisitor Methods
//===----------------------------------------------------------------------===//

Operand BSCIRBuilder::VisitDeclRefExpr(DeclRefExpr *E) {
  if (auto *VD = dyn_cast<VarDecl>(E->getDecl())) {
    LocalId Id = getOrCreateLocal(VD);
    Place P(Id, VD->getType(), E->getLocation());
    if (shouldMove(E))
      return Operand::createMove(P);
    return Operand::createCopy(P);
  }
  return Operand::createConstant(APValue(), E->getType());
}

Operand BSCIRBuilder::VisitIntegerLiteral(IntegerLiteral *E) {
  return Operand::createConstant(APValue(llvm::APSInt(E->getValue(),
                                                E->getType()->isUnsignedIntegerType())),
                                 E->getType());
}

Operand BSCIRBuilder::VisitFloatingLiteral(FloatingLiteral *E) {
  return Operand::createConstant(APValue(E->getValue()), E->getType());
}

Operand BSCIRBuilder::VisitCharacterLiteral(CharacterLiteral *E) {
  return Operand::createConstant(
      APValue(llvm::APSInt(llvm::APInt(32, E->getValue()), true)), E->getType());
}

Operand BSCIRBuilder::VisitStringLiteral(StringLiteral *E) {
  // getString() asserts on wide strings (L"...", u"...", U"...").
  if (E->getCharByteWidth() == 1)
    return Operand::createStringConstant(E->getString(), E->getType());
  // Wide string: treat as opaque constant.
  return Operand::createConstant(APValue(), E->getType());
}

Operand BSCIRBuilder::VisitBinaryOperator(BinaryOperator *BO) {
  if (BO->isAssignmentOp()) {
    Place Dest = lowerToPlace(BO->getLHS());
    Operand Src = lowerToOperand(BO->getRHS());

    if (BO->isCompoundAssignmentOp()) {
      Operand DestOp = Operand::createCopy(Dest);
      BinaryOperatorKind CompOp =
          BinaryOperator::getOpForCompoundAssignment(BO->getOpcode());
      LocalId Tmp = TheBody->addTemp(BO->getType(), BO->getExprLoc());
      Place TmpPlace(Tmp, BO->getType(), BO->getExprLoc());
      emit(Statement::createAssign(
          TmpPlace, Rvalue::createBinaryOp(CompOp, DestOp, Src),
          currentSafeZone(), BO, BO->getExprLoc()));
      emit(Statement::createAssign(Dest, Rvalue::createUse(
                                              Operand::createCopy(TmpPlace)),
                                   currentSafeZone(), BO, BO->getExprLoc()));
      return Operand::createCopy(Dest);
    }

    emit(Statement::createAssign(Dest, Rvalue::createUse(Src),
                                 currentSafeZone(), BO, BO->getExprLoc()));
    return Operand::createCopy(Dest);
  }

  // Short-circuit logical operators: && and ||
  if (BO->getOpcode() == BO_LAnd || BO->getOpcode() == BO_LOr) {
    bool IsAnd = (BO->getOpcode() == BO_LAnd);

    // Eval LHS in current block
    Operand LHS = lowerToOperand(BO->getLHS());

    // Result temporary
    LocalId Result = TheBody->addTemp(BO->getType(), BO->getExprLoc());
    Place ResultPlace(Result, BO->getType(), BO->getExprLoc());

    BasicBlockId ShortBB = createBlock();  // short-circuit block
    BasicBlockId RhsBB = createBlock();    // evaluate RHS block
    BasicBlockId JoinBB = createBlock();   // join block

    // For &&: if LHS truthy → eval RHS, otherwise short-circuit to 0
    // For ||: if LHS truthy → short-circuit to 1, otherwise eval RHS
    emitBoolSwitch(std::move(LHS),
                   IsAnd ? RhsBB : ShortBB,
                   IsAnd ? ShortBB : RhsBB);

    // Short-circuit block: result = 0 (&&) or 1 (||)
    switchToBlock(ShortBB);
    int ShortVal = IsAnd ? 0 : 1;
    Operand ShortOp = Operand::createConstant(
        APValue(llvm::APSInt(llvm::APInt(32, ShortVal), false)), BO->getType());
    emit(Statement::createAssign(ResultPlace, Rvalue::createUse(ShortOp),
                                 currentSafeZone(), BO, BO->getExprLoc()));
    setTerminator(Terminator::createGoto(JoinBB, currentSafeZone()));

    // RHS block: eval RHS, assign to result
    switchToBlock(RhsBB);
    Operand RHS = lowerToOperand(BO->getRHS());
    emit(Statement::createAssign(ResultPlace, Rvalue::createUse(RHS),
                                 currentSafeZone(), BO, BO->getExprLoc()));
    setTerminator(Terminator::createGoto(JoinBB, currentSafeZone()));

    // Continue in join block
    switchToBlock(JoinBB);
    return Operand::createCopy(ResultPlace);
  }

  // Comma operator: evaluate LHS for side effects, return RHS.
  if (BO->getOpcode() == BO_Comma) {
    lowerToOperand(BO->getLHS());   // side effects only
    return lowerToOperand(BO->getRHS());
  }

  Operand LHS = lowerToOperand(BO->getLHS());
  Operand RHS = lowerToOperand(BO->getRHS());

  // For comparison operators, force copy — comparing reads values without
  // consuming ownership. Convert any Move to Copy.
  if (BO->isComparisonOp() || BO->isEqualityOp()) {
    if (LHS.K == Operand::Move)
      LHS = Operand::createCopy(LHS.getPlace());
    if (RHS.K == Operand::Move)
      RHS = Operand::createCopy(RHS.getPlace());
  }

  LocalId Tmp = TheBody->addTemp(BO->getType(), BO->getExprLoc());
  Place TmpPlace(Tmp, BO->getType(), BO->getExprLoc());
  emit(Statement::createAssign(
      TmpPlace, Rvalue::createBinaryOp(BO->getOpcode(), LHS, RHS),
      currentSafeZone(), BO, BO->getExprLoc()));
  return Operand::createCopy(TmpPlace);
}

Operand BSCIRBuilder::VisitUnaryOperator(UnaryOperator *UO) {
  UnaryOperatorKind Op = UO->getOpcode();

  // BSC borrow operators
  if (Op == UO_AddrMut || Op == UO_AddrMutDeref) {
    Place P = lowerToPlace(UO->getSubExpr());
    unsigned RId = NextRegionId++;
    if (Op == UO_AddrMutDeref) {
      LocalId Tmp = TheBody->addTemp(UO->getType(), UO->getExprLoc());
      Place TmpPlace(Tmp, UO->getType(), UO->getExprLoc());
      emit(Statement::createAssign(
          TmpPlace,
          Rvalue::createRef(BorrowKind::Mut, P, /*IsReborrow=*/true, RId),
          currentSafeZone(), UO, UO->getExprLoc()));
      return Operand::createCopy(TmpPlace);
    }
    LocalId Tmp = TheBody->addTemp(UO->getType(), UO->getExprLoc());
    Place TmpPlace(Tmp, UO->getType(), UO->getExprLoc());
    AddrOfOrigins[Tmp] = P;
    emit(Statement::createAssign(
        TmpPlace, Rvalue::createRef(BorrowKind::Mut, P, /*IsReborrow=*/false, RId),
        currentSafeZone(), UO, UO->getExprLoc()));
    return Operand::createCopy(TmpPlace);
  }

  if (Op == UO_AddrConst || Op == UO_AddrConstDeref) {
    Place P = lowerToPlace(UO->getSubExpr());
    bool IsReborrow = (Op == UO_AddrConstDeref);
    unsigned RId = NextRegionId++;
    LocalId Tmp = TheBody->addTemp(UO->getType(), UO->getExprLoc());
    Place TmpPlace(Tmp, UO->getType(), UO->getExprLoc());
    if (!IsReborrow)
      AddrOfOrigins[Tmp] = P;
    emit(Statement::createAssign(
        TmpPlace, Rvalue::createRef(BorrowKind::Shared, P, IsReborrow, RId),
        currentSafeZone(), UO, UO->getExprLoc()));
    return Operand::createCopy(TmpPlace);
  }

  // C-style address-of
  // C-style &: no RegionId — raw pointers don't participate in borrow checking.
  if (Op == UO_AddrOf) {
    Place P = lowerToPlace(UO->getSubExpr());
    LocalId Tmp = TheBody->addTemp(UO->getType(), UO->getExprLoc());
    Place TmpPlace(Tmp, UO->getType(), UO->getExprLoc());
    AddrOfOrigins[Tmp] = P;
    emit(Statement::createAssign(
        TmpPlace, Rvalue::createAddressOf(P),
        currentSafeZone(), UO, UO->getExprLoc()));
    return Operand::createCopy(TmpPlace);
  }

  // Dereference: reads through the pointer but does NOT consume it.
  // Force a copy even if the pointer is _Owned.
  if (Op == UO_Deref) {
    Place SubPlace = lowerToPlace(UO->getSubExpr());
    Operand Sub = Operand::createCopy(SubPlace);
    LocalId Tmp = TheBody->addTemp(UO->getSubExpr()->getType(),
                                   UO->getExprLoc());
    Place TmpPlace(Tmp, UO->getSubExpr()->getType(), UO->getExprLoc());
    emit(Statement::createAssign(TmpPlace, Rvalue::createUse(Sub),
                                 currentSafeZone(), UO, UO->getExprLoc()));
    Place Derefed = TmpPlace.project(
        ProjectionElem::createDeref(UO->getType()),
        TheBody->getAllocator());
    return Operand::createCopy(Derefed);
  }

  // Pre/Post increment/decrement: decompose into BinOp + write-back
  if (Op == UO_PreInc || Op == UO_PreDec ||
      Op == UO_PostInc || Op == UO_PostDec) {
    bool IsInc = (Op == UO_PreInc || Op == UO_PostInc);
    bool IsPre = (Op == UO_PreInc || Op == UO_PreDec);

    Place Dest = lowerToPlace(UO->getSubExpr());

    // Build the "1" constant with the correct bit-width for the operand type.
    // For pointer types, use the target pointer width; for integers, use the
    // type's actual width so downstream analyses see consistent bit-widths.
    QualType OpTy = UO->getSubExpr()->getType();
    unsigned BitWidth = OpTy->isPointerType()
        ? Ctx.getTargetInfo().getPointerWidth(static_cast<unsigned>(LangAS::Default))
        : Ctx.getTypeSize(OpTy);
    bool IsUnsigned = OpTy->isUnsignedIntegerType() || OpTy->isPointerType();
    Operand One = Operand::createConstant(
        APValue(llvm::APSInt(llvm::APInt(BitWidth, 1), IsUnsigned)),
        UO->getType());

    if (!IsPre) {
      // Post: save old value first
      LocalId Old = TheBody->addTemp(UO->getType(), UO->getExprLoc());
      Place OldPlace(Old, UO->getType(), UO->getExprLoc());
      emit(Statement::createAssign(OldPlace,
                                   Rvalue::createUse(Operand::createCopy(Dest)),
                                   currentSafeZone(), UO, UO->getExprLoc()));

      // new = old +/- 1
      LocalId New = TheBody->addTemp(UO->getType(), UO->getExprLoc());
      Place NewPlace(New, UO->getType(), UO->getExprLoc());
      emit(Statement::createAssign(
          NewPlace,
          Rvalue::createBinaryOp(IsInc ? BO_Add : BO_Sub,
                                 Operand::createCopy(Dest), One),
          currentSafeZone(), UO, UO->getExprLoc()));

      // write back: dest = new
      emit(Statement::createAssign(Dest,
                                   Rvalue::createUse(Operand::createCopy(NewPlace)),
                                   currentSafeZone(), UO, UO->getExprLoc()));

      // Post returns the old value
      return Operand::createCopy(OldPlace);
    }

    // Pre: new = old +/- 1, write back, return new
    LocalId New = TheBody->addTemp(UO->getType(), UO->getExprLoc());
    Place NewPlace(New, UO->getType(), UO->getExprLoc());
    emit(Statement::createAssign(
        NewPlace,
        Rvalue::createBinaryOp(IsInc ? BO_Add : BO_Sub,
                               Operand::createCopy(Dest), One),
        currentSafeZone(), UO, UO->getExprLoc()));

    // write back: dest = new
    emit(Statement::createAssign(Dest,
                                 Rvalue::createUse(Operand::createCopy(NewPlace)),
                                 currentSafeZone(), UO, UO->getExprLoc()));

    return Operand::createCopy(NewPlace);
  }

  // Other unary ops
  Operand Sub = lowerToOperand(UO->getSubExpr());
  LocalId Tmp = TheBody->addTemp(UO->getType(), UO->getExprLoc());
  Place TmpPlace(Tmp, UO->getType(), UO->getExprLoc());
  emit(Statement::createAssign(
      TmpPlace, Rvalue::createUnaryOp(Op, Sub),
      currentSafeZone(), UO, UO->getExprLoc()));
  return Operand::createCopy(TmpPlace);
}

Operand BSCIRBuilder::VisitCallExpr(CallExpr *CE) {
  const FunctionDecl *CalleeDecl = CE->getDirectCallee();

  // For direct calls, the callee is a constant placeholder (the Decl carries
  // the identity). For indirect calls (function pointers), lower the callee
  // expression so that the function pointer local is preserved in the IR.
  Operand Callee = CalleeDecl
      ? Operand::createConstant(APValue(), CE->getCallee()->getType())
      : lowerToOperand(CE->getCallee());

  // Extract FunctionProtoType from callee for indirect call ensure_init support.
  const FunctionProtoType *CalleeProtoType = nullptr;
  {
    QualType CalleeType = CE->getCallee()->getType();
    if (const auto *FnPtr = CalleeType->getAs<PointerType>())
      CalleeProtoType = FnPtr->getPointeeType()->getAs<FunctionProtoType>();
    else
      CalleeProtoType = CalleeType->getAs<FunctionProtoType>();
  }

  SmallVector<Operand, 4> Args;
  SmallVector<llvm::Optional<Place>, 4> ArgPlaces;
  for (unsigned I = 0; I < CE->getNumArgs(); ++I) {
    Operand Arg = lowerToOperand(CE->getArg(I));
    if (shouldMove(CE->getArg(I)) && Arg.K == Operand::Copy) {
      Arg.K = Operand::Move;
    }
    // Capture address-of origin for ensure_init support
    if (Arg.K == Operand::Copy || Arg.K == Operand::Move) {
      auto It = AddrOfOrigins.find(Arg.getPlace().Base);
      if (It != AddrOfOrigins.end()) {
        ArgPlaces.push_back(It->second);
        Args.push_back(std::move(Arg));
        continue;
      }
    }
    ArgPlaces.push_back(llvm::None);
    Args.push_back(std::move(Arg));
  }

  LocalId Dest = TheBody->addTemp(CE->getType(), CE->getExprLoc());
  Place DestPlace(Dest, CE->getType(), CE->getExprLoc());

  bool Diverges = false;
  if (CalleeDecl && CalleeDecl->isNoReturn())
    Diverges = true;
  if (!Diverges && CE->getCallee()->getType()->getAs<FunctionProtoType>()) {
    if (CE->getCallee()->getType()->getAs<FunctionProtoType>()->getNoReturnAttr())
      Diverges = true;
  }

  BasicBlockId Successor = createBlock();

  setTerminator(Terminator::createCall(
      std::move(Callee), std::move(Args), DestPlace, Successor,
      CalleeDecl, currentSafeZone(), CE, CE->getExprLoc(), Diverges,
      std::move(ArgPlaces), CalleeProtoType));

  switchToBlock(Successor);

  return Operand::createCopy(DestPlace);
}

Operand BSCIRBuilder::VisitCastExpr(CastExpr *CE) {
  if (CE->getCastKind() == CK_NoOp ||
      CE->getCastKind() == CK_LValueToRValue) {
    return lowerToOperand(CE->getSubExpr());
  }

  // If casting to a non-pointer type (e.g., _Owned ptr to _Bool), force copy.
  // The cast doesn't transfer ownership — it just reads the pointer value.
  Operand Sub = [&]() -> Operand {
    QualType DestTy = CE->getType();
    QualType SrcTy = CE->getSubExpr()->getType();
    if (SrcTy->isPointerType() && SrcTy.isOwnedQualified() &&
        !DestTy->isPointerType()) {
      Place P = lowerToPlace(CE->getSubExpr());
      return Operand::createCopy(P);
    }
    return lowerToOperand(CE->getSubExpr());
  }();
  LocalId Tmp = TheBody->addTemp(CE->getType(), CE->getExprLoc());
  Place TmpPlace(Tmp, CE->getType(), CE->getExprLoc());
  // Propagate AddrOfOrigins through pointer casts (e.g., int* -> void*)
  // so that ensure_init tracking can see through implicit casts.
  if (Sub.K == Operand::Copy || Sub.K == Operand::Move) {
    auto It = AddrOfOrigins.find(Sub.getPlace().Base);
    if (It != AddrOfOrigins.end())
      AddrOfOrigins[Tmp] = It->second;
  }
  emit(Statement::createAssign(
      TmpPlace, Rvalue::createCast(CE->getCastKind(), Sub, CE->getType()),
      currentSafeZone(), CE, CE->getExprLoc()));
  return Operand::createCopy(TmpPlace);
}

Operand BSCIRBuilder::VisitImplicitCastExpr(ImplicitCastExpr *CE) {
  return VisitCastExpr(CE);
}

// Walk array-of-array chains looking for a variable-length array with a
// size expression, the same way CFG construction does.
static const VariableArrayType *findVLA(const Type *T) {
  while (const auto *AT = dyn_cast<ArrayType>(T)) {
    if (const auto *VAT = dyn_cast<VariableArrayType>(AT))
      if (VAT->getSizeExpr())
        return VAT;
    T = AT->getElementType().getTypePtr();
  }
  return nullptr;
}

void BSCIRBuilder::lowerVLASizeExprs(QualType T, const Stmt *S) {
  for (const VariableArrayType *VA = findVLA(T.getTypePtr()); VA;
       VA = findVLA(VA->getElementType().getTypePtr())) {
    const Expr *Size = VA->getSizeExpr();
    Operand SizeOp = lowerToOperand(Size);
    LocalId Tmp = TheBody->addTemp(Size->getType(), Size->getExprLoc());
    Place TmpPlace(Tmp, Size->getType(), Size->getExprLoc());
    emit(Statement::createAssign(TmpPlace, Rvalue::createUse(SizeOp),
                                 currentSafeZone(), S, Size->getExprLoc()));
  }
}

Operand BSCIRBuilder::VisitDeclStmt(DeclStmt *DS) {
  for (auto *D : DS->decls()) {
    if (auto *VD = dyn_cast<VarDecl>(D)) {
      // VLA size expressions are evaluated when the declaration is reached
      lowerVLASizeExprs(VD->getType(), DS);
      LocalId Id = getOrCreateLocal(VD);
      emit(Statement::createStorageLive(Id, currentSafeZone(),
                                        VD->getLocation()));

      // Track this local in the current scope for StorageDead
      if (!ScopeStack.empty())
        ScopeStack.back().Locals.push_back(Id);

      if (VD->hasInit()) {
        const Expr *Init = VD->getInit();
        Operand InitOp = lowerToOperand(Init);
        Place Dest(Id, VD->getType(), VD->getLocation());
        emit(Statement::createAssign(Dest, Rvalue::createUse(InitOp),
                                     currentSafeZone(), DS,
                                     VD->getLocation()));
      }
    } else if (auto *TND = dyn_cast<TypedefNameDecl>(D)) {
      // typedef int b[n]; — the size expression is evaluated here
      lowerVLASizeExprs(TND->getUnderlyingType(), DS);
    }
  }
  return Operand::createConstant(APValue(), Ctx.VoidTy);
}

Operand BSCIRBuilder::VisitReturnStmt(ReturnStmt *RS) {
  if (RS->getRetValue()) {
    Operand RetVal = lowerToOperand(RS->getRetValue());
    Place RetPlace(LocalId{0}, FD.getReturnType(), RS->getReturnLoc());
    emit(Statement::createAssign(RetPlace, Rvalue::createUse(RetVal),
                                 currentSafeZone(), RS,
                                 RS->getReturnLoc()));
  }
  return Operand::createConstant(APValue(), Ctx.VoidTy);
}

Operand BSCIRBuilder::VisitMemberExpr(MemberExpr *ME) {
  if (isa<FieldDecl>(ME->getMemberDecl())) {
    Place P = lowerToPlace(ME);
    if (shouldMove(ME))
      return Operand::createMove(P);
    return Operand::createCopy(P);
  }
  return Operand::createConstant(APValue(), ME->getType());
}

Operand BSCIRBuilder::VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
  Place P = lowerToPlace(ASE);
  return Operand::createCopy(P);
}

Operand BSCIRBuilder::VisitVAArgExpr(VAArgExpr *E) {
  // va_arg(args, type): lower the va_list sub-expression, produce a typed temp.
  lowerToOperand(E->getSubExpr());
  LocalId Tmp = TheBody->addTemp(E->getType(), E->getExprLoc());
  Place TmpPlace(Tmp, E->getType(), E->getExprLoc());
  emit(Statement::createAssign(
      TmpPlace, Rvalue::createUse(Operand::createConstant(APValue(), E->getType())),
      currentSafeZone(), E, E->getExprLoc()));
  return Operand::createCopy(TmpPlace);
}

Operand BSCIRBuilder::VisitPredefinedExpr(PredefinedExpr *E) {
  // __func__, __FUNCTION__, __PRETTY_FUNCTION__ — treat as string constant.
  if (auto *SL = E->getFunctionName())
    return VisitStringLiteral(const_cast<StringLiteral *>(SL));
  return Operand::createConstant(APValue(), E->getType());
}

Operand BSCIRBuilder::VisitCompoundLiteralExpr(CompoundLiteralExpr *E) {
  // (struct S){1, 2} — lower through the inner initializer.
  return lowerToOperand(E->getInitializer());
}

Operand BSCIRBuilder::VisitAtomicExpr(AtomicExpr *E) {
  // __atomic_load_n etc. — model as a Call terminator.
  SmallVector<Operand, 4> Args;
  for (unsigned I = 0; I < E->getNumSubExprs(); ++I) {
    Operand Arg = lowerToOperand(E->getSubExprs()[I]);
    Args.push_back(std::move(Arg));
  }

  LocalId Dest = TheBody->addTemp(E->getType(), E->getExprLoc());
  Place DestPlace(Dest, E->getType(), E->getExprLoc());
  BasicBlockId Successor = createBlock();

  // Build callee name from the AtomicOp enum.
  static const char *AtomicOpNames[] = {
#define BUILTIN(ID, TYPE, ATTRS)
#define ATOMIC_BUILTIN(ID, TYPE, ATTRS) #ID,
#include "clang/Basic/Builtins.def"
  };
  std::string CalleeName = AtomicOpNames[E->getOp()];

  Operand Callee = Operand::createConstant(APValue(), E->getType());
  auto T = Terminator::createCall(
      std::move(Callee), std::move(Args), DestPlace, Successor,
      /*FD=*/nullptr, currentSafeZone(), E, E->getExprLoc());
  T.getCall().CalleeName = std::move(CalleeName);
  setTerminator(std::move(T));

  switchToBlock(Successor);
  return Operand::createCopy(DestPlace);
}

Operand BSCIRBuilder::VisitImplicitValueInitExpr(ImplicitValueInitExpr *E) {
  // Zero-initialized field in an aggregate — just emit a constant, no nop.
  return Operand::createConstant(APValue(), E->getType());
}

Operand BSCIRBuilder::VisitInitListExpr(InitListExpr *ILE) {
  SmallVector<Operand, 4> Fields;
  for (unsigned I = 0; I < ILE->getNumInits(); ++I) {
    Operand FieldOp = lowerToOperand(ILE->getInit(I));
    if (shouldMove(ILE->getInit(I)) && FieldOp.K == Operand::Copy)
      FieldOp.K = Operand::Move;
    Fields.push_back(std::move(FieldOp));
  }
  LocalId Tmp = TheBody->addTemp(ILE->getType(), ILE->getExprLoc());
  Place TmpPlace(Tmp, ILE->getType(), ILE->getExprLoc());
  Rvalue RV;
  if (auto *RD = ILE->getType()->getAsRecordDecl()) {
    RV = Rvalue::createAggregate(RD, std::move(Fields));
  } else {
    QualType ElemTy;
    if (const auto *AT = ILE->getType()->getAsArrayTypeUnsafe())
      ElemTy = AT->getElementType();
    else
      ElemTy = ILE->getType();
    RV = Rvalue::createArray(ElemTy, std::move(Fields));
  }
  emit(Statement::createAssign(TmpPlace, std::move(RV), currentSafeZone(), ILE,
                               ILE->getExprLoc()));
  return Operand::createCopy(TmpPlace);
}

Operand BSCIRBuilder::VisitUnaryExprOrTypeTraitExpr(
    UnaryExprOrTypeTraitExpr *E) {
  if (E->getKind() == UETT_SizeOf) {
    QualType ArgTy;
    if (E->isArgumentType()) {
      ArgTy = E->getArgumentType();
      // sizeof of a VLA type evaluates its size expressions.
      lowerVLASizeExprs(ArgTy, E);
    } else {
      ArgTy = E->getArgumentExpr()->getType();
    }
    LocalId Tmp = TheBody->addTemp(E->getType(), E->getExprLoc());
    Place TmpPlace(Tmp, E->getType(), E->getExprLoc());
    emit(Statement::createAssign(
        TmpPlace, Rvalue::createSizeOf(ArgTy),
        currentSafeZone(), E, E->getExprLoc()));
    return Operand::createCopy(TmpPlace);
  }
  return Operand::createConstant(APValue(), E->getType());
}

Operand BSCIRBuilder::VisitParenExpr(ParenExpr *PE) {
  return lowerToOperand(PE->getSubExpr());
}

Operand BSCIRBuilder::VisitCompoundAssignOperator(CompoundAssignOperator *CAO) {
  return VisitBinaryOperator(CAO);
}

Operand BSCIRBuilder::VisitOpaqueValueExpr(OpaqueValueExpr *OVE) {
  auto It = OpaqueValueMap.find(OVE);
  if (It == OpaqueValueMap.end())
    return VisitStmt(OVE);
  return Operand::createCopy(
      Place(It->second, OVE->getType(), OVE->getExprLoc()));
}

Operand BSCIRBuilder::VisitAbstractConditionalOperator(
    AbstractConditionalOperator *CO) {
  // `a ?: b` evaluates `a` once; the opaque value resolves to that temp.
  if (auto *BCO = dyn_cast<BinaryConditionalOperator>(CO)) {
    Expr *Common = BCO->getCommon();
    Operand CommonVal = lowerToOperand(Common);
    LocalId CommonTmp =
        TheBody->addTemp(Common->getType(), Common->getExprLoc());
    Place CommonPlace(CommonTmp, Common->getType(), Common->getExprLoc());
    emit(Statement::createAssign(CommonPlace, Rvalue::createUse(CommonVal),
                                 currentSafeZone(), Common,
                                 Common->getExprLoc()));
    OpaqueValueMap[BCO->getOpaqueValue()] = CommonTmp;
  }

  // Lower condition in current block
  Operand Cond = lowerToOperand(CO->getCond());

  // Result temporary — shared across both branches
  LocalId Result = TheBody->addTemp(CO->getType(), CO->getExprLoc());
  Place ResultPlace(Result, CO->getType(), CO->getExprLoc());

  BasicBlockId ThenBB = createBlock();
  BasicBlockId ElseBB = createBlock();
  BasicBlockId JoinBB = createBlock();

  emitBoolSwitch(std::move(Cond), ThenBB, ElseBB);

  // Then branch: eval true expr, assign to result
  switchToBlock(ThenBB);
  Operand TrueOp = lowerToOperand(CO->getTrueExpr());
  emit(Statement::createAssign(ResultPlace, Rvalue::createUse(TrueOp),
                               currentSafeZone(), CO, CO->getExprLoc()));
  setTerminator(Terminator::createGoto(JoinBB, currentSafeZone()));

  // Else branch: eval false expr, assign to result
  switchToBlock(ElseBB);
  Operand FalseOp = lowerToOperand(CO->getFalseExpr());
  emit(Statement::createAssign(ResultPlace, Rvalue::createUse(FalseOp),
                               currentSafeZone(), CO, CO->getExprLoc()));
  setTerminator(Terminator::createGoto(JoinBB, currentSafeZone()));

  // Continue in join block
  switchToBlock(JoinBB);
  return Operand::createCopy(ResultPlace);
}

Operand BSCIRBuilder::VisitSafeExpr(SafeExpr *SE) {
  SafeZoneStack.push_back(SE->getSafeZoneSpecifier());
  Operand Result = lowerToOperand(SE->getSubExpr());
  SafeZoneStack.pop_back();
  return Result;
}

Operand BSCIRBuilder::VisitGNUNullExpr(GNUNullExpr *E) {
  LocalId Tmp = TheBody->addTemp(E->getType(), E->getExprLoc());
  Place TmpPlace(Tmp, E->getType(), E->getExprLoc());
  emit(Statement::createAssign(TmpPlace, Rvalue::createNullPtr(E->getType()),
                               currentSafeZone(), E, E->getExprLoc()));
  return Operand::createCopy(TmpPlace);
}

Operand BSCIRBuilder::VisitCXXNullPtrLiteralExpr(CXXNullPtrLiteralExpr *E) {
  // nullptr is a zero constant — no temp needed.
  return Operand::createConstant(
      APValue(llvm::APSInt(llvm::APInt(1, 0), /*isUnsigned=*/false)),
      E->getType());
}

Operand BSCIRBuilder::VisitStmtExpr(StmtExpr *SE) {
  const CompoundStmt *CS = SE->getSubStmt();

  // Copy the block's value out before its own locals go dead.
  LocalId Result = TheBody->addTemp(SE->getType(), SE->getExprLoc());
  Place ResultPlace(Result, SE->getType(), SE->getExprLoc());

  ScopeStack.push_back({});

  // The value is the statement a statement expression yields, which skips
  // trailing null statements and reaches through labels and attributes.
  const Stmt *ResultStmt = CS->body_empty() ? nullptr : CS->getStmtExprResult();

  for (const Stmt *S : CS->body()) {
    if (S != ResultStmt) {
      lowerStmt(S);
      continue;
    }
    const Stmt *Value = ResultStmt;
    while (!isa<Expr>(Value)) {
      if (const auto *LS = dyn_cast<LabelStmt>(Value)) {
        enterLabelBlock(LS);
        Value = LS->getSubStmt();
      } else if (const auto *AS = dyn_cast<AttributedStmt>(Value)) {
        Value = AS->getSubStmt();
      } else {
        break;
      }
    }
    const auto *ValueExpr = dyn_cast<Expr>(Value);
    if (!ValueExpr) {
      lowerStmt(Value);
      continue;
    }
    Operand Op = lowerToOperand(ValueExpr);
    if (!SE->getType()->isVoidType())
      emit(Statement::createAssign(ResultPlace, Rvalue::createUse(Op),
                                   currentSafeZone(), SE, SE->getExprLoc()));
  }

  emitScopeExit(CS->getRBracLoc());

  return Operand::createCopy(ResultPlace);
}

Operand BSCIRBuilder::VisitStmt(Stmt *S) {
  if (S)
    emit(Statement::createNop(currentSafeZone(), S->getBeginLoc()));
  return Operand::createConstant(APValue(), Ctx.VoidTy);
}

//===----------------------------------------------------------------------===//
// Label Pre-scan
//===----------------------------------------------------------------------===//

/// Walk the AST before lowering to record the scope depth of every label.
/// This mirrors the scope-push behavior of lowerCompoundStmt, lowerForStmt,
/// and lowerSwitchStmt so that forward gotos can compute the correct
/// StorageDead cleanup depth instead of falling back to 0.
void BSCIRBuilder::prescanLabels(const Stmt *S, unsigned Depth) {
  if (!S)
    return;

  if (const auto *CS = dyn_cast<CompoundStmt>(S)) {
    unsigned Inner = Depth + 1;
    for (const Stmt *Child : CS->body())
      prescanLabels(Child, Inner);
    return;
  }

  if (const auto *LS = dyn_cast<LabelStmt>(S)) {
    LabelScopeDepth[LS->getDecl()] = Depth;
    prescanLabels(LS->getSubStmt(), Depth);
    return;
  }

  if (const auto *FS = dyn_cast<ForStmt>(S)) {
    unsigned ForDepth = Depth;
    // ForStmt pushes an extra scope when the init clause is a DeclStmt.
    if (isa_and_nonnull<DeclStmt>(FS->getInit()))
      ++ForDepth;
    prescanLabels(FS->getInit(), ForDepth);
    prescanLabels(FS->getCond(), ForDepth);
    prescanLabels(FS->getInc(), ForDepth);
    prescanLabels(FS->getBody(), ForDepth);
    return;
  }

  if (const auto *SS = dyn_cast<SwitchStmt>(S)) {
    // SwitchStmt pushes a dedicated scope for the body.
    prescanLabels(SS->getCond(), Depth);
    prescanLabels(SS->getBody(), Depth + 1);
    return;
  }

  // Default: recurse into all children at the same depth.
  for (const Stmt *Child : S->children())
    prescanLabels(Child, Depth);
}

//===----------------------------------------------------------------------===//
// Main Build Entry Point
//===----------------------------------------------------------------------===//

std::unique_ptr<Body> BSCIRBuilder::build() {
  // Create return slot and parameter locals
  lowerParams();

  // Create entry block (bb0) — first real code goes here
  BasicBlockId EntryBB = TheBody->addBlock();
  CurrentBlock = EntryBB;

  // Create return block (will be populated at end)
  ReturnBlock = TheBody->addBlock();

  // Pre-scan to record scope depths of all labels before lowering.
  // This lets forward gotos emit correct StorageDead cleanup.
  const Stmt *FnBody = FD.getBody();
  if (FnBody)
    prescanLabels(FnBody, 0);

  // Lower the function body recursively
  if (FnBody)
    lowerStmt(FnBody);

  // If current block hasn't been terminated (implicit return for void
  // functions), emit owned parameter drops and goto return block.
  if (TheBody->getBlock(CurrentBlock).Term.K == Terminator::Unreachable) {
    SourceLocation EndLoc = FnBody ? FnBody->getEndLoc() : FD.getEndLoc();
    SmallVector<LocalId, 4> ParamLocals;
    for (unsigned I = TheBody->NumParams; I >= 1; --I)
      ParamLocals.push_back(LocalId{I});
    emitLocalsCleanup(ParamLocals, EndLoc, CleanupKind::DropOnly);
    setTerminator(Terminator::createGoto(ReturnBlock, currentSafeZone()));
  }

  // Set return block terminator
  switchToBlock(ReturnBlock);
  setTerminator(Terminator::createReturn(TheBody->FuncSafeZone));

  // Simplify: collapse goto chains, merge blocks, remove dead blocks
  TheBody->simplify();

  // Compute predecessor map
  TheBody->computePredecessors();

  // Record total number of region variables for borrow checker.
  TheBody->NumRegions = NextRegionId - 1;

  return std::move(TheBody);
}

#endif // ENABLE_BSC
