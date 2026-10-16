//===- BSCOwnerShip.h - OwnerShip Analysis for Source CFGs -*- BSC --*--------//
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

#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_BSCOWNERSHIP_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_BSCOWNERSHIP_H

#if ENABLE_BSC

#include "clang/AST/Decl.h"
#include "clang/AST/Stmt.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Analysis/Analyses/BSC/BSCPlace.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include <cassert>
#include <string>

namespace clang {

// an array type is an "owned element array" if its (possibly
// multi-dimensional) element type is an _Owned pointer, an _Owned struct, or a
// move-semantic record (a struct that contains _Owned members). These arrays
// are tracked as whole-array aggregates by ownership analysis, and element
// ownership transfer is only allowed inside qualifying for-loops.
// The element is inspected one array level at a time: an owned pointer element
// or an element record that itself contains owned fields answers true, an
// array of them does not until the walk reaches the element level.
static inline bool IsOwnedElementArrayType(QualType type) {
  while (const auto *AT = type->getAsArrayTypeUnsafe()) {
    QualType Elem = AT->getElementType();
    if (Elem.isOwnedPointer())
      return true;
    if (Elem->isRecordType() &&
        Elem.isOrContainsOwned(BSCLookThrough::NoPointer))
      return true;
    type = Elem;
  }
  return false;
}

// T *_Owned _ArrayElem where transferring an element would move ownership of
// an owned pointer / move-semantic value / owned fields of the pointee. Such
// pointers never carry length information, so element transfer is forbidden.
static inline bool IsOwnedArrayElemPtrTransferBase(QualType type) {
  if (!type.isOwnedPointer() || !type.isArrayElemQualified())
    return false;
  return type->getPointeeType().isOrContainsOwned(BSCLookThrough::AnyPointer);
}

// IsTrackedType judges if the status of a variable needs to be tracked.
// We track:
// 1. the basic owned pointer type such as `int * owned p`
// 2. the struct owned pointer type such as `struct S * owned s`
// 3. the struct type with at least one owned field
// 4. the owned struct type such as `owned struct S`
// 5. arrays whose element type is tracked (owned pointer / move-semantic)
static inline bool IsTrackedType(QualType type) {
  // case 1 and case 2
  if (type.isOwnedPointer())
    return true;

  // case 3 and case 4
  if (type->isRecordType() &&
      type.isOrContainsOwned(BSCLookThrough::NoPointer))
    return true;

  // case 5: array of tracked elements
  if (const auto *AT = type->getAsArrayTypeUnsafe())
    return IsTrackedType(AT->getElementType());

  return false;
}

// Whether E is a null pointer constant (after peeling casts), e.g. nullptr
// / (void*)0. Shared by the CFG dataflow and the loop classifier's
// null-check recognition.
static inline bool IsNullExpr(ASTContext &Context, const Expr *E) {
  return E->IgnoreParenImpCasts()->isNullPointerConstant(
             Context, Expr::NPC_ValueDependentIsNotNull) !=
         Expr::NPCK_NotNull;
}

//===----------------------------------------------------------------------===//
// Host-relative field paths, carried as a bsc::Place chain (one Field
// projection per member, one Index ("[]") projection per field-array level).
// The host variable is Path->getRoot(). Shared by the CFG dataflow
// (BSCOwnership.cpp) and the qualifying-loop classifier
// (BSCOwnershipArrayLoopClassification.cpp).
//===----------------------------------------------------------------------===//

// Peel `w.arr[0].a` / `s[i].a` / `s[i].a[j]` to the host-relative projection
// chain. Host-array indexes (the `i` of `s[i]`) are not part of the chain.
// Returns false when the base is not a variable, when a member is not a plain
// field, or when an array level does not sit on an array-typed field — the
// shapes the historical string-to-Place conversion rejected. Path is set to
// null on failure.
static inline bool PeelHostAndFieldPlace(bsc::PlaceBuilder &PB, const Expr *E,
                                         const bsc::Place *&Path) {
  Path = nullptr;
  // Steps from E down to the host, applied in reverse to build host -> leaf.
  llvm::SmallVector<const Expr *, 4> Steps;
  const Expr *Cur = E ? E->IgnoreParenImpCasts() : nullptr;
  while (Cur) {
    if (const MemberExpr *ME = dyn_cast<MemberExpr>(Cur)) {
      Steps.push_back(ME);
      Cur = ME->getBase()->IgnoreParenImpCasts();
    } else if (const ArraySubscriptExpr *ASE =
                   dyn_cast<ArraySubscriptExpr>(Cur)) {
      const Expr *B = ASE->getBase()->IgnoreParenImpCasts();
      // MemberExpr base -> array-level field index. A nested ArraySubscriptExpr
      // base may be a field-array index (w.arr[i][j]) or a host pointer index
      // (p2[1][2] where p2 is `int **_Borrow`); only the former belongs in
      // the chain. Decide by looking at the ultimate base of the nested chain.
      bool IsFieldIndex = dyn_cast<MemberExpr>(B) != nullptr;
      if (const ArraySubscriptExpr *Nested =
              dyn_cast<ArraySubscriptExpr>(B)) {
        const Expr *BB = Nested->getBase()->IgnoreParenImpCasts();
        while (const ArraySubscriptExpr *BA =
                   dyn_cast<ArraySubscriptExpr>(BB))
          BB = BA->getBase()->IgnoreParenImpCasts();
        IsFieldIndex = dyn_cast<MemberExpr>(BB) != nullptr;
      }
      if (IsFieldIndex)
        Steps.push_back(ASE);
      Cur = B;
    } else {
      break;
    }
  }
  const auto *DRE = dyn_cast<DeclRefExpr>(Cur);
  const VarDecl *HostVD =
      DRE ? dyn_cast<VarDecl>(DRE->getDecl()) : nullptr;
  if (!HostVD)
    return false;
  Path = PB.Build(HostVD, HostVD->getLocation());
  for (auto It = Steps.rbegin(), EIt = Steps.rend(); It != EIt; ++It) {
    if (const auto *ME = dyn_cast<MemberExpr>(*It)) {
      const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());
      if (!FD)
        return false;
      Path = PB.BuildField(Path, FD, FD->getLocation());
      continue;
    }
    const auto *ASE = cast<ArraySubscriptExpr>(*It);
    QualType BaseTy = ASE->getBase()->IgnoreParenImpCasts()->getType();
    const auto *AT = BaseTy->getAsArrayTypeUnsafe();
    if (!AT)
      return false; // an array level on a non-array field
    Path = PB.BuildIndex(Path, AT->getElementType(), "[]", ASE->getExprLoc());
  }
  return true;
}

