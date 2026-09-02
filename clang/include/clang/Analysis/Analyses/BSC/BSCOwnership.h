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
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include <string>

namespace clang {

// an array type is an "owned element array" if its (possibly
// multi-dimensional) element type is an _Owned pointer, an _Owned struct, or a
// move-semantic record (a struct that contains _Owned members). These arrays
// are tracked as whole-array aggregates by ownership analysis, and element
// ownership transfer is only allowed inside qualifying for-loops.
// Do not canonicalize before isOwnedQualified(): Owned may only appear as a
// local qualifier on the sugar type, and getCanonicalType() can drop it for
// the QualType we then inspect with isOwnedQualified().
static inline bool IsOwnedElementArrayType(QualType type) {
  while (const auto *AT = type->getAsArrayTypeUnsafe()) {
    QualType Elem = AT->getElementType();
    if (Elem->isPointerType() && Elem.isOwnedQualified())
      return true;
    if (Elem->isRecordType() &&
        (Elem.getTypePtr()->isOwnedStructureType() ||
         Elem->isMoveSemanticType()))
      return true;
    type = Elem;
  }
  return false;
}

// T *_Owned _ArrayElem where transferring an element would move ownership of
// an owned pointer / move-semantic value / owned fields of the pointee. Such
// pointers never carry length information, so element transfer is forbidden.
static inline bool IsOwnedArrayElemPtrTransferBase(QualType type) {
  if (!type->isPointerType() || !type.isOwnedQualified() ||
      !type.isArrayElemQualified())
    return false;
  QualType Pointee = type->getPointeeType();
  if (Pointee.isOwnedQualified())
    return true;
  if (Pointee->hasOwnedFields())
    return true;
  if (Pointee->isRecordType() &&
      (Pointee.getTypePtr()->isOwnedStructureType() ||
       Pointee->isMoveSemanticType()))
    return true;
  return false;
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
  if (type->isPointerType() && type.isOwnedQualified())
    return true;

  // case 3
  if ((type->isRecordType() && type.getTypePtr()->isOwnedStructureType()) ||
      (type->isRecordType() && type->isMoveSemanticType()))
    return true;

  // case 5: array of tracked elements
  if (const auto *AT = type->getAsArrayTypeUnsafe())
    return IsTrackedType(AT->getElementType());

  return false;
}

// Is `HostVD.FieldPath` an owned-element array field (its element type is an
// _Owned pointer or move-semantic record)? Used to track `w.arr[i]` where arr
// is `int *_Owned arr[2]` inside `struct W`, and nested forms like
// `o.w.arr[i]` (path "w.arr") where the intermediate members are records. The
// array aggregate is tracked as a field of the root host variable.
static inline bool IsOwnedArrayField(const VarDecl *HostVD,
                                     const std::string &FieldPath) {
  // Strip the host array level(s): the host-array index is not part of the
  // path (e.g. `struct S arr[3]` -> element `struct S`). Also strip a
  // leading pointer so a `struct S *_Owned` host works: `p->arr[i]` tracks
  // the array field of the pointee like a local struct-array host.
  QualType HostTy = HostVD->getType();
  while (const auto *AT = HostTy->getAsArrayTypeUnsafe())
    HostTy = AT->getElementType();
  if (HostTy->isPointerType())
    HostTy = HostTy->getPointeeType();
  const RecordDecl *RD =
      HostTy.getCanonicalType()->getAs<RecordType>()
          ? HostTy.getCanonicalType()->getAs<RecordType>()->getDecl()
          : nullptr;
  if (!RD)
    return false;
  size_t Pos = 0;
  while (Pos < FieldPath.size()) {
    // Skip array-field levels (`[]`) and member separators (`.`): they do
    // not name a member.
    while (Pos < FieldPath.size() &&
           (FieldPath.compare(Pos, 2, "[]") == 0 || FieldPath[Pos] == '.'))
      Pos += FieldPath.compare(Pos, 2, "[]") == 0 ? 2 : 1;
    if (Pos >= FieldPath.size())
      break;
    // A segment ends at `.` (record member separator) or `[]` (array level).
    size_t Dot = FieldPath.find('.', Pos);
    size_t Bracket = FieldPath.find("[]", Pos);
    size_t End;
    if (Dot == std::string::npos)
      End = Bracket;
    else if (Bracket == std::string::npos)
      End = Dot;
    else
      End = std::min(Dot, Bracket);
    std::string Member = FieldPath.substr(
        Pos, End == std::string::npos ? std::string::npos : End - Pos);
    const FieldDecl *FD = nullptr;
    for (const FieldDecl *F : RD->fields())
      if (F->getNameAsString() == Member) {
        FD = F;
        break;
      }
    if (!FD)
      return false;
    // If the segment is the last one (nothing but `[]` array levels and `.`
    // separators after it), the field itself must be an owned-element array.
    if (End == std::string::npos)
      return IsOwnedElementArrayType(FD->getType());
    if (FieldPath.compare(End, 2, "[]") == 0) {
      bool MoreAfter = false;
      for (size_t I = End + 2; I < FieldPath.size(); ++I)
        if (FieldPath[I] == '.') {
          MoreAfter = true;
          break;
        }
      if (!MoreAfter)
        return IsOwnedElementArrayType(FD->getType());
    }
    // Intermediate segment: a member that is itself an owned-element array
    // field (e.g. arr in `m[a].arr[0].p` where arr is `struct Inner arr[2]`
    // with owned members) makes the path a field-array element transfer even
    // though the final member (p) is not an array -- recognise it so the
    // transfer stays gated by the qualifying-loop rules.
    if (IsOwnedElementArrayType(FD->getType()))
      return true;
    // Otherwise descend into the record (stripping field-array levels of
    // the member itself).
    QualType FT = FD->getType();
    while (const auto *AT = FT->getAsArrayTypeUnsafe())
      FT = AT->getElementType();
    const RecordType *RT = FT.getCanonicalType()->getAs<RecordType>();
    if (!RT)
      return false;
    RD = RT->getDecl();
    Pos = End;  // the next iteration skips `[]` / `.`
  }
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

// Peel `w.arr[0].a` / `s[i].a` / `s[i].a[j]` to the host variable and the
// nested field path ("arr[].a" / "a" / "a[]"). The host is the root
// DeclRefExpr; array levels of *fields* are rendered as `[]` (indexes of the
// host array itself, e.g. s[i], are not part of the path). Shared by the CFG
// dataflow (BSCOwnership.cpp) and the qualifying-loop classifier
// (BSCOwnershipArrayLoopClassification.cpp).
static inline bool PeelHostAndFieldPath(const Expr *E, const VarDecl *&HostVD,
                                        std::string &FieldPath) {
  llvm::SmallVector<std::string, 4> chain; // E -> host
  const Expr *Cur = E ? E->IgnoreParenImpCastsSafe() : nullptr;
  while (Cur) {
    if (const MemberExpr *ME = dyn_cast<MemberExpr>(Cur)) {
      chain.push_back(ME->getMemberNameInfo().getAsString());
      Cur = ME->getBase()->IgnoreParenImpCastsSafe();
    } else if (const ArraySubscriptExpr *ASE =
                   dyn_cast<ArraySubscriptExpr>(Cur)) {
      const Expr *B = ASE->getBase()->IgnoreParenImpCastsSafe();
      // MemberExpr base -> array-level field index. A nested ArraySubscriptExpr
      // base may be a field-array index (w.arr[i][j]) or a host pointer index
      // (p2[1][2] where p2 is `int **_Borrow`); only the former belongs in
      // the path. Decide by looking at the ultimate base of the nested chain.
      bool IsFieldIndex = dyn_cast<MemberExpr>(B) != nullptr;
      if (const ArraySubscriptExpr *Nested =
              dyn_cast<ArraySubscriptExpr>(B)) {
        const Expr *BB = Nested->getBase()->IgnoreParenImpCastsSafe();
        while (const ArraySubscriptExpr *BA =
                   dyn_cast<ArraySubscriptExpr>(BB))
          BB = BA->getBase()->IgnoreParenImpCastsSafe();
        IsFieldIndex = dyn_cast<MemberExpr>(BB) != nullptr;
      }
      if (IsFieldIndex) {
        chain.push_back("[]"); // array-level field index
      }
      Cur = B;
    } else {
      break;
    }
  }
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(Cur))
    HostVD = dyn_cast<VarDecl>(DRE->getDecl());
  else
    return false;
  if (!HostVD)
    return false;
  std::string path;
  for (auto It = chain.rbegin(); It != chain.rend(); ++It) {
    if (*It == "[]")
      path += "[]";
    else if (path.empty())
      path = *It;
    else
      path += "." + *It;
  }
  FieldPath = path;
  return true;
}

// Whether FieldPath (from PeelHostAndFieldPath) denotes an owned-element
// array field of HostVD: it must contain an array level (`[]`) and the
// resolved field must be an owned-element array. Single predicate shared by
// the dataflow transfer sites (subscript / member / cast / null-check), so
// the "path has `[]`" and "field is owned array" checks are not duplicated.
static inline bool IsOwnedArrayFieldPath(const VarDecl *HostVD,
                                         const std::string &FieldPath) {
  return !FieldPath.empty() &&
         FieldPath.find("[]") != std::string::npos &&
         IsOwnedArrayField(HostVD, FieldPath);
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
  ElemAccessMismatch,  // array access does not use the loop index (a[i])
  VlaBoundVarMismatch, // VLA length variable is not the loop bound variable
  VlaBoundVarModified, // VLA length variable address-taken / borrowed / modified
  PathInconsistent,    // if/else branches transfer differently
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
  enum Status {
    Uninitialized = 0x1,
    Null = 0x2,
    Owned = 0x4,
    Moved = 0x8,
    PartialMoved = 0x10,
    AllMoved = 0x20,
  };

  class OwnershipStatus {
  public:
    enum Source { 
      OPS, // Owned Pointer to Struct, e.g. struct S* owned s
      S,   // Struct, e.g. struct S s
      BOP  // Owned Pointer to Basic Types, e.g. int* owned p
    };
    using OwnershipSet = llvm::BitVector;

    // owned pointer struct status, e.g. struct S * owned s
    using OPSOwnedField = llvm::SmallSet<std::string, 10>;
    llvm::DenseMap<const VarDecl *, OwnershipSet> OPSStatus;
    llvm::DenseMap<const VarDecl *, OPSOwnedField> OPSAllOwnedFields;
    llvm::DenseMap<const VarDecl *, OPSOwnedField> OPSOwnedOwnedFields;
    llvm::DenseMap<const VarDecl *, OPSOwnedField> OPSNullOwnedFields;

    // struct status, e.g. struct S s
    using SOwnedField = llvm::SmallSet<std::string, 10>;
    llvm::DenseMap<const VarDecl *, OwnershipSet> SStatus;
    llvm::DenseMap<const VarDecl *, SOwnedField> SAllOwnedFields;
    llvm::DenseMap<const VarDecl *, SOwnedField> SOwnedOwnedFields;
    llvm::DenseMap<const VarDecl *, SOwnedField> SNullOwnedFields;
    // Owned field paths never yet assigned; distinguishes uninitialized from
    // moved-out at field granularity.
    llvm::DenseMap<const VarDecl *, SOwnedField> SUninitOwnedFields;

    // basic owned pointer status, e.g. int * owned p
    using BOPOwnedField = llvm::SmallSet<std::string, 10>;
    llvm::DenseMap<const VarDecl *, OwnershipSet> BOPStatus;
    llvm::DenseMap<const VarDecl *, BOPOwnedField> BOPAllOwnedFields;
    llvm::DenseMap<const VarDecl *, BOPOwnedField> BOPOwnedOwnedFields;

    bool equals(const OwnershipStatus &V) const;
    bool empty() const;
    bool is(const VarDecl *VD, Status S) const;
    bool has(const VarDecl *VD, Status S) const;
    bool canAssign(const VarDecl *VD) const;
    void set(const VarDecl *VD, Status S);
    void reset(const VarDecl *VD, Status S);
    void resetAll(const VarDecl *VD);
    void init(const VarDecl *VD);
    void setToOwned(const VarDecl *VD);
    void setToAllMoved(const VarDecl *VD);
    void setToAllMoved(const Expr *E);
    void setToNull(const VarDecl *VD);
    void setToNull(const Expr *E);
    void setOwnedFieldNull(const VarDecl *VD, const std::string &fieldPath);
    void setToMoved(const VarDecl *VD);
    void setToMoved(const Expr *E);

    // whole-array / struct-field ownership transitions applied
    // by the dataflow when a qualifying for-loop transfers array elements.
    // The whole array is one aggregate; an element transfer applies to all
    // elements uniformly, so the aggregate state moves as a unit.
    void setArrayElemMoved(const VarDecl *VD);
    void setArrayElemOwned(const VarDecl *VD);
    void setArrayElemNull(const VarDecl *VD);
  void setArrayFieldMoved(const VarDecl *VD, const std::string &fieldName);
  void setArrayFieldOwned(const VarDecl *VD, const std::string &fieldName);
  void setArrayFieldNull(const VarDecl *VD, const std::string &fieldName);
    // Shared by the three setArrayField*: after erasing / inserting fieldName
    // (and its `fieldName.` prefix children) in the per-VD owned set, refresh
    // the aggregate bit state from the owned count vs the static field list.
    void refreshArrayFieldState(const VarDecl *VD,
                                const llvm::SmallSet<std::string, 10> &Owned,
                                const llvm::SmallSet<std::string, 10> &All);
    // Whether the aggregate owns nothing (every tracked field moved out) /
    // is (possibly) uninitialized / owns nothing or the field is null.
    // Shared by the move-out checks (BOP uses bits; S/OPS use field sets).
    bool arrayAggregateMoved(const VarDecl *VD) const;
    bool arrayFieldOwned(const VarDecl *VD,
                         const std::string &fieldName) const;
    bool arrayAggregateUninit(const VarDecl *VD) const;
    bool arrayAggregateNull(const VarDecl *VD,
                            const std::string &fieldName) const;

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
                     std::string fullFieldName, bool isGetAddr);
    llvm::SmallVector<OwnershipDiagInfo>
    checkOPSAssign(const VarDecl *VD, const SourceLocation &Loc);
    llvm::SmallVector<OwnershipDiagInfo>
    checkOPSDerefAssign(const VarDecl *VD, const SourceLocation &Loc);
    llvm::SmallVector<OwnershipDiagInfo>
    checkOPSFieldAssign(const VarDecl *VD, const SourceLocation &Loc,
                        std::string fullFieldName);

    llvm::SmallVector<OwnershipDiagInfo>
    checkSUse(const VarDecl *VD, const SourceLocation &Loc, bool isGetAddr, bool isAddrMut);
    llvm::SmallVector<OwnershipDiagInfo>
    checkSFieldUse(const VarDecl *VD, const SourceLocation &Loc,
                   std::string fullFieldName, bool isGetAddr);
    llvm::SmallVector<OwnershipDiagInfo>
    checkSAssign(const VarDecl *VD, const SourceLocation &Loc);
    llvm::SmallVector<OwnershipDiagInfo>
    checkSFieldAssign(const VarDecl *VD, const SourceLocation &Loc,
                      std::string fullFieldName);

    llvm::SmallVector<OwnershipDiagInfo>
    checkBOPUse(const VarDecl *VD, const SourceLocation &Loc, bool isGetAddr, bool isAddrMut);
    llvm::SmallVector<OwnershipDiagInfo>
    checkBOPFieldUse(const VarDecl *VD, const SourceLocation &Loc,
                     std::string fullFieldName, bool isGetAddr);
    llvm::SmallVector<OwnershipDiagInfo>
    checkBOPAssign(const VarDecl *VD, const SourceLocation &Loc);
    llvm::SmallVector<OwnershipDiagInfo>
    checkBOPFieldAssign(const VarDecl *VD, const SourceLocation &Loc,
                        std::string fullFieldName);

    llvm::SmallVector<OwnershipDiagInfo>
    checkCastOPS(const VarDecl *VD, const SourceLocation &Loc);
    llvm::SmallVector<OwnershipDiagInfo>
    checkCastBOP(const VarDecl *VD, const SourceLocation &Loc);
    llvm::SmallVector<OwnershipDiagInfo>
    checkCastField(const VarDecl *VD, const SourceLocation &Loc,
                   std::string fullFieldName);
    llvm::SmallVector<OwnershipDiagInfo>
    checkMemoryLeak(const VarDecl *VD, const SourceLocation &Loc,
                    bool isDestructor);

    OwnershipStatus()
        : OPSStatus(0), OPSAllOwnedFields(0), OPSOwnedOwnedFields(0),
          OPSNullOwnedFields(0),
          SStatus(0), SAllOwnedFields(0), SOwnedOwnedFields(0), BOPStatus(0),
          BOPAllOwnedFields(0), BOPOwnedOwnedFields(0) {}

    OwnershipStatus(llvm::DenseMap<const VarDecl *, OwnershipSet> opss,
                    llvm::DenseMap<const VarDecl *, OPSOwnedField> opsaof,
                    llvm::DenseMap<const VarDecl *, OPSOwnedField> opsoof,
                    llvm::DenseMap<const VarDecl *, OPSOwnedField> opsofn,
                    llvm::DenseMap<const VarDecl *, OwnershipSet> ss,
                    llvm::DenseMap<const VarDecl *, OPSOwnedField> saof,
                    llvm::DenseMap<const VarDecl *, SOwnedField> soof,
                    llvm::DenseMap<const VarDecl *, OPSOwnedField> snof,
                    llvm::DenseMap<const VarDecl *, SOwnedField> sunof,
                    llvm::DenseMap<const VarDecl *, OwnershipSet> bops,
                    llvm::DenseMap<const VarDecl *, BOPOwnedField> bopaof,
                    llvm::DenseMap<const VarDecl *, BOPOwnedField> bopoof)
        : OPSStatus(opss), OPSAllOwnedFields(opsaof),
          OPSOwnedOwnedFields(opsoof), OPSNullOwnedFields(opsofn),
          SStatus(ss), SAllOwnedFields(saof),
          SOwnedOwnedFields(soof), SNullOwnedFields(snof),
          SUninitOwnedFields(sunof), BOPStatus(bops),
          BOPAllOwnedFields(bopaof), BOPOwnedOwnedFields(bopoof) {}

  private:
    void initOPS(const RecordDecl *RD, const VarDecl *VD, Source source,
                 int depth = 10, std::string parentFieldName = "");
    void initS(const RecordDecl *RD, const VarDecl *VD, Source source,
               int depth = 10, std::string parentFieldName = "");
    void initBOP(QualType QT, const VarDecl *VD, Source source, int depth = 10,
                 std::string parentFieldName = "");
    std::string collectMovedFields(const VarDecl *VD);

    friend class OwnerShip;
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
  /// Arrays covered by at least one qualifying for-loop. The loop-header
  /// ArraySubscript / MemberExpr sites where ownership transfer is allowed.
  llvm::SmallPtrSet<const Expr *, 16> AllowedTransferExprs;
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
                             OwnedArrayLoopInfo &LoopInfo,
                             OwnershipDiagReporter &OwnershipReporter);

} // end namespace clang

#endif // ENABLE_BSC

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_BSCOWNERSHIP_H