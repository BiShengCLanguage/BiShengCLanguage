//===- BSCBorrowChecker.h - Borrow Check for Source CFGs -*- BSC --*----------//
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

#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_BSCBORROWCHECKER_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_BSCBORROWCHECKER_H

#if ENABLE_BSC

#include "clang/AST/BSC/TypeBSC.h"
#include "clang/AST/Decl.h"
#include "clang/Analysis/CFG.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"
#include <set>

#define DEBUG_PRINT 0

namespace clang {
namespace borrow {
class DefVarianceAnalysis;
class Environment;
class RegionCheck;
class RegionGenerator;
class Ty;

/// Name of region. Each RegionName corresponds to an AST node.
///
/// Each bound region is named `'region` plus a positive integer,
/// such as 'region_0, 'region_1, etc. Bound region is related to
/// variables or a borrow/reborrow expression in the function.
/// The free region is named 'region_r. Free region is related to
/// the return point in the function or points in the caller.
class RegionName {
private:
  RegionName(std::string Name) : Name(Name) {}

public:
  std::string Name;
  constexpr static const char *const NamePrefix = "'region_";
  static unsigned Cnt;

  bool operator<(const RegionName &other) const { return Name < other.Name; }

  static RegionName Create() {
    return RegionName(NamePrefix + std::to_string(Cnt++));
  }

  static llvm::SmallVector<RegionName, 2> CreateN(unsigned N) {
    llvm::SmallVector<RegionName, 2> Regions;
    for (unsigned I = 0; I != N; ++I)
      Regions.push_back(Create());
    return Regions;
  }

  static RegionName CreateFree() {
    return RegionName(std::string(NamePrefix) + "r");
  }

  static const RegionName &GetUntracked();

  RegionName() { Name = "invalid"; }

  bool isInvalid() const { return Name == "invalid"; }
  bool isUntracked() const { return Name == GetUntracked().Name; }
};

/// An index representation of RegionName, in order to facilitate calculations.
///
/// Each RegionName corresponds to a RegionVariable.
/// The index of RegionVariable increases from 0.
struct RegionVariable {
  unsigned index;

  RegionVariable(unsigned index = 0) : index(index) {}
};

class Place;
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Place *P);

/// `Place` models a borrow-checkable memory location as a root variable
/// followed by zero or more field, dereference, or index projections.
///
/// Examples:
/// - the Place of `a` is `Var("a")`.
/// - the Place of `p.a` is `Field(Var("p"), "a")`.
/// - the Place of `p->a` is `Field(Deref(Var("p")), "a")`.
/// - the Place of `*p.a` is `Deref(Field(Var("p"), "a"))`.
/// - the Place of `*(p->a)` is
///   `Deref(Field(Deref(Var("p")), "a"))`.
/// - the Place of `p->a->b` is
///   `Field(Deref(Field(Deref(Var("p")), "a")), "b")`.
/// - the Place of `p[i]` is `Index(Var("p"))`; the concrete index is
///   intentionally not part of a Place.
class Place {
public:
  enum class Kind : unsigned char { Var, Field, Deref, Index };

private:
  Kind K;
  const Place *Base = nullptr;
  llvm::StringRef Name;
  const Ty *T = nullptr;
  const Decl *D = nullptr;
  SourceLocation Loc;

  Place(Kind K, const Place *Base, llvm::StringRef Name, const Ty *T,
        const Decl *D, SourceLocation Loc)
      : K(K), Base(Base), Name(Name), T(T), D(D), Loc(Loc) {}

  static const Place *CreateVar(const ASTContext &Ctx, const NamedDecl *D,
                                const Ty *T, SourceLocation Loc) {
    return new (Ctx) Place(Kind::Var, nullptr, D->getName(), T, D, Loc);
  }

  static const Place *CreateField(const ASTContext &Ctx, const Place *Base,
                                  const FieldDecl *FD, const Ty *T,
                                  SourceLocation Loc) {
    return new (Ctx) Place(Kind::Field, Base, FD->getName(), T, FD, Loc);
  }

  static const Place *CreateDeref(const ASTContext &Ctx, const Place *Base,
                                  const Ty *T, SourceLocation Loc) {
    return new (Ctx) Place(Kind::Deref, Base, llvm::StringRef(), T, nullptr,
                           Loc);
  }

  static const Place *CreateIndex(const ASTContext &Ctx, const Place *Base,
                                  const Ty *T, SourceLocation Loc) {
    return new (Ctx) Place(Kind::Index, Base, llvm::StringRef(), T, nullptr,
                           Loc);
  }

public:
  Kind getKind() const { return K; }
  const Place *getBase() const { return Base; }
  llvm::StringRef getName() const { return Name; }
  const Ty *getType() const { return T; }
  SourceLocation getLocation() const { return Loc; }

  std::string toString() const {
    std::string Result;
    llvm::raw_string_ostream OS(Result);
    OS << this;
    return Result;
  }

  /// Compare abstract borrow-check places. Type and source location are not
  /// part of the identity, and dereference and index projections are
  /// considered equivalent.
  bool equals(const Place *Other) const;

