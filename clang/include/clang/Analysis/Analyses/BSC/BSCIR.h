//===- BSCIR.h - IR for BSC safety analyses -*- C++ -*----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the core types for BSCIR, a flat CFG-based intermediate
// representation shared by all BSC safety analyses (nullability, ownership,
// borrow checking, initialization).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_BSC_BSCIR_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_BSC_BSCIR_H

#if ENABLE_BSC

#include "clang/AST/ASTContext.h"
#include <cassert>
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/Specifiers.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Allocator.h"
#include <string>

namespace clang {
namespace bscir {

//===----------------------------------------------------------------------===//
// Forward Declarations
//===----------------------------------------------------------------------===//

struct BasicBlock;
struct Body;

//===----------------------------------------------------------------------===//
// LocalId - Identifies a local variable or temporary
//===----------------------------------------------------------------------===//

struct LocalId {
  unsigned Index;

  bool operator==(const LocalId &Other) const { return Index == Other.Index; }
  bool operator!=(const LocalId &Other) const { return Index != Other.Index; }
  bool operator<(const LocalId &Other) const { return Index < Other.Index; }
};

} // namespace bscir
} // namespace clang

// DenseMapInfo for LocalId
namespace llvm {
template <> struct DenseMapInfo<clang::bscir::LocalId> {
  static clang::bscir::LocalId getEmptyKey() {
    return {DenseMapInfo<unsigned>::getEmptyKey()};
  }
  static clang::bscir::LocalId getTombstoneKey() {
    return {DenseMapInfo<unsigned>::getTombstoneKey()};
  }
  static unsigned getHashValue(const clang::bscir::LocalId &Val) {
    return DenseMapInfo<unsigned>::getHashValue(Val.Index);
  }
  static bool isEqual(const clang::bscir::LocalId &LHS,
                      const clang::bscir::LocalId &RHS) {
    return LHS.Index == RHS.Index;
  }
};
} // namespace llvm

namespace clang {
namespace bscir {

//===----------------------------------------------------------------------===//
// BasicBlockId - Identifies a basic block
//===----------------------------------------------------------------------===//

struct BasicBlockId {
  unsigned Index;

  bool operator==(const BasicBlockId &Other) const {
    return Index == Other.Index;
  }
  bool operator!=(const BasicBlockId &Other) const {
    return Index != Other.Index;
  }
  bool operator<(const BasicBlockId &Other) const {
    return Index < Other.Index;
  }
};

} // namespace bscir
} // namespace clang

namespace llvm {
template <> struct DenseMapInfo<clang::bscir::BasicBlockId> {
  static clang::bscir::BasicBlockId getEmptyKey() {
    return {DenseMapInfo<unsigned>::getEmptyKey()};
  }
  static clang::bscir::BasicBlockId getTombstoneKey() {
    return {DenseMapInfo<unsigned>::getTombstoneKey()};
  }
  static unsigned getHashValue(const clang::bscir::BasicBlockId &Val) {
    return DenseMapInfo<unsigned>::getHashValue(Val.Index);
  }
  static bool isEqual(const clang::bscir::BasicBlockId &LHS,
                      const clang::bscir::BasicBlockId &RHS) {
    return LHS.Index == RHS.Index;
  }
};
} // namespace llvm

namespace clang {
namespace bscir {

//===----------------------------------------------------------------------===//
// LocalDecl - Declaration of a local variable or temporary
//===----------------------------------------------------------------------===//

struct LocalDecl {
  LocalId Id;
  QualType Ty;
  const VarDecl *OriginalDecl; // null for temporaries
  StringRef Name; // Backed by IdentifierInfo (outlives IR) or temp name storage
  bool IsTemp;
  SourceLocation DeclLoc;

  LocalDecl(LocalId Id, QualType Ty, const VarDecl *OriginalDecl,
            StringRef Name, bool IsTemp, SourceLocation DeclLoc)
      : Id(Id), Ty(Ty), OriginalDecl(OriginalDecl), Name(Name),
        IsTemp(IsTemp), DeclLoc(DeclLoc) {}
};

//===----------------------------------------------------------------------===//
// ProjectionElem - Element of a place projection chain
//===----------------------------------------------------------------------===//

struct ProjectionElem {
  enum Kind : uint8_t { Deref, Field, Index, ConstantIndex };

  Kind K;
  unsigned FieldIndex = 0; // Only meaningful for Field; fills padding after K.
  QualType ResultTy;
  union {
    const FieldDecl *FD;   // Field
    LocalId IndexLocal;    // Index
    uint64_t ConstIdx;     // ConstantIndex
  };

  static ProjectionElem createDeref(QualType ResultTy) {
    ProjectionElem E;
    E.K = Deref;
    E.ConstIdx = 0;
    E.ResultTy = ResultTy;
    return E;
  }