// Whether Path contains an array level (an Index projection).
static inline bool HasIndexProjection(const bsc::Place *Path) {
  for (const bsc::Place *Cur = Path; Cur; Cur = Cur->getBase())
    if (Cur->getKind() == bsc::Place::Kind::Index)
      return true;
  return false;
}

// Is Path an owned-element array field path (its element type is an _Owned
// pointer or move-semantic record)? Replays the historical name resolution
// over the place chain: every Field projection is looked up by name in its
// parent's record, so the *sugared* field type drives the check —
// canonicalizing an array type drops the _Owned qualifier of its element
// (int *_Owned arr[2]), so Place::getType() cannot be used here.
static inline bool IsOwnedArrayField(const bsc::Place *Path) {
  // The record holding the member a Field projection names: the parent
  // place's type with host array levels and a leading pointer stripped (the
  // historical host handling), and array levels stripped for field parents.
  auto ParentRecord = [](const bsc::Place *F) -> const RecordDecl * {
    const bsc::Place *Base = F->getBase();
    const bsc::Ty *T = Base ? Base->getType() : nullptr;
    if (!T)
      return nullptr;
    QualType QT = T->getQualType();
    while (const auto *AT = QT->getAsArrayTypeUnsafe())
      QT = AT->getElementType();
    if (Base->getKind() == bsc::Place::Kind::Var && QT->isPointerType())
      QT = QT->getPointeeType();
    const RecordType *RT = QT.getCanonicalType()->getAs<RecordType>();
    return RT ? RT->getDecl() : nullptr;
  };

  // Field projections, host -> leaf.
  llvm::SmallVector<const bsc::Place *, 4> Fields;
  for (const bsc::Place *Cur = Path; Cur; Cur = Cur->getBase())
    if (Cur->getKind() == bsc::Place::Kind::Field)
      Fields.push_back(Cur);
  for (auto It = Fields.rbegin(); It != Fields.rend(); ++It) {
    const bsc::Place *F = *It;
    const RecordDecl *RD = ParentRecord(F);
    if (!RD)
      return false;
    const FieldDecl *FD = nullptr;
    for (const FieldDecl *Cand : RD->fields())
      if (Cand->getNameAsString() == F->getName()) {
        FD = Cand;
        break;
      }
    // A name that does not resolve (e.g. a promoted field of an anonymous
    // struct/union, which RecordDecl::fields() does not list in C) ends the
    // scan, like the historical string lookup.
    if (!FD)
      return false;
    // The last member segment (Fields holds leaf -> host, so the leaf-most
    // projection is front()): the field itself must be an owned-element
    // array. Trailing `[]` levels produce no further Field projection.
    if (F == Fields.front())
      return IsOwnedElementArrayType(FD->getType());
    if (IsOwnedElementArrayType(FD->getType()))
      return true;
    // An intermediate member that cannot be descended into (not a record
    // after stripping its array levels) ends the scan too.
    QualType FT = FD->getType();
    while (const auto *AT = FT->getAsArrayTypeUnsafe())
      FT = AT->getElementType();
    if (!FT.getCanonicalType()->getAs<RecordType>())
      return false;
  }
  return false;
}

// Whether Path (from PeelHostAndFieldPlace) denotes an owned-element array
// field: it must contain an array level (`[]`) and resolve to an
// owned-element array. Single predicate shared by the dataflow transfer sites
// (subscript / member / cast / null-check).
static inline bool IsOwnedArrayFieldPath(const bsc::Place *Path) {
  return HasIndexProjection(Path) && IsOwnedArrayField(Path);
}

// An enum of reasons why a for-loop does not qualify for owned-array element
// transfer. Filled by the AST pre-pass (BSCOwnershipArrayLoopClassification.cpp)
// and used to emit a note when an element transfer is rejected inside a
// non-qualifying loop.
enum class NonQualifyingLoopReason : unsigned {
  NotAForLoop,        // the statement is not a for-loop at all
  NotQualifying,      // generic: a qualifying-loop rule failed
  InitNotZero,         // init is not zero (`T i = 0`)
  InitTypeTooSmall,    // loop variable type cannot hold the loop bound
  CondNotMatch,        // cond is not `i < N`
  IncrNotByOne,        // increment is not step one
  LoopVarModified,     // loop variable modified / address-taken in the body
  EarlyExit,           // return / goto in the loop body
  BoundMismatch,       // loop bound != array length
  BoundNotConstant,    // fixed array: loop bound is not a constant expression
  ElemAccessMismatch,  // array access does not use the loop index (a[i])
  IndexReused,         // one loop index drives several subscript levels
  VlaBoundVarMismatch, // VLA length variable is not the loop bound variable
  VlaBoundVarModified, // VLA length variable address-taken / borrowed / modified
};