  /// The prefixes of a place are all the places obtained by stripping away
  /// field, dereference, and index projections.
  ///
  /// Examples:
  ///
  /// - the prefixes of `*a.b` where `a` is a struct are
  ///   `*a.b`, `a.b`, and `a`.
  /// - the prefixes of `a.b.c` where both `a` and `b` are structs are
  ///   `a.b.c`, `a.b`, and `a`.
  /// - the prefixes of `p[i]` are `p[_]` and `p`, where `p[_]` represents
  ///   indexing `p` without distinguishing the concrete index.
  llvm::SmallVector<const Place *> prefixes() const;

  /// The supporting prefixes of a place are all the prefixes that must remain
  /// valid for the place itself to remain valid. For the most part, this means
  /// all prefixes, except that traversal stops when dereferencing or indexing
  /// through a shared borrow pointer.
  ///
  /// Examples:
  ///
  /// - the supporting prefixes of `s.f` where `s` is a struct are
  ///   `s.f` and `s`.
  /// - the supporting prefixes of `(*r).f` where `r` is a shared borrow are
  ///   `(*r).f` and `*r`, but not `r`.
  ///   - Intuition: one could copy `r` into a temporary `t` and reach the data
  ///     through `(*t).f`, so it is not important to preserve `r` itself.
  /// - the supporting prefixes of `(*m).f` where `m` is a mutable borrow are
  ///   `(*m).f`, `*m`, and `m`.
  /// - the supporting prefixes of `r[i]` where `r` is a shared borrow are
  ///   `r[_]`, but not `r`.
  llvm::SmallVector<const Place *> supportingPrefixes() const;

  friend class PlaceBuilder;
};

enum class BorrowKind { Mut, Shared };

enum class Variance { Co, Contra, In, Bi };

class Ty {
public:
  enum class TyKind : unsigned char {
    Base,
    Pointer,
    Struct,
    Array,
  };

private:
  /// Active variant of this `Ty`. The payload fields below are valid according
  /// to this kind.
  TyKind Kind;

  /// Clang type represented by this node. Pointer ownership/borrow qualifiers
  /// are intentionally read from this `QualType`, not duplicated in `Ty`.
  QualType QT;

  /// Valid only for borrow pointer nodes. Points into the precomputed region
  /// vector owned by Environment, or to the process-lifetime untracked region
  /// for borrow pointers reached through a raw pointer. This pointer must not
  /// be used after the backing storage is destroyed unless it is untracked.
  const RegionName *RN = nullptr;

  /// Valid only for pointer nodes. The pointee retains its structural shape
  /// across raw pointers even though regions beyond that boundary are not
  /// currently tracked. Owned by the ASTContext arena; `Ty` nodes are never
  /// freed individually.
  const Ty *Pointee = nullptr;

  /// Valid only for array nodes. Arena-owned, see `Pointee`.
  const Ty *Element = nullptr;

  /// Valid only for struct nodes. These are the implicit region parameters of
  /// the aggregate type, not field-level payloads. The pointer array is owned
  /// by the ASTContext arena; each region is owned by Environment or is the
  /// process-lifetime untracked region.
  llvm::ArrayRef<const RegionName *> RegionParams;

  Ty(TyKind Kind, QualType QT) : Kind(Kind), QT(QT) {}

  static Ty *CreateBase(const ASTContext &Ctx, QualType QT) {
    assert(!QT->isPointerType() && !QT->isArrayType() &&
           !QT->getAs<RecordType>() &&
           "CreateBase only accepts non-structural types");
    return new (Ctx) Ty(TyKind::Base, QT);
  }

  static Ty *CreateStruct(const ASTContext &Ctx, QualType QT,
                          llvm::ArrayRef<const RegionName *> Regions) {
    assert(QT->getAs<RecordType>() &&
           "CreateStruct only valid on a record type");
    Ty *T = new (Ctx) Ty(TyKind::Struct, QT);
    T->RegionParams = Regions.copy(Ctx.getAllocator());
    return T;
  }

  static Ty *CreateArray(const ASTContext &Ctx, QualType QT,
                         const Ty *ElementTy) {
    assert(QT->isArrayType() && "CreateArray only valid on an array type");
    Ty *T = new (Ctx) Ty(TyKind::Array, QT);
    T->Element = ElementTy;
    return T;
  }

public:
  /// Build the `Ty` tree for `QT` using record layouts available through `Env`
  /// and regions from `Regions` in outermost-borrow-first, depth-first order.
  ///
  /// Although nodes are arena-allocated, their region pointers refer to
  /// Environment-owned storage. A returned `Ty` is intended only for local
  /// borrow-checking work and must not be cached on AST nodes or outlive `Env`.
  static Ty *Create(const Environment &Env, QualType QT,
                    llvm::ArrayRef<const RegionName *> Regions);

  static Ty *CreatePointer(const ASTContext &Ctx, QualType QT,
                           const RegionName *Region,
                           const Ty *PointeeTy) {
    assert(QT->isPointerType() && "CreatePointer only valid on a pointer type");
    Ty *T = new (Ctx) Ty(TyKind::Pointer, QT);
    T->RN = Region;
    T->Pointee = PointeeTy;
    return T;
  }