  static ProjectionElem createField(unsigned Idx, const FieldDecl *FD,
                                    QualType ResultTy) {
    ProjectionElem E;
    E.K = Field;
    E.FieldIndex = Idx;
    E.FD = FD;
    E.ResultTy = ResultTy;
    return E;
  }

  static ProjectionElem createIndex(LocalId IndexLocal, QualType ResultTy) {
    ProjectionElem E;
    E.K = Index;
    E.IndexLocal = IndexLocal;
    E.ResultTy = ResultTy;
    return E;
  }

  static ProjectionElem createConstantIndex(uint64_t Idx, QualType ResultTy) {
    ProjectionElem E;
    E.K = ConstantIndex;
    E.ConstIdx = Idx;
    E.ResultTy = ResultTy;
    return E;
  }

  bool operator==(const ProjectionElem &Other) const;
};

/// Format a synthetic name for an anonymous struct/union field (for IR dumps).
/// E.g., "anonymous_1_in_AnonInner".
inline std::string formatAnonymousFieldName(unsigned FieldIndex,
                                            const RecordDecl *Parent) {
  std::string ParentName = Parent ? Parent->getNameAsString() : "unknown";
  if (ParentName.empty())
    ParentName = "anonymous";
  return "anonymous_" + std::to_string(FieldIndex) + "_in_" + ParentName;
}

//===----------------------------------------------------------------------===//
// Place - A memory location: base local + projections
//===----------------------------------------------------------------------===//

struct Place {
  LocalId Base;
  ArrayRef<ProjectionElem> Projections;
  QualType Ty;
  SourceLocation Loc;

  Place() : Base{0}, Ty(), Loc() {}

  explicit Place(LocalId Base, QualType Ty, SourceLocation Loc = SourceLocation())
      : Base(Base), Ty(Ty), Loc(Loc) {}

  Place(LocalId Base, ArrayRef<ProjectionElem> Projections, QualType Ty,
        SourceLocation Loc = SourceLocation())
      : Base(Base), Projections(Projections), Ty(Ty), Loc(Loc) {}

  /// Return all prefixes of this place (e.g., for x.f1.f2: [x, x.f1, x.f1.f2])
  SmallVector<Place, 4> prefixes() const;

  /// Return supporting prefixes (stops at const deref) for borrow checking.
  SmallVector<Place, 4> supportingPrefixes() const;

  /// Create a new place by appending a projection (arena-allocated).
  /// If OverrideLoc is valid, the projected Place uses it instead of
  /// inheriting the base location.
  Place project(ProjectionElem Elem, llvm::BumpPtrAllocator &Alloc,
                SourceLocation OverrideLoc = SourceLocation()) const;

  /// Pretty-print: "x.f1.f2" or "*p"
  std::string toString() const;

  /// Whether this is a simple local (no projections).
  bool isLocal() const { return Projections.empty(); }

  bool operator==(const Place &Other) const;
};

//===----------------------------------------------------------------------===//
// Operand - A value used in an rvalue
//===----------------------------------------------------------------------===//

struct Operand {
  enum Kind : uint8_t { Copy, Move, Constant };

  Kind K;

  // --- Accessors ---
  Place &getPlace() { assert(K == Copy || K == Move); return Data.PlaceVal; }
  const Place &getPlace() const { assert(K == Copy || K == Move); return Data.PlaceVal; }
  APValue &getConstVal() { assert(K == Constant); return Data.ConstData.Val; }
  const APValue &getConstVal() const { assert(K == Constant); return Data.ConstData.Val; }
  QualType getConstTy() const { assert(K == Constant); return Data.ConstData.Ty; }
  StringRef getStringVal() const { assert(K == Constant); return Data.ConstData.Str; }
  bool hasString() const { return K == Constant && Data.ConstData.HasStr; }

  // --- Factories ---
  static Operand createCopy(Place P) {
    Operand O(PlaceInit{});
    O.K = Copy;
    O.Data.PlaceVal = std::move(P);
    return O;
  }

  static Operand createMove(Place P) {
    Operand O(PlaceInit{});
    O.K = Move;
    O.Data.PlaceVal = std::move(P);
    return O;
  }

  static Operand createConstant(APValue Val, QualType Ty) {
    Operand O; // default = Constant
    O.Data.ConstData.Val = std::move(Val);
    O.Data.ConstData.Ty = Ty;
    return O;
  }

  static Operand createStringConstant(StringRef Str, QualType Ty) {
    Operand O; // default = Constant
    O.Data.ConstData.Ty = Ty;
    O.Data.ConstData.Str = Str;
    O.Data.ConstData.HasStr = true;
    return O;
  }