// Why a loop / array failed the qualifying-loop rules, with the exact
// source location to point the diagnostic note at (a return/break statement,
// a loop-variable modification site, ...). Loc is invalid when the failure
// is at the loop itself (e.g. the condition shape).
struct LoopFailure {
  NonQualifyingLoopReason Reason;
  SourceLocation Loc;
};


enum OwnershipDiagKind {
  InvalidUseOfMoved,
  InvalidUseOfPartiallyMoved,
  InvalidUseOfAllMoved,
  InvalidUseOfPossiblyUninit,
  InvalidUseOfUninit,
  InvalidAssignOfOwned,
  InvalidAssignOfPartiallyMoved,
  InvalidAssignOfPossiblyPartiallyMoved,
  InvalidAssignOfAllMoved,
  InvalidAssignFieldOfUninit,
  InvalidAssignFieldOfOwned,
  InvalidAssignFieldOfMoved,
  InvalidAssignSubFieldOwned,
  InvalidCastMoved,
  InvalidCastUninit,
  InvalidCastFieldOwned,
  FieldMemoryLeak,
  MemoryLeak,
  ArrayElemTransferForbidden,
  ArrayElemPathInconsistent,
  ArrayElemAssignOwned,
  OwnedStructPartiallyMoved,
  OwnedStructNotProperlyFreed,
  PassCastToArgOrRet,
  OwnershipMaxDiagKind
};

class OwnershipDiagInfo {
  public:
    SourceLocation Loc;
    OwnershipDiagKind Kind;
    std::string Name;
    std::string Fields;
    SourceLocation Location;
    QualType Type = QualType();
    // Optional follow-up note: location of the non-qualifying for-loop and the
    // reason why it does not qualify (attached to ArrayElemTransferForbidden).
    SourceLocation NoteLoc;
    std::string Note;
  
    OwnershipDiagInfo(SourceLocation Loc, OwnershipDiagKind Kind,
                      std::string Name)
        : Loc(Loc), Kind(Kind), Name(Name), Fields(""),
          Location(SourceLocation()), NoteLoc(SourceLocation()), Note("") {}

    OwnershipDiagInfo(SourceLocation Loc, OwnershipDiagKind Kind, QualType QT)
        : Loc(Loc), Kind(Kind), Name(""), Fields(""),
          Location(SourceLocation()), Type(QT), NoteLoc(SourceLocation()),
          Note("") {}

    OwnershipDiagInfo(SourceLocation Loc, OwnershipDiagKind Kind,
                      std::string Name, std::string fields)
        : Loc(Loc), Kind(Kind), Name(Name), Fields(fields),
          Location(SourceLocation()), NoteLoc(SourceLocation()), Note("") {}
  
    OwnershipDiagInfo(SourceLocation Loc, OwnershipDiagKind Kind,
                      std::string Name, std::string fields,
                      SourceLocation location)
        : Loc(Loc), Kind(Kind), Name(Name), Fields(fields), Location(location),
          NoteLoc(SourceLocation()), Note("") {}
  
    bool operator==(const OwnershipDiagInfo &other) const {
      return Loc == other.Loc && Kind == other.Kind && Name == other.Name &&
             Fields == other.Fields && Location == other.Location &&
             Type == other.Type;
    }
  };

class Ownership : public ManagedAnalysis {
public:
  // The ownership state of a tracked variable or field is a *set* of possible
  // states: the analysis is a may-analysis, so a CFG merge unions the states
  // of the incoming paths ({Owned, Moved} means "owned on one path, moved on
  // another"). A single state is a one-element set: is(VD, S) asks for exactly
  // {S}, has(VD, S) for {S} plus at least one more. None is the empty set.
  enum Status : unsigned char {
    None = 0x0,
    Uninitialized = 0x1,
    Null = 0x2,
    Owned = 0x4,
    Moved = 0x8,
    PartialMoved = 0x10,
    AllMoved = 0x20,
  };

  friend constexpr Status operator|(Status A, Status B) {
    return static_cast<Status>(static_cast<unsigned char>(A) |
                               static_cast<unsigned char>(B));
  }
  friend constexpr Status &operator|=(Status &A, Status B) { return A = A | B; }
  friend constexpr Status operator&(Status A, Status B) {
    return static_cast<Status>(static_cast<unsigned char>(A) &
                               static_cast<unsigned char>(B));
  }
  friend constexpr Status &operator&=(Status &A, Status B) { return A = A & B; }
  // Complement within the six defined states.
  friend constexpr Status operator~(Status A) {
    return static_cast<Status>(~static_cast<unsigned char>(A) & 0x3f);
  }

  // Which kind of tracked variable a VarState describes. The three families
  // differ only in the rules they follow — the owned-struct special cases,
  // the OPS AllMoved exemption, the BOP diagnostic spelling — not in how
  // their state is stored.
  enum class HostKind : unsigned char {
    Unset, // no family: an entry created by an unguarded per-variable access
    OPS,   // Owned Pointer to Struct, e.g. struct S* owned s
    S,     // Struct, e.g. struct S s
    BOP    // Owned Pointer to Basic Types, e.g. int* owned p
  };

