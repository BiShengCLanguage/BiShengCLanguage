//===- BSCOwnership.cpp - Ownership Analysis for Source CFGs -*- BSC --*------//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements BSC Ownership analysis for source-level CFGs.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/Analysis/Analyses/BSC/BSCOwnership.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Analysis/Analyses/BSC/BSCNullCheckInfo.h"
#include "clang/Analysis/Analyses/BSC/BSCPlace.h"
#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/Builtins.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include <algorithm>
#include <queue>
#include <utility>

using namespace clang;
using namespace std;

namespace {
class OwnershipImpl {
public:
  AnalysisDeclContext &analysisContext;
  ASTContext &ctx;
  OwnedArrayLoopInfo LoopInfo;
  llvm::DenseMap<const CFGBlock *, Ownership::OwnershipStatus>
      blocksBeginStatus;
  llvm::DenseMap<const CFGBlock *, Ownership::OwnershipStatus> blocksEndStatus;

  // Used to force left-to-right evaluation order on arguments.
  // See CollectArgumentCalls() comments for details.
  llvm::DenseSet<const Stmt *> ArgCalls;

  Ownership::OwnershipStatus merge(Ownership::OwnershipStatus statsA,
                                   Ownership::OwnershipStatus statsB);

  Ownership::OwnershipStatus runOnBlock(const CFGBlock *block,
                                        Ownership::OwnershipStatus status,
                                        OwnershipDiagReporter &reporter,
                                        bool isDestructor);

  void MaybeSetNull(const CFGBlock *block, const CFGBlock *cur, Ownership::OwnershipStatus &status);

  OwnershipImpl(AnalysisDeclContext &ac, ASTContext &context,
                OwnedArrayLoopInfo loopInfo)
      : analysisContext(ac), ctx(context), LoopInfo(std::move(loopInfo)),
        blocksBeginStatus(0), blocksEndStatus(0) {}
};
} // namespace

//===----------------------------------------------------------------------===//
// Common static functions.
//===----------------------------------------------------------------------===//

// Collect every CallExpr that appears as a (possibly parenthesized/cast)
// argument of an enclosing CallExpr. The CFG built with setAllAlwaysAdd()
// emits such nested calls as standalone CFGStmt elements that precede the
// enclosing call statement; if the ownership analysis followed that order,
// `f(*p, g(p))` would move `p` (via `g(p)`) before `*p` is checked, which
// is a false "use of moved value" report. These calls are instead analyzed
// inline, left-to-right, by the enclosing call, and their hoisted CFGStmt
// elements are skipped in runOnBlock().
// (IsTrackedType lives in BSCOwnership.h as a static inline helper; its
// array-aware version is shared by the array-element feature.)
static void CollectArgumentCalls(Stmt *S, bool InCallArg,
                                 llvm::DenseSet<const Stmt *> &ArgCalls) {
  if (!S)
    return;
  if (CallExpr *CE = dyn_cast<CallExpr>(S)) {
    if (InCallArg)
      ArgCalls.insert(CE);
    for (Expr *Arg : CE->arguments())
      CollectArgumentCalls(Arg, true, ArgCalls);
    // The callee is not an argument: `f()(p)` still analyzes `f()` as its
    // own top-level call.
    CollectArgumentCalls(CE->getCallee(), false, ArgCalls);
    return;
  }
  for (Stmt *Child : S->children())
    CollectArgumentCalls(Child, InCallArg, ArgCalls);
}

// Whether a CFGStmt element is a discarded expression statement (e.g. `*p;`)
// rather than a sub-expression hoisted by the setAllAlwaysAdd() CFG build —
// hoisted ones are analyzed inline by their enclosing statement instead.
static bool IsDiscardedExprStmt(const Stmt *S, ParentMap &PM) {
  const auto *E = dyn_cast<Expr>(S);
  return E && !PM.isConsumedExpr(E);
}

// Element kinds that run their transfer here, as their own CFGStmt element;
// every other element is a hoisted sub-expression re-visited inline by its
// enclosing statement (or a discarded expression, visited read-only).
static bool HasTransferSemantics(const Stmt *S) {
  if (isa<DeclStmt, CallExpr, ReturnStmt>(S))
    return true;
  if (const auto *BO = dyn_cast<BinaryOperator>(S))
    return BO->isAssignmentOp();
  if (const auto *UO = dyn_cast<UnaryOperator>(S))
    return UO->isIncrementDecrementOp();
  return false;
}

// Whether Terminator identifies a block that evaluates part of a statement
// condition. Short-circuit operators split their operands across blocks, so
// intermediate blocks have a logical BinaryOperator terminator rather than
// the owning control statement.
static bool IsStatementConditionTerminator(const Stmt *Terminator,
                                           ParentMap &PM) {
  if (!Terminator)
    return false;

  if (isa<IfStmt, WhileStmt, DoStmt, ForStmt, SwitchStmt>(Terminator))
    return true;

  const auto *BO = dyn_cast<BinaryOperator>(Terminator);
  if (!BO || !BO->isLogicalOp())
    return false;

  const Stmt *S = BO;
  while (const Stmt *Parent = PM.getParent(S)) {
    if (const auto *IS = dyn_cast<IfStmt>(Parent))
      return IS->getCond() == S;
    if (const auto *WS = dyn_cast<WhileStmt>(Parent))
      return WS->getCond() == S;
    if (const auto *DS = dyn_cast<DoStmt>(Parent))
      return DS->getCond() == S;
    if (const auto *FS = dyn_cast<ForStmt>(Parent))
      return FS->getCond() == S;
    if (const auto *SS = dyn_cast<SwitchStmt>(Parent))
      return SS->getCond() == S;

    const auto *ParentExpr = dyn_cast<Expr>(Parent);
    if (!ParentExpr)
      return false;
    if (const auto *ParentBO = dyn_cast<BinaryOperator>(ParentExpr)) {
      if (!ParentBO->isLogicalOp())
        return false;
    } else if (!isa<ParenExpr, ImplicitCastExpr>(ParentExpr)) {
      return false;
    }
    S = Parent;
  }
  return false;
}

// Field-path state is keyed by bsc::Place: a root variable followed by
// field / dereference / index projections. The old string keys map as:
//   ""            -> the Var place of the root variable
//   "f" / "f.g"   -> Field(Field(Var, "f"), "g")
//   "arr[]"       -> Index(place, "[]") (whole-array aggregate, index-free)
//   "f*"          -> Deref(Field(Var, "f")) (nested owned pointer of f)
//   "pp**"        -> Deref(Deref(Field(Var, "pp"))) (deref use markers)
using FieldKey = const bsc::Place *;
using FieldSet = Ownership::OwnershipStatus::FieldSet;

// The old key spelling of a place, used for diagnostics and for the grown-mode
// iteration order of FieldSet. This reproduces the historical concatenations:
//   Var   -> ""                  (parentFieldName starts empty)
//   Field -> "base.f"            (parentFieldName + name, "." between levels)
//   Index -> "base[]"            (the array suffix appended per array level)
//   Deref -> "base*"             (the "*" use/registration marker)
static std::string placeToKeyString(FieldKey P) {
  if (!P)
    return "";
  switch (P->getKind()) {
  case bsc::Place::Kind::Var:
    return "";
  case bsc::Place::Kind::Field: {
    std::string Base = placeToKeyString(P->getBase());
    if (Base.empty())
      return P->getName().str();
    return Base + "." + P->getName().str();
  }
  case bsc::Place::Kind::Index:
    return placeToKeyString(P->getBase()) + "[]";
  case bsc::Place::Kind::Deref:
    return placeToKeyString(P->getBase()) + "*";
  }
  llvm_unreachable("unknown place kind");
}

bool Ownership::OwnershipStatus::FieldSet::insert(const bsc::Place *P) {
  if (Set.count(P))
    return false;
  // The 11th distinct element flips the set into grown (sorted) mode
  // permanently, like the SmallSet vector->std::set overflow.
  if (Order.size() >= 10)
    Grown = true;
  Order.push_back(P);
  Set.insert(P);
  CacheValid = false;
  return true;
}

bool Ownership::OwnershipStatus::FieldSet::erase(const bsc::Place *P) {
  auto It = Set.find(P);
  if (It == Set.end())
    return false;
  // Erase the stored (canonical) pointer from Order too: a later insert of
  // a structurally-equal place is a no-op, so Order may hold a different
  // pointer than the one passed here.
  const bsc::Place *Stored = *It;
  Set.erase(It);
  for (auto I = Order.begin(), E = Order.end(); I != E; ++I)
    if (*I == Stored) {
      Order.erase(I);
      break;
    }
  CacheValid = false;
  return true;
}

void Ownership::OwnershipStatus::FieldSet::ensureSorted() const {
  if (CacheValid)
    return;
  SortedCache.assign(Order.begin(), Order.end());
  if (Grown)
    llvm::sort(SortedCache, [](FieldKey A, FieldKey B) {
      return placeToKeyString(A) < placeToKeyString(B);
    });
  CacheValid = true;
}

Ownership::OwnershipStatus::FieldSet::const_iterator
Ownership::OwnershipStatus::FieldSet::begin() const {
  ensureSorted();
  return SortedCache.begin();
}

Ownership::OwnershipStatus::FieldSet::const_iterator
Ownership::OwnershipStatus::FieldSet::end() const {
  ensureSorted();
  return SortedCache.end();
}

bool Ownership::OwnershipStatus::FieldSet::operator==(
    const FieldSet &RHS) const {
  if (Set.size() != RHS.Set.size())
    return false;
  for (FieldKey P : Set)
    if (!RHS.Set.count(P))
      return false;
  return true;
}

// Whether Prefix names the same field as Path or a field enclosing it. The
// old string version accepted a following '.' or '*' as the boundary but NOT
// '[': "a.b" is a prefix of "a.b", "a.b.c" and "a.b*", but neither "a.bc"
// nor "a" of "a[]". Structurally: Prefix occurs in Path's chain and the
// projection first applied to it (the child on the walk from Path down to
// Prefix) is a Field or Deref, not an Index.
static bool isFieldPathPrefix(FieldKey Prefix, FieldKey Path) {
  const bsc::Place *Child = nullptr;
  for (const bsc::Place *Cur = Path; Cur; Child = Cur, Cur = Cur->getBase()) {
    if (Cur->equals(Prefix))
      return Child == nullptr || Child->getKind() != bsc::Place::Kind::Index;
  }
  return false;
}

// The strict descendants of Prefix in S reached through a Field projection
// directly above it. This is the old `findPrefixStrings(S, prefix + ".")`:
// string "in.ip" matched under "in" (and "in.ip*" below that), but the deref
// key "in*" did not ("in.*" was never a registered key spelling). A Var-kind
// prefix never matches: the old "."-prefixed lookup found no keys (none start
// with ".").
static FieldSet fieldDescendants(const FieldSet &S, FieldKey Prefix) {
  FieldSet Desc;
  if (!Prefix || Prefix->getKind() == bsc::Place::Kind::Var)
    return Desc;
  for (FieldKey P : S) {
    for (const bsc::Place *Cur = P; Cur; Cur = Cur->getBase()) {
      if (Cur->getBase() && Cur->getBase()->equals(Prefix) &&
          Cur->getKind() == bsc::Place::Kind::Field) {
        Desc.insert(P);
        break;
      }
    }
  }
  return Desc;
}

// The strict descendants of Prefix in S reached through a Deref projection
// directly above it: places under Deref(Prefix), including Deref(Prefix)
// itself. This is the old `findPrefixStrings(S, prefix + "*")` (the
// registered nested-owned-pointer keys like "f*" under "f", and use markers
// like "f**" below them). A Var-kind prefix never matches: the old
// "*"-prefixed lookup found no keys at the root level ("" was the root key,
// never "*" or "**" with an empty base).
static FieldSet derefDescendants(const FieldSet &S, FieldKey Prefix) {
  FieldSet Desc;
  if (!Prefix || Prefix->getKind() == bsc::Place::Kind::Var)
    return Desc;
  for (FieldKey P : S) {
    for (const bsc::Place *Cur = P; Cur; Cur = Cur->getBase()) {
      if (Cur->getBase() && Cur->getBase()->equals(Prefix) &&
          Cur->getKind() == bsc::Place::Kind::Deref) {
        Desc.insert(P);
        break;
      }
    }
  }
  return Desc;
}

// The strict descendants of P in S under any projection, including through
// Index levels. This is the old bare `findPrefixStrings(S, key)` minus the
// key itself: a plain string prefix match (so "arr" also picks up "arr[]"
// and "arr[].q"), used by the whole-array aggregate queries.
static FieldSet strictDescendants(const FieldSet &S, FieldKey P) {
  FieldSet Desc;
  for (FieldKey Q : S)
    if (Q != P && Q->hasPrefix(P))
      Desc.insert(Q);
  return Desc;
}

// Whether the access path touches any field tracked in the set: the path is
// a tracked field itself, lies inside one ("inner.a" under "inner"), or
// covers one ("a1" over "a1.b1").
static bool overlapsOwnedFields(const FieldSet &OwnedFields, FieldKey Path) {
  for (FieldKey Owned : OwnedFields) {
    if (isFieldPathPrefix(Owned, Path) || isFieldPathPrefix(Path, Owned))
      return true;
  }
  return false;
}

// Erase the field itself and every descendant (any projection below it,
// Index levels included) from a moved-set — the re-initialization path of
// the explicit moved-field tracking.
static void eraseFieldAndDescendants(FieldSet &fields, FieldKey fieldName) {
  llvm::SmallVector<FieldKey, 10> toErase;
  for (FieldKey field : fields)
    if (field->hasPrefix(fieldName))
      toErase.push_back(field);
  for (FieldKey field : toErase)
    fields.erase(field);
}

// Record the field itself and every descendant in the moved-set (only
// fields that are tracked at all).
static void
markFieldAndDescendantsMoved(const FieldSet &allFields,
                             FieldSet &movedFields, FieldKey fieldName) {
  for (FieldKey field : allFields)
    if (field->hasPrefix(fieldName))
      movedFields.insert(field);
}

// The outermost key of the deref chain above P that is recorded in the
// explicit moved-set: like findMovedFieldKey, but over the moved-set alone
// (a field moved on some branch, regardless of may-own state).
static FieldKey
findExplicitMovedFieldKey(const FieldSet &movedFields, FieldKey P) {
  FieldKey movedFieldName = nullptr;
  const bsc::Place *Cur = P;
  while (Cur) {
    if (movedFields.count(Cur))
      movedFieldName = Cur;
    if (Cur->getKind() != bsc::Place::Kind::Deref)
      break;
    Cur = Cur->getBase();
  }
  return movedFieldName;
}

// The old key spelling of a field-path access. The aggregate form (the
// historical trailing '.' from the '*'→'.' rewrite of a deref key) never
// matched a registered spelling — registered keys never end in '.' — so every
// direct key lookup under it saw "not present"; only its descendant prefix
// query matched real keys.
static std::string accessKeySpelling(FieldKey P, bool IsAggregateAccess) {
  return placeToKeyString(P) + (IsAggregateAccess ? "." : "");
}

// Deref uses name a deref-descendant cell of a field, e.g. **q.pp is
// Deref(Deref(Field(q, "pp"))). The ownership sets may also contain real
// deref keys for nested owned fields, such as Deref(Field(q, "pp")) for the
// inner owned pointer of pp, so we cannot strip all derefs to the bare
// field. Walk back through the deref chain and return the outermost moved
// key that is actually tracked, keeping diagnostics focused on the field
// that was moved out. The aggregate form's key ends in '.', so the
// historical loop broke on its first iteration without matching anything.
static FieldKey findMovedFieldKey(const FieldSet &AllFields,
                                  const FieldSet &OwnedFields,
                                  const FieldSet *NullFields, FieldKey P,
                                  bool IsAggregateAccess = false) {
  FieldKey Moved = nullptr;
  if (IsAggregateAccess)
    return Moved;
  // Skip the pure deref markers first: a use place Deref(Deref(Field(pp)))
  // must be tried as itself, then as Deref(Field(pp)), then as Field(pp).
  // hasPrefix stops at the field node, so walk the chain manually.
  const bsc::Place *Cur = P;
  while (Cur) {
    if (AllFields.count(Cur) && !OwnedFields.count(Cur) &&
        (!NullFields || !NullFields->count(Cur)))
      Moved = Cur;
    if (Cur->getKind() != bsc::Place::Kind::Deref)
      break;
    Cur = Cur->getBase();
  }
  return Moved;
}

// The condition-1 parent check of the field use/assign paths: find an
// enclosing field that was moved out (in All but not in Owned). The
// historical string code truncated the use key from the tail, one segment
// per step, but its index scan never reached the segment under a trailing
// '*' or '.' marker, so a write through a moved owned field (`*s.p = v`)
// passed silently; walking the place's ancestor chain directly reaches
// every enclosing projection, the pointer under a deref included. The
// aggregate form (a write `(*p).f = v` to the aggregate under a moved
// pointer field) starts one level deeper — the accessed field itself is a
// candidate too — matching the historical first truncation step, which
// peeled the trailing '.' off the aggregate key spelling. The root
// variable is not a field path and never a candidate.
// Returns the first ancestor found in All but not in Owned, or nullptr.
// `Spelling` receives the matched old key spelling.
static FieldKey findMovedEnclosingField(const FieldSet &AllFields,
                                        const FieldSet &OwnedFields,
                                        FieldKey Use, std::string &Spelling,
                                        bool IsAggregateAccess = false) {
  const bsc::Place *Cur =
      IsAggregateAccess ? Use : Use->getBase();
  for (; Cur && Cur->getKind() != bsc::Place::Kind::Var; Cur = Cur->getBase()) {
    if (AllFields.count(Cur) && !OwnedFields.count(Cur)) {
      Spelling = placeToKeyString(Cur);
      return Cur;
    }
  }
  return nullptr;
}

static unsigned getIndex(Ownership::Status S) {
  int value = static_cast<int>(S);
  unsigned bitIndex = 0;
  while ((value & 1) == 0) {
    value = value >> 1;
    bitIndex++;
  }
  return bitIndex;
}

// Build the root-relative field place of a member expression: the Field-chain
// place rooted at the member chain's base variable. `s.f.g` and `s->f.g` /
// `(*s).f.g` both resolve to Field(Field(Var(s), f), g) — arrow / leading
// derefs collapse, mirroring the historical getMemberFullField +
// getRootDREFromMemberBase pair. Returns nullptr when the base is not a
// plain variable (e.g. an array element or a call).
// The type recorded for one subscript/deref level. Deref places compare by
// base only (BSCPlace.h), so the type never takes part in key equality; it
// just has to be non-null for Ty::Create. A subscript or decayed deref on an
// array reaches the element type, not a pointee, so fall back to the element
// type when there is no pointee.
static QualType pointeeOrElem(QualType QT) {
  if (QT.isNull())
    return QT;
  if (QT->isPointerType())
    return QT->getPointeeType();
  if (QT->isArrayType())
    return QT->getAsArrayTypeUnsafe()->getElementType();
  return QT;
}

static FieldKey getMemberFieldPlace(const MemberExpr *ME, ASTContext &Ctx) {
  bsc::PlaceBuilder PB(Ctx);
  // Collect the member chain outermost-first (ME is the outermost).
  llvm::SmallVector<const FieldDecl *, 4> Fields;
  const Expr *Base = ME;
  while (const MemberExpr *Cur = dyn_cast<MemberExpr>(Base)) {
    if (const FieldDecl *FD = dyn_cast<FieldDecl>(Cur->getMemberDecl()))
      Fields.push_back(FD);
    else
      return nullptr;
    Base = Cur->getBase()->IgnoreParenImpCasts();
  }
  // Collapse a leading deref (`s->f`, `(*s).f`) to the root variable.
  if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(Base)) {
    if (UO->getOpcode() == UO_Deref)
      Base = UO->getSubExpr()->IgnoreParenImpCasts();
  }
  const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Base);
  if (!DRE)
    return nullptr;
  const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
  if (!VD)
    return nullptr;
  FieldKey Place = PB.Build(VD, VD->getLocation());
  for (const FieldDecl *FD : llvm::reverse(Fields))
    Place = PB.BuildField(Place, FD, FD->getLocation());
  return Place;
}

// Peel an array-element expression a[i] / a[i][j] / s[i].f / s[i].a[j] down
// to the base VarDecl (of an array or of a _ArrayElem pointer); nullptr if the
// expression is not such an element access. Shared by the dataflow transfer
// functions and the null-check handling.
static const VarDecl *PeelArrayElemBase(const Expr *E) {
  if (!E)
    return nullptr;
  const Expr *Base = E->IgnoreParenImpCasts();
  if (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(Base)) {
    Base = ASE->getBase()->IgnoreParenImpCasts();
    while (const ArraySubscriptExpr *Inner =
               dyn_cast<ArraySubscriptExpr>(Base))
      Base = Inner->getBase()->IgnoreParenImpCasts();
  } else if (const MemberExpr *ME = dyn_cast<MemberExpr>(Base)) {
    Base = ME->getBase()->IgnoreParenImpCasts();
    while (const ArraySubscriptExpr *A = dyn_cast<ArraySubscriptExpr>(Base))
      Base = A->getBase()->IgnoreParenImpCasts();
  } else {
    return nullptr;
  }
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Base))
    return dyn_cast<VarDecl>(DRE->getDecl());
  return nullptr;
}


// A transfer site whose base is an owned-element array or a _Owned
// _ArrayElem pointer: element ownership transfer applies.
static bool IsArrayTransferBase(const VarDecl *VD, bool IsArrElemPtr) {
  return IsOwnedElementArrayType(VD->getType()) || IsArrElemPtr;
}

// Peel an owned-element *array field* access (w.arr[i] / o.w.arr[i]) to the
// host and the "arr[]" / "w.arr[]" path. Returns false for plain nested
// fields (no `[]`) or non-array fields. Used by the subscript / cast
// transfer branches.
static bool GetOwnedArrayField(const Expr *E, ASTContext &Ctx,
                               const bsc::Place *&Path) {
  bsc::PlaceBuilder PB(Ctx);
  return PeelHostAndFieldPlace(PB, E, Path) && IsOwnedArrayFieldPath(Path);
}

// Whether the owned-field place `FieldPath` of aggregate `VD` has lost
// ownership. `FieldPath` may name an exact tracked field ("p", "arr[].a") or
// an aggregate prefix ("arr[]", "inner") whose tracked descendants are all
// moved. Paths that do not touch any tracked owned field (plain non-owned
// members) are never "moved". Null-owned fields own nothing but are not
// moved, so a path whose remaining tracked fields are all null stays readable
// (mirror of the scalar null-owned read rule).
static bool ArrayFieldPathMoved(const Ownership::OwnershipStatus &Stat,
                                const VarDecl *VD, FieldKey FieldPath,
                                bool IsOPS) {
  const FieldSet *All = nullptr;
  const FieldSet *Owned = nullptr;
  const FieldSet *Null = nullptr;
  if (IsOPS) {
    auto AllIt = Stat.OPSAllOwnedFields.find(VD);
    auto OwnedIt = Stat.OPSOwnedOwnedFields.find(VD);
    auto NullIt = Stat.OPSNullOwnedFields.find(VD);
    if (AllIt == Stat.OPSAllOwnedFields.end())
      return false;
    All = &AllIt->second;
    Owned = OwnedIt == Stat.OPSOwnedOwnedFields.end()
                ? nullptr
                : &OwnedIt->second;
    Null = NullIt == Stat.OPSNullOwnedFields.end() ? nullptr : &NullIt->second;
  } else {
    auto AllIt = Stat.SAllOwnedFields.find(VD);
    auto OwnedIt = Stat.SOwnedOwnedFields.find(VD);
    auto NullIt = Stat.SNullOwnedFields.find(VD);
    if (AllIt == Stat.SAllOwnedFields.end())
      return false;
    All = &AllIt->second;
    Owned = OwnedIt == Stat.SOwnedOwnedFields.end() ? nullptr : &OwnedIt->second;
    Null = NullIt == Stat.SNullOwnedFields.end() ? nullptr : &NullIt->second;
  }
  if (All->empty())
    return false;
  bool HasTracked = false;
  bool HasOwnedOrNull = false;
  if (!FieldPath || FieldPath->getKind() == bsc::Place::Kind::Var) {
    // Whole-aggregate query (the root Var place, the historical "" — i.e.
    // `a[i]` or the array variable itself): every tracked field belongs to
    // the aggregate. Handled separately because a Var place prefixes every
    // key of the variable. The aggregate is moved only when none of its
    // tracked fields is still owned or null-owned.
    HasTracked = true;
    for (FieldKey Key : *All) {
      if ((Owned && Owned->count(Key)) || (Null && Null->count(Key))) {
        HasOwnedOrNull = true;
        break;
      }
    }
  } else {
    for (FieldKey Key : *All) {
      if (isFieldPathPrefix(FieldPath, Key) ||
          isFieldPathPrefix(Key, FieldPath)) {
        HasTracked = true;
        if ((Owned && Owned->count(Key)) || (Null && Null->count(Key)))
          HasOwnedOrNull = true;
      }
    }
  }
  return HasTracked && !HasOwnedOrNull;
}

static string moveAsterisksToFront(string str) {
  size_t asterisk_pos = str.find_last_not_of('*');
  if (asterisk_pos != string::npos) {
    string asterisks = str.substr(asterisk_pos + 1);
    str.erase(asterisk_pos + 1);
    str.insert(0, asterisks);
  }
  return str;
}

// Join the old key spellings of a field set for a diagnostic. Iteration is
// the FieldSet order contract (insertion order up to 10 elements, then the
// historical sorted-overflow order), matching the old SmallSet iteration.
// Rendering per element matches the historical output: BOP form
// "element + var", struct form "moveAsterisksToFront(var.element)".
static string concatFields(const VarDecl *VD, const FieldSet &fields) {
  llvm::SmallVector<string, 16> orderedFields;
  for (FieldKey P : fields)
    orderedFields.push_back(placeToKeyString(P));
  std::sort(orderedFields.begin(), orderedFields.end());
  string contactedFields = "";
  size_t count = 0;
  for (const string &element : orderedFields) {
    if (count++ != 0) {
      contactedFields += ", ";
    }
    if (VD->getType()->isPointerType() &&
        !VD->getType()->getPointeeType().getCanonicalType()->isRecordType()) {
      contactedFields = contactedFields + element + VD->getNameAsString();
    } else {
      contactedFields =
          contactedFields +
          moveAsterisksToFront(VD->getNameAsString() + "." + element);
    }
    if (count > 8) {
      break;
    }
  }
  if (count == 0) {
    contactedFields += "*" + VD->getNameAsString();
  }
  if (count < fields.size()) {
    contactedFields += "...";
  }
  if (count > 1) {
    contactedFields += " are";
  } else {
    contactedFields += " is";
  }
  return contactedFields;
}

static string concatUnmovedFields(const VarDecl *VD, const FieldSet &ownedFields,
                                  const FieldSet &allFields) {
  FieldSet unmovedFields;
  for (FieldKey P : allFields) {
    if (!ownedFields.count(P)) {
      unmovedFields.insert(P);
    }
  }
  return concatFields(VD, unmovedFields);
}