  // --- Special members ---
  Operand() : K(Constant) { new (&Data.ConstData) ConstPayload(); }

  ~Operand() { destroy(); }

  Operand(const Operand &O) : K(O.K) { copyConstruct(O); }

  Operand(Operand &&O) noexcept : K(O.K) { moveConstruct(std::move(O)); }

  Operand &operator=(const Operand &O) {
    if (this != &O) {
      destroy();
      K = O.K;
      copyConstruct(O);
    }
    return *this;
  }

  Operand &operator=(Operand &&O) noexcept {
    if (this != &O) {
      destroy();
      K = O.K;
      moveConstruct(std::move(O));
    }
    return *this;
  }

private:
  struct ConstPayload {
    APValue Val;
    QualType Ty;
    StringRef Str;
    bool HasStr = false;
  };

  union PayloadUnion {
    Place PlaceVal;
    ConstPayload ConstData;
    PayloadUnion() {}
    ~PayloadUnion() {}
  } Data;

  // Tag for constructing the Place variant
  struct PlaceInit {};
  Operand(PlaceInit) : K(Copy) { new (&Data.PlaceVal) Place(); }

  void destroy() {
    switch (K) {
    case Copy:
    case Move:
      Data.PlaceVal.~Place();
      break;
    case Constant:
      Data.ConstData.~ConstPayload();
      break;
    }
  }

  void copyConstruct(const Operand &O) {
    switch (K) {
    case Copy:
    case Move:
      new (&Data.PlaceVal) Place(O.Data.PlaceVal);
      break;
    case Constant:
      new (&Data.ConstData) ConstPayload(O.Data.ConstData);
      break;
    }
  }

  void moveConstruct(Operand &&O) {
    switch (K) {
    case Copy:
    case Move:
      new (&Data.PlaceVal) Place(std::move(O.Data.PlaceVal));
      break;
    case Constant:
      new (&Data.ConstData) ConstPayload(std::move(O.Data.ConstData));
      break;
    }
  }
};

//===----------------------------------------------------------------------===//
// BorrowKind - Distinguishes &_Mut from &_Const borrows
//===----------------------------------------------------------------------===//

enum class BorrowKind : uint8_t { Mut, Shared };

//===----------------------------------------------------------------------===//
// FieldPath - Identifies a nested field within a local
//===----------------------------------------------------------------------===//

/// A path from a local to a nested struct field, e.g., _1.inner.x = {_1, [0, 0]}.
/// Used by init analysis and ownership analysis for field-level tracking.
struct FieldPath {
  LocalId Base;
  llvm::SmallVector<unsigned, 4> Indices;

  bool operator<(const FieldPath &O) const {
    if (Base.Index != O.Base.Index)
      return Base.Index < O.Base.Index;
    return Indices < O.Indices;
  }
  bool operator==(const FieldPath &O) const {
    return Base == O.Base && Indices == O.Indices;
  }
};

//===----------------------------------------------------------------------===//
// Point - Identifies a specific program point (block + statement index)
//===----------------------------------------------------------------------===//

/// Identifies a precise BSCIR program point as { block, statement_index }.
/// Note: BSCBorrowChecker.h has a separate Point for CFG-based analysis.
struct Point {
  BasicBlockId Block;
  unsigned StmtIndex;

  static Point endPoint() {
    return {BasicBlockId{UINT_MAX}, UINT_MAX};
  }
  bool isEnd() const {
    return Block.Index == UINT_MAX && StmtIndex == UINT_MAX;
  }
  bool operator==(const Point &O) const {
    return Block == O.Block && StmtIndex == O.StmtIndex;
  }
  bool operator!=(const Point &O) const { return !(*this == O); }
  bool operator<(const Point &O) const {
    if (Block < O.Block) return true;
    if (O.Block < Block) return false;
    return StmtIndex < O.StmtIndex;
  }
};

//===----------------------------------------------------------------------===//
// Rvalue - The right-hand side of an assignment
//===----------------------------------------------------------------------===//

struct Rvalue {
  enum Kind : uint8_t {
    Use,       // copy/move an operand
    Ref,       // &_Mut place or &_Const place (BSC borrow)
    AddressOf, // C-style & (raw pointer, not a borrow)
    BinaryOp,
    UnaryOp,
    Aggregate,
    Array,
    Cast,
    NullPtr,
    SizeOf
  };

  Kind K;