  class OwnershipStatus {
  public:
    // The state set of one variable: a bitmask of Ownership::Status values.
    using OwnershipSet = Status;

    // Field-path state is keyed by bsc::Place: a root variable followed by
    // field / deref / index projections, mirroring the historical string
    // paths ("f.g", "arr[]", "f*", "pp**" — see BSCOwnership.cpp for the
    // key-shape mapping). Membership is structural (DenseMapInfo over
    // places), while iteration reproduces the historical SmallSet<std::string,
    // 10> order contract: up to 10 elements iterate in insertion order (an
    // erase keeps the relative order of the rest, a re-insert appends at the
    // end), and once an 11th distinct element is inserted the set stays in
    // "grown" mode and iterates lexicographically by the historical key
    // spelling (the old std::set overflow). Field lists in diagnostics
    // depend on this order. One deliberate corner-case divergence: after
    // growing, SmallSet returns to insertion order once fully emptied,
    // while Grown here is sticky — unreachable in practice (per-function
    // field counts rarely reach 11) and only diagnostic order would differ.
    class FieldSet {
      llvm::SmallVector<const bsc::Place *, 10> Order;
      llvm::DenseSet<const bsc::Place *> Set;
      bool Grown = false;
      mutable llvm::SmallVector<const bsc::Place *, 16> SortedCache;
      mutable bool CacheValid = false;

      void ensureSorted() const;

    public:
      using const_iterator =
          typename llvm::SmallVector<const bsc::Place *, 16>::const_iterator;

      bool insert(const bsc::Place *P);
      bool erase(const bsc::Place *P);
      size_t count(const bsc::Place *P) const { return Set.count(P); }
      void clear() {
        Order.clear();
        Set.clear();
        CacheValid = false;
      }
      size_t size() const { return Set.size(); }
      bool empty() const { return Set.empty(); }
      const_iterator begin() const;
      const_iterator end() const;
      bool operator==(const FieldSet &RHS) const;
      bool operator!=(const FieldSet &RHS) const { return !(*this == RHS); }
    };

    // The flow state of the tracked field paths of one variable: the may-set
    // of possible states of each path. A cell whose set empties out is erased,
    // so an entry always holds a non-empty set.
    using CellMap = llvm::DenseMap<const bsc::Place *, OwnershipSet>;

    // Everything tracked about one variable. The static schema of its owned
    // field paths (All) is written once by init and never mutated by an
    // ownership transition; Root and Cells are the flow state.
    struct VarState {
      HostKind Kind = HostKind::Unset;
      OwnershipSet Root = None;
      FieldSet All;
      CellMap Cells;

      bool operator==(const VarState &RHS) const {
        return Kind == RHS.Kind && Root == RHS.Root && All == RHS.All &&
               Cells == RHS.Cells;
      }
      bool operator!=(const VarState &RHS) const { return !(*this == RHS); }
    };
    llvm::DenseMap<const VarDecl *, VarState> Vars;

    // The fields of one variable that are currently in state State: a view
    // over the variable's cell map, so all four field states share one entry
    // per path. Every operation touches only the State bit of a cell. size(),
    // empty() and iteration walk the static schema (All), preserving the
    // iteration-order contract the diagnostics depend on.
    class FieldStateView {
      CellMap &Cells;
      const FieldSet &All;
      Status State;
      mutable llvm::SmallVector<const bsc::Place *, 16> Snapshot;
      mutable bool SnapshotValid = false;

      // A view owns its snapshot cache, so a copy is a second cache over the
      // same cells that nothing invalidates: mutating through one view would
      // leave the other reporting stale fields. Copies are therefore kept
      // private to OwnershipStatus -- which is all the by-value accessor chain
      // (ownedFields() -> fieldsInState()) needs -- so every other caller has
      // to use the view as a temporary expression.
      FieldStateView(const FieldStateView &) = default;
      FieldStateView &operator=(const FieldStateView &) = delete;
      friend class OwnershipStatus;

      // The materialized set of fields in State, built once and reused by
      // begin()/end(). Mirrors FieldSet's SortedCache/CacheValid pair: every
      // mutator below invalidates it, and the rebuild clears the buffer first
      // (push_back appends, so rebuilding into a dirty buffer would keep the
      // stale entries and duplicate the fresh ones). The supported pattern is
      // therefore iterate -> mutate -> iterate; as with FieldSet, a view must
      // not be mutated while an iteration over it is still in flight.
      void buildSnapshot() const {
        if (SnapshotValid)
          return;
        Snapshot.clear();
        for (const bsc::Place *P : All)
          if (count(P))
            Snapshot.push_back(P);
        SnapshotValid = true;
      }

    public:
      FieldStateView(CellMap &Cells, const FieldSet &All, Status State)
          : Cells(Cells), All(All), State(State) {}

