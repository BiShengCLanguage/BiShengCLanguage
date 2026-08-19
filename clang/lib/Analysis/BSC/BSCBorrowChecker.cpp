//===- BSCBorrowChecker.cpp - Borrow Check for Source CFGs -*- BSC --*--------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements BSC borrow checker for source-level CFGs.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/Analysis/Analyses/BSC/BSCBorrowChecker.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace clang;
using namespace clang::borrow;

namespace {
bool IsTrackedTypeImpl(const Environment &env, QualType type,
                       llvm::SmallPtrSetImpl<const RecordDecl *> &visited) {
  type = type.getCanonicalType();
  if (type->isPointerType() && type.isOwnedQualified()) {
    return IsTrackedTypeImpl(env, type->getPointeeType(), visited);
  }
  if (type->isArrayType()) {
    if (const ArrayType *AT = env.Ctx.getAsArrayType(type)) {
      return IsTrackedTypeImpl(env, AT->getElementType(), visited);
    }
  }
  if (type.isBorrowQualified())
    return true;
  if (const RecordType *RT = type->getAs<RecordType>()) {
    if (const RecordDecl *RD = RT->getDecl()->getDefinition()) {
      if (!visited.insert(RD).second) {
        return false;
      }
      for (const FieldDecl *FD : RD->fields()) {
        if (IsTrackedTypeImpl(env, FD->getType(), visited)) {
          return true;
        }
      }
    }
  }
  return false;
}

bool IsTrackedType(const Environment &env, QualType type) {
  llvm::SmallPtrSet<const RecordDecl *, 8> visited;
  return IsTrackedTypeImpl(env, type, visited);
}
} // namespace

namespace {
/// Given a statement, returns the corresponding (defs, uses).
///
/// The `defs` contains variables whose current value is completely
/// overwritten, and the `uses` contains variables whose current value is used.
/// Note that a variable may exist in both sets.
class DefUse : public clang::StmtVisitor<DefUse> {
  enum { None, Def, Use } Action;
  bool isAssign = false;
  llvm::SmallVector<VarDecl *> defs;
  llvm::SmallVector<VarDecl *> uses;

public:
  DefUse(Stmt *S) {
    Action = None;
    Visit(S);
  }

  const llvm::SmallVector<VarDecl *> &getDefs() const { return defs; }
  const llvm::SmallVector<VarDecl *> &getUses() const { return uses; }

  void VisitBinaryOperator(BinaryOperator *BO);
  void VisitBinAssign(BinaryOperator *BO);
  void VisitBinComma(BinaryOperator *BO);
  void VisitCallExpr(CallExpr *CE);
  void VisitDeclRefExpr(DeclRefExpr *DRE);
  void VisitDeclStmt(DeclStmt *DS);
  void VisitMemberExpr(MemberExpr *ME);
  void VisitReturnStmt(ReturnStmt *RS);
  void VisitStmt(Stmt *S);
  void VisitUnaryDeref(UnaryOperator *UO);
  void VisitUnaryOperator(UnaryOperator *UO);
  void VisitArraySubscriptExpr(ArraySubscriptExpr *ASE);
};
} // namespace

void DefUse::VisitBinaryOperator(BinaryOperator *BO) {
  auto Opcode = BO->getOpcode();
  if ((Opcode >= BO_Mul && Opcode <= BO_Shr) ||
      (Opcode >= BO_And && Opcode <= BO_LOr) ||
      (Opcode >= BO_LT && Opcode <= BO_NE)) {
    Action = Use;
    Visit(BO->getLHS());
    Visit(BO->getRHS());
  } else if (Opcode >= BO_MulAssign && Opcode <= BO_OrAssign) {
    Action = Def;
    Visit(BO->getLHS());
    Action = Use;
    Visit(BO->getLHS());
    Visit(BO->getRHS());
  }
}

void DefUse::VisitBinComma(BinaryOperator *BO) {
  Action = Use;
  Visit(BO->getLHS());
  Visit(BO->getRHS());
}

void DefUse::VisitBinAssign(BinaryOperator *BO) {
  Action = Def;
  llvm::SaveAndRestore<bool> save_is_assign(isAssign, true);
  Visit(BO->getLHS());
  Action = Use;
  Visit(BO->getRHS());
}

void DefUse::VisitCallExpr(CallExpr *CE) {
  Action = Use;
  if (!CE->getDirectCallee()) {
    Visit(CE->getCallee());
  }
  for (Expr *E : CE->arguments()) {
    Visit(E);
  }
}

void DefUse::VisitDeclRefExpr(DeclRefExpr *DRE) {
  if (VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
    if (Action == Def) {
      defs.push_back(VD);
    } else if (Action == Use) {
      uses.push_back(VD);
    }
  }
}

void DefUse::VisitDeclStmt(DeclStmt *DS) {
  for (Decl *D : DS->decls()) {
    if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
      defs.push_back(VD);
      if (VD->hasInit()) {
        Action = Use;
        Visit(VD->getInit());
      }
    }
  }
}

void DefUse::VisitMemberExpr(MemberExpr *ME) {
  /// When you have `p = ...`, which variable is reassigned?
  /// If `p` is `x`, then `x` is. Otherwise, nothing.
  /// When you have `p = ...`, which variable is read?
  /// If `p` is `x.a`, then `x` is. Otherwise, nothing.
  if (Action == Def && isAssign)
    Action = Use;
  if (Action == Use)
    Visit(ME->getBase());
}

void DefUse::VisitReturnStmt(ReturnStmt *RS) {
  Action = Use;
  if (Expr *RV = RS->getRetValue()) {
    Visit(RV);
  }
}

void DefUse::VisitStmt(Stmt *S) {
  for (auto *C : S->children()) {
    if (C)
      Visit(C);
  }
}

void DefUse::VisitUnaryDeref(UnaryOperator *UO) {
  Action = Use;
  Visit(UO->getSubExpr());
}

void DefUse::VisitUnaryOperator(UnaryOperator *UO) {
  if (UO->isIncrementDecrementOp()) {
    Action = Def;
    Visit(UO->getSubExpr());
    Action = Use;
    Visit(UO->getSubExpr());
  } else {
    Visit(UO->getSubExpr());
  }
}

void DefUse::VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
  if (Action == Use || Action == Def) {
    // Subscript reads its base pointer even on the assignment LHS (`p[i] = …`).
    Action = Use;
    Visit(ASE->getBase());
    Action = Use;
    Visit(ASE->getIdx());
  } else {
    VisitStmt(ASE);
  }
}

namespace {
/// Given a statement, extract and generate actions.
///
/// Due to the complexity of AST nodes, implementing the borrow checker becomes
/// challenging, so it is necessary to parse out the corresponding actions to
/// simplify the algorithm implementations.
///
/// We classify each statement into six type of actions, with each statement
/// corresponding to one or more actions.
class ActionExtract : public clang::StmtVisitor<ActionExtract> {
  RegionCheck &rc;
  std::vector<std::unique_ptr<Action>> actions;
  std::vector<std::unique_ptr<Path>> Sources;
  std::unique_ptr<Path> Src = nullptr;
  std::unique_ptr<Path> Dest = nullptr;
  RegionName RNL;
  RegionName RNR;
  borrow::BorrowKind BK;
  Action::ActionKind Kind = Action::Noop;
  enum { LHS, RHS } op;
  unsigned pathDepth = 0;
  Decl *D = nullptr;
  bool isArrow = false;
  bool BuildOnGet = true;

  bool IsTrackedType(QualType QT) const {
    return ::IsTrackedType(rc.getEnv(), QT);
  }

  void Reset() {
    Sources.clear();
    Src = nullptr;
    Dest = nullptr;
    RNL = RegionName();
    RNR = RegionName();
    Kind = Action::Noop;
    op = LHS;
    pathDepth = 0;
    D = nullptr;
    isArrow = false;
  }