  // --- Payload types ---
  struct UseData { Operand Op; };
  struct RefData {
    BorrowKind BK;
    Place P;
    bool IsReborrow;
    unsigned RegionId = 0; // Unique region variable for future borrow checking
  };
  struct AddressOfData { Place P; };
  struct BinaryOpData { BinaryOperatorKind Op; Operand LHS, RHS; };
  struct UnaryOpData { UnaryOperatorKind Op; Operand Sub; };
  struct AggregateData { const RecordDecl *Decl; SmallVector<Operand, 4> Fields; };
  struct ArrayData { QualType ElementTy; SmallVector<Operand, 4> Elements; };
  struct CastData { CastKind CK; Operand Op; QualType Ty; };
  struct TypeData { QualType Ty; }; // shared by NullPtr and SizeOf

  // --- Accessors ---
  const UseData &getUse() const { assert(K == Use); return Data.use; }
  UseData &getUse() { assert(K == Use); return Data.use; }
  const RefData &getRef() const { assert(K == Ref); return Data.ref; }
  RefData &getRef() { assert(K == Ref); return Data.ref; }
  const AddressOfData &getAddrOf() const { assert(K == AddressOf); return Data.addrOf; }
  const BinaryOpData &getBinOp() const { assert(K == BinaryOp); return Data.binOp; }
  const UnaryOpData &getUnOp() const { assert(K == UnaryOp); return Data.unOp; }
  const AggregateData &getAgg() const { assert(K == Aggregate); return Data.agg; }
  const ArrayData &getArray() const { assert(K == Array); return Data.arr; }
  ArrayData &getArray() { assert(K == Array); return Data.arr; }
  const CastData &getCast() const { assert(K == Cast); return Data.cast; }
  const TypeData &getTypeData() const { assert(K == NullPtr || K == SizeOf); return Data.typeData; }

  // --- Factories ---
  static Rvalue createUse(Operand Op) {
    Rvalue R(Use);
    R.Data.use.Op = std::move(Op);
    return R;
  }
  static Rvalue createRef(BorrowKind BK, Place P, bool IsReborrow = false,
                           unsigned RegionId = 0) {
    Rvalue R(Ref);
    R.Data.ref.BK = BK;
    R.Data.ref.P = std::move(P);
    R.Data.ref.IsReborrow = IsReborrow;
    R.Data.ref.RegionId = RegionId;
    return R;
  }
  static Rvalue createAddressOf(Place P) {
    Rvalue R(AddressOf);
    R.Data.addrOf.P = std::move(P);
    return R;
  }
  static Rvalue createBinaryOp(BinaryOperatorKind Op, Operand LHS,
                                Operand RHS) {
    Rvalue R(BinaryOp);
    R.Data.binOp.Op = Op;
    R.Data.binOp.LHS = std::move(LHS);
    R.Data.binOp.RHS = std::move(RHS);
    return R;
  }
  static Rvalue createUnaryOp(UnaryOperatorKind Op, Operand Operand) {
    Rvalue R(UnaryOp);
    R.Data.unOp.Op = Op;
    R.Data.unOp.Sub = std::move(Operand);
    return R;
  }
  static Rvalue createAggregate(const RecordDecl *Decl,
                                 SmallVector<Operand, 4> Fields) {
    Rvalue R(Aggregate);
    R.Data.agg.Decl = Decl;
    R.Data.agg.Fields = std::move(Fields);
    return R;
  }
  static Rvalue createArray(QualType ElementTy,
                             SmallVector<Operand, 4> Elements) {
    Rvalue R(Array);
    R.Data.arr.ElementTy = ElementTy;
    R.Data.arr.Elements = std::move(Elements);
    return R;
  }
  static Rvalue createCast(CastKind CK, Operand Op, QualType Ty) {
    Rvalue R(Cast);
    R.Data.cast.CK = CK;
    R.Data.cast.Op = std::move(Op);
    R.Data.cast.Ty = Ty;
    return R;
  }
  static Rvalue createNullPtr(QualType Ty) {
    Rvalue R(NullPtr);
    R.Data.typeData.Ty = Ty;
    return R;
  }
  static Rvalue createSizeOf(QualType Ty) {
    Rvalue R(SizeOf);
    R.Data.typeData.Ty = Ty;
    return R;
  }

  // --- Special members ---
  Rvalue() : K(NullPtr) { new (&Data.typeData) TypeData(); }
  ~Rvalue() { destroy(); }
  Rvalue(const Rvalue &O) : K(O.K) { copyConstruct(O); }
  Rvalue(Rvalue &&O) noexcept : K(O.K) { moveConstruct(std::move(O)); }
  Rvalue &operator=(const Rvalue &O) {
    if (this != &O) { destroy(); K = O.K; copyConstruct(O); }
    return *this;
  }
  Rvalue &operator=(Rvalue &&O) noexcept {
    if (this != &O) { destroy(); K = O.K; moveConstruct(std::move(O)); }
    return *this;
  }

private:
  union PayloadUnion {
    UseData use;
    RefData ref;
    AddressOfData addrOf;
    BinaryOpData binOp;
    UnaryOpData unOp;
    AggregateData agg;
    ArrayData arr;
    CastData cast;
    TypeData typeData;
    PayloadUnion() {}
    ~PayloadUnion() {}
  } Data;