      bool count(const bsc::Place *P) const {
        return (Cells.lookup(P) & State) != None;
      }
      bool insert(const bsc::Place *P) {
        OwnershipSet &C = Cells[P];
        if ((C & State) != None)
          return false;
        C |= State;
        SnapshotValid = false;
        return true;
      }
      bool erase(const bsc::Place *P) {
        auto It = Cells.find(P);
        if (It == Cells.end() || (It->second & State) == None)
          return false;
        It->second &= ~State;
        if (It->second == None)
          Cells.erase(It);
        SnapshotValid = false;
        return true;
      }
      void clear() {
        for (const bsc::Place *P : All)
          erase(P);
      }
      // Replace the set with exactly RHS: the tracked paths in RHS gain State,
      // every other tracked path loses it.
      FieldStateView &operator=(const FieldSet &RHS) {
        for (const bsc::Place *P : All) {
          if (RHS.count(P))
            insert(P);
          else
            erase(P);
        }
        return *this;
      }
      size_t size() const {
        size_t N = 0;
        for (const bsc::Place *P : All)
          if (count(P))
            ++N;
        return N;
      }
      bool empty() const { return size() == 0; }

      // The set of fields this view denotes, in the static schema's order.
      // The set helpers (fieldDescendants, findMovedFieldKey, concatFields, …)
      // take a FieldSet, so a view converts to the set it denotes. The explicit
      // member operations above still win for a view, so `v = s` writes through
      // the view rather than assigning to a temporary.
      operator FieldSet() const {
        FieldSet Fields;
        for (const bsc::Place *P : All)
          if (count(P))
            Fields.insert(P);
        return Fields;
      }

      using const_iterator = const bsc::Place *const *;
      const_iterator begin() const {
        buildSnapshot();
        return Snapshot.begin();
      }
      const_iterator end() const {
        buildSnapshot();
        return Snapshot.end();
      }
    };

    // A variable is tracked iff init() registered it; its family never changes
    // afterwards. Every per-variable access below must be guarded by
    // isTracked(), since the non-const ones create the entry — such an entry
    // stays HostKind::Unset and reads as untracked, so an unguarded access
    // cannot flip the family of a variable.
    bool isTracked(const VarDecl *VD) const {
      const VarState *VS = lookupVar(VD);
      return VS && VS->Kind != HostKind::Unset;
    }
    // The state of a tracked variable, or nullptr when it is not tracked.
    const VarState *lookupVar(const VarDecl *VD) const;
    // Whether VD is tracked and belongs to family K. Replaces the historical
    // per-family XStatus.count(VD) test.
    bool isHost(const VarDecl *VD, HostKind K) const {
      const VarState *VS = lookupVar(VD);
      return VS && VS->Kind == K;
    }
    // The static schema of a tracked variable.
    FieldSet &allFields(const VarDecl *VD) {
      assert(Vars[VD].Kind != HostKind::Unset && "untracked variable");
      return Vars[VD].All;
    }
    // The fields of a tracked variable currently in state S.
    FieldStateView fieldsInState(const VarDecl *VD, Status S) {
      assert(Vars[VD].Kind != HostKind::Unset && "untracked variable");
      VarState &VS = Vars[VD];
      return FieldStateView(VS.Cells, VS.All, S);
    }
    FieldStateView ownedFields(const VarDecl *VD) {
      return fieldsInState(VD, Owned);
    }
    FieldStateView nullFields(const VarDecl *VD) {
      return fieldsInState(VD, Null);
    }
    FieldStateView uninitFields(const VarDecl *VD) {
      return fieldsInState(VD, Uninitialized);
    }
    FieldStateView movedFields(const VarDecl *VD) {
      return fieldsInState(VD, Moved);
    }

    bool equals(const OwnershipStatus &V) const;
    bool empty() const;
    bool is(const VarDecl *VD, Status S) const;
    bool has(const VarDecl *VD, Status S) const;
    // The S component is present, exactly or together with other bits (e.g.
    // Moved|Null): `is(S) || has(S)`, the "possibly S" idiom of the move /
    // use-after-move checks.
    bool isOrHas(const VarDecl *VD, Status S) const;
    // Shared by is / has / isOrHas: the status bits of VD, or nullptr when it
    // is not a tracked aggregate.
    const OwnershipSet *bitsOf(const VarDecl *VD) const;
    bool canAssign(const VarDecl *VD) const;
    void set(const VarDecl *VD, Status S);
    void resetAll(const VarDecl *VD);
    // Methods that build field-path places from the AST take the ASTContext
    // (arena + PlaceBuilder) as a parameter: OwnershipStatus itself holds no
    // context.
    void init(const VarDecl *VD, ASTContext &Ctx);
    void setToOwned(const VarDecl *VD);
    void setToAllMoved(const VarDecl *VD);
    void setToAllMoved(const Expr *E, ASTContext &Ctx);
    void setToNull(const VarDecl *VD);
    void setToNull(const Expr *E, ASTContext &Ctx);
    void setToMoved(const VarDecl *VD);
    void setToMoved(const Expr *E, ASTContext &Ctx);

