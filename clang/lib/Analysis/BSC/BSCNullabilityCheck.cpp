//===- BSCNullabilityCheck.cpp - Nullability Check for Source CFGs -*- BSC--//
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

#if ENABLE_BSC

#include "clang/AST/BSC/ExprBSC.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Analysis/Analyses/BSC/BSCNullCheckInfo.h"
#include "clang/Analysis/Analyses/BSC/BSCNullabilityCheck.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/FlowSensitive/DataflowWorklist.h"
#include "clang/Basic/Builtins.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Optional.h"

using namespace clang;
using namespace std;

// DefNullability is determined when a pointer is declared:
//   1. has nullability specifier, DefNullability depends on nullability
//   specifier.
//   2. has no nullability specifier, DefNullability depends on type:
//      1) raw pointer is nullable by default,
//      2) owned or borrow pointer is nonnull by default.
// Pointer with nullable DefNullability have PathNullability,
// which will change with control flow.
// FieldPath models any nullable-pointer cell reachable from one local variable
// through member access and unary dereference, normalized to a single spelling.
// The empty path denotes the variable itself; ".name" is a field access and
// ".*" is a dereference. Normalization makes equivalent spellings share a key:
//   s->p   and   (*s).p   both map to (s, ".*.p")
//   *p / **p             map to (p, ".*") / (p, ".*.*")
// Every path segment begins with '.', so a path Q is a descendant of path P iff
// P is a strict prefix of Q that ends right before a '.' (see
// isFieldPathPrefix). This guards against a field named "mv" reading as a
// descendant of a field named "m".
using FieldPath = std::pair<VarDecl *, std::string>;
using StatusFP = std::map<FieldPath, NullabilityKind>;

class NullabilityCheckImpl {
public:
  llvm::DenseMap<const CFGBlock *, StatusFP> BlocksBeginStatusFP;
  llvm::DenseMap<const CFGBlock *, StatusFP> BlocksEndStatusFP;

  // For branch statement with condition, such as IfStmt, WhileStmt,
  // true branch and else branch may have different status.
  // For example:
  // @code
  //     int *p = nullptr;
  //     if (p != nullptr) {
  //         *p = 5;   // p is NonNull, so deref p is Ok
  //     } else {
  //         *p = 10;  // p is Nullable, so deref p is forbidden
  //     }
  // @endcode
  // CFG is:
  //        B4(has condition as terminitor)
  //    true /      \ false
  //        B3      B2
  //          \    /
  //            B1
  // BlocksConditionStatus records condition status:
  // Key is current BB, value is condition status passed from pred BB to current
  // BB, for this example, BlocksConditionStatusFP will be:
  // B3 : { B4 : { p NonNull } }  B2 : { B4 : { p Nullable } }
  llvm::DenseMap<const CFGBlock *, llvm::DenseMap<const CFGBlock *, StatusFP>>
      BlocksConditionStatusFP;

  StatusFP mergeFP(StatusFP statusA, StatusFP statusB);

  StatusFP runOnBlock(const CFGBlock *block, StatusFP statusFP,
                      NullabilityCheckDiagReporter &reporter,
                      ASTContext &ctx, const FunctionDecl &fd, ParentMap &PM);
  void initStatus(const CFG &cfg);

  NullabilityCheckImpl()
      : BlocksBeginStatusFP(0), BlocksEndStatusFP(0),
        BlocksConditionStatusFP(0) {}
};

//===----------------------------------------------------------------------===//
// Dataflow computation.
//===----------------------------------------------------------------------===//
namespace {
class TransferFunctions : public StmtVisitor<TransferFunctions> {
  NullabilityCheckImpl &NCI;
  const CFGBlock *Block;
  StatusFP &CurrStatusFP;
  NullabilityCheckDiagReporter &Reporter;
  ASTContext &Ctx;
  const FunctionDecl &Fd;
  ParentMap &PM;

public:
  TransferFunctions(NullabilityCheckImpl &nci, const CFGBlock *block,
                    StatusFP &statusFP,
                    NullabilityCheckDiagReporter &reporter, ASTContext &ctx,
                    const FunctionDecl &fd, ParentMap &pm)
      : NCI(nci), Block(block), CurrStatusFP(statusFP), Reporter(reporter),
        Ctx(ctx), Fd(fd), PM(pm) {}