  explicit Rvalue(Kind K) : K(K) { constructDefault(); }

  void constructDefault() {
    switch (K) {
    case Use:       new (&Data.use) UseData(); break;
    case Ref:       new (&Data.ref) RefData(); break;
    case AddressOf: new (&Data.addrOf) AddressOfData(); break;
    case BinaryOp:  new (&Data.binOp) BinaryOpData(); break;
    case UnaryOp:   new (&Data.unOp) UnaryOpData(); break;
    case Aggregate: new (&Data.agg) AggregateData(); break;
    case Array:     new (&Data.arr) ArrayData(); break;
    case Cast:      new (&Data.cast) CastData(); break;
    case NullPtr:
    case SizeOf:    new (&Data.typeData) TypeData(); break;
    }
  }

  void destroy() {
    switch (K) {
    case Use:       Data.use.~UseData(); break;
    case Ref:       Data.ref.~RefData(); break;
    case AddressOf: Data.addrOf.~AddressOfData(); break;
    case BinaryOp:  Data.binOp.~BinaryOpData(); break;
    case UnaryOp:   Data.unOp.~UnaryOpData(); break;
    case Aggregate: Data.agg.~AggregateData(); break;
    case Array:     Data.arr.~ArrayData(); break;
    case Cast:      Data.cast.~CastData(); break;
    case NullPtr:
    case SizeOf:    Data.typeData.~TypeData(); break;
    }
  }

  void copyConstruct(const Rvalue &O) {
    switch (K) {
    case Use:       new (&Data.use) UseData(O.Data.use); break;
    case Ref:       new (&Data.ref) RefData(O.Data.ref); break;
    case AddressOf: new (&Data.addrOf) AddressOfData(O.Data.addrOf); break;
    case BinaryOp:  new (&Data.binOp) BinaryOpData(O.Data.binOp); break;
    case UnaryOp:   new (&Data.unOp) UnaryOpData(O.Data.unOp); break;
    case Aggregate: new (&Data.agg) AggregateData(O.Data.agg); break;
    case Array:     new (&Data.arr) ArrayData(O.Data.arr); break;
    case Cast:      new (&Data.cast) CastData(O.Data.cast); break;
    case NullPtr:
    case SizeOf:    new (&Data.typeData) TypeData(O.Data.typeData); break;
    }
  }

  void moveConstruct(Rvalue &&O) {
    switch (K) {
    case Use:       new (&Data.use) UseData(std::move(O.Data.use)); break;
    case Ref:       new (&Data.ref) RefData(std::move(O.Data.ref)); break;
    case AddressOf: new (&Data.addrOf) AddressOfData(std::move(O.Data.addrOf)); break;
    case BinaryOp:  new (&Data.binOp) BinaryOpData(std::move(O.Data.binOp)); break;
    case UnaryOp:   new (&Data.unOp) UnaryOpData(std::move(O.Data.unOp)); break;
    case Aggregate: new (&Data.agg) AggregateData(std::move(O.Data.agg)); break;
    case Array:     new (&Data.arr) ArrayData(std::move(O.Data.arr)); break;
    case Cast:      new (&Data.cast) CastData(std::move(O.Data.cast)); break;
    case NullPtr:
    case SizeOf:    new (&Data.typeData) TypeData(std::move(O.Data.typeData)); break;
    }
  }
};

//===----------------------------------------------------------------------===//
// Statement - An instruction within a basic block
//===----------------------------------------------------------------------===//

struct Statement {
  enum Kind : uint8_t { Assign, StorageLive, StorageDead, Nop };

  Kind K;

  // --- Payload types ---
  struct AssignData { Place Dest; Rvalue Src; };
  struct StorageData { LocalId Local; };

  // --- Accessors ---
  const AssignData &getAssign() const { assert(K == Assign); return Data.assign; }
  AssignData &getAssign() { assert(K == Assign); return Data.assign; }
  LocalId getStorageLocal() const { assert(K == StorageLive || K == StorageDead); return Data.storage.Local; }

  // --- Common fields ---
  bool IsSafe = false;
  const Stmt *OriginalStmt = nullptr;
  SourceLocation Loc;

  // --- Factories ---
  static Statement createAssign(Place Dest, Rvalue Src,
                                 bool IsSafe,
                                 const Stmt *Orig = nullptr,
                                 SourceLocation Loc = SourceLocation()) {
    Statement S(Assign);
    S.Data.assign.Dest = std::move(Dest);
    S.Data.assign.Src = std::move(Src);
    S.IsSafe = IsSafe;
    S.OriginalStmt = Orig;
    S.Loc = Loc;
    return S;
  }

