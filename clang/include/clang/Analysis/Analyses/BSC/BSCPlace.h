//===- BSCPlace.h - Shared place infrastructure for BSC analyses -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared AST-level place / type infrastructure for BSC flow-sensitive analyses
// (nullability, ownership, borrow). A Place names a nullable / borrowable memory
// cell reachable from one local variable through member access, dereference,
// and indexing, normalized to a single spelling so equivalent expressions share
// one key.
//
// This is a simplified, Environment-free cousin of `borrow::Place`: it is keyed
// by the AST root (VarDecl) rather than by regions, so the borrow analysis can
// later build its region-parameterized `Place`/`Ty` on top of it.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_BSC_PLACE_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_BSC_PLACE_H

#if ENABLE_BSC

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace clang {

class ASTContext;
class Expr;
class FieldDecl;

namespace bsc {

/// AST-level type node. Simplified from `borrow::Ty`: it keeps the same
/// Base/Pointer/Struct/Array shape but carries no region parameters, so it can
/// be built from a QualType alone without an Environment.
class Ty {
public:
  enum class TyKind : unsigned char { Base, Pointer, Struct, Array };

private:
  TyKind Kind;
  QualType QT;

  /// Valid only for pointer nodes. Arena-owned, never freed individually.
  const Ty *Pointee = nullptr;

  /// Valid only for array nodes. Arena-owned, see `Pointee`.
  const Ty *Element = nullptr;

  Ty(TyKind Kind, QualType QT) : Kind(Kind), QT(QT) {}

  static Ty *CreateBase(const ASTContext &Ctx, QualType QT) {
    return new (Ctx) Ty(TyKind::Base, QT);
  }

  static Ty *CreateStruct(const ASTContext &Ctx, QualType QT) {
    return new (Ctx) Ty(TyKind::Struct, QT);
  }

public:
  /// Build the `Ty` tree for `QT` (Pointers recurse into pointees, arrays into
  /// elements, records into struct nodes, everything else into base nodes).
  static Ty *Create(const ASTContext &Ctx, QualType QT);

  static Ty *CreatePointer(const ASTContext &Ctx, QualType QT,
                           const Ty *PointeeTy) {
    Ty *T = new (Ctx) Ty(TyKind::Pointer, QT);
    T->Pointee = PointeeTy;
    return T;
  }

  static Ty *CreateArray(const ASTContext &Ctx, QualType QT,
                         const Ty *ElementTy) {
    Ty *T = new (Ctx) Ty(TyKind::Array, QT);
    T->Element = ElementTy;
    return T;
  }

  TyKind getKind() const { return Kind; }
  QualType getQualType() const { return QT; }

  bool isPointerType() const { return Kind == TyKind::Pointer; }
  bool isArrayType() const { return Kind == TyKind::Array; }
  bool isRecordType() const { return Kind == TyKind::Struct; }

  const Ty *getPointee() const {
    assert(Kind == TyKind::Pointer && "only pointer type has a pointee");
    return Pointee;
  }

  const Ty *getElement() const {
    assert(Kind == TyKind::Array && "only array type has an element");
    return Element;
  }
};

/// A memory place: a root variable followed by a chain of projections.
///
/// A place is an immutable, arena-allocated singly-linked list. The root is
/// always a `VarDecl`, except when the base of a member expression is
/// untrackable (e.g. an array element `a[i].f`); such a place is rooted at
/// null, matching the historical behavior of sharing one cell per field name
/// across all untrackable bases.
///
/// Examples:
/// - `p`     is `Var(p)`.
/// - `s.f`   is `Field(Var(s), "f")`.
/// - `*p`    is `Deref(Var(p))`; `**p` is `Deref(Deref(Var(p)))`.
/// - `p->f` and `(*p).f` are both `Field(Deref(Var(p)), "f")`.
///
/// Identity deliberately does not include the source location or the type.
class Place {
public:
  enum class Kind : unsigned char { Var, Field, Deref, Index };

  friend class PlaceBuilder;

private:
  Kind K;
  const Place *Base = nullptr;
  llvm::StringRef Name;      // var/field name, or index discriminator "[i]".
  const Ty *T = nullptr;     // type of the cell named by this place.
  const VarDecl *Root;       // cached root (null for null-root places).
  SourceLocation Loc;
  uint64_t Hash;

  // Sentinel mixed into the hash when `Base` is null, so a null root never
  // collides with a real base whose hash happens to match.
  static constexpr uint64_t NullBaseSeed = 0x9e3779b97f4a7c15ull;

  uint64_t computeHash(const Place *Base, llvm::StringRef Name,
                       const VarDecl *Root) const {
    switch (K) {
    case Kind::Var:
      return llvm::hash_combine((unsigned)K, (const void *)Root);
    case Kind::Field:
    case Kind::Index:
      return llvm::hash_combine((unsigned)K, Name,
                                Base ? Base->Hash : NullBaseSeed);
    case Kind::Deref:
      return llvm::hash_combine((unsigned)K, Base ? Base->Hash : NullBaseSeed);
    }
    llvm_unreachable("unknown place kind");
  }

  Place(Kind K, const Place *Base, llvm::StringRef Name, const Ty *T,
        const VarDecl *Root, SourceLocation Loc)
      : K(K), Base(Base), Name(Name), T(T), Root(Root), Loc(Loc),
        Hash(computeHash(Base, Name, Root)) {}

  static const Place *CreateVar(const ASTContext &Ctx, const VarDecl *VD,
                                const Ty *T, SourceLocation Loc) {
    return new (Ctx) Place(Kind::Var, nullptr, VD->getName(), T, VD, Loc);
  }

  static const Place *CreateField(const ASTContext &Ctx, const Place *Base,
                                  const FieldDecl *FD, const Ty *T,
                                  SourceLocation Loc) {
    return new (Ctx)
        Place(Kind::Field, Base, FD->getName(), T, Base ? Base->Root : nullptr,
              Loc);
  }

  static const Place *CreateDeref(const ASTContext &Ctx, const Place *Base,
                                  const Ty *T, SourceLocation Loc) {
    return new (Ctx) Place(Kind::Deref, Base, llvm::StringRef(), T,
                           Base ? Base->Root : nullptr, Loc);
  }

  static const Place *CreateIndex(const ASTContext &Ctx, const Place *Base,
                                  const Ty *T, SourceLocation Loc,
                                  llvm::StringRef Discr) {
    // Discr is usually a synthetic string ("[0]", "[2:5]"); copy it into the
    // arena so the name outlives the temporary it was built from.
    Discr = Discr.copy(Ctx.getAllocator());
    return new (Ctx) Place(Kind::Index, Base, Discr, T,
                           Base ? Base->Root : nullptr, Loc);
  }

  bool baseEquals(const Place *Other) const {
    return Base == Other->Base ||
           (Base && Other->Base && Base->equals(Other->Base));
  }

public:
  Kind getKind() const { return K; }
  const Place *getBase() const { return Base; }
  llvm::StringRef getName() const { return Name; }
  const Ty *getType() const { return T; }
  const VarDecl *getRoot() const { return Root; }
  SourceLocation getLocation() const { return Loc; }

  /// Structural hash of the place chain (kind, name, and base recursively).
  uint64_t hash() const { return Hash; }

  /// Structural equality: kind, name, and base chain must all match. Field
  /// identity is by name only (not FieldDecl) so same-named fields of
  /// untrackable bases share one cell, as the string-path state did before.
  bool equals(const Place *Other) const {
    if (this == Other)
      return true;
    if (!Other || K != Other->K)
      return false;
    switch (K) {
    case Kind::Var:
      return Root == Other->Root;
    case Kind::Field:
    case Kind::Index:
      return Name == Other->Name && baseEquals(Other);
    case Kind::Deref:
      return baseEquals(Other);
    }
    llvm_unreachable("unknown place kind");
  }

  /// Whether \p Prefix is an ancestor of (or equal to) this place.
  bool hasPrefix(const Place *Prefix) const {
    for (const Place *Cur = this; Cur; Cur = Cur->Base)
      if (Cur->equals(Prefix))
        return true;
    return false;
  }

  /// Whether this place is a (non-empty) chain of dereferences from its root
  /// variable: `*p`, `**p`, ... Excludes the variable itself and any place
  /// containing a field or index projection.
  bool isDerefChainFromRoot() const {
    if (K == Kind::Var)
      return false;
    const Place *Cur = this;
    while (Cur && Cur->K == Kind::Deref)
      Cur = Cur->Base;
    return Cur && Cur->K == Kind::Var;
  }

  std::string toString() const {
    std::string Result;
    llvm::raw_string_ostream OS(Result);
    print(OS);
    return Result;
  }

private:
  void print(llvm::raw_ostream &OS) const;
};

/// Builds a `Place` from an expression, without needing an Environment.
class PlaceBuilder {
  const ASTContext &Ctx;

public:
  PlaceBuilder(const ASTContext &Ctx) : Ctx(Ctx) {}

  /// Build the place denoting \p E. Returns null when \p E does not name a
  /// trackable cell (constant, call, etc.). Member dereference and field
  /// access normalize to the same spellings as the historical string paths.
  const Place *Build(const Expr *E);

  /// Build the place rooted at a variable declaration.
  const Place *Build(const VarDecl *VD, SourceLocation Loc);

  /// Append a field projection \p FD to \p Base, with the field's type.
  const Place *BuildField(const Place *Base, const FieldDecl *FD,
                          SourceLocation Loc);

  /// Append a dereference projection typed \p PointeeTy to \p Base.
  const Place *BuildDeref(const Place *Base, QualType PointeeTy,
                          SourceLocation Loc);

  /// Append an index projection typed \p ElemTy to \p Base. \p Discr is a
  /// synthetic discriminator ("[0]", "[2:5]"); it is copied into the arena.
  const Place *BuildIndex(const Place *Base, QualType ElemTy,
                          llvm::StringRef Discr, SourceLocation Loc);
};

} // namespace bsc
} // namespace clang

namespace llvm {
template <> struct DenseMapInfo<const clang::bsc::Place *> {
  using Ptr = const clang::bsc::Place *;

  static Ptr getEmptyKey() { return reinterpret_cast<Ptr>(-1); }
  static Ptr getTombstoneKey() { return reinterpret_cast<Ptr>(-2); }

  static unsigned getHashValue(Ptr P) {
    return P == getEmptyKey() || P == getTombstoneKey()
               ? 0
               : (unsigned)P->hash();
  }

  static bool isEqual(Ptr A, Ptr B) {
    if (A == getEmptyKey() || A == getTombstoneKey())
      return A == B;
    if (B == getEmptyKey() || B == getTombstoneKey())
      return false;
    return A->equals(B);
  }
};
} // namespace llvm

#endif // ENABLE_BSC
#endif // LLVM_CLANG_ANALYSIS_ANALYSES_BSC_PLACE_H