    // whole-array / struct-field ownership transition applied
    // by the dataflow when a qualifying for-loop transfers array elements.
    // The whole array is one aggregate; an element transfer applies to all
    // elements uniformly, so the aggregate state moves as a unit. `Field` is
    // nullptr for the whole aggregate, otherwise the host-relative array-field
    // path ("arr[]", "arr[].p"); `S` is the target state (Owned / Null /
    // Moved).
    void setArrayAggregate(const VarDecl *VD, const bsc::Place *Field, Status S);
    // Shared by setArrayAggregate's field-path cases: refresh the aggregate bit
    // state from the owned-field count vs the static field count.
    void refreshArrayFieldState(const VarDecl *VD, size_t OwnedCount,
                                size_t AllCount);
    // Whether the aggregate is (possibly) uninitialized, or owns nothing (the
    // whole aggregate / the given field is null). Shared by the move-out checks
    // (BOP uses status bits; S/OPS use the per-field sets).
    bool arrayAggregateUninit(const VarDecl *VD) const;
    bool arrayAggregateNull(const VarDecl *VD,
                            const bsc::Place *fieldName) const;
    // Whether a null clear (`= nullptr`) of the aggregate / field drops nothing:
    // no path of it can still hold an owning value (Null, Moved, Moved|Null,
    // uninitialized, ... all count, but any Owned component does not). Such a
    // clear is a no-op (the aggregate simply also becomes null -- e.g. Moved
    // becomes Moved|Null); a clear of an aggregate that may still own something
    // is an ownership transfer and stays gated by the qualifying-loop rules.
    // Unlike arrayAggregateNull this deliberately rejects Owned|Null: owning on
    // one path is not cleared by a single null assignment.
    bool arrayAggregateNotOwned(const VarDecl *VD,
                                const bsc::Place *fieldName) const;
    // Whether the aggregate is in the forbidden Owned|Moved state at a
    // qualifying for-loop's end: the element possibly owns a value on some path
    // and was moved out on another. A null path is compatible with both (a null
    // element owns nothing), so Owned|Null and Moved|Null are stable states.
    bool arrayElemPathInconsistent(const VarDecl *VD) const;
    // Whether the aggregate (or a tracked field) has been moved out on some
    // path: the Moved component is present, possibly alongside Owned / Null
    // (exactly Moved also counts). Transferring ownership out again may double
    // free, so this is the predicate the move-out checks use.
    bool arrayElemPossiblyMoved(const VarDecl *VD) const;
    // Whether a move-out / clear of the aggregate (or field) is a pure-null
    // no-op: the value is null on every path and was never moved, so
    // free(null) / `= nullptr` changes nothing. A field that is only possibly
    // null but was already moved on another path (Moved|Null) is NOT a null
    // no-op: transferring it again may double free.
    bool arrayElemNullNoop(const VarDecl *VD, const bsc::Place *field) const;

    // Remove the aggregate state of a set of owned-element arrays. Used by the
    // loop-header dataflow to drop the back-edge state of arrays covered by a
    // qualifying loop before merging (the header keeps the pre-loop state).
    void removeArrays(llvm::ArrayRef<const VarDecl *> Arrays);
    void removeArraysOne(const VarDecl *VD);

    // Replace the aggregate state of a set of owned-element arrays with the
    // corresponding state from Other. Used by the loop-exit dataflow: the exit
    // block takes the post-loop array state from the back-edge block.
    void takeArraysFrom(const OwnershipStatus &Other,
                        llvm::ArrayRef<const VarDecl *> Arrays);

    llvm::SmallVector<OwnershipDiagInfo>
    checkOPSUse(const VarDecl *VD, const SourceLocation &Loc, bool isGetAddr,
                bool isStar, bool isAddrMut);
    llvm::SmallVector<OwnershipDiagInfo>
    checkOPSFieldUse(const VarDecl *VD, const SourceLocation &Loc,
                     const bsc::Place *fullFieldName, bool isGetAddr,
                     bool IsAggregateAccess = false);
    // Root assign check shared by the OPS and BOP families.
    llvm::SmallVector<OwnershipDiagInfo>
    checkPointerAssign(const VarDecl *VD, const SourceLocation &Loc);
    llvm::SmallVector<OwnershipDiagInfo>
    checkOPSDerefAssign(const VarDecl *VD, const SourceLocation &Loc);
    llvm::SmallVector<OwnershipDiagInfo>
    checkOPSFieldAssign(const VarDecl *VD, const SourceLocation &Loc,
                        const bsc::Place *fullFieldName,
                        bool IsAggregateAccess = false);

    llvm::SmallVector<OwnershipDiagInfo>
    checkSUse(const VarDecl *VD, const SourceLocation &Loc, bool isGetAddr, bool isAddrMut);
    llvm::SmallVector<OwnershipDiagInfo>
    checkSFieldUse(const VarDecl *VD, const SourceLocation &Loc,
                   const bsc::Place *fullFieldName, bool isGetAddr,
                   bool IsAggregateAccess = false);
    llvm::SmallVector<OwnershipDiagInfo>
    checkSAssign(const VarDecl *VD, const SourceLocation &Loc);
    llvm::SmallVector<OwnershipDiagInfo>
    checkSFieldAssign(const VarDecl *VD, const SourceLocation &Loc,
                      const bsc::Place *fullFieldName,
                      bool IsAggregateAccess = false);

    llvm::SmallVector<OwnershipDiagInfo>
    checkBOPUse(const VarDecl *VD, const SourceLocation &Loc, bool isGetAddr, bool isAddrMut);
    llvm::SmallVector<OwnershipDiagInfo>
    checkBOPFieldUse(const VarDecl *VD, const SourceLocation &Loc,
                     const bsc::Place *fullFieldName, bool isGetAddr);
    llvm::SmallVector<OwnershipDiagInfo>
    checkBOPFieldAssign(const VarDecl *VD, const SourceLocation &Loc,
                        const bsc::Place *fullFieldName);

    // Cast to `void * _Owned` of a whole owned pointer: shared by the OPS and
    // BOP families.
    llvm::SmallVector<OwnershipDiagInfo>
    checkCastPointer(const VarDecl *VD, const SourceLocation &Loc);
    llvm::SmallVector<OwnershipDiagInfo>
    checkCastField(const VarDecl *VD, const SourceLocation &Loc,
                   const bsc::Place *fullFieldName);
    llvm::SmallVector<OwnershipDiagInfo>
    checkMemoryLeak(const VarDecl *VD, const SourceLocation &Loc,
                    bool isDestructor);