  static Statement createStorageLive(LocalId L, bool IsSafe,
                                      SourceLocation Loc = SourceLocation()) {
    Statement S(StorageLive);
    S.Data.storage.Local = L;
    S.IsSafe = IsSafe;
    S.Loc = Loc;
    return S;
  }

  static Statement createStorageDead(LocalId L, bool IsSafe,
                                      SourceLocation Loc = SourceLocation()) {
    Statement S(StorageDead);
    S.Data.storage.Local = L;
    S.IsSafe = IsSafe;
    S.Loc = Loc;
    return S;
  }

  static Statement createNop(bool IsSafe,
                              SourceLocation Loc = SourceLocation()) {
    Statement S(Nop);
    S.IsSafe = IsSafe;
    S.Loc = Loc;
    return S;
  }

  // --- Special members ---
  Statement() : K(Nop) {}
  ~Statement() { destroy(); }
  Statement(const Statement &O) : K(O.K), IsSafe(O.IsSafe),
      OriginalStmt(O.OriginalStmt), Loc(O.Loc) { copyConstruct(O); }
  Statement(Statement &&O) noexcept : K(O.K), IsSafe(O.IsSafe),
      OriginalStmt(O.OriginalStmt), Loc(O.Loc) { moveConstruct(std::move(O)); }
  Statement &operator=(const Statement &O) {
    if (this != &O) {
      destroy(); K = O.K; IsSafe = O.IsSafe;
      OriginalStmt = O.OriginalStmt; Loc = O.Loc; copyConstruct(O);
    }
    return *this;
  }
  Statement &operator=(Statement &&O) noexcept {
    if (this != &O) {
      destroy(); K = O.K; IsSafe = O.IsSafe;
      OriginalStmt = O.OriginalStmt; Loc = O.Loc; moveConstruct(std::move(O));
    }
    return *this;
  }

private:
  union PayloadUnion {
    AssignData assign;
    StorageData storage;
    PayloadUnion() {}
    ~PayloadUnion() {}
  } Data;

  explicit Statement(Kind K) : K(K) { constructDefault(); }

  void constructDefault() {
    switch (K) {
    case Assign:      new (&Data.assign) AssignData(); break;
    case StorageLive:
    case StorageDead: new (&Data.storage) StorageData(); break;
    case Nop:         break;
    }
  }

  void destroy() {
    switch (K) {
    case Assign:      Data.assign.~AssignData(); break;
    case StorageLive:
    case StorageDead: Data.storage.~StorageData(); break;
    case Nop:         break;
    }
  }

  void copyConstruct(const Statement &O) {
    switch (K) {
    case Assign:      new (&Data.assign) AssignData(O.Data.assign); break;
    case StorageLive:
    case StorageDead: new (&Data.storage) StorageData(O.Data.storage); break;
    case Nop:         break;
    }
  }

  void moveConstruct(Statement &&O) {
    switch (K) {
    case Assign:      new (&Data.assign) AssignData(std::move(O.Data.assign)); break;
    case StorageLive:
    case StorageDead: new (&Data.storage) StorageData(std::move(O.Data.storage)); break;
    case Nop:         break;
    }
  }
};

//===----------------------------------------------------------------------===//
// Terminator - Control flow at the end of a basic block
//===----------------------------------------------------------------------===//

struct Terminator {
  enum Kind : uint8_t {
    Goto,
    SwitchInt,
    Call,
    Drop,
    Return,
    Unreachable
  };

  Kind K;

  // --- Payload types ---
  struct GotoData { BasicBlockId Target; };
  struct SwitchIntData {
    Operand Discriminant;
    SmallVector<std::pair<llvm::APInt, BasicBlockId>, 4> Targets;
    BasicBlockId Otherwise;
  };
  struct CallData {
    Operand Callee;
    SmallVector<Operand, 4> Args;
    Place Dest;
    BasicBlockId Successor;
    bool Diverges;
    const FunctionDecl *Decl;
    SmallVector<llvm::Optional<Place>, 4> ArgPlaces;
    const FunctionProtoType *CalleeProtoType = nullptr;
    std::string CalleeName; // For builtins without a FunctionDecl (e.g. AtomicExpr)
  };
  struct DropData { Place Dropped; BasicBlockId Successor; };