  bool IsStmtInSafeZone(Stmt *S);
  bool ShouldReportNullPtrError(Stmt *S);
  void VisitDeclStmt(DeclStmt *S);
  void CheckInit(DeclStmt *DS, VarDecl *VD,
                 QualType QT, Expr *Init, std::string Path);
  bool checkPathSensitiveFirstLevel(Expr *FirstLevel, QualType &CurLHS,
                                     QualType &CurRHS,
                                     NullabilityCheckDiagKind DiagKind,
                                     SourceLocation DiagLoc);
  void VisitBinaryOperator(BinaryOperator *BO);
  void VisitUnaryOperator(UnaryOperator *UO);
  void VisitArraySubscriptExpr(ArraySubscriptExpr *ASE);
  void VisitMemberExpr(MemberExpr *ME);
  void VisitCallExpr(CallExpr *CE);
  void VisitReturnStmt(ReturnStmt *RS);
  void VisitCStyleCastExpr(CStyleCastExpr *CSCE);
  bool IsDirectBorrowAddrOperand(Stmt *S);
  NullabilityKind getExprPathNullability(Expr *E);
  void SetCFGBlocksByExpr(Expr *PtrE, const CFGBlock *NonNullBlock,
                          const CFGBlock *NullableBlock);
  void PassConditionStatusToSuccBlocks(Expr *CondExpr);
  void UpdateDerefStatusFromRHS(VarDecl *VD, QualType RHSType);
};

// Whether \p prefix is a strict ancestor of (or equal to) \p path. Paths are
// normalized so every segment begins with '.' (".name" / ".*"), which lets a
// single trailing-'.' check delimit segments without confusing a field named
// "mv" for a descendant of a field named "m".
bool isFieldPathPrefix(const std::string &prefix, const std::string &path) {
  if (prefix == path)
    return true;
  if (path.size() > prefix.size() &&
      path.compare(0, prefix.size(), prefix) == 0 &&
      path[prefix.size()] == '.')
    return true;
  return false;
}

// Resolve \p E to the nullable-pointer cell it names, normalized to a canonical
// spelling. The root is a local/parameter VarDecl; the path is a sequence of
// ".name" (field access) and ".*" (unary dereference) segments. Equivalent
// spellings collapse to one key:
//   p              -> {p, ""}
//   s.p            -> {s, ".p"}
//   s->p, (*s).p   -> {s, ".*.p"}
//   *p, **p        -> {p, ".*"}, {p, ".*.*"}
//   *base.f1       -> {base, ".f1.*"}
// Returns None when \p E is not a trackable cell (constant, call, etc.).
llvm::Optional<FieldPath> getFieldPath(Expr *E) {
  if (!E)
    return llvm::None;

  if (auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (auto *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      return FieldPath(VD, "");
    return llvm::None;
  }

  if (auto *ME = dyn_cast<MemberExpr>(E)) {
    auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
    if (!FD)
      return llvm::None;
    auto Base = getFieldPath(ME->getBase());
    // When the base is untrackable (an array element `a[i].f`, a call result,
    // ...), fall back to a field-name-keyed cell rooted at null. The old
    // VisitMEForFieldPath stopped descending at such bases and left the root
    // null; array-element fields of the same name then share one cell, seeded
    // Nullable and refined by assignment.
    FieldPath Result = Base ? *Base : FieldPath(nullptr, "");
    // p->f dereferences p, so it contributes a ".*" segment ahead of ".f".
    if (ME->isArrow())
      Result.second += ".*";
    Result.second += "." + FD->getNameAsString();
    return Result;
  }

  if (auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() != UO_Deref)
      return llvm::None;
    auto Sub = getFieldPath(UO->getSubExpr());
    if (!Sub)
      return llvm::None;
    Sub->second += ".*";
    return Sub;
  }

  if (auto *ICE = dyn_cast<ImplicitCastExpr>(E))
    return getFieldPath(ICE->getSubExpr());

  if (auto *PE = dyn_cast<ParenExpr>(E))
    return getFieldPath(PE->getSubExpr());

  if (auto *SE = dyn_cast<SafeExpr>(E))
    return getFieldPath(SE->getSubExpr());

  return llvm::None;
}

// Erase only the strict descendants of \p Path, preserving \p Path itself (used
// when the cell named by \p Path is overwritten but its own nullability is
// refreshed rather than dropped).
void eraseDeeperPaths(StatusFP &Status, const FieldPath &Path) {
  auto It = Status.begin();
  while (It != Status.end()) {
    if (It->first.first == Path.first &&
        It->first.second != Path.second &&
        isFieldPathPrefix(Path.second, It->first.second))
      It = Status.erase(It);
    else
      ++It;
  }
}

// A "deref chain" path is a (possibly empty) repetition of the ".*" segment:
// "" (the var), ".*" (*p), ".*.*" (**p), ...
bool isDerefChainPath(const std::string &Path) {
  if (Path.size() % 2 != 0)
    return false;
  for (size_t I = 0; I < Path.size(); I += 2)
    if (Path[I] != '.' || Path[I + 1] != '*')
      return false;
  return true;
}

// Erase the deref-chain cells of \p VD — *p, **p, ... — while preserving field
// cells reachable through \p VD ("*.name", "" ), whose defaultability comes
// from the field type rather than VD's rebound pointee. Used on a plain-variable
// rebind (VD = RHS), which replaces the dereference target but not the pointee's
// field layout; UpdateDerefStatusFromRHS re-seeds the deref chain.
void eraseDerefChainCells(StatusFP &Status, VarDecl *VD) {
  auto It = Status.begin();
  while (It != Status.end()) {
    if (It->first.first == VD && !It->first.second.empty() &&
        isDerefChainPath(It->first.second))
      It = Status.erase(It);
    else
      ++It;
  }
}

VarDecl *getVarDeclFromExpr(Expr *E) {
  if (auto *DRE = dyn_cast<DeclRefExpr>(E))
    if (VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      return VD;
  if (auto *ICE = dyn_cast<ImplicitCastExpr>(E))
    return getVarDeclFromExpr(ICE->getSubExpr());
  if (auto *PE = dyn_cast<ParenExpr>(E))
    return getVarDeclFromExpr(PE->getSubExpr());
  if (auto *SE = dyn_cast<SafeExpr>(E))
    return getVarDeclFromExpr(SE->getSubExpr());
  if (auto *BO = dyn_cast<BinaryOperator>(E))
    return getVarDeclFromExpr(BO->getLHS());
  return nullptr;
}

MemberExpr *getMemberExprFromExpr(Expr *E) {
  if (auto *ME = dyn_cast<MemberExpr>(E))
    return ME;
  if (auto *ICE = dyn_cast<ImplicitCastExpr>(E))
    return getMemberExprFromExpr(ICE->getSubExpr());
  if (auto *PE = dyn_cast<ParenExpr>(E))
    return getMemberExprFromExpr(PE->getSubExpr());
  if (auto *SE = dyn_cast<SafeExpr>(E))
    return getMemberExprFromExpr(SE->getSubExpr());
  return nullptr;
}

/// Resolve the InitListExpr that directly carries the value of \p Base when
/// \p Base denotes an element/field of a compound-literal aggregate. Supports
/// two base shapes:
///  - the compound literal itself: `(T[N]){...}` or `(struct S){...}`
///  - a member of a compound-literal record whose field is itself an
///    aggregate: `((struct SArr){0}).arr` (arr is T[N])
/// Returns nullptr when the base is not rooted at a compound literal, in
/// which case the static element/field type should be trusted as before.
static InitListExpr *getCompoundLiteralInitList(Expr *Base, ASTContext &Ctx) {
  Base = Base->IgnoreParenImpCastsSafe();
  if (auto *CLE = dyn_cast<CompoundLiteralExpr>(Base))
    return dyn_cast<InitListExpr>(CLE->getInitializer());
  // `((struct SArr){0}).arr` — the base is a MemberExpr whose base is a
  // compound-literal record; recurse to that record's init list, then index
  // into the accessed field.
  if (auto *ME = dyn_cast<MemberExpr>(Base)) {
    auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
    if (!FD)
      return nullptr;
    InitListExpr *ParentILE = getCompoundLiteralInitList(ME->getBase(), Ctx);
    if (!ParentILE)
      return nullptr;
    unsigned Idx = FD->getFieldIndex();
    if (Idx >= ParentILE->getNumInits())
      return nullptr;
    return dyn_cast<InitListExpr>(ParentILE->getInit(Idx));
  }
  return nullptr;
}

/// Given the base of an ArraySubscriptExpr / MemberExpr that denotes an
/// element or field of a compound literal aggregate, return the specific
/// initializer expression for that element/field, or nullptr if the base is
/// not a compound literal (in which case the static element/field type
/// should be trusted, as before).
///
/// \p IndexE — for array subscript, the index expression (must fold to a
///             constant).
/// \p Field  — for member access, the accessed FieldDecl.
static Expr *getCompoundLiteralInitElem(Expr *Base, Expr *IndexE,
                                        FieldDecl *Field, ASTContext &Ctx) {
  InitListExpr *ILE = getCompoundLiteralInitList(Base, Ctx);
  if (!ILE)
    return nullptr;
  unsigned Idx;
  if (Field) {
    Idx = Field->getFieldIndex();
  } else {
    if (!IndexE)
      return nullptr;
    Expr::EvalResult ER;
    if (!IndexE->EvaluateAsInt(ER, Ctx) || !ER.Val.isInt())
      return nullptr;
    llvm::APInt V = ER.Val.getInt();
    if (V.isNegative())
      return nullptr;
    Idx = (unsigned)V.getLimitedValue();
  }
  if (Idx >= ILE->getNumInits()) {
    // Omitted array elements are implicitly zero-initialized; Sema materializes
    // them as the InitListExpr's array_filler (an ImplicitValueInitExpr).
    // Surface it so a _Nonnull slot filled with NULL is not laundered to the
    // declared _Nonnull type. (Records never use a filler — Sema fills every
    // field slot explicitly.)
    if (!Field && ILE->hasArrayFiller())
      return ILE->getArrayFiller();
    return nullptr;
  }
  return ILE->getInit(Idx);
}

/// Return the source pointer name used by nullability diagnostics.
/// Comma and assignment expressions produce the value of their RHS, so name
/// extraction follows the RHS. This intentionally differs from data-flow
/// tracking, which follows an assignment's LHS to update the assigned object.
static std::string getDiagNameFromExpr(Expr *E) {
  if (!E)
    return {};

  E = E->IgnoreParenImpCastsSafe();
  if (auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->getOpcode() == BO_Comma || BO->getOpcode() == BO_Assign)
      return getDiagNameFromExpr(BO->getRHS());
    return {};
  }

  if (auto *UO = dyn_cast<UnaryOperator>(E)) {
    // An implicit reborrow should preserve the source name in diagnostics.
    if (UO->getOpcode() == UO_AddrMutDeref ||
        UO->getOpcode() == UO_AddrConstDeref) {
      return getDiagNameFromExpr(UO->getSubExpr());
    }
    return {};
  }

  if (VarDecl *VD = getVarDeclFromExpr(E))
    return VD->getNameAsString();
  if (MemberExpr *ME = getMemberExprFromExpr(E))
    return ME->getMemberDecl()->getNameAsString();
  return {};
}
} // namespace