  /// Build the field type projected from a struct Ty and its region parameters.
  static Ty *CreateField(const Environment &Env, const Ty *BaseTy,
                         const FieldDecl *FD);

  TyKind getKind() const { return Kind; }
  QualType getQualType() const { return QT; }

  bool isBorrowPointer() const {
    return Kind == TyKind::Pointer && QT.isBorrowQualified();
  }

  bool isOwnedPointer() const {
    return Kind == TyKind::Pointer && !QT.isNull() && QT.isOwnedQualified();
  }

  bool isRawPointer() const {
    return Kind == TyKind::Pointer && !QT.isNull() && !QT.isBorrowQualified() &&
           !QT.isOwnedQualified();
  }

  BorrowKind getBorrowKind() const {
    assert(isBorrowPointer() && "only borrow pointer has borrow kind");
    return QT.isConstBorrow() ? BorrowKind::Shared : BorrowKind::Mut;
  }

  RegionName getRegion() const {
    assert(isBorrowPointer() && "only borrow pointer has region");
    assert(RN && "borrow pointer should reference a region");
    return *RN;
  }

  const Ty *getPointee() const {
    assert(Kind == TyKind::Pointer && "only pointer type has a pointee");
    return Pointee;
  }

  const Ty *getElement() const {
    assert(Kind == TyKind::Array && "only array type has an element");
    return Element;
  }

  llvm::ArrayRef<const RegionName *> getRegionParams() const {
    assert(Kind == TyKind::Struct &&
           "only struct type has region parameters");
    return RegionParams;
  }
};

enum class BorrowDiagKind {
  ForImmutWhenMut,
  ForMove,
  ForMultiMut,
  ForMutWhenImmut,
  ForRead,
  ForWrite,
  ForStorageDead,
  LifetimeNotLong,
};

struct BorrowDiagInfo {
  BorrowDiagKind Kind;
  SourceLocation Location;
  const Place *place = nullptr;
  SourceLocation LoanLoc;
  const Place *loanPlace = nullptr;

  BorrowDiagInfo(BorrowDiagKind Kind, SourceLocation Location)
      : Kind(Kind), Location(Location) {}

  BorrowDiagInfo(BorrowDiagKind Kind, SourceLocation Location,
                 const Place *place, SourceLocation LoanLoc)
      : Kind(Kind), Location(Location), place(place), LoanLoc(LoanLoc) {}

  BorrowDiagInfo(BorrowDiagKind Kind, SourceLocation Location,
                 const Place *place, SourceLocation LoanLoc,
                 const Place *loanPlace)
      : Kind(Kind), Location(Location), place(place), LoanLoc(LoanLoc),
        loanPlace(loanPlace) {}
};

/// An abstraction of CFG nodes.
///
/// Every CFG node is converted to one or more actions.
struct Action {
  enum ActionKind {
    Noop,
    Init,
    Borrow,
    Assign,
    Aggregate,
    Use,
    Call,
    Return,
    StorageDead
  };

  ActionKind Kind;

  Action(ActionKind Kind) : Kind(Kind) {}

  virtual ~Action() = default;

  ActionKind getKind() const { return Kind; }

  virtual llvm::Optional<const Place *> OverWrites() const {
    return llvm::None;
  }

private:
  virtual void anchor();
};

/// ActionNoop represents statement that does not use any variables.
struct ActionNoop : public Action {
  ActionNoop() : Action(Noop) {}

  ~ActionNoop() override = default;

  static const ActionNoop *Create(const ASTContext &Ctx) {
    return new (Ctx) ActionNoop();
  }

  static bool classof(const Action *A) { return A->getKind() == Noop; }

private:
  void anchor() override;
};

/// ActionCall represents a function call.
struct ActionCall final : public Action {
  struct Arg {
    /// Null for a variadic argument without a corresponding parameter type.
    const Ty *ParamTy;
    /// Null when the argument does not denote a place.
    const Place *Value;
  };

  /// Call-site anchor for diagnostics and call-specific queries.
  const CallExpr *CE;
  const Ty *ReturnTy;
  const Place *Result;
  llvm::ArrayRef<Arg> Args;

  ~ActionCall() override = default;

  static const ActionCall *Create(const ASTContext &Ctx, const CallExpr *CE,
                                  const Ty *ReturnTy, const Place *Result,
                                  llvm::ArrayRef<Arg> Args) {
    return new (Ctx)
        ActionCall(CE, ReturnTy, Result, Args.copy(Ctx.getAllocator()));
  }

  llvm::Optional<const Place *> OverWrites() const override {
    if (!Result)
      return llvm::None;
    return llvm::Optional<const Place *>(Result);
  }

  static bool classof(const Action *A) { return A->getKind() == Call; }

private:
  ActionCall(const CallExpr *CE, const Ty *ReturnTy, const Place *Result,
             llvm::ArrayRef<Arg> Args)
      : Action(Call), CE(CE), ReturnTy(ReturnTy), Result(Result),
        Args(Args) {}

  void anchor() override;
};

/// ActionReturn represents returning from the current function.
struct ActionReturn final : public Action {
  const Ty *ReturnTy;
  /// Null when the returned value does not denote a place.
  const Place *Value;