  // --- Accessors ---
  const GotoData &getGoto() const { assert(K == Goto); return Data.go; }
  GotoData &getGoto() { assert(K == Goto); return Data.go; }
  const SwitchIntData &getSwitchInt() const { assert(K == SwitchInt); return Data.sw; }
  SwitchIntData &getSwitchInt() { assert(K == SwitchInt); return Data.sw; }
  const CallData &getCall() const { assert(K == Call); return Data.call; }
  CallData &getCall() { assert(K == Call); return Data.call; }
  const DropData &getDrop() const { assert(K == Drop); return Data.drop; }
  DropData &getDrop() { assert(K == Drop); return Data.drop; }

  // --- Common fields ---
  bool IsSafe = false;
  const Stmt *OriginalStmt = nullptr;
  SourceLocation Loc;

  // --- Factories ---
  static Terminator createGoto(BasicBlockId Target,
                                bool IsSafe) {
    Terminator T(Goto);
    T.Data.go.Target = Target;
    T.IsSafe = IsSafe;
    return T;
  }

  static Terminator createSwitchInt(
      Operand Discriminant,
      SmallVector<std::pair<llvm::APInt, BasicBlockId>, 4> Targets,
      BasicBlockId Otherwise, bool IsSafe) {
    Terminator T(SwitchInt);
    T.Data.sw.Discriminant = std::move(Discriminant);
    T.Data.sw.Targets = std::move(Targets);
    T.Data.sw.Otherwise = Otherwise;
    T.IsSafe = IsSafe;
    return T;
  }

  static Terminator createCall(Operand Callee, SmallVector<Operand, 4> Args,
                                Place Dest, BasicBlockId Successor,
                                const FunctionDecl *FD,
                                bool IsSafe,
                                const Stmt *Orig = nullptr,
                                SourceLocation Loc = SourceLocation(),
                                bool Diverges = false,
                                SmallVector<llvm::Optional<Place>, 4> ArgPlaces = {},
                                const FunctionProtoType *ProtoType = nullptr) {
    Terminator T(Call);
    T.Data.call.Callee = std::move(Callee);
    T.Data.call.Args = std::move(Args);
    T.Data.call.Dest = std::move(Dest);
    T.Data.call.Successor = Successor;
    T.Data.call.Diverges = Diverges;
    T.Data.call.Decl = FD;
    T.Data.call.ArgPlaces = std::move(ArgPlaces);
    T.Data.call.CalleeProtoType = ProtoType;
    T.IsSafe = IsSafe;
    T.OriginalStmt = Orig;
    T.Loc = Loc;
    return T;
  }

  static Terminator createDrop(Place Dropped, BasicBlockId Successor,
                                bool IsSafe) {
    Terminator T(Drop);
    T.Data.drop.Dropped = std::move(Dropped);
    T.Data.drop.Successor = Successor;
    T.IsSafe = IsSafe;
    return T;
  }

  static Terminator createReturn(bool IsSafe) {
    Terminator T(Return);
    T.IsSafe = IsSafe;
    return T;
  }

  static Terminator createUnreachable(bool IsSafe) {
    Terminator T(Unreachable);
    T.IsSafe = IsSafe;
    return T;
  }

  /// Get all successor block IDs for this terminator.
  SmallVector<BasicBlockId, 4> successors() const;

  /// Invoke Fn(BasicBlockId) for each successor. Avoids allocation.
  template <typename Fn> void forEachSuccessor(Fn &&F) const {
    switch (K) {
    case Goto:
      F(Data.go.Target);
      break;
    case SwitchInt:
      for (const auto &Target : Data.sw.Targets)
        F(Target.second);
      F(Data.sw.Otherwise);
      break;
    case Call:
      if (!Data.call.Diverges)
        F(Data.call.Successor);
      break;
    case Drop:
      F(Data.drop.Successor);
      break;
    case Return:
    case Unreachable:
      break;
    }
  }

  // --- Special members ---
  Terminator() : K(Unreachable) {}
  ~Terminator() { destroy(); }
  Terminator(const Terminator &O) : K(O.K), IsSafe(O.IsSafe),
      OriginalStmt(O.OriginalStmt), Loc(O.Loc) { copyConstruct(O); }
  Terminator(Terminator &&O) noexcept : K(O.K), IsSafe(O.IsSafe),
      OriginalStmt(O.OriginalStmt), Loc(O.Loc) { moveConstruct(std::move(O)); }
  Terminator &operator=(const Terminator &O) {
    if (this != &O) {
      destroy(); K = O.K; IsSafe = O.IsSafe;
      OriginalStmt = O.OriginalStmt; Loc = O.Loc; copyConstruct(O);
    }
    return *this;
  }
  Terminator &operator=(Terminator &&O) noexcept {
    if (this != &O) {
      destroy(); K = O.K; IsSafe = O.IsSafe;
      OriginalStmt = O.OriginalStmt; Loc = O.Loc; moveConstruct(std::move(O));
    }
    return *this;
  }

private:
  union PayloadUnion {
    GotoData go;
    SwitchIntData sw;
    CallData call;
    DropData drop;
    PayloadUnion() {}
    ~PayloadUnion() {}
  } Data;