namespace clang {
bool FindNonnull(QualType QT, const ASTContext &Ctx) {
  QualType CanQT = QT.getCanonicalType();
  if (CanQT->isPointerType())
    return QT.getDefNullability() == NullabilityKind::NonNull;
  if (CanQT->isArrayType()) {
    const auto *ArrayTy = QT->getAsArrayTypeUnsafe();
    QualType ElemTy = ArrayTy->getElementType();
    return FindNonnull(ElemTy, Ctx);
  }
  if (CanQT->isRecordType()) {
    const auto *RT = QT->getAs<RecordType>();
    if (RecordDecl *RD = RT->getDecl()) {
      for (FieldDecl *FD : RD->fields()) {
        QualType FieldTy = FD->getType();
        if (FindNonnull(FieldTy, Ctx))
          return true;
      }
    }
  }
  return false;
}

// Normalize init expr (ignore casts, compound literal)
Expr *NormalizeInitExpr(Expr *E) {
  if (!E)
    return nullptr;
  while (true) {
    if (auto *CSE = dyn_cast<CStyleCastExpr>(E)) {
      E = CSE->getSubExpr()->IgnoreParenImpCasts();
      continue;
    }
    if (auto *CLE = dyn_cast<CompoundLiteralExpr>(E)) {
      if (Expr *Sub = CLE->getInitializer()) {
        E = Sub->IgnoreParenImpCasts();
        continue;
      }
    }
    break;
  }
  return E;
}
} // namespace clang

// We can get PathNullability for these exprs:
//   1. int *p = nullptr;   // nullptr is NullExpr
//   2. int *p = foo();     // foo() is CallExpr
//   3. int *p = &a;        // &a is UnaryOperator
//   4. int *p = p1;        // p1 is VarDecl
//   5. int *p = s.p;       // s.p is MemberExpr
//   6. int *p = a == 1 ? nullptr : &a; // ConditionOperator
//   7. int *p = p1 ?: &a;    // GNU BinaryConditionalOperator
//   8. int *p = va_arg(ap, int *_Nullable); // VAArgExpr
//   9. int *p = ({ int *_Nullable t = q; t; }); // StmtExpr
//  10. int *p = _Generic(q, int *: q, default: (int*)0); // GenericSelectionExpr
NullabilityKind TransferFunctions::getExprPathNullability(Expr *E) {
  if (E->isNullExpr(Ctx))
    return NullabilityKind::Nullable;
  if (E->getStmtClass() == Expr::StringLiteralClass)
    return NullabilityKind::NonNull;
  if (E->getStmtClass() == Expr::ImplicitValueInitExprClass &&
      E->getType()->isPointerType())
    return NullabilityKind::Nullable;
  QualType QT = E->getType();
  QualType CanQT = QT.getCanonicalType();
  if (CanQT->isPointerType()) {
    switch (E->getStmtClass()) {
    case Expr::ParenExprClass:
      return getExprPathNullability(cast<ParenExpr>(E)->getSubExpr());
    case Expr::SafeExprClass:
      return getExprPathNullability(cast<SafeExpr>(E)->getSubExpr());
    case Expr::ImplicitCastExprClass:
      return getExprPathNullability(cast<ImplicitCastExpr>(E)->getSubExpr());
    case Expr::CompoundLiteralExprClass:
      return getExprPathNullability(
          cast<CompoundLiteralExpr>(E)->getInitializer());
    case Expr::CallExprClass: {
      CallExpr *CE = cast<CallExpr>(E);
      if (FunctionDecl *FD = CE->getDirectCallee()) {
        if (CE->getNumArgs() == 1 &&
            (FD->getBuiltinID() == Builtin::BI__move_to_raw ||
             FD->getBuiltinID() == Builtin::BI__take_from_raw ||
             FD->getBuiltinID() == Builtin::BI__move_array_to_raw ||
             FD->getBuiltinID() == Builtin::BI__take_array_from_raw)) {
          return getExprPathNullability(CE->getArg(0));
        }
      }
      return CE->getType().getDefNullability();
    }
    case Expr::ConditionalOperatorClass: {
      NullabilityKind LHSNK =
          getExprPathNullability(cast<ConditionalOperator>(E)->getLHS());
      NullabilityKind RHSNK =
          getExprPathNullability(cast<ConditionalOperator>(E)->getRHS());
      if (LHSNK == NullabilityKind::Nullable ||
          RHSNK == NullabilityKind::Nullable)
        return NullabilityKind::Nullable;
      if (LHSNK == NullabilityKind::NonNull &&
          RHSNK == NullabilityKind::NonNull)
        return NullabilityKind::NonNull;
      break;
    }
    case Expr::BinaryConditionalOperatorClass: {
      // GNU ?: — the true arm reuses the condition value, which is known
      // non-null whenever it is chosen, so the false arm alone decides.
      NullabilityKind FalseNK = getExprPathNullability(
          cast<BinaryConditionalOperator>(E)->getFalseExpr());
      if (FalseNK == NullabilityKind::Nullable ||
          FalseNK == NullabilityKind::NonNull)
        return FalseNK;
      break;
    }
    case Expr::VAArgExprClass:
      return E->getType().getDefNullability();
    case Expr::StmtExprClass: {
      // GCC statement expression ({...}) — the value is the trailing
      // expression of the enclosing compound statement. Use
      // getStmtExprResult() rather than body_back() so that a trailing
      // NullStmt (e.g. ({...; t;; })) is skipped; otherwise the nullable
      // value would be laundered through to Unspecified and escape the
      // NonnullAssignedByNullable / NullablePointerDereference guards.
      if (auto *LastExpr = dyn_cast<Expr>(
              cast<StmtExpr>(E)->getSubStmt()->getStmtExprResult()))
        return getExprPathNullability(LastExpr);
      break;
    }
    case Expr::GenericSelectionExprClass: {
      // _Generic — if the selection is not result-dependent, only the
      // chosen arm can be evaluated at runtime; otherwise be conservative
      // and union the nullability of all arms.
      auto *GSE = cast<GenericSelectionExpr>(E);
      if (!GSE->isResultDependent())
        return getExprPathNullability(GSE->getResultExpr());
      NullabilityKind Result = NullabilityKind::Unspecified;
      for (unsigned I = 0, N = GSE->getNumAssocs(); I < N; ++I) {
        NullabilityKind NK =
            getExprPathNullability(GSE->getAssocExprs()[I]);
        if (NK == NullabilityKind::Nullable)
          return NullabilityKind::Nullable;
        if (NK == NullabilityKind::NonNull)
          Result = NullabilityKind::NonNull;
      }
      return Result;
    }
    case Expr::CStyleCastExprClass: {
      // A pointer cast from a non-zero integer constant expression
      // (e.g. (int*)0x1234, (int*)(123 - 2)) produces a well-known
      // valid address — treat it as NonNull.
      Expr *Sub = cast<CStyleCastExpr>(E)->getSubExpr()->IgnoreParenImpCasts();
      Expr::EvalResult Eval;
      if (Sub->EvaluateAsInt(Eval, Ctx) && Eval.Val.isInt() &&
          !Eval.Val.getInt().isZero())
        return NullabilityKind::NonNull;
      QualType CastTy = cast<CStyleCastExpr>(E)->getTypeAsWritten();
      NullabilityKind CastNK = CastTy.getDefNullability();
      if (CastNK == NullabilityKind::Nullable &&
          Sub->getType().getCanonicalType()->isPointerType())
        return getExprPathNullability(Sub);
      return CastNK;
    }
    case Expr::UnaryOperatorClass: {
      UnaryOperator::Opcode Op = cast<UnaryOperator>(E)->getOpcode();
      if (Op == UO_AddrOf || Op == UO_AddrMut || Op == UO_AddrConst) {
        // &_Mut p[i] / &_Const p[i] borrow an array element without
        // dereferencing p; the result follows the base pointer's
        // path-sensitive nullability, exactly like &_Mut *p.
        if (Op != UO_AddrOf) {
          if (auto *ASE = dyn_cast<ArraySubscriptExpr>(
                  cast<UnaryOperator>(E)->getSubExpr()->IgnoreParenImpCastsSafe())) {
            Expr *Base = ASE->getBase()->IgnoreParenImpCasts();
            if (Base->getType().getCanonicalType()->isPointerType())
              return getExprPathNullability(Base);
            // An array lvalue can never be null.
            return NullabilityKind::NonNull;
          }
        }
        return NullabilityKind::NonNull;
      }
      if (Op == UO_Deref) {
        // Prefer path-sensitive state produced by condition propagation (if
        // (*p), if (**p), ...). Fall back to declaration/default semantics.
        if (auto FP = getFieldPath(E)) {
          auto It = CurrStatusFP.find(*FP);
          if (It != CurrStatusFP.end())
            return It->second;
        }
        return cast<UnaryOperator>(E)->getType().getDefNullability();
      }
      if (Op == UO_AddrMutDeref || Op == UO_AddrConstDeref) {
        return getExprPathNullability(cast<UnaryOperator>(E)->getSubExpr());
      }
      if (cast<UnaryOperator>(E)->isIncrementDecrementOp()) {
        // ++p / p++ / --p / p-- only offset the pointer; the resulting value
        // keeps the operand's nullability.
        return getExprPathNullability(cast<UnaryOperator>(E)->getSubExpr());
      }
      break;
    }
    case Expr::BinaryOperatorClass: {
      BinaryOperator::Opcode Op = cast<BinaryOperator>(E)->getOpcode();
      if (Op == BO_Comma || Op == BO_Assign) {
        return getExprPathNullability(cast<BinaryOperator>(E)->getRHS());
      }
      if (Op == BO_Add || Op == BO_Sub) {
        // Pointer arithmetic (p + n, n + p, p - n) only offset the pointer;
        // the resulting value keeps the operand's nullability.
        BinaryOperator *B = cast<BinaryOperator>(E);
        if (B->getLHS()->getType()->isPointerType())
          return getExprPathNullability(B->getLHS());
        if (B->getRHS()->getType()->isPointerType())
          return getExprPathNullability(B->getRHS());
      }
      break;
    }
    case Expr::CompoundAssignOperatorClass:
      return getExprPathNullability(cast<CompoundAssignOperator>(E)->getLHS());
    case Expr::InitListExprClass: {
      InitListExpr *ILE = cast<InitListExpr>(E);
      if (ILE->getNumInits() > 0) {
        return getExprPathNullability(ILE->getInit(0));
      }
      break;
    }
    case Expr::DeclRefExprClass: {
      if (VarDecl *VD = dyn_cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl())) {
        NullabilityKind NK = VD->getType().getDefNullability();
        if (NK == NullabilityKind::NonNull)
          return NullabilityKind::NonNull;
        if (NK == NullabilityKind::Nullable) {
          FieldPath FP(VD, "");
          if (CurrStatusFP.count(FP))
            return CurrStatusFP[FP];
        }
      }
      break;
    }
    case Expr::ArraySubscriptExprClass: {
      // Builtin array elements cannot have independent path-sensitive state.
      // Exception: the base is a compound literal aggregate.
      auto *ASE = cast<ArraySubscriptExpr>(E);
      if (Expr *InitE = getCompoundLiteralInitElem(
              ASE->getBase(), ASE->getIdx(), /*Field=*/nullptr, Ctx))
        return getExprPathNullability(InitE);
      NullabilityKind NK = ASE->getType().getDefNullability();
      if (NK == NullabilityKind::NonNull || NK == NullabilityKind::Nullable)
        return NK;
      break;
    }
    case Expr::MemberExprClass: {
      auto *ME = cast<MemberExpr>(E);
      if (auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
        if (Expr *InitE = getCompoundLiteralInitElem(
                ME->getBase(), /*IndexE=*/nullptr, FD, Ctx))
          return getExprPathNullability(InitE);
        NullabilityKind NK = FD->getType().getDefNullability();
        if (NK == NullabilityKind::NonNull)
          return NullabilityKind::NonNull;
        if (NK == NullabilityKind::Nullable) {
          if (auto FP = getFieldPath(ME)) {
            if (CurrStatusFP.count(*FP))
              return CurrStatusFP[*FP];
          }
        }
      }
      break;
    }
    default:
      break;
    }
  }
  // For no-pointer type, we treat it as Unspecified.
  return NullabilityKind::Unspecified;
}