  ~ActionReturn() override = default;

  static const ActionReturn *Create(const ASTContext &Ctx, const Ty *ReturnTy,
                                    const Place *Value) {
    return new (Ctx) ActionReturn(ReturnTy, Value);
  }

  static bool classof(const Action *A) { return A->getKind() == Return; }

private:
  ActionReturn(const Ty *ReturnTy, const Place *Value)
      : Action(Return), ReturnTy(ReturnTy), Value(Value) {}

  void anchor() override;
};

/// ActionBorrow represents a statement that explicity borrows a variable using
/// `&mut`, `&const`, `&mut *`, or `&const *`.
struct ActionBorrow final : public Action {
  const Place *Dest;
  BorrowKind BK;
  const Ty *BorrowTy;
  const Place *Source;

  ~ActionBorrow() override = default;

  static const ActionBorrow *Create(const ASTContext &Ctx, const Place *Dest,
                                    BorrowKind BK, const Ty *BorrowTy,
                                    const Place *Source) {
    return new (Ctx) ActionBorrow(Dest, BK, BorrowTy, Source);
  }

  llvm::Optional<const Place *> OverWrites() const override {
    return llvm::Optional<const Place *>(Dest);
  }

  static bool classof(const Action *A) { return A->getKind() == Borrow; }

private:
  ActionBorrow(const Place *Dest, BorrowKind BK, const Ty *BorrowTy,
               const Place *Source)
      : Action(Borrow), Dest(Dest), BK(BK), BorrowTy(BorrowTy), Source(Source) {}

  void anchor() override;
};

/// ActionInit represents an assignment whose right-hand side is evaluated as a
/// collection of source places.
struct ActionInit final : public Action {
  const Place *Dest;
  llvm::ArrayRef<const Place *> Sources;

  ~ActionInit() override = default;

  static const ActionInit *Create(const ASTContext &Ctx, const Place *Dest,
                                  llvm::ArrayRef<const Place *> Sources) {
    return new (Ctx) ActionInit(Dest, Sources.copy(Ctx.getAllocator()));
  }

  llvm::Optional<const Place *> OverWrites() const override {
    return llvm::Optional<const Place *>(Dest);
  }

  static bool classof(const Action *A) { return A->getKind() == Init; }

private:
  ActionInit(const Place *Dest, llvm::ArrayRef<const Place *> Sources)
      : Action(Init), Dest(Dest), Sources(Sources) {}

  void anchor() override;
};

/// ActionAssign represents an assignment between borrow-typed places. It is not
/// used for ordinary non-borrow assignments.
struct ActionAssign final : public Action {
  const Place *Dest;
  const Place *Source;

  ~ActionAssign() override = default;

  static const ActionAssign *Create(const ASTContext &Ctx, const Place *Dest,
                                    const Place *Source) {
    return new (Ctx) ActionAssign(Dest, Source);
  }

  llvm::Optional<const Place *> OverWrites() const override {
    return llvm::Optional<const Place *>(Dest);
  }

  static bool classof(const Action *A) { return A->getKind() == Assign; }

private:
  ActionAssign(const Place *Dest, const Place *Source)
      : Action(Assign), Dest(Dest), Source(Source) {}

  void anchor() override;
};

/// ActionAggregate represents assigning or initializing an aggregate that
/// contains tracked subobjects.
struct ActionAggregate final : public Action {
  enum class AggregateKind {
    /// A whole-place transfer such as `s1 = s2`. Mutable borrow subobjects
    /// implicitly reborrow their corresponding source subobjects.
    Copy,
    /// Element-wise construction from normalized initializer temporaries. The
    /// temporary assignments have already introduced any required reborrows.
    Init,
  };

  struct Initializer {
    const Ty *DestTy;
    const Place *Value;
  };

  struct ImplicitReborrow {
    /// The mutable borrow type of the corresponding destination subobject.
    const Ty *DestTy;
    /// The source place borrowed by the implicit reborrow.
    const Place *BorrowedPlace;
  };

  AggregateKind AK;
  /// The root aggregate place overwritten by this action.
  const Place *Dest;
  /// The root source aggregate. Valid only for Copy.
  const Place *CopySource;
  /// Initializer values and their destination subobject types. Valid only for
  /// Init.
  llvm::ArrayRef<Initializer> Initializers;
  /// Mutable implicit reborrows produced by a whole-aggregate copy. Valid only
  /// for Copy.
  llvm::ArrayRef<ImplicitReborrow> ImplicitReborrows;

  ~ActionAggregate() override = default;

  static const ActionAggregate *
  CreateCopy(const ASTContext &Ctx, const Place *Dest, const Place *Source,
             llvm::ArrayRef<ImplicitReborrow> ImplicitReborrows) {
    return new (Ctx) ActionAggregate(
        AggregateKind::Copy, Dest, Source, llvm::ArrayRef<Initializer>(),
        ImplicitReborrows.copy(Ctx.getAllocator()));
  }