  explicit Terminator(Kind K) : K(K) { constructDefault(); }

  void constructDefault() {
    switch (K) {
    case Goto:        new (&Data.go) GotoData(); break;
    case SwitchInt:   new (&Data.sw) SwitchIntData(); break;
    case Call:        new (&Data.call) CallData(); break;
    case Drop:        new (&Data.drop) DropData(); break;
    case Return:
    case Unreachable: break;
    }
  }

  void destroy() {
    switch (K) {
    case Goto:        Data.go.~GotoData(); break;
    case SwitchInt:   Data.sw.~SwitchIntData(); break;
    case Call:        Data.call.~CallData(); break;
    case Drop:        Data.drop.~DropData(); break;
    case Return:
    case Unreachable: break;
    }
  }

  void copyConstruct(const Terminator &O) {
    switch (K) {
    case Goto:        new (&Data.go) GotoData(O.Data.go); break;
    case SwitchInt:   new (&Data.sw) SwitchIntData(O.Data.sw); break;
    case Call:        new (&Data.call) CallData(O.Data.call); break;
    case Drop:        new (&Data.drop) DropData(O.Data.drop); break;
    case Return:
    case Unreachable: break;
    }
  }

  void moveConstruct(Terminator &&O) {
    switch (K) {
    case Goto:        new (&Data.go) GotoData(std::move(O.Data.go)); break;
    case SwitchInt:   new (&Data.sw) SwitchIntData(std::move(O.Data.sw)); break;
    case Call:        new (&Data.call) CallData(std::move(O.Data.call)); break;
    case Drop:        new (&Data.drop) DropData(std::move(O.Data.drop)); break;
    case Return:
    case Unreachable: break;
    }
  }
};

//===----------------------------------------------------------------------===//
// BasicBlock - A sequence of statements ending with a terminator
//===----------------------------------------------------------------------===//

struct BasicBlock {
  BasicBlockId Id;
  SmallVector<Statement, 8> Statements;
  Terminator Term;

  BasicBlock() : Id{0} {}
  explicit BasicBlock(BasicBlockId Id) : Id(Id) {}
};

//===----------------------------------------------------------------------===//
// Body - The complete BSCIR for a function
//===----------------------------------------------------------------------===//

struct Body {
  const FunctionDecl *SourceFD = nullptr;
  SmallVector<LocalDecl, 16> Locals;
  SmallVector<BasicBlock, 32> Blocks;
  unsigned NumParams = 0;

  /// Arena allocator for Place projection arrays.
  llvm::BumpPtrAllocator Alloc;

  /// Get the arena allocator for Place projections.
  llvm::BumpPtrAllocator &getAllocator() { return Alloc; }

  // Pre-computed predecessor map
  llvm::DenseMap<BasicBlockId, SmallVector<BasicBlockId, 4>> Predecessors;

  bool IsSafe = false;

  /// Total number of region variables allocated by the builder.
  /// Prepared for future borrow checker integration (not consumed yet).
  unsigned NumRegions = 0;

  /// Add a new local and return its ID. Index 0 = return slot.
  LocalId addLocal(QualType Ty, const VarDecl *VD, StringRef Name,
                   bool IsTemp, SourceLocation Loc);

  /// Add a new temporary local.
  LocalId addTemp(QualType Ty, SourceLocation Loc = SourceLocation());

  /// Add a new basic block and return its ID.
  BasicBlockId addBlock();

  /// Get a block by ID.
  BasicBlock &getBlock(BasicBlockId Id) { return Blocks[Id.Index]; }
  const BasicBlock &getBlock(BasicBlockId Id) const {
    return Blocks[Id.Index];
  }

  /// Get a local declaration by ID.
  const LocalDecl &getLocal(LocalId Id) const { return Locals[Id.Index]; }

  /// Get the entry block.
  BasicBlock &entryBlock() { return Blocks[0]; }
  const BasicBlock &entryBlock() const { return Blocks[0]; }

  /// Simplify the CFG: collapse goto chains, merge blocks, remove dead blocks.
  void simplify();

  /// Compute the predecessor map from successor edges.
  void computePredecessors();

  /// Get predecessors for a given block.
  ArrayRef<BasicBlockId> getPredecessors(BasicBlockId Id) const;
};

} // namespace bscir
} // namespace clang

#endif // ENABLE_BSC
#endif // LLVM_CLANG_ANALYSIS_ANALYSES_BSC_BSCIR_H