bool TransferFunctions::IsStmtInSafeZone(Stmt *S) {
  if (!S)
    return false;
  const Stmt *ParentStmt = PM.getParent(S);
  while (ParentStmt) {
    if (auto *CS = dyn_cast<CompoundStmt>(ParentStmt)) {
      SafeZoneSpecifier SafeZoneSpec = CS->getCompSafeZoneSpecifier();
      if (SafeZoneSpec == SZ_Safe)
        return true;
      if (SafeZoneSpec == SZ_Unsafe)
        return false;
    }
    if (auto *SS = dyn_cast<SafeStmt>(ParentStmt)) {
      SafeZoneSpecifier SafeZoneSpec = SS->getSafeZoneSpecifier();
      if (SafeZoneSpec == SZ_Safe)
        return true;
      if (SafeZoneSpec == SZ_Unsafe)
        return false;
    }
    if (auto *SE = dyn_cast<SafeExpr>(ParentStmt)) {
      SafeZoneSpecifier SafeZoneSpec = SE->getSafeZoneSpecifier();
      if (SafeZoneSpec == SZ_Safe)
        return true;
      if (SafeZoneSpec == SZ_Unsafe)
        return false;
    }
    ParentStmt = PM.getParent(ParentStmt);
  }
  return Fd.getSafeZoneSpecifier() == SZ_Safe;
}

bool TransferFunctions::ShouldReportNullPtrError(Stmt *S) {
  LangOptions::NullCheckZone CheckZone = Ctx.getLangOpts().getNullabilityCheck();
  if (CheckZone == LangOptions::NC_ALL) {
    return true;
  }
  return IsStmtInSafeZone(S);
}