  static const ActionAggregate *
  CreateInit(const ASTContext &Ctx, const Place *Dest,
             llvm::ArrayRef<Initializer> Initializers) {
    return new (Ctx) ActionAggregate(
        AggregateKind::Init, Dest, nullptr,
        Initializers.copy(Ctx.getAllocator()),
        llvm::ArrayRef<ImplicitReborrow>());
  }

  llvm::Optional<const Place *> OverWrites() const override {
    return llvm::Optional<const Place *>(Dest);
  }

  static bool classof(const Action *A) {
    return A->getKind() == Aggregate;
  }

private:
  ActionAggregate(AggregateKind AK, const Place *Dest,
                  const Place *CopySource,
                  llvm::ArrayRef<Initializer> Initializers,
                  llvm::ArrayRef<ImplicitReborrow> ImplicitReborrows)
      : Action(Aggregate), AK(AK), Dest(Dest), CopySource(CopySource),
        Initializers(Initializers), ImplicitReborrows(ImplicitReborrows) {}

  void anchor() override;
};

/// ActionUse represents evaluating a collection of places.
struct ActionUse final : public Action {
  llvm::ArrayRef<const Place *> Places;

  ~ActionUse() override = default;

  static const ActionUse *Create(const ASTContext &Ctx,
                                 llvm::ArrayRef<const Place *> Places) {
    return new (Ctx) ActionUse(Places.copy(Ctx.getAllocator()));
  }

  static bool classof(const Action *A) { return A->getKind() == Use; }

private:
  ActionUse(llvm::ArrayRef<const Place *> Places)
      : Action(Use), Places(Places) {}

  void anchor() override;
};

/// ActionStorageDead represents leaving the lexical scope of a place.
struct ActionStorageDead final : public Action {
  const Place *P;

  ~ActionStorageDead() override = default;

  static const ActionStorageDead *Create(const ASTContext &Ctx,
                                         const Place *P) {
    return new (Ctx) ActionStorageDead(P);
  }

  llvm::Optional<const Place *> OverWrites() const override {
    return llvm::Optional<const Place *>(P);
  }

  static bool classof(const Action *A) {
    return A->getKind() == StorageDead;
  }

private:
  ActionStorageDead(const Place *P) : Action(StorageDead), P(P) {}

  void anchor() override;
};

/// Represents the position of nodes in the cfg.
///
/// `blockID` represents the index of the basic block, and `index` represents
/// the index of node in the current basic block.
struct Point {
  unsigned blockID;
  unsigned index;

private:
  Point(unsigned blockID, unsigned index) : blockID(blockID), index(index) {}

public:
  static Point Create(const CFGBlock *Block, CFGBlock::const_iterator It) {
    return Point(Block->getBlockID(), std::distance(Block->begin(), It) + 1);
  }

  static Point Create(const CFGBlock *Block,
                      CFGBlock::const_reverse_iterator It) {
    return Point(Block->getBlockID(), std::distance(It, Block->rend()));
  }

  static Point MakeFirstPoint(const CFGBlock *Block) {
    assert(Block && !Block->empty() &&
           "first point requires a non-empty CFG block");
    return Point(Block->getBlockID(), 1);
  }

  Point Next(const CFGBlock *Block) const {
    assert(Block && "next point requires a CFG block");
    assert(blockID == Block->getBlockID() &&
           "point must belong to the given CFG block");
    assert(1 <= index && index < Block->size() &&
           "next point requires a non-last point in a non-empty CFG block");
    return Point(blockID, index + 1);
  }

  bool operator==(const Point &other) const {
    return blockID == other.blockID && index == other.index;
  }

  bool operator<(const Point &other) const {
    if (this->blockID != other.blockID)
      return this->blockID < other.blockID;
    return this->index < other.index;
  }
};

/// Represents the region scope of a region variable, consisting a series
/// points in the CFG and `End('r)` entries for free regions `'r`.
class Region {
private:
  std::set<Point> points;
  std::set<RegionName> endRegions;

public:
  bool AddPoint(Point P) {
    auto Ret = points.insert(P);
    return Ret.second;
  }

  bool AddEndRegion(RegionName RN) {
    auto Ret = endRegions.insert(RN);
    return Ret.second;
  }

  const std::set<RegionName> &getEndRegions() const { return endRegions; }

  bool MayContain(Point P) const { return points.find(P) != points.end(); }

  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Region &R);
};

/// A complete definition of a region variable, consisting of the corresponding
/// region name, region scope and a capped flag.
///
/// Note that each region variable corresponds to a VarDecl or an explicit or
/// implicit borrow expression.
struct VarDefinition {
  RegionName name;

  /// The current value of this region name. This is adjusted during region
  /// check by calls to `AddLivePoint` and `AddEndRegion`, and then finally
  /// adjusted further by the call to `Solve`.
  Region value;

  /// Capped region names should no longer have to grow as a result of
  /// inference. If they do wind up growing, we will report an error.
  bool capped;

  VarDefinition(RegionName name, Region value, bool capped)
      : name(name), value(value), capped(capped) {}
};

/// The constraint indicates `sub` outlives `sup` at `point`.
struct Constraint {
  RegionVariable sub;
  RegionVariable sup;

  /// CFG point where this outlives constraint starts to hold.
  Point point;