static bool IsCastFromVoidPointer(Expr *E) {
  E = E->IgnoreParenImpCasts();

  if (AbstractConditionalOperator *ACO =
          dyn_cast<AbstractConditionalOperator>(E)) {
    return IsCastFromVoidPointer(ACO->getTrueExpr()) ||
           IsCastFromVoidPointer(ACO->getFalseExpr());
  }
  if (CStyleCastExpr *CSCE = dyn_cast<CStyleCastExpr>(E)) {
    QualType QT = CSCE->getType();
    if (QT.isOwnedPointer() &&
        !QT->getPointeeType()->isVoidType()) {
      Expr *Sub = CSCE->getSubExpr();
      if (Sub->getType()->isVoidPointerType() &&
          Sub->getType().isOwnedPointer()) {
        return true;
      }
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Operations and queries on OwnershipStatus.
//===----------------------------------------------------------------------===//

bool Ownership::OwnershipStatus::empty() const {
  return OPSStatus.empty() && OPSAllOwnedFields.empty() &&
         OPSOwnedOwnedFields.empty() && OPSNullOwnedFields.empty() &&
         OPSMovedOwnedFields.empty() && SStatus.empty() &&
         SAllOwnedFields.empty() && SOwnedOwnedFields.empty() &&
         SNullOwnedFields.empty() && SMovedOwnedFields.empty() &&
         BOPStatus.empty() && BOPAllOwnedFields.empty() &&
         BOPOwnedOwnedFields.empty();
}

Ownership::OwnershipStatus
OwnershipImpl::merge(Ownership::OwnershipStatus statsA,
                     Ownership::OwnershipStatus statsB) {
  if (statsA.empty())
    return Ownership::OwnershipStatus(
        statsB.OPSStatus, statsB.OPSAllOwnedFields, statsB.OPSOwnedOwnedFields,
        statsB.OPSNullOwnedFields, statsB.OPSMovedOwnedFields, statsB.SStatus,
        statsB.SAllOwnedFields, statsB.SOwnedOwnedFields,
        statsB.SNullOwnedFields, statsB.SUninitOwnedFields,
        statsB.SMovedOwnedFields, statsB.BOPStatus, statsB.BOPAllOwnedFields,
        statsB.BOPOwnedOwnedFields);

  for (auto it = statsB.OPSStatus.begin(), ei = statsB.OPSStatus.end();
       it != ei; ++it) {
    const VarDecl *VD = it->first;
    const llvm::BitVector BV = it->second;
    if (statsA.OPSStatus.count(VD)) {
      statsA.OPSStatus[VD] |= BV;
    } else {
      statsA.OPSStatus[VD] = BV;
    }
  }
  for (auto it = statsB.OPSAllOwnedFields.begin(),
            ei = statsB.OPSAllOwnedFields.end();
       it != ei; ++it) {
    const VarDecl *VD = it->first;
    FieldSet OwnedValue = statsB.OPSOwnedOwnedFields[VD];
    FieldSet NullValue = statsB.OPSNullOwnedFields[VD];
    if (statsA.OPSAllOwnedFields.count(VD) &&
        (!statsA.OPSOwnedOwnedFields.empty() ||
         !statsA.OPSNullOwnedFields.empty())) {
      if (!statsA.OPSOwnedOwnedFields.empty() && !OwnedValue.empty()) {
        for (FieldKey s : OwnedValue)
          statsA.OPSOwnedOwnedFields[VD].insert(s);
      }
      if (!statsA.OPSNullOwnedFields.empty() && !NullValue.empty()) {
        for (FieldKey s : NullValue)
          statsA.OPSNullOwnedFields[VD].insert(s);
      }
    } else {
      // statsA has no per-field state for VD: adopt statsB's field sets.
      // Copy the static All list and the Owned / Null sets separately
      // (mirror of the S branch), instead of overwriting All with the
      // Owned values.
      for (FieldKey s : it->second)
        statsA.OPSAllOwnedFields[VD].insert(s);
      for (FieldKey s : OwnedValue)
        statsA.OPSOwnedOwnedFields[VD].insert(s);
      for (FieldKey s : NullValue)
        statsA.OPSNullOwnedFields[VD].insert(s);
    }
  }

  for (auto it = statsB.SStatus.begin(), ei = statsB.SStatus.end(); it != ei;
       ++it) {
    const VarDecl *VD = it->first;
    const llvm::BitVector BV = it->second;
    if (statsA.SStatus.count(VD)) {
      statsA.SStatus[VD] |= BV;
    } else {
      statsA.SStatus[VD] = BV;
    }
  }
  for (auto it = statsB.SAllOwnedFields.begin(),
            ei = statsB.SAllOwnedFields.end();
       it != ei; ++it) {
    const VarDecl *VD = it->first;
    FieldSet OwnedValue = statsB.SOwnedOwnedFields[VD];
    FieldSet NullValue = statsB.SNullOwnedFields[VD];
    if (statsA.SAllOwnedFields.count(VD) &&
        (!statsA.SOwnedOwnedFields.empty() || !statsA.SNullOwnedFields.empty())) {
      if (!statsA.SOwnedOwnedFields.empty() && !OwnedValue.empty()) {
        for (FieldKey s : OwnedValue)
          statsA.SOwnedOwnedFields[VD].insert(s);
      }
      if (!statsA.SNullOwnedFields.empty() && !NullValue.empty()) {
        for (FieldKey s : NullValue)
          statsA.SNullOwnedFields[VD].insert(s);
      }
    } else {
      // statsA has no per-field state for VD (both owned and null field sets
      // are empty): adopt statsB's field sets. SAllOwnedFields must be copied
      // too, otherwise later field transfers (setArrayFieldMoved etc.) see an
      // empty static field list and skip the transition.
      for (FieldKey s : it->second)
        statsA.SAllOwnedFields[VD].insert(s);
      for (FieldKey s : OwnedValue)
        statsA.SOwnedOwnedFields[VD].insert(s);
      for (FieldKey s : NullValue)
        statsA.SNullOwnedFields[VD].insert(s);
    }
  }
  // A path uninitialized on either branch stays uninitialized.
  for (auto it = statsB.SUninitOwnedFields.begin(),
            ei = statsB.SUninitOwnedFields.end();
       it != ei; ++it)
    for (FieldKey s : it->second)
      statsA.SUninitOwnedFields[it->first].insert(s);
  // A field moved on either branch remains unsafe to use after the join.  It
  // may simultaneously remain in SOwnedOwnedFields when the other branch
  // still owns it; that possible ownership is needed by leak checking.
  for (auto it = statsB.SMovedOwnedFields.begin(),
            ei = statsB.SMovedOwnedFields.end();
       it != ei; ++it)
    for (const auto &s : it->second)
      statsA.SMovedOwnedFields[it->first].insert(s);
  // Owned-pointer-to-struct host counterpart (mirror of SMovedOwnedFields).
  for (auto it = statsB.OPSMovedOwnedFields.begin(),
            ei = statsB.OPSMovedOwnedFields.end();
       it != ei; ++it)
    for (const auto &s : it->second)
      statsA.OPSMovedOwnedFields[it->first].insert(s);

  for (auto it = statsB.BOPStatus.begin(), ei = statsB.BOPStatus.end();
       it != ei; ++it) {
    const VarDecl *VD = it->first;
    const llvm::BitVector BV = it->second;
    if (statsA.BOPStatus.count(VD)) {
      statsA.BOPStatus[VD] |= BV;
    } else {
      statsA.BOPStatus[VD] = BV;
    }
  }
  for (auto it = statsB.BOPAllOwnedFields.begin(),
            ei = statsB.BOPAllOwnedFields.end();
       it != ei; ++it) {
    const VarDecl *VD = it->first;
    FieldSet value = statsB.BOPOwnedOwnedFields[VD];
    if (statsA.BOPAllOwnedFields.count(VD) &&
        !statsA.BOPOwnedOwnedFields.empty()) {
      if (!value.empty()) {
        for (FieldKey s : value)
          statsA.BOPOwnedOwnedFields[VD].insert(s);
      }
    } else {
      statsA.BOPAllOwnedFields[VD] = value;
    }
  }

  return Ownership::OwnershipStatus(
      statsA.OPSStatus, statsA.OPSAllOwnedFields, statsA.OPSOwnedOwnedFields,
      statsA.OPSNullOwnedFields, statsA.OPSMovedOwnedFields, statsA.SStatus,
      statsA.SAllOwnedFields, statsA.SOwnedOwnedFields, statsA.SNullOwnedFields,
      statsA.SUninitOwnedFields, statsA.SMovedOwnedFields, statsA.BOPStatus,
      statsA.BOPAllOwnedFields, statsA.BOPOwnedOwnedFields);
}

bool Ownership::OwnershipStatus::equals(const OwnershipStatus &V) const {
  return OPSStatus == V.OPSStatus && OPSAllOwnedFields == V.OPSAllOwnedFields &&
         OPSOwnedOwnedFields == V.OPSOwnedOwnedFields &&
         OPSNullOwnedFields == V.OPSNullOwnedFields &&
         OPSMovedOwnedFields == V.OPSMovedOwnedFields && SStatus == V.SStatus &&
         SAllOwnedFields == V.SAllOwnedFields &&
         SOwnedOwnedFields == V.SOwnedOwnedFields &&
         SNullOwnedFields == V.SNullOwnedFields &&
         SUninitOwnedFields == V.SUninitOwnedFields &&
         SMovedOwnedFields == V.SMovedOwnedFields && BOPStatus == V.BOPStatus &&
         BOPAllOwnedFields == V.BOPAllOwnedFields &&
         BOPOwnedOwnedFields == V.BOPOwnedOwnedFields;
}

// The status bits of VD, or nullptr when it is not a tracked aggregate. A
// variable is tracked by exactly one of the three aggregate maps; shared by
// is / has / isOrHas.
const llvm::BitVector *
Ownership::OwnershipStatus::bitsOf(const VarDecl *VD) const {
  auto Find = [VD](const auto &Map) -> const llvm::BitVector * {
    auto It = Map.find(VD);
    return It == Map.end() ? nullptr : &It->second;
  };
  if (const llvm::BitVector *Bits = Find(OPSStatus))
    return Bits;
  if (const llvm::BitVector *Bits = Find(SStatus))
    return Bits;
  if (const llvm::BitVector *Bits = Find(BOPStatus))
    return Bits;
  return nullptr;
}

// Exactly one bit is set and it is S.
bool Ownership::OwnershipStatus::is(const VarDecl *VD, Status S) const {
  const llvm::BitVector *StatusBits = bitsOf(VD);
  if (!StatusBits || !StatusBits->test(getIndex(S)))
    return false;
  llvm::BitVector Rest = *StatusBits;
  Rest.reset(getIndex(S));
  return !Rest.any();
}

// The S bit is set, together with at least one other bit.
bool Ownership::OwnershipStatus::has(const VarDecl *VD, Status S) const {
  const llvm::BitVector *StatusBits = bitsOf(VD);
  if (!StatusBits || !StatusBits->test(getIndex(S)))
    return false;
  llvm::BitVector Rest = *StatusBits;
  Rest.reset(getIndex(S));
  return Rest.any();
}

// The S component is present, exactly or together with other bits (e.g.
// Moved|Null): `is(VD, S) || has(VD, S)`, the "possibly S" idiom of the move /
// use-after-move checks.
bool Ownership::OwnershipStatus::isOrHas(const VarDecl *VD, Status S) const {
  return is(VD, S) || has(VD, S);
}

bool Ownership::OwnershipStatus::canAssign(const VarDecl *VD) const {
  unsigned uninitIndex = getIndex(Uninitialized);
  unsigned movedIndex = getIndex(Moved);
  unsigned nullIndex = getIndex(Null);
  if (OPSStatus.count(VD)) {
    auto it = OPSStatus.find(VD);
    llvm::BitVector status = it->second;

    if (status.test(uninitIndex)) {
      status.reset(uninitIndex);
    }
    if (status.test(movedIndex)) {
      status.reset(movedIndex);
    }
    if (status.test(nullIndex)) {
      status.reset(nullIndex);
    }
    return !status.any();
  }

  if (BOPStatus.count(VD)) {
    auto it = BOPStatus.find(VD);
    llvm::BitVector status = it->second;

    if (status.test(uninitIndex)) {
      status.reset(uninitIndex);
    }
    if (status.test(movedIndex)) {
      status.reset(movedIndex);
    }
    if (status.test(nullIndex)) {
      status.reset(nullIndex);
    }
    return !status.any();
  }

  return false;
}

void Ownership::OwnershipStatus::init(const VarDecl *VD, ASTContext &Ctx) {
  QualType VDType = VD->getType();
  if (VDType->isPointerType()) {
    if (VDType->getPointeeType().getCanonicalType()->isRecordType()) {
      if (!OPSStatus.count(VD)) {
        if (const RecordType *RT = dyn_cast<RecordType>(
                VDType->getPointeeType().getCanonicalType())) {
          bsc::PlaceBuilder PB(Ctx);
          initOPS(RT->getDecl(), VD, Source::OPS, Ctx, 10,
                  PB.Build(VD, VD->getLocation()));
        }
      }
    } else {
      if (!BOPStatus.count(VD)) {
        bsc::PlaceBuilder PB(Ctx);
        initBOP(VDType, VD, Source::BOP, Ctx, 10,
                PB.Build(VD, VD->getLocation()));
      }
    }
  } else if (const auto *AT = VDType->getAsArrayTypeUnsafe()) {
    // Whole-array tracking for owned-element arrays: the array
    // is one aggregate; element-level transfers update the aggregate state.
    // Do not canonicalize the element: canonicalizing an array type drops the
    // _Owned qualifier of its element type.
    QualType ElemTy = AT->getElementType();
    while (const auto *Inner = ElemTy->getAsArrayTypeUnsafe())
      ElemTy = Inner->getElementType();
    if (ElemTy.isOwnedPointer()) {
      if (!BOPStatus.count(VD)) {
        BOPStatus[VD] = llvm::BitVector(7, 0);
        set(VD, Uninitialized);
      }
    } else if (ElemTy->isRecordType() &&
               ElemTy.isOrContainsOwned(BSCLookThrough::NoPointer)) {
      if (!SStatus.count(VD)) {
        // ElemTy may be an ElaboratedType (e.g. "struct S") rather than the
        // canonical RecordType; peel sugar so dyn_cast finds the record.
        if (const RecordType *RT =
                dyn_cast<RecordType>(ElemTy.getCanonicalType())) {
          bsc::PlaceBuilder PB(Ctx);
          initS(RT->getDecl(), VD, Source::S, Ctx, 10,
                PB.Build(VD, VD->getLocation()));
        }
      }
    }
  } else {
    if (!SStatus.count(VD)) {
      if (const RecordType *RT =
              dyn_cast<RecordType>(VDType.getCanonicalType())) {
        bsc::PlaceBuilder PB(Ctx);
        initS(RT->getDecl(), VD, Source::S, Ctx, 10,
              PB.Build(VD, VD->getLocation()));
      }
    }
  }
}

// Strip the array levels of a field type, appending one Index projection
// ("[]", index-free) per level to the key place and returning the element
// type. Returns false when the field is not an array. Shared by initS /
// initOPS (the historical path used `[]` for each array level).
static bool StripArrayFieldLevels(bsc::PlaceBuilder &PB, QualType FieldTy,
                                  FieldKey &FieldPlace, SourceLocation Loc,
                                  QualType &ElemTy) {
  bool IsArray = false;
  ElemTy = FieldTy;
  while (const auto *AT = dyn_cast<ArrayType>(ElemTy)) {
    FieldPlace = PB.BuildIndex(FieldPlace, AT->getElementType(), "[]", Loc);
    IsArray = true;
    ElemTy = AT->getElementType();
  }
  return IsArray;
}

// Register an array field (Index place "arr[]" for owned-pointer elements,
// "arr[].a" via recursion for owned-record elements) into the target All
// set. Returns the record to recurse into, or nullptr for owned-pointer
// elements (leaf).
static const RecordDecl *RegisterArrayField(FieldSet &AllSet,
                                            FieldKey ArrFieldPlace,
                                            QualType ElemTy) {
  if (ElemTy.isOwnedPointer()) {
    AllSet.insert(ArrFieldPlace);
    return nullptr;
  }
  if (const RecordType *RT =
          dyn_cast<RecordType>(ElemTy.getCanonicalType())) {
    if (RT->isOwnedStruct())
      AllSet.insert(ArrFieldPlace);
    return RT->getDecl();
  }
  return nullptr;
}

void Ownership::OwnershipStatus::initOPS(const RecordDecl *RD,
                                         const VarDecl *VD, Source source,
                                         ASTContext &Ctx, int depth,
                                         const bsc::Place *Parent) {
  if (depth == 0) {
    return;
  }
  bsc::PlaceBuilder PB(Ctx);
  // record all owned fields of VD in OPSAllOwnedFields
  for (RecordDecl::field_iterator it = RD->field_begin(), ei = RD->field_end();
       it != ei; ++it) {
    // Use the sugared type: canonicalizing an array type drops the local
    // _Owned qualifier of its element (int *_Owned arr[2]).
    QualType FT = it->getType();
    FieldKey FieldPlace = PB.BuildField(Parent, *it, it->getLocation());
    if (IsTrackedType(FT)) {
      // Array fields (e.g. struct S { int *_Owned arr[2]; }): the key uses
      // an Index projection ("[]") for the array level and recurses into the
      // element type, so the tracked field becomes "arr[]" (owned pointers) /
      // "arr[].a" (records).
      QualType FieldTy;
      if (StripArrayFieldLevels(PB, FT, FieldPlace, it->getLocation(),
                                FieldTy)) {
        if (source == Source::OPS) {
          if (OPSAllOwnedFields[VD].empty()) {
            OPSAllOwnedFields[VD] = {};
            OPSOwnedOwnedFields[VD] = {};
            OPSNullOwnedFields[VD] = {};
          }
          if (const RecordDecl *RT = RegisterArrayField(
                  OPSAllOwnedFields[VD], FieldPlace, FieldTy))
            initOPS(RT, VD, source, Ctx, depth - 1, FieldPlace);
        } else if (source == Source::S) {
          if (SAllOwnedFields[VD].empty()) {
            SAllOwnedFields[VD] = {};
            SOwnedOwnedFields[VD] = {};
            SNullOwnedFields[VD] = {};
          }
          if (const RecordDecl *RT = RegisterArrayField(
                  SAllOwnedFields[VD], FieldPlace, FieldTy))
            initOPS(RT, VD, source, Ctx, depth - 1, FieldPlace);
        }
        continue;
      }
      if (source == Source::OPS) {
        if (OPSAllOwnedFields[VD].empty()) {
          OPSAllOwnedFields[VD] = {};
          OPSOwnedOwnedFields[VD] = {};
          OPSNullOwnedFields[VD] = {};
        }
        if (!FT->isRecordType() || FT->isOwnedStruct())
          OPSAllOwnedFields[VD].insert(FieldPlace);
      } else if (source == Source::S) {
        if (!FT->isRecordType() || FT->isOwnedStruct())
          SAllOwnedFields[VD].insert(FieldPlace);
      } else {
        llvm_unreachable("Unexpected branch");
      }

      // if FT has owned fields,
      // recursively record all owned fields of FT in OPSAllOwnedFields[VD]
      if (FT.isOwnedPointer()) {
        if (const RecordType *RT =
                dyn_cast<RecordType>(FT->getPointeeType().getCanonicalType())) {
          if (RT->isOwnedStruct()) {
            // The historical key "fieldName + '*'" (the nested owned pointer
            // of the field) is a Deref projection over the field.
            FieldKey DerefPlace =
                PB.BuildDeref(FieldPlace, FT->getPointeeType(),
                              it->getLocation());
            if (source == Source::OPS) {
              OPSAllOwnedFields[VD].insert(DerefPlace);
            } else if (source == Source::S) {
              SAllOwnedFields[VD].insert(DerefPlace);
            } else {
              llvm_unreachable("Unexpected branch");
            }
          }
          initOPS(RT->getDecl(), VD, source, Ctx, depth - 1, FieldPlace);
        } else {
          initBOP(FT, VD, source, Ctx, depth - 1, FieldPlace);
        }
      }
      if (const RecordType *RT = dyn_cast<RecordType>(FT.getCanonicalType())) {
        if (RT->isOwnedStruct()) {
          if (source == Source::OPS) {
            OPSAllOwnedFields[VD].insert(FieldPlace);
          } else if (source == Source::S) {
            SAllOwnedFields[VD].insert(FieldPlace);
          } else {
            llvm_unreachable("Unexpected branch");
          }
        }
        initS(RT->getDecl(), VD, source, Ctx, depth - 1, FieldPlace);
      }
    }
  }
  // set VD to UNINITIALIZED
  if (source == Source::OPS && depth == 10) {
    OPSStatus[VD] = llvm::BitVector(7, 0);
    set(VD, Ownership::Status::Uninitialized);
  }
}

void Ownership::OwnershipStatus::initS(const RecordDecl *RD, const VarDecl *VD,
                                       Source source, ASTContext &Ctx,
                                       int depth, const bsc::Place *Parent) {
  if (depth == 0) {
    return;
  }
  bsc::PlaceBuilder PB(Ctx);

  // owned struct special manipulation
  if (VD->getType().getTypePtr()->isOwnedStruct()) {
    if (source == Source::S) {
      if (SAllOwnedFields[VD].empty()) {
        SAllOwnedFields[VD] = {};
        SOwnedOwnedFields[VD] = {};
        SNullOwnedFields[VD] = {};
      }
    }
  }

  // record all owned fields of VD in SAllOwnedFields
  for (RecordDecl::field_iterator it = RD->field_begin(), ei = RD->field_end();
       it != ei; ++it) {
    // Use the sugared type: canonicalizing an array type drops the local
    // _Owned qualifier of its element (int *_Owned arr[2]).
    QualType FT = it->getType();
    FieldKey FieldPlace = PB.BuildField(Parent, *it, it->getLocation());
    if (IsTrackedType(FT)) {
      // Array fields (e.g. struct Wrap { struct S arr[2]; }): the key uses
      // an Index projection ("[]") for the array level and recurses into the
      // element type, so the tracked field becomes "arr[].a" instead of just
      // "arr".
      QualType FieldTy;
      if (StripArrayFieldLevels(PB, FT, FieldPlace, it->getLocation(),
                                FieldTy)) {
        if (source == Source::OPS) {
          if (const RecordDecl *RT = RegisterArrayField(
                  OPSAllOwnedFields[VD], FieldPlace, FieldTy))
            initS(RT, VD, source, Ctx, depth - 1, FieldPlace);
        } else if (source == Source::S) {
          if (SAllOwnedFields[VD].empty()) {
            SAllOwnedFields[VD] = {};
            SOwnedOwnedFields[VD] = {};
            SNullOwnedFields[VD] = {};
          }
          if (const RecordDecl *RT = RegisterArrayField(
                  SAllOwnedFields[VD], FieldPlace, FieldTy))
            initS(RT, VD, source, Ctx, depth - 1, FieldPlace);
        }
        continue;
      }
      if (source == Source::OPS) {
        if (!FT->isRecordType() || FT->isOwnedStruct())
          OPSAllOwnedFields[VD].insert(FieldPlace);
      } else if (source == Source::S) {
        if (SAllOwnedFields[VD].empty()) {
          SAllOwnedFields[VD] = {};
          SOwnedOwnedFields[VD] = {};
          SNullOwnedFields[VD] = {};
        }
        if (!FT->isRecordType() || FT->isOwnedStruct())
          SAllOwnedFields[VD].insert(FieldPlace);
      } else {
        llvm_unreachable("Unexpected branch");
      }

      // recursively record all owned fields
      if (FT.isOwnedPointer()) {
        if (const RecordType *RT =
                dyn_cast<RecordType>(FT->getPointeeType().getCanonicalType())) {
          if (RT->isOwnedStruct()) {
            // The historical key "fieldName + '*'" is a Deref projection
            // over the field.
            FieldKey DerefPlace =
                PB.BuildDeref(FieldPlace, FT->getPointeeType(),
                              it->getLocation());
            if (source == Source::OPS) {
              OPSAllOwnedFields[VD].insert(DerefPlace);
            } else if (source == Source::S) {
              SAllOwnedFields[VD].insert(DerefPlace);
            } else {
              llvm_unreachable("Unexpected branch");
            }
          }
          initOPS(RT->getDecl(), VD, source, Ctx, depth - 1, FieldPlace);
        } else {
          initBOP(FT, VD, source, Ctx, depth - 1, FieldPlace);
        }
      }
      if (const RecordType *RT = dyn_cast<RecordType>(FT.getCanonicalType())) {
        if (RT->isOwnedStruct()) {
          if (source == Source::OPS) {
            OPSAllOwnedFields[VD].insert(FieldPlace);
          } else if (source == Source::S) {
            SAllOwnedFields[VD].insert(FieldPlace);
          } else {
            llvm_unreachable("Unexpected branch");
          }
        }
        initS(RT->getDecl(), VD, source, Ctx, depth - 1, FieldPlace);
      }
    }
  }
  // set VD to UNINITIALIZED
  if (source == Source::S && depth == 10) {
    SStatus[VD] = llvm::BitVector(7, 0);
    set(VD, Ownership::Status::Uninitialized);
    SUninitOwnedFields[VD] = SAllOwnedFields[VD];
  }
}

void Ownership::OwnershipStatus::initBOP(QualType QT, const VarDecl *VD,
                                         Source source, ASTContext &Ctx,
                                         int depth, const bsc::Place *Parent) {
  if (depth == 0) {
    return;
  }
  bsc::PlaceBuilder PB(Ctx);
  // Each nested owned-pointer level is one Deref projection over the parent
  // chain: the historical keys were Parent + "*", Parent + "**", ... — at a
  // BOP root that is "*", "**", ... (Deref(Var(p)) etc.).
  FieldKey Cur = Parent;
  QT = QT->getPointeeType().getCanonicalType();
  while (true) {
    Cur = PB.BuildDeref(Cur, QT, VD->getLocation());
    if (QT.isOwnedPointer()) {
      if (source == Source::OPS) {
        OPSAllOwnedFields[VD].insert(Cur);
      } else if (source == Source::S) {
        SAllOwnedFields[VD].insert(Cur);
      } else {
        if (BOPAllOwnedFields[VD].empty()) {
          BOPAllOwnedFields[VD] = {};
          BOPOwnedOwnedFields[VD] = {};
        }
        BOPAllOwnedFields[VD].insert(Cur);
      }
      QT = QT->getPointeeType().getCanonicalType();
    } else {
      break;
    }
  }
  // set VD to UNINITIALIZED
  if (source == Source::BOP) {
    BOPStatus[VD] = llvm::BitVector(7, 0);
    set(VD, Ownership::Status::Uninitialized);
  }
}

void Ownership::OwnershipStatus::set(const VarDecl *VD, Status S) {
  if (OPSStatus.count(VD)) {
    llvm::BitVector &status = OPSStatus[VD];
    unsigned bitIndex = getIndex(S);
    status.set(bitIndex);
  }

  if (SStatus.count(VD)) {
    llvm::BitVector &status = SStatus[VD];
    unsigned bitIndex = getIndex(S);
    status.set(bitIndex);
  }

  if (BOPStatus.count(VD)) {
    llvm::BitVector &status = BOPStatus[VD];
    unsigned bitIndex = getIndex(S);
    status.set(bitIndex);
  }
}

void Ownership::OwnershipStatus::reset(const VarDecl *VD, Status S) {
  if (OPSStatus.count(VD)) {
    llvm::BitVector &status = OPSStatus[VD];
    unsigned bitIndex = getIndex(S);
    status.reset(bitIndex);
  }

  if (SStatus.count(VD)) {
    llvm::BitVector &status = SStatus[VD];
    unsigned bitIndex = getIndex(S);
    status.reset(bitIndex);
  }

  if (BOPStatus.count(VD)) {
    llvm::BitVector &status = BOPStatus[VD];
    unsigned bitIndex = getIndex(S);
    status.reset(bitIndex);
  }
}

void Ownership::OwnershipStatus::resetAll(const VarDecl *VD) {
  if (OPSStatus.count(VD)) {
    llvm::BitVector &status = OPSStatus[VD];
    for (unsigned index = 0; index < 7; index++)
      status.reset(index);
  }

  if (SStatus.count(VD)) {
    llvm::BitVector &status = SStatus[VD];
    for (unsigned index = 0; index < 7; index++)
      status.reset(index);
  }

  if (BOPStatus.count(VD)) {
    llvm::BitVector &status = BOPStatus[VD];
    for (unsigned index = 0; index < 7; index++)
      status.reset(index);
  }
}

void Ownership::OwnershipStatus::setToOwned(const VarDecl *VD) {
  if (OPSStatus.count(VD)) {
    resetAll(VD);
    set(VD, Ownership::Status::Owned);
    OPSOwnedOwnedFields[VD] = OPSAllOwnedFields[VD];
    OPSNullOwnedFields[VD].clear();
    OPSMovedOwnedFields[VD].clear();
  }

  if (SStatus.count(VD)) {
    resetAll(VD);
    set(VD, Ownership::Status::Owned);
    SOwnedOwnedFields[VD] = SAllOwnedFields[VD];
    SNullOwnedFields[VD].clear();
    SUninitOwnedFields[VD].clear();
    SMovedOwnedFields[VD].clear();
  }

  if (BOPStatus.count(VD)) {
    resetAll(VD);
    set(VD, Ownership::Status::Owned);
    BOPOwnedOwnedFields[VD] = BOPAllOwnedFields[VD];
  }
}

void Ownership::OwnershipStatus::setToAllMoved(const VarDecl *VD) {
  if (OPSStatus.count(VD)) {
    resetAll(VD);
    if (!OPSAllOwnedFields[VD].empty()) {
      set(VD, Ownership::Status::AllMoved);
      OPSOwnedOwnedFields[VD].clear();
    } else {
      set(VD, Ownership::Status::Owned);
    }
  }

  if (BOPStatus.count(VD)) {
    resetAll(VD);
    if (!BOPAllOwnedFields[VD].empty()) {
      set(VD, Ownership::Status::AllMoved);
      BOPOwnedOwnedFields[VD].clear();
    } else {
      set(VD, Ownership::Status::Owned);
    }
  }
}

void Ownership::OwnershipStatus::setToAllMoved(const Expr *E, ASTContext &Ctx) {
  E = E->IgnoreParenImpCasts();
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
    setToAllMoved(VD);
    return;
  }
  if (const MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
    FieldKey FieldPlace = getMemberFieldPlace(ME, Ctx);
    if (FieldPlace) {
      const VarDecl *VD = FieldPlace->getRoot();
      if (OPSStatus.count(VD)) {
        if (OPSAllOwnedFields[VD].count(FieldPlace)) {
          OPSOwnedOwnedFields[VD].insert(FieldPlace);
          auto allPrefixStrs = fieldDescendants(OPSAllOwnedFields[VD], FieldPlace);
          for (FieldKey str : allPrefixStrs) {
            OPSOwnedOwnedFields[VD].erase(str);
          }
          resetAll(VD);
          if (OPSOwnedOwnedFields[VD].size() == 0) {
            set(VD, Ownership::Status::AllMoved);
          } else if (OPSOwnedOwnedFields[VD].size() < OPSAllOwnedFields[VD].size()) {
            set(VD, Ownership::Status::PartialMoved);
          } else {
            set(VD, Ownership::Status::Owned);
          }
        }
      }
      if (SStatus.count(VD)) {
        if (SAllOwnedFields[VD].count(FieldPlace)) {
          SOwnedOwnedFields[VD].insert(FieldPlace);
          SNullOwnedFields[VD].erase(FieldPlace);
          SUninitOwnedFields[VD].erase(FieldPlace);
          SMovedOwnedFields[VD].erase(FieldPlace);
          auto allPrefixStrs = fieldDescendants(SAllOwnedFields[VD], FieldPlace);
          for (FieldKey str : allPrefixStrs) {
            SOwnedOwnedFields[VD].erase(str);
            SNullOwnedFields[VD].erase(str);
            SUninitOwnedFields[VD].erase(str);
            SMovedOwnedFields[VD].insert(str);
          }
          resetAll(VD);
          if (SOwnedOwnedFields[VD].size() == 0) {
            set(VD, Ownership::Status::AllMoved);
          } else if (SOwnedOwnedFields[VD].size() < SAllOwnedFields[VD].size()) {
            set(VD, Ownership::Status::PartialMoved);
          } else {
            set(VD, Ownership::Status::Owned);
          }
        }
      }
    }
  }
}

void Ownership::OwnershipStatus::setToNull(const VarDecl *VD) {
  if (OPSStatus.count(VD)) {
    OPSOwnedOwnedFields[VD].clear();
    resetAll(VD);
    set(VD, Ownership::Status::Null);
  }
  if (BOPStatus.count(VD)) {
    BOPOwnedOwnedFields[VD].clear();
    resetAll(VD);
    set(VD, Ownership::Status::Null);
  }
}

// Mark a single owned field of an owned-pointer-to-struct (VD in OPSStatus) as
// null: remove it from OwnedOwned, add it to NullOwned, and propagate to any
// fields nested below it (field descendants).
void Ownership::OwnershipStatus::setOwnedFieldNull(const VarDecl *VD,
                                                   FieldKey FieldPath) {
  if (!OPSStatus.count(VD))
    return;
  if (OPSAllOwnedFields[VD].count(FieldPath)) {
    OPSOwnedOwnedFields[VD].erase(FieldPath);
    OPSNullOwnedFields[VD].insert(FieldPath);
    auto allPrefixStrs = fieldDescendants(OPSAllOwnedFields[VD], FieldPath);
    for (FieldKey str : allPrefixStrs) {
      OPSOwnedOwnedFields[VD].erase(str);
      OPSNullOwnedFields[VD].insert(str);
    }
  }
}

// __assume_null(*s) on an owned struct pointer: drop ownership of every
// registered owned field of the struct.
void Ownership::OwnershipStatus::assumeAllOwnedFieldsNull(
    const VarDecl *VD) {
  if (!OPSStatus.count(VD))
    return;
  llvm::SmallVector<FieldKey, 16> paths(OPSAllOwnedFields[VD].begin(),
                                        OPSAllOwnedFields[VD].end());
  for (FieldKey path : paths) {
    // Array fields ("arr[]", "arr[].a") carry their own element-wise
    // ownership; an aggregate __assume_null cannot drop them (the W001
    // warning already reports this in Sema). Skip them so a qualifying-loop
    // element transfer is not laundered as null.
    if (placeToKeyString(path).find("[]") != string::npos)
      continue;
    setOwnedFieldNull(VD, path);
  }
}

// __assume_null(t) on a struct-value variable (VD in SStatus): drop ownership
// of every registered owned field of the struct value, mirroring
// assumeAllOwnedFieldsNull for the owned-struct-pointer (OPS) host.
void Ownership::OwnershipStatus::assumeAllSFieldsNull(const VarDecl *VD) {
  if (!SStatus.count(VD))
    return;
  llvm::SmallVector<FieldKey, 16> paths(SAllOwnedFields[VD].begin(),
                                        SAllOwnedFields[VD].end());
  for (FieldKey path : paths) {
    // Array fields carry element-wise ownership; an aggregate __assume_null
    // cannot drop them (W001 already reports this). Skip so a qualifying-loop
    // element transfer is not laundered as null.
    if (placeToKeyString(path).find("[]") != string::npos)
      continue;
    if (SAllOwnedFields[VD].count(path)) {
      SOwnedOwnedFields[VD].erase(path);
      SNullOwnedFields[VD].insert(path);
      SUninitOwnedFields[VD].erase(path);
      auto allPrefixStrs = fieldDescendants(SAllOwnedFields[VD], path);
      for (FieldKey str : allPrefixStrs) {
        SOwnedOwnedFields[VD].erase(str);
        SNullOwnedFields[VD].insert(str);
        SUninitOwnedFields[VD].erase(str);
      }
    }
  }
}

// __assume_null(s->in) / __assume_null(t.in) on a struct-typed field: drop
// ownership of every owned field nested under `fieldPath` (field descendants)
// on whichever host tracks VD — an owned struct pointer (OPS) or a struct
// value (S). `fieldPath` is e.g. Field(Var, "in"); fields under it are
// "in.ip", "in.x.y".
void Ownership::OwnershipStatus::assumeOwnedFieldsUnderNull(
    const VarDecl *VD, FieldKey FieldPath) {
  if (OPSStatus.count(VD)) {
    auto allPrefixStrs = fieldDescendants(OPSAllOwnedFields[VD], FieldPath);
    for (FieldKey str : allPrefixStrs) {
      if (placeToKeyString(str).find("[]") != string::npos)
        continue;
      setOwnedFieldNull(VD, str);
    }
  }
  if (SStatus.count(VD)) {
    auto allPrefixStrs = fieldDescendants(SAllOwnedFields[VD], FieldPath);
    for (FieldKey str : allPrefixStrs) {
      if (placeToKeyString(str).find("[]") != string::npos)
        continue;
      if (SAllOwnedFields[VD].count(str)) {
        SOwnedOwnedFields[VD].erase(str);
        SNullOwnedFields[VD].insert(str);
        SUninitOwnedFields[VD].erase(str);
      }
    }
  }
}

// __forget(*s) on an owned struct pointer: forget every registered owned
// field of the pointee (Moved semantics — the fields are not asserted null,
// they are abandoned). Unlike assumeAllOwnedFieldsNull, array fields
// ("arr[]") are forgotten too: __forget is a promise to abandon the whole
// pointee contents, and there is no Sema warning that could exclude them.
// The pointer itself keeps owning the struct allocation (still must be freed
// or forgotten), so no aggregate bit changes.
void Ownership::OwnershipStatus::forgetAllOwnedFields(
    const VarDecl *VD) {
  if (!OPSStatus.count(VD))
    return;
  OPSOwnedOwnedFields[VD].clear();
  OPSNullOwnedFields[VD].clear();
}

// __forget(t) on a struct-value variable (VD in SStatus): forget every
// registered owned field of the struct value, mirroring
// forgetAllOwnedFields for the owned-struct-pointer (OPS) host. The
// fields count as explicitly moved (SMovedOwnedFields), so a later field use
// reports use-of-moved like a per-field move-out would.
void Ownership::OwnershipStatus::forgetAllSFields(const VarDecl *VD) {
  if (!SStatus.count(VD))
    return;
  for (FieldKey path : SAllOwnedFields[VD]) {
    SOwnedOwnedFields[VD].erase(path);
    SNullOwnedFields[VD].erase(path);
    SUninitOwnedFields[VD].erase(path);
  }
  SMovedOwnedFields[VD] = SAllOwnedFields[VD];
}

// __forget(s->in) / __forget(t.in) on a struct-typed field: forget every
// owned field nested under `fieldPath` (field descendants) on whichever host
// tracks VD — an owned struct pointer (OPS) or a struct value (S). Mirror of
// assumeOwnedFieldsUnderNull with Moved semantics (no Null insertion; S-host
// fields are marked moved so later uses report use-of-moved).
void Ownership::OwnershipStatus::forgetOwnedFieldsUnderPath(
    const VarDecl *VD, FieldKey FieldPath) {
  if (OPSStatus.count(VD)) {
    auto allPrefixStrs = fieldDescendants(OPSAllOwnedFields[VD], FieldPath);
    for (FieldKey str : allPrefixStrs) {
      OPSOwnedOwnedFields[VD].erase(str);
      OPSNullOwnedFields[VD].erase(str);
    }
  }
  if (SStatus.count(VD)) {
    auto allPrefixStrs = fieldDescendants(SAllOwnedFields[VD], FieldPath);
    for (FieldKey str : allPrefixStrs) {
      SOwnedOwnedFields[VD].erase(str);
      SNullOwnedFields[VD].erase(str);
      SUninitOwnedFields[VD].erase(str);
      markFieldAndDescendantsMoved(SAllOwnedFields[VD], SMovedOwnedFields[VD],
                                   str);
    }
  }
}

void Ownership::OwnershipStatus::setToNull(const Expr *E, ASTContext &Ctx) {
  E = E->IgnoreParenImpCasts();
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
    // __assume_null(t) on a struct-value variable: drop ownership of every owned
    // field of the struct value. (A plain `t = nullptr` is ill-formed for a
    // struct value, so this path is only reached via __assume_null.) Pointer
    // variables fall through to setToNull(VD) below.
    if (E->getType()->isRecordType() && SStatus.count(VD)) {
      assumeAllSFieldsNull(VD);
      return;
    }
    setToNull(VD);
    return;
  }
  // assigning nullptr to an array element (a[i] = nullptr, or
  // s[i].f = nullptr for a struct array member) nulls the whole aggregate:
  // within a qualifying loop every element receives the same assignment.
  if (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    const Expr *Base = ASE->getBase()->IgnoreParenImpCasts();
    while (const ArraySubscriptExpr *Inner = dyn_cast<ArraySubscriptExpr>(Base))
      Base = Inner->getBase()->IgnoreParenImpCasts();
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Base)) {
      if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        if (IsOwnedElementArrayType(VD->getType()))
          setArrayElemNull(VD);
        return;
      }
    }
    // Struct-field array element: w.arr[i] (base is a member access). Peel
    // the host and the "arr[]" path and null the field aggregate.
    if (dyn_cast<MemberExpr>(Base)) {
      bsc::PlaceBuilder PB(Ctx);
      const bsc::Place *FieldPath = nullptr;
      if (PeelHostAndFieldPlace(PB, E, FieldPath) &&
          IsOwnedArrayFieldPath(FieldPath))
        setArrayFieldNull(FieldPath->getRoot(), FieldPath);
      return;
    }
  }
  if (const MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
    // Struct-array member: s[i].f = nullptr (base is an ArraySubscriptExpr).
    FieldKey FieldPlace = getMemberFieldPlace(ME, Ctx);
    const Expr *Base = ME->getBase()->IgnoreParenImpCasts();
    while (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(Base))
      Base = ASE->getBase()->IgnoreParenImpCasts();
    if (const DeclRefExpr *BaseDRE = dyn_cast<DeclRefExpr>(Base)) {
      if (const VarDecl *VD = dyn_cast<VarDecl>(BaseDRE->getDecl())) {
        if (IsOwnedElementArrayType(VD->getType())) {
          // The historical key was the bare member-name chain (getMemberFullField,
          // e.g. "f" for s[i].f): build it from the host-relative path, since
          // getMemberFieldPlace cannot resolve the array-subscript base.
          bsc::PlaceBuilder PB(Ctx);
          const bsc::Place *SArrFieldPath = nullptr;
          if (PeelHostAndFieldPlace(PB, ME, SArrFieldPath) &&
              SArrFieldPath->getRoot() == VD)
            setArrayFieldNull(VD, SArrFieldPath);
          return;
        }
      }
    }
    {
      bsc::PlaceBuilder PB(Ctx);
      const bsc::Place *FieldPath = nullptr;
      if (PeelHostAndFieldPlace(PB, E, FieldPath) &&
          IsOwnedArrayFieldPath(FieldPath)) {
        setArrayFieldNull(FieldPath->getRoot(), FieldPath);
        return;
      }
    }
    // Resolve `q->p` (base is an LValueToRValue ImplicitCastExpr around `q`)
    // and `(*q).p` to the root variable's field place, like `s.p` for a
    // struct value.
    if (FieldPlace) {
      const VarDecl *VD = FieldPlace->getRoot();
      // __assume_null(s->in) / __assume_null(t.in) where the member is a
      // (sub-)struct: drop ownership of every owned field nested under it on
      // whichever host tracks VD. (A plain `s->in = nullptr` / `t.in = nullptr`
      // is ill-formed for a struct member, so this path is only reached via
      // __assume_null.) Pointer-typed members fall through to the single-field
      // drop below.
      if (E->getType()->isRecordType()) {
        assumeOwnedFieldsUnderNull(VD, FieldPlace);
        return;
      }
      if (OPSStatus.count(VD))
        setOwnedFieldNull(VD, FieldPlace);
      if (SStatus.count(VD)) {
        if (SAllOwnedFields[VD].count(FieldPlace)) {
          SOwnedOwnedFields[VD].erase(FieldPlace);
          SNullOwnedFields[VD].insert(FieldPlace);
          SUninitOwnedFields[VD].erase(FieldPlace);
          eraseFieldAndDescendants(SMovedOwnedFields[VD], FieldPlace);
          auto allPrefixStrs = fieldDescendants(SAllOwnedFields[VD], FieldPlace);
          for (FieldKey str : allPrefixStrs) {
            SOwnedOwnedFields[VD].erase(str);
            SNullOwnedFields[VD].insert(str);
            SUninitOwnedFields[VD].erase(str);
          }
        }
      }
    }
  }
  if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(E)) {
    // Build the deref-chain place over the root variable: **q is
    // Deref(Deref(Var(q))), the historical "suffix" of trailing '*' markers.
    // First pass: peel casts and derefs down to the root variable, collecting
    // each deref's type outermost-first.
    llvm::SmallVector<QualType, 4> DerefTys;
    const Expr *e = UO;
    const VarDecl *VD = nullptr;
    while (true) {
      if (const ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(e)) {
        e = ICE->getSubExpr();
      } else if (const UnaryOperator *uo = dyn_cast<UnaryOperator>(e)) {
        if (uo->getOpcode() != UO_Deref)
          break;
        DerefTys.push_back(uo->getType());
        e = uo->getSubExpr()->IgnoreParenImpCasts();
      } else {
        break;
      }
    }
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(e))
      VD = dyn_cast<VarDecl>(DRE->getDecl());
    FieldKey DerefPlace = nullptr;
    if (VD && !DerefTys.empty()) {
      bsc::PlaceBuilder PB(Ctx);
      DerefPlace = PB.Build(VD, VD->getLocation());
      for (QualType QualTy : DerefTys)
        DerefPlace = PB.BuildDeref(DerefPlace, QualTy, UO->getExprLoc());
    }
    if (DerefPlace) {
      const VarDecl *RootVD = DerefPlace->getRoot();
      // __assume_null(*s) on an owned struct pointer (`struct S *_Owned s`):
      // drop ownership of every _Nullable owned field. Sema has already
      // rejected structs with a _Nonnull field, so every trackable owned field
      // here is _Nullable.
      if (DerefPlace->getBase()->getKind() == bsc::Place::Kind::Var &&
          OPSStatus.count(RootVD)) {
        QualType PointeeTy = RootVD->getType()->getPointeeType();
        if (PointeeTy->isRecordType())
          assumeAllOwnedFieldsNull(RootVD);
      }
      if (BOPStatus.count(RootVD)) {
        if (BOPAllOwnedFields[RootVD].count(DerefPlace)) {
          BOPOwnedOwnedFields[RootVD].erase(DerefPlace);
          auto allPrefixStrs = derefDescendants(BOPAllOwnedFields[RootVD], DerefPlace);
          for (FieldKey str : allPrefixStrs) {
            BOPOwnedOwnedFields[RootVD].erase(str);
          }
          if (BOPAllOwnedFields[RootVD].size() !=
              BOPOwnedOwnedFields[RootVD].size()) {
            if (!is(RootVD, Moved)) {
              resetAll(RootVD);
              set(RootVD, PartialMoved);
            }
          }
          if (BOPOwnedOwnedFields[RootVD].empty()) {
            if (!is(RootVD, Moved)) {
              resetAll(RootVD);
              set(RootVD, AllMoved);
            }
          }
        }
      }
    }
    // __assume_null(*h->sp) / __assume_null(*t.sp): the field form of
    // __assume_null(*s) above — deref of an owned struct-pointer field. Drop
    // ownership of the pointee's owned fields (the field descendants of the
    // member place on the host variable) by assuming them null; the field
    // itself keeps owning the pointee allocation.
    if (DerefTys.size() == 1) {
      if (const MemberExpr *ME = dyn_cast<MemberExpr>(e)) {
        QualType PointeeTy = ME->getType()->getPointeeType();
        if (PointeeTy->isRecordType()) {
          FieldKey FieldPlace = getMemberFieldPlace(ME, Ctx);
          if (FieldPlace)
            assumeOwnedFieldsUnderNull(FieldPlace->getRoot(), FieldPlace);
        }
      }
    }
  }
}