void TransferFunctions::VisitDeclStmt(DeclStmt *DS) {
  for (Decl *D : DS->decls()) {
    if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
      Expr *Init = VD->getInit();
      if (Init || VD->isStaticLocal()) {
        CheckInit(DS, VD, VD->getType(), Init, "");
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// checkNestedPointerNullability — shared by CheckInit, VisitBinaryOperator,
// and VisitCallExpr
//===----------------------------------------------------------------------===//

/// Walk nested pointer levels of \p LHS and \p RHS in parallel.  At each
/// inner level where either side is itself a pointer type, require one of the
/// following two cases:
/// 1. The type-based (def) nullability of the two pointers is identical
/// 2. Two pointers are const and is assigning _Nonnull pointers to _Nullable
/// Reports via \p Reporter on the first mismatch and stops.
/// \p OrigLHS / \p OrigRHS are the full (pre-stripped) types, used only for
/// the diagnostic message.
static void checkNestedPointerNullability(QualType LHS, QualType RHS,
    QualType OrigLHS, QualType OrigRHS,
    ASTContext &Ctx, NullabilityCheckDiagReporter &Reporter,
    SourceLocation DiagLoc) {
  QualType CurLHS = LHS;
  QualType CurRHS = RHS;
  while (CurLHS->isPointerType() && CurRHS->isPointerType()) {
    NullabilityKind LHSKind = CurLHS.getDefNullability();
    NullabilityKind RHSKind = CurRHS.getDefNullability();
    if (LHSKind != RHSKind &&
        !(LHSKind == NullabilityKind::Nullable &&
          RHSKind == NullabilityKind::NonNull &&
          CurLHS.isConstQualified() && CurRHS.isConstQualified())) {
      NullabilityCheckDiagInfo DI(DiagLoc, NestedNullabilityMismatch,
                                  OrigRHS, OrigLHS, CurRHS, CurLHS);
      Reporter.addDiagInfo(DI);
      return;
    }
    const auto *LHSPT = CurLHS->getAs<PointerType>();
    const auto *RHSPT = CurRHS->getAs<PointerType>();
    if (!LHSPT || !RHSPT)
      return;
    CurLHS = LHSPT->getPointeeType();
    CurRHS = RHSPT->getPointeeType();
  }
}

/// If \p E is &_Mut var / &_Const var, return var (the borrowed
/// variable expression); otherwise return nullptr
static Expr *stripBorrow(Expr *E) {
  if (auto *UO = dyn_cast<UnaryOperator>(E->IgnoreParenImpCasts()))
    if (UO->getOpcode() == UO_AddrMut ||
        UO->getOpcode() == UO_AddrConst)
      return UO->getSubExpr()->IgnoreParenImpCasts();
  return nullptr;
}

/// Path-sensitive first-level check for &_Mut var / &_Const var.
/// When \p FirstLevel is non-null and the LHS first-inner expects NonNull,
/// checks getExprPathNullability(FirstLevel).  On violation reports and
/// returns false.  Always advances \p CurLHS / \p CurRHS past the first
/// level on success so the type walker only checks deeper levels.
bool TransferFunctions::checkPathSensitiveFirstLevel(
    Expr *FirstLevel, QualType &CurLHS, QualType &CurRHS,
    NullabilityCheckDiagKind DiagKind,
    SourceLocation DiagLoc) {
  const auto *PT = CurLHS->getAs<PointerType>();
  const auto *RHSPT = CurRHS->getAs<PointerType>();
  if (!PT || !RHSPT)
    return false;

  // One-level advance (strip outer, e.g. _Borrow / first pointer).
  auto AdvanceOne = [&]() {
    CurLHS = PT->getPointeeType();
    CurRHS = RHSPT->getPointeeType();
  };

  if (FirstLevel) {
    QualType Inner = PT->getPointeeType();
    if (Inner->isPointerType() &&
        Inner.getDefNullability() == NullabilityKind::NonNull) {
      if (getExprPathNullability(FirstLevel) == NullabilityKind::Nullable) {
        NullabilityCheckDiagInfo DI(DiagLoc, DiagKind);
        DI.Name = getDiagNameFromExpr(FirstLevel);
        Reporter.addDiagInfo(DI);
        return false;
      }
      // Path check passed — advance two levels: the outer (e.g. _Borrow)
      // and the level that was just proven NonNull by path-sensitivity.
      AdvanceOne(); // now CurLHS/CurRHS = the path-checked level
      const auto *NextPT = CurLHS->getAs<PointerType>();
      if (!NextPT) return true;
      CurLHS = NextPT->getPointeeType();
      const auto *NextRHSPT = CurRHS->getAs<PointerType>();
      if (!NextRHSPT) return true;
      CurRHS = NextRHSPT->getPointeeType();
      return true;
    }
  }

  AdvanceOne();
  return true;
}

void TransferFunctions::CheckInit(DeclStmt *DS, VarDecl *VD,
                                  QualType QT, Expr *Init,
                                  std::string path) {
  QualType CanQT = QT.getCanonicalType();
  // early return if no initialization
  if (!Init || isa<ImplicitValueInitExpr>(Init)) {
    if (ShouldReportNullPtrError(DS) && FindNonnull(QT, Ctx)) {
      NullabilityCheckDiagInfo DI(VD->getLocation(), NonnullInitByDefault,
                                  VD->getNameAsString());
      Reporter.addDiagInfo(DI);
    }
    return;
  }
  if (CanQT->isPointerType()) {
    // check pointer initialization
    NullabilityKind LHSKind = QT.getDefNullability();
    NullabilityKind RHSKind = getExprPathNullability(Init);
    if (LHSKind == NullabilityKind::NonNull) {
      if (RHSKind == NullabilityKind::Nullable && ShouldReportNullPtrError(DS)) {
        NullabilityCheckDiagInfo DI(VD->getLocation(),
                                    NonnullAssignedByNullable,
                                    getDiagNameFromExpr(Init));
        Reporter.addDiagInfo(DI);
      }
    } else {
      FieldPath FP(VD, path);
      if (CurrStatusFP.count(FP)) {
        // Here we update PathNullability of nullable pointer.
        CurrStatusFP[FP] = RHSKind;
      }
    }
    if (path.empty()) {
      // Declaration-time rebinding can stale existing dereference-chain facts.
      // Example:
      //   if (*p) { /* (*p) is NonNull on this path */ }
      //   int **p = q; // root pointer changes, old (*p) fact must be dropped.
      eraseDerefChainCells(CurrStatusFP, VD);
      UpdateDerefStatusFromRHS(VD, Init->IgnoreParenImpCasts()->getType());
    }

    // --- Inner-pointer check for init expressions ---
    // IgnoreParenImpCasts: Sema may BitCast the initializer to the declared
    // type when nested nullability lives in qualifier bits; use the pre-cast
    // type so nested mismatches remain visible.
    if (ShouldReportNullPtrError(DS)) {
      Expr *InitBase = Init->IgnoreParenImpCasts();
      QualType CurLHS = QT;
      QualType CurRHS = InitBase->getType();
      QualType OrigLHS = CurLHS;
      QualType OrigRHS = CurRHS;
      if (checkPathSensitiveFirstLevel(stripBorrow(InitBase), CurLHS, CurRHS,
              NonnullAssignedByNullable, VD->getLocation()))
        checkNestedPointerNullability(CurLHS, CurRHS, OrigLHS, OrigRHS,
                                      Ctx, Reporter, VD->getLocation());
    }

    return;
  }
  // check record/array initialization recursively
  Init = NormalizeInitExpr(Init->IgnoreParenImpCasts());
  if (auto *ILE = dyn_cast<InitListExpr>(Init)) {
    unsigned NumInits = ILE->getNumInits();
    if (CanQT->isRecordType()) {
      // check record initialization
      auto *RT = CanQT->getAs<RecordType>();
      RecordDecl *RD = RT->getDecl();
      if (!RD || RD->field_empty())
        return;
      // check empty initialized record which has non-null fields
      if (NumInits == 0) {
        if (RD->isUnion()) {
          FieldDecl *FD = ILE->getInitializedFieldInUnion();
          CheckInit(DS, VD, FD->getType(), nullptr, path);
        } else {
          CheckInit(DS, VD, QT, nullptr, path);
        }
        return;
      }
      // prepare fields to process
      std::vector<FieldDecl *> FieldsToProcess;
      if (RD->isUnion()) {
        FieldsToProcess.push_back(ILE->getInitializedFieldInUnion());
      } else if (RD->isStruct()) {
        for (FieldDecl *FD : RD->fields())
          FieldsToProcess.push_back(FD);
      }
      // check fields to process
      for (FieldDecl *FD : FieldsToProcess) {
        unsigned idx = RD->isStruct() ? FD->getFieldIndex() : 0;
        Expr *FieldInit = idx < NumInits ? ILE->getInit(idx) : nullptr;
        std::string newPath = path + "." + FD->getNameAsString();
        CheckInit(DS, VD, FD->getType(), FieldInit, newPath);
      }
    } else if (CanQT->isArrayType()) {
      // check array initialization
      auto *AT = QT->getAsArrayTypeUnsafe();
      QualType ElemTy = AT->getElementType();
      // check explicitly provided initializers
      for (unsigned i = 0; i < NumInits; ++i) {
        Expr *ElemInit = ILE->getInit(i);
        std::string newPath = path + "[" + std::to_string(i) + "]";
        CheckInit(DS, VD, ElemTy, ElemInit, newPath);
      }
      // check implicitly zero-initialized trailing elements
      if (auto *CAT = dyn_cast<ConstantArrayType>(AT)) {
        unsigned ArraySize = (unsigned)CAT->getSize().getZExtValue();
        if (NumInits < ArraySize) {
          std::string newPath = path + "[" + std::to_string(NumInits) + ":" + std::to_string(ArraySize) + "]";
          CheckInit(DS, VD, ElemTy, nullptr, newPath);
        }
      }
    }
  }
}

/// After `VD = RHS`, the dereference chain rooted at VD must reflect the
/// nullability of RHS's pointee levels rather than VD's declared type — the
/// runtime target of `*p`, `**p`, ... is whatever RHS pointed at. Each pointer
/// level contributes a ".*" segment ("*p" => ".*" at Depth+1 = 1).
void TransferFunctions::UpdateDerefStatusFromRHS(VarDecl *VD,
                                                  QualType RHSType) {
  unsigned Depth = 0;
  for (QualType QT = RHSType.getCanonicalType(); QT->isPointerType();
       ++Depth) {
    QT = QT->getPointeeType().getCanonicalType();
    // Seed the deref cell (VD, ".*" x (Depth+1)) with the pointee's def
    // nullability, so a later path that never refines the cell still finds
    // an entry at the CFG join.
    std::string Path;
    for (unsigned I = 0; I <= Depth; ++I)
      Path += ".*";
    CurrStatusFP[FieldPath(VD, Path)] = QT.getDefNullability();
  }
}

void TransferFunctions::VisitBinaryOperator(BinaryOperator *BO) {
  if (BO->isAssignmentOp()) {
    Expr *LHS = BO->getLHS();
    QualType LHSQT = LHS->getType();
    if (LHSQT.getCanonicalType()->isPointerType()) {
      NullabilityKind RHSKind = BO->isCompoundAssignmentOp()
                                    ? getExprPathNullability(LHS)
                                    : getExprPathNullability(BO->getRHS());
      NullabilityKind LHSKind = LHSQT.getDefNullability();
      std::string SourceName = getDiagNameFromExpr(BO->getRHS());

      if (auto FP = getFieldPath(LHS)) {
        // A member-access LHS reports against the member location; every other
        // spelling (bare variable, deref) reports against the whole assignment.
        Expr *LocE = getMemberExprFromExpr(LHS);
        SourceLocation DiagLoc = LocE ? LocE->getBeginLoc() : BO->getBeginLoc();

        if (LHSKind == NullabilityKind::NonNull) {
          // NonNull pointer cannot be assigned by expr whose PathNullability is
          // nullable.
          if (RHSKind == NullabilityKind::Nullable && ShouldReportNullPtrError(BO)) {
            NullabilityCheckDiagInfo DI(DiagLoc,
                                        NonnullAssignedByNullable,
                                        SourceName);
            Reporter.addDiagInfo(DI);
          }
        } else if (CurrStatusFP.count(*FP)) {
          // Here we update PathNullability of nullable pointer.
          CurrStatusFP[*FP] = RHSKind;
        }

        // Invalidate stale descendants based on the LHS spelling's shape. A
        // plain-variable rebind clears the whole subtree (and is reseeded from
        // the RHS pointee levels); a deref cell (path ending ".*") preserves the
        // overwritten cell and clears only deeper cells; a member access clears
        // nothing (its pointee is a separate allocation).
        if (VarDecl *VD = getVarDeclFromExpr(LHS)) {
          eraseDerefChainCells(CurrStatusFP, VD); // FP == {VD, ""}
          UpdateDerefStatusFromRHS(
              VD, BO->getRHS()->IgnoreParenImpCasts()->getType());
        } else if (!FP->second.empty() && FP->second.back() == '*') {
          eraseDeeperPaths(CurrStatusFP, *FP);
        }
      }

      // --- Inner-pointer check for assignments ---
      // Use IgnoreParenImpCasts so nested nullability is read from the
      // pre-cast RHS type. With qualifier-bit nullability, Sema inserts a
      // BitCast to the LHS type; BO->getRHS()->getType() would already look
      // like the LHS and hide the nested mismatch.
      if (ShouldReportNullPtrError(BO)) {
        Expr *RHSBase = BO->getRHS()->IgnoreParenImpCasts();
        QualType CurLHS = LHSQT;
        QualType CurRHS = RHSBase->getType();
        QualType OrigLHS = CurLHS;
        QualType OrigRHS = CurRHS;
        if (checkPathSensitiveFirstLevel(stripBorrow(RHSBase), CurLHS, CurRHS,
                NonnullAssignedByNullable, BO->getBeginLoc()))
          checkNestedPointerNullability(CurLHS, CurRHS, OrigLHS, OrigRHS,
                                        Ctx, Reporter, BO->getBeginLoc());
      }
    }
  }
}

// For __assume_null on a struct-typed argument: mark every reachable _Nullable
// pointer field as null in the field-path state. Recurses only into struct-typed
// (by-value) fields so nested _Nullable pointers (e.g. s.in.ip) are covered; a
// pointer field is a leaf — its pointee is a separate allocation that is
// unreachable once the field is null, so it is not descended into. \p prefix is
// the normalized path of the struct cell itself ("" for a bare struct variable,
// ".in" for a field, ".*" for a dereference), so a field f under it is
// "prefix.f".
static void assumeAllNullableFieldsNull(VarDecl *VD, const RecordDecl *RD,
                                        const std::string &prefix,
                                        StatusFP &FPMap) {
  if (!RD)
    return;
  for (const FieldDecl *FD : RD->fields()) {
    QualType FT = FD->getType();
    std::string path = prefix + "." + FD->getNameAsString();
    if (FT->isPointerType()) {
      if (FT.getDefNullability() == NullabilityKind::Nullable) {
        FieldPath FP = {VD, path};
        if (FPMap.count(FP))
          FPMap[FP] = NullabilityKind::Nullable;
      }
    } else if (const RecordType *RT = FT->getAs<RecordType>()) {
      assumeAllNullableFieldsNull(VD, RT->getDecl(), path, FPMap);
    }
  }
}

// NonNull parameter cannot take Nullable pointer as argument.
void TransferFunctions::VisitCallExpr(CallExpr *CE) {
  if (FunctionDecl *FD = CE->getDirectCallee()) {
    if (FD->getBuiltinID() == Builtin::BI__assume_null) {
      // __assume_null: assert the _Nullable pointer is null at this point, as if
      // `p = nullptr` had executed. Sema has already guaranteed the argument is
      // a trackable expression whose type is either a _Nullable pointer or a
      // struct (with no _Nonnull field reachable through embedded fields).
      Expr *Arg = CE->getArg(0)->IgnoreParenImpCasts();
      QualType ArgTy = CE->getArg(0)->getType();
      if (ArgTy->isPointerType()) {
        if (auto FP = getFieldPath(Arg)) {
          if (CurrStatusFP.count(*FP))
            CurrStatusFP[*FP] = NullabilityKind::Nullable;
        }
      } else if (ArgTy->isStructureType()) {
        // Mark every reachable _Nullable pointer field null. getFieldPath on
        // the struct cell yields its normalized prefix ("" / ".in" / ".*").
        const RecordType *RT = ArgTy->getAs<RecordType>();
        if (auto FP = getFieldPath(Arg))
          assumeAllNullableFieldsNull(FP->first, RT->getDecl(), FP->second,
                                      CurrStatusFP);
      }
      return;
    }
    for (unsigned i = 0; i < FD->getNumParams(); i++) {
      ParmVarDecl *PVD = FD->getParamDecl(i);
      if (PVD->getType().getDefNullability() == NullabilityKind::NonNull) {
        Expr *ArgE = CE->getArg(i);
        if (getExprPathNullability(ArgE) == NullabilityKind::Nullable && ShouldReportNullPtrError(CE)) {
          NullabilityCheckDiagInfo DI(ArgE->getBeginLoc(), PassNullableArgument,
                                      getDiagNameFromExpr(ArgE));
          Reporter.addDiagInfo(DI);
        }
      }

      Expr *ArgBase = CE->getArg(i)->IgnoreParenImpCasts();
      QualType CurParam = PVD->getType();
      QualType CurArg = ArgBase->getType();
      QualType OrigParam = CurParam;
      QualType OrigArg = CurArg;
      if (ShouldReportNullPtrError(CE) &&
          checkPathSensitiveFirstLevel(stripBorrow(ArgBase), CurParam, CurArg,
              PassNullableArgument, CE->getArg(i)->getBeginLoc()))
        checkNestedPointerNullability(CurParam, CurArg, OrigParam, OrigArg,
                                      Ctx, Reporter,
                                      CE->getArg(i)->getBeginLoc());
    }
  }
}

// Returns true when \p S is the direct operand of a borrow address-of
// operator (&_Mut / &_Const), possibly through parentheses. Only a constant
// zero subscript (&_Mut p[0] and parenthesized variants) borrows the element
// without actually dereferencing the nullable base pointer, so the
// nullable-dereference check below must not fire for it. Non-zero or
// non-constant indices (&_Mut p[i]) require a non-null base pointer and are
// still reported. (&_Mut *p / &_Const *p are normalized into
// UO_AddrMutDeref / UO_AddrConstDeref by Sema and therefore never reach the
// UO_Deref check here.)
bool TransferFunctions::IsDirectBorrowAddrOperand(Stmt *S) {
  Stmt *Cur = S;
  while (Cur) {
    Stmt *Parent = PM.getParent(Cur);
    if (!Parent)
      return false;
    if (auto *PE = dyn_cast<ParenExpr>(Parent)) {
      Cur = PE;
      continue;
    }
    // _Safe(...) / _Unsafe(...) are transparent wrappers around the subscript,
    // so &_Mut _Safe(p[0]) / &_Mut _Unsafe(p[0]) are exempt exactly like
    // &_Mut (p[0]).
    if (auto *SE = dyn_cast<SafeExpr>(Parent)) {
      Cur = SE;
      continue;
    }
    if (auto *UO = dyn_cast<UnaryOperator>(Parent)) {
      UnaryOperator::Opcode Op = UO->getOpcode();
      if (Op != UO_AddrMut && Op != UO_AddrConst)
        return false;
      auto *ASE = dyn_cast<ArraySubscriptExpr>(S);
      if (!ASE)
        return false;
      Expr::EvalResult ER;
      return ASE->getIdx()->EvaluateAsInt(ER, Ctx) && ER.Val.isInt() &&
             ER.Val.getInt().isZero();
    }
    return false;
  }
  return false;
}

// *p is not allowed when p has nullable PathNullability.
// &mut *p and &const *p are allowed because no actual dereferencing happens.
// These expressions only change the pointer type without accessing the memory,
// so they do not violate any nullability guarantees.
void TransferFunctions::VisitUnaryOperator(UnaryOperator *UO) {
  UnaryOperator::Opcode Op = UO->getOpcode();
  if (Op == UO_Deref) {
    if (getExprPathNullability(UO->getSubExpr()) == NullabilityKind::Nullable && ShouldReportNullPtrError(UO)) {
      NullabilityCheckDiagInfo DI(UO->getBeginLoc(),
                                  NullablePointerDereference,
                                  getDiagNameFromExpr(UO->getSubExpr()));
      Reporter.addDiagInfo(DI);
    }
  }
}

// p[i] is not allowed when p has nullable PathNullability.
// Array subscript is equivalent to *(p + i), so it dereferences the pointer.
// &_Mut p[i] / &_Const p[i] borrow the element without dereferencing p, so
// they are exempt.
void TransferFunctions::VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
  if (!IsDirectBorrowAddrOperand(ASE) &&
      getExprPathNullability(ASE->getBase()) == NullabilityKind::Nullable &&
      ShouldReportNullPtrError(ASE)) {
    NullabilityCheckDiagInfo DI(ASE->getBeginLoc(),
                                NullablePointerDereference,
                                getDiagNameFromExpr(ASE->getBase()));
    Reporter.addDiagInfo(DI);
  }
}

// p->a is not allowed when p has nullable PathNullability.
void TransferFunctions::VisitMemberExpr(MemberExpr *ME) {
  if (ME->isArrow()) {
    if (getExprPathNullability(ME->getBase()) == NullabilityKind::Nullable && ShouldReportNullPtrError(ME)) {
      Expr *Base = ME->getBase()->IgnoreParenImpCastsSafe();
      NullabilityCheckDiagInfo DI(Base->getExprLoc(),
                                  NullablePointerAccessMember,
                                  getDiagNameFromExpr(ME->getBase()));
      Reporter.addDiagInfo(DI);
    }
  }
}

// (int *_Nonnull)p, (int *borrow)p, (int *owned)p is not allowed
// when p has nullable PathNullability.
void TransferFunctions::VisitCStyleCastExpr(CStyleCastExpr *CSCE) {
  if (CSCE->getTypeAsWritten().getDefNullability() == NullabilityKind::NonNull) {
    if (getExprPathNullability(CSCE->getSubExpr()->IgnoreParenImpCasts()) ==
            NullabilityKind::Nullable &&
        ShouldReportNullPtrError(CSCE)) {
      Expr *Source = CSCE->getSubExpr()->IgnoreParenImpCasts();
      NullabilityCheckDiagInfo DI(CSCE->getBeginLoc(), NullableCastNonnull,
                                  getDiagNameFromExpr(Source));
      Reporter.addDiagInfo(DI);
    }
  }
}

// If function return type is NonNull, cannot return pointer
// which has nullable PathNullability.
void TransferFunctions::VisitReturnStmt(ReturnStmt *RS) {
  Expr *RV = RS->getRetValue();
  if (!RV)
    return;
  if (Fd.getReturnType().getDefNullability() == NullabilityKind::NonNull) {
    if (getExprPathNullability(RV) == NullabilityKind::Nullable && ShouldReportNullPtrError(RS)) {
      NullabilityCheckDiagInfo DI(RV->getBeginLoc(), ReturnNullable,
                                  getDiagNameFromExpr(RV));
      Reporter.addDiagInfo(DI);
    }
  }

  // --- Inner-pointer check for return expressions ---
  // IgnoreParenImpCasts so nested nullability is from the pre-cast return value.
  if (ShouldReportNullPtrError(RS)) {
    Expr *RVBase = RV->IgnoreParenImpCasts();
    QualType CurLHS = Fd.getReturnType();
    QualType CurRHS = RVBase->getType();
    QualType OrigLHS = CurLHS, OrigRHS = CurRHS;
    if (checkPathSensitiveFirstLevel(stripBorrow(RVBase), CurLHS, CurRHS,
                                     ReturnNullable, RV->getBeginLoc()))
      checkNestedPointerNullability(CurLHS, CurRHS, OrigLHS, OrigRHS,
                                    Ctx, Reporter, RV->getBeginLoc());
  }
}

// This function handles CFGBlocks setting by expression
void TransferFunctions::SetCFGBlocksByExpr(Expr *PtrE,
                                           const CFGBlock *NonNullBlock,
                                           const CFGBlock *NullableBlock) {
  if (auto FP = getFieldPath(PtrE)) {
    if (PtrE->getType().getDefNullability() == NullabilityKind::Nullable &&
        CurrStatusFP.count(*FP) &&
        CurrStatusFP[*FP] != NullabilityKind::NonNull) {
      NCI.BlocksConditionStatusFP[NonNullBlock][Block][*FP] =
          NullabilityKind::NonNull;
      NCI.BlocksConditionStatusFP[NullableBlock][Block][*FP] =
          NullabilityKind::Nullable;
    }
  }
}

// This function handles condition expression and
// pass the conditions to successor block.
void TransferFunctions::PassConditionStatusToSuccBlocks(Expr *CondExpr) {
  if (!CondExpr)
    return;
  CondExpr = CondExpr->IgnoreParenImpCasts();

  // When building CFG, if a block has a condition as terminitor,
  // the successors has certain order, for example:
  //    B4(has condition as terminitor)
  //   /  \
  //  B3  B2
  // The first successor of B4 must be true branch,
  // and the second successor of B4 must be false branch.
  // Here we only handle terminators with two successors.

  // Because the caller has guaranteed that the block has two successors,
  // we can directly get these two successor blocks without
  // checking results of the iterator step by step
  CFGBlock::const_succ_iterator it = Block->succ_begin();
  const CFGBlock *TrueBlock = *it++;
  const CFGBlock *FalseBlock = *it;
  if (!TrueBlock || !FalseBlock)
    return;

  NullCheckInfo Info (CondExpr, Ctx);
  for(const auto* E: Info.presentCheckedExprs){
    SetCFGBlocksByExpr(const_cast<Expr*>(E), TrueBlock, FalseBlock);
  }
  for (const auto* E: Info.nullCheckedExprs){
    SetCFGBlocksByExpr(const_cast<Expr*>(E), FalseBlock, TrueBlock);
  }
}

/// Recursively walk \p S while initStatus pre-scans the CFG. The caller passes
/// the entry-state map, so cells discovered in any ordinary block are seeded at
/// the function entry rather than in the block being scanned. Variables, field
/// paths, and dereference cells use their declared Nullable default. A
/// deref cell is added only when its result has Nullable DefNullability. The
/// explicit Nullable entry prevents a predecessor that never narrows the path
/// from contributing a missing map entry at a CFG join; initialization,
/// assignments, and branch conditions may subsequently refine or replace the
/// seeded state.
static void collectNullableDecls(Stmt *S, StatusFP &FPMap) {
  if (!S)
    return;
  if (auto *DRE = dyn_cast<DeclRefExpr>(S)) {
    if (VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl()))
      if (VD->getType().getDefNullability() == NullabilityKind::Nullable)
        FPMap[FieldPath(VD, "")] = NullabilityKind::Nullable;
  } else if (auto *ME = dyn_cast<MemberExpr>(S)) {
    if (auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      if (FD->getType().getDefNullability() == NullabilityKind::Nullable) {
        if (auto FP = getFieldPath(ME))
          FPMap[*FP] = NullabilityKind::Nullable;
      }
    }
  } else if (auto *UO = dyn_cast<UnaryOperator>(S)) {
    if (UO->getOpcode() == UO_Deref &&
        UO->getType().getDefNullability() == NullabilityKind::Nullable) {
      if (auto FP = getFieldPath(UO))
        FPMap[*FP] = NullabilityKind::Nullable;
    }
  }
  for (Stmt *Child : S->children())
    collectNullableDecls(Child, FPMap);
}

// Traverse the CFG once and seed entry PathNullability for every trackable
// declaration, field path, and dereference path with Nullable DefNullability.
void NullabilityCheckImpl::initStatus(const CFG &cfg) {
  const CFGBlock *entry = &cfg.getEntry();
  for (const CFGBlock *B : cfg.const_nodes()) {
    if (B != entry && B != &cfg.getExit() && !B->succ_empty() &&
        !B->pred_empty()) {
      for (CFGBlock::const_iterator it = B->begin(), ei = B->end(); it != ei;
           ++it) {
        const CFGElement &elem = *it;
        if (elem.getAs<CFGStmt>()) {
          Stmt *S = const_cast<Stmt *>(elem.castAs<CFGStmt>().getStmt());
          collectNullableDecls(S, BlocksEndStatusFP[entry]);
        }
      }
    }
  }
}

StatusFP NullabilityCheckImpl::mergeFP(StatusFP statusA, StatusFP statusB) {
  if (statusA.empty())
    return statusB;
  for (auto NullabilityOfFP : statusB) {
    FieldPath FP = NullabilityOfFP.first;
    NullabilityKind NK = NullabilityOfFP.second;
    if (statusA.count(FP)) {
      statusA[FP] = NK == NullabilityKind::Nullable ? NullabilityKind::Nullable
                                                    : statusA[FP];
    } else {
      statusA[FP] = NK;
    }
  }
  return statusA;
}

StatusFP NullabilityCheckImpl::runOnBlock(
    const CFGBlock *block, StatusFP statusFP, NullabilityCheckDiagReporter &reporter,
    ASTContext &ctx, const FunctionDecl &fd, ParentMap &PM) {
  TransferFunctions TF(*this, block, statusFP, reporter, ctx, fd, PM);

  for (CFGBlock::const_iterator it = block->begin(), ei = block->end();
       it != ei; ++it) {
    const CFGElement &elem = *it;
    if (elem.getAs<CFGStmt>()) {
      const Stmt *S = elem.castAs<CFGStmt>().getStmt();
      TF.Visit(const_cast<Stmt *>(S));
    }
  }

  // Here we will handle the condition in IfStmt, or other branch stmts
  // which will change the nullability of a field path.
  // Limit block's successor to 2 to ensure compatibility of existing
  // implementation of PassConditionStatusToSuccBlocks, which assumes the first
  // successor is true branch and the second successor is false branch.
  if (block->succ_size() == 2) {
    // Use block-local last condition instead of terminator condition to be
    // consistent for conditions already split in CFG (&& ||).
    Expr *CondExpr = const_cast<Expr *>(block->getLastCondition());
    TF.PassConditionStatusToSuccBlocks(CondExpr);
  }
  return statusFP;
}

void clang::runNullabilityCheck(const FunctionDecl &fd, const CFG &cfg,
                                AnalysisDeclContext &ac,
                                NullabilityCheckDiagReporter &reporter,
                                ASTContext &ctx) {
  // The analysis currently has scalability issues for very large CFGs.
  // Bail out if it looks too large.
  if (cfg.getNumBlockIDs() > 300000)
    return;

  NullabilityCheckImpl NCI;
  NCI.initStatus(cfg);

  // Proceed with the worklist.
  ForwardDataflowWorklist worklist(cfg, ac);
  const CFGBlock *entry = &cfg.getEntry();
  for (const CFGBlock *B : cfg.const_reverse_nodes())
    if (B != entry && !B->pred_empty())
      worklist.enqueueBlock(B);

  while (const CFGBlock *block = worklist.dequeue()) {
    StatusFP &preValFP = NCI.BlocksBeginStatusFP[block];
    StatusFP valFP;
    for (CFGBlock::const_pred_iterator it = block->pred_begin(),
                                       ei = block->pred_end();
         it != ei; ++it) {
      if (const CFGBlock *pred = *it) {
        StatusFP predValFP = NCI.BlocksEndStatusFP[pred];
        if (NCI.BlocksConditionStatusFP.count(block)) {
          if (NCI.BlocksConditionStatusFP[block].count(pred)) {
            for (auto &CondState : NCI.BlocksConditionStatusFP[block][pred]) {
              predValFP[CondState.first] = CondState.second;
            }
          }
        }
        valFP = NCI.mergeFP(valFP, predValFP);
      }
    }

    StatusFP val = NCI.runOnBlock(block, valFP, reporter, ctx, fd,
                                  ac.getParentMap());
    NCI.BlocksEndStatusFP[block] = val;
    if (preValFP == val)
      continue;

    preValFP = val;

    // Enqueue the value to the successors.
    worklist.enqueueSuccessors(block);
  }
}
#endif // ENABLE_BSC