  /// Source location of the expression that created this constraint. It is used
  /// only when constraint solving reports an error for this constraint.
  SourceLocation diagLoc;

  Constraint(RegionVariable sub, RegionVariable sup, Point point,
             SourceLocation diagLoc = SourceLocation())
      : sub(sub), sup(sup), point(point), diagLoc(diagLoc) {}
};

class Environment {
public:
  using RegionMapKeyTy = llvm::PointerUnion<const Decl *, const Stmt *>;
  using RegionMapValueTy = llvm::SmallVector<RegionName, 2>;
  using RegionMap = llvm::DenseMap<RegionMapKeyTy, RegionMapValueTy>;
  using CallExprParamRegionMapValueTy =
      llvm::SmallVector<RegionMapValueTy, 4>;
  using CallExprParamRegionMap =
      llvm::DenseMap<const CallExpr *, CallExprParamRegionMapValueTy>;
  using DefVarianceMapValueTy = llvm::SmallVector<Variance, 2>;
  using DefVarianceMap =
      llvm::DenseMap<const RecordDecl *, DefVarianceMapValueTy>;
  using FreeRegionList = llvm::SmallVector<RegionName, 2>;

  const FunctionDecl &fd;
  const CFG &cfg;
  const ASTContext &Ctx;
  const RegionMap regionMap;
  const CallExprParamRegionMap callExprParamRegions;
  /// Lazily populated cache of fixed implicit-region layouts for records used
  /// while constructing borrow-checker types.
  mutable RecordRegionLayoutMap recordRegionLayouts;
  /// Declaration-site variance of each implicit region parameter, indexed in
  /// the same order as the corresponding Ty region parameters.
  const DefVarianceMap defVarianceMap;
  const FreeRegionList freeRegions;

#if DEBUG_PRINT
  void printRegionMapKey(RegionMapKeyTy Key) const;
  void printRegionMapValue(const RegionMapValueTy &Regions) const;
  void printRegionMap() const;
#endif

public:
  Environment(const FunctionDecl &fd, const CFG &cfg, const ASTContext &Ctx,
              RegionGenerator &RG, DefVarianceAnalysis &DVA);

  /// Returns references to the precomputed regions associated with an AST key.
  llvm::SmallVector<const RegionName *, 2>
  getRegions(RegionMapKeyTy Key) const;

  /// Returns references to the regions for the I-th parameter at a call.
  llvm::SmallVector<const RegionName *, 2>
  getCallExprParamRegions(const CallExpr *CE, unsigned I) const;

  /// Returns declaration-site variances for a record's implicit regions.
  llvm::ArrayRef<Variance> getDefVariances(const RecordDecl *RD) const;

  /// Returns the fixed implicit-region layout of a record, building it on
  /// demand.
  const RecordRegionLayout &getRecordRegionLayout(const RecordDecl *RD) const;

  /// Returns the indices used to project a record's regions to one field.
  llvm::ArrayRef<unsigned>
  getFieldRegionIndices(const FieldDecl *FD) const;

  llvm::SmallVector<Point> SuccessorPoints(Point point) const;
};

/// All the information required for inference solving, including the
/// definition of region variables and all the constraints in the function.
class InferenceContext {
private:
  llvm::SmallVector<VarDefinition> definitions;
  llvm::SmallVector<Constraint> constraints;

public:
  InferenceContext() {}

  RegionVariable AddVar(RegionName Name);
  void CapVar(RegionVariable RV) { definitions[RV.index].capped = true; }
  void AddLivePoint(RegionVariable RV, Point P);
  void AddEndRegion(RegionVariable RV, RegionName RN);
  void AddOutLives(RegionVariable Sup, RegionVariable Sub, Point P,
                   SourceLocation DiagLoc);

  const Region &getRegion(RegionVariable RV) const {
    return definitions[RV.index].value;
  }

  llvm::SmallVector<BorrowDiagInfo> Solve(const Environment &env);
};

class DFS {
private:
  llvm::SmallVector<Point, 4> stack;
  std::set<Point> visited;
  const Environment &env;

public:
  DFS(const Environment &env) : env(env) {}

  bool Copy(const Region &From, Region &To, Point StartPoint);
};

/// Compute the set of live variables at each point.
class Liveness {
private:
  using LivenessFact = llvm::DenseSet<VarDecl *>;

  const Environment &env;

  /// For a given key, which is a basic block, the value is the set of all live
  /// variables at the entry of the block.
  llvm::DenseMap<const CFGBlock *, LivenessFact> liveness;

  void Kill(LivenessFact &fact, VarDecl *D) { fact.erase(D); }

  void Gen(LivenessFact &fact, VarDecl *D) { fact.insert(D); }

  bool SetFrom(LivenessFact &Dest, const LivenessFact &Src) {
    if (Src.empty())
      return false;

    unsigned old = Dest.size();
    Dest.insert(Src.begin(), Src.end());
    return old != Dest.size();
  }

  template <typename CB>
  void SimulateBlock(LivenessFact &fact, const CFGBlock *Block, CB callback);

public:
  Liveness(const Environment &env) : env(env) { Compute(); }

  void Compute();

  template <typename CB> void Walk(CB callback);