void Ownership::OwnershipStatus::setToMoved(const VarDecl *VD) {
  if (OPSStatus.count(VD)) {
    OPSOwnedOwnedFields[VD].clear();
    resetAll(VD);
    set(VD, Ownership::Status::Moved);
  }
  if (BOPStatus.count(VD)) {
    BOPOwnedOwnedFields[VD].clear();
    resetAll(VD);
    set(VD, Ownership::Status::Moved);
  }
}

void Ownership::OwnershipStatus::setToMoved(const Expr *E, ASTContext &Ctx) {
  E = E->IgnoreParenImpCasts();
  // A plain owned-pointer variable (e.g. int *_Owned p): drop ownership by
  // moving it out wholesale — no leak, no double-free, later use reports
  // use-of-moved. A struct-value variable (struct S t with _Owned fields, or
  // an owned struct) owns nothing itself: forget every owned field instead.
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
    if (E->getType()->isRecordType() && SStatus.count(VD)) {
      forgetAllSFields(VD);
      return;
    }
    setToMoved(VD);
    return;
  }
  // An owned-element array element (a[i], w.arr[i]): the array is one
  // aggregate and the user explicitly promises to abandon the element, so
  // apply the aggregate Moved transition (the qualifying-loop gate lives in
  // the caller, TransferFunctions::isArrayElemForgetAllowed).
  if (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
    const Expr *Base = ASE->getBase()->IgnoreParenImpCasts();
    while (const ArraySubscriptExpr *Inner = dyn_cast<ArraySubscriptExpr>(Base))
      Base = Inner->getBase()->IgnoreParenImpCasts();
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Base)) {
      if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        if (IsOwnedElementArrayType(VD->getType()))
          setArrayElemMoved(VD);
        return;
      }
    }
    // Struct-field array element: w.arr[i] (base is a member access). Peel
    // the host and the "arr[]" path and move the field aggregate out.
    if (dyn_cast<MemberExpr>(Base)) {
      bsc::PlaceBuilder PB(Ctx);
      const bsc::Place *FieldPath = nullptr;
      if (PeelHostAndFieldPlace(PB, E, FieldPath) &&
          IsOwnedArrayFieldPath(FieldPath))
        setArrayFieldMoved(FieldPath->getRoot(), FieldPath);
      return;
    }
  }
  // A struct owned field (e.g. s.p, s->p, (*s).p): drop the field's ownership
  // by moving it out, mirroring the field move-out performed when the field is
  // freed via `(void *_Owned)s.p`. Erase the field path (and its sub-field
  // prefixes) from the owned-field sets so no leak is reported at scope end;
  // unlike setToNull, do not insert into the Null-owned-field sets (the
  // pointer is not asserted null, its ownership is simply forgotten).
  if (const MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
    // Struct-array member: __forget(s[i].f) (base is an ArraySubscriptExpr on
    // a local struct array): move the field aggregate of every element out.
    // The historical key was the bare member-name chain, so build the place
    // from the host-relative path (getMemberFieldPlace cannot resolve the
    // array-subscript base).
    const Expr *Base = ME->getBase()->IgnoreParenImpCasts();
    while (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(Base))
      Base = ASE->getBase()->IgnoreParenImpCasts();
    if (const DeclRefExpr *BaseDRE = dyn_cast<DeclRefExpr>(Base)) {
      if (const VarDecl *VD = dyn_cast<VarDecl>(BaseDRE->getDecl())) {
        if (IsOwnedElementArrayType(VD->getType())) {
          bsc::PlaceBuilder PB(Ctx);
          const bsc::Place *SArrFieldPath = nullptr;
          if (PeelHostAndFieldPlace(PB, ME, SArrFieldPath) &&
              SArrFieldPath->getRoot() == VD)
            setArrayFieldMoved(VD, SArrFieldPath);
          return;
        }
      }
    }
    // Struct-field array member: __forget(w.arr[i].f) — an array-field path
    // of a host aggregate; move the field aggregate out (mirror of the
    // VisitMemberExpr move-out path).
    {
      bsc::PlaceBuilder PB(Ctx);
      const bsc::Place *FieldPath = nullptr;
      if (PeelHostAndFieldPlace(PB, ME, FieldPath) &&
          IsOwnedArrayFieldPath(FieldPath)) {
        setArrayFieldMoved(FieldPath->getRoot(), FieldPath);
        return;
      }
    }
    FieldKey FieldPlace = getMemberFieldPlace(ME, Ctx);
    if (FieldPlace) {
      const VarDecl *VD = FieldPlace->getRoot();
      // __forget(s->in) / __forget(t.in) where the member is a (sub-)struct:
      // forget every owned field nested under it on whichever host tracks VD.
      // Pointer-typed members fall through to the single-field drop below.
      if (E->getType()->isRecordType()) {
        forgetOwnedFieldsUnderPath(VD, FieldPlace);
        return;
      }
      if (OPSStatus.count(VD)) {
        if (OPSAllOwnedFields[VD].count(FieldPlace)) {
          OPSOwnedOwnedFields[VD].erase(FieldPlace);
          OPSNullOwnedFields[VD].erase(FieldPlace);
          auto allPrefixStrs = fieldDescendants(OPSAllOwnedFields[VD], FieldPlace);
          for (FieldKey str : allPrefixStrs) {
            OPSOwnedOwnedFields[VD].erase(str);
            OPSNullOwnedFields[VD].erase(str);
          }
        }
      }
      if (SStatus.count(VD)) {
        if (SAllOwnedFields[VD].count(FieldPlace)) {
          SOwnedOwnedFields[VD].erase(FieldPlace);
          SNullOwnedFields[VD].erase(FieldPlace);
          SUninitOwnedFields[VD].erase(FieldPlace);
          markFieldAndDescendantsMoved(
              SAllOwnedFields[VD], SMovedOwnedFields[VD], FieldPlace);
          auto allPrefixStrs = fieldDescendants(SAllOwnedFields[VD], FieldPlace);
          for (FieldKey str : allPrefixStrs) {
            SOwnedOwnedFields[VD].erase(str);
            SNullOwnedFields[VD].erase(str);
            SUninitOwnedFields[VD].erase(str);
          }
        }
      }
    }
  }
  // A deref chain: __forget(*s) on an owned struct pointer forgets every
  // owned field of the pointee (the pointer itself keeps owning the struct
  // allocation — it must still be freed or forgotten, so no aggregate bit
  // changes); __forget(*q) on a pointer-to-owned-pointer (int *_Owned *q)
  // forgets the inner owned pointer tracked as the "*" field of the BOP host.
  if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(E)) {
    // Build the deref-chain place over the root variable: **q is
    // Deref(Deref(Var(q))), the historical "suffix" of trailing '*' markers.
    // First pass: peel casts and derefs down to the root variable, collecting
    // each deref's type outermost-first.
    llvm::SmallVector<QualType, 4> DerefTys;
    const Expr *e = UO;
    const VarDecl *VD = nullptr;
    while (true) {
      if (const ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(e)) {
        e = ICE->getSubExpr();
      } else if (const UnaryOperator *uo = dyn_cast<UnaryOperator>(e)) {
        if (uo->getOpcode() != UO_Deref)
          break;
        DerefTys.push_back(uo->getType());
        e = uo->getSubExpr()->IgnoreParenImpCasts();
      } else {
        break;
      }
    }
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(e))
      VD = dyn_cast<VarDecl>(DRE->getDecl());
    FieldKey DerefPlace = nullptr;
    if (VD && !DerefTys.empty()) {
      bsc::PlaceBuilder PB(Ctx);
      DerefPlace = PB.Build(VD, VD->getLocation());
      for (QualType QualTy : DerefTys)
        DerefPlace = PB.BuildDeref(DerefPlace, QualTy, UO->getExprLoc());
    }
    if (DerefPlace) {
      const VarDecl *RootVD = DerefPlace->getRoot();
      if (DerefPlace->getBase()->getKind() == bsc::Place::Kind::Var &&
          OPSStatus.count(RootVD)) {
        QualType PointeeTy = RootVD->getType()->getPointeeType();
        if (PointeeTy->isRecordType())
          forgetAllOwnedFields(RootVD);
      }
      if (BOPStatus.count(RootVD)) {
        if (BOPAllOwnedFields[RootVD].count(DerefPlace)) {
          BOPOwnedOwnedFields[RootVD].erase(DerefPlace);
          auto allPrefixStrs =
              derefDescendants(BOPAllOwnedFields[RootVD], DerefPlace);
          for (FieldKey str : allPrefixStrs) {
            BOPOwnedOwnedFields[RootVD].erase(str);
          }
          if (BOPAllOwnedFields[RootVD].size() !=
              BOPOwnedOwnedFields[RootVD].size()) {
            if (!is(RootVD, Moved)) {
              resetAll(RootVD);
              set(RootVD, PartialMoved);
            }
          }
          if (BOPOwnedOwnedFields[RootVD].empty()) {
            if (!is(RootVD, Moved)) {
              resetAll(RootVD);
              set(RootVD, AllMoved);
            }
          }
        }
      }
    }
    // __forget(*h->sp) / __forget(*t.sp): the field form of __forget(*s)
    // above — deref of an owned struct-pointer field. The pointee's owned
    // fields are the field descendants of the member place on the host
    // variable; forget them while the field itself keeps owning the pointee
    // allocation (it must still be freed or moved, so no aggregate bit
    // changes).
    if (DerefTys.size() == 1) {
      if (const MemberExpr *ME = dyn_cast<MemberExpr>(e)) {
        QualType PointeeTy = ME->getType()->getPointeeType();
        if (PointeeTy->isRecordType()) {
          FieldKey FieldPlace = getMemberFieldPlace(ME, Ctx);
          if (FieldPlace)
            forgetOwnedFieldsUnderPath(FieldPlace->getRoot(), FieldPlace);
        }
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// Whole-array / struct-field transitions for owned-element arrays.
//===----------------------------------------------------------------------===//

void Ownership::OwnershipStatus::setArrayElemMoved(const VarDecl *VD) {
  if (BOPStatus.count(VD)) {
    // Basic owned-pointer arrays, e.g. int *_Owned a[N]: every element was
    // moved out (freed / passed / returned), the aggregate no longer owns.
    resetAll(VD);
    set(VD, Ownership::Status::Moved);
  }
  if (SStatus.count(VD)) {
    // Struct arrays, e.g. struct S a[N] with _Owned members: treat every
    // tracked field of every element as moved out.
    for (FieldKey field : SAllOwnedFields[VD])
      SOwnedOwnedFields[VD].erase(field);
    SMovedOwnedFields[VD] = SAllOwnedFields[VD];
    resetAll(VD);
    set(VD, Ownership::Status::AllMoved);
  }
}

void Ownership::OwnershipStatus::setArrayElemOwned(const VarDecl *VD) {
  if (BOPStatus.count(VD)) {
    resetAll(VD);
    set(VD, Ownership::Status::Owned);
    BOPOwnedOwnedFields[VD] = BOPAllOwnedFields[VD];
  }
  if (SStatus.count(VD)) {
    setToOwned(VD);
  }
}

void Ownership::OwnershipStatus::setArrayElemNull(const VarDecl *VD) {
  if (BOPStatus.count(VD)) {
    BOPOwnedOwnedFields[VD].clear();
    resetAll(VD);
    set(VD, Ownership::Status::Null);
  }
  if (SStatus.count(VD)) {
    SOwnedOwnedFields[VD].clear();
    SMovedOwnedFields[VD].clear();
    if (SNullOwnedFields.count(VD))
      SNullOwnedFields[VD] = SAllOwnedFields[VD];
    resetAll(VD);
    set(VD, Ownership::Status::AllMoved);
  }
}

void Ownership::OwnershipStatus::setArrayFieldMoved(const VarDecl *VD,
                                                    FieldKey fieldName) {
  if (SStatus.count(VD)) {
    if (!SAllOwnedFields[VD].count(fieldName))
      return;
    SOwnedOwnedFields[VD].erase(fieldName);
    SNullOwnedFields[VD].erase(fieldName);
    markFieldAndDescendantsMoved(SAllOwnedFields[VD], SMovedOwnedFields[VD],
                                 fieldName);
    for (FieldKey str : fieldDescendants(SAllOwnedFields[VD], fieldName)) {
      SOwnedOwnedFields[VD].erase(str);
      SNullOwnedFields[VD].erase(str);
    }
    refreshArrayFieldState(VD, SOwnedOwnedFields[VD], SAllOwnedFields[VD]);
  }
  // Owned struct-pointer host (`struct S *_Owned p`): the array field of the
  // pointee is tracked with the OPS field sets.
  if (OPSStatus.count(VD)) {
    if (!OPSAllOwnedFields[VD].count(fieldName))
      return;
    OPSOwnedOwnedFields[VD].erase(fieldName);
    OPSNullOwnedFields[VD].erase(fieldName);
    markFieldAndDescendantsMoved(OPSAllOwnedFields[VD],
                                 OPSMovedOwnedFields[VD], fieldName);
    for (FieldKey str : fieldDescendants(OPSAllOwnedFields[VD], fieldName)) {
      OPSOwnedOwnedFields[VD].erase(str);
      OPSNullOwnedFields[VD].erase(str);
    }
    refreshArrayFieldState(VD, OPSOwnedOwnedFields[VD],
                           OPSAllOwnedFields[VD]);
  }
}

void Ownership::OwnershipStatus::setArrayFieldOwned(const VarDecl *VD,
                                                    FieldKey fieldName) {
  if (SStatus.count(VD)) {
    bool Found = false;
    if (SAllOwnedFields[VD].count(fieldName)) {
      SOwnedOwnedFields[VD].insert(fieldName);
      SNullOwnedFields[VD].erase(fieldName);
      SMovedOwnedFields[VD].erase(fieldName);
      Found = true;
    }
    // A mutable borrow of an aggregate field path (e.g. "arr[]" or "inner")
    // may write to any tracked descendant field through the borrow, so promote
    // all of them to owned (conservative: it may now own values).
    for (FieldKey F : fieldDescendants(SAllOwnedFields[VD], fieldName)) {
      SOwnedOwnedFields[VD].insert(F);
      SNullOwnedFields[VD].erase(F);
      SMovedOwnedFields[VD].erase(F);
      Found = true;
    }
    if (!Found)
      return;
    refreshArrayFieldState(VD, SOwnedOwnedFields[VD], SAllOwnedFields[VD]);
  }
  if (OPSStatus.count(VD)) {
    bool Found = false;
    if (OPSAllOwnedFields[VD].count(fieldName)) {
      OPSOwnedOwnedFields[VD].insert(fieldName);
      OPSNullOwnedFields[VD].erase(fieldName);
      OPSMovedOwnedFields[VD].erase(fieldName);
      Found = true;
    }
    for (FieldKey F : fieldDescendants(OPSAllOwnedFields[VD], fieldName)) {
      OPSOwnedOwnedFields[VD].insert(F);
      OPSNullOwnedFields[VD].erase(F);
      OPSMovedOwnedFields[VD].erase(F);
      Found = true;
    }
    if (!Found)
      return;
    refreshArrayFieldState(VD, OPSOwnedOwnedFields[VD], OPSAllOwnedFields[VD]);
  }
}

void Ownership::OwnershipStatus::setArrayFieldNull(const VarDecl *VD,
                                                   FieldKey fieldName) {
  if (SStatus.count(VD)) {
    if (!SAllOwnedFields[VD].count(fieldName))
      return;
    SOwnedOwnedFields[VD].erase(fieldName);
    SNullOwnedFields[VD].insert(fieldName);
    eraseFieldAndDescendants(SMovedOwnedFields[VD], fieldName);
    refreshArrayFieldState(VD, SOwnedOwnedFields[VD], SAllOwnedFields[VD]);
  }
  if (OPSStatus.count(VD)) {
    if (!OPSAllOwnedFields[VD].count(fieldName))
      return;
    OPSOwnedOwnedFields[VD].erase(fieldName);
    OPSNullOwnedFields[VD].insert(fieldName);
    eraseFieldAndDescendants(OPSMovedOwnedFields[VD], fieldName);
    refreshArrayFieldState(VD, OPSOwnedOwnedFields[VD], OPSAllOwnedFields[VD]);
  }
}


bool Ownership::OwnershipStatus::arrayAggregateUninit(const VarDecl *VD) const {
  if (BOPStatus.count(VD) || SStatus.count(VD) || OPSStatus.count(VD))
    return is(VD, Ownership::Status::Uninitialized);
  return false;
}

bool Ownership::OwnershipStatus::arrayAggregateNull(
    const VarDecl *VD, FieldKey fieldName) const {
  // A Var-kind (root) place is the whole-aggregate query, the historical "".
  if (!fieldName || fieldName->getKind() == bsc::Place::Kind::Var) {
    if (BOPStatus.count(VD))
      return is(VD, Ownership::Status::Null);
    if (SStatus.count(VD))
      return SNullOwnedFields.lookup(VD).size() ==
             SAllOwnedFields.lookup(VD).size();
    if (OPSStatus.count(VD))
      return OPSNullOwnedFields.lookup(VD).size() ==
             OPSAllOwnedFields.lookup(VD).size();
    return false;
  }
  if (SStatus.count(VD)) {
    const auto &Null = SNullOwnedFields.lookup(VD);
    if (Null.count(fieldName))
      return true;
    // Aggregate prefix (e.g. "arr[]"): the field aggregate is null when every
    // tracked descendant field (e.g. "arr[].q") is null. This keeps an
    // all-null struct-element array field treated like a null-owned pointer
    // field (moving a null element out is a no-op).
    const auto &All = SAllOwnedFields.lookup(VD);
    bool HasTracked = false;
    for (FieldKey Key : All) {
      if (isFieldPathPrefix(fieldName, Key) ||
          isFieldPathPrefix(Key, fieldName)) {
        HasTracked = true;
        if (!Null.count(Key))
          return false;
      }
    }
    return HasTracked;
  }
  if (OPSStatus.count(VD)) {
    const auto &Null = OPSNullOwnedFields.lookup(VD);
    if (Null.count(fieldName))
      return true;
    const auto &All = OPSAllOwnedFields.lookup(VD);
    bool HasTracked = false;
    for (FieldKey Key : All) {
      if (isFieldPathPrefix(fieldName, Key) ||
          isFieldPathPrefix(Key, fieldName)) {
        HasTracked = true;
        if (!Null.count(Key))
          return false;
      }
    }
    return HasTracked;
  }
  return false;
}

// Whether a null clear of the aggregate / field drops nothing: no path of it can
// still hold an owning value. Clearing such an aggregate is a no-op (it simply
// also becomes null, e.g. Moved becomes Moved|Null), while clearing one that may
// still own something is an ownership transfer and stays gated by the
// qualifying-loop rules. A partial move keeps the remaining elements owned, so
// PartialMoved is treated as holding something.
bool Ownership::OwnershipStatus::arrayAggregateNotOwned(
    const VarDecl *VD, FieldKey fieldName) const {
  if (!fieldName || fieldName->getKind() == bsc::Place::Kind::Var) {
    if (BOPStatus.count(VD))
      return !isOrHas(VD, Ownership::Status::Owned) &&
             !isOrHas(VD, Ownership::Status::PartialMoved);
    if (SStatus.count(VD))
      return SOwnedOwnedFields.lookup(VD).empty();
    if (OPSStatus.count(VD))
      return OPSOwnedOwnedFields.lookup(VD).empty();
    return false;
  }
  auto PathNotOwned = [&](const FieldSet &Owned) {
    if (Owned.count(fieldName))
      return false;
    for (FieldKey Key : Owned) {
      if (isFieldPathPrefix(fieldName, Key) ||
          isFieldPathPrefix(Key, fieldName))
        return false;
    }
    return true;
  };
  if (SStatus.count(VD))
    return PathNotOwned(SOwnedOwnedFields.lookup(VD));
  if (OPSStatus.count(VD))
    return PathNotOwned(OPSOwnedOwnedFields.lookup(VD));
  return false;
}

// The forbidden loop-end state of an owned-element array: the element possibly
// owns a value on some path and was moved out on another (Owned|Moved). A path
// that leaves the element null is compatible with both, because a null element
// owns nothing: Owned|Null and Moved|Null are stable states and may be released
// directly or after a null check. Null alone owns nothing.
bool Ownership::OwnershipStatus::arrayElemPathInconsistent(
    const VarDecl *VD) const {
  // Basic owned-pointer element arrays: the aggregate bits carry the state.
  if (BOPStatus.count(VD))
    return isOrHas(VD, Ownership::Status::Owned) &&
           isOrHas(VD, Ownership::Status::Moved);
  // Struct-element / owned-struct-pointer-host field arrays: a tracked field
  // moved out on one path but still owned on another. The moved field sets are
  // unions over the paths, so this distinguishes Owned|Null (allowed) from
  // Owned|Moved (rejected).
  auto FindSet = [](const auto &Map, const VarDecl *VD) -> const FieldSet * {
    auto It = Map.find(VD);
    return It == Map.end() ? nullptr : &It->second;
  };
  const FieldSet *All = nullptr;
  const FieldSet *Owned = nullptr;
  const FieldSet *Moved = nullptr;
  if (SStatus.count(VD)) {
    All = FindSet(SAllOwnedFields, VD);
    Owned = FindSet(SOwnedOwnedFields, VD);
    Moved = FindSet(SMovedOwnedFields, VD);
  } else if (OPSStatus.count(VD)) {
    All = FindSet(OPSAllOwnedFields, VD);
    Owned = FindSet(OPSOwnedOwnedFields, VD);
    Moved = FindSet(OPSMovedOwnedFields, VD);
  } else {
    return false;
  }
  if (!All || !Owned || !Moved)
    return false;
  for (FieldKey Field : *All)
    if (Moved->count(Field) && Owned->count(Field))
      return true;
  return false;
}

bool Ownership::OwnershipStatus::arrayElemPossiblyMoved(
    const VarDecl *VD) const {
  // The Moved component present: exactly Moved, or moved on some path only
  // (Owned|Moved from a partial release, Moved|Null from a null-checked one).
  if (BOPStatus.count(VD))
    return isOrHas(VD, Ownership::Status::Moved);
  if (SStatus.count(VD))
    return !SMovedOwnedFields.lookup(VD).empty();
  if (OPSStatus.count(VD))
    return !OPSMovedOwnedFields.lookup(VD).empty();
  return false;
}

bool Ownership::OwnershipStatus::arrayElemNullNoop(const VarDecl *VD,
                                                   FieldKey field) const {
  if (!arrayAggregateNull(VD, field))
    return false;
  // Basic owned-pointer arrays: arrayAggregateNull is the exact Null bit, so a
  // Moved|Null aggregate (Null + Moved bits) is already excluded here.
  if (!field || field->getKind() == bsc::Place::Kind::Var)
    return true;
  // Field-level: the null field set is a union over the paths, so a field that
  // was also moved on some path (Moved|Null) is a double free, not a no-op.
  auto FindSet = [](const auto &Map, const VarDecl *VD) -> const FieldSet * {
    auto It = Map.find(VD);
    return It == Map.end() ? nullptr : &It->second;
  };
  const FieldSet *Moved = nullptr;
  if (SStatus.count(VD))
    Moved = FindSet(SMovedOwnedFields, VD);
  else if (OPSStatus.count(VD))
    Moved = FindSet(OPSMovedOwnedFields, VD);
  if (!Moved)
    return true;
  // The field itself or any tracked path below it (a moved "arr[].p" also means
  // the enclosing "arr[]" element field was moved on some path).
  return !Moved->count(field) && fieldDescendants(*Moved, field).empty();
}

void Ownership::OwnershipStatus::refreshArrayFieldState(
    const VarDecl *VD, const FieldSet &Owned, const FieldSet &All) {
  resetAll(VD);
  if (Owned.empty()) {
    if (All.empty())
      set(VD, Ownership::Status::Owned);
    else
      set(VD, Ownership::Status::AllMoved);
  } else if (Owned.size() < All.size()) {
    set(VD, Ownership::Status::PartialMoved);
  } else {
    set(VD, Ownership::Status::Owned);
  }
}

void Ownership::OwnershipStatus::removeArraysOne(const VarDecl *VD) {
  OPSStatus.erase(VD);
  OPSAllOwnedFields.erase(VD);
  OPSOwnedOwnedFields.erase(VD);
  OPSNullOwnedFields.erase(VD);
  OPSMovedOwnedFields.erase(VD);
  SStatus.erase(VD);
  SAllOwnedFields.erase(VD);
  SOwnedOwnedFields.erase(VD);
  SNullOwnedFields.erase(VD);
  SUninitOwnedFields.erase(VD);
  SMovedOwnedFields.erase(VD);
  BOPStatus.erase(VD);
  BOPAllOwnedFields.erase(VD);
  BOPOwnedOwnedFields.erase(VD);
}

void Ownership::OwnershipStatus::removeArrays(
    llvm::ArrayRef<const VarDecl *> Arrays) {
  for (const VarDecl *VD : Arrays)
    removeArraysOne(VD);
}

void Ownership::OwnershipStatus::takeArraysFrom(
    const OwnershipStatus &Other, llvm::ArrayRef<const VarDecl *> Arrays) {
  for (const VarDecl *VD : Arrays) {
    removeArraysOne(VD);
    if (Other.OPSStatus.count(VD)) {
      OPSStatus[VD] = Other.OPSStatus.lookup(VD);
      OPSAllOwnedFields[VD] = Other.OPSAllOwnedFields.lookup(VD);
      OPSOwnedOwnedFields[VD] = Other.OPSOwnedOwnedFields.lookup(VD);
      OPSNullOwnedFields[VD] = Other.OPSNullOwnedFields.lookup(VD);
      OPSMovedOwnedFields[VD] = Other.OPSMovedOwnedFields.lookup(VD);
    }
    if (Other.SStatus.count(VD)) {
      SStatus[VD] = Other.SStatus.lookup(VD);
      SAllOwnedFields[VD] = Other.SAllOwnedFields.lookup(VD);
      SOwnedOwnedFields[VD] = Other.SOwnedOwnedFields.lookup(VD);
      SNullOwnedFields[VD] = Other.SNullOwnedFields.lookup(VD);
      SUninitOwnedFields[VD] = Other.SUninitOwnedFields.lookup(VD);
      SMovedOwnedFields[VD] = Other.SMovedOwnedFields.lookup(VD);
    }
    if (Other.BOPStatus.count(VD)) {
      BOPStatus[VD] = Other.BOPStatus.lookup(VD);
      BOPAllOwnedFields[VD] = Other.BOPAllOwnedFields.lookup(VD);
      BOPOwnedOwnedFields[VD] = Other.BOPOwnedOwnedFields.lookup(VD);
    }
  }
}

string Ownership::OwnershipStatus::collectMovedFields(const VarDecl *VD) {
  const FieldSet *allFields = nullptr;
  const FieldSet *ownedFields = nullptr;
  const FieldSet *explicitMovedFields = nullptr;
  if (OPSStatus.count(VD)) {
    allFields = &OPSAllOwnedFields[VD];
    ownedFields = &OPSOwnedOwnedFields[VD];
  } else if (SStatus.count(VD)) {
    allFields = &SAllOwnedFields[VD];
    ownedFields = &SOwnedOwnedFields[VD];
    explicitMovedFields = &SMovedOwnedFields[VD];
  } else if (BOPStatus.count(VD)) {
    allFields = &BOPAllOwnedFields[VD];
    ownedFields = &BOPOwnedOwnedFields[VD];
  }
  string movedFields = "";
  if (!allFields || !ownedFields)
    return movedFields + " is";
  // Iteration follows the FieldSet order contract (the historical
  // SmallSet iteration). No 8-element cap here (the historical
  // collectMovedFields listed every moved field).
  int count = 0;
  for (FieldKey P : *allFields) {
    // A field moved on some branch is reported as moved even if the merge
    // restored its ownership (may-own preserved for leak checks only).
    if (explicitMovedFields && explicitMovedFields->count(P)) {
      // fall through: reported as moved
    } else if (ownedFields->count(P)) {
      continue;
    }
    const string element = placeToKeyString(P);
    if (count++ != 0) {
      movedFields += ", ";
    }
    if (VD->getType()->isPointerType() &&
        !VD->getType()->getPointeeType().getCanonicalType()->isRecordType()) {
      movedFields = movedFields + element + VD->getNameAsString();
    } else {
      movedFields = movedFields + moveAsterisksToFront(VD->getNameAsString() +
                                                       "." + element);
    }
  }
  if (count > 1) {
    movedFields += " are";
  } else {
    movedFields += " is";
  }
  return movedFields;
}

SmallVector<OwnershipDiagInfo> Ownership::OwnershipStatus::checkOPSUse(
    const VarDecl *VD, const SourceLocation &Loc, bool isGetAddr, bool isStar, bool isAddrMut) {
  SmallVector<OwnershipDiagInfo> diags;

  // when use ops, we must ensure the variable is owned

  // check the status of the variable
  if (!is(VD, Ownership::Status::Owned)) {
    if (isOrHas(VD, Ownership::Status::Moved)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfMoved, VD->getNameAsString()));
    } else if (is(VD, Ownership::Status::Uninitialized)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfUninit, VD->getNameAsString()));
    } else if (has(VD, Ownership::Status::Uninitialized)) {
      diags.push_back(
          OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfPossiblyUninit,
                            VD->getNameAsString()));
    } else if (is(VD, Ownership::Status::PartialMoved)) {
      diags.push_back(
          OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfPartiallyMoved,
                            VD->getNameAsString(), collectMovedFields(VD)));
    } else if (is(VD, Ownership::Status::AllMoved)) {
      // Allow consuming the whole pointer when every owned field was dropped
      // via __assume_null (sits in NullOwned) rather than genuinely moved out.
      // Such fields are null/handled, not moved, so moving the struct pointer
      // is safe; only report AllMoved when some field is neither owned nor
      // assumed-null.
      bool allFieldsAssumedNull = !OPSAllOwnedFields[VD].empty();
      for (FieldKey f : OPSAllOwnedFields[VD])
        if (!OPSNullOwnedFields[VD].count(f)) {
          allFieldsAssumedNull = false;
          break;
        }
      if (!allFieldsAssumedNull)
        diags.push_back(
            OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfAllMoved,
                              VD->getNameAsString(), collectMovedFields(VD)));
    }
  }
  if (isAddrMut) {
    if (is(VD, Ownership::Status::Null)) {
      setToOwned(VD);
    }
  }
  if (!isGetAddr) {
    // change the status to moved
    if (!is(VD, Ownership::Status::Null)) {
      OPSOwnedOwnedFields[VD].clear();
      resetAll(VD);
      if (!isStar) {
        set(VD, Ownership::Status::Moved);
      } else {
        set(VD, AllMoved);
      }
    }
  }
  return diags;
}