    OwnershipStatus() : Vars(0) {}

  private:
    // init* register the owned field paths of a tracked variable into its
    // schema: Parent threads the place of the enclosing field chain from the
    // root variable (nullptr at the root). Field keys are Field chains from
    // the root; nested owned pointer keys are Deref(Field(...)); array-field
    // levels are Index.
    void initStruct(const RecordDecl *RD, const VarDecl *VD, ASTContext &Ctx,
                    int depth = 10, const bsc::Place *Parent = nullptr);
    void initBOP(QualType QT, const VarDecl *VD, ASTContext &Ctx, int depth = 10,
                 const bsc::Place *Parent = nullptr);
    // The __assume_null / __forget transitions over an explicit key set: the
    // whole-object variants pass the static schema, the "under path" variants
    // the field descendants of one path. Both dispatch on the host family.
    void assumeFieldsNull(const VarDecl *VD, const FieldSet &Keys);
    void forgetFields(const VarDecl *VD, const FieldSet &Keys);
    // The explicit-drop family for state S: Null asserts the fields null
    // (__assume_null / `= nullptr`), Moved abandons them (__forget).
    void dropFields(Status S, const VarDecl *VD, const FieldSet &Keys);
    // Drop one owned pointer member of an S / OPS host, in the family S: the
    // Null family moves it (and its field descendants) into the Null set, the
    // Moved family clears Null and marks the S-host fields as moved.
    void dropOwnedField(Status S, const VarDecl *VD,
                        const bsc::Place *FieldPath);
    // Shared implementation of setToNull / setToMoved over an expression.
    void applyExplicitDrop(Status S, const Expr *E, ASTContext &Ctx);
    // __assume_null(*q) / __forget(*q) on a pointer-to-owned-pointer
    // (int *_Owned *q): drop the inner owned pointer tracked as the Deref field
    // of a BOP host, then refresh its aggregate root state.
    void dropBOPDerefField(const VarDecl *RootVD, const bsc::Place *DerefPlace);
    std::string collectMovedFields(const VarDecl *VD);
  };
};

static const unsigned OwnershipDiagIdList[] = {
    diag::err_ownership_use_moved,
    diag::err_ownership_use_partially_moved,
    diag::err_ownership_use_all_moved,
    diag::err_ownership_use_possibly_uninit,
    diag::err_ownership_use_uninit,
    diag::err_ownership_assign_owned,
    diag::err_ownership_assign_partially_moved,
    diag::err_ownership_assign_partially_moved,
    diag::err_ownership_assign_all_moved,
    diag::err_ownership_assign_field_uninit,
    diag::err_ownership_assign_field_owned,
    diag::err_ownership_assign_field_moved,
    diag::err_ownership_assign_field_subfield_owned,
    diag::err_ownership_cast_moved,
    diag::err_ownership_cast_uninit,
    diag::err_ownership_cast_subfield_owned,
    diag::err_ownership_memory_leak_field,
    diag::err_ownership_memory_leak,
    diag::err_ownership_array_elem_transfer_forbidden,
    diag::err_ownership_array_elem_path_inconsistent,
    diag::err_ownership_array_elem_assign_owned,
    diag::err_ownership_owned_struct_partially_moved,
    diag::err_ownership_owned_struct_not_properly_freed,
    diag::err_ownership_cast_pass_to_arg_or_ret};

// One diag:: per OwnershipDiagKind, in enum order: getOwnershipDiagID indexes
// this table by the enum value, and the runtime assert only bounds the index —
// a kind added in the middle would silently shift every later entry onto the
// wrong diagnostic.
static_assert(sizeof(OwnershipDiagIdList) / sizeof(OwnershipDiagIdList[0]) ==
                  static_cast<size_t>(OwnershipMaxDiagKind),
              "OwnershipDiagIdList must have one entry per OwnershipDiagKind");

class OwnershipDiagReporter {
  Sema &S;
  std::vector<OwnershipDiagInfo> DIV;

public:
  OwnershipDiagReporter(Sema &S) : S(S) {}

  void addDiags(llvm::SmallVector<OwnershipDiagInfo> &diags) {
    for (auto it = diags.begin(), ei = diags.end(); it != ei; ++it) {
      addDiagInfo(*it);
    }
  }

  void addDiagInfo(OwnershipDiagInfo &DI) {
    for (auto it = DIV.begin(), ei = DIV.end(); it != ei; ++it) {
      if (DI == *it)
        return;
    }
    if (S.getDiagnostics().getDiagnosticLevel(getOwnershipDiagID(DI.Kind),
                                              DI.Loc) ==
        DiagnosticsEngine::Ignored) {
      return;
    }
    DIV.push_back(DI);
  }

  unsigned getOwnershipDiagID(OwnershipDiagKind Kind) {
    unsigned index = static_cast<unsigned>(Kind);
    assert(index <
               static_cast<unsigned>(OwnershipDiagKind::OwnershipMaxDiagKind) &&
           "Unknown error type");
    return OwnershipDiagIdList[index];
  }

  void flushDiagnostics() {
    // Sort the diag info by SourceLocation. While not strictly
    // guaranteed to produce them in line/column order, this will provide
    // a stable ordering.
    std::sort(DIV.begin(), DIV.end(),
              [this](const OwnershipDiagInfo &a, const OwnershipDiagInfo &b) {
                return S.getSourceManager().isBeforeInTranslationUnit(a.Loc,
                                                                      b.Loc);
              });

    for (const OwnershipDiagInfo &DI : DIV) {
      switch (DI.Kind) {
      case InvalidAssignOfOwned:
      case InvalidAssignOfAllMoved:
      case InvalidAssignFieldOfUninit:
      case InvalidAssignFieldOfOwned:
      case InvalidAssignFieldOfMoved:
      case InvalidCastMoved:
      case InvalidCastUninit:
      case InvalidUseOfAllMoved:
      case InvalidUseOfMoved:
      case InvalidUseOfPossiblyUninit:
      case InvalidUseOfUninit:
      case MemoryLeak:
      case ArrayElemPathInconsistent:
        S.Diag(DI.Loc, getOwnershipDiagID(DI.Kind)) << DI.Name;
        break;
      case ArrayElemTransferForbidden:
      case ArrayElemAssignOwned:
        S.Diag(DI.Loc, getOwnershipDiagID(DI.Kind)) << DI.Name;
        // When the rejected site sits inside a non-qualifying for-loop,
        // explain which qualifying-loop rule failed.
        if (DI.NoteLoc.isValid())
          S.Diag(DI.NoteLoc, diag::note_ownership_array_elem_not_qualifying)
              << DI.Note;
        break;
      case InvalidAssignOfPartiallyMoved:
      case InvalidAssignOfPossiblyPartiallyMoved:
        S.Diag(DI.Loc, getOwnershipDiagID(DI.Kind))
            << DI.Name << DI.Fields
            << (DI.Kind == InvalidAssignOfPossiblyPartiallyMoved ? 1 : 0);
        break;
      case InvalidAssignSubFieldOwned:
      case InvalidCastFieldOwned:
      case InvalidUseOfPartiallyMoved:
      case FieldMemoryLeak:
      case OwnedStructPartiallyMoved:
      case OwnedStructNotProperlyFreed:
        S.Diag(DI.Loc, getOwnershipDiagID(DI.Kind)) << DI.Name << DI.Fields;
        break;
      case PassCastToArgOrRet:
        S.Diag(DI.Loc, getOwnershipDiagID(DI.Kind)) << DI.Type;
        break;
      default:
        llvm_unreachable("Unknown error type");
        break;
      }
      S.getDiagnostics().increaseOwnershipErrors();
    }
  }

  unsigned getNumErrors() { return DIV.size(); }
};

/// Information about for-loops that may transfer ownership of owned array
/// elements. Filled by an AST pre-pass and consumed by
/// ownership analysis. The pre-pass only classifies loops and records where
/// element transfer is allowed / the net per-loop effect; the actual
/// ownership state transitions are performed by the CFG dataflow analysis.
struct OwnedArrayLoopInfo {
  /// Qualifying for-loops (full-range shape).
  llvm::SmallPtrSet<const ForStmt *, 8> QualifyingLoops;
  /// Per qualifying loop, the ArraySubscript / MemberExpr sites whose transfer
  /// that loop's classification validated (it drives one level of the access
  /// and every other level is iterated in full by its own loop). The loop is
  /// part of the key on purpose: a site validated by one loop must not be
  /// allowed at an occurrence that only an unrelated loop encloses.
  llvm::DenseMap<const ForStmt *, llvm::SmallPtrSet<const Expr *, 16>>
      AllowedTransferExprs;
  /// For-loops that are NOT qualifying, with the rule that failed and the
  /// exact location to point the note at (e.g. the return/break statement or
  /// the loop-variable modification site; invalid when the failure is at the
  /// loop head itself).
  llvm::DenseMap<const ForStmt *, LoopFailure> NonQualifyingLoops;
  /// Per qualifying loop, the arrays it covers (shape-check-valid loops whose
  /// bound matches that array's length). The loop header only drops the
  /// back-edge state of *these* arrays, and the exit takes their state from
  /// the back edge.
  llvm::DenseMap<const ForStmt *,
                 llvm::SmallVector<const VarDecl *, 4>> LoopCoveredArrays;
  /// Per qualifying-shape loop, the arrays accessed inside it that are NOT
  /// covered, with the per-array reason (bound mismatch / index misuse /
  /// VLA length mismatch) and the location to point the note at. Used to
  /// attach a per-array note even when the loop itself qualifies for another
  /// array.
  llvm::DenseMap<const ForStmt *,
                 llvm::DenseMap<const VarDecl *, LoopFailure>>
      NonCoveredArrays;
};

void runOwnershipAnalysis(const FunctionDecl &fd, const CFG &cfg,
                          AnalysisDeclContext &ac,
                          OwnershipDiagReporter &reporter,
                          ASTContext &ctx);

// AST pre-pass that decides which for-loops may transfer ownership of
// owned-element arrays (the "qualifying for-loop" classifier). It records
// qualifying loops, the arrays they cover, the transfer sites allowed inside
// them, and — for for-loops that are NOT qualifying — the rule that failed
// (for a diagnostic note). It computes no ownership state itself.
void classifyOwnedArrayLoops(ASTContext &Context, const FunctionDecl *FD,
                             OwnedArrayLoopInfo &LoopInfo);

} // end namespace clang

#endif // ENABLE_BSC

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_BSCOWNERSHIP_H