  std::set<RegionName> LiveRegions(const LivenessFact &liveFact);

  void dump() const {
    llvm::outs() << "Liveness Result: \n";
    for (auto elem : liveness) {
      llvm::outs() << "CFG Block ID: " << elem.first->getBlockID() << '\n';
      llvm::outs() << "Live Variables at block entry: \n";
      for (VarDecl *var : elem.second) {
        llvm::outs() << var->getName() << '\t';
      }
      llvm::outs() << '\n';
    }
  }
};

struct Loan {
  Point point;
  const Place *place;
  BorrowKind kind;
  const Region &region;

  Loan(Point point, const Place *place, BorrowKind kind, const Region &region)
      : point(point), place(place), kind(kind), region(region) {}
};

class LoansInScope {
private:
  using LoansFact = llvm::DenseSet<unsigned>;

  const Environment &env;
  const RegionCheck &rc;

  /// All loans in the function.
  llvm::SmallVector<Loan> loans;

  llvm::DenseMap<const CFGBlock *, LoansFact> loansInScopeAfterBlock;

  /// For a given key, which is a point of the CFG, the value is a vector of
  /// the index of all loans at the entry of the point.
  std::map<Point, std::vector<unsigned>> loansByPoint;

  void Kill(LoansFact &fact, unsigned index) { fact.erase(index); }

  void Gen(LoansFact &fact, std::vector<unsigned> indexes) {
    fact.insert(indexes.begin(), indexes.end());
  }

  bool SetFrom(LoansFact &Dest, const LoansFact &Src) {
    if (Src.empty())
      return false;

    unsigned old = Dest.size();
    Dest.insert(Src.begin(), Src.end());
    return old != Dest.size();
  }

  llvm::SmallVector<unsigned> LoansNotInScopeAt(Point point) const {
    llvm::SmallVector<unsigned> ret;
    for (const Loan *it = loans.begin(), *ei = loans.end(); it != ei; ++it) {
      // A loan is not in scope at a point if its region does not contain the
      // point or the loan starts at that point.
      if (!it->region.MayContain(point) || it->point == point)
        ret.push_back(std::distance(loans.begin(), it));
    }
    return ret;
  }

  llvm::SmallVector<unsigned> LoansKilledByWriteTo(const Place *P) const;

  template <typename CB>
  void SimulateBlock(LoansFact &fact, const CFGBlock *Block, CB callback);

public:
  LoansInScope(const Environment &env, const RegionCheck &rc);

  void Compute();

  template <typename CB> void Walk(CB callback);
};

enum class Depth { Shallow, Deep };

enum class Mode { Read, Write };

class BorrowDiagReporter {
private:
  Sema &S;
  llvm::SmallVector<BorrowDiagInfo> Infos;

  void flushDiagnostics() {
    for (BorrowDiagInfo Info : Infos) {
      switch (Info.Kind) {
      case BorrowDiagKind::ForImmutWhenMut:
        S.Diag(Info.Location, diag::err_borrow_immut_borrow_when_mut_borrowed)
            << Info.place->toString();
        S.Diag(Info.LoanLoc, diag::note_mutable_borrow_occurs_here);
        break;
      case BorrowDiagKind::ForMove:
        S.Diag(Info.Location, diag::err_borrow_move_when_borrowed)
            << Info.place->toString();
        S.Diag(Info.LoanLoc, diag::note_borrowed_here)
            << Info.loanPlace->toString();
        break;
      case BorrowDiagKind::ForMultiMut:
        S.Diag(Info.Location, diag::err_borrow_mut_borrow_more_than_once)
            << Info.place->toString();
        S.Diag(Info.LoanLoc, diag::note_first_mut_borrow_occurs_here);
        break;
      case BorrowDiagKind::ForMutWhenImmut:
        S.Diag(Info.Location, diag::err_borrow_mut_borrow_when_immut_borrowed)
            << Info.place->toString();
        S.Diag(Info.LoanLoc, diag::note_immutable_borrow_occurs_here);
        break;
      case BorrowDiagKind::ForRead:
        S.Diag(Info.Location, diag::err_borrow_use_when_mut_borrowed)
            << Info.place->toString();
        S.Diag(Info.LoanLoc, diag::note_borrowed_here)
            << Info.loanPlace->toString();
        break;
      case BorrowDiagKind::ForWrite:
        S.Diag(Info.Location, diag::err_borrow_assign_when_borrowed)
            << Info.place->toString();
        S.Diag(Info.LoanLoc, diag::note_borrowed_here)
            << Info.place->toString();
        break;
      case BorrowDiagKind::ForStorageDead:
        S.Diag(Info.Location, diag::err_borrow_not_live_long)
            << Info.place->toString();
        S.Diag(Info.LoanLoc, diag::note_dropped_while_borrowed)
            << Info.place->toString();
        break;
      case BorrowDiagKind::LifetimeNotLong:
        S.Diag(Info.Location, diag::err_lifetime_may_not_live_long);
        break;
      }
      S.getDiagnostics().increaseBorrowCheckErrors();
    }
  }

public:
  BorrowDiagReporter(Sema &S) : S(S) {}

  ~BorrowDiagReporter() { flushDiagnostics(); }

  void ForImmutWhenMut(SourceLocation Location, const Place *P,
                       SourceLocation LoanLoc) {
    Infos.push_back(BorrowDiagInfo(BorrowDiagKind::ForImmutWhenMut, Location,
                                   P, LoanLoc));
  }

  void ForMove(SourceLocation Location, const Place *P,
               SourceLocation LoanLoc, const Place *LoanPlace) {
    Infos.push_back(BorrowDiagInfo(BorrowDiagKind::ForMove, Location,
                                   P, LoanLoc, LoanPlace));
  }

  void ForMultiMut(SourceLocation Location, const Place *P,
                   SourceLocation LoanLoc) {
    Infos.push_back(BorrowDiagInfo(BorrowDiagKind::ForMultiMut, Location,
                                   P, LoanLoc));
  }

  void ForMutWhenImmut(SourceLocation Location, const Place *P,
                       SourceLocation LoanLoc) {
    Infos.push_back(BorrowDiagInfo(BorrowDiagKind::ForMutWhenImmut, Location,
                                   P, LoanLoc));
  }

  void ForRead(SourceLocation Location, const Place *P,
               SourceLocation LoanLoc, const Place *LoanPlace) {
    Infos.push_back(BorrowDiagInfo(BorrowDiagKind::ForRead, Location,
                                   P, LoanLoc, LoanPlace));
  }

  void ForStorageDead(SourceLocation Location, const Place *P,
                      SourceLocation LoanLoc) {
    Infos.push_back(BorrowDiagInfo(BorrowDiagKind::ForStorageDead, LoanLoc,
                                   P, Location));
  }

  void ForWrite(SourceLocation Location, const Place *P,
                SourceLocation LoanLoc) {
    Infos.push_back(BorrowDiagInfo(BorrowDiagKind::ForWrite, Location,
                                   P, LoanLoc));
  }

  void LifetimeNotLong(SourceLocation Location) {
    Infos.push_back(BorrowDiagInfo(BorrowDiagKind::LifetimeNotLong, Location));
  }
};

class BorrowCheck {
private:
  BorrowDiagReporter &reporter;
  Point point;
  const llvm::SmallVector<Loan> &loans;
  bool IsBorrow = false;

  void CheckBorrows(Depth Depth, Mode AccessMode, const Place *P);
  void CheckMove(const Place *P);
  void CheckMutBorrow(const Place *P);
  void CheckRead(const Place *P);
  void CheckShallowWrite(const Place *P);
  void CheckStorageDead(const Place *P);

  llvm::SmallVector<const Loan *> FindLoansThatFreeze(const Place *P);
  llvm::SmallVector<const Loan *> FindLoansThatIntersect(const Place *P);
  llvm::SmallVector<const Place *> FrozenByBorrowOf(const Place *P);

public:
  BorrowCheck(BorrowDiagReporter &reporter, Point point,
              const llvm::SmallVector<Loan> &loans)
      : reporter(reporter), point(point), loans(loans) {}

  void CheckAction(const Action *A);
};

class RegionCheck {
private:
  BorrowDiagReporter &reporter;
  Environment env;
  InferenceContext infer;
  std::map<RegionName, RegionVariable> regionMap;
  std::map<Point, std::vector<const Action *>> actionMap;

  /// Map all RegionNames to corresponding RegionVariables.
  void MapRegionNamesToRegionVariables();

  /// Create the corresponding RegionVariable for a RegionName.
  /// If it already exists, return directly.
  void createRegionVariable(RegionName RN);

  /// Initialize all free region variables with CFG points and corresponding
  /// `End('r)`.
  void InitFreeRegions();

  /// Generate actions using the new action pipeline.
  void GenerateActions();

  void PopulateInference(Liveness &liveness);
  RegionVariable getRegionVariable(RegionName RN);
  void SubTypes(const Ty *Sub, const Ty *Sup, Point P,
                SourceLocation DiagLoc);
  void RelateTypes(const Ty *A, Variance V, const Ty *B, Point P,
                   SourceLocation DiagLoc);
  void RelateRegions(RegionName RegionA, Variance V, RegionName RegionB,
                     Point P, SourceLocation DiagLoc);
  void AddReborrowConstraints(RegionName RN, const Place *Source, Point P,
                              SourceLocation DiagLoc);

public:
  RegionCheck(const FunctionDecl &fd, const CFG &cfg, ASTContext &Ctx,
              BorrowDiagReporter &Reporter, RegionGenerator &RG,
              DefVarianceAnalysis &DVA);

  void Check();

  const Environment &getEnv() const { return env; }
  const Region &getRegion(RegionName RN) const;
  BorrowDiagReporter &getReporter() { return reporter; }
  const std::map<Point, std::vector<const Action *>> &getActionMap() const {
    return actionMap;
  }
};

void BorrowCk(RegionCheck &rc, LoansInScope &LIS);
void runBorrowChecker(const FunctionDecl &fd, const CFG &cfg, ASTContext &Ctx,
                      BorrowDiagReporter &Reporter);

} // end namespace borrow
} // end namespace clang

#endif // ENABLE_BSC

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_BSCBORROWCHECKER_H