SmallVector<OwnershipDiagInfo> Ownership::OwnershipStatus::checkOPSFieldUse(
    const VarDecl *VD, const SourceLocation &Loc, FieldKey fullFieldName,
    bool isGetAddr, bool IsAggregateAccess) {
  SmallVector<OwnershipDiagInfo> diags;

  // when assign a field, we check three conditions:
  // 1. the field's parent must be owned
  // 2. the status of VD must not be moved or uninit
  // 3. the field and the field's subfields must be all moved

  // check condition 1
  std::string MovedEnclosingSpelling;
  if (findMovedEnclosingField(OPSAllOwnedFields[VD],
                              OPSOwnedOwnedFields[VD], fullFieldName,
                              MovedEnclosingSpelling, IsAggregateAccess)) {
    OwnershipDiagKind Kind = is(VD, Uninitialized) || has(VD, Uninitialized)
                                 ? OwnershipDiagKind::InvalidUseOfUninit
                                 : OwnershipDiagKind::InvalidUseOfMoved;
    diags.push_back(OwnershipDiagInfo(
        Loc, Kind,
        moveAsterisksToFront(VD->getNameAsString() + "." +
                             MovedEnclosingSpelling)));
  }

  // check condition 2
  if (isOrHas(VD, Moved) && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                          VD->getNameAsString() + "." +
                              accessKeySpelling(fullFieldName,
                                                IsAggregateAccess)));
  }
  if (isOrHas(VD, Uninitialized) && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfUninit,
                          VD->getNameAsString() + "." +
                              accessKeySpelling(fullFieldName,
                                                IsAggregateAccess)));
  }

  // check condition 3
  // if fullFieldName has been moved, report error. Pass OPSNullOwnedFields so
  // a field dropped via __assume_null (moved out of OwnedOwned into
  // NullOwned) is not mistaken for a moved field on a later borrow —
  // mirroring the S-path call below, which already passes &SNullOwnedFields.
  FieldKey movedFieldName =
      findMovedFieldKey(OPSAllOwnedFields[VD], OPSOwnedOwnedFields[VD],
                        &OPSNullOwnedFields[VD], fullFieldName,
                        IsAggregateAccess);
  if (movedFieldName && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                          VD->getNameAsString() + "." +
                              placeToKeyString(movedFieldName)));
  }
  // calculate the fields under the fullFieldName prefix: the historical
  // trailing-'.' aggregate marker queried "K." only (a "K.*" star query
  // matches nothing, since '.' is always followed by a field name); the
  // normal form paired the "." and "*" prefix lookups, i.e. all strict
  // field and deref descendants — the key itself is never included.
  FieldSet allPrefixStrs;
  if (IsAggregateAccess) {
    allPrefixStrs = fieldDescendants(OPSAllOwnedFields[VD], fullFieldName);
  } else {
    allPrefixStrs = fieldDescendants(OPSAllOwnedFields[VD], fullFieldName);
    for (FieldKey P : derefDescendants(OPSAllOwnedFields[VD], fullFieldName))
      allPrefixStrs.insert(P);
  }
  FieldSet ownedPrefixStrs;
  if (IsAggregateAccess) {
    ownedPrefixStrs = fieldDescendants(OPSOwnedOwnedFields[VD], fullFieldName);
  } else {
    ownedPrefixStrs = fieldDescendants(OPSOwnedOwnedFields[VD], fullFieldName);
    for (FieldKey P : derefDescendants(OPSOwnedOwnedFields[VD], fullFieldName))
      ownedPrefixStrs.insert(P);
  }
  // A sub-field dropped via __assume_null (moved out of OwnedOwned into
  // NullOwned) is null/handled, not moved — it must not count as a
  // partially-moved child of the accessed field (mirrors the AllMoved
  // exemption in checkOPSUse).
  FieldSet nullPrefixStrs;
  if (IsAggregateAccess) {
    nullPrefixStrs = fieldDescendants(OPSNullOwnedFields[VD], fullFieldName);
  } else {
    nullPrefixStrs = fieldDescendants(OPSNullOwnedFields[VD], fullFieldName);
    for (FieldKey P : derefDescendants(OPSNullOwnedFields[VD], fullFieldName))
      nullPrefixStrs.insert(P);
  }
  FieldSet ownedOrNullPrefixStrs = ownedPrefixStrs;
  for (FieldKey P : nullPrefixStrs)
    ownedOrNullPrefixStrs.insert(P);
  if (allPrefixStrs.size() != ownedOrNullPrefixStrs.size() && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfPartiallyMoved,
                          VD->getNameAsString(), collectMovedFields(VD)));
  }
  // Reading non-owned fields will not update ownership state. The aggregate
  // key (trailing '.') never matched a registered spelling, so it touches an
  // owned subtree only through its descendants.
  bool TouchesOwnedSubtree =
      !IsAggregateAccess && OPSAllOwnedFields[VD].count(fullFieldName) != 0;
  TouchesOwnedSubtree = TouchesOwnedSubtree || !allPrefixStrs.empty();
  if (!isGetAddr && TouchesOwnedSubtree) {
    // change the status of the fields
    if (!IsAggregateAccess)
      OPSOwnedOwnedFields[VD].erase(fullFieldName);
    // remove ownedPrefixStrs from OPSOwnedOwnedFields
    for (FieldKey str : ownedPrefixStrs) {
      OPSOwnedOwnedFields[VD].erase(str);
    }
    if (OPSAllOwnedFields[VD].size() != OPSOwnedOwnedFields[VD].size()) {
      if (!is(VD, Ownership::Status::Moved)) {
        resetAll(VD);
        set(VD, Ownership::Status::PartialMoved);
      }
    }
    if (!OPSAllOwnedFields[VD].empty() && OPSOwnedOwnedFields[VD].empty() &&
        !VD->getType()->getPointeeType()->isOwnedStruct()) {
      if (!is(VD, Ownership::Status::Moved)) {
        resetAll(VD);
        set(VD, Ownership::Status::AllMoved);
      }
    }
  }

  return diags;
}

SmallVector<OwnershipDiagInfo>
Ownership::OwnershipStatus::checkOPSAssign(const VarDecl *VD,
                                           const SourceLocation &Loc) {
  SmallVector<OwnershipDiagInfo> diags;

  // when assign ops, we must ensure the variable is uninit or moved

  // check the status of the variable
  if (!canAssign(VD)) {
    if (isOrHas(VD, Ownership::Status::Owned)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidAssignOfOwned, VD->getNameAsString()));
    } else if (is(VD, Ownership::Status::PartialMoved)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidAssignOfPartiallyMoved,
          VD->getNameAsString(), collectMovedFields(VD)));
    } else if (has(VD, Ownership::Status::PartialMoved)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidAssignOfPossiblyPartiallyMoved,
          VD->getNameAsString(), collectMovedFields(VD)));
    } else if (is(VD, AllMoved)) {
      diags.push_back(
          OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignOfAllMoved,
                            VD->getNameAsString()));
    }
  }
  // change the status to owned
  OPSOwnedOwnedFields[VD] = OPSAllOwnedFields[VD];
  OPSMovedOwnedFields[VD].clear();
  resetAll(VD);
  set(VD, Ownership::Status::Owned);

  return diags;
}

SmallVector<OwnershipDiagInfo>
Ownership::OwnershipStatus::checkOPSDerefAssign(const VarDecl *VD,
                                               const SourceLocation &Loc) {
  SmallVector<OwnershipDiagInfo> diags;
  if (!is(VD, AllMoved)) {
    if (isOrHas(VD, Moved)) {
      OwnershipDiagInfo Info(Loc, InvalidUseOfMoved, VD->getNameAsString());
      diags.push_back(Info);
    } else {
      QualType Pointee = VD->getType()->getPointeeType();
      if (!Pointee.isNull() &&
          Pointee.isOrContainsOwned(BSCLookThrough::NoPointer)) {
        if (isOrHas(VD, Owned)) {
          OwnershipDiagInfo Info(Loc, InvalidAssignOfOwned,
                                 VD->getNameAsString());
          diags.push_back(Info);
        } else if (is(VD, PartialMoved)) {
          OwnershipDiagInfo Info(Loc, InvalidAssignOfPartiallyMoved,
                                 VD->getNameAsString(), collectMovedFields(VD));
          diags.push_back(Info);
        } else if (has(VD, PartialMoved)) {
          OwnershipDiagInfo Info(Loc, InvalidAssignOfPossiblyPartiallyMoved,
                                 VD->getNameAsString(), collectMovedFields(VD));
          diags.push_back(Info);
        }
      }
    }
  }

  if (diags.empty()) {
    OPSOwnedOwnedFields[VD] = OPSAllOwnedFields[VD];
    OPSMovedOwnedFields[VD].clear();
    resetAll(VD);
    set(VD, Ownership::Status::Owned);
  }
  return diags;
}

SmallVector<OwnershipDiagInfo>
Ownership::OwnershipStatus::checkOPSFieldAssign(const VarDecl *VD,
                                                const SourceLocation &Loc,
                                                FieldKey fullFieldName,
                                                bool IsAggregateAccess) {
  SmallVector<OwnershipDiagInfo> diags;

  // when assign a field, we check three conditions:
  // 1. the field's parent must be owned
  // 2. the status of VD must not be moved or uninit
  // 3. the field and the field's subfields must be all moved

  // check condition 1
  std::string MovedEnclosingSpelling;
  if (findMovedEnclosingField(OPSAllOwnedFields[VD],
                              OPSOwnedOwnedFields[VD], fullFieldName,
                              MovedEnclosingSpelling, IsAggregateAccess)) {
    OwnershipDiagKind Kind = is(VD, Uninitialized) || has(VD, Uninitialized)
                                 ? OwnershipDiagKind::InvalidUseOfUninit
                                 : OwnershipDiagKind::InvalidUseOfMoved;
    diags.push_back(OwnershipDiagInfo(
        Loc, Kind,
        moveAsterisksToFront(VD->getNameAsString() + "." +
                             MovedEnclosingSpelling)));
    return diags;
  }

  // check condition 2
  if (isOrHas(VD, Moved) && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignFieldOfMoved,
                          moveAsterisksToFront(
                              VD->getNameAsString() + "." +
                              accessKeySpelling(fullFieldName,
                                                IsAggregateAccess))));
  }
  if (isOrHas(VD, Uninitialized) && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignFieldOfUninit,
                          VD->getNameAsString()));
  }

  // check condition 3
  if (OPSAllOwnedFields[VD].count(fullFieldName) &&
      OPSOwnedOwnedFields[VD].count(fullFieldName) && diags.empty() &&
      !IsAggregateAccess) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignFieldOfOwned,
                          VD->getNameAsString() + "." +
                              placeToKeyString(fullFieldName)));
  }
  // calculate the fields under the fullFieldName prefix: the historical
  // trailing-'.' aggregate marker queried "K." only (a "K.*" star query
  // matches nothing, since '.' is always followed by a field name); the
  // normal form paired the "." and "*" prefix lookups, i.e. all strict
  // field and deref descendants — the key itself is never included.
  FieldSet allPrefixStrs;
  if (IsAggregateAccess) {
    allPrefixStrs = fieldDescendants(OPSAllOwnedFields[VD], fullFieldName);
  } else {
    allPrefixStrs = fieldDescendants(OPSAllOwnedFields[VD], fullFieldName);
    for (FieldKey P : derefDescendants(OPSAllOwnedFields[VD], fullFieldName))
      allPrefixStrs.insert(P);
  }
  FieldSet ownedPrefixStrs;
  if (IsAggregateAccess) {
    ownedPrefixStrs = fieldDescendants(OPSOwnedOwnedFields[VD], fullFieldName);
  } else {
    ownedPrefixStrs = fieldDescendants(OPSOwnedOwnedFields[VD], fullFieldName);
    for (FieldKey P : derefDescendants(OPSOwnedOwnedFields[VD], fullFieldName))
      ownedPrefixStrs.insert(P);
  }
  if (!ownedPrefixStrs.empty() && diags.empty()) {
    diags.push_back(OwnershipDiagInfo(
        Loc, OwnershipDiagKind::InvalidAssignSubFieldOwned,
        VD->getNameAsString(), concatFields(VD, ownedPrefixStrs)));
  }
  if (!is(VD, Ownership::Status::Moved)) {
    if (!IsAggregateAccess && OPSAllOwnedFields[VD].count(fullFieldName)) {
      OPSOwnedOwnedFields[VD].insert(fullFieldName);
    }
    // add allPrefixStrs to OPSOwnedOwnedFields
    for (FieldKey str : allPrefixStrs) {
      OPSOwnedOwnedFields[VD].insert(str);
    }
  }
  if (OPSAllOwnedFields[VD].size() == OPSOwnedOwnedFields[VD].size()) {
    if (!is(VD, Ownership::Status::Owned) &&
        !is(VD, Ownership::Status::Moved)) {
      resetAll(VD);
      set(VD, Ownership::Status::Owned);
    }
  }
  if (OPSAllOwnedFields[VD].size() != OPSOwnedOwnedFields[VD].size()) {
    if (!is(VD, Ownership::Status::Owned) &&
        !is(VD, Ownership::Status::Moved)) {
      // When OPSOwnedOwnedFields is empty, all owned fields have been moved.
      // This can happen when assigning to a non-owned field (e.g. an int field)
      // after moving out owned fields. Set AllMoved, not PartialMoved.
      if (!OPSAllOwnedFields[VD].empty() && OPSOwnedOwnedFields[VD].empty() &&
          !VD->getType()->getPointeeType()->isOwnedStruct()) {
        resetAll(VD);
        set(VD, Ownership::Status::AllMoved);
      } else {
        resetAll(VD);
        set(VD, Ownership::Status::PartialMoved);
      }
    }
  }

  return diags;
}

SmallVector<OwnershipDiagInfo> Ownership::OwnershipStatus::checkSUse(
    const VarDecl *VD, const SourceLocation &Loc, bool isGetAddr, bool isAddrMut) {
  SmallVector<OwnershipDiagInfo> diags;

  // owned struct special manipulation
  if (VD->getType().getTypePtr()->isOwnedStruct()) {
    if (!is(VD, Owned)) {
      if (isOrHas(VD, Uninitialized)) {
        diags.push_back(OwnershipDiagInfo(
            Loc, OwnershipDiagKind::InvalidUseOfUninit, VD->getNameAsString()));
      } else if (isOrHas(VD, Moved)) {
        diags.push_back(OwnershipDiagInfo(
            Loc, OwnershipDiagKind::InvalidUseOfMoved, VD->getNameAsString()));
      }
    }
    if (!isGetAddr) {
      resetAll(VD);
      set(VD, Moved);
    }
  }

  if ((!SMovedOwnedFields[VD].empty() ||
       ((SAllOwnedFields[VD].size() - SOwnedOwnedFields[VD].size() -
             SNullOwnedFields[VD].size() !=
         0) &&
        SAllOwnedFields[VD].size() != SOwnedOwnedFields[VD].size())) &&
      diags.empty()) {
    diags.push_back(OwnershipDiagInfo(Loc, InvalidUseOfPartiallyMoved,
                                      VD->getNameAsString(),
                                      collectMovedFields(VD)));
  }
  if (isAddrMut) {
    if (SAllOwnedFields[VD].size() == SOwnedOwnedFields[VD].size() + SNullOwnedFields[VD].size()) {
      for (const auto &s : SNullOwnedFields[VD])
        SOwnedOwnedFields[VD].insert(s);
      SNullOwnedFields[VD].clear();
    }
  }
  if (!isGetAddr) {
    SMovedOwnedFields[VD] = SAllOwnedFields[VD];
    SOwnedOwnedFields[VD].clear();
  }
  return diags;
}

SmallVector<OwnershipDiagInfo> Ownership::OwnershipStatus::checkSFieldUse(
    const VarDecl *VD, const SourceLocation &Loc, FieldKey fullFieldName,
    bool isGetAddr, bool IsAggregateAccess) {
  SmallVector<OwnershipDiagInfo> diags;

  // owned struct special manipulation
  if (VD->getType().getTypePtr()->isOwnedStruct()) {
    if (is(VD, Uninitialized)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfUninit, VD->getNameAsString()));
    } else if (is(VD, Moved)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfMoved, VD->getNameAsString()));
    }
  }

  if (isOrHas(VD, Moved) && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                          VD->getNameAsString() + "." +
                              accessKeySpelling(fullFieldName,
                                                IsAggregateAccess)));
  }
  // The aggregate form ("p.") only ever matched the field's own registered
  // spelling through the historical isFieldPathPrefix, never an ancestor or
  // descendant; the normal form ("p", "p*") uses the full overlap rules.
  bool OverlapsUninit =
      IsAggregateAccess
          ? SUninitOwnedFields[VD].count(fullFieldName) != 0
          : overlapsOwnedFields(SUninitOwnedFields[VD], fullFieldName);
  if (OverlapsUninit && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfUninit,
                          VD->getNameAsString() + "." +
                              accessKeySpelling(fullFieldName,
                                                IsAggregateAccess)));
  }

  // The aggregate form's "K." key never matched a registered spelling, so
  // the historical explicit-moved lookup was a no-op under it.
  FieldKey movedFieldName =
      IsAggregateAccess
          ? nullptr
          : findExplicitMovedFieldKey(SMovedOwnedFields[VD], fullFieldName);
  if (!movedFieldName)
    movedFieldName =
        findMovedFieldKey(SAllOwnedFields[VD], SOwnedOwnedFields[VD],
                          &SNullOwnedFields[VD], fullFieldName,
                          IsAggregateAccess);
  if (movedFieldName && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                          VD->getNameAsString() + "." +
                              placeToKeyString(movedFieldName)));
  }
  // calculate the fields under the fullFieldName prefix: the historical
  // trailing-'.' aggregate marker queried "K." only (a "K.*" star query
  // matches nothing, since '.' is always followed by a field name); the
  // normal form paired the "." and "*" prefix lookups, i.e. all strict
  // field and deref descendants — the key itself is never included.
  FieldSet allPrefixStrs;
  if (fullFieldName->getKind() == bsc::Place::Kind::Var) {
    // whole-aggregate access (old empty key): only field children match
    // the historical bare "." query.
    allPrefixStrs = fieldDescendants(SAllOwnedFields[VD], fullFieldName);
  } else if (IsAggregateAccess) {
    allPrefixStrs = fieldDescendants(SAllOwnedFields[VD], fullFieldName);
  } else {
    allPrefixStrs = fieldDescendants(SAllOwnedFields[VD], fullFieldName);
    for (FieldKey P : derefDescendants(SAllOwnedFields[VD], fullFieldName))
      allPrefixStrs.insert(P);
  }
  FieldSet ownedPrefixStrs;
  if (fullFieldName->getKind() == bsc::Place::Kind::Var) {
    ownedPrefixStrs = fieldDescendants(SOwnedOwnedFields[VD], fullFieldName);
  } else if (IsAggregateAccess) {
    ownedPrefixStrs = fieldDescendants(SOwnedOwnedFields[VD], fullFieldName);
  } else {
    ownedPrefixStrs = fieldDescendants(SOwnedOwnedFields[VD], fullFieldName);
    for (FieldKey P : derefDescendants(SOwnedOwnedFields[VD], fullFieldName))
      ownedPrefixStrs.insert(P);
  }
  // A sub-field dropped via __assume_null (moved out of OwnedOwned into
  // NullOwned) is null/handled, not moved — it must not count as a
  // partially-moved child of the accessed field.
  FieldSet nullPrefixStrs;
  if (fullFieldName->getKind() == bsc::Place::Kind::Var) {
    nullPrefixStrs = fieldDescendants(SNullOwnedFields[VD], fullFieldName);
  } else if (IsAggregateAccess) {
    nullPrefixStrs = fieldDescendants(SNullOwnedFields[VD], fullFieldName);
  } else {
    nullPrefixStrs = fieldDescendants(SNullOwnedFields[VD], fullFieldName);
    for (FieldKey P : derefDescendants(SNullOwnedFields[VD], fullFieldName))
      nullPrefixStrs.insert(P);
  }
  FieldSet ownedOrNullPrefixStrs = ownedPrefixStrs;
  for (FieldKey P : nullPrefixStrs)
    ownedOrNullPrefixStrs.insert(P);
  bool hasExplicitMovedChild = false;
  for (FieldKey field : SMovedOwnedFields[VD]) {
    if (field->hasPrefix(fullFieldName) && !field->equals(fullFieldName)) {
      hasExplicitMovedChild = true;
      break;
    }
  }
  // A CFG merge may restore a child to may-own while it remains moved on
  // another path.
  if ((allPrefixStrs.size() != ownedOrNullPrefixStrs.size() ||
       hasExplicitMovedChild) &&
      diags.empty()) {
    diags.push_back(OwnershipDiagInfo(
        Loc, OwnershipDiagKind::InvalidUseOfPartiallyMoved,
        VD->getNameAsString() + "." + accessKeySpelling(fullFieldName,
                                                        IsAggregateAccess),
        collectMovedFields(VD)));
  }
  if (!isGetAddr) {
    // The aggregate form's "K." key marked nothing historically (no
    // registered key starts with "K." above the aggregate itself).
    if (!IsAggregateAccess)
      markFieldAndDescendantsMoved(SAllOwnedFields[VD], SMovedOwnedFields[VD],
                                   fullFieldName);
    if (!IsAggregateAccess)
      SOwnedOwnedFields[VD].erase(fullFieldName);
    for (FieldKey str : ownedPrefixStrs) {
      SOwnedOwnedFields[VD].erase(str);
    }
  }

  return diags;
}

SmallVector<OwnershipDiagInfo>
Ownership::OwnershipStatus::checkSAssign(const VarDecl *VD,
                                         const SourceLocation &Loc) {
  SmallVector<OwnershipDiagInfo> diags;

  // special handling is required for owned struct: do not check ownership when reassign, for example:
  // @code
  // owned struct S s1 = init();
  // owned struct S s2 = init();
  // s2 = s1; // Ok
  // @endcode
  if (VD->getType().getTypePtr()->isOwnedStruct()) {
    resetAll(VD);
    set(VD, Owned);
  }
  // when assign a struct, all of its owned fields must be MOVED
  // owned struct does not abide with this rule.
  if (!VD->getType().getTypePtr()->isOwnedStruct()) {
    if (!SOwnedOwnedFields[VD].empty() && diags.empty()) {
      if (!SMovedOwnedFields[VD].empty()) {
        diags.push_back(OwnershipDiagInfo(
            Loc, OwnershipDiagKind::InvalidAssignOfPartiallyMoved,
            VD->getNameAsString(), collectMovedFields(VD)));
      } else if (SAllOwnedFields[VD].size() == SOwnedOwnedFields[VD].size()) {
        diags.push_back(
            OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignOfOwned,
                              VD->getNameAsString()));
      }
    }
  }
  if (!SOwnedOwnedFields[VD].empty() && diags.empty()) {
    if (SAllOwnedFields[VD].size() != SOwnedOwnedFields[VD].size()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidAssignOfPartiallyMoved,
          VD->getNameAsString(), collectMovedFields(VD)));
    }
  }
  SOwnedOwnedFields[VD] = SAllOwnedFields[VD];
  SNullOwnedFields[VD].clear();
  SUninitOwnedFields[VD].clear();
  SMovedOwnedFields[VD].clear();

  return diags;
}

SmallVector<OwnershipDiagInfo> Ownership::OwnershipStatus::checkSFieldAssign(
    const VarDecl *VD, const SourceLocation &Loc, FieldKey fullFieldName,
    bool IsAggregateAccess) {
  SmallVector<OwnershipDiagInfo> diags;

  // owned struct special manipulation
  if (VD->getType().getTypePtr()->isOwnedStruct()) {
    if (!is(VD, Owned)) {
      if (isOrHas(VD, Uninitialized)) {
        diags.push_back(OwnershipDiagInfo(
            Loc, OwnershipDiagKind::InvalidAssignFieldOfUninit,
            VD->getNameAsString()));
      } else if (isOrHas(VD, Moved)) {
        diags.push_back(
            OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignFieldOfMoved,
                              moveAsterisksToFront(
                                  VD->getNameAsString() + "." +
                                  accessKeySpelling(fullFieldName,
                                                    IsAggregateAccess))));
      }
    }
  }

  // when assign a field, we check two conditions:
  // 1. the field's parent must be owned
  // 2. the field and the field's subfields must be all moved

  // check condition 1
  // Walk the enclosing field chain (the aggregate itself included under
  // the aggregate form) deepest-first. A field moved on some branch counts
  // as moved even when a CFG merge restored its may-own state, hence the
  // SMovedOwnedFields alternative to the plain unowned test.
  {
    llvm::SmallVector<FieldKey, 8> Ancestors;
    if (IsAggregateAccess)
      Ancestors.push_back(fullFieldName);
    for (const bsc::Place *A = fullFieldName->getBase();
         A && A->getKind() != bsc::Place::Kind::Var; A = A->getBase())
      Ancestors.push_back(A);
    for (FieldKey parent : Ancestors) {
      if (SAllOwnedFields[VD].count(parent) &&
          (SMovedOwnedFields[VD].count(parent) ||
           !SOwnedOwnedFields[VD].count(parent)) &&
          diags.empty()) {
        OwnershipDiagKind Kind =
            overlapsOwnedFields(SUninitOwnedFields[VD], parent)
                ? OwnershipDiagKind::InvalidUseOfUninit
                : OwnershipDiagKind::InvalidUseOfMoved;
        diags.push_back(OwnershipDiagInfo(
            Loc, Kind,
            moveAsterisksToFront(VD->getNameAsString() + "." +
                                 placeToKeyString(parent))));
        return diags;
      }
    }
  }

  // check condition 2
  if (SAllOwnedFields[VD].count(fullFieldName) && SOwnedOwnedFields[VD].count(fullFieldName) && diags.empty() && !IsAggregateAccess) {
    diags.push_back(OwnershipDiagInfo(
        Loc, OwnershipDiagKind::InvalidAssignFieldOfOwned,
        moveAsterisksToFront(VD->getNameAsString() + "." +
                             placeToKeyString(fullFieldName))));
  }
  // calculate the fields under the fullFieldName prefix: the historical
  // trailing-'.' aggregate marker queried "K." only (a "K.*" star query
  // matches nothing, since '.' is always followed by a field name); the
  // normal form paired the "." and "*" prefix lookups, i.e. all strict
  // field and deref descendants — the key itself is never included.
  FieldSet allPrefixStrs;
  if (fullFieldName->getKind() == bsc::Place::Kind::Var) {
    // whole-aggregate access (old empty key): only field children match
    // the historical bare "." query.
    allPrefixStrs = fieldDescendants(SAllOwnedFields[VD], fullFieldName);
  } else if (IsAggregateAccess) {
    allPrefixStrs = fieldDescendants(SAllOwnedFields[VD], fullFieldName);
  } else {
    allPrefixStrs = fieldDescendants(SAllOwnedFields[VD], fullFieldName);
    for (FieldKey P : derefDescendants(SAllOwnedFields[VD], fullFieldName))
      allPrefixStrs.insert(P);
  }
  FieldSet ownedPrefixStrs;
  if (fullFieldName->getKind() == bsc::Place::Kind::Var) {
    ownedPrefixStrs = fieldDescendants(SOwnedOwnedFields[VD], fullFieldName);
  } else if (IsAggregateAccess) {
    ownedPrefixStrs = fieldDescendants(SOwnedOwnedFields[VD], fullFieldName);
  } else {
    ownedPrefixStrs = fieldDescendants(SOwnedOwnedFields[VD], fullFieldName);
    for (FieldKey P : derefDescendants(SOwnedOwnedFields[VD], fullFieldName))
      ownedPrefixStrs.insert(P);
  }
  if (!ownedPrefixStrs.empty() && diags.empty()) {
    diags.push_back(OwnershipDiagInfo(
        Loc, OwnershipDiagKind::InvalidAssignSubFieldOwned,
        VD->getNameAsString(), concatFields(VD, ownedPrefixStrs)));
  }
  // change the status of the fields
  if (!IsAggregateAccess && SAllOwnedFields[VD].count(fullFieldName))
    SOwnedOwnedFields[VD].insert(fullFieldName);
  // add allPrefixStrs to SOwnedOwnedFields
  for (FieldKey str : allPrefixStrs) {
    SOwnedOwnedFields[VD].insert(str);
  }
  if (!IsAggregateAccess) {
    SNullOwnedFields[VD].erase(fullFieldName);
    SUninitOwnedFields[VD].erase(fullFieldName);
    eraseFieldAndDescendants(SMovedOwnedFields[VD], fullFieldName);
  }
  for (FieldKey str : allPrefixStrs) {
    SNullOwnedFields[VD].erase(str);
    SUninitOwnedFields[VD].erase(str);
  }

  return diags;
}

SmallVector<OwnershipDiagInfo> Ownership::OwnershipStatus::checkBOPUse(
    const VarDecl *VD, const SourceLocation &Loc, bool isGetAddr, bool isAddrMut) {
  SmallVector<OwnershipDiagInfo> diags;

  // check the status of the variable
  if (!is(VD, Ownership::Status::Owned)) {
    if (isOrHas(VD, Ownership::Status::Moved)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfMoved, VD->getNameAsString()));
    } else if (is(VD, Ownership::Status::Uninitialized)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfUninit, VD->getNameAsString()));
    } else if (has(VD, Ownership::Status::Uninitialized)) {
      diags.push_back(
          OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfPossiblyUninit,
                            VD->getNameAsString()));
    } else if (is(VD, Ownership::Status::PartialMoved)) {
      diags.push_back(
          OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfPartiallyMoved,
                            VD->getNameAsString(), collectMovedFields(VD)));
    } else if (is(VD, AllMoved)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfAllMoved, VD->getNameAsString()));
    }
  }
  if (isAddrMut) {
    if (is(VD, Ownership::Status::Null)) {
      set(VD, Ownership::Status::Owned);
    }
  }
  if (!isGetAddr) {
    // change the status to moved
    if (!is(VD, Uninitialized) && !is(VD, Ownership::Status::Null)) {
      BOPOwnedOwnedFields[VD].clear();
      resetAll(VD);
      set(VD, Ownership::Status::Moved);
    }
  }

  return diags;
}

SmallVector<OwnershipDiagInfo> Ownership::OwnershipStatus::checkBOPFieldUse(
    const VarDecl *VD, const SourceLocation &Loc, FieldKey fullFieldName,
    bool isGetAddr) {
  SmallVector<OwnershipDiagInfo> diags;

  // check field's parent: the historical loop tested every proper string
  // prefix of the key deepest-first — every enclosing deref level; the root
  // variable itself (the empty key) was never tested.
  for (const bsc::Place *A = fullFieldName->getBase();
       A && A->getKind() != bsc::Place::Kind::Var; A = A->getBase()) {
    if (BOPAllOwnedFields[VD].count(A) && !BOPOwnedOwnedFields[VD].count(A)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfMoved,
          placeToKeyString(fullFieldName) + VD->getNameAsString()));
      break;
    }
  }

  // check VD has value
  bool isUninit = is(VD, Ownership::Status::Uninitialized);
  bool mayUninit = has(VD, Ownership::Status::Uninitialized);
  if ((isUninit || mayUninit) && diags.empty()) {
    if (isUninit) {
      diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfUninit,
                                        VD->getNameAsString()));
    } else {
      diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfPossiblyUninit,
                                        VD->getNameAsString()));
    }
  }
  if (isOrHas(VD, Ownership::Status::Moved) &&
      diags.empty()) {
    diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                                      VD->getNameAsString()));
  }

  // if the field is owned qualified, check the field and its child
  if (BOPAllOwnedFields[VD].count(fullFieldName)) {
    // if fullFieldName has been moved, report error
    if (!BOPOwnedOwnedFields[VD].count(fullFieldName) && diags.empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfMoved,
          placeToKeyString(fullFieldName) + VD->getNameAsString()));
    }
    // calculate the fields with fullFieldName prefix: the historical bare
    // prefix query includes the key itself plus everything below it.
    FieldSet allPrefixStrs =
        strictDescendants(BOPAllOwnedFields[VD], fullFieldName);
    allPrefixStrs.insert(fullFieldName);
    FieldSet ownedPrefixStrs =
        strictDescendants(BOPOwnedOwnedFields[VD], fullFieldName);
    if (BOPOwnedOwnedFields[VD].count(fullFieldName))
      ownedPrefixStrs.insert(fullFieldName);
    if (allPrefixStrs.size() != ownedPrefixStrs.size() && diags.empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfPartiallyMoved,
          placeToKeyString(fullFieldName) + VD->getNameAsString(),
          collectMovedFields(VD)));
    }
    if (!isGetAddr) {
      // change the status of the fields
      BOPOwnedOwnedFields[VD].erase(fullFieldName);
      // remove ownedPrefixStrs from BOPOwnedOwnedFields
      for (FieldKey str : ownedPrefixStrs) {
        BOPOwnedOwnedFields[VD].erase(str);
      }
      if (BOPAllOwnedFields[VD].size() != BOPOwnedOwnedFields[VD].size()) {
        if (!is(VD, Ownership::Status::Moved)) {
          resetAll(VD);
          set(VD, Ownership::Status::PartialMoved);
        }
      }
      if (BOPOwnedOwnedFields[VD].empty()) {
        if (!is(VD, Ownership::Status::Moved)) {
          resetAll(VD);
          set(VD, Ownership::Status::AllMoved);
        }
      }
    }
  }

  return diags;
}

SmallVector<OwnershipDiagInfo>
Ownership::OwnershipStatus::checkBOPAssign(const VarDecl *VD,
                                           const SourceLocation &Loc) {
  SmallVector<OwnershipDiagInfo> diags;

  // check the status of the variable
  if (!canAssign(VD)) {
    if (isOrHas(VD, Ownership::Status::Owned)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidAssignOfOwned, VD->getNameAsString()));
    } else if (is(VD, Ownership::Status::PartialMoved)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidAssignOfPartiallyMoved,
          VD->getNameAsString(), collectMovedFields(VD)));
    } else if (has(VD, Ownership::Status::PartialMoved)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidAssignOfPossiblyPartiallyMoved,
          VD->getNameAsString(), collectMovedFields(VD)));
    } else if (is(VD, Ownership::Status::AllMoved)) {
      diags.push_back(
          OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignOfAllMoved,
                            VD->getNameAsString()));
    }
  }
  // change the status to owned
  BOPOwnedOwnedFields[VD] = BOPAllOwnedFields[VD];
  resetAll(VD);
  set(VD, Ownership::Status::Owned);

  return diags;
}

SmallVector<OwnershipDiagInfo>
Ownership::OwnershipStatus::checkBOPFieldAssign(const VarDecl *VD,
                                                const SourceLocation &Loc,
                                                FieldKey fullFieldName) {
  SmallVector<OwnershipDiagInfo> diags;

  // for a non owned qualified field, just check its parent
  // for a owned qualified field, we check three conditions:
  // // 1. the status of VD must be PartiallyMoved or AllMoved
  // // 2. the field's parents must be not moved
  // // 3. the field and the field's subfields must be all moved

  // the parent field must be owned: every enclosing deref level deepest-first,
  // never the root variable (the historical empty key).
  for (const bsc::Place *A = fullFieldName->getBase();
       A && A->getKind() != bsc::Place::Kind::Var; A = A->getBase()) {
    if (BOPAllOwnedFields[VD].count(A) && !BOPOwnedOwnedFields[VD].count(A)) {
      diags.push_back(
          OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignFieldOfMoved,
                            placeToKeyString(fullFieldName) +
                                VD->getNameAsString()));
      break;
    }
  }
  if ((isOrHas(VD, Moved) || isOrHas(VD, Uninitialized)) && diags.empty()) {
    diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                                      VD->getNameAsString()));
  }

  if (BOPAllOwnedFields[VD].count(fullFieldName)) {
    // check condition 3
    if (BOPOwnedOwnedFields[VD].count(fullFieldName) && diags.empty()) {
      diags.push_back(
          OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignFieldOfOwned,
                            VD->getNameAsString() +
                                placeToKeyString(fullFieldName)));
    }
    // calculate the fields with fullFieldName prefix: the historical bare
    // prefix query includes the key itself plus everything below it.
    FieldSet allPrefixStrs =
        strictDescendants(BOPAllOwnedFields[VD], fullFieldName);
    allPrefixStrs.insert(fullFieldName);
    FieldSet ownedPrefixStrs =
        strictDescendants(BOPOwnedOwnedFields[VD], fullFieldName);
    if (BOPOwnedOwnedFields[VD].count(fullFieldName))
      ownedPrefixStrs.insert(fullFieldName);
    if (!ownedPrefixStrs.empty() && diags.empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidAssignSubFieldOwned,
          VD->getNameAsString(), concatFields(VD, ownedPrefixStrs)));
    }
    if (!is(VD, Ownership::Status::Moved)) {
      if (BOPAllOwnedFields[VD].count(fullFieldName)) {
        BOPOwnedOwnedFields[VD].insert(fullFieldName);
      }
      // add allPrefixStrs to BOPOwnedOwnedFields
      for (FieldKey str : allPrefixStrs) {
        BOPOwnedOwnedFields[VD].insert(str);
      }
    }
    if (BOPAllOwnedFields[VD].size() == BOPOwnedOwnedFields[VD].size()) {
      if (!is(VD, Ownership::Status::Owned) &&
          !is(VD, Ownership::Status::Moved)) {
        resetAll(VD);
        set(VD, Ownership::Status::Owned);
      }
    }
    if (BOPAllOwnedFields[VD].size() != BOPOwnedOwnedFields[VD].size()) {
      if (!is(VD, Ownership::Status::Owned) &&
          !is(VD, Ownership::Status::Moved)) {
        resetAll(VD);
        set(VD, Ownership::Status::PartialMoved);
      }
    }
  }

  return diags;
}

// if cast a `struct S * owned` variable to `void * owned`, we must ensure:
// 1. it is not moved or uninit
// 2. its owned fields are all moved
SmallVector<OwnershipDiagInfo>
Ownership::OwnershipStatus::checkCastOPS(const VarDecl *VD,
                                         const SourceLocation &Loc) {
  SmallVector<OwnershipDiagInfo> diags;

  if (isOrHas(VD, Ownership::Status::Moved)) {
    diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidCastMoved,
                                      VD->getNameAsString()));
  }
  if (isOrHas(VD, Uninitialized)) {
    diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidCastUninit,
                                      VD->getNameAsString()));
  }
  if (is(VD, PartialMoved)) {
    diags.push_back(OwnershipDiagInfo(
        Loc, OwnershipDiagKind::InvalidCastFieldOwned, VD->getNameAsString(),
        concatFields(VD, OPSOwnedOwnedFields[VD])));
  }
  if (!OPSOwnedOwnedFields[VD].empty() && diags.empty()) {
    diags.push_back(OwnershipDiagInfo(
        Loc, OwnershipDiagKind::InvalidCastFieldOwned, VD->getNameAsString(),
        concatFields(VD, OPSOwnedOwnedFields[VD])));
  }
  OPSOwnedOwnedFields[VD].clear();
  resetAll(VD);
  set(VD, Ownership::Status::Moved);

  return diags;
}

// if cast a `int * owned` variable to `void * owned`, we must ensure:
// 1. it is not moved or uninit
// 2. its owned fields are all moved
SmallVector<OwnershipDiagInfo>
Ownership::OwnershipStatus::checkCastBOP(const VarDecl *VD,
                                         const SourceLocation &Loc) {
  SmallVector<OwnershipDiagInfo> diags;

  // AllMoved is OK - it means all sub-fields are freed, so we can free the variable itself
  if (isOrHas(VD, Ownership::Status::Moved) &&
      !is(VD, Ownership::Status::AllMoved)) {
    diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidCastMoved,
                                      VD->getNameAsString()));
  }
  if (isOrHas(VD, Uninitialized)) {
    diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidCastUninit,
                                      VD->getNameAsString()));
  }
  if (!BOPOwnedOwnedFields[VD].empty() && diags.empty()) {
    diags.push_back(OwnershipDiagInfo(
        Loc, OwnershipDiagKind::InvalidCastFieldOwned, VD->getNameAsString(),
        concatFields(VD, BOPOwnedOwnedFields[VD])));
  }
  BOPOwnedOwnedFields[VD].clear();
  resetAll(VD);
  set(VD, Ownership::Status::Moved);

  return diags;
}

SmallVector<OwnershipDiagInfo> Ownership::OwnershipStatus::checkCastField(
    const VarDecl *VD, const SourceLocation &Loc, FieldKey fullFieldName) {
  SmallVector<OwnershipDiagInfo> diags;

  if (OPSStatus.count(VD)) {
    if (!OPSOwnedOwnedFields[VD].count(fullFieldName) && diags.empty()) {
      OwnershipDiagKind Kind = isOrHas(VD, Uninitialized)
                                   ? InvalidCastUninit
                                   : InvalidCastMoved;
      diags.push_back(OwnershipDiagInfo(
          Loc, Kind,
          moveAsterisksToFront(VD->getNameAsString() + "." +
                               placeToKeyString(fullFieldName))));
    }
    // calculate the fields with fullFieldName prefix
    FieldSet allPrefixStrs =
        fieldDescendants(OPSAllOwnedFields[VD], fullFieldName);
    FieldSet ownedPrefixStrs =
        fieldDescendants(OPSOwnedOwnedFields[VD], fullFieldName);
    if (!ownedPrefixStrs.empty() && diags.empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidCastFieldOwned,
          moveAsterisksToFront(VD->getNameAsString() + "." +
                               placeToKeyString(fullFieldName)),
          concatFields(VD, ownedPrefixStrs)));
    }
    if (OPSAllOwnedFields[VD].count(fullFieldName)) {
      OPSOwnedOwnedFields[VD].erase(fullFieldName);
    }
    // remove allPrefixStrs from OPSOwnedOwnedFields
    for (FieldKey str : ownedPrefixStrs) {
      OPSOwnedOwnedFields[VD].erase(str);
    }
    if (OPSAllOwnedFields[VD].size() != OPSOwnedOwnedFields[VD].size()) {
      if (!is(VD, Ownership::Status::Moved)) {
        resetAll(VD);
        set(VD, Ownership::Status::PartialMoved);
      }
    }
    if (OPSOwnedOwnedFields[VD].empty() && !VD->getType()->getPointeeType()->isOwnedStruct()) {
      if (!is(VD, Ownership::Status::Moved)) {
        resetAll(VD);
        set(VD, Ownership::Status::AllMoved);
      }
    }
  }

  if (SStatus.count(VD)) {
    // An Index level in the path marks an array-field element (e.g.
    // w.arr[].a): field-level tracking treats the whole array as one
    // aggregate, so element-granularity moved detection cannot distinguish
    // arr[0].a from arr[1].a; skip the error and keep erasing the aggregate
    // field.
    bool ArrayFieldPath = false;
    for (const bsc::Place *Cur = fullFieldName; Cur;
         Cur = Cur->getBase())
      if (Cur->getKind() == bsc::Place::Kind::Index) {
        ArrayFieldPath = true;
        break;
      }
    FieldKey movedFieldName =
        findExplicitMovedFieldKey(SMovedOwnedFields[VD], fullFieldName);
    if (movedFieldName && !ArrayFieldPath) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidCastMoved,
          moveAsterisksToFront(VD->getNameAsString() + "." +
                               placeToKeyString(movedFieldName))));
    } else if (!SOwnedOwnedFields[VD].count(fullFieldName) && !ArrayFieldPath) {
      OwnershipDiagKind Kind = isOrHas(VD, Uninitialized)
                                   ? InvalidCastUninit
                                   : InvalidCastMoved;
      diags.push_back(OwnershipDiagInfo(
          Loc, Kind,
          moveAsterisksToFront(VD->getNameAsString() + "." +
                               placeToKeyString(fullFieldName))));
    } else if (overlapsOwnedFields(SUninitOwnedFields[VD], fullFieldName)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, InvalidCastUninit,
          moveAsterisksToFront(VD->getNameAsString() + "." +
                               placeToKeyString(fullFieldName))));
    }
    // calculate the fields with fullFieldName prefix
    FieldSet allPrefixStrs =
        fieldDescendants(SAllOwnedFields[VD], fullFieldName);
    FieldSet ownedPrefixStrs =
        fieldDescendants(SOwnedOwnedFields[VD], fullFieldName);
    if (ownedPrefixStrs.size() != 0 && diags.empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidCastFieldOwned,
          moveAsterisksToFront(VD->getNameAsString() + "." +
                               placeToKeyString(fullFieldName)),
          concatFields(VD, ownedPrefixStrs)));
    }
    SOwnedOwnedFields[VD].erase(fullFieldName);
    markFieldAndDescendantsMoved(SAllOwnedFields[VD], SMovedOwnedFields[VD],
                                 fullFieldName);
    for (FieldKey str : ownedPrefixStrs) {
      SOwnedOwnedFields[VD].erase(str);
    }
  }

  if (BOPStatus.count(VD)) {
    if (!BOPOwnedOwnedFields[VD].count(fullFieldName) && diags.empty()) {
      OwnershipDiagKind Kind = isOrHas(VD, Uninitialized)
                                   ? InvalidCastUninit
                                   : InvalidCastMoved;
      diags.push_back(OwnershipDiagInfo(
          Loc, Kind,
          moveAsterisksToFront(VD->getNameAsString() + "." +
                               placeToKeyString(fullFieldName))));
    }
    // Check for deeper-level owned fields (e.g., when freeing **, check if *** exists)
    FieldSet ownedPrefixStrs =
        derefDescendants(BOPOwnedOwnedFields[VD], fullFieldName);
    if (!ownedPrefixStrs.empty() && diags.empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidCastFieldOwned,
          moveAsterisksToFront(VD->getNameAsString() + "." +
                               placeToKeyString(fullFieldName)),
          concatFields(VD, ownedPrefixStrs)));
    }
    BOPOwnedOwnedFields[VD].erase(fullFieldName);
    // Remove deeper-level fields
    for (FieldKey str : ownedPrefixStrs) {
      BOPOwnedOwnedFields[VD].erase(str);
    }
    if (BOPOwnedOwnedFields[VD].empty()) {
      resetAll(VD);
      set(VD, Ownership::Status::AllMoved);
    }
  }

  return diags;
}

SmallVector<OwnershipDiagInfo> Ownership::OwnershipStatus::checkMemoryLeak(
    const VarDecl *VD, const SourceLocation &Loc, bool isDestructor) {
  SmallVector<OwnershipDiagInfo> diags;
  // for a struct pointer owned variable,
  // the following two situations indicates that a memory leak has occurred:
  // 1. if OPSStatus[VD] is not MOVED or UNINITIALIZED, VD memory leak
  // 2. if OPSOwnedOwnedFields[VD] is not empty, VD's owned fields memory leak
  if (OPSStatus.count(VD)) {
    // situation 1
    if (!canAssign(VD)) {
      diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::MemoryLeak,
                                        VD->getNameAsString()));
      // reset the state of VD, it's important in for/while
      resetAll(VD);
      set(VD, Ownership::Status::Moved);
    }
    // situation 2
    if (!OPSOwnedOwnedFields[VD].empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::FieldMemoryLeak, VD->getNameAsString(),
          concatFields(VD, OPSOwnedOwnedFields[VD])));
      OPSOwnedOwnedFields[VD].clear();
    }
  }

  // for a struct with owned fields,
  // the following situation indicates that a memory leak has occurred:
  // 1. if SOwnedOwnedFields[VD] is not empty, VD's owned fields memory leak
  if (SStatus.count(VD)) {
    if (!VD->getType()->isOwnedStruct()) {
      if (!SOwnedOwnedFields[VD].empty()) {
        diags.push_back(OwnershipDiagInfo(
            Loc, OwnershipDiagKind::FieldMemoryLeak, VD->getNameAsString(),
            concatFields(VD, SOwnedOwnedFields[VD])));
        SOwnedOwnedFields[VD].clear();
      }
    } else {
      if (isDestructor) {
        if (isa<ParmVarDecl>(VD)) {
          if (!SOwnedOwnedFields[VD].empty()) {
            diags.push_back(OwnershipDiagInfo(
                Loc, OwnershipDiagKind::OwnedStructNotProperlyFreed,
                VD->getType().getAsString(),
                concatFields(VD, SOwnedOwnedFields[VD])));
            SOwnedOwnedFields[VD].clear();
          }
        }
      } else if (has(VD, Uninitialized)) {
        // Initialized on some path only: the destructor reads the object.
        diags.push_back(OwnershipDiagInfo(
            Loc, OwnershipDiagKind::InvalidUseOfPossiblyUninit,
            VD->getNameAsString()));
        SOwnedOwnedFields[VD].clear();
      } else {
        if ((SOwnedOwnedFields[VD].size() + SNullOwnedFields[VD].size() <
             SAllOwnedFields[VD].size()) &&
            !is(VD, Moved)) {
          diags.push_back(OwnershipDiagInfo(
              Loc, OwnershipDiagKind::OwnedStructPartiallyMoved,
              VD->getNameAsString(),
              concatUnmovedFields(VD, SOwnedOwnedFields[VD],
                                  SAllOwnedFields[VD])));
          SOwnedOwnedFields[VD].clear();
        }
      }
    }
  }

  // for a basic owned pointer,
  // the following situation indicates that a memory leak has occurred:
  // 1. if BOPStatus[VD] is not MOVED, VD's memory leak
  // 2. if BOPOwnedOwnedFields[VD] is not empty, VD's owned fields memory leak
  if (BOPStatus.count(VD)) {
    // situation 1
    if (!canAssign(VD)) {
      diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::MemoryLeak,
                                        VD->getNameAsString()));
      resetAll(VD);
      set(VD, Ownership::Status::Moved);
    }
    // situation 2
    if (!BOPOwnedOwnedFields[VD].empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::FieldMemoryLeak, VD->getNameAsString(),
          concatFields(VD, BOPOwnedOwnedFields[VD])));
      BOPOwnedOwnedFields[VD].clear();
    }
  }

  return diags;
}

//===----------------------------------------------------------------------===//
// Dataflow computation.
//===----------------------------------------------------------------------===//

namespace {
// Extra null test applied to a leaf initializer expression by the helpers
// below: the syntactic test (omitted init / null literal) is always applied,
// while this one folds expressions that are must-be-null in the current
// dataflow state (e.g. a null _Owned variable moved into a field:
// `struct S st = {null_v};`), mirroring `st.v = null_v;`.
using NullInitTest = std::function<bool(const Expr *)>;

// recursively check that an initializer list initializes every
// element/field with a null (or omitted / zero) value, so the whole array can
// be treated as null-initialized.
// True when an initializer of type `Ty` leaves every _Owned pointer (or
// move-semantic sub-object) null: implicit init, an explicit null, an
// expression that is must-be-null in the current state, or (for a struct)
// every _Owned field owned-null. Plain (non-owned) fields may be non-zero
// without implying ownership.
static bool isOwnedNullInit(ASTContext &ctx, const Expr *Init, QualType Ty,
                            const NullInitTest &ExtraNull) {
  if (!Init)
    return true;
  Init = Init->IgnoreParenImpCasts();
  if (isa<ImplicitValueInitExpr>(Init))
    return true;
  if (IsNullExpr(ctx, Init) || (ExtraNull && ExtraNull(Init)))
    return true;
  const auto *ILE = dyn_cast<InitListExpr>(Init);
  if (!ILE)
    return false;
  Ty = Ty.getCanonicalType();
  if (const RecordType *RT = Ty->getAs<RecordType>()) {
    for (const FieldDecl *FD : RT->getDecl()->fields()) {
      QualType FT = FD->getType();
      if (!IsTrackedType(FT))
        continue; // plain field: ignore
      const Expr *FieldInit = FD->getFieldIndex() < ILE->getNumInits()
                                  ? ILE->getInit(FD->getFieldIndex())
                                  : nullptr;
      if (!isOwnedNullInit(ctx, FieldInit, FT, ExtraNull))
        return false;
    }
    return true;
  }
  // Non-struct element (owned pointer / nested array): every entry null.
  QualType InnerTy = Ty;
  if (const auto *AT = InnerTy->getAsArrayTypeUnsafe())
    InnerTy = AT->getElementType();
  for (unsigned I = 0; I < ILE->getNumInits(); ++I)
    if (!isOwnedNullInit(ctx, ILE->getInit(I), InnerTy, ExtraNull))
      return false;
  return true;
}

// True when every element of an owned-element array initializer owns nothing
// (all elements owned-null).
static bool isAllNullInits(ASTContext &ctx, const InitListExpr *ILE,
                           QualType ElemType, const NullInitTest &ExtraNull) {
  for (unsigned I = 0; I < ILE->getNumInits(); ++I)
    if (!isOwnedNullInit(ctx, ILE->getInit(I), ElemType, ExtraNull))
      return false;
  return true;
}

// For a struct-element array initializer with mixed / non-null elements,
// track each owned field independently: a field that is null in every element
// owns nothing and is recorded as null; a field with any non-null element
// keeps the whole field aggregate owned.
static void NullAllNullStructArrayFields(Ownership::OwnershipStatus &Stat,
                                         ASTContext &Ctx, const VarDecl *VD,
                                         RecordDecl *RD,
                                         const InitListExpr *ILE,
                                         const NullInitTest &ExtraNull) {
  for (const FieldDecl *FD : RD->fields()) {
    QualType FT = FD->getType();
    if (!IsTrackedType(FT))
      continue;
    bool AllNull = true;
    for (unsigned I = 0; I < ILE->getNumInits(); ++I) {
      const Expr *ElemInit = ILE->getInit(I);
      if (!ElemInit)
        continue; // omitted element -> zero initialized
      ElemInit = ElemInit->IgnoreParenImpCasts();
      const InitListExpr *ElemILE = dyn_cast<InitListExpr>(ElemInit);
      if (!ElemILE) {
        AllNull = false; // unknown element shape: be conservative
        break;
      }
      const Expr *FieldInit =
          FD->getFieldIndex() < ElemILE->getNumInits()
              ? ElemILE->getInit(FD->getFieldIndex())
              : nullptr;
      if (!isOwnedNullInit(Ctx, FieldInit, FT, ExtraNull)) {
        AllNull = false;
        break;
      }
    }
    if (AllNull) {
      // Use isFieldPathPrefix so a field named "a" does not also null a
      // sibling field named "ab".
      bsc::PlaceBuilder PB(Ctx);
      const bsc::Place *FieldPlace = PB.BuildField(
          PB.Build(VD, VD->getLocation()), FD, FD->getLocation());
      for (FieldKey Key : Stat.SAllOwnedFields[VD])
        // hasPrefix (any projection below the field, Index levels included):
        // the old string prefix accepted a '[' boundary, so the plain field
        // name "arr" also nulled its array key "arr[]" — isFieldPathPrefix
        // deliberately excludes the Index child and would miss it.
        if (Key->hasPrefix(FieldPlace)) {
          Stat.SOwnedOwnedFields[VD].erase(Key);
          Stat.SNullOwnedFields[VD].insert(Key);
        }
    }
  }
  Stat.refreshArrayFieldState(VD, Stat.SOwnedOwnedFields[VD],
                              Stat.SAllOwnedFields[VD]);
}

class TransferFunctions : public StmtVisitor<TransferFunctions> {
  OwnershipImpl &OS;
  Ownership::OwnershipStatus &stat;
  OwnershipDiagReporter &reporter;
  const llvm::DenseSet<const StmtExpr *> &StmtExprsWithDeferredLifetimeEnds;
  bool isAddrMut = false;
  // The RHS of the assignment currently being visited (nullptr check for
  // "assign to null" which must not be reported as overwriting an owned
  // value).
  const Expr *CurRHS = nullptr;

  enum Operation { None, Assign, Move, GetAddr };
  Operation op = Operation::None;
  // The CFGStmt element whose transfer this walk executes, or null in a
  // read-only walk. An assignment, ++/--, or call visited while not being
  // this element is a re-visit — its own hoisted element runs the semantics.
  const Stmt *TransferStmt = nullptr;

public:
  TransferFunctions(OwnershipImpl &os, Ownership::OwnershipStatus &Stat,
                    OwnershipDiagReporter &reporter,
                    const llvm::DenseSet<const StmtExpr *> &DeferredStmtExprs)
      : OS(os), stat(Stat), reporter(reporter),
        StmtExprsWithDeferredLifetimeEnds(DeferredStmtExprs) {}

  void SetTransferStmt(const Stmt *S) { TransferStmt = S; }

  void VisitArraySubscriptExpr(ArraySubscriptExpr *ASE);
  void VisitBinaryOperator(BinaryOperator *BO);
  void VisitCompoundAssignOperator(CompoundAssignOperator *CAO);
  void VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *UE);
  void VisitCallExpr(CallExpr *CE);
  void VisitStmtExpr(StmtExpr *SE);
  void VisitStmtExprResult(Expr *E);
  void VisitCStyleCastExpr(CStyleCastExpr *CSCE);
  void VisitDeclRefExpr(DeclRefExpr *DRE);
  void VisitDeclRefExprPlace(FieldKey Place, SourceLocation Loc);
  void VisitDeclStmt(DeclStmt *DS);
  void VisitInitListExpr(InitListExpr *ILE);
  void VisitMemberExpr(MemberExpr *ME);
  void VisitReturnStmt(ReturnStmt *RS);
  void VisitLifetimeEnds(VarDecl *VD, SourceLocation SL, bool isDestructor);
  void VisitStmt(Stmt *S);
  void VisitReadOnlyExpr(Stmt *S);
  void VisitUnaryOperator(UnaryOperator *UO);
  void VisitAbstractConditionalOperator(AbstractConditionalOperator *ACO);
  void VisitBinaryConditionalOperator(BinaryConditionalOperator *BCO);

  void HandleInitListExpr(VarDecl *VD, RecordDecl *RD, InitListExpr *ILE,
                          FieldKey fullFieldName = nullptr);
  void HandleDREPlaceAssign(FieldKey Place, SourceLocation Loc);
  void HandleDREPlaceUse(FieldKey Place, SourceLocation Loc);

  /// Returns true if \p E is a must-be-null owned pointer expression: a
  /// DeclRefExpr referring to a tracked variable whose only OwnershipStatus
  /// is Null, or a struct-value field tracked as must-be-null. Peels parens,
  /// implicit casts, value-preserving C-style casts, comma RHS, and ternary
  /// with both arms must-be-null.
  bool isExprRefToNullOwnedVar(const Expr *E) const {
    E = E->IgnoreParenImpCasts();
    if (E->isNullExpr(OS.ctx))
      return true;
    if (const CStyleCastExpr *CSCE = dyn_cast<CStyleCastExpr>(E))
      return isExprRefToNullOwnedVar(CSCE->getSubExpr());
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(E)) {
      if (BO->getOpcode() == BO_Comma)
        return isExprRefToNullOwnedVar(BO->getRHS());
    }
    if (const AbstractConditionalOperator *ACO =
            dyn_cast<AbstractConditionalOperator>(E)) {
      return isExprRefToNullOwnedVar(ACO->getTrueExpr()) &&
             isExprRefToNullOwnedVar(ACO->getFalseExpr());
    }
    if (const MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
      // Peel a leading deref so `(*p).f` behaves like `p->f`.
      if (const bsc::Place *FieldPlace = getMemberFieldPlace(ME, OS.ctx)) {
        if (const VarDecl *SrcVD = FieldPlace->getRoot()) {
          // Struct value whose field is must-be-null. SNullOwnedFields is the
          // union over the paths reaching here, so require the field to own
          // nothing on *every* path: a field that is null on one path but owned
          // on another (Owned|Null) still owns the value being read, and folding
          // it as a null source would leave the destination unowned and hide the
          // leak of that value (the scalar form below uses the exact test).
          if (stat.SStatus.count(SrcVD) &&
              stat.SNullOwnedFields[SrcVD].count(FieldPlace) &&
              !stat.SOwnedOwnedFields[SrcVD].count(FieldPlace))
            return true;
          // Owned pointer to struct in must-be-null state: any field access
          // yields a must-be-null value.
          if (stat.OPSStatus.count(SrcVD) &&
              stat.is(SrcVD, Ownership::Status::Null))
            return true;
        }
      }
      return false;
    }
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
      if (const VarDecl *SrcVD = dyn_cast<VarDecl>(DRE->getDecl())) {
        if ((stat.BOPStatus.count(SrcVD) &&
             stat.is(SrcVD, Ownership::Status::Null)) ||
            (stat.OPSStatus.count(SrcVD) &&
             stat.is(SrcVD, Ownership::Status::Null)))
          return true;
      }
    }
    return false;
  }

  // Extra null test for initializer lists: folds expressions that are
  // must-be-null in the current state into "the slot owns nothing", so
  // `struct S st = {null_v};` is treated like `st.v = null_v;` instead of
  // leaving the field owned (a false leak). Values that still own something
  // are left alone, keeping the conservative owned state.
  NullInitTest stateNullInitTest() const {
    return [this](const Expr *E) { return isExprRefToNullOwnedVar(E); };
  }

  // helpers for owned-element array element access.
  // Peel an expression that may be a[i], a[i][j] or s[i].f / s[i].a[j] down
  // to the base VarDecl of an owned-element array; returns the array variable
  // (or nullptr). If the base is a T *_Owned _ArrayElem pointer, IsArrElemPtr
  // is set (element transfer is always forbidden for those).
  const VarDecl *peelArrayBase(const Expr *E, bool &IsArrElemPtr) const;
  // Check that a transfer site is allowed; emit ArrayElemTransferForbidden
  // otherwise. Returns true if the transfer is allowed.
  bool isArrayElemTransferAllowed(const Expr *Site,
                                  const VarDecl *ArrVD,
                                  bool IsArrElemPtr);
  // Check that a __forget argument may be forgotten here. An argument that
  // addresses an element of an owned-element array (arr[i], w.arr[i],
  // a[i].p) follows the same qualifying-loop rule as any other element
  // transfer; other shapes are always allowed. Emits
  // ArrayElemTransferForbidden and returns false when forbidden.
  bool isArrayElemForgetAllowed(const Expr *Arg);
  // Apply the dataflow transition for a[i] / s[i].f transfers. Move-out of an
  // already-moved aggregate / field reports use-after-move. The whole-array
  // aggregate is the root Var place (the historical "" key).
  void applyArrayElemTransition(SourceLocation Loc, const VarDecl *ArrVD,
                                FieldKey fieldName, bool IsAssign);
  void applyArrayElemAssign(SourceLocation Loc, const VarDecl *ArrVD,
                            FieldKey fieldName);
  void applyArrayElemMoveOut(SourceLocation Loc, const VarDecl *ArrVD,
                             FieldKey fieldName);
  // Check a field-array transfer site (w.arr[i] / w.arr[i].a) against the
  // qualifying-loop allow-sites; apply the transition when allowed and
  // return whether it was performed.
  bool tryTransferArrayField(const Expr *Site, const VarDecl *HostVD,
                             FieldKey FieldPath, bool IsAssign);
  // True when a member write/move on an owned-element array \p VD at
  // \p FieldPath touches an owned value and must be gated by the
  // qualifying-loop rules. A plain non-owned terminal member (e.g.
  // `a[i]->x`, `w.arr[i].x` for an `int x`) does not change ownership.
  bool memberAccessTouchesOwned(const VarDecl *VD,
                                FieldKey FieldPath,
                                QualType MemberTy) const;
  // Record a transfer-forbidden diagnostic for ArrVD at Loc.
  void emitArrayElemForbidden(SourceLocation Loc, const VarDecl *ArrVD);
};
} // namespace

void TransferFunctions::VisitStmt(Stmt *S) {
  for (auto *C : S->children()) {
    if (C) {
      Visit(C);
    }
  }
}

/// Visit an expression as a read-only use of its bases: op == GetAddr checks
/// use-of-moved / use-of-uninit without consuming ownership.
void TransferFunctions::VisitReadOnlyExpr(Stmt *S) {
  const Stmt *Saved = TransferStmt;
  TransferStmt = nullptr;
  op = GetAddr;
  Visit(S);
  op = None;
  isAddrMut = false;
  TransferStmt = Saved;
}

void TransferFunctions::VisitStmtExpr(StmtExpr *SE) {
  CompoundStmt *CS = SE->getSubStmt();
  if (CS->body_empty()) {
    op = Operation::None;
    return;
  }

  // getStmtExprResult() skips trailing NullStmts, so stop the body walk at
  // its node rather than at body_back(): otherwise a trailing ';' makes the
  // result statement part of the walk AND the result visit.
  Stmt *ResultStmt = CS->getStmtExprResult();

  // Only the result expression is the StmtExpr's value: the incoming op is
  // propagated to it alone — a consuming op must not reach statement-
  // position reads and assignments in the body, which nothing consumes.
  // Cross-block consumers arrive before the body's elements have run, so
  // unlike the deferred case the body is still walked here — as reads.
  if (!StmtExprsWithDeferredLifetimeEnds.count(SE)) {
    Operation Saved = op;
    op = GetAddr;
    for (Stmt *S : CS->body()) {
      if (S == ResultStmt)
        break;
      Visit(S);
    }
    op = Saved;
  }
  if (auto *Result = dyn_cast<ValueStmt>(ResultStmt))
    if (Expr *ResultExpr = Result->getExprStmt())
      VisitStmtExprResult(ResultExpr);

  op = Operation::None;
}

void TransferFunctions::VisitStmtExprResult(Expr *E) {
  E = E->IgnoreParenImpCasts();

  if (auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->isAssignmentOp()) {
      Visit(BO->getLHS());
      return;
    }

    if (BO->getOpcode() == BO_Comma) {
      VisitStmtExprResult(BO->getRHS());
      return;
    }
  }

  Visit(E);
}

void TransferFunctions::VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
  // Owned-element stack arrays and _Owned _ArrayElem bases:
  // element ownership transfer is only allowed inside a qualifying for-loop,
  // and the transition is applied to the whole-array aggregate state here.
  // The whole-array aggregate is addressed by the root Var place (the
  // historical "" key).
  bool IsArrElemPtr = false;
  const VarDecl *ArrVD = peelArrayBase(ASE, IsArrElemPtr);
  if (ArrVD && IsArrayTransferBase(ArrVD, IsArrElemPtr)) {
    bsc::PlaceBuilder PB(OS.ctx);
    FieldKey WholePlace = PB.Build(ArrVD, ArrVD->getLocation());
    if (op == Move || op == Assign) {
      // A null aggregate releases/clears nothing (free(null) and `= nullptr`
      // are no-ops): no qualifying-loop site is needed for a null element
      // move-out / null clear.
      if (op == Move && stat.arrayAggregateNull(ArrVD, WholePlace)) {
        Visit(ASE->getIdx());
        return;
      }
      // A clear only needs no qualifying loop when the aggregate drops
      // nothing (no path still owns): clearing a null/moved aggregate just
      // records "also null", while clearing one that may own is a transfer.
      if (op == Assign && CurRHS && CurRHS->isNullExpr(OS.ctx) &&
              stat.arrayAggregateNotOwned(ArrVD, WholePlace)) {
        Visit(ASE->getIdx());
        return;
      }
      bool Allowed = isArrayElemTransferAllowed(ASE, ArrVD, IsArrElemPtr);
      if (!Allowed) {
        Visit(ASE->getIdx());
        return;
      }
      applyArrayElemTransition(ASE->getExprLoc(), ArrVD, WholePlace, op == Assign);
    } else if (op == GetAddr || op == None) {
      // Borrowing (mutable or immutable) or reading a _Owned array that has
      // lost its ownership is forbidden: the for-loop may already have moved
      // every element out (manual: cannot borrow/use a moved _Owned value).
      bool Moved = false;
      if (stat.BOPStatus.count(ArrVD))
        Moved = stat.is(ArrVD, Ownership::Status::Moved);
      else if (stat.SStatus.count(ArrVD))
        Moved = ArrayFieldPathMoved(stat, ArrVD, WholePlace, /*IsOPS=*/false);
      if (stat.arrayAggregateUninit(ArrVD)) {
        OwnershipDiagInfo DI(ASE->getExprLoc(),
                             OwnershipDiagKind::InvalidUseOfUninit,
                             ArrVD->getNameAsString());
        reporter.addDiagInfo(DI);
        Visit(ASE->getIdx());
        return;
      }
      if (Moved) {
        OwnershipDiagInfo DI(ASE->getExprLoc(),
                             OwnershipDiagKind::InvalidUseOfMoved,
                             ArrVD->getNameAsString());
        reporter.addDiagInfo(DI);
        Visit(ASE->getIdx());
        return;
      }
      // Taking a mutable borrow (&_Mut a[i]) exposes the element to writes
      // through the borrow, so the array is no longer known to be all-null:
      // it may now own values. Promote the aggregate to owned (conservative),
      // so a scope exit without freeing reports a leak.
      if (isAddrMut)
        stat.setArrayElemOwned(ArrVD);
    }
    // Read-only accesses (comparisons, deref, borrows) do not change the
    // aggregate state; just visit the index and return.
    Visit(ASE->getIdx());
    return;
  }

  // Struct-field owned-element array: `w.arr[i]` (base is a member access,
  // so peelArrayBase above cannot see it). PeelHostAndFieldPlace yields the
  // host and the array-field path "arr[]"; the field is tracked as an owned
  // array aggregate of the host, same rules as a local owned-element array.
  {
    const bsc::Place *FieldPath = nullptr;
    if (GetOwnedArrayField(ASE, OS.ctx, FieldPath)) {
      FieldKey FieldPlace = FieldPath;
      const VarDecl *HostVD = FieldPath->getRoot();
      if (op == Move || op == Assign) {
        if (!tryTransferArrayField(ASE, HostVD, FieldPlace, op == Assign)) {
          Visit(ASE->getIdx());
          return;
        }
      } else if (op == GetAddr || op == None) {
          // Borrowing (mutable or immutable) or reading an array field whose
          // aggregate has lost its ownership is forbidden (mirror of the
          // local-array rule): the qualifying loop may already have moved every
          // element out.
          bool Moved = false;
          if (stat.SStatus.count(HostVD))
            Moved = ArrayFieldPathMoved(stat, HostVD, FieldPlace,
                                        /*IsOPS=*/false);
          else if (stat.OPSStatus.count(HostVD))
            Moved = ArrayFieldPathMoved(stat, HostVD, FieldPlace,
                                        /*IsOPS=*/true);
          if (stat.arrayAggregateUninit(HostVD)) {
            OwnershipDiagInfo DI(ASE->getExprLoc(),
                                 OwnershipDiagKind::InvalidUseOfUninit,
                                 HostVD->getNameAsString());
            reporter.addDiagInfo(DI);
          } else if (Moved) {
            OwnershipDiagInfo DI(ASE->getExprLoc(),
                                 OwnershipDiagKind::InvalidUseOfMoved,
                                 HostVD->getNameAsString());
            reporter.addDiagInfo(DI);
          } else if (isAddrMut) {
            // A mutable borrow (&_Mut w.arr[i]) exposes the field to writes
            // through the borrow, so promote the aggregate to owned.
            stat.setArrayFieldOwned(HostVD, FieldPlace);
          }
        }
        Visit(ASE->getIdx());
        return;
      }
  }

  // Collect the array levels (outermost first) and the deref suffix of the
  // base expression chain: each subscript level was one "*" in the historical
  // key suffix.
  unsigned DerefLevels = 0;
  Expr *E = ASE;
  while (ArraySubscriptExpr *ase = dyn_cast<ArraySubscriptExpr>(E)) {
    DerefLevels++;
    E = ase->getBase()->IgnoreParenImpCasts();
    ASE = ase;
  }

  if (MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
    if (FieldKey BasePlace = getMemberFieldPlace(ME, OS.ctx)) {
      bsc::PlaceBuilder PB(OS.ctx);
      FieldKey Place = BasePlace;
      // A subscript on the pointee dereferences the (pointer-valued) base,
      // like the historical "*"-suffix appended to the member field path.
      while (DerefLevels--) {
        QualType PT =
            Place->getType() ? Place->getType()->getQualType() : QualType();
        Place = PB.BuildDeref(Place, pointeeOrElem(PT), ME->getExprLoc());
      }
      VisitDeclRefExprPlace(Place, ME->getExprLoc());
      return;
    }
  }
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
    if (VD) {
      bsc::PlaceBuilder PB(OS.ctx);
      FieldKey Place = PB.Build(VD, VD->getLocation());
      while (DerefLevels--)
        Place = PB.BuildDeref(Place, pointeeOrElem(VD->getType()),
                              DRE->getLocation());
      VisitDeclRefExprPlace(Place, DRE->getLocation());
      return;
    }
  }

  Visit(ASE->getBase());
}

const VarDecl *
TransferFunctions::peelArrayBase(const Expr *E, bool &IsArrElemPtr) const {
  IsArrElemPtr = false;
  const VarDecl *VD = PeelArrayElemBase(E);
  if (!VD)
    return nullptr;
  if (IsOwnedElementArrayType(VD->getType()))
    return VD;
  if (IsOwnedArrayElemPtrTransferBase(VD->getType())) {
    IsArrElemPtr = true;
    return VD;
  }
  return nullptr;
}

bool TransferFunctions::isArrayElemTransferAllowed(const Expr *Site,
                                                   const VarDecl *ArrVD,
                                                   bool IsArrElemPtr) {
  // _Owned _ArrayElem pointers carry no length information: element transfer
  // is always forbidden.
  if (IsArrElemPtr) {
    emitArrayElemForbidden(Site->getExprLoc(), ArrVD);
    return false;
  }
  // A transfer site is legal only inside the loop whose classification recorded
  // it: AllowedTransferExprs is keyed by that loop, so a site validated by one
  // loop cannot be allowed at an occurrence enclosed by an unrelated loop. Any
  // enclosing loop may be the one that recorded it: for a multi-dim nest the
  // outermost loop records a[i][j] while the transfer sits in an inner loop. The
  // recorded expression may also be an enclosing subscript of the site (e.g. the
  // outer loop records a[i][j] for a site a[i][j].f), hence the base-chain walk.
  bool Allowed = false;
  {
    ParentMap &PM = OS.analysisContext.getParentMap();
    auto RecordedInLoop = [&](const ForStmt *F) {
      auto It = OS.LoopInfo.AllowedTransferExprs.find(F);
      if (It == OS.LoopInfo.AllowedTransferExprs.end())
        return false;
      for (const Expr *E = Site; E;) {
        if (It->second.count(E))
          return true;
        if (const ArraySubscriptExpr *A = dyn_cast<ArraySubscriptExpr>(E))
          E = A->getBase()->IgnoreParenImpCasts();
        else
          break;
      }
      return false;
    };
    for (const Stmt *S = Site; S && !Allowed;) {
      const Stmt *Parent = PM.getParent(S);
      if (!Parent)
        break;
      if (const ForStmt *F = dyn_cast<ForStmt>(Parent))
        Allowed = RecordedInLoop(F);
      S = Parent;
    }
  }
  if (!Allowed)
    emitArrayElemForbidden(Site->getExprLoc(), ArrVD);
  return Allowed;
}

bool TransferFunctions::isArrayElemForgetAllowed(const Expr *Arg) {
  Arg = Arg->IgnoreParenImpCasts();
  // Local owned-element array (arr[i], a[i] / its fields a[i].p) or a
  // T *_Owned _ArrayElem base (p[i] — element transfer is always forbidden).
  bool IsArrElemPtr = false;
  if (const VarDecl *ArrVD = peelArrayBase(Arg, IsArrElemPtr))
    return isArrayElemTransferAllowed(Arg, ArrVD, IsArrElemPtr);
  // Owned array field of a host aggregate (w.arr[i], s->arr[i].f).
  const bsc::Place *FieldPath = nullptr;
  if (GetOwnedArrayField(Arg, OS.ctx, FieldPath))
    return isArrayElemTransferAllowed(Arg, FieldPath->getRoot(),
                                      /*IsArrElemPtr=*/false);
  // Not element-shaped (p, s.p, *s, t.in): no qualifying-loop requirement.
  return true;
}

// True when the owned-field place `fieldName` of the struct-array `ArrVD`
// names a whole array-typed field (a plain single field projection directly
// under the root variable, e.g. `s[i].f` where f is an array). Field-level
// tracking treats such a field as one aggregate, so element-granularity
// use-after-move cannot be distinguished here; it is skipped (element tracking
// is a future refinement). Paths with `[]` levels or nested fields (the
// historical "arr[]" / "arr[].a" spellings) never matched a field name and
// returned false.
static bool isArrayField(const VarDecl *ArrVD, FieldKey fieldName) {
  return fieldName &&
         fieldName->getKind() == bsc::Place::Kind::Field &&
         fieldName->getBase()->getKind() == bsc::Place::Kind::Var &&
         fieldName->getType() &&
         fieldName->getType()->getQualType()->isArrayType();
}

void TransferFunctions::applyArrayElemTransition(SourceLocation Loc,
                                                  const VarDecl *ArrVD,
                                                  FieldKey fieldName,
                                                  bool IsAssign) {
  if (IsAssign)
    applyArrayElemAssign(Loc, ArrVD, fieldName);
  else
    applyArrayElemMoveOut(Loc, ArrVD, fieldName);
}

bool TransferFunctions::tryTransferArrayField(const Expr *Site,
                                              const VarDecl *HostVD,
                                              FieldKey FieldPath,
                                              bool IsAssign) {
  // A null element releases/clears nothing (free(null) and `= nullptr` are
  // no-ops): no transfer is needed and the site does not need to qualify --
  // e.g. the null branch of a `if (!s[i].arr[i]) safe_free(...)`, or clearing
  // an already-null field with `w.arr[0].q = nullptr`. A release of a field
  // that is only possibly null (Moved|Null) is not a no-op though: its moved
  // paths are already released, so the release may double free. Clearing such a
  // field (`= nullptr`) stays harmless.
  bool IsNullAssign = IsAssign && CurRHS && CurRHS->isNullExpr(OS.ctx);
  // Clearing (`= nullptr`) is a no-op when the aggregate drops nothing (no path
  // still owns it): it only records "also null". Clearing an aggregate that may
  // own is an ownership transfer, so it stays gated; a *release* of a field that
  // was only possibly null but also moved (Moved|Null) is a double free and is
  // not a no-op either (arrayElemNullNoop).
  bool NullNoop = IsAssign ? stat.arrayAggregateNotOwned(HostVD, FieldPath)
                           : stat.arrayElemNullNoop(HostVD, FieldPath);
  if (NullNoop && (!IsAssign || IsNullAssign))
    return true;
  if (!isArrayElemTransferAllowed(Site, HostVD, /*IsArrElemPtr=*/false))
    return false;
  applyArrayElemTransition(Site->getExprLoc(), HostVD, FieldPath, IsAssign);
  return true;
}

void TransferFunctions::applyArrayElemAssign(SourceLocation Loc,
                                             const VarDecl *ArrVD,
                                             FieldKey fieldName) {
  // Assigning to an element of an aggregate that still holds ownership
  // silently leaks the previous value; report it (mirror of the scalar
  // "cannot assign to owned value" rule). This covers both a null clear
  // (`= nullptr` discards the owned value) and a non-null assignment. Only
  // an exactly-owned (already initialized) aggregate triggers this: a
  // possibly-uninitialized aggregate reached through a loop back edge must
  // not.
    bool IsNullAssign = CurRHS && CurRHS->isNullExpr(OS.ctx);
    bool HoldsOwned = false;
    bool IsWholeAggregate =
        !fieldName || fieldName->getKind() == bsc::Place::Kind::Var;
    if (IsWholeAggregate) {
      if (stat.BOPStatus.count(ArrVD))
        HoldsOwned = stat.is(ArrVD, Ownership::Status::Owned) &&
                     !stat.has(ArrVD, Ownership::Status::Uninitialized);
      else if (stat.SStatus.count(ArrVD))
        HoldsOwned = !stat.SOwnedOwnedFields[ArrVD].empty() &&
                     !stat.has(ArrVD, Ownership::Status::Uninitialized);
    } else {
      if (stat.SStatus.count(ArrVD))
        HoldsOwned = stat.SOwnedOwnedFields[ArrVD].count(fieldName) &&
                     !stat.has(ArrVD, Ownership::Status::Uninitialized);
      else if (stat.OPSStatus.count(ArrVD))
        HoldsOwned = stat.OPSOwnedOwnedFields[ArrVD].count(fieldName) &&
                     !stat.has(ArrVD, Ownership::Status::Uninitialized);
    }
    if (HoldsOwned) {
      std::string Display =
          IsWholeAggregate ? ArrVD->getNameAsString() + "[]"
                           : placeToKeyString(fieldName);
      OwnershipDiagInfo DI(Loc, OwnershipDiagKind::ArrayElemAssignOwned,
                           Display);
      reporter.addDiagInfo(DI);
    }
  if (IsNullAssign) {
    if (IsWholeAggregate)
      stat.setArrayElemNull(ArrVD);
    else
      stat.setArrayFieldNull(ArrVD, fieldName);
  } else {
    if (IsWholeAggregate)
      stat.setArrayElemOwned(ArrVD);
    else
      stat.setArrayFieldOwned(ArrVD, fieldName);
  }
}

void TransferFunctions::applyArrayElemMoveOut(SourceLocation Loc,
                                              const VarDecl *ArrVD,
                                              FieldKey fieldName) {
  // Move-out: detect use-after-move. With the qualifying-loop header no longer
  // merging the back-edge, the aggregate reaches this point with the clean
  // pre-loop state, so a second move-out of the same aggregate / field is a
  // real use-after-move.
  // A move-out of a value that was already moved on some path is a
  // use-after-move even when the merge also has an owned or null path
  // (Owned|Moved from a partial release, Moved|Null from a null-checked one):
  // the element may already be freed, so transferring it out again may double
  // free. Only a state without the Moved component (pure Null owns nothing,
  // pure Owned / Uninitialized) is not already moved.
  bool AlreadyMoved = false;
  bool IsWholeAggregate =
      !fieldName || fieldName->getKind() == bsc::Place::Kind::Var;
  if (IsWholeAggregate) {
    AlreadyMoved = stat.arrayElemPossiblyMoved(ArrVD);
  } else {
    // Every tracked owned-field path (plain "a", pure array field "arr[]",
    // or nested array-field path "arr[].a") is an aggregate state in the
    // caller's per-field sets. A second move-out of the same path is a real
    // use-after-move, so detect it uniformly: explicitly moved on some path
    // (the moved field sets are unions over the paths), or gone entirely
    // (absent from both the owned and the null sets).
    if (stat.SStatus.count(ArrVD))
      AlreadyMoved = stat.SMovedOwnedFields[ArrVD].count(fieldName) ||
                     (!stat.SOwnedOwnedFields[ArrVD].count(fieldName) &&
                      !stat.SNullOwnedFields[ArrVD].count(fieldName) &&
                      !isArrayField(ArrVD, fieldName));
    else if (stat.OPSStatus.count(ArrVD))
      AlreadyMoved = stat.OPSMovedOwnedFields[ArrVD].count(fieldName) ||
                     (!stat.OPSOwnedOwnedFields[ArrVD].count(fieldName) &&
                      !stat.OPSNullOwnedFields[ArrVD].count(fieldName) &&
                      !isArrayField(ArrVD, fieldName));
  }
  // A move-out of an uninitialized aggregate / field is a use of an
  // uninitialized value (mirror of the scalar cast-uninit check), not a
  // use-after-move. Use `is` (bit test), not `has` -- `has` means "some
  // *other* bit is set besides this one", which is false for a state that is
  // exactly Uninitialized.
  bool Uninit = stat.arrayAggregateUninit(ArrVD);
  if (Uninit) {
    std::string Display =
        IsWholeAggregate ? ArrVD->getNameAsString() + "[]"
                         : placeToKeyString(fieldName);
    OwnershipDiagInfo DI(Loc, OwnershipDiagKind::InvalidCastUninit, Display);
    reporter.addDiagInfo(DI);
    return;
  }
  if (AlreadyMoved) {
    OwnershipDiagInfo DI(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                         ArrVD->getNameAsString());
    reporter.addDiagInfo(DI);
    // The value is treated as moved from here on (mirror of the scalar
    // move-out checks, which report and then set Moved): without this, the
    // already-reported double free would be re-reported as a loop-end
    // inconsistency / scope-exit leak of the same element.
    if (!fieldName || fieldName->getKind() == bsc::Place::Kind::Var)
      stat.setArrayElemMoved(ArrVD);
    else
      stat.setArrayFieldMoved(ArrVD, fieldName);
    return;
  }
  // A must-be-null aggregate (all elements initialized to null) owns nothing:
  // moving a null element out does not consume ownership (same rule as for
  // scalar _Owned pointers, 87dbd70f), so the aggregate stays Null instead of
  // becoming Moved. This allows e.g. consume(a[i]) on an all-null array to be
  // repeated, and a later borrow sees Null (may own) rather than Moved.
  bool NullTransfer = stat.arrayAggregateNull(ArrVD, fieldName);
  if (!NullTransfer) {
    if (IsWholeAggregate)
      stat.setArrayElemMoved(ArrVD);
    else
      stat.setArrayFieldMoved(ArrVD, fieldName);
  }
}

// A one-line description of why a for-loop failed the qualifying-loop rules,
// used for the note attached to ArrayElemTransferForbidden.
static std::string getNonQualifyingReasonString(NonQualifyingLoopReason R) {
  switch (R) {
  case NonQualifyingLoopReason::InitNotZero:
    return "loop variable must be initialized to zero (`T i = 0`)";
  case NonQualifyingLoopReason::InitTypeTooSmall:
    return "type of the loop variable is too small to hold the loop bound";
  case NonQualifyingLoopReason::CondNotMatch:
    return "condition must be `i < N` where N is the array length";
  case NonQualifyingLoopReason::IncrNotByOne:
    return "increment must be step one (`i++` / `++i` / `i += 1`)";
  case NonQualifyingLoopReason::LoopVarModified:
    return "loop variable must not be modified or have its address taken outside the increment";
  case NonQualifyingLoopReason::VlaBoundVarMismatch:
    return "loop bound variable must be the same variable that declares the VLA length";
  case NonQualifyingLoopReason::VlaBoundVarModified:
    return "VLA length / loop bound variable must not be modified, address-taken or mutably borrowed";
  case NonQualifyingLoopReason::EarlyExit:
    return "loop body must not break, return or goto out of the loop";
  case NonQualifyingLoopReason::BoundMismatch:
    return "loop bound must equal the array length";
  case NonQualifyingLoopReason::BoundNotConstant:
    return "loop bound must be an integer constant expression equal to the "
           "array length";
  case NonQualifyingLoopReason::ElemAccessMismatch:
    return "array accesses in the body must use the loop index (a[i])";
  case NonQualifyingLoopReason::IndexReused:
    return "one loop index must not index several array levels";
  case NonQualifyingLoopReason::NotAForLoop:
  case NonQualifyingLoopReason::NotQualifying:
    return "loop shape does not match the qualifying for-loop rules";
  }
  llvm_unreachable("unknown reason");
}

void TransferFunctions::emitArrayElemForbidden(SourceLocation Loc,
                                               const VarDecl *ArrVD) {
  OwnershipDiagInfo DI(Loc, OwnershipDiagKind::ArrayElemTransferForbidden,
                       ArrVD->getNameAsString());
  // Attach a note explaining why the transfer is rejected. Qualifying is a
  // per-array concept: a loop may qualify for one array but not another
  // (e.g. bound mismatch), so first check loop-level (array-independent)
  // failures, then per-array failures.
  const SourceManager &SM = OS.ctx.getSourceManager();
  const ForStmt *Best = nullptr;
  auto pickInnermost = [&](const ForStmt *FS) {
    SourceRange R = FS->getSourceRange();
    if (R.getBegin().isInvalid() || R.getEnd().isInvalid())
      return;
    if (SM.isPointWithin(Loc, R.getBegin(), R.getEnd())) {
      // Prefer the innermost (latest-beginning) enclosing loop.
      if (!Best || SM.isBeforeInTranslationUnit(Best->getBeginLoc(),
                                                FS->getBeginLoc()))
        Best = FS;
    }
  };
  for (const auto &KV : OS.LoopInfo.NonQualifyingLoops)
    pickInnermost(KV.first);
  for (const auto &KV : OS.LoopInfo.NonCoveredArrays)
    pickInnermost(KV.first);

  if (!Best) {
    reporter.addDiagInfo(DI);
    return;
  }
  // The note points at the recorded failure site (a return/break statement,
  // a loop-variable or bound-variable modification site, ...); fall back to
  // the for-loop keyword when the failure is at the loop head itself.
  auto NoteFor = [&](const LoopFailure &F) {
    DI.NoteLoc = F.Loc.isValid() ? F.Loc : Best->getForLoc();
    DI.Note = getNonQualifyingReasonString(F.Reason);
  };
  auto LoopIt = OS.LoopInfo.NonQualifyingLoops.find(Best);
  if (LoopIt != OS.LoopInfo.NonQualifyingLoops.end()) {
    NoteFor(LoopIt->second);
  } else {
    // Loop shape qualifies; the rejection is per-array (bound / index /
    // VLA length).
    auto ArrIt = OS.LoopInfo.NonCoveredArrays.find(Best);
    if (ArrIt != OS.LoopInfo.NonCoveredArrays.end()) {
      auto AIt = ArrIt->second.find(ArrVD);
      if (AIt != ArrIt->second.end())
        NoteFor(AIt->second);
      else
        NoteFor({NonQualifyingLoopReason::NotQualifying,
                 SourceLocation()});
    }
  }
  reporter.addDiagInfo(DI);
}

bool TransferFunctions::memberAccessTouchesOwned(
    const VarDecl *VD, FieldKey FieldPath, QualType MemberTy) const {
  auto IsOwnedConsuming = [](QualType T) {
    return T.isOrContainsOwned(BSCLookThrough::NoPointer);
  };
  if (stat.SStatus.count(VD))
    return FieldPath &&
           overlapsOwnedFields(stat.SAllOwnedFields[VD], FieldPath) &&
           IsOwnedConsuming(MemberTy);
  if (stat.OPSStatus.count(VD))
    return FieldPath &&
           overlapsOwnedFields(stat.OPSAllOwnedFields[VD], FieldPath) &&
           IsOwnedConsuming(MemberTy);
  if (stat.BOPStatus.count(VD))
    return IsOwnedConsuming(MemberTy);
  return true;
}

void TransferFunctions::VisitMemberExpr(MemberExpr *ME) {
  // Struct-array member access: s[i].f / s[i].a[j]. Ownership transfer of the
  // member is only allowed inside a qualifying loop and updates the field's
  // aggregate state. The member path is rebuilt as a host-relative place
  // (root Var + Field/Index projections, the historical "" / "f" / "arr[]"
  // string keys).
  bool IsArrElemPtr = false;
  const VarDecl *ArrVD = peelArrayBase(ME, IsArrElemPtr);
  if (ArrVD &&
      IsArrayTransferBase(ArrVD, IsArrElemPtr)) {
    bsc::PlaceBuilder PB(OS.ctx);
    FieldKey ArrFieldPlace = nullptr;
    PeelHostAndFieldPlace(PB, ME, ArrFieldPlace);
    if (op == Move || op == Assign) {
      // Assigning/moving a plain non-owned field (e.g. `a[i].x = 5` for an
      // `int x`) does not change array-element ownership and must not be
      // gated by the qualifying-loop rules.
      if (!memberAccessTouchesOwned(ArrVD, ArrFieldPlace, ME->getType()))
        return;
      // Null member releases/clears nothing (free(null) and `= nullptr` are
      // no-ops); a release of a possibly-null member that was also moved
      // (Moved|Null) is not a no-op, while clearing it remains harmless.
      if (op == Move && stat.arrayElemNullNoop(ArrVD, ArrFieldPlace))
        return;
      if (op == Assign && CurRHS && CurRHS->isNullExpr(OS.ctx) &&
          stat.arrayAggregateNotOwned(ArrVD, ArrFieldPlace))
        return;
      bool Allowed = isArrayElemTransferAllowed(ME, ArrVD, IsArrElemPtr);
      if (!Allowed)
        return;
      applyArrayElemTransition(ME->getExprLoc(), ArrVD, ArrFieldPlace, op == Assign);
    } else if (op == GetAddr || op == None) {
      // Borrowing (mutable or immutable) or reading a field of an owned-element
      // array that has lost its ownership is forbidden: the qualifying loop may
      // already have moved every element's field out.
      bool Moved = false;
      if (stat.BOPStatus.count(ArrVD))
        Moved = stat.is(ArrVD, Ownership::Status::Moved);
      else if (stat.SStatus.count(ArrVD))
        Moved = ArrayFieldPathMoved(stat, ArrVD, ArrFieldPlace,
                                    /*IsOPS=*/false);
      else if (stat.OPSStatus.count(ArrVD))
        Moved = ArrayFieldPathMoved(stat, ArrVD, ArrFieldPlace,
                                    /*IsOPS=*/true);
      if (stat.arrayAggregateUninit(ArrVD)) {
        OwnershipDiagInfo DI(ME->getExprLoc(),
                             OwnershipDiagKind::InvalidUseOfUninit,
                             ArrVD->getNameAsString());
        reporter.addDiagInfo(DI);
        return;
      }
      if (Moved) {
        OwnershipDiagInfo DI(ME->getExprLoc(),
                             OwnershipDiagKind::InvalidUseOfMoved,
                             ArrVD->getNameAsString());
        reporter.addDiagInfo(DI);
        return;
      }
      if (isAddrMut)
        stat.setArrayFieldOwned(ArrVD, ArrFieldPlace);
    }
    return;
  }

  // Nested struct-array field (w.arr[i].a): array-field paths follow the
  // qualifying-loop rules like ordinary arrays.
  bsc::PlaceBuilder PB(OS.ctx);
  const bsc::Place *FieldPath = nullptr;
  if (PeelHostAndFieldPlace(PB, ME, FieldPath) &&
      IsOwnedArrayFieldPath(FieldPath)) {
    FieldKey FieldPlace = FieldPath;
    const VarDecl *HostVD = FieldPath->getRoot();
    if (op == Move || op == Assign) {
      // A plain non-owned member of an array element (e.g. `w.arr[i].x = 5`
      // for an `int x`) does not change ownership; only paths that touch a
      // tracked owned field need the qualifying-loop gate.
      if (!memberAccessTouchesOwned(HostVD, FieldPlace, ME->getType()))
        return;
      tryTransferArrayField(ME, HostVD, FieldPlace, op == Assign);
    } else if (op == GetAddr || op == None) {
      // Same borrow/read rule as the local array: borrowing or reading an
      // array-field path whose aggregate has lost ownership is forbidden.
      bool Moved = false;
      if (stat.SStatus.count(HostVD))
        Moved = ArrayFieldPathMoved(stat, HostVD, FieldPlace,
                                    /*IsOPS=*/false);
      else if (stat.OPSStatus.count(HostVD))
        Moved = ArrayFieldPathMoved(stat, HostVD, FieldPlace,
                                    /*IsOPS=*/true);
      if (stat.arrayAggregateUninit(HostVD)) {
        OwnershipDiagInfo DI(ME->getExprLoc(),
                             OwnershipDiagKind::InvalidUseOfUninit,
                             HostVD->getNameAsString());
        reporter.addDiagInfo(DI);
      } else if (Moved) {
        OwnershipDiagInfo DI(ME->getExprLoc(),
                             OwnershipDiagKind::InvalidUseOfMoved,
                             HostVD->getNameAsString());
        reporter.addDiagInfo(DI);
      } else if (isAddrMut) {
        stat.setArrayFieldOwned(HostVD, FieldPlace);
      }
    }
    return;
  }

  // manipulate struct member expr assign / use: the field place is the
  // root-relative Field chain (`(*p).f` and `p->f` both give Field(Var(p),f))
  FieldKey MemberFieldPlace = getMemberFieldPlace(ME, OS.ctx);
  if (MemberFieldPlace) {
    if (op == Assign)
      HandleDREPlaceAssign(MemberFieldPlace, ME->getExprLoc());
    if (op == Move || op == GetAddr)
      HandleDREPlaceUse(MemberFieldPlace, ME->getExprLoc());
  }
}

void TransferFunctions::VisitDeclRefExpr(DeclRefExpr *DRE) {
  const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
  if (!VD)
    return;
  bsc::PlaceBuilder PB(OS.ctx);
  FieldKey VarPlace = PB.Build(VD, VD->getLocation());
  if (op == Assign) {
    HandleDREPlaceAssign(VarPlace, DRE->getLocation());
  }

  if (op == Move || op == GetAddr) {
    HandleDREPlaceUse(VarPlace, DRE->getLocation());
  }
}

void TransferFunctions::VisitDeclRefExprPlace(FieldKey Place,
                                              SourceLocation Loc) {
  if (op == Assign) {
    HandleDREPlaceAssign(Place, Loc);
  }

  if (op == Move || op == GetAddr) {
    HandleDREPlaceUse(Place, Loc);
  }
}

void TransferFunctions::VisitUnaryOperator(UnaryOperator *UO) {
  unsigned DerefLevels = 0;
  Expr *E = UO;
  while (UO->getOpcode() == UO_Deref) {
    if (ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(E)) {
      E = ICE->getSubExpr();
    } else if (UnaryOperator *uo = dyn_cast<UnaryOperator>(E)) {
      UO = uo;
      DerefLevels++;
      E = UO->getSubExpr();
    } else {
      break;
    }
    E = E->IgnoreParens();
  }

  if (MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
    if (FieldKey BasePlace = getMemberFieldPlace(ME, OS.ctx)) {
      bsc::PlaceBuilder PB(OS.ctx);
      FieldKey Place = BasePlace;
      while (DerefLevels--) {
        QualType PT =
            Place->getType() ? Place->getType()->getQualType() : QualType();
        Place = PB.BuildDeref(Place, pointeeOrElem(PT), ME->getExprLoc());
      }
      VisitDeclRefExprPlace(Place, ME->getExprLoc());
      return;
    }
  }
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
    if (VD) {
      bsc::PlaceBuilder PB(OS.ctx);
      FieldKey Place = PB.Build(VD, VD->getLocation());
      while (DerefLevels--)
        Place = PB.BuildDeref(Place, pointeeOrElem(VD->getType()),
                              DRE->getLocation());
      VisitDeclRefExprPlace(Place, DRE->getLocation());
      return;
    }
  }
  // Write-through / read via deref does not transfer ownership of the pointer
  // (e.g. *p[i].ptr = 42 on an _Owned _ArrayElem base, or *a[i] = 42 inside a
  // qualifying loop on an owned-element array).
  if (UO->getOpcode() == UO_Deref && (op == Assign || op == Move)) {
    Operation Saved = op;
    op = GetAddr;
    Visit(UO->getSubExpr());
    op = Saved;
    return;
  }
  if (UO->getOpcode() == UO_AddrConstDeref ||
      UO->getOpcode() == UO_AddrMutDeref ||
      UO->getOpcode() == UO_AddrConst ||
      UO->getOpcode() == UO_AddrMut ||
      UO->getOpcode() == UO_AddrOf) {
    if (UO->getOpcode() == UO_AddrMut) {
      isAddrMut = true;
    }
    op = GetAddr;
  } else if (UO->isIncrementDecrementOp()) {
    // Re-visit: see the assignment case below.
    if (UO != TransferStmt)
      return;
    op = Assign;
  }
  Visit(UO->getSubExpr());
  op = None;
}

void TransferFunctions::VisitBinaryOperator(BinaryOperator *BO) {
  if (BO->isAssignmentOp()) {
    // A consuming re-visit uses the assignment's value — the LHS after the
    // store — so route to it (as VisitStmtExprResult does); every other
    // re-visit skips: the transfer ran at this assignment's own element.
    if (BO != TransferStmt) {
      if (op == Move)
        Visit(BO->getLHS());
      return;
    }
    Expr *LHS = BO->getLHS();
    Expr *RHS = BO->getRHS();

    // Peek at RHS before visiting: if it refers to a Null-state owned
    // variable, propagate Null to the destination after assignment.
    bool RHSFromNull = isExprRefToNullOwnedVar(RHS);

    // Assignment consumes RHS only when destination is move semantic.
    QualType LHSType = LHS->getType().getCanonicalType();
    op = IsTrackedType(LHSType) ? Move : GetAddr;
    Visit(RHS);
    op = None;

    op = Assign;
    CurRHS = RHS;
    Visit(LHS);
    CurRHS = nullptr;
    op = None;

    if (RHS->isNullExpr(OS.ctx) || RHSFromNull) {
      stat.setToNull(LHS, OS.ctx);
    } else if (IsCastFromVoidPointer(RHS)) {
      stat.setToAllMoved(LHS, OS.ctx);
    }
  } else {
    // for operators doesn't consume ownership, change the operation to GetAddr
    // instead of inheritance from parent operation
    Operation VisitMode =
        BO->isComparisonOp() || BO->isLogicalOp() || BO->isAdditiveOp()
            ? GetAddr
            : op;
    op = VisitMode;
    if (BO->getOpcode() == BO_Comma) {
      // comma operator is not symmetric, the LHS never consumes ownership
      op = GetAddr;
      Visit(BO->getLHS());
    } else {
      Visit(BO->getLHS());
    }
    op = VisitMode;
    Visit(BO->getRHS());
  }
}

/// Compound assignment (`x += y`) reads LHS before writing it. Visit LHS with
/// a use-checking op first, then fall through to the regular assignment path.
void TransferFunctions::VisitCompoundAssignOperator(
    CompoundAssignOperator *CAO) {
  Expr *LHS = CAO->getLHS();
  Operation Saved = op;
  op = GetAddr;
  Visit(LHS);
  op = Saved;
  VisitBinaryOperator(CAO);
}

void TransferFunctions::VisitAbstractConditionalOperator(
    AbstractConditionalOperator *ACO) {
  Operation Inherited = op;

  op = GetAddr;
  Visit(ACO->getCond());
  Ownership::OwnershipStatus StatAfterCond = stat;

  op = Inherited;
  Visit(ACO->getTrueExpr());
  Ownership::OwnershipStatus StatAfterTrue = stat;

  stat = StatAfterCond;
  op = Inherited;
  Visit(ACO->getFalseExpr());

  stat = OS.merge(StatAfterTrue, stat);
  op = None;
}

/// GNU `x ?: y` reuses the common value (x) via an OpaqueValueExpr whose
/// children are empty — visiting the OpaqueValueExpr directly records no move.
void TransferFunctions::VisitBinaryConditionalOperator(
    BinaryConditionalOperator *BCO) {
  Operation Inherited = op;

  op = GetAddr;
  Visit(BCO->getCond());
  Ownership::OwnershipStatus StatAfterCond = stat;

  op = Inherited;
  if (auto *OVE = dyn_cast<OpaqueValueExpr>(BCO->getTrueExpr()))
    if (Expr *Src = OVE->getSourceExpr())
      Visit(Src);
  Ownership::OwnershipStatus StatAfterTrue = stat;

  stat = StatAfterCond;
  op = Inherited;
  Visit(BCO->getFalseExpr());

  stat = OS.merge(StatAfterTrue, stat);
  op = None;
}

void TransferFunctions::VisitCallExpr(CallExpr *CE) {
  // A call's semantics run as its own element, or inline as an argument of
  // the call being processed (ArgCalls) — nowhere else.
  if (TransferStmt != CE && !OS.ArgCalls.count(CE))
    return;

  // __assume_null: the callee promises the _Nullable owned pointer has been
  // freed and set to null by an opaque legacy API the analyzer cannot model.
  // Unlike a normal Move (which records Moved and still flags a leak at scope
  // end), set the pointer to Null: no ownership held, no leak, and a later
  // use reports use-of-null. This must run before the generic argument Move
  // loop below, which would otherwise move the owned pointer out.
  if (FunctionDecl *Callee = CE->getDirectCallee()) {
    if (Callee->getBuiltinID() == Builtin::BI__assume_null) {
      if (CE->getNumArgs() == 1)
        stat.setToNull(CE->getArg(0), OS.ctx);
      return;
    }
  }

  // __forget: the user explicitly drops ownership of an _Owned pointer,
  // promising it will neither be freed nor moved again. Set the pointer to
  // Moved: no leak at scope end, no double-free, and a later use reports
  // use-of-moved. This must run before the generic argument Move loop below,
  // which would otherwise move the owned pointer out.
  if (FunctionDecl *Callee = CE->getDirectCallee()) {
    if (Callee->getBuiltinID() == Builtin::BI__forget) {
      if (CE->getNumArgs() == 1) {
        // Forgetting an element of an owned-element array (arr[i], w.arr[i],
        // a[i].p) follows the same qualifying-loop rule as any other element
        // transfer (safe_free / move-out / assignment). When forbidden, the
        // diagnostic has been emitted and the aggregate must not move.
        if (!isArrayElemForgetAllowed(CE->getArg(0)))
          return;
        stat.setToMoved(CE->getArg(0), OS.ctx);
      }
      return;
    }
  }

  for (auto it = CE->arg_begin(), ei = CE->arg_end(); it != ei; ++it) {
    Expr *Arg = *it;
    if (IsCastFromVoidPointer(Arg) &&
        Arg->getType()->containsOwned(BSCLookThrough::AnyPointer)) {
      SmallVector<OwnershipDiagInfo> diags;
      diags.push_back(OwnershipDiagInfo(Arg->getExprLoc(),
                                        OwnershipDiagKind::PassCastToArgOrRet,
                                        Arg->getType()));
      reporter.addDiags(diags);
    }
    // Passing a whole owned-element array as an argument would transfer
    // ownership of the entire aggregate. Only per-element transfers inside a
    // qualifying for-loop are allowed, so a whole-array argument is rejected
    // at the call site (and must not silently move the aggregate). This
    // covers both local arrays (arr) and struct-field arrays (w.arr).
    // The argument may be an ArrayToPointerDecay implicit cast (w.arr decays
    // to `int *_Owned *` when passed), so look through it to the array-typed
    // expression.
    const Expr *ArgCore = Arg->IgnoreParenImpCasts();
    if (IsOwnedElementArrayType(ArgCore->getType())) {
      const VarDecl *ArrVD = nullptr;
      if (const DeclRefExpr *ArgDRE = dyn_cast<DeclRefExpr>(ArgCore))
        ArrVD = dyn_cast<VarDecl>(ArgDRE->getDecl());
      else {
        bsc::PlaceBuilder PB(OS.ctx);
        const bsc::Place *FieldPath = nullptr;
        if (PeelHostAndFieldPlace(PB, ArgCore, FieldPath) &&
            FieldPath->getKind() != bsc::Place::Kind::Var)
          ArrVD = FieldPath->getRoot();
      }
      if (ArrVD) {
        OwnershipDiagInfo DI(Arg->getExprLoc(),
                             OwnershipDiagKind::ArrayElemTransferForbidden,
                             ArrVD->getNameAsString());
        reporter.addDiagInfo(DI);
      }
      continue;
    }
    // Passing an argument consumes ownership only when the argument's type
    // carries ownership itself, otherwise is a copy and must not move the arg
    // (a _Bool parameter is not tracked, so this also covers the old
    // `isBooleanType ? GetAddr : Move` special case).
    op = IsTrackedType(Arg->getType()) ? Move : GetAddr;
    Visit(Arg);
    op = None;
  }
}

/// UnaryExprOrTypeTraitExpr - expression with either a type or (unevaluated) expression operand.
/// Used for sizeof/alignof (C99 6.5.3.4) and vec_step (OpenCL 1.1 6.11.12).
/// 'alignof/_Alignof' applied to an expression is a GNU extension.
void TransferFunctions::VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr* UE) {
  return;
}

void TransferFunctions::VisitCStyleCastExpr(CStyleCastExpr *CSCE) {
  if (CSCE->getType()->isVoidPointerType() &&
      CSCE->getType().isOwnedPointer()) {
    // Read-only walk: inspect the operand without committing its move.
    if (!TransferStmt) {
      Operation SavedOp = op;
      op = GetAddr;
      Visit(CSCE->getSubExpr());
      op = SavedOp;
      return;
    }

    // ignore explicit/implicit casts, get canonical expr
    const Expr *InnerE = CSCE->getSubExpr()->IgnoreParenCasts();
    // The value of a comma expression is its RHS; keep peeling nested comma RHS.
    while (const BinaryOperator *BO = dyn_cast<BinaryOperator>(InnerE)) {
      if (BO->getOpcode() != BO_Comma)
        break;
      InnerE = BO->getRHS()->IgnoreParenCasts();
    }

    // @code
    // (void * owned)s
    // @endcode
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(InnerE)) {
      const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
      if (stat.OPSStatus.count(VD)) {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkCastOPS(VD, DRE->getLocation());
        reporter.addDiags(diags);
      }
      if (stat.BOPStatus.count(VD)) {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkCastBOP(VD, DRE->getLocation());
        reporter.addDiags(diags);
      }
    }
    // @code
    // (void * owned)s->p
    // (void * owned)s.p
    // @endcode
    else if (const MemberExpr *ME = dyn_cast<MemberExpr>(InnerE)) {
      // Struct-array member move-out: (void *_Owned)s[i].f.
      bool IsArrElemPtr = false;
      const VarDecl *ArrVD = peelArrayBase(ME, IsArrElemPtr);
      if (ArrVD &&
          IsArrayTransferBase(ArrVD, IsArrElemPtr)) {
        bsc::PlaceBuilder PB(OS.ctx);
        FieldKey MemberFieldPlace = nullptr;
        PeelHostAndFieldPlace(PB, ME, MemberFieldPlace);
        // Null member releases nothing (free(null) is a no-op); a
        // possibly-null member that was also moved is not a no-op.
        if (!stat.arrayElemNullNoop(ArrVD, MemberFieldPlace)) {
          bool Allowed = isArrayElemTransferAllowed(ME, ArrVD, IsArrElemPtr);
          if (Allowed)
            applyArrayElemTransition(CSCE->getExprLoc(), ArrVD,
                                     MemberFieldPlace, /*IsAssign=*/false);
        }
        return;
      }
      // Nested struct-array field (e.g. (void *_Owned)w.arr[0].a): peel the
      // host variable and the "arr[].a" path. Array-field paths (containing
      // `[]`) follow the qualifying-loop rules like ordinary arrays; plain
      // nested fields use the regular field move-out.
      bsc::PlaceBuilder PB(OS.ctx);
      const bsc::Place *HostFieldPath = nullptr;
      if (PeelHostAndFieldPlace(PB, ME, HostFieldPath)) {
        FieldKey HostFieldPlace = HostFieldPath;
        const VarDecl *HostVD = HostFieldPath->getRoot();
        if (IsOwnedArrayFieldPath(HostFieldPath)) {
          tryTransferArrayField(ME, HostVD, HostFieldPlace,
                                /*IsAssign=*/false);
        } else if (HostFieldPlace) {
          SmallVector<OwnershipDiagInfo> diags =
              stat.checkCastField(HostVD, ME->getExprLoc(), HostFieldPlace);
          reporter.addDiags(diags);
        }
        return;
      }
      if (FieldKey MemberFieldPlace = getMemberFieldPlace(ME, OS.ctx)) {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkCastField(MemberFieldPlace->getRoot(),
                                ME->getExprLoc(), MemberFieldPlace);
        reporter.addDiags(diags);
      }
    }
    // @code
    // (void * owned)arr[i]
    // @endcode
    else if (const ArraySubscriptExpr *ASE =
                 dyn_cast<ArraySubscriptExpr>(InnerE)) {
      bool IsArrElemPtr = false;
      const VarDecl *ArrVD = peelArrayBase(ASE, IsArrElemPtr);
      if (ArrVD &&
          IsArrayTransferBase(ArrVD, IsArrElemPtr)) {
        // Null aggregate releases nothing (free(null) is a no-op).
        bsc::PlaceBuilder PB(OS.ctx);
        FieldKey WholePlace = PB.Build(ArrVD, ArrVD->getLocation());
        if (!stat.arrayAggregateNull(ArrVD, WholePlace)) {
          bool Allowed = isArrayElemTransferAllowed(ASE, ArrVD, IsArrElemPtr);
          if (Allowed)
            applyArrayElemTransition(CSCE->getExprLoc(), ArrVD, WholePlace,
                                     /*IsAssign=*/false);
        }
        return;
      }
      // Struct-field owned-element array: (void *_Owned)w.arr[i].
      {
        const bsc::Place *FieldPath = nullptr;
        if (GetOwnedArrayField(ASE, OS.ctx, FieldPath)) {
          FieldKey FieldPlace = FieldPath;
          const VarDecl *HostVD = FieldPath->getRoot();
          if (!stat.arrayElemNullNoop(HostVD, FieldPlace)) {
            bool Allowed = isArrayElemTransferAllowed(ASE, HostVD,
                                                      /*IsArrElemPtr=*/false);
            if (Allowed)
              applyArrayElemTransition(CSCE->getExprLoc(), HostVD, FieldPlace,
                                       /*IsAssign=*/false);
          }
          return;
        }
      }
      Visit(const_cast<ArraySubscriptExpr *>(ASE));
    }
    // @code
    // (void * owned)*outer
    // (void * owned)**ultraout
    // @endcode
    else if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(InnerE)) {
      // Count dereferences and find the base DeclRefExpr; each deref is one
      // Deref projection over the root variable (the historical "*", "**"...).
      const Expr *E = UO;
      llvm::SmallVector<QualType, 4> DerefTys;
      while (const UnaryOperator *U = dyn_cast<UnaryOperator>(E)) {
        if (U->getOpcode() == UO_Deref) {
          DerefTys.push_back(U->getType());
          E = U->getSubExpr()->IgnoreParenImpCasts();
        } else {
          break;
        }
      }
      if (const DeclRefExpr *DRE =
              dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts())) {
        const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
        if (VD) {
          bsc::PlaceBuilder PB(OS.ctx);
          FieldKey Place = PB.Build(VD, VD->getLocation());
          for (QualType DerefTy : llvm::reverse(DerefTys))
            Place = PB.BuildDeref(Place, DerefTy, DRE->getLocation());
          SmallVector<OwnershipDiagInfo> diags =
              stat.checkCastField(VD, DRE->getLocation(), Place);
          reporter.addDiags(diags);
        }
      }
    }
    // @code
    // (void * owned)mkNested()
    // @endcode
    else if (InnerE->getType()->containsOwned(BSCLookThrough::AnyPointer)) {
      QualType InnerTy = InnerE->getType();
      SmallVector<OwnershipDiagInfo> diags;
      diags.push_back(OwnershipDiagInfo(
          InnerE->getExprLoc(), OwnershipDiagKind::InvalidCastFieldOwned,
          InnerTy.getAsString(),
          InnerTy->getPointeeType().getAsString() + " is"));
      reporter.addDiags(diags);
      Visit(CSCE->getSubExpr());
    }
    // if the canonical node is not handled, continue traverse to avoid breaking
    // visit
    else {
      Visit(CSCE->getSubExpr());
    }
    return;
  }
  Operation SavedOp = op;
  // Casting to an integer type doesn't consume ownership
  if (CSCE->getType()->isIntegerType()) {
    op = GetAddr;
  }
  Visit(CSCE->getSubExpr());
  op = SavedOp;
}

void TransferFunctions::VisitDeclStmt(DeclStmt *DS) {
  for (Decl *D : DS->decls()) {
    if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
      // BSC canonicalizes arrays by canonicalizing the element type, which
      // drops the _Owned qualifier (it lives on the sugar type), so keep the
      // sugared type for the owned-element-array checks.
      QualType VQT = VD->getType().getCanonicalType();
      QualType SugaredVQT = VD->getType();
      if (IsTrackedType(SugaredVQT)) {
        stat.init(VD, OS.ctx);
      }
      if (Expr *Init = VD->getInit()) {
        // if has init expr, change the status of VD from UNINIT to OWNED or NULL
        if (VQT.isOwnedPointer()) {
          // Null-state propagation takes priority over the void* cast shape:
          // `(T *_Owned)(void *_Owned)p` from a must-be-null p is still null.
          if (Init->isNullExpr(OS.ctx) || isExprRefToNullOwnedVar(Init)) {
            stat.setToNull(VD);
          } else if (IsCastFromVoidPointer(Init)) {
            stat.setToAllMoved(VD);
          } else {
            stat.setToOwned(VD);
          }
        } else if (SugaredVQT->isArrayType() &&
                   IsOwnedElementArrayType(SugaredVQT)) {
          // Whole-array init: an empty / all-null initializer
          // means no element owns anything; any non-null initializer makes
          // the aggregate owned. Use the sugared element type so the _Owned
          // qualifier survives.
          QualType ElemTy = SugaredVQT;
          while (const auto *AT = ElemTy->getAsArrayTypeUnsafe())
            ElemTy = AT->getElementType();
          if (const auto *ILE = dyn_cast<InitListExpr>(Init)) {
            NullInitTest IsNullState = stateNullInitTest();
            if (ILE->getNumInits() == 0 ||
                isAllNullInits(OS.ctx, ILE, ElemTy, IsNullState)) {
              stat.setArrayElemNull(VD);
            } else if (const RecordType *RT =
                           dyn_cast<RecordType>(ElemTy.getCanonicalType())) {
              // Mixed / non-null struct-element array initializer: each owned
              // field is tracked independently, so a field that is null in
              // every element is recorded as null even when another field
              // owns values.
              stat.setArrayElemOwned(VD);
              NullAllNullStructArrayFields(stat, OS.ctx, VD, RT->getDecl(),
                                           ILE, IsNullState);
            } else {
              stat.setArrayElemOwned(VD);
            }
          } else {
            stat.setArrayElemOwned(VD);
          }
        } else if (VQT->isRecordType() &&
                   VQT->containsOwned(BSCLookThrough::AnyPointer) &&
                   isa<InitListExpr>(Init)) {
          stat.setToOwned(VD);
          if (stat.SStatus.count(VD)) {
            // If we init owned fields of a struct var with nullptr,
            // for example `struct S s = { .p = nullptr };`, here
            // we reset the status of s.p.
            RecordDecl *RD = dyn_cast<RecordType>(VQT)->getDecl();
            InitListExpr *ILE = dyn_cast<InitListExpr>(Init);
            HandleInitListExpr(VD, RD, ILE);
            if (stat.SOwnedOwnedFields[VD].size() == 0) {
              stat.set(VD, Ownership::Status::AllMoved);
            } else if (stat.SAllOwnedFields[VD].size() != stat.SOwnedOwnedFields[VD].size()) {
              stat.set(VD, Ownership::Status::PartialMoved);
            }
          }
        } else {
          stat.setToOwned(VD);
        }
        // Initialization consumes RHS only for move-semantic destination types.
        op = IsTrackedType(VQT) ? Move : GetAddr;
        Visit(Init);
        op = None;
      }
    }
  }
}

void TransferFunctions::HandleInitListExpr(VarDecl *VD, RecordDecl *RD,
                                           InitListExpr *ILE,
                                           FieldKey fullFieldName) {
  // unexplicitly initialized fields are implicitly initialized automatically
  Expr **Inits = ILE->getInits();
  // In semantic InitListExpr, omitted fields are ImplicitValueInitExpr both for
  // `{}` and for partial initializers like `{.a = 1}`. Only `{}` null-inits the
  // whole nested record family here; partial omission should keep leak checking.
  bool IsEmptyInitList = ILE->isSemanticForm() && ILE->getSyntacticForm() &&
                         ILE->getSyntacticForm()->getNumInits() == 0;

  auto markAsNull = [this, VD](FieldKey fieldName) {
    stat.SOwnedOwnedFields[VD].erase(fieldName);
    stat.SNullOwnedFields[VD].insert(fieldName);
  };

  auto markFieldDescendantsAsNull = [this, VD, &markAsNull](FieldKey prefix) {
    for (FieldKey str : fieldDescendants(stat.SAllOwnedFields[VD], prefix))
      markAsNull(str);
  };

  NullInitTest IsNullState = stateNullInitTest();

  auto markDerefDescendantsAsNull = [this, VD, &markAsNull](FieldKey prefix) {
    for (FieldKey str : derefDescendants(stat.SAllOwnedFields[VD], prefix))
      markAsNull(str);
  };

  // The historical bare `findPrefixStrings(SAll, key)`: the key itself plus
  // every descendant under any projection (the array-field "arr[]" query,
  // which picks up "arr[]" itself and "arr[].a").
  auto markPrefixAsNull = [this, VD, &markAsNull](FieldKey prefix) {
    for (FieldKey str : stat.SAllOwnedFields[VD])
      if (str->hasPrefix(prefix))
        markAsNull(str);
  };
  for (const auto &FD : RD->fields()) {
    Expr *FieldInit = Inits[FD->getFieldIndex()];
    bsc::PlaceBuilder PB(OS.ctx);
    // The historical fullFieldName chain was "" at the top level and
    // "parent.child" below; the place equivalent builds Field projections
    // from the root variable (a null fullFieldName means the root Var).
    FieldKey newFullFieldName = PB.BuildField(
        fullFieldName ? fullFieldName : PB.Build(VD, VD->getLocation()), FD,
        FD->getLocation());
    bool IsTrackedOwnedField = stat.SAllOwnedFields[VD].count(newFullFieldName);
    bool IsImplicitValueInit = isa<ImplicitValueInitExpr>(FieldInit);

    // Array fields (e.g. int *_Owned arr[2] or struct S arr[2]) are tracked
    // with "[]" levels ("arr[]" / "arr[].a"). A null / zero / omitted
    // initializer makes the whole array field own nothing; otherwise it is
    // left in the owned state established by setToOwned (conservative for
    // partial / non-null initializers).
    FieldKey ArrFieldPlace = newFullFieldName;
    QualType FieldTy;
    if (StripArrayFieldLevels(PB, FD->getType(), ArrFieldPlace,
                              FD->getLocation(), FieldTy)) {
      bool IsNullInit = IsImplicitValueInit || IsNullState(FieldInit) ||
                        FieldInit->isNullExpr(OS.ctx);
      if (!IsNullInit)
        if (const InitListExpr *FieldILE = dyn_cast<InitListExpr>(FieldInit))
          IsNullInit = isAllNullInits(OS.ctx, FieldILE, FieldTy, IsNullState);
      if (IsNullInit)
        markPrefixAsNull(ArrFieldPlace);
      continue;
    }

    // allow ImplicitValueInit, e.g. struct S s = {0}; and a null value moved
    // in through a variable, e.g. `struct S s = {null_v}` (the assignment
    // path folds the same case for `s.p = null_v`).
    if (IsImplicitValueInit || IsNullState(FieldInit) ||
        FieldInit->isNullExpr(OS.ctx)) {
      if (IsTrackedOwnedField) {
        // For `int *_Owned _Nullable *_Owned _Nullable f`, null-init also
        // covers deref-derived keys like "f*" (the internal key for `*f`).
        markAsNull(newFullFieldName);
        markFieldDescendantsAsNull(newFullFieldName);
        markDerefDescendantsAsNull(newFullFieldName);
      } else if (IsImplicitValueInit && IsEmptyInitList) {
        // For `struct Outer o = {};`, the record field itself (e.g. "inner")
        // may not be tracked, but its owned descendants ("inner.f") are.
        markFieldDescendantsAsNull(newFullFieldName);
      }
      continue;
    }

    if (InitListExpr *FieldILE = dyn_cast<InitListExpr>(FieldInit)) {
      QualType QT = FieldInit->getType().getCanonicalType();
      if (QT->isRecordType() && QT->containsOwned(BSCLookThrough::AnyPointer)) {
        RecordDecl *FieldRD = dyn_cast<RecordType>(QT)->getDecl();
        HandleInitListExpr(VD, FieldRD, FieldILE, newFullFieldName);
      }
      continue;
    }

    if (IsCastFromVoidPointer(FieldInit) && IsTrackedOwnedField) {
      stat.SOwnedOwnedFields[VD].insert(newFullFieldName);
      stat.SNullOwnedFields[VD].erase(newFullFieldName);
      for (FieldKey str : fieldDescendants(stat.SAllOwnedFields[VD],
                                           newFullFieldName)) {
        stat.SOwnedOwnedFields[VD].erase(str);
        stat.SNullOwnedFields[VD].erase(str);
      }
    }
  }
}

void TransferFunctions::VisitInitListExpr(InitListExpr *ILE) {
  for (auto *Init : ILE->inits()) {
    op = IsTrackedType(Init->getType()) ? Move : GetAddr;
    Visit(Init);
    op = None;
  }
}

void TransferFunctions::VisitReturnStmt(ReturnStmt *RS) {
  if (Expr *RV = RS->getRetValue()) {
    if (IsCastFromVoidPointer(RV) &&
        RV->getType()->containsOwned(BSCLookThrough::AnyPointer)) {
      SmallVector<OwnershipDiagInfo> diags;
      diags.push_back(OwnershipDiagInfo(RV->getExprLoc(),
                                        OwnershipDiagKind::PassCastToArgOrRet,
                                        RV->getType()));
      reporter.addDiags(diags);
    }
    op = IsTrackedType(RV->getType()) ? Move : GetAddr;
    Visit(RV);
    op = None;
  }
}

void TransferFunctions::VisitLifetimeEnds(VarDecl *VD, SourceLocation SL,
                                          bool isDestructor) {
  SmallVector<OwnershipDiagInfo> diags =
      stat.checkMemoryLeak(VD, SL, isDestructor);
  reporter.addDiags(diags);
}

void TransferFunctions::HandleDREPlaceAssign(FieldKey Place,
                                              SourceLocation Loc) {
  // for a field-path assign, there is 6 situations:
  // 1. assign a struct owned pointer as a whole, i.e. Place is the Var
  // 2. assign a struct owned pointer's field, i.e. Place is a Field chain
  // 3. assign a struct as a whole
  // 4. assign a struct's owned field
  // 5. assign a basic owned pointer as a whole
  // 6. assign a basic owned pointer's owned field
  const VarDecl *VD = Place->getRoot();
  // situation 1 and 2
  if (stat.OPSStatus.count(VD)) {
    if (Place->getKind() == bsc::Place::Kind::Var) {
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkOPSAssign(VD, Loc);
      reporter.addDiags(diags);
    } else if (Place->getKind() == bsc::Place::Kind::Deref &&
               Place->getBase()->getKind() == bsc::Place::Kind::Var) {
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkOPSDerefAssign(VD, Loc);
      reporter.addDiags(diags);
    } else {
      // The historical trailing-'*' trick first re-checked the key with the
      // trailing star replaced by a '.' (the aggregate under the pointer,
      // i.e. the deref's base) when that aggregate is tracked, then always
      // re-checked the key as-is.
      if (Place->getKind() == bsc::Place::Kind::Deref) {
        FieldKey Aggregate = Place->getBase();
        if (stat.OPSAllOwnedFields[VD].count(Aggregate)) {
          SmallVector<OwnershipDiagInfo> diags =
              stat.checkOPSFieldAssign(VD, Loc, Aggregate,
                                       /*IsAggregateAccess=*/true);
          reporter.addDiags(diags);
        }
      }
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkOPSFieldAssign(VD, Loc, Place);
      reporter.addDiags(diags);
    }
  }

  // situation 3 and 4
  if (stat.SStatus.count(VD)) {
    if (Place->getKind() == bsc::Place::Kind::Var) {
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkSAssign(VD, Loc);
      reporter.addDiags(diags);
    } else {
      if (Place->getKind() == bsc::Place::Kind::Deref) {
        FieldKey Aggregate = Place->getBase();
        if (stat.SAllOwnedFields[VD].count(Aggregate)) {
          SmallVector<OwnershipDiagInfo> diags =
              stat.checkSFieldAssign(VD, Loc, Aggregate,
                                     /*IsAggregateAccess=*/true);
          reporter.addDiags(diags);
        }
      }
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkSFieldAssign(VD, Loc, Place);
      reporter.addDiags(diags);
    }
  }

  // situation 5 and 6
  if (stat.BOPStatus.count(VD)) {
    if (Place->getKind() == bsc::Place::Kind::Var) {
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkBOPAssign(VD, Loc);
      reporter.addDiags(diags);
    } else {
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkBOPFieldAssign(VD, Loc, Place);
      reporter.addDiags(diags);
    }
  }
}

void TransferFunctions::HandleDREPlaceUse(FieldKey Place, SourceLocation Loc) {
  // for a field-path use, there is 6 situations:
  // 1. use a struct owned pointer as a whole, i.e. Place is the Var
  // 2. use a struct owned pointer's field, i.e. Place is a Field chain
  // 3. use a struct as a whole
  // 4. use a struct's owned field
  // 5. use a basic owned pointer as a whole
  // 6. use a basic owned pointer's owned field
  const VarDecl *VD = Place->getRoot();
  // situation 1 and 2
  if (stat.OPSStatus.count(VD)) {
    if (Place->getKind() == bsc::Place::Kind::Var) {
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkOPSUse(VD, Loc, op == GetAddr, false, isAddrMut);
      isAddrMut = false;
      reporter.addDiags(diags);
    } else if (Place->getKind() == bsc::Place::Kind::Deref &&
               Place->getBase()->getKind() == bsc::Place::Kind::Var) {
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkOPSUse(VD, Loc, op == GetAddr, true, false);
      reporter.addDiags(diags);
    } else {
      if (Place->getKind() == bsc::Place::Kind::Deref) {
        FieldKey Aggregate = Place->getBase();
        if (stat.OPSAllOwnedFields[VD].count(Aggregate)) {
          SmallVector<OwnershipDiagInfo> diags = stat.checkOPSFieldUse(
              VD, Loc, Aggregate, op == GetAddr,
              /*IsAggregateAccess=*/true);
          reporter.addDiags(diags);
        }
      }
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkOPSFieldUse(VD, Loc, Place, op == GetAddr);
      reporter.addDiags(diags);
    }
  }

  // situation 3 and 4
  if (stat.SStatus.count(VD)) {
    if (Place->getKind() == bsc::Place::Kind::Var) {
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkSUse(VD, Loc, op == GetAddr, isAddrMut);
      isAddrMut = false;
      reporter.addDiags(diags);
    } else {
      if (Place->getKind() == bsc::Place::Kind::Deref) {
        FieldKey Aggregate = Place->getBase();
        if (stat.SAllOwnedFields[VD].count(Aggregate)) {
          SmallVector<OwnershipDiagInfo> diags = stat.checkSFieldUse(
              VD, Loc, Aggregate, op == GetAddr,
              /*IsAggregateAccess=*/true);
          reporter.addDiags(diags);
        }
      }
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkSFieldUse(VD, Loc, Place, op == GetAddr);
      reporter.addDiags(diags);
    }
  }

  // situation 5 and 6
  if (stat.BOPStatus.count(VD)) {
    if (Place->getKind() == bsc::Place::Kind::Var) {
      SmallVector<OwnershipDiagInfo> diags =
          stat.checkBOPUse(VD, Loc, op == GetAddr, isAddrMut);
      isAddrMut = false;
      reporter.addDiags(diags);
    } else {
      SmallVector<OwnershipDiagInfo> diags = stat.checkBOPFieldUse(
          VD, Loc, Place, op == GetAddr);
      reporter.addDiags(diags);
    }
  }
}

void OwnershipImpl::MaybeSetNull(const CFGBlock *block, const CFGBlock *cur,
                                 Ownership::OwnershipStatus &status) {
  if (!block || !cur)
    return;
  const Expr *Cond =
      dyn_cast_or_null<Expr>(cur->getLastCondition());
  if (!Cond) {
    return;
  }
  NullCheckInfo Info(Cond->IgnoreParenImpCasts(), ctx);

  // NullCheckInfo deliberately skips array accesses (containsArrayAccess), so
  // null checks on owned-array elements (a[i] != nullptr / s[i].f != nullptr)
  // are handled here: on the branch where the element is null, null the
  // whole-array aggregate.
  const Expr *ArrElem = nullptr;
  bool ArrElemNonNull = false;
  if (const BinaryOperator *BO =
          dyn_cast<BinaryOperator>(Cond->IgnoreParenImpCasts())) {
    if (BO->getOpcode() == BO_NE || BO->getOpcode() == BO_EQ) {
      const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
      if (IsNullExpr(ctx, RHS) && !IsNullExpr(ctx, LHS))
        ArrElem = LHS;
      else if (IsNullExpr(ctx, LHS) && !IsNullExpr(ctx, RHS))
        ArrElem = RHS;
      ArrElemNonNull = (BO->getOpcode() == BO_NE);
    }
  } else if (const UnaryOperator *UO =
                 dyn_cast<UnaryOperator>(Cond->IgnoreParenImpCasts())) {
    if (UO->getOpcode() == UO_LNot) {
      // if (!arr[i]) -- implicit null check (arr[i] == nullptr)
      ArrElem = UO->getSubExpr()->IgnoreParenImpCasts();
      ArrElemNonNull = false;
    } else {
      // if (arr[i]) -- implicit null check (arr[i] != nullptr)
      ArrElem = Cond->IgnoreParenImpCasts();
      ArrElemNonNull = true;
    }
  } else {
    // if (arr[i]) -- implicit null check (arr[i] != nullptr)
    ArrElem = Cond->IgnoreParenImpCasts();
    ArrElemNonNull = true;
  }
  const VarDecl *ArrElemVD = ArrElem ? PeelArrayElemBase(ArrElem) : nullptr;
  bool ArrElemIsArray = ArrElemVD != nullptr;
  if (!ArrElemIsArray && ArrElem) {
    // Struct-field array element: w.arr[i] / o.w.arr[i]. The base is a
    // member access, which PeelArrayElemBase cannot unwrap; recognise it via
    // the host + "arr[]" path so the null-check branch nulls the field
    // aggregate (same as the local-array null-check).
    bsc::PlaceBuilder PB(ctx);
    const bsc::Place *FieldPath = nullptr;
    if (PeelHostAndFieldPlace(PB, ArrElem, FieldPath) &&
        IsOwnedArrayFieldPath(FieldPath)) {
      ArrElemIsArray = true;
      ArrElemVD = FieldPath->getRoot();
    }
  }
  // Narrowing the whole aggregate is only sound when the check applies to every
  // element, i.e. when it sits inside a qualifying for-loop that covers this
  // array (LoopCoveredArrays is filled for qualifying loops only). Outside such
  // a loop one subscript cannot prove the other elements are null, and marking
  // the aggregate null would turn their releases / clears into silent no-ops
  // (a double-free window).
  bool CoversWholeArray = false;
  if (ArrElemVD) {
    for (const Stmt *S = ArrElem; S;) {
      const Stmt *Parent = analysisContext.getParentMap().getParent(S);
      if (!Parent)
        break;
      if (const ForStmt *F = dyn_cast<ForStmt>(Parent)) {
        auto It = LoopInfo.LoopCoveredArrays.find(F);
        if (It != LoopInfo.LoopCoveredArrays.end() &&
            llvm::is_contained(It->second, ArrElemVD)) {
          CoversWholeArray = true;
          break;
        }
      }
      S = Parent;
    }
  }

  if (cur->succ_begin()[0] == block) {
    // block is True branch
    for (const Expr *E : Info.nullCheckedExprs)
      status.setToNull(E, ctx);
    if (ArrElemIsArray && !ArrElemNonNull && CoversWholeArray)
      status.setToNull(ArrElem, ctx);
  } else if (cur->succ_begin()[1] == block) {
    // block is False branch, inversely set present checked exprs to null
    for (const Expr *E : Info.presentCheckedExprs)
      status.setToNull(E, ctx);
    if (ArrElemIsArray && ArrElemNonNull && CoversWholeArray)
      status.setToNull(ArrElem, ctx);
  }
}

Ownership::OwnershipStatus
OwnershipImpl::runOnBlock(const CFGBlock *block,
                          Ownership::OwnershipStatus status,
                          OwnershipDiagReporter &reporter, bool isDestructor) {
  // Defer StmtExpr lifetime checks until after same-block consumers.
  llvm::DenseSet<const StmtExpr *> StmtExprsWithDeferredLifetimeEnds;
  TransferFunctions TF(*this, status, reporter,
                       StmtExprsWithDeferredLifetimeEnds);
  SmallVector<std::pair<const VarDecl *, SourceLocation>, 4>
      DeferredStmtExprLifetimeEnds;
  const Stmt *Terminator = block->getTerminatorStmt();
  const Expr *Condition = nullptr;
  if (IsStatementConditionTerminator(
          Terminator, analysisContext.getParentMap())) {
    Condition = block->getLastCondition();
    if (!Condition) {
      const Stmt *TerminatorCondition = block->getTerminatorCondition();
      Condition = dyn_cast_or_null<Expr>(TerminatorCondition);
    }
    if (Condition)
      Condition = Condition->IgnoreParenImpCasts();
  }

  for (CFGBlock::const_iterator it = block->begin(), ei = block->end();
       it != ei; ++it) {
    const CFGElement &elem = *it;

    if (elem.getAs<CFGStmt>()) {
      const Stmt *S = elem.castAs<CFGStmt>().getStmt();
      // Any non-whitelisted element in statement position (e.g. `*p;`,
      // `s.p;`, `(p == q);`) is a discarded expression statement: visited as
      // a read-only use of its bases. Hoisted sub-expressions are skipped —
      // they are re-visited inline by their enclosing statement, and a
      // standalone read-only visit would check the base before the current
      // dataflow iteration runs the consuming transfer (false positive once
      // the iteration re-enters the block with the post-transfer state).
      // Whitelisted statements keep priority: `f(p);` in statement position
      // is both discarded-position and a real-semantics CallExpr, and the
      // latter wins.

      // Nested calls inside an enclosing call's argument list are analyzed
      // inline by that enclosing call (see CollectArgumentCalls); skip their
      // hoisted CFGStmt elements.
      if (ArgCalls.count(S))
        continue;
      if (HasTransferSemantics(S)) {
        // The element running its transfer; nested transfers become
        // re-visits (see TransferStmt).
        TF.SetTransferStmt(S);
        TF.Visit(const_cast<Stmt *>(S));
        TF.SetTransferStmt(nullptr);
      } else if (IsDiscardedExprStmt(S, analysisContext.getParentMap())) {
        TF.VisitReadOnlyExpr(const_cast<Stmt *>(S));
      }
    }

    if (elem.getAs<CFGLifetimeEnds>()) {
      const Stmt *S = elem.castAs<CFGLifetimeEnds>().getTriggerStmt();
      const VarDecl *VD = elem.castAs<CFGLifetimeEnds>().getVarDecl();
      if (const auto *SE = dyn_cast_or_null<StmtExpr>(
              analysisContext.getParentMap().getParent(S))) {
        StmtExprsWithDeferredLifetimeEnds.insert(SE);
        DeferredStmtExprLifetimeEnds.emplace_back(VD, S->getEndLoc());
        continue;
      }
      TF.VisitLifetimeEnds(const_cast<VarDecl *>(VD), S->getEndLoc(),
                           isDestructor);
    }
  }

  if (Condition)
    TF.VisitReadOnlyExpr(const_cast<Expr *>(Condition));

  for (const auto &LifetimeEnd : DeferredStmtExprLifetimeEnds)
    TF.VisitLifetimeEnds(const_cast<VarDecl *>(LifetimeEnd.first),
                         LifetimeEnd.second, isDestructor);

  return status;
}


// True when Pred is the back-edge predecessor of a qualifying for-loop's
// header (condition) block, i.e. Pred's terminator lies inside the for loop
// (its increment or body) rather than before it.
static bool isLoopBackEdge(const CFGBlock *Pred, const ForStmt *FS,
                           const SourceManager &SM) {
  if (!Pred)
    return false;
  const Stmt *Term = Pred->getTerminatorStmt();
  SourceLocation TermLoc = Term ? Term->getBeginLoc() : SourceLocation();
  if (TermLoc.isInvalid()) {
    // Fall back to the block's first statement when there is no terminator.
    for (const CFGElement &E : *Pred) {
      if (E.getAs<CFGStmt>()) {
        TermLoc = E.castAs<CFGStmt>().getStmt()->getBeginLoc();
        break;
      }
    }
  }
  // The back-edge predecessors (increment / body) lie after the condition
  // expression, while the entry predecessor (the loop's init statement, which
  // sits inside the for's parentheses) lies before it. Using the `for` keyword
  // would misclassify the init block as a back edge.
  SourceLocation CondEnd =
      FS->getCond() ? FS->getCond()->getEndLoc() : SourceLocation();
  if (TermLoc.isInvalid() || CondEnd.isInvalid())
    return false;
  return SM.isBeforeInTranslationUnit(CondEnd, TermLoc);
}

void clang::runOwnershipAnalysis(const FunctionDecl &fd, const CFG &cfg,
                                 AnalysisDeclContext &ac,
                                 OwnershipDiagReporter &reporter,
                                 ASTContext &ctx) {
  // The analysis currently has scalability issues for very large CFGs.
  // Bail out if it looks too large.
  if (cfg.getNumBlockIDs() > 300000)
    return;

  // Classify qualifying for-loops for owned-element arrays and record the
  // per-loop allow-sites for element transfer.
  OwnedArrayLoopInfo LoopInfo;
  if (fd.getBody())
    classifyOwnedArrayLoops(ctx, &fd, LoopInfo);

  OwnershipImpl *OS = new OwnershipImpl(ac, ctx, std::move(LoopInfo));

  // Precompute the exit block of every qualifying loop: the back-edge change
  // handler below needs the exit of a loop whose back edge just changed and
  // must not scan the whole CFG for it on every change. Local to this
  // function (only used here).
  llvm::DenseMap<const ForStmt *, const CFGBlock *> LoopExitBlocks;
  for (const CFGBlock *B : cfg) {
    const Stmt *Term = B->getTerminatorStmt();
    const ForStmt *FS = dyn_cast_or_null<ForStmt>(Term);
    if (FS && OS->LoopInfo.QualifyingLoops.count(FS) && B->succ_size() == 2)
      LoopExitBlocks[FS] = B->succ_begin()[1];
  }

  if (const Stmt *Body = fd.getBody())
    CollectArgumentCalls(const_cast<Stmt *>(Body), false, OS->ArgCalls);

  const CFGBlock *entry = &cfg.getEntry();
  const CFGBlock *exit = &cfg.getExit();

  bool isDestructor = false;

  llvm::BitVector Enqueued(cfg.getNumBlockIDs());
  // A plain FIFO worklist (instead of ForwardDataflowWorklist): a qualifying
  // loop's exit block is re-enqueued explicitly whenever its back-edge state
  // changes (see below), which requires controlling the queue ourselves. The
  // initial order still follows the post-order view so ancestor blocks tend
  // to be processed first.
  std::queue<const CFGBlock *> WorkQueue;
  auto enqueueBlock = [&](const CFGBlock *B) {
    if (B && !Enqueued[B->getBlockID()]) {
      Enqueued[B->getBlockID()] = true;
      WorkQueue.push(B);
    }
  };
  auto enqueueSuccessors = [&](const CFGBlock *B) {
    for (const CFGBlock *Succ : B->succs())
      enqueueBlock(Succ);
  };

  // Locals exist from entry: a goto/switch that jumps over a declaration
  // must meet the variable as uninitialized at the join, not as absent.
  for (const CFGBlock *B : cfg) {
    for (const CFGElement &E : *B) {
      Optional<CFGStmt> CS = E.getAs<CFGStmt>();
      const auto *DS = CS ? dyn_cast<DeclStmt>(CS->getStmt()) : nullptr;
      if (!DS)
        continue;
      for (const Decl *D : DS->decls())
        if (const auto *VD = dyn_cast<VarDecl>(D))
          if (IsTrackedType(VD->getType()))
            OS->blocksEndStatus[entry].init(VD, ctx);
    }
  }

  // Mark all owned parameter Owned at the entry
  for (ParmVarDecl *PVD : fd.parameters()) {
    if (IsTrackedType(PVD->getType())) {
      if (const VarDecl *VD = dyn_cast<VarDecl>(PVD)) {
        OS->blocksEndStatus[entry].init(VD, ctx);
        OS->blocksEndStatus[entry].setToOwned(VD);
      }
    }
  }

  if (PostOrderCFGView *POV = ac.getAnalysis<PostOrderCFGView>()) {
    for (const CFGBlock *B : *POV) {
      if (B != entry && !B->pred_empty())
        enqueueBlock(B);
    }
  }

  if (const BSCMethodDecl *md = dyn_cast<BSCMethodDecl>(&fd)) {
    isDestructor = md->isDestructor();
  }

  while (!WorkQueue.empty()) {
    const CFGBlock *block = WorkQueue.front();
    WorkQueue.pop();
    Enqueued[block->getBlockID()] = false;
    Ownership::OwnershipStatus &preVal = OS->blocksBeginStatus[block];

    // meet operator
    Ownership::OwnershipStatus val;
    // for the header (condition) block of a qualifying
    // for-loop, drop the back-edge state of covered arrays before merging. A
    // qualifying loop applies a state-independent transition, so the header
    // keeps the pre-loop array state and the body's single-pass transition
    // yields the post-loop state (no fixed-point iteration over the back edge).
    const ForStmt *HeaderFS =
        dyn_cast_or_null<ForStmt>(block->getTerminatorStmt());
    bool IsQualifyingHeader =
        HeaderFS && OS->LoopInfo.QualifyingLoops.count(HeaderFS);

    // Exit block of a qualifying loop: its only predecessor is the loop's
    // condition block reached on the false branch. The loop header keeps the
    // pre-loop state for the covered arrays (the back-edge state is dropped,
    // see removeArrays below); the exit state is taken from the back-edge
    // block once the dataflow converges (takeArraysFrom further down).
    const ForStmt *LoopExit = nullptr;
    for (CFGBlock::const_pred_iterator it = block->pred_begin(),
                                       ei = block->pred_end();
         it != ei; ++it) {
      Ownership::OwnershipStatus Status = OS->blocksEndStatus[*it];
      OS->MaybeSetNull(block, *it, Status);
      if (IsQualifyingHeader &&
          isLoopBackEdge(*it, HeaderFS, ctx.getSourceManager())) {
        // Only drop the back-edge state of the arrays *this* loop covers;
        // other arrays must not be affected by this loop's header. This is
        // intentional: non-covered variables (scalars, other arrays, ...)
        // keep their normal back-edge merge so that modifications inside the
        // loop body still flow to the loop head. Only the covered arrays get
        // the state-independent (pre-loop) header state.
        auto CovIt = OS->LoopInfo.LoopCoveredArrays.find(HeaderFS);
        if (CovIt != OS->LoopInfo.LoopCoveredArrays.end())
          Status.removeArrays(CovIt->second);
      }
      val = OS->merge(val, Status);
      if (!LoopExit && block != &cfg.getEntry() && block != &cfg.getExit() &&
          block->pred_size() == 1 && it == block->pred_begin()) {
        const CFGBlock *Pred = *it;
        const Stmt *TermStmt = Pred ? Pred->getTerminatorStmt() : nullptr;
        if (const ForStmt *FS = dyn_cast_or_null<ForStmt>(TermStmt)) {
          if (Pred->succ_size() == 2 && Pred->succ_begin()[1] == block &&
              OS->LoopInfo.QualifyingLoops.count(FS))
            LoopExit = FS;
        }
      }
    }
    if (LoopExit) {
      const CFGBlock *HeaderBlock = *block->pred_begin();
      const CFGBlock *BackBlock = nullptr;
      for (CFGBlock::const_pred_iterator it = HeaderBlock->pred_begin(),
                                         ei = HeaderBlock->pred_end();
           it != ei; ++it) {
        if (*it && isLoopBackEdge(*it, LoopExit, ctx.getSourceManager())) {
          BackBlock = *it;
          break;
        }
      }
      // The back-edge block is only deferred while it is untouched (its
      // status is still empty). A processed back-edge block always carries
      // the covered arrays of this qualifying loop (the loop covers at least
      // one array, which is tracked in the increment/body state), so this is
      // never a permanent stall: the back-edge is enqueued through normal
      // successor propagation and, once it changes, the exit block is
      // re-enqueued below.
      if (BackBlock && OS->blocksEndStatus[BackBlock].empty()) {
        enqueueBlock(block);
        continue;
      }
      if (BackBlock) {
        auto CovIt = OS->LoopInfo.LoopCoveredArrays.find(LoopExit);
        if (CovIt != OS->LoopInfo.LoopCoveredArrays.end())
          val.takeArraysFrom(OS->blocksEndStatus[BackBlock], CovIt->second);
      }
    }

    OS->blocksEndStatus[block] =
        OS->runOnBlock(block, val, reporter, isDestructor);

    if (preVal.equals(val))
      continue;

    preVal = val;

    // A qualifying loop's exit block takes the covered-array state from the
    // back-edge block. The header drops the back-edge state, so the header
    // never changes and the exit block is not re-triggered through normal
    // successor propagation once the back edge later converges. Re-enqueue
    // the exit block whenever its back edge changes.
    for (const auto &KV : OS->LoopInfo.LoopCoveredArrays) {
      const ForStmt *FS = KV.first;
      if (!isLoopBackEdge(block, FS, ctx.getSourceManager()))
        continue;
      auto ExitIt = LoopExitBlocks.find(FS);
      if (ExitIt != LoopExitBlocks.end())
        enqueueBlock(ExitIt->second);
    }

    // Enqueue the value to the successors.
    enqueueSuccessors(block);
  }

  // Loop-end element-state consistency for qualifying loops. The loop exit's
  // begin state is the post-loop state taken from the back edge, i.e. the merge
  // of one body iteration's paths: an owned-element array left Owned|Moved is
  // reported. Reading the converged state here (instead of during the worklist)
  // avoids reporting on an intermediate back-edge state. The dataflow already
  // computes the per-path union for us (bits for owned-pointer element arrays,
  // moved/owned/null field sets for struct hosts), so no separate AST-side path
  // analysis is needed: any control flow (`if`, `?:`, `switch`, ...) is covered.
  for (const auto &KV : LoopExitBlocks) {
    const ForStmt *FS = KV.first;
    const CFGBlock *Exit = KV.second;
    auto CoveredIt = OS->LoopInfo.LoopCoveredArrays.find(FS);
    if (CoveredIt == OS->LoopInfo.LoopCoveredArrays.end())
      continue;
    const Ownership::OwnershipStatus &ExitState = OS->blocksBeginStatus[Exit];
    for (const VarDecl *VD : CoveredIt->second) {
      if (!ExitState.arrayElemPathInconsistent(VD))
        continue;
      OwnershipDiagInfo DI(FS->getEndLoc(),
                           OwnershipDiagKind::ArrayElemPathInconsistent,
                           VD->getNameAsString());
      reporter.addDiagInfo(DI);
    }
  }

  // check ownership rules of function parameters
  for (ParmVarDecl *PVD : fd.parameters()) {
    if (const VarDecl *VD = dyn_cast<VarDecl>(PVD)) {
      SmallVector<OwnershipDiagInfo> diags =
          OS->blocksEndStatus[exit].checkMemoryLeak(
              VD, fd.getSourceRange().getEnd(), isDestructor);
      reporter.addDiags(diags);
    }
  }

  delete OS;
}

#endif // ENABLE_BSC