  void BuildAction() {
    switch (Kind) {
    case Action::Assign: {
      assert(!RNL.isInvalid() && !RNR.isInvalid() &&
             "do not expect a invalid region name!");
      RegionName DerefRN = RegionName::Create();

      std::vector<std::unique_ptr<Path>> DerefSources =
          ProcessDeref(Dest->ty, Sources[0]);
      actions.emplace_back(std::make_unique<ActionAssign>(
          std::move(Dest), RNL, RNR, std::move(Sources[0]), DerefRN,
          std::move(DerefSources)));
      break;
    }
    case Action::Borrow: {
      assert(!RNL.isInvalid() && !RNR.isInvalid() &&
             "do not expect a invalid region name!");
      actions.emplace_back(std::make_unique<ActionBorrow>(
          std::move(Dest), RNL, RNR, BK, std::move(Sources[0])));
      break;
    }
    case Action::Init: {
      if (IsTrackedType(Dest->ty)) {
        GenerateImplicitAssign();
        actions.insert(
            actions.begin(),
            std::make_unique<ActionInit>(std::move(Dest), std::move(Sources)));
      } else {
        std::vector<std::unique_ptr<Path>> DerefSources;
        for (const std::unique_ptr<Path> &Source : Sources) {
          QualType QT = Source->ty;
          std::vector<std::unique_ptr<Path>> Derefs = ProcessDeref(QT, Source);
          DerefSources.insert(DerefSources.end(),
                              std::make_move_iterator(Derefs.begin()),
                              std::make_move_iterator(Derefs.end()));
        }
        actions.emplace_back(std::make_unique<ActionInit>(
            std::move(Dest), std::move(Sources), std::move(DerefSources)));
      }
      break;
    };
    case Action::StorageDead: {
      actions.emplace_back(
          std::make_unique<ActionStorageDead>(std::move(Dest)));
      break;
    }
    case Action::Use: {
      std::vector<std::unique_ptr<Path>> DerefSources;
      for (const std::unique_ptr<Path> &Source : Sources) {
        QualType QT = Source->ty;
        std::vector<std::unique_ptr<Path>> Derefs = ProcessDeref(QT, Source);
        DerefSources.insert(DerefSources.end(),
                            std::make_move_iterator(Derefs.begin()),
                            std::make_move_iterator(Derefs.end()));
      }
      actions.emplace_back(std::make_unique<ActionUse>(
          std::move(Sources), std::move(DerefSources)));
      break;
    }
    default: {
      actions.emplace_back(std::make_unique<ActionNoop>());
      break;
    }
    }

    Reset();
  }

  std::vector<std::unique_ptr<Path>>
  ProcessDeref(QualType QT, const std::unique_ptr<Path> &path) {
    std::vector<std::unique_ptr<Path>> Res;
    if (QT.isBorrowQualified()) {
      std::unique_ptr<Path> DerefSource = std::make_unique<Path>(*path);
      DerefSource = std::make_unique<Path>(std::move(DerefSource), "*",
                                           DerefSource->ty->getPointeeType(),
                                           DerefSource->Location);
      Res.emplace_back(std::move(DerefSource));
    } else if (QT->withBorrowFields()) {
      const RecordType *RT = dyn_cast<RecordType>(QT.getCanonicalType());
      RecursiveForFields(RT, Res, path);
    }
    return Res;
  }

  // Traverse the field of record type and create implicit dereferences.
  void RecursiveForFields(const RecordType *RT,
                          std::vector<std::unique_ptr<Path>> &Res,
                          const std::unique_ptr<Path> &Base) {
    for (FieldDecl *FD : RT->getDecl()->fields()) {
      std::unique_ptr<Path> Deref = std::make_unique<Path>(*Base);
      Deref = std::make_unique<Path>(std::move(Deref), FD->getName().str(),
                                     FD->getType(), Deref->Location);
      if (FD->getType().isBorrowQualified()) {
        Deref = std::make_unique<Path>(std::move(Deref), "*",
                                       Deref->ty->getPointeeType(),
                                       Deref->Location);
        Res.emplace_back(std::move(Deref));
      }
      if (FD->getType()->withBorrowFields()) {
        const RecordType *rt =
            dyn_cast<RecordType>(FD->getType().getCanonicalType());
        RecursiveForFields(rt, Res, Deref);
      }
    }
  }

  void GenerateImplicitAssign() {
    RegionName DerefRN = RegionName::Create();
    for (const std::unique_ptr<Path> &Source : Sources) {
      if (IsTrackedType(Source->ty) && Source->D != nullptr) {
        std::unique_ptr<Path> DestCopy = std::make_unique<Path>(*Dest);
        std::unique_ptr<Path> SourceCopy = std::make_unique<Path>(*Source);
        assert(Source->D != nullptr && "expected non nullptr");
        RegionName SourceRN = rc.getRegionName(Source->D);
        assert(!SourceRN.isInvalid() && "expected valid region name");
        std::vector<std::unique_ptr<Path>> DerefSources =
            ProcessDeref(SourceCopy->ty, Source);
        actions.emplace_back(std::make_unique<ActionAssign>(
            std::move(DestCopy), RNL, SourceRN, std::move(SourceCopy), DerefRN,
            std::move(DerefSources)));
      }
    }
  }

  void VisitIncrementDecrementOp(UnaryOperator *UO) {
    QualType SubTy = UO->getSubExpr()->getType();
    if (SubTy.isBorrowQualified() && SubTy.isArrayElemQualified()) {
      Kind = Action::Use;
      op = RHS;
      Visit(UO->getSubExpr());
      return;
    }
    Kind = Action::Init;
    op = LHS;
    Visit(UO->getSubExpr());
    op = RHS;
    Visit(UO->getSubExpr());
  }

public:
  ActionExtract(Stmt *S, const VarDecl *VD, SourceLocation LifetimeEndsLoc,
                RegionCheck &rc)
      : rc(rc) {
    /// For CFGLifetimeEnds, just create an `ActionStorageDead`.
    if (VD) {
      Kind = Action::StorageDead;
      Dest = std::make_unique<Path>(VD->getName().str(), VD->getType(),
                                    LifetimeEndsLoc);
    } else {
      Visit(S);
    }
  }
  ActionExtract(Stmt *S, RegionCheck &rc, Action::ActionKind K, decltype(op) Op)
      : rc(rc), Kind(K), op(Op) {
    Visit(S);
  }

  ~ActionExtract() = default;

  std::vector<std::unique_ptr<Action>> GetAction() {
    if (BuildOnGet)
      BuildAction();

    return std::move(actions);
  }

  void VisitArraySubscriptExprOrUnaryDeref(Expr *E, Expr *SubExpr,
                                           bool FromArraySubscript = false);
  void VisitArraySubscriptExpr(ArraySubscriptExpr *ASE);
  void VisitBinaryOperator(BinaryOperator *BO);
  void VisitBinAssign(BinaryOperator *BO);
  void VisitBinComma(BinaryOperator *BO);
  void VisitCallExpr(CallExpr *CE);
  void VisitCStyleCastExpr(CStyleCastExpr *CSCE);
  void VisitDeclRefExpr(DeclRefExpr *DRE);
  void VisitDeclStmt(DeclStmt *DS);
  void VisitInitListExpr(InitListExpr *ILE);
  void VisitMemberExpr(MemberExpr *ME);
  void VisitReturnStmt(ReturnStmt *RS);
  void VisitStmt(Stmt *S);
  void VisitStmtExpr(StmtExpr *SE);
  void VisitUnaryAddrConst(UnaryOperator *UO);
  void VisitUnaryAddrConstDeref(UnaryOperator *UO);
  void VisitUnaryAddrMut(UnaryOperator *UO);
  void VisitUnaryAddrMutDeref(UnaryOperator *UO);
  void VisitUnaryDeref(UnaryOperator *UO);
  void VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *UE);
  void VisitUnaryPostDec(UnaryOperator *UO);
  void VisitUnaryPostInc(UnaryOperator *UO);
  void VisitUnaryPreDec(UnaryOperator *UO);
  void VisitUnaryPreInc(UnaryOperator *UO);
};

QualType getPointeeOrElementType(QualType T) {
  auto CT = T.getCanonicalType();
  if (CT->isPointerType()) {
    return CT->getPointeeType();
  }
  if (CT->isArrayType()) {
    QualType ElemTy = CT->getAsArrayTypeUnsafe()->getElementType();
    return ElemTy;
  }
  return T;
}
} // namespace

void ActionExtract::VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
  VisitArraySubscriptExprOrUnaryDeref(ASE, ASE->getBase(),
                                      /*FromArraySubscript=*/true);
}

void ActionExtract::VisitBinaryOperator(BinaryOperator *BO) {
  auto Opcode = BO->getOpcode();
  if ((Opcode >= BO_Mul && Opcode <= BO_Shr) ||
      (Opcode >= BO_And && Opcode <= BO_LOr)) {
    op = RHS;
    Visit(BO->getLHS());
    Visit(BO->getRHS());
  } else if (Opcode == BO_Add || Opcode == BO_Sub) {
    op = RHS;
    Visit(BO->getLHS());
    Visit(BO->getRHS());
  } else if (Opcode >= BO_LT && Opcode <= BO_NE) {
    Kind = Action::Use;
    op = RHS;
    Visit(BO->getLHS());
    Visit(BO->getRHS());
  } else if (Opcode >= BO_MulAssign && Opcode <= BO_OrAssign) {
    Kind = Action::Init;
    op = LHS;
    Visit(BO->getLHS());
    op = RHS;
    Visit(BO->getLHS());
    Visit(BO->getRHS());
  }
}

void ActionExtract::VisitBinComma(BinaryOperator *BO) {
  std::vector<std::unique_ptr<Action>> LHSActions =
      ActionExtract(BO->getLHS(), nullptr, SourceLocation(), rc).GetAction();
  actions.insert(actions.end(), std::make_move_iterator(LHSActions.begin()),
                 std::make_move_iterator(LHSActions.end()));
  op = RHS;
  Visit(BO->getRHS());
}

void ActionExtract::VisitStmtExpr(StmtExpr *SE) {
  CompoundStmt *CS = SE->getSubStmt();
  if (CS->body_empty())
    return;
  for (Stmt *S : CS->body()) {
    if (S == CS->body_back())
      break;
    std::vector<std::unique_ptr<Action>> SubActions =
        ActionExtract(S, rc, Action::Use, RHS).GetAction();
    actions.insert(actions.end(), std::make_move_iterator(SubActions.begin()),
                   std::make_move_iterator(SubActions.end()));
  }
  Visit(CS->body_back());
}

void ActionExtract::VisitBinAssign(BinaryOperator *BO) {
  if (BO->getType()->isStructureType() && IsTrackedType(BO->getType()) &&
      isa<CompoundLiteralExpr>(BO->getRHS()->IgnoreImpCasts())) {
    BuildOnGet = false;
    op = LHS;
    Visit(BO->getLHS());
    op = RHS;
    Visit(BO->getRHS());
  } else {
    Kind = Action::Assign;
    op = LHS;
    Visit(BO->getLHS());
    op = RHS;
    Visit(BO->getRHS());
    // ActionAssign should only be used when assigning borrow-qualified types.
    // If the assignment type itself is not tracked, use ActionInit.
    if (RNL.isInvalid() || RNR.isInvalid() || Sources.empty() ||
        !IsTrackedType(BO->getType()))
      Kind = Action::Init;
  }
}

void ActionExtract::VisitCallExpr(CallExpr *CE) {
  if (Kind == Action::Noop)
    Kind = Action::Use;
  else if (Dest != nullptr)
    Kind = Action::Init;
  op = RHS;
  if (!CE->getDirectCallee()) {
    Visit(CE->getCallee());
  }
  for (Expr *E : CE->arguments()) {
    Visit(E);
  }
}

void ActionExtract::VisitCStyleCastExpr(CStyleCastExpr *CSCE) {
  if (CStyleCastExpr *sub =
          dyn_cast<CStyleCastExpr>(CSCE->getSubExpr()->IgnoreParens())) {
    if (sub->getCastKind() == CK_NullToPointer) {
      return;
    }
  }
  Visit(CSCE->getSubExpr());
  if (op == RHS) {
    if (CSCE->getType().isBorrowQualified()) {
      if (!Sources.empty() && Sources[0] != nullptr) {
        RNR = rc.getRegionName(CSCE);
        if (CSCE->getType().isConstBorrow()) {
          BK = borrow::BorrowKind::Shared;
        } else {
          BK = borrow::BorrowKind::Mut;
        }
        if (!Sources[0]->ty.isBorrowQualified()) {
          Kind = Action::Borrow;
        }
        bool AddDeref = false;
        if (Dest) {
          AddDeref = Sources[0]->ty != Dest->ty->getPointeeType();
        }
        if (UnaryOperator *Sub = dyn_cast<UnaryOperator>(
                CSCE->getSubExpr()->IgnoreParenImpCasts())) {
          if (Sub->getOpcode() == UO_AddrOf) {
            // If the sub expr of a CStyleCastExpr is UO_AddrOf such as
            // `(int* borrow)&x`, don't add dereference.
            AddDeref = false;
          }
        }
        if (AddDeref) {
          std::unique_ptr<Path> Deref = std::make_unique<Path>(
              std::move(Sources[0]), "*", CSCE->getType(), CSCE->getEndLoc());
          Sources[0] = std::move(Deref);
        }
      }
    }
  }
}

void ActionExtract::VisitDeclRefExpr(DeclRefExpr *DRE) {
  auto DREType = DRE->getType();
  if (op == LHS) {
    Dest = std::make_unique<Path>(DRE->getDecl()->getName().str(),
                                  DREType, DRE->getLocation());
    if (isArrow) {
      QualType PT = getPointeeOrElementType(DREType);
      Dest = std::make_unique<Path>(std::move(Dest), "*",
                                    PT,
                                    DRE->getLocation());
    }
    if (IsTrackedType(DREType)) {
      RNL = rc.getRegionName(DRE->getDecl());
    }
  } else if (op == RHS) {
    Src = std::make_unique<Path>(DRE->getDecl()->getName().str(),
                                 DREType, DRE->getLocation());
    // Decl of DeclRefExpr is for reborrow constraints.
    if (DREType.isBorrowQualified()) {
      Src->setDecl(DRE->getDecl());
    } else if (IsTrackedType(DREType)) {
      Src->setDecl(DRE->getDecl());
      D = DRE->getDecl();
    }
    if (Kind == Action::Assign && IsTrackedType(DREType)) {
      RNR = rc.getRegionName(DRE->getDecl());
    }
    if (isArrow) {
      QualType PT = getPointeeOrElementType(DREType);
      Src = std::make_unique<Path>(std::move(Src), "*",
                                   PT,
                                   DRE->getLocation());
    }
    if (pathDepth == 0)
      Sources.emplace_back(std::move(Src));
  }
}

void ActionExtract::VisitDeclStmt(DeclStmt *DS) {
  // Note: The construction of CFG ensures that there is only one VarDecl in
  // the DeclStmt.
  Decl *D = DS->getSingleDecl();
  if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
    Dest = std::make_unique<Path>(VD->getName().str(), VD->getType(),
                                  VD->getLocation());
    if (IsTrackedType(VD->getType())) {
      RNL = rc.getRegionName(VD);
    }
    if (Expr *Init = VD->getInit()) {
      if ((VD->getType()->isStructureType() || VD->getType()->isArrayType()) &&
          IsTrackedType(VD->getType()) &&
          isa<InitListExpr, CompoundLiteralExpr>(Init)) {
        BuildOnGet = false;
        Visit(Init);
      } else {
        Kind = Action::Assign;
        Sources.clear();
        op = RHS;
        Visit(Init);
        // Handle cases like `int *borrow p = (int *borrow)NULL` and
        // `int *borrow p = (int *borrow)q`.
        if (RNL.isInvalid() || RNR.isInvalid() || Sources.empty())
          Kind = Action::Init;
      }
    } else {
      Kind = Action::Init;
    }
  }
}

void ActionExtract::VisitInitListExpr(InitListExpr *ILE) {
  if (const RecordType *RT =
          dyn_cast<RecordType>(ILE->getType().getCanonicalType())) {
    if (IsTrackedType(ILE->getType())) {
      // Handle struct/union initialization with tracked fields
      Kind = Action::Noop;
      RegionName RN = RNL;
      RegionName CurRN;
      std::unique_ptr<Path> Base = std::make_unique<Path>(*Dest);
      unsigned Index = 0;
      for (FieldDecl *Field : RT->getDecl()->fields()) {
        Kind = Action::Assign;
        std::unique_ptr<Path> FieldName = std::make_unique<Path>(*Base);
        FieldName =
            std::make_unique<Path>(std::move(FieldName), Field->getName().str(),
                                   Field->getType(), ILE->getBeginLoc());
        Dest = std::move(FieldName);
        if (IsTrackedType(Field->getType()))
          CurRN = RN;
        RNL = CurRN;
        op = RHS;
        Visit(ILE->getInit(Index));
        if (RNL.isInvalid() || RNR.isInvalid() || Sources.empty())
          Kind = Action::Init;
        if (Dest)
          BuildAction();
        ++Index;
      }
      return;
    }
  }

  if (ILE->getType()->isArrayType() && IsTrackedType(ILE->getType())) {
    // Handle array initialization with borrow-qualified elements
    Kind = Action::Noop;
    RegionName RN = RNL;
    RegionName CurRN;
    std::unique_ptr<Path> Base = std::make_unique<Path>(*Dest);
    const ArrayType *AT = ILE->getType()->getAsArrayTypeUnsafe();
    QualType ElementType = AT->getElementType();
    unsigned Index = 0;
    for (Expr *Init : ILE->inits()) {
      Kind = Action::Assign;
      std::unique_ptr<Path> ArrayElem = std::make_unique<Path>(*Base);
      ArrayElem = std::make_unique<Path>(std::move(ArrayElem),
                                         "[" + std::to_string(Index) + "]",
                                         ElementType, ILE->getBeginLoc());
      Dest = std::move(ArrayElem);
      if (IsTrackedType(ElementType))
        CurRN = RN;
      RNL = CurRN;
      op = RHS;
      Visit(Init);
      if (RNL.isInvalid() || RNR.isInvalid() || Sources.empty())
        Kind = Action::Init;
      if (Dest)
        BuildAction();
      ++Index;
    }
  } else {
    for (Expr *Init : ILE->inits()) {
      Visit(Init);
    }
  }
}

void ActionExtract::VisitMemberExpr(MemberExpr *ME) {
  auto METype = ME->getType();
  if (op == LHS) {
    llvm::SaveAndRestore<bool> save_is_arrow(isArrow);
    isArrow = ME->isArrow();
    Visit(ME->getBase());
    std::unique_ptr<Path> Member = std::make_unique<Path>(
        std::move(Dest), ME->getMemberNameInfo().getAsString(), METype,
        ME->getMemberLoc());
    if (save_is_arrow.get()) {
      QualType PT = getPointeeOrElementType(METype);
      Member = std::make_unique<Path>(std::move(Member), "*",
                                      PT,
                                      ME->getMemberLoc());
    }
    Dest = std::move(Member);
  } else if (op == RHS) {
    ++pathDepth;
    llvm::SaveAndRestore<bool> save_is_arrow(isArrow);
    isArrow = ME->isArrow();
    Visit(ME->getBase());
    // Early return when `Src` is nullptr, which is a special handling for
    // member offset calculation pattern like:
    // \code
    // #define OFFSET_OF_START (unsigned int *)0x1000
    // (((unsigned int *)&((type *)OFFSET_OF_START)->member) - OFFSET_OF_START)
    // \endcode
    if (!Src) {
      --pathDepth;
      return;
    }
    std::unique_ptr<Path> Member = std::make_unique<Path>(
        std::move(Src), ME->getMemberNameInfo().getAsString(), METype,
        ME->getMemberLoc());
    if (METype.isBorrowQualified()) {
      Member->setDecl(D);
    }
    if (save_is_arrow.get()) {
      QualType PT = getPointeeOrElementType(METype);
      Member = std::make_unique<Path>(std::move(Member), "*",
                                      PT,
                                      ME->getMemberLoc());
    }
    Src = std::move(Member);
    --pathDepth;
    if (pathDepth == 0)
      Sources.emplace_back(std::move(Src));
  }
}

void ActionExtract::VisitReturnStmt(ReturnStmt *RS) {
  if (!RS->getRetValue()) {
    return;
  }
  if (!IsTrackedType(RS->getRetValue()->getType())) {
    Kind = Action::Use;
    op = RHS;
    Visit(RS->getRetValue());
    return;
  }
  Kind = Action::Assign;
  Dest = std::make_unique<Path>("__ret", RS->getRetValue()->getType(),
                                RS->getReturnLoc());
  RNL = RegionName::CreateFree();
  op = RHS;
  Visit(RS->getRetValue());
  if (RNR.isInvalid())
    Kind = Action::Init;
}

void ActionExtract::VisitStmt(Stmt *S) {
  for (auto *C : S->children()) {
    if (C)
      Visit(C);
  }
}

void ActionExtract::VisitUnaryAddrConst(UnaryOperator *UO) {
  RNR = rc.getRegionName(UO);
  BK = borrow::BorrowKind::Shared;
  Kind = Action::Borrow;
  Visit(UO->getSubExpr());
}

void ActionExtract::VisitUnaryAddrConstDeref(UnaryOperator *UO) {
  RNR = rc.getRegionName(UO);
  BK = borrow::BorrowKind::Shared;
  Kind = Action::Borrow;
  Visit(UO->getSubExpr());
  if (Sources.empty()) {
    Kind = Action::Init;
    return;
  }
  if (Sources[0]->ty->isPointerType()) {
    std::unique_ptr<Path> Deref =
        std::make_unique<Path>(std::move(Sources[0]), "*",
                               UO->getType()->getPointeeType(), UO->getEndLoc());
    Sources[0] = std::move(Deref);
  }
}

void ActionExtract::VisitUnaryAddrMut(UnaryOperator *UO) {
  RNR = rc.getRegionName(UO);
  BK = borrow::BorrowKind::Mut;
  Kind = Action::Borrow;
  Visit(UO->getSubExpr());
}

void ActionExtract::VisitUnaryAddrMutDeref(UnaryOperator *UO) {
  RNR = rc.getRegionName(UO);
  BK = borrow::BorrowKind::Mut;
  Kind = Action::Borrow;
  Visit(UO->getSubExpr());
  // Note that in some cases such as `&mut *&p`, there is no need to add
  // dereference, as it allows for more accurate analysis.
  if (Sources[0]->ty->isPointerType()) {
    std::unique_ptr<Path> Deref =
        std::make_unique<Path>(std::move(Sources[0]), "*",
                               UO->getType()->getPointeeType(), UO->getEndLoc());
    Sources[0] = std::move(Deref);
  }
}

void ActionExtract::VisitArraySubscriptExprOrUnaryDeref(
    Expr *E, Expr *SubExpr, bool FromArraySubscript) {
  if (Kind == Action::Noop) {
    Kind = Action::Use;
    op = RHS;
  }
  QualType Type = E->getType();
  QualType BaseTy = SubExpr->getType();
  const bool ArrayElemSubscriptNotation =
      FromArraySubscript && BaseTy.isBorrowQualified() &&
      BaseTy.isArrayElemQualified();
  if (op == LHS) {
    Visit(SubExpr);
    std::unique_ptr<Path> Deref = std::make_unique<Path>(
        std::move(Dest), "*", Type, E->getBeginLoc());
    if (ArrayElemSubscriptNotation)
      Deref->UsesArraySubscriptNotation = true;
    Dest = std::move(Deref);
  } else if (op == RHS) {
    ++pathDepth;
    Visit(SubExpr);
    bool DontAddDeref = false;
    if (UnaryOperator *Sub =
            dyn_cast<UnaryOperator>(SubExpr->IgnoreParenImpCasts())) {
      if (Sub->getOpcode() == UO_AddrOf) {
        DontAddDeref = true;
      }
    }
    if (!DontAddDeref) {
      std::unique_ptr<Path> Deref = std::make_unique<Path>(
          std::move(Src), "*", Type, E->getBeginLoc());
      if (ArrayElemSubscriptNotation)
        Deref->UsesArraySubscriptNotation = true;
      Src = std::move(Deref);
    }
    --pathDepth;
    if (pathDepth == 0)
      Sources.emplace_back(std::move(Src));
  }
}

void ActionExtract::VisitUnaryDeref(UnaryOperator *UO) {
  VisitArraySubscriptExprOrUnaryDeref(UO, UO->getSubExpr());
}

// Skip visiting UE's children to avoid treating them as accesses.
// For example, `sizeof(x)` is not an access to `x`.
void ActionExtract::VisitUnaryExprOrTypeTraitExpr(
    UnaryExprOrTypeTraitExpr *UE) {
  return;
}

void ActionExtract::VisitUnaryPostDec(UnaryOperator *UO) {
  VisitIncrementDecrementOp(UO);
}

void ActionExtract::VisitUnaryPostInc(UnaryOperator *UO) {
  VisitIncrementDecrementOp(UO);
}

void ActionExtract::VisitUnaryPreDec(UnaryOperator *UO) {
  VisitIncrementDecrementOp(UO);
}

void ActionExtract::VisitUnaryPreInc(UnaryOperator *UO) {
  VisitIncrementDecrementOp(UO);
} 

//===----------------------------------------------------------------------===//
//                         Operations on RegionName
//===----------------------------------------------------------------------===//

unsigned RegionName::Cnt = 0;

//===----------------------------------------------------------------------===//
//                            Achors for Actions
//===----------------------------------------------------------------------===//

void Action::anchor() {}
void ActionNoop::anchor() {}
void ActionInit::anchor() {}
void ActionBorrow::anchor() {}
void ActionAssign::anchor() {}
void ActionUse::anchor() {}
void ActionStorageDead::anchor() {}

//===----------------------------------------------------------------------===//
//                       Query functions on Environment
//===----------------------------------------------------------------------===//

/// Given a point, returns the set of all its successor points in the CFG.
llvm::SmallVector<Point> Environment::SuccessorPoints(Point point) const {
  llvm::SmallVector<Point> Succs;

  const CFGBlock *block = *(cfg.nodes_begin() + point.blockID);
  if (point.index != block->size()) {
    Succs.push_back(Point(point.blockID, point.index + 1));
  } else {
    llvm::DenseSet<const CFGBlock *> succs(block->succ_begin(),
                                           block->succ_end());
    bool changed = true;
    while (changed) {
      changed = false;
      size_t oldSize = succs.size();
      // Note: to avoid iterator invalidation
      llvm::DenseSet<const CFGBlock *> newBlocks;
      for (const CFGBlock *succ : succs) {
        if (succ && succ->empty()) {
          newBlocks.insert(succ->succ_begin(), succ->succ_end());
        }
      }
      succs.insert(newBlocks.begin(), newBlocks.end());
      if (succs.size() != oldSize)
        changed = true;
    }
    for (const CFGBlock *succ : succs) {
      if (succ && !succ->empty())
        Succs.push_back(Point(succ->BlockID, 1));
    }
  }

  return Succs;
}

//===----------------------------------------------------------------------===//
//             Operation functions on InferenceContext and DFS
//===----------------------------------------------------------------------===//

void InferenceContext::AddLivePoint(RegionVariable RV, Point P) {
#if DEBUG_PRINT
  llvm::outs() << "AddLivePoint: " << RV.print() << " @ " << P.print() << '\n';
#endif
  VarDefinition &definition = definitions[RV.index];
  if (definition.value.AddPoint(P)) {
    if (definition.capped) {
      llvm_unreachable("Free region should not grow anymore!");
    }
  }
}

/// Inference algorithm, which is implemented based on fixed-point iteration.
///
/// During fixed-point iteration, the algorithm solves inference constraints
/// and updates the regions of region variables.
void InferenceContext::Solve(const Environment &env) {
  bool changed = true;
  DFS dfs(env);
  while (changed) {
    changed = false;

    for (Constraint &constraint : constraints) {
      const Region &Sup = definitions[constraint.sup.index].value;
      VarDefinition &SubDef = definitions[constraint.sub.index];
#if DEBUG_PRINT
      llvm::outs() << "constraint: " << constraint.print();
      llvm::outs() << "  sup (before): " << Sup.print() << '\n';
      llvm::outs() << "  sub (before): " << SubDef.value.print() << '\n';
#endif

      // DFS from the start point of constraint.
      if (dfs.Copy(Sup, SubDef.value, constraint.point)) {
        changed = true;

        if (SubDef.capped) {
          llvm_unreachable("Free region should not grow anymore!");
        }
      }

#if DEBUG_PRINT
      llvm::outs() << "  sub (after) : " << SubDef.value.print() << '\n';
      llvm::outs() << "  changed     : " << (changed ? "true" : "false")
                   << '\n';
#endif
    }
#if DEBUG_PRINT
    llvm::outs() << '\n';
#endif
  }
}

/// Update `To` using `From`， starting DFS from `StartPoint` until the visited
/// is not in `From` or has already been visited.
bool DFS::Copy(const Region &From, Region &To, Point StartPoint) {
  bool changed = false;

  stack.clear();
  visited.clear();

  stack.push_back(StartPoint);
  while (!stack.empty()) {
    Point p = stack.back();
    stack.pop_back();

#if DEBUG_PRINT
    llvm::outs() << "    dfs: p=" << p.print() << '\n';
#endif

    if (!From.MayContain(p)) {
#if DEBUG_PRINT
      llvm::outs() << "      not in From-Region\n";
#endif
      continue;
    }

    if (!visited.insert(p).second) {
#if DEBUG_PRINT
      llvm::outs() << "      already visited\n";
#endif
      continue;
    }

    changed |= To.AddPoint(p);

    llvm::SmallVector<Point> SuccessorPoints = env.SuccessorPoints(p);
    if (SuccessorPoints.empty()) {
      Point endPoint(Point::EndBlockID, Point::EndIndex);
      if (From.MayContain(endPoint)) {
        changed |= To.AddPoint(endPoint);
      }
    } else {
      stack.insert(stack.end(), SuccessorPoints.begin(), SuccessorPoints.end());
    }
  }

  return changed;
}

//===----------------------------------------------------------------------===//
//                           Liveness computations
//===----------------------------------------------------------------------===//

/// Iterates until a fixed point, computing live variables on the entry of each
/// basic block.
///
/// Note that an empty callback is sufficient when computing liveness.
void Liveness::Compute() {
  LivenessFact fact;
  bool changed = true;
  while (changed) {
    changed = false;

    for (const CFGBlock *B : env.cfg.const_nodes()) {
      SimulateBlock(fact, B,
                    [](auto _p, auto _a, auto _s, auto _v, auto _l) {});
      changed |= SetFrom(liveness[B], fact);
    }
  }
}

template <typename CB>
void Liveness::SimulateBlock(LivenessFact &fact, const CFGBlock *Block,
                             CB callback) {
  fact.clear();

  // Everything live in a successor is live at the exit of the block.
  for (auto succ : Block->succs()) {
    if (succ)
      SetFrom(fact, liveness[succ]);
  }

  // Walk backwards through the actions.
  for (CFGBlock::const_reverse_iterator it = Block->rbegin(),
                                        ei = Block->rend();
       it != ei; ++it) {
    const CFGElement &elem = *it;
    const Stmt *S = nullptr;
    const VarDecl *LifetimeEndsVD = nullptr;
    SourceLocation LifetimeEndsLoc;

    if (elem.getAs<CFGStmt>()) {
      S = elem.castAs<CFGStmt>().getStmt();
      // Get the def-use information of a given statement.
      DefUse DU(const_cast<Stmt *>(S));
      const llvm::SmallVector<VarDecl *> &defs = DU.getDefs();
      const llvm::SmallVector<VarDecl *> &uses = DU.getUses();

      // Anything we write to is no longer live.
      for (VarDecl *def : defs) {
        Kill(fact, def);
      }

      // Any variables we read from, we make live.
      for (VarDecl *use : uses) {
        Gen(fact, use);
      }
    }

    // There is no need to handle CFGLifetimeEnds when calculating liveness,
    // while it's necessary to handle CFGLifetimeEnds when populating inference,
    // so we need to get the `LifetimeEndsVD` and `LifetimeEndsLoc` here.
    if (elem.getAs<CFGLifetimeEnds>()) {
      LifetimeEndsVD = elem.castAs<CFGLifetimeEnds>().getVarDecl();
      LifetimeEndsLoc =
          elem.castAs<CFGLifetimeEnds>().getTriggerStmt()->getEndLoc();
    }

    Point point(Block->getBlockID(), std::distance(it, ei));

    callback(point, S, fact, LifetimeEndsVD, LifetimeEndsLoc);
  }
}

/// Invokes callback once for each statement with:
///   a. the point of the statement;
///   b. the statement itself;
///   c. the set of live variables on entry to the statement.
///
/// Note that all constraints will be generated after the walk.
template <typename CB> void Liveness::Walk(CB callback) {
  LivenessFact fact;

  for (const CFGBlock *B : env.cfg.const_nodes()) {
    SimulateBlock(fact, B, callback);
  }
}

/// Given a LivenessFact, return the corresponding set of RegionName based on
/// the type of the variables.
///
/// Only variables of borrowed qualified type or structure type with borrowed
/// qualifed field has corresponding RegionName.
std::set<RegionName> Liveness::LiveRegions(const LivenessFact &liveFact) {
  std::set<RegionName> set;
  for (VarDecl *VD : liveFact) {
    if (IsTrackedType(env, VD->getType())) {
      set.insert(rc.getRegionName(VD));
    }
  }
  return set;
}

//===----------------------------------------------------------------------===//
//                         LoansInScope computations
//===----------------------------------------------------------------------===//

LoansInScope::LoansInScope(const Environment &env, const RegionCheck &rc)
    : env(env), rc(rc) {
  // Collect the full set of loans, including explicit loans created with
  // `&const` and `&mut` expressions, as well as implicit loans created when
  // using variables of borrow qualified types.
  for (const CFGBlock *Block : env.cfg.const_reverse_nodes()) {
    for (CFGBlock::const_iterator it = Block->begin(), ei = Block->end();
         it != ei; ++it) {
      Point point(Block->getBlockID(), std::distance(Block->begin(), it) + 1);
      if (rc.getActionMap().find(point) != rc.getActionMap().end()) {
        const auto &actions = rc.getActionMap().at(point);
        for (const std::unique_ptr<Action> &action : actions) {
          if (action->getKind() == Action::ActionKind::Borrow) {
            const ActionBorrow *AB = dyn_cast<ActionBorrow>(action.get());
            const Region &region = rc.getRegion(AB->RNR);
            loans.push_back(Loan(point, AB->Source, AB->BK, region));
          } else if (action->getKind() == Action::ActionKind::Assign) {
            const ActionAssign *AA = dyn_cast<ActionAssign>(action.get());
            const Region &region = rc.getRegion(AA->DerefRN);
            for (const std::unique_ptr<Path> &DerefSource : AA->DerefSources) {
              borrow::BorrowKind BK = DerefSource->base->ty.isConstBorrow()
                                          ? BorrowKind::Shared
                                          : BorrowKind::Mut;
              loans.push_back(Loan(point, DerefSource, BK, region));
            }
          } else if (action->getKind() == Action::ActionKind::Use) {
            const ActionUse *AU = dyn_cast<ActionUse>(action.get());
            const Region &region = rc.getEmptyRegion();
            for (const std::unique_ptr<Path> &DerefSource : AU->DerefSources) {
              borrow::BorrowKind BK = DerefSource->base->ty.isConstBorrow()
                                          ? BorrowKind::Shared
                                          : BorrowKind::Mut;
              loans.push_back(Loan(point, DerefSource, BK, region));
            }
          } else if (action->getKind() == Action::ActionKind::Init) {
            const ActionInit *AI = dyn_cast<ActionInit>(action.get());
            const Region &region = rc.getEmptyRegion();
            for (const std::unique_ptr<Path> &DerefSource : AI->DerefSources) {
              borrow::BorrowKind BK = DerefSource->base->ty.isConstBorrow()
                                          ? BorrowKind::Shared
                                          : BorrowKind::Mut;
              loans.push_back(Loan(point, DerefSource, BK, region));
            }
          }
        }
      }
    }
  }

#if DEBUG_PRINT
  llvm::outs() << "loans: [\n";
  for (const Loan &loan : loans) {
    llvm::outs() << loan.print() << ",\n";
  }
  llvm::outs() << "]\n";
#endif

  // Make a convenient hash map for getting the index of a loan based on where
  // it appears.
  for (const auto &IndexedLoan : llvm::enumerate(loans)) {
    loansByPoint[IndexedLoan.value().point].push_back(IndexedLoan.index());
  }

  // Iterates until a fixed point.
  Compute();
}

llvm::SmallVector<unsigned>
LoansInScope::LoansKilledByWriteTo(const Path *path) const {
  llvm::SmallVector<unsigned> loanIndexes;

  // When an assignment like `a.b.c = ...` occurs, we kill all
  // the loans for `a.b.c` or some subpath like `a.b.c.d`, since
  // the path no longer evaluates to the same thing.
  for (const auto &IndexedLoan : llvm::enumerate(loans)) {
    for (const Path *p : IndexedLoan.value().path->prefixes()) {
      if (p->structurallyEquals(*path)) {
        loanIndexes.push_back(IndexedLoan.index());
      }
    }
  }

  return loanIndexes;
}

template <typename CB>
void LoansInScope::SimulateBlock(LoansFact &fact, const CFGBlock *Block,
                                 CB callback) {
  fact.clear();

  // Everything live at end of a pred is live at the entry of the block.
  for (auto pred : Block->preds()) {
    SetFrom(fact, loansInScopeAfterBlock[pred]);
  }

  // Walk forwards through the actions on by one.
  for (CFGBlock::const_iterator it = Block->begin(), ei = Block->end();
       it != ei; ++it) {
    const CFGElement &elem = *it;
    const Stmt *S = nullptr;

    if (elem.getAs<CFGStmt>())
      S = elem.castAs<CFGStmt>().getStmt();

    Point point(Block->getBlockID(), std::distance(Block->begin(), it) + 1);

    // kill any loans where `point` is not in their region
    for (unsigned loanIndex : LoansNotInScopeAt(point)) {
      Kill(fact, loanIndex);
    }

    // callback at start of the action
    callback(point, S, fact);

    // bring the loan into scope after the borrow
    if (loansByPoint.find(point) != loansByPoint.end()) {
      Gen(fact, loansByPoint[point]);
    }

    // Figure out which path is overwritten by this action; this may cancel out
    // some loans.
    if (S) {
      const auto &actions = rc.getActionMap().at(point);
      for (const std::unique_ptr<Action> &action : actions) {
        llvm::Optional<const Path *> overwritten = action->OverWrites();
        if (overwritten.hasValue()) {
          const Path *overwrittenPath = overwritten.getValue();
          for (unsigned loanIndex : LoansKilledByWriteTo(overwrittenPath)) {
            Kill(fact, loanIndex);
          }
        }
      }
    }
  }
}

/// Iterates until a fixed point, computing the loans in scope after each block
/// terminates.
void LoansInScope::Compute() {
  LoansFact fact;
  bool changed = true;
  while (changed) {
    changed = false;

    for (const CFGBlock *B : env.cfg.const_reverse_nodes()) {
      SimulateBlock(fact, B, [](auto _p, auto _a, auto _s) {});
      changed |= SetFrom(loansInScopeAfterBlock[B], fact);
    }
  }
}

/// Invoke `callback` with the loans in scope at each point.
template <typename CB> void LoansInScope::Walk(CB callback) {
  llvm::SmallVector<Loan> loans;
  LoansFact fact;

  for (const CFGBlock *B : env.cfg.const_reverse_nodes()) {
    SimulateBlock(fact, B, [&](Point point, const Stmt *S, LoansFact &fact) {
      // Convert from the LoansFact into a vector of loans.
      loans.clear();
      for (const auto &IndexedLoan : llvm::enumerate(this->loans)) {
        if (fact.find(IndexedLoan.index()) != fact.end())
          loans.push_back(IndexedLoan.value());
      }

      // Invoke the callback, check actions at each point according to `loans`.
      callback(point, S, loans);
    });
  }
}

//===----------------------------------------------------------------------===//
//                      Check functions on BorrowCheck
//===----------------------------------------------------------------------===//

void BorrowCheck::CheckAction(const std::unique_ptr<Action> &action) {
  IsBorrow = false;
#if DEBUG_PRINT
  llvm::outs() << "CheckAction(" << action->print() << ") at " << point.print()
               << '\n';
#endif
  switch (action->getKind()) {
  case Action::Assign: {
    const ActionAssign *AA = llvm::cast<ActionAssign>(action.get());
    CheckShallowWrite(AA->Dest);
    CheckRead(AA->Source);
    for (const std::unique_ptr<Path> &Deref : AA->DerefSources) {
      if (Deref->base->ty.isConstBorrow()) {
        CheckRead(Deref);
      } else {
        CheckMutBorrow(Deref);
      }
    }
    break;
  }
  case Action::Borrow: {
    const ActionBorrow *AB = llvm::cast<ActionBorrow>(action.get());
    CheckShallowWrite(AB->Dest);
    IsBorrow = true;
    if (AB->BK == BorrowKind::Shared) {
      CheckRead(AB->Source);
    } else {
      CheckMutBorrow(AB->Source);
    }
    break;
  }
  case Action::Init: {
    const ActionInit *AI = llvm::cast<ActionInit>(action.get());
    CheckShallowWrite(AI->Dest);
    for (const std::unique_ptr<Path> &Source : AI->Sources) {
      if (Source->ty.isOwnedQualified() || Source->ty->isMoveSemanticType()) {
        CheckMove(Source);
      } else {
        CheckRead(Source);
      }
    }
    for (const std::unique_ptr<Path> &Deref : AI->DerefSources) {
      if (Deref->base->ty.isConstBorrow()) {
        CheckRead(Deref);
      } else {
        CheckMutBorrow(Deref);
      }
    }
    break;
  }
  case Action::StorageDead: {
    const ActionStorageDead *ASD = llvm::cast<ActionStorageDead>(action.get());
    CheckStorageDead(ASD->Var);
    break;
  }
  case Action::Use: {
    const ActionUse *AU = llvm::cast<ActionUse>(action.get());
    for (const std::unique_ptr<Path> &Use : AU->Uses) {
      if (Use->ty.isOwnedQualified() || Use->ty->isMoveSemanticType()) {
        CheckMove(Use);
      } else {
        CheckRead(Use);
      }
    }
    for (const std::unique_ptr<Path> &Deref : AU->DerefSources) {
      if (Deref->base->ty.isConstBorrow()) {
        CheckRead(Deref);
      } else {
        CheckMutBorrow(Deref);
      }
    }
    break;
  }
  default:
    break;
  }
}

void BorrowCheck::CheckBorrows(Depth depth, Mode accessMode,
                               const std::unique_ptr<Path> &path) {
  llvm::SmallVector<const Loan *> loans;
  switch (depth) {
  case Depth::Shallow:
    loans = FindLoansThatFreeze(path);
    break;
  case Depth::Deep:
    loans = FindLoansThatIntersect(path);
    break;
  }

  for (const Loan *loan : loans) {
    switch (accessMode) {
    case Mode::Read: {
      switch (loan->kind) {
      case BorrowKind::Shared:
        /* Ok */
        break;
      case BorrowKind::Mut: {
        if (IsBorrow) {
          reporter.emitDiag(BorrowDiagKind::ForImmutWhenMut, path->Location,
                            path.get(), loan->path->Location);
        } else {
          reporter.emitDiag(BorrowDiagKind::ForRead, path->Location, path.get(),
                            loan->path->Location, loan->path.get());
        }
        return;
      }
      }
      break;
    }
    case Mode::Write: {
      if (depth == Depth::Shallow) {
        reporter.emitDiag(BorrowDiagKind::ForWrite, path->Location, path.get(),
                          loan->path->Location, loan->path.get());
      } else {
        if (loan->kind == BorrowKind::Mut) {
          reporter.emitDiag(BorrowDiagKind::ForMultiMut, path->Location,
                            path.get(), loan->path->Location);
        } else {
          reporter.emitDiag(BorrowDiagKind::ForMutWhenImmut, path->Location,
                            path.get(), loan->path->Location);
        }
      }
      return;
    }
    }
  }
}

/// Cannot move from a path `p` if:
/// - `p` is borrowed;
/// - some subpath `p.foo` is borrowed;
/// - some prefix of `p` is borrowed.
///
/// Note that it is stricter than both write and storage dead. In particular,
/// you can write to a variable `x` that contains an `&mut` value when `*x` is
/// borrowed, but you cannot move `x`. This is because moving it would make the
/// `&mut` variable in the new location, but writing (and storage dead) both
/// kill it forever.
void BorrowCheck::CheckMove(const std::unique_ptr<Path> &path) {
  for (const Loan *loan : FindLoansThatIntersect(path)) {
    reporter.emitDiag(BorrowDiagKind::ForMove, path->Location, path.get(),
                      loan->path->Location, loan->path.get());
  }
}

/// `&mut x` may mutate `x`, but it can also *read* from `x`, and mutate things
/// reachable from `x`.
void BorrowCheck::CheckMutBorrow(const std::unique_ptr<Path> &path) {
  CheckBorrows(Depth::Deep, Mode::Write, path);
}

/// `use(x)` may access `x` and (by going through the produced value) anything
/// reachable from `x`.
void BorrowCheck::CheckRead(const std::unique_ptr<Path> &path) {
  CheckBorrows(Depth::Deep, Mode::Read, path);
}

/// `x = ...` overwrites `x` (without reading it) and prevents any further
/// reads from that path.
void BorrowCheck::CheckShallowWrite(const std::unique_ptr<Path> &path) {
  CheckBorrows(Depth::Shallow, Mode::Write, path);
}

/// Cannot free a local variable `var` if:
/// -data interior to `var` is borrowed.
///
/// In particular, having something like `*var` borrowed is ok.
void BorrowCheck::CheckStorageDead(const std::unique_ptr<Path> &path) {
  for (const Loan *loan : FindLoansThatFreeze(path)) {
    reporter.emitDiag(BorrowDiagKind::ForStorageDead, path->Location,
                      loan->path.get(), loan->path->Location);
  }
}

/// Helper for `CheckWrite` and `CheckStorageDead`.
///
/// Finds if there is a loan that "freezes" the given path -- that is, a
/// loan that would make modifying the `path` (or freeing it) illegal.
/// This is slightly more permissive than the rules around move and reads,
/// precisely because overwriting or freeing `path` makes the previous value
/// unavailable from that point on.
llvm::SmallVector<const Loan *>
BorrowCheck::FindLoansThatFreeze(const std::unique_ptr<Path> &path) {
  llvm::SmallVector<const Loan *> loans;
  llvm::SmallVector<const Path *> pathPrefixes = path->prefixes();
  for (const Loan &loan : this->loans) {
    bool NeedToInsert = false;
    llvm::SmallVector<const Path *> frozenPaths = FrozenByBorrowOf(loan.path);
    // If you have borrowed `a.b`, this prevents writes to `a` or `a.b`:
    for (const Path *frozen : frozenPaths) {
      if (frozen->structurallyEquals(*path)) {
        NeedToInsert = true;
        break;
      }
    }
    // If you have borrowed `a.b`, this prevents writes to `a.b.c`:
    for (const Path *prefix : pathPrefixes) {
      if (prefix->structurallyEquals(*loan.path)) {
        NeedToInsert = true;
        break;
      }
    }
    if (NeedToInsert) {
      loans.push_back(&loan);
    }
  }
  return loans;
}

/// A loan L *intersects* a path P if either:
///
/// - the loan is for the path P; or,
/// - the path P can be extended to reach the data in the loan; or,
/// - the loan path can be extended to reach the data in P.
///
/// So, for example, if the path P is `a.b.c`, then:
///
/// - a loan of `a.b.c` intersects P;
/// - a loan of `a.b.c.d` intersects P, because (e.g.) after reading P
///   you have also read `a.b.c.d`;
/// - a loan of `a.b` intersects P, because you can use the
///   reference to access the data at P.
llvm::SmallVector<const Loan *>
BorrowCheck::FindLoansThatIntersect(const std::unique_ptr<Path> &path) {
  llvm::SmallVector<const Loan *> loans;
  llvm::SmallVector<const Path *> pathPrefixes = path->prefixes();
  for (const Loan &loan : this->loans) {
    bool NeedToInsert = false;
    // Accessing `a.b.c` intersects a loan of `a.b.c`, `a.b` and `a`.
    for (const Path *prefix : pathPrefixes) {
      if (prefix->structurallyEquals(*loan.path)) {
        NeedToInsert = true;
        break;
      }
    }
    /// Accessing `a.b.c` also intersects a loan of `a.b.c.d`.
    for (const Path *prefix : loan.path->supportingPrefixes()) {
      if (prefix->structurallyEquals(*path)) {
        NeedToInsert = true;
        break;
      }
    }
    if (NeedToInsert) {
      loans.push_back(&loan);
    }
  }
  return loans;
}

/// If `path` is mutably borrowed, returns a vector of paths which -- if
/// moved or if the storage went away -- would invalidate this
/// reference.
llvm::SmallVector<const Path *>
BorrowCheck::FrozenByBorrowOf(const std::unique_ptr<Path> &path) {
  llvm::SmallVector<const Path *> paths;
  const Path *curPath = path.get();
  while (true) {
    paths.push_back(curPath);
    switch (curPath->type) {
    case Path::PathType::Var: {
      return paths;
    }
    case Path::PathType::Extension: {
      const Path *basePath = curPath->base.get();
      // If you borrowed `*r`, writing to `r` does not actually affect the
      // memory at `*r`, so we can stop iterating backwards now.
      if (basePath->ty->isPointerType() && !basePath->ty.isOwnedQualified()) {
        return paths;
      }
      // If you have borrowed `a.b`, then writing to `a` would overwrite `a.b`,
      // which is disallowed. Besides, for owned pointer `p`, if you have
      // borrowed `*p`, then writing to `p` would overwrite `*p`, which is also
      // disallowed.
      curPath = basePath;
      break;
    }
    }
  }
  return paths;
}

//===----------------------------------------------------------------------===//
//               Operation and query functions on RegionCheck
//===----------------------------------------------------------------------===//

void RegionCheck::PopulateInference(Liveness &liveness) {
  // Process free region for function parameters.
  PreprocessForParamAndReturn();

  // Walk statements to generate liveness constraints, subtyping constraints
  // and reborrow constraints.
  liveness.Walk([&, this](Point point, const Stmt *S,
                          llvm::DenseSet<VarDecl *> liveOnEntry,
                          const VarDecl *VD,
                          SourceLocation LifetimeEndsLoc) -> void {
    // To start, find every variable `x` that is live. All regions in the type
    // of `x` must include `point`.
    std::set<RegionName> liveRegionsOnEntry = liveness.LiveRegions(liveOnEntry);
    for (RegionName Name : liveRegionsOnEntry) {
      RegionVariable RV = getRegionVariable(Name);
      infer.AddLivePoint(RV, point);
    }

    // Skip CFGScopeBegin nodes.
    if (!S && !VD)
      return;

    // Note: Since the last statement of the CFG basic block may be a
    // borrow/reborrow statement, the set of successor points needs to be
    // calculated here.
    llvm::SmallVector<Point> SuccPoints = env.SuccessorPoints(point);

    // Next, walk the actions and establish any additional constraints that may
    // arise from subtyping.
    ActionExtract AE(const_cast<Stmt *>(S), VD, LifetimeEndsLoc, *this);
    actionMap[point] = AE.GetAction();
    const auto &actions = actionMap[point];
    for (const std::unique_ptr<Action> &action : actions) {
#if DEBUG_PRINT
      llvm::outs() << point.print() << ": " << action->print() << '\n';
#endif
      switch (action->getKind()) {
      case Action::Borrow: {
        const ActionBorrow *AB = llvm::dyn_cast<ActionBorrow>(action.get());
        for (Point SuccPoint : SuccPoints) {
          RelateRegions(SuccPoint, AB->RNR, AB->RNL);
          EnsureBorrowSource(SuccPoint, AB->RNR, AB->Source);
        }
        break;
      }
      case Action::Assign: {
        const ActionAssign *AA = llvm::dyn_cast<ActionAssign>(action.get());
        // Note: manually add DerefRN to regionMap.
        getRegionVariable(AA->DerefRN);
        for (Point SuccPoint : SuccPoints) {
          RelateRegions(SuccPoint, AA->DerefRN, AA->RNL);
          RelateRegions(SuccPoint, AA->RNR, AA->DerefRN);
        }
        break;
      }
      default:
        break;
      }
    }
  });
}

void RegionCheck::Check() {
  // Compute liveness.
  Liveness liveness(env, *this);
#if DEBUG_PRINT
  liveness.print();
#endif
  // Add inference constraints.
  PopulateInference(liveness);

  // Solve inference constraints, reporting any errors.
  infer.Solve(env);

  // Compute loans in scope at each point.
  LoansInScope LIS(env, *this);

#if DEBUG_PRINT
  llvm::outs() << "========== BorrowCk ==========\n";
#endif
  // Run the borrow check, reporting any errors.
  BorrowCk(*this, LIS);
}

RegionName RegionCheck::getRegionName(Decl *D) {
  if (declToRegionNameMap.find(D) != declToRegionNameMap.end())
    return declToRegionNameMap[D];
  RegionName RN = RegionName::Create();
#if DEBUG_PRINT
  llvm::outs() << "Decl: { " << cast<VarDecl>(D)->getName() << " } => "
               << RN.print() << '\n';
#endif
  declToRegionNameMap[D] = RN;
  // Ensure that the region variable corresponding to RN exists.
  (void)getRegionVariable(RN);
  return RN;
}

RegionName RegionCheck::getRegionName(Stmt *S) {
  if (stmtToRegionNameMap.find(S) != stmtToRegionNameMap.end())
    return stmtToRegionNameMap[S];
  RegionName RN = RegionName::Create();
#if DEBUG_PRINT
  llvm::outs() << "Stmt: { " << S->getStmtClassName() << " } => " << RN.print()
               << '\n';
#endif
  stmtToRegionNameMap[S] = RN;
  // Ensure that the region variable corresponding to RN exists.
  (void)getRegionVariable(RN);
  return RN;
}

/// Given a RegionName, returns the corresponding RegionVariable from regionMap.
/// If not existing, insert it into regionMap and return it.
RegionVariable RegionCheck::getRegionVariable(RegionName Name) {
  if (regionMap.find(Name) != regionMap.end())
    return regionMap[Name];
  RegionVariable RV = infer.AddVar(Name);
  regionMap[Name] = RV;
  return RV;
}

/// Given a RegionName, returns the corresponding Region from InferenceContext.
const Region &RegionCheck::getRegion(RegionName RN) const {
  assert(regionMap.find(RN) != regionMap.end() &&
         ("no region variable ever created with name: " + RN.print()).c_str());
  RegionVariable RV = regionMap.at(RN);
  return infer.getRegion(RV);
}

/// EnsureBorrowSource - Add any relations between regions that are needed to
/// ensures that reborrows live long enough.
/// Specifically, if we borrow something like `*r` for `'a`,
/// where `r: &'b i32`, then `'b: 'a` is required.
void RegionCheck::EnsureBorrowSource(Point SuccPoint,
                                     RegionName BorrowRegionName,
                                     const std::unique_ptr<Path> &SourcePath) {
#if DEBUG_PRINT
  llvm::outs() << "EnsureBorrowSource(" << SuccPoint.print() << ", "
               << BorrowRegionName.print() << ", " << SourcePath->print()
               << ")\n";
#endif
  for (const Path *prefix : SourcePath->supportingPrefixes()) {
    switch (prefix->type) {
    case Path::PathType::Var: {
      return;
    }
    case Path::PathType::Extension: {
      if (prefix->base->ty.isBorrowQualified() && prefix->base->D != nullptr) {
        RegionName RefRegionName = getRegionName(prefix->base->D);
        RegionVariable BorrowRV = getRegionVariable(BorrowRegionName);
        RegionVariable RefRV = getRegionVariable(RefRegionName);
        infer.AddOutLives(BorrowRV, RefRV, SuccPoint);
      }
      break;
    }
    }
  }
}

void RegionCheck::RelateRegions(Point SuccPoint, RegionName Sub,
                                RegionName Sup) {
  RegionVariable SubRV = getRegionVariable(Sub);
  RegionVariable SupRV = getRegionVariable(Sup);
#if DEBUG_PRINT
  llvm::outs() << "RelateRegions: " << SubRV.print() << " : " << SupRV.print()
               << " @ " << SuccPoint.print() << '\n';
#endif
  infer.AddOutLives(SupRV, SubRV, SuccPoint);
}

/// If the return type of a function is borrow qualifed or has borrow fields,
/// we relate a free region to function parameters.
void RegionCheck::PreprocessForParamAndReturn() {
  if (!IsTrackedType(env, env.fd.getReturnType()))
    return;

  RegionName ParamRN = RegionName::Create();
  for (ParmVarDecl *PVD : env.fd.parameters()) {
    if (IsTrackedType(env, PVD->getType())) {
#if DEBUG_PRINT
      llvm::outs() << "Decl: { " << cast<VarDecl>(PVD)->getName() << " } => "
                   << ParamRN.print() << '\n';
#endif
      declToRegionNameMap[PVD] = ParamRN;
    }
  }

  RegionName FreeRN = RegionName::CreateFree();
  // This is sort of a hack, but for the free region `'region_r`, we will wind
  // up with a region variable. We want that region variable to be inferred to
  // precisely the set: `{G, End('region_r)}`, where `G` is all the points in
  // the control-flow graph, and `End('region_r)` is the end-point of
  // `'region_r`. We are enforcing (in inference) that `'region_r` doesn't get
  // inferred to some larger region.
  RegionVariable FreeRV = getRegionVariable(FreeRN);
  for (const CFGBlock *block : env.cfg.const_nodes()) {
    unsigned blockID = block->getBlockID();
    for (CFGBlock::const_iterator it = block->begin(), ei = block->end();
         it != ei; ++it) {
      Point point(blockID, std::distance(block->begin(), it) + 1);
      infer.AddLivePoint(FreeRV, point);
    }
  }
  infer.AddLivePoint(FreeRV, Point(Point::EndBlockID, Point::EndIndex));
  infer.CapVar(FreeRV);
}

/// Traverse each statement in the CFG, check the corresponding actions and
/// report erros according to the loans in scope information.
void clang::borrow::BorrowCk(RegionCheck &rc,
                             LoansInScope &LIS) {
  LIS.Walk([&](Point point, const Stmt *S,
               const llvm::SmallVector<Loan> &loans) -> void {
    BorrowCheck borrowck(rc.getReporter(), point, loans);
#if DEBUG_PRINT
    llvm::outs() << "Point: " << point.print() << '\n';
    llvm::outs() << "Loans: [\n";
    for (const Loan &loan : loans) {
      llvm::outs() << loan.print() << ",\n";
    }
    llvm::outs() << "]\n";
#endif
    if (rc.getActionMap().find(point) != rc.getActionMap().end()) {
      const auto &actions = rc.getActionMap().at(point);
      for (const std::unique_ptr<Action> &action : actions) {
        borrowck.CheckAction(action);
      }
    }
  });
}

// Entry point of borrow checker.
void clang::borrow::runBorrowChecker(const FunctionDecl &fd, const CFG &cfg,
                                     ASTContext &Ctx,
                                     BorrowDiagReporter &Reporter) {
  RegionCheck RC(fd, cfg, Ctx, Reporter);
  RC.Check();
}

#endif