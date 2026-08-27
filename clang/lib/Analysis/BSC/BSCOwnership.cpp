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
#include "clang/AST/StmtVisitor.h"
#include "clang/Analysis/Analyses/BSC/BSCNullCheckInfo.h"
#include "clang/Analysis/Analyses/PostOrderCFGView.h"
#include "clang/Analysis/CFG.h"
#include "clang/Analysis/FlowSensitive/DataflowWorklist.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include <algorithm>
#include <functional>
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

static llvm::SmallSet<string, 10>
findPrefixStrings(const llvm::SmallSet<string, 10> fieldSet, string prefix) {
  llvm::SmallSet<string, 10> prefixStrings = {};
  for (const auto &str : fieldSet) {
    if (str.compare(0, prefix.size(), prefix) == 0) {
      prefixStrings.insert(str);
    }
  }
  return prefixStrings;
}

// Whether prefix names the same field as path or a field enclosing it,
// honoring path separators: "a.b" is a prefix of "a.b", "a.b.c" and "a.b*",
// but not of "a.bc".
static bool isFieldPathPrefix(const string &prefix, const string &path) {
  if (path.size() < prefix.size() ||
      path.compare(0, prefix.size(), prefix) != 0)
    return false;
  return path.size() == prefix.size() || path[prefix.size()] == '.' ||
         path[prefix.size()] == '*';
}

// Whether the access path touches any field tracked in the set: the path is
// a tracked field itself or a dereference of one ("data.", "data*"), lies
// inside one ("inner.a" under "inner"), or covers one ("a1" over "a1.b1").
// A trailing '.' is an access marker, not part of the key, and is dropped
// before matching.
static bool overlapsOwnedFields(const llvm::SmallSet<string, 10> &ownedFields,
                                string path) {
  if (!path.empty() && path.back() == '.')
    path.pop_back();
  if (path.empty())
    return true;
  for (const auto &owned : ownedFields)
    if (isFieldPathPrefix(owned, path) || isFieldPathPrefix(path, owned))
      return true;
  return false;
}

// Deref uses append trailing '*' markers to the field path, e.g. **q.pp is
// encoded as "pp**". The ownership sets may also contain real '*' keys for
// nested owned fields, such as "pp*" for the inner owned pointer of pp, so we
// cannot strip all markers to the bare field. Walk back through "pp**" ->
// "pp*" -> "pp" and return the outermost moved key that is actually tracked,
// keeping diagnostics focused on the field that was moved out.
static string findMovedFieldKey(const llvm::SmallSet<string, 10> &allFields,
                                const llvm::SmallSet<string, 10> &ownedFields,
                                const llvm::SmallSet<string, 10> *nullFields,
                                string fieldName) {
  string movedFieldName;
  while (!fieldName.empty()) {
    if (allFields.count(fieldName) && !ownedFields.count(fieldName) &&
        (!nullFields || !nullFields->count(fieldName)))
      movedFieldName = fieldName;
    if (fieldName.back() != '*')
      break;
    fieldName.pop_back();
  }
  return movedFieldName;
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

static pair<const Expr *, string> getMemberFullField(const MemberExpr *ME) {
  const Expr *base = ME->getBase();
  string memberFieldName = ME->getMemberNameInfo().getAsString();

  while (true) {
    base = base->IgnoreParenImpCastsSafe();
    if (const MemberExpr *me = dyn_cast<MemberExpr>(base)) {
      memberFieldName =
          me->getMemberNameInfo().getAsString() + "." + memberFieldName;
      base = me->getBase();
    } else {
      break;
    }
  }

  return {base, memberFieldName};
}

/// Peel a leading dereference from a member-expr base so that `p->f` and
/// `(*p).f` resolve to the same root DeclRefExpr.
static const DeclRefExpr *getRootDREFromMemberBase(const Expr *Base) {
  const Expr *E = Base->IgnoreParenImpCastsSafe();
  if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_Deref)
      E = UO->getSubExpr()->IgnoreParenImpCastsSafe();
  }
  return dyn_cast<DeclRefExpr>(E);
}

// Peel an array-element expression a[i] / a[i][j] / s[i].f / s[i].a[j] down
// to the base VarDecl (of an array or of a _ArrayElem pointer); nullptr if the
// expression is not such an element access. Shared by the dataflow transfer
// functions and the null-check handling.
static const VarDecl *PeelArrayElemBase(const Expr *E) {
  if (!E)
    return nullptr;
  const Expr *Base = E->IgnoreParenImpCastsSafe();
  if (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(Base)) {
    Base = ASE->getBase()->IgnoreParenImpCastsSafe();
    while (const ArraySubscriptExpr *Inner =
               dyn_cast<ArraySubscriptExpr>(Base))
      Base = Inner->getBase()->IgnoreParenImpCastsSafe();
  } else if (const MemberExpr *ME = dyn_cast<MemberExpr>(Base)) {
    Base = ME->getBase()->IgnoreParenImpCastsSafe();
    while (const ArraySubscriptExpr *A = dyn_cast<ArraySubscriptExpr>(Base))
      Base = A->getBase()->IgnoreParenImpCastsSafe();
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
static bool GetOwnedArrayField(const Expr *E, const VarDecl *&HostVD,
                               std::string &FieldPath) {
  if (!PeelHostAndFieldPath(E, HostVD, FieldPath))
    return false;
  return IsOwnedArrayFieldPath(HostVD, FieldPath);
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

static string concatFields(const VarDecl *VD,
                           const llvm::SmallSet<string, 10> &fields) {
  string contactedFields = "";
  size_t count = 0;
  for (const string &element : fields) {
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

static string concatUnmovedFields(const VarDecl *VD,
                                  const llvm::SmallSet<string, 10> &ownedFields,
                                  const llvm::SmallSet<string, 10> &allFields) {
  llvm::SmallSet<string, 10> unmovedFields;
  for (const auto &field : allFields) {
    if (!ownedFields.count(field)) {
      unmovedFields.insert(field);
    }
  }
  return concatFields(VD, unmovedFields);
}

static bool IsCastFromVoidPointer(Expr *E) {
  while (true) {
    E = E->IgnoreParenImpCasts();
    if (SafeExpr *SE = dyn_cast<SafeExpr>(E)) {
      E = SE->getSubExpr();
    } else {
      break;
    }
  }

  if (AbstractConditionalOperator *ACO =
          dyn_cast<AbstractConditionalOperator>(E)) {
    return IsCastFromVoidPointer(ACO->getTrueExpr()) ||
           IsCastFromVoidPointer(ACO->getFalseExpr());
  }
  if (CStyleCastExpr *CSCE = dyn_cast<CStyleCastExpr>(E)) {
    QualType QT = CSCE->getType();
    if (QT->isPointerType() && QT.isOwnedQualified() &&
        !QT->getPointeeType()->isVoidType()) {
      Expr *Sub = CSCE->getSubExpr();
      if (Sub->getType()->isVoidPointerType() &&
          Sub->getType().isOwnedQualified()) {
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
         SStatus.empty() &&
         SAllOwnedFields.empty() && SOwnedOwnedFields.empty() &&
         SNullOwnedFields.empty() && BOPStatus.empty() &&
         BOPAllOwnedFields.empty() && BOPOwnedOwnedFields.empty();
}

Ownership::OwnershipStatus
OwnershipImpl::merge(Ownership::OwnershipStatus statsA,
                     Ownership::OwnershipStatus statsB) {
  if (statsA.empty())
    return Ownership::OwnershipStatus(
        statsB.OPSStatus, statsB.OPSAllOwnedFields, statsB.OPSOwnedOwnedFields,
        statsB.OPSNullOwnedFields, statsB.SStatus, statsB.SAllOwnedFields,
        statsB.SOwnedOwnedFields,
        statsB.SNullOwnedFields, statsB.SUninitOwnedFields,
        statsB.BOPStatus, statsB.BOPAllOwnedFields, statsB.BOPOwnedOwnedFields);

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
    llvm::SmallSet<string, 10> OwnedValue = statsB.OPSOwnedOwnedFields[VD];
    llvm::SmallSet<string, 10> NullValue = statsB.OPSNullOwnedFields[VD];
    if (statsA.OPSAllOwnedFields.count(VD) &&
        (!statsA.OPSOwnedOwnedFields.empty() ||
         !statsA.OPSNullOwnedFields.empty())) {
      if (!statsA.OPSOwnedOwnedFields.empty() && !OwnedValue.empty()) {
        for (const auto &s : OwnedValue)
          statsA.OPSOwnedOwnedFields[VD].insert(s);
      }
      if (!statsA.OPSNullOwnedFields.empty() && !NullValue.empty()) {
        for (const auto &s : NullValue)
          statsA.OPSNullOwnedFields[VD].insert(s);
      }
    } else {
      // statsA has no per-field state for VD: adopt statsB's field sets.
      // Copy the static All list and the Owned / Null sets separately
      // (mirror of the S branch), instead of overwriting All with the
      // Owned values.
      for (const auto &s : it->second)
        statsA.OPSAllOwnedFields[VD].insert(s);
      for (const auto &s : OwnedValue)
        statsA.OPSOwnedOwnedFields[VD].insert(s);
      for (const auto &s : NullValue)
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
    llvm::SmallSet<string, 10> OwnedValue = statsB.SOwnedOwnedFields[VD];
    llvm::SmallSet<string, 10> NullValue = statsB.SNullOwnedFields[VD];
    if (statsA.SAllOwnedFields.count(VD) &&
        (!statsA.SOwnedOwnedFields.empty() || !statsA.SNullOwnedFields.empty())) {
      if (!statsA.SOwnedOwnedFields.empty() && !OwnedValue.empty()) {
        for (const auto &s : OwnedValue)
          statsA.SOwnedOwnedFields[VD].insert(s);
      }
      if (!statsA.SNullOwnedFields.empty() && !NullValue.empty()) {
        for (const auto &s : NullValue)
          statsA.SNullOwnedFields[VD].insert(s);
      }
    } else {
      // statsA has no per-field state for VD (both owned and null field sets
      // are empty): adopt statsB's field sets. SAllOwnedFields must be copied
      // too, otherwise later field transfers (setArrayFieldMoved etc.) see an
      // empty static field list and skip the transition.
      for (const auto &s : it->second)
        statsA.SAllOwnedFields[VD].insert(s);
      for (const auto &s : OwnedValue)
        statsA.SOwnedOwnedFields[VD].insert(s);
      for (const auto &s : NullValue)
        statsA.SNullOwnedFields[VD].insert(s);
    }
  }
  // A path uninitialized on either branch stays uninitialized.
  for (auto it = statsB.SUninitOwnedFields.begin(),
            ei = statsB.SUninitOwnedFields.end();
       it != ei; ++it)
    for (const auto &s : it->second)
      statsA.SUninitOwnedFields[it->first].insert(s);

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
    llvm::SmallSet<string, 10> value = statsB.BOPOwnedOwnedFields[VD];
    if (statsA.BOPAllOwnedFields.count(VD) &&
        !statsA.BOPOwnedOwnedFields.empty()) {
      if (!value.empty()) {
        for (const auto& s : value)
          statsA.BOPOwnedOwnedFields[VD].insert(s);
      }
    } else {
      statsA.BOPAllOwnedFields[VD] = value;
    }
  }

  return Ownership::OwnershipStatus(
      statsA.OPSStatus, statsA.OPSAllOwnedFields, statsA.OPSOwnedOwnedFields,
      statsA.OPSNullOwnedFields, statsA.SStatus, statsA.SAllOwnedFields,
      statsA.SOwnedOwnedFields, statsA.SNullOwnedFields,
      statsA.SUninitOwnedFields, statsA.BOPStatus, statsA.BOPAllOwnedFields,
      statsA.BOPOwnedOwnedFields);
}

bool Ownership::OwnershipStatus::equals(const OwnershipStatus &V) const {
  return OPSStatus == V.OPSStatus && OPSAllOwnedFields == V.OPSAllOwnedFields &&
         OPSOwnedOwnedFields == V.OPSOwnedOwnedFields &&
         OPSNullOwnedFields == V.OPSNullOwnedFields && SStatus == V.SStatus &&
         SAllOwnedFields == V.SAllOwnedFields &&
         SOwnedOwnedFields == V.SOwnedOwnedFields &&
         SNullOwnedFields == V.SNullOwnedFields &&
         SUninitOwnedFields == V.SUninitOwnedFields &&
         BOPStatus == V.BOPStatus &&
         BOPAllOwnedFields == V.BOPAllOwnedFields &&
         BOPOwnedOwnedFields == V.BOPOwnedOwnedFields;
}

bool Ownership::OwnershipStatus::is(const VarDecl *VD, Status S) const {
  if (OPSStatus.count(VD)) {
    auto it = OPSStatus.find(VD);
    llvm::BitVector status = it->second;
    unsigned bitIndex = getIndex(S);
    if (status.test(bitIndex)) {
      status.reset(bitIndex);
      return !status.any();
    }
    return false;
  }

  if (SStatus.count(VD)) {
    auto it = SStatus.find(VD);
    llvm::BitVector status = it->second;
    unsigned bitIndex = getIndex(S);
    if (status.test(bitIndex)) {
      status.reset(bitIndex);
      return !status.any();
    }
    return false;
  }

  if (BOPStatus.count(VD)) {
    auto it = BOPStatus.find(VD);
    llvm::BitVector status = it->second;
    unsigned bitIndex = getIndex(S);
    if (status.test(bitIndex)) {
      status.reset(bitIndex);
      return !status.any();
    }
    return false;
  }

  return false;
}

bool Ownership::OwnershipStatus::has(const VarDecl *VD, Status S) const {
  if (OPSStatus.count(VD)) {
    auto it = OPSStatus.find(VD);
    llvm::BitVector status = it->second;
    unsigned bitIndex = getIndex(S);
    if (status.test(bitIndex)) {
      status.reset(bitIndex);
      return status.any();
    }
    return false;
  }

  if (SStatus.count(VD)) {
    auto it = SStatus.find(VD);
    llvm::BitVector status = it->second;
    unsigned bitIndex = getIndex(S);
    if (status.test(bitIndex)) {
      status.reset(bitIndex);
      return status.any();
    }
    return false;
  }

  if (BOPStatus.count(VD)) {
    auto it = BOPStatus.find(VD);
    llvm::BitVector status = it->second;
    unsigned bitIndex = getIndex(S);
    if (status.test(bitIndex)) {
      status.reset(bitIndex);
      return status.any();
    }
    return false;
  }

  return false;
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

void Ownership::OwnershipStatus::init(const VarDecl *VD) {
  QualType VDType = VD->getType();
  if (VDType->isPointerType()) {
    if (VDType->getPointeeType().getCanonicalType()->isRecordType()) {
      if (!OPSStatus.count(VD)) {
        if (const RecordType *RT = dyn_cast<RecordType>(
                VDType->getPointeeType().getCanonicalType())) {
          initOPS(RT->getDecl(), VD, Source::OPS);
        }
      }
    } else {
      if (!BOPStatus.count(VD)) {
        initBOP(VDType, VD, Source::BOP);
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
    if (ElemTy->isPointerType() && ElemTy.isOwnedQualified()) {
      if (!BOPStatus.count(VD)) {
        BOPStatus[VD] = llvm::BitVector(7, 0);
        set(VD, Uninitialized);
      }
    } else if (ElemTy->isRecordType() &&
               (ElemTy.getTypePtr()->isOwnedStructureType() ||
                ElemTy->isMoveSemanticType())) {
      if (!SStatus.count(VD)) {
        // ElemTy may be an ElaboratedType (e.g. "struct S") rather than the
        // canonical RecordType; peel sugar so dyn_cast finds the record.
        if (const RecordType *RT =
                dyn_cast<RecordType>(ElemTy.getCanonicalType())) {
          initS(RT->getDecl(), VD, Source::S);
        }
      }
    }
  } else {
    if (!SStatus.count(VD)) {
      if (const RecordType *RT =
              dyn_cast<RecordType>(VDType.getCanonicalType())) {
        initS(RT->getDecl(), VD, Source::S);
      }
    }
  }
}

// Strip the array levels of a field type, returning the "[]" suffix and the
// element type. Returns false when the field is not an array. Shared by
// initS / initOPS (the path uses `[]` for each array level).
static bool StripArrayFieldLevels(QualType FieldTy, std::string &ArraySuffix,
                                  QualType &ElemTy) {
  ArraySuffix.clear();
  ElemTy = FieldTy;
  while (const auto *AT = dyn_cast<ArrayType>(ElemTy)) {
    ArraySuffix += "[]";
    ElemTy = AT->getElementType();
  }
  return !ArraySuffix.empty();
}

// Register an array field ("arr[]" for owned-pointer elements, "arr[].a" via
// recursion for owned-record elements) into the target All set. Returns the
// record to recurse into, or nullptr for owned-pointer elements (leaf).
static const RecordDecl *
RegisterArrayField(llvm::SmallSet<std::string, 10> &AllSet,
                   const std::string &ArrFieldName, QualType ElemTy) {
  if (ElemTy->isPointerType() && ElemTy.isOwnedQualified()) {
    AllSet.insert(ArrFieldName);
    return nullptr;
  }
  if (const RecordType *RT =
          dyn_cast<RecordType>(ElemTy.getCanonicalType())) {
    if (RT->isOwnedStructureType())
      AllSet.insert(ArrFieldName);
    return RT->getDecl();
  }
  return nullptr;
}

void Ownership::OwnershipStatus::initOPS(const RecordDecl *RD,
                                         const VarDecl *VD, Source source,
                                         int depth, string parentFieldName) {
  if (depth == 0) {
    return;
  }
  // record all owned fields of VD in OPSAllOwnedFields
  for (RecordDecl::field_iterator it = RD->field_begin(), ei = RD->field_end();
       it != ei; ++it) {
    // Use the sugared type: canonicalizing an array type drops the local
    // _Owned qualifier of its element (int *_Owned arr[2]).
    QualType FT = it->getType();
    string fieldName = parentFieldName + it->getNameAsString();
    if (IsTrackedType(FT)) {
      // Array fields (e.g. struct S { int *_Owned arr[2]; }): the path uses
      // `[]` for the array level and recurses into the element type, so the
      // tracked field becomes "arr[]" (owned pointers) / "arr[].a" (records).
      QualType FieldTy;
      string arraySuffix;
      if (StripArrayFieldLevels(FT, arraySuffix, FieldTy)) {
        string arrFieldName = fieldName + arraySuffix;
        if (source == Source::OPS) {
          if (OPSAllOwnedFields[VD].empty()) {
            OPSAllOwnedFields[VD] = {};
            OPSOwnedOwnedFields[VD] = {};
            OPSNullOwnedFields[VD] = {};
          }
          if (const RecordDecl *RT = RegisterArrayField(
                  OPSAllOwnedFields[VD], arrFieldName, FieldTy))
            initOPS(RT, VD, source, depth - 1, arrFieldName + ".");
        } else if (source == Source::S) {
          if (SAllOwnedFields[VD].empty()) {
            SAllOwnedFields[VD] = {};
            SOwnedOwnedFields[VD] = {};
            SNullOwnedFields[VD] = {};
          }
          if (const RecordDecl *RT = RegisterArrayField(
                  SAllOwnedFields[VD], arrFieldName, FieldTy))
            initOPS(RT, VD, source, depth - 1, arrFieldName + ".");
        }
        continue;
      }
      if (source == Source::OPS) {
        if (OPSAllOwnedFields[VD].empty()) {
          OPSAllOwnedFields[VD] = {};
          OPSOwnedOwnedFields[VD] = {};
          OPSNullOwnedFields[VD] = {};
        }
        if (!FT->isRecordType() || FT->isOwnedStructureType())
          OPSAllOwnedFields[VD].insert(fieldName);
      } else if (source == Source::S) {
        if (!FT->isRecordType() || FT->isOwnedStructureType())
          SAllOwnedFields[VD].insert(fieldName);
      } else {
        llvm_unreachable("Unexpected branch");
      }

      // if FT has owned fields,
      // recursively record all owned fields of FT in OPSAllOwnedFields[VD]
      if (FT->isPointerType() && FT.isOwnedQualified()) {
        if (const RecordType *RT =
                dyn_cast<RecordType>(FT->getPointeeType().getCanonicalType())) {
          if (RT->isOwnedStructureType()) {
            if (source == Source::OPS) {
              OPSAllOwnedFields[VD].insert(fieldName + "*");
            } else if (source == Source::S) {
              SAllOwnedFields[VD].insert(fieldName + "*");
            } else {
              llvm_unreachable("Unexpected branch");
            }
          }
          initOPS(RT->getDecl(), VD, source, depth - 1, fieldName + ".");
        } else {
          initBOP(FT, VD, source, depth - 1, fieldName);
        }
      }
      if (const RecordType *RT = dyn_cast<RecordType>(FT.getCanonicalType())) {
        if (RT->isOwnedStructureType()) {
          if (source == Source::OPS) {
            OPSAllOwnedFields[VD].insert(fieldName);
          } else if (source == Source::S) {
            SAllOwnedFields[VD].insert(fieldName);
          } else {
            llvm_unreachable("Unexpected branch");
          }
        }
        initS(RT->getDecl(), VD, source, depth - 1, fieldName + ".");
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
                                       Source source, int depth,
                                       string parentFieldName) {
  if (depth == 0) {
    return;
  }

  // owned struct special manipulation
  if (VD->getType().getTypePtr()->isOwnedStructureType()) {
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
    string fieldName = parentFieldName + it->getNameAsString();
    if (IsTrackedType(FT)) {
      // Array fields (e.g. struct Wrap { struct S arr[2]; }): the path uses
      // `[]` for the array level and recurses into the element type, so the
      // tracked field becomes "arr[].a" instead of just "arr".
      QualType FieldTy;
      string arraySuffix;
      if (StripArrayFieldLevels(FT, arraySuffix, FieldTy)) {
        string arrFieldName = fieldName + arraySuffix;
        if (source == Source::OPS) {
          if (const RecordDecl *RT = RegisterArrayField(
                  OPSAllOwnedFields[VD], arrFieldName, FieldTy))
            initS(RT, VD, source, depth - 1, arrFieldName + ".");
        } else if (source == Source::S) {
          if (SAllOwnedFields[VD].empty()) {
            SAllOwnedFields[VD] = {};
            SOwnedOwnedFields[VD] = {};
            SNullOwnedFields[VD] = {};
          }
          if (const RecordDecl *RT = RegisterArrayField(
                  SAllOwnedFields[VD], arrFieldName, FieldTy))
            initS(RT, VD, source, depth - 1, arrFieldName + ".");
        }
        continue;
      }
      if (source == Source::OPS) {
        if (!FT->isRecordType() || FT->isOwnedStructureType())
          OPSAllOwnedFields[VD].insert(fieldName);
      } else if (source == Source::S) {
        if (SAllOwnedFields[VD].empty()) {
          SAllOwnedFields[VD] = {};
          SOwnedOwnedFields[VD] = {};
          SNullOwnedFields[VD] = {};
        }
        if (!FT->isRecordType() || FT->isOwnedStructureType())
          SAllOwnedFields[VD].insert(fieldName);
      } else {
        llvm_unreachable("Unexpected branch");
      }

      // recursively record all owned fields
      if (FT->isPointerType() && FT.isOwnedQualified()) {
        if (const RecordType *RT =
                dyn_cast<RecordType>(FT->getPointeeType().getCanonicalType())) {
          if (RT->isOwnedStructureType()) {
            if (source == Source::OPS) {
              OPSAllOwnedFields[VD].insert(fieldName + "*");
            } else if (source == Source::S) {
              SAllOwnedFields[VD].insert(fieldName + "*");
            } else {
              llvm_unreachable("Unexpected branch");
            }
          }
          initOPS(RT->getDecl(), VD, source, depth - 1, fieldName + ".");
        } else {
          initBOP(FT, VD, source, depth - 1, fieldName);
        }
      }
      if (const RecordType *RT = dyn_cast<RecordType>(FT.getCanonicalType())) {
        if (RT->isOwnedStructureType()) {
          if (source == Source::OPS) {
            OPSAllOwnedFields[VD].insert(fieldName);
          } else if (source == Source::S) {
            SAllOwnedFields[VD].insert(fieldName);
          } else {
            llvm_unreachable("Unexpected branch");
          }
        }
        initS(RT->getDecl(), VD, source, depth - 1, fieldName + ".");
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
                                         Source source, int depth,
                                         string parentFieldName) {
  if (depth == 0) {
    return;
  }
  string fieldName;
  QT = QT->getPointeeType().getCanonicalType();
  while (true) {
    fieldName += "*";
    if (QT->isPointerType() && QT.isOwnedQualified()) {
      if (source == Source::OPS) {
        OPSAllOwnedFields[VD].insert(parentFieldName + fieldName);
      } else if (source == Source::S) {
        SAllOwnedFields[VD].insert(parentFieldName + fieldName);
      } else {
        if (BOPAllOwnedFields[VD].empty()) {
          BOPAllOwnedFields[VD] = {};
          BOPOwnedOwnedFields[VD] = {};
        }
        BOPAllOwnedFields[VD].insert(fieldName);
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
  }

  if (SStatus.count(VD)) {
    resetAll(VD);
    set(VD, Ownership::Status::Owned);
    SOwnedOwnedFields[VD] = SAllOwnedFields[VD];
    SUninitOwnedFields[VD].clear();
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

void Ownership::OwnershipStatus::setToAllMoved(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
    setToAllMoved(VD);
    return;
  }
  if (const MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
    pair<const Expr *, string> memberField = getMemberFullField(ME);
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(memberField.first)) {
      const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
      if (OPSStatus.count(VD)) {
        if (OPSAllOwnedFields[VD].count(memberField.second)) {
          OPSOwnedOwnedFields[VD].insert(memberField.second);
          auto allPrefixStrs = findPrefixStrings(OPSAllOwnedFields[VD],
                                                 memberField.second + ".");
          for (const string &str : allPrefixStrs) {
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
        if (SAllOwnedFields[VD].count(memberField.second)) {
          SOwnedOwnedFields[VD].insert(memberField.second);
          SNullOwnedFields[VD].erase(memberField.second);
          SUninitOwnedFields[VD].erase(memberField.second);
          auto allPrefixStrs =
              findPrefixStrings(SAllOwnedFields[VD], memberField.second + ".");
          for (const string &str : allPrefixStrs) {
            SOwnedOwnedFields[VD].erase(str);
            SNullOwnedFields[VD].erase(str);
            SUninitOwnedFields[VD].erase(str);
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

void Ownership::OwnershipStatus::setToNull(const Expr *E) {
  E = E->IgnoreParenImpCasts();
  if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
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
      const VarDecl *HostVD = nullptr;
      std::string FieldPath;
      if (PeelHostAndFieldPath(E, HostVD, FieldPath) &&
          IsOwnedArrayFieldPath(HostVD, FieldPath))
        setArrayFieldNull(HostVD, FieldPath);
      return;
    }
  }
  if (const MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
    pair<const Expr *, string> memberField = getMemberFullField(ME);
    // Struct-array member: s[i].f = nullptr (base is an ArraySubscriptExpr).
    const Expr *Base = memberField.first->IgnoreParenImpCasts();
    while (const ArraySubscriptExpr *ASE = dyn_cast<ArraySubscriptExpr>(Base))
      Base = ASE->getBase()->IgnoreParenImpCasts();
    if (const DeclRefExpr *BaseDRE = dyn_cast<DeclRefExpr>(Base)) {
      if (const VarDecl *VD = dyn_cast<VarDecl>(BaseDRE->getDecl())) {
        if (IsOwnedElementArrayType(VD->getType())) {
          setArrayFieldNull(VD, memberField.second);
          return;
        }
      }
    }
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(memberField.first)) {
      const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
      if (OPSStatus.count(VD)) {
        if (OPSAllOwnedFields[VD].count(memberField.second)) {
          OPSOwnedOwnedFields[VD].erase(memberField.second);
          auto allPrefixStrs = findPrefixStrings(OPSAllOwnedFields[VD],
                                                 memberField.second + ".");
          for (const string &str : allPrefixStrs) {
            OPSOwnedOwnedFields[VD].erase(str);
          }
        }
      }
      if (SStatus.count(VD)) {
        if (SAllOwnedFields[VD].count(memberField.second)) {
          SOwnedOwnedFields[VD].erase(memberField.second);
          SNullOwnedFields[VD].insert(memberField.second);
          SUninitOwnedFields[VD].erase(memberField.second);
          auto allPrefixStrs =
              findPrefixStrings(SAllOwnedFields[VD], memberField.second + ".");
          for (const string &str : allPrefixStrs) {
            SOwnedOwnedFields[VD].erase(str);
            SNullOwnedFields[VD].insert(str);
            SUninitOwnedFields[VD].erase(str);
          }
        }
      }
    }
  }
  if (const UnaryOperator *UO = dyn_cast<UnaryOperator>(E)) {
    string suffix;
    const Expr *e = UO;
    while (UO->getOpcode() == UO_Deref) {
      if (const ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(e)) {
        e = ICE->getSubExpr();
      } else if (const UnaryOperator *uo = dyn_cast<UnaryOperator>(e)) {
        UO = uo;
        suffix += "*";
        e = UO->getSubExpr();
      } else {
        break;
      }
    }
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(e)) {
      const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
      if (BOPStatus.count(VD)) {
        if (BOPAllOwnedFields[VD].count(suffix)) {
          BOPOwnedOwnedFields[VD].erase(suffix);
          auto allPrefixStrs =
              findPrefixStrings(BOPAllOwnedFields[VD], suffix + "*");
          for (const string &str : allPrefixStrs) {
            BOPOwnedOwnedFields[VD].erase(str);
          }
          if (BOPAllOwnedFields[VD].size() != BOPOwnedOwnedFields[VD].size()) {
            if (!is(VD, Moved)) {
              resetAll(VD);
              set(VD, PartialMoved);
            }
          }
          if (BOPOwnedOwnedFields[VD].empty()) {
            if (!is(VD, Moved)) {
              resetAll(VD);
              set(VD, AllMoved);
            }
          }
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
    for (const string &field : SAllOwnedFields[VD])
      SOwnedOwnedFields[VD].erase(field);
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
    if (SNullOwnedFields.count(VD))
      SNullOwnedFields[VD] = SAllOwnedFields[VD];
    resetAll(VD);
    set(VD, Ownership::Status::AllMoved);
  }
}

void Ownership::OwnershipStatus::setArrayFieldMoved(const VarDecl *VD,
                                                    const std::string &fieldName) {
  if (SStatus.count(VD)) {
    if (!SAllOwnedFields[VD].count(fieldName))
      return;
    SOwnedOwnedFields[VD].erase(fieldName);
    SNullOwnedFields[VD].erase(fieldName);
    for (const string &str :
         findPrefixStrings(SAllOwnedFields[VD], fieldName + ".")) {
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
    for (const string &str :
         findPrefixStrings(OPSAllOwnedFields[VD], fieldName + ".")) {
      OPSOwnedOwnedFields[VD].erase(str);
      OPSNullOwnedFields[VD].erase(str);
    }
    refreshArrayFieldState(VD, OPSOwnedOwnedFields[VD],
                           OPSAllOwnedFields[VD]);
  }
}

void Ownership::OwnershipStatus::setArrayFieldOwned(const VarDecl *VD,
                                                    const std::string &fieldName) {
  if (SStatus.count(VD)) {
    if (!SAllOwnedFields[VD].count(fieldName))
      return;
    SOwnedOwnedFields[VD].insert(fieldName);
    SNullOwnedFields[VD].erase(fieldName);
    refreshArrayFieldState(VD, SOwnedOwnedFields[VD], SAllOwnedFields[VD]);
  }
  if (OPSStatus.count(VD)) {
    if (!OPSAllOwnedFields[VD].count(fieldName))
      return;
    OPSOwnedOwnedFields[VD].insert(fieldName);
    OPSNullOwnedFields[VD].erase(fieldName);
    refreshArrayFieldState(VD, OPSOwnedOwnedFields[VD], OPSAllOwnedFields[VD]);
  }
}

void Ownership::OwnershipStatus::setArrayFieldNull(const VarDecl *VD,
                                                   const std::string &fieldName) {
  if (SStatus.count(VD)) {
    if (!SAllOwnedFields[VD].count(fieldName))
      return;
    SOwnedOwnedFields[VD].erase(fieldName);
    SNullOwnedFields[VD].insert(fieldName);
    refreshArrayFieldState(VD, SOwnedOwnedFields[VD], SAllOwnedFields[VD]);
  }
  if (OPSStatus.count(VD)) {
    if (!OPSAllOwnedFields[VD].count(fieldName))
      return;
    OPSOwnedOwnedFields[VD].erase(fieldName);
    OPSNullOwnedFields[VD].insert(fieldName);
    refreshArrayFieldState(VD, OPSOwnedOwnedFields[VD], OPSAllOwnedFields[VD]);
  }
}

bool Ownership::OwnershipStatus::arrayAggregateMoved(const VarDecl *VD) const {
  if (BOPStatus.count(VD))
    return is(VD, Ownership::Status::Moved);
  if (SStatus.count(VD))
    return SOwnedOwnedFields.lookup(VD).empty();
  if (OPSStatus.count(VD))
    return OPSOwnedOwnedFields.lookup(VD).empty();
  return false;
}

bool Ownership::OwnershipStatus::arrayFieldOwned(
    const VarDecl *VD, const std::string &fieldName) const {
  if (SStatus.count(VD))
    return SOwnedOwnedFields.lookup(VD).count(fieldName) > 0;
  if (OPSStatus.count(VD))
    return OPSOwnedOwnedFields.lookup(VD).count(fieldName) > 0;
  return false;
}

bool Ownership::OwnershipStatus::arrayAggregateUninit(const VarDecl *VD) const {
  if (BOPStatus.count(VD) || SStatus.count(VD) || OPSStatus.count(VD))
    return is(VD, Ownership::Status::Uninitialized);
  return false;
}

bool Ownership::OwnershipStatus::arrayAggregateNull(
    const VarDecl *VD, const std::string &fieldName) const {
  if (fieldName.empty()) {
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
  if (SStatus.count(VD))
    return SNullOwnedFields.lookup(VD).count(fieldName) > 0;
  if (OPSStatus.count(VD))
    return OPSNullOwnedFields.lookup(VD).count(fieldName) > 0;
  return false;
}

void Ownership::OwnershipStatus::refreshArrayFieldState(
    const VarDecl *VD, const llvm::SmallSet<std::string, 10> &Owned,
    const llvm::SmallSet<std::string, 10> &All) {
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
  SStatus.erase(VD);
  SAllOwnedFields.erase(VD);
  SOwnedOwnedFields.erase(VD);
  SNullOwnedFields.erase(VD);
  SUninitOwnedFields.erase(VD);
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
    }
    if (Other.SStatus.count(VD)) {
      SStatus[VD] = Other.SStatus.lookup(VD);
      SAllOwnedFields[VD] = Other.SAllOwnedFields.lookup(VD);
      SOwnedOwnedFields[VD] = Other.SOwnedOwnedFields.lookup(VD);
      SNullOwnedFields[VD] = Other.SNullOwnedFields.lookup(VD);
      SUninitOwnedFields[VD] = Other.SUninitOwnedFields.lookup(VD);
    }
    if (Other.BOPStatus.count(VD)) {
      BOPStatus[VD] = Other.BOPStatus.lookup(VD);
      BOPAllOwnedFields[VD] = Other.BOPAllOwnedFields.lookup(VD);
      BOPOwnedOwnedFields[VD] = Other.BOPOwnedOwnedFields.lookup(VD);
    }
  }
}

string Ownership::OwnershipStatus::collectMovedFields(const VarDecl *VD) {
  llvm::SmallSet<string, 10> allFields, ownedFields;
  if (OPSStatus.count(VD)) {
    allFields = OPSAllOwnedFields[VD];
    ownedFields = OPSOwnedOwnedFields[VD];
  } else if (SStatus.count(VD)) {
    allFields = SAllOwnedFields[VD];
    ownedFields = SOwnedOwnedFields[VD];
  } else if (BOPStatus.count(VD)) {
    allFields = BOPAllOwnedFields[VD];
    ownedFields = BOPOwnedOwnedFields[VD];
  }
  string movedFields = "";
  int count = 0;
  for (const auto &element : allFields) {
    if (ownedFields.count(element) == 0) {
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
    if (has(VD, Ownership::Status::Moved) || is(VD, Ownership::Status::Moved)) {
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
    const VarDecl *VD, const SourceLocation &Loc, string fullFieldName,
    bool isGetAddr) {
  SmallVector<OwnershipDiagInfo> diags;

  // when assign a field, we check three conditions:
  // 1. the field's parent must be owned
  // 2. the status of VD must not be moved or uninit
  // 3. the field and the field's subfields must be all moved

  // check condition 1
  int index = fullFieldName.length() - 2;
  string current = fullFieldName;
  while (index > 0) {
    if (current[index] == '*') {
      current = current.substr(0, index + 1);
      index--;
    } else {
      size_t pos = current.find_last_of('.');
      if (pos != string::npos) {
        current = current.substr(0, pos);
        index = pos;
      } else {
        break;
      }
    }
    if (OPSAllOwnedFields[VD].count(current) &&
        !OPSOwnedOwnedFields[VD].count(current)) {
      OwnershipDiagKind Kind = is(VD, Uninitialized) || has(VD, Uninitialized)
                                   ? OwnershipDiagKind::InvalidUseOfUninit
                                   : OwnershipDiagKind::InvalidUseOfMoved;
      diags.push_back(OwnershipDiagInfo(
          Loc, Kind,
          moveAsterisksToFront(VD->getNameAsString() + "." + current)));
      break;
    }
  }

  // check condition 2
  if ((is(VD, Moved) || has(VD, Moved)) && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                          VD->getNameAsString() + "." + fullFieldName));
  }
  if ((is(VD, Uninitialized) || has(VD, Uninitialized)) && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfUninit,
                          VD->getNameAsString() + "." + fullFieldName));
  }

  // check condition 3
  // if fullFieldName has been moved, report error
  string movedFieldName =
      findMovedFieldKey(OPSAllOwnedFields[VD], OPSOwnedOwnedFields[VD],
                        nullptr, fullFieldName);
  if (!movedFieldName.empty() && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                          VD->getNameAsString() + "." + movedFieldName));
  }
  // calculate the fields with fullFieldName prefix
  llvm::SmallSet<string, 10> allPrefixStrs;
  if (fullFieldName[fullFieldName.size() - 1] == '.') {
     allPrefixStrs = findPrefixStrings(OPSAllOwnedFields[VD], fullFieldName);
  } else {
    allPrefixStrs = findPrefixStrings(OPSAllOwnedFields[VD], fullFieldName + ".");
  }
  auto allPrefixStrsStar =
      findPrefixStrings(OPSAllOwnedFields[VD], fullFieldName + "*");
  for (const auto &elem : allPrefixStrsStar) {
    allPrefixStrs.insert(elem);
  }
  llvm::SmallSet<string, 10> ownedPrefixStrs;
  if (fullFieldName[fullFieldName.size() - 1] == '.') {
    ownedPrefixStrs = findPrefixStrings(OPSOwnedOwnedFields[VD], fullFieldName);
  } else {
    ownedPrefixStrs = findPrefixStrings(OPSOwnedOwnedFields[VD], fullFieldName + ".");
  }
  auto ownedPrefixStrsStar =
      findPrefixStrings(OPSOwnedOwnedFields[VD], fullFieldName + "*");
  for (const auto& elem : ownedPrefixStrsStar) {
    ownedPrefixStrs.insert(elem);
  }
  if (allPrefixStrs.size() != ownedPrefixStrs.size() && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfPartiallyMoved,
                          VD->getNameAsString(), collectMovedFields(VD)));
  }
  // Reading non-owned fields will not update ownership state.
  bool TouchesOwnedSubtree =
      OPSAllOwnedFields[VD].count(fullFieldName) != 0 || !allPrefixStrs.empty();
  if (!isGetAddr && TouchesOwnedSubtree) {
    // change the status of the fields
    OPSOwnedOwnedFields[VD].erase(fullFieldName);
    // remove ownedPrefixStrs from OPSOwnedOwnedFields
    for (const auto &str : ownedPrefixStrs) {
      OPSOwnedOwnedFields[VD].erase(str);
    }
    if (OPSAllOwnedFields[VD].size() != OPSOwnedOwnedFields[VD].size()) {
      if (!is(VD, Ownership::Status::Moved)) {
        resetAll(VD);
        set(VD, Ownership::Status::PartialMoved);
      }
    }
    if (!OPSAllOwnedFields[VD].empty() && OPSOwnedOwnedFields[VD].empty() &&
        !VD->getType()->getPointeeType()->isOwnedStructureType()) {
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
    if (has(VD, Ownership::Status::Owned) || is(VD, Ownership::Status::Owned)) {
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
  resetAll(VD);
  set(VD, Ownership::Status::Owned);

  return diags;
}

SmallVector<OwnershipDiagInfo>
Ownership::OwnershipStatus::checkOPSDerefAssign(const VarDecl *VD,
                                               const SourceLocation &Loc) {
  SmallVector<OwnershipDiagInfo> diags;
  if (!is(VD, AllMoved)) {
    if (is(VD, Moved) || has(VD, Moved)) {
      OwnershipDiagInfo Info(Loc, InvalidUseOfMoved, VD->getNameAsString());
      diags.push_back(Info);
    } else if (const Type *Pointee =
                   VD->getType()->getPointeeType().getTypePtr()) {
      if (Pointee->isMoveSemanticType()) {
        if (has(VD, Owned) || is(VD, Owned)) {
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
    resetAll(VD);
    set(VD, Ownership::Status::Owned);
  }
  return diags;
}

SmallVector<OwnershipDiagInfo>
Ownership::OwnershipStatus::checkOPSFieldAssign(const VarDecl *VD,
                                                const SourceLocation &Loc,
                                                string fullFieldName) {
  SmallVector<OwnershipDiagInfo> diags;

  // when assign a field, we check three conditions:
  // 1. the field's parent must be owned
  // 2. the status of VD must not be moved or uninit
  // 3. the field and the field's subfields must be all moved

  // check condition 1
  int index = fullFieldName.length() - 2;
  string current = fullFieldName;
  while (index > 0) {
    if (current[index] == '*') {
      current = current.substr(0, index + 1);
      index--;
    } else {
      size_t pos = current.find_last_of('.');
      if (pos != string::npos) {
        current = current.substr(0, pos);
        index = pos;
      } else {
        break;
      }
    }
    if (OPSAllOwnedFields[VD].count(current) &&
        !OPSOwnedOwnedFields[VD].count(current)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidAssignFieldOfMoved,
          moveAsterisksToFront(VD->getNameAsString() + "." + current)));
      return diags;
    }
  }

  // check condition 2
  if ((is(VD, Moved) || has(VD, Moved)) && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignFieldOfMoved,
                          moveAsterisksToFront(VD->getNameAsString() + "." + fullFieldName)));
  }
  if ((is(VD, Uninitialized) || has(VD, Uninitialized)) && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignFieldOfUninit,
                          VD->getNameAsString()));
  }

  // check condition 3
  if (OPSAllOwnedFields[VD].count(fullFieldName) && OPSOwnedOwnedFields[VD].count(fullFieldName) && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignFieldOfOwned,
                          VD->getNameAsString() + "." + fullFieldName));
  }
  // calculate the fields with fullFieldName prefix
  llvm::SmallSet<string, 10> allPrefixStrs;
  if (fullFieldName[fullFieldName.size() - 1] == '.') {
     allPrefixStrs = findPrefixStrings(OPSAllOwnedFields[VD], fullFieldName);
  } else {
    allPrefixStrs = findPrefixStrings(OPSAllOwnedFields[VD], fullFieldName + ".");
  }
  auto allPrefixStrsStar =
      findPrefixStrings(OPSAllOwnedFields[VD], fullFieldName + "*");
  for (const auto &elem : allPrefixStrsStar) {
    allPrefixStrs.insert(elem);
  }
  llvm::SmallSet<string, 10> ownedPrefixStrs;
  if (fullFieldName[fullFieldName.size() - 1] == '.') {
    ownedPrefixStrs = findPrefixStrings(OPSOwnedOwnedFields[VD], fullFieldName);
  } else {
    ownedPrefixStrs = findPrefixStrings(OPSOwnedOwnedFields[VD], fullFieldName + ".");
  }
  auto ownedPrefixStrsStar =
      findPrefixStrings(OPSOwnedOwnedFields[VD], fullFieldName + "*");
  for (const auto &elem : ownedPrefixStrsStar) {
    ownedPrefixStrs.insert(elem);
  }
  if (!ownedPrefixStrs.empty() && diags.empty()) {
    diags.push_back(OwnershipDiagInfo(
        Loc, OwnershipDiagKind::InvalidAssignSubFieldOwned,
        VD->getNameAsString(), concatFields(VD, ownedPrefixStrs)));
  }
  if (!is(VD, Ownership::Status::Moved)) {
    if (OPSAllOwnedFields[VD].count(fullFieldName)) {
      OPSOwnedOwnedFields[VD].insert(fullFieldName);
    }
    // add allPrefixStrs to OPSOwnedOwnedFields
    for (const auto &str : allPrefixStrs) {
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
          !VD->getType()->getPointeeType()->isOwnedStructureType()) {
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
  if (VD->getType().getTypePtr()->isOwnedStructureType()) {
    if (!is(VD, Owned)) {
      if (is(VD, Uninitialized) || has(VD, Uninitialized)) {
        diags.push_back(OwnershipDiagInfo(
            Loc, OwnershipDiagKind::InvalidUseOfUninit, VD->getNameAsString()));
      } else if (is(VD, Moved) || has(VD, Moved)) {
        diags.push_back(OwnershipDiagInfo(
            Loc, OwnershipDiagKind::InvalidUseOfMoved, VD->getNameAsString()));
      }
    }
    if (!isGetAddr) {
      resetAll(VD);
      set(VD, Moved);
    }
  }

  if (SAllOwnedFields[VD].size() - SOwnedOwnedFields[VD].size() - SNullOwnedFields[VD].size() != 0 &&
      SAllOwnedFields[VD].size()!= SOwnedOwnedFields[VD].size() &&
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
  if (!isGetAddr)
    SOwnedOwnedFields[VD].clear();
  return diags;
}

SmallVector<OwnershipDiagInfo> Ownership::OwnershipStatus::checkSFieldUse(
    const VarDecl *VD, const SourceLocation &Loc, string fullFieldName,
    bool isGetAddr) {
  SmallVector<OwnershipDiagInfo> diags;

  // owned struct special manipulation
  if (VD->getType().getTypePtr()->isOwnedStructureType()) {
    if (is(VD, Uninitialized)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfUninit, VD->getNameAsString()));
    } else if (is(VD, Moved)) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfMoved, VD->getNameAsString()));
    }
  }

  if ((is(VD, Moved) || has(VD, Moved)) && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                          VD->getNameAsString() + "." + fullFieldName));
  }
  if (overlapsOwnedFields(SUninitOwnedFields[VD], fullFieldName) &&
      diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfUninit,
                          VD->getNameAsString() + "." + fullFieldName));
  }

  string movedFieldName =
      findMovedFieldKey(SAllOwnedFields[VD], SOwnedOwnedFields[VD],
                        &SNullOwnedFields[VD], fullFieldName);
  if (!movedFieldName.empty() && diags.empty()) {
    diags.push_back(
        OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                          VD->getNameAsString() + "." + movedFieldName));
  }
  // calculate the fields with fullFieldName prefix
  llvm::SmallSet<string, 10> allPrefixStrs;
  if (fullFieldName[fullFieldName.size() - 1] == '.') {
     allPrefixStrs = findPrefixStrings(SAllOwnedFields[VD], fullFieldName);
  } else {
    allPrefixStrs = findPrefixStrings(SAllOwnedFields[VD], fullFieldName + ".");
  }
  auto allPrefixStrsStar =
      findPrefixStrings(SAllOwnedFields[VD], fullFieldName + "*");
  for (const auto &elem : allPrefixStrsStar) {
    allPrefixStrs.insert(elem);
  }
  llvm::SmallSet<string, 10> ownedPrefixStrs;
  if (fullFieldName[fullFieldName.size() - 1] == '.') {
    ownedPrefixStrs = findPrefixStrings(SOwnedOwnedFields[VD], fullFieldName);
  } else {
    ownedPrefixStrs = findPrefixStrings(SOwnedOwnedFields[VD], fullFieldName + ".");
  }
  auto ownedPrefixStrsStar =
      findPrefixStrings(SOwnedOwnedFields[VD], fullFieldName + "*");
  for (const auto &elem : ownedPrefixStrsStar) {
    ownedPrefixStrs.insert(elem);
  }
  if (allPrefixStrs.size() != ownedPrefixStrs.size() && diags.empty()) {
    diags.push_back(OwnershipDiagInfo(
        Loc, OwnershipDiagKind::InvalidUseOfPartiallyMoved,
        VD->getNameAsString() + "." + fullFieldName, collectMovedFields(VD)));
  }
  if (!isGetAddr) {
    SOwnedOwnedFields[VD].erase(fullFieldName);
    for (const auto &str : ownedPrefixStrs) {
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
  if (VD->getType().getTypePtr()->isOwnedStructureType()) {
    resetAll(VD);
    set(VD, Owned);
  }
  // when assign a struct, all of its owned fields must be MOVED
  // owned struct does not abide with this rule.
  if (!VD->getType().getTypePtr()->isOwnedStructureType()) {
    if (!SOwnedOwnedFields[VD].empty() && diags.empty()) {
      if (SAllOwnedFields[VD].size() == SOwnedOwnedFields[VD].size()) {
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

  return diags;
}

SmallVector<OwnershipDiagInfo> Ownership::OwnershipStatus::checkSFieldAssign(
    const VarDecl *VD, const SourceLocation &Loc, string fullFieldName) {
  SmallVector<OwnershipDiagInfo> diags;

  // owned struct special manipulation
  if (VD->getType().getTypePtr()->isOwnedStructureType()) {
    if (!is(VD, Owned)) {
      if (is(VD, Uninitialized) || has(VD, Uninitialized)) {
        diags.push_back(OwnershipDiagInfo(
            Loc, OwnershipDiagKind::InvalidAssignFieldOfUninit,
            VD->getNameAsString()));
      } else if (is(VD, Moved) || has(VD, Moved)) {
        diags.push_back(
            OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignFieldOfMoved,
                              moveAsterisksToFront(VD->getNameAsString() + "." + fullFieldName)));
      }
    }
  }

  // when assign a field, we check two conditions:
  // 1. the field's parent must be owned
  // 2. the field and the field's subfields must be all moved

  // check condition 1
  int index = fullFieldName.length() - 2;
  string current = fullFieldName;
  while (index > 0) {
    if (current[index] == '*') {
      current = current.substr(0, index + 1);
      index--;
    } else {
      size_t pos = current.find_last_of('.');
      if (pos != string::npos) {
        current = current.substr(0, pos);
        index = pos;
      } else {
        break;
      }
    }
    if (SAllOwnedFields[VD].count(current) &&
        !SOwnedOwnedFields[VD].count(current) && diags.empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidAssignFieldOfMoved,
          moveAsterisksToFront(VD->getNameAsString() + "." + current)));
      return diags;
    }
  }

  // check condition 2
  if (SAllOwnedFields[VD].count(fullFieldName) && SOwnedOwnedFields[VD].count(fullFieldName) && diags.empty()) {
    diags.push_back(OwnershipDiagInfo(
        Loc, OwnershipDiagKind::InvalidAssignFieldOfOwned,
        moveAsterisksToFront(VD->getNameAsString() + "." + fullFieldName)));
  }
  // calculate the fields with fullFieldName prefix
  llvm::SmallSet<string, 10> allPrefixStrs;
  if (fullFieldName[fullFieldName.size() - 1] == '.') {
     allPrefixStrs = findPrefixStrings(SAllOwnedFields[VD], fullFieldName);
  } else {
    allPrefixStrs = findPrefixStrings(SAllOwnedFields[VD], fullFieldName + ".");
  }
  auto allPrefixStrsStar =
      findPrefixStrings(SAllOwnedFields[VD], fullFieldName + "*");
  for (const auto &elem : allPrefixStrsStar) {
    allPrefixStrs.insert(elem);
  }
  llvm::SmallSet<string, 10> ownedPrefixStrs;
  if (fullFieldName[fullFieldName.size() - 1] == '.') {
    ownedPrefixStrs = findPrefixStrings(SOwnedOwnedFields[VD], fullFieldName);
  } else {
    ownedPrefixStrs = findPrefixStrings(SOwnedOwnedFields[VD], fullFieldName + ".");
  }
  auto ownedPrefixStrsStar =
      findPrefixStrings(SOwnedOwnedFields[VD], fullFieldName + "*");
  for (const auto &elem : ownedPrefixStrsStar) {
    ownedPrefixStrs.insert(elem);
  }
  if (!ownedPrefixStrs.empty() && diags.empty()) {
    diags.push_back(OwnershipDiagInfo(
        Loc, OwnershipDiagKind::InvalidAssignSubFieldOwned,
        VD->getNameAsString(), concatFields(VD, ownedPrefixStrs)));
  }
  // change the status of the fields
  if (SAllOwnedFields[VD].count(fullFieldName))
    SOwnedOwnedFields[VD].insert(fullFieldName);
  // add allPrefixStrs to SOwnedOwnedFields
  for (const auto &str : allPrefixStrs) {
    SOwnedOwnedFields[VD].insert(str);
  }
  SNullOwnedFields[VD].erase(fullFieldName);
  SUninitOwnedFields[VD].erase(fullFieldName);
  for (const auto &str : allPrefixStrs) {
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
    if (has(VD, Ownership::Status::Moved) || is(VD, Ownership::Status::Moved)) {
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
    const VarDecl *VD, const SourceLocation &Loc, string fullFieldName,
    bool isGetAddr) {
  SmallVector<OwnershipDiagInfo> diags;

  // check field's parent
  for (int i = fullFieldName.length() - 2; i >= 0; i--) {
    string current = fullFieldName.substr(0, i + 1);
    if (BOPAllOwnedFields[VD].count(current) &&
        !BOPOwnedOwnedFields[VD].count(current)) {
      diags.push_back(OwnershipDiagInfo(Loc,
                                        OwnershipDiagKind::InvalidUseOfMoved,
                                        fullFieldName + VD->getNameAsString()));
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
  if ((is(VD, Ownership::Status::Moved) || has(VD, Ownership::Status::Moved)) &&
      diags.empty()) {
    diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                                      VD->getNameAsString()));
  }

  // if the field is owned qualified, check the field and its child
  if (BOPAllOwnedFields[VD].count(fullFieldName)) {
    // if fullFieldName has been moved, report error
    if (!BOPOwnedOwnedFields[VD].count(fullFieldName) && diags.empty()) {
      diags.push_back(OwnershipDiagInfo(Loc,
                                        OwnershipDiagKind::InvalidUseOfMoved,
                                        fullFieldName + VD->getNameAsString()));
    }
    // calculate the fields with fullFieldName prefix
    auto allPrefixStrs =
        findPrefixStrings(BOPAllOwnedFields[VD], fullFieldName);
    auto ownedPrefixStrs =
        findPrefixStrings(BOPOwnedOwnedFields[VD], fullFieldName);
    if (allPrefixStrs.size() != ownedPrefixStrs.size() && diags.empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidUseOfPartiallyMoved,
          fullFieldName + VD->getNameAsString(), collectMovedFields(VD)));
    }
    if (!isGetAddr) {
      // change the status of the fields
      BOPOwnedOwnedFields[VD].erase(fullFieldName);
      // remove ownedPrefixStrs from BOPOwnedOwnedFields
      for (const auto &str : ownedPrefixStrs) {
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
    if (has(VD, Ownership::Status::Owned) || is(VD, Ownership::Status::Owned)) {
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
                                                string fullFieldName) {
  SmallVector<OwnershipDiagInfo> diags;

  // for a non owned qualified field, just check its parent
  // for a owned qualified field, we check three conditions:
  // // 1. the status of VD must be PartiallyMoved or AllMoved
  // // 2. the field's parents must be not moved
  // // 3. the field and the field's subfields must be all moved

  // the parent field must be owned
  for (int i = fullFieldName.length() - 2; i >= 0; i--) {
    string current = fullFieldName.substr(0, i + 1);
    if (BOPAllOwnedFields[VD].count(current) &&
        !BOPOwnedOwnedFields[VD].count(current)) {
      diags.push_back(
          OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignFieldOfMoved,
                            fullFieldName + VD->getNameAsString()));
      break;
    }
  }
  if ((is(VD, Moved) || has(VD, Moved) || is(VD, Uninitialized) ||
       has(VD, Uninitialized)) &&
      diags.empty()) {
    diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                                      VD->getNameAsString()));
  }

  if (BOPAllOwnedFields[VD].count(fullFieldName)) {
    // check condition 3
    if (BOPOwnedOwnedFields[VD].count(fullFieldName) && diags.empty()) {
      diags.push_back(
          OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidAssignFieldOfOwned,
                            VD->getNameAsString() + fullFieldName));
    }
    // calculate the fields with fullFieldName prefix
    auto allPrefixStrs =
        findPrefixStrings(BOPAllOwnedFields[VD], fullFieldName);
    auto ownedPrefixStrs =
        findPrefixStrings(BOPOwnedOwnedFields[VD], fullFieldName);
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
      for (const auto &str : allPrefixStrs) {
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

  if (has(VD, Ownership::Status::Moved) || is(VD, Ownership::Status::Moved)) {
    diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidCastMoved,
                                      VD->getNameAsString()));
  }
  if (has(VD, Uninitialized) || is(VD, Uninitialized)) {
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
  if ((has(VD, Ownership::Status::Moved) || is(VD, Ownership::Status::Moved)) &&
      !is(VD, Ownership::Status::AllMoved)) {
    diags.push_back(OwnershipDiagInfo(Loc, OwnershipDiagKind::InvalidCastMoved,
                                      VD->getNameAsString()));
  }
  if (has(VD, Uninitialized) || is(VD, Uninitialized)) {
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
    const VarDecl *VD, const SourceLocation &Loc, string fullFieldName) {
  SmallVector<OwnershipDiagInfo> diags;

  if (OPSStatus.count(VD)) {
    if (!OPSOwnedOwnedFields[VD].count(fullFieldName) && diags.empty()) {
      OwnershipDiagKind Kind = is(VD, Uninitialized) || has(VD, Uninitialized)
                                   ? InvalidCastUninit
                                   : InvalidCastMoved;
      diags.push_back(OwnershipDiagInfo(
          Loc, Kind,
          moveAsterisksToFront(VD->getNameAsString() + "." + fullFieldName)));
    }
    // calculate the fields with fullFieldName prefix
    auto allPrefixStrs =
        findPrefixStrings(OPSAllOwnedFields[VD], fullFieldName + ".");
    auto ownedPrefixStrs =
        findPrefixStrings(OPSOwnedOwnedFields[VD], fullFieldName + ".");
    if (!ownedPrefixStrs.empty() && diags.empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidCastFieldOwned,
          moveAsterisksToFront(VD->getNameAsString() + "." + fullFieldName),
          concatFields(VD, ownedPrefixStrs)));
    }
    if (OPSAllOwnedFields[VD].count(fullFieldName)) {
      OPSOwnedOwnedFields[VD].erase(fullFieldName);
    }
    // remove allPrefixStrs from OPSOwnedOwnedFields
    for (const auto &str : ownedPrefixStrs) {
      OPSOwnedOwnedFields[VD].erase(str);
    }
    if (OPSAllOwnedFields[VD].size() != OPSOwnedOwnedFields[VD].size()) {
      if (!is(VD, Ownership::Status::Moved)) {
        resetAll(VD);
        set(VD, Ownership::Status::PartialMoved);
      }
    }
    if (OPSOwnedOwnedFields[VD].empty() && !VD->getType()->getPointeeType()->isOwnedStructureType()) {
      if (!is(VD, Ownership::Status::Moved)) {
        resetAll(VD);
        set(VD, Ownership::Status::AllMoved);
      }
    }
  }

  if (SStatus.count(VD)) {
    // A `[]`-suffixed path is an array-field element (e.g. w.arr[].a):
    // field-level tracking treats the whole array as one aggregate, so
    // element-granularity moved detection cannot distinguish arr[0].a from
    // arr[1].a; skip the error and keep erasing the aggregate field.
    bool ArrayFieldPath = fullFieldName.find("[]") != std::string::npos;
    if (!SOwnedOwnedFields[VD].count(fullFieldName) && !ArrayFieldPath) {
      OwnershipDiagKind Kind = is(VD, Uninitialized) || has(VD, Uninitialized)
                                   ? InvalidCastUninit
                                   : InvalidCastMoved;
      diags.push_back(OwnershipDiagInfo(
          Loc, Kind,
          moveAsterisksToFront(VD->getNameAsString() + "." + fullFieldName)));
    }
    // calculate the fields with fullFieldName prefix
    auto allPrefixStrs =
        findPrefixStrings(SAllOwnedFields[VD], fullFieldName + ".");
    auto ownedPrefixStrs =
        findPrefixStrings(SOwnedOwnedFields[VD], fullFieldName + ".");
    if (ownedPrefixStrs.size() != 0 && diags.empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidCastFieldOwned,
          moveAsterisksToFront(VD->getNameAsString() + "." + fullFieldName),
          concatFields(VD, ownedPrefixStrs)));
    }
    SOwnedOwnedFields[VD].erase(fullFieldName);
    for (const auto &str : ownedPrefixStrs) {
      SOwnedOwnedFields[VD].erase(str);
    }
  }

  if (BOPStatus.count(VD)) {
    if (!BOPOwnedOwnedFields[VD].count(fullFieldName) && diags.empty()) {
      OwnershipDiagKind Kind = is(VD, Uninitialized) || has(VD, Uninitialized)
                                   ? InvalidCastUninit
                                   : InvalidCastMoved;
      diags.push_back(OwnershipDiagInfo(
          Loc, Kind,
          moveAsterisksToFront(VD->getNameAsString() + "." + fullFieldName)));
    }
    // Check for deeper-level owned fields (e.g., when freeing **, check if *** exists)
    auto ownedPrefixStrs = findPrefixStrings(BOPOwnedOwnedFields[VD], fullFieldName + "*");
    if (!ownedPrefixStrs.empty() && diags.empty()) {
      diags.push_back(OwnershipDiagInfo(
          Loc, OwnershipDiagKind::InvalidCastFieldOwned,
          moveAsterisksToFront(VD->getNameAsString() + "." + fullFieldName),
          concatFields(VD, ownedPrefixStrs)));
    }
    BOPOwnedOwnedFields[VD].erase(fullFieldName);
    // Remove deeper-level fields
    for (const auto &str : ownedPrefixStrs) {
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
    if (!VD->getType().getCanonicalType()->isOwnedStructureType()) {
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
// recursively check that an initializer list initializes every
// element/field with a null (or omitted / zero) value, so the whole array can
// be treated as null-initialized.
// True when an initializer of type `Ty` leaves every _Owned pointer (or
// move-semantic sub-object) null: implicit init, an explicit null, or (for a
// struct) every _Owned field owned-null. Plain (non-owned) fields may be
// non-zero without implying ownership.
static bool isOwnedNullInit(ASTContext &ctx, const Expr *Init, QualType Ty) {
  if (!Init)
    return true;
  Init = Init->IgnoreParenImpCasts();
  if (isa<ImplicitValueInitExpr>(Init))
    return true;
  if (IsNullExpr(ctx, Init))
    return true;
  const auto *ILE = dyn_cast<InitListExpr>(Init);
  if (!ILE)
    return false;
  Ty = Ty.getCanonicalType();
  if (const RecordType *RT = Ty->getAs<RecordType>()) {
    for (const FieldDecl *FD : RT->getDecl()->fields()) {
      QualType FT = FD->getType();
      if (!FT.isOwnedQualified() && !FT->hasOwnedFields())
        continue; // plain field: ignore
      const Expr *FieldInit = FD->getFieldIndex() < ILE->getNumInits()
                                  ? ILE->getInit(FD->getFieldIndex())
                                  : nullptr;
      if (!isOwnedNullInit(ctx, FieldInit, FT))
        return false;
    }
    return true;
  }
  // Non-struct element (owned pointer / nested array): every entry null.
  for (unsigned I = 0; I < ILE->getNumInits(); ++I)
    if (!isOwnedNullInit(ctx, ILE->getInit(I), Ty))
      return false;
  return true;
}

// True when every element of an owned-element array initializer owns nothing
// (all elements owned-null).
static bool isAllNullInits(ASTContext &ctx, const InitListExpr *ILE,
                           QualType ElemType) {
  for (unsigned I = 0; I < ILE->getNumInits(); ++I)
    if (!isOwnedNullInit(ctx, ILE->getInit(I), ElemType))
      return false;
  return true;
}

class TransferFunctions : public StmtVisitor<TransferFunctions> {
  OwnershipImpl &OS;
  Ownership::OwnershipStatus &stat;
  OwnershipDiagReporter &reporter;
  bool isHandlingCallExpr = false;
  bool isAddrMut = false;
  // The RHS of the assignment currently being visited (nullptr check for
  // "assign to null" which must not be reported as overwriting an owned
  // value).
  const Expr *CurRHS = nullptr;

  enum Operation { None, Assign, Move, GetAddr };
  Operation op = Operation::None;

public:
  TransferFunctions(OwnershipImpl &os, Ownership::OwnershipStatus &Stat,
                    OwnershipDiagReporter &reporter)
      : OS(os), stat(Stat), reporter(reporter) {}

  void VisitArraySubscriptExpr(ArraySubscriptExpr *ASE);
  void VisitBinaryOperator(BinaryOperator *BO);
  void VisitCompoundAssignOperator(CompoundAssignOperator *CAO);
  void VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *UE);
  void VisitCallExpr(CallExpr *CE);
  void VisitCStyleCastExpr(CStyleCastExpr *CSCE);
  void VisitDeclRefExpr(DeclRefExpr *DRE);
  void VisitDeclRefExpr(const DeclRefExpr *DRE, std::string fieldName);
  void VisitDeclStmt(DeclStmt *DS);
  void VisitInitListExpr(InitListExpr *ILE);
  void VisitMemberExpr(MemberExpr *ME);
  void VisitReturnStmt(ReturnStmt *RS);
  void VisitLifetimeEnds(VarDecl *VD, SourceLocation SL, bool isDestructor);
  void VisitStmt(Stmt *S);
  void VisitUnaryOperator(UnaryOperator *UO);
  void VisitAbstractConditionalOperator(AbstractConditionalOperator *ACO);

  void HandleInitListExpr(VarDecl *VD, RecordDecl *RD, InitListExpr *ILE, std::string fullFieldName = "");
  void HandleDREAssign(const DeclRefExpr *DRE, std::string fullFieldName = "");
  void HandleDREUse(const DeclRefExpr *DRE, std::string fullFieldName = "");

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
      pair<const Expr *, string> memberField = getMemberFullField(ME);
      // Peel a leading deref so `(*p).f` behaves like `p->f`.
      if (const DeclRefExpr *DRE =
              getRootDREFromMemberBase(memberField.first)) {
        if (const VarDecl *SrcVD = dyn_cast<VarDecl>(DRE->getDecl())) {
          // Struct value whose field is tracked as must-be-null.
          if (stat.SStatus.count(SrcVD) &&
              stat.SNullOwnedFields[SrcVD].count(memberField.second))
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
  // Apply the dataflow transition for a[i] / s[i].f transfers. Move-out of an
  // already-moved aggregate / field reports use-after-move.
  void applyArrayElemTransition(SourceLocation Loc, const VarDecl *ArrVD,
                                const std::string &fieldName, bool IsAssign);
  void applyArrayElemAssign(SourceLocation Loc, const VarDecl *ArrVD,
                            const std::string &fieldName);
  void applyArrayElemMoveOut(SourceLocation Loc, const VarDecl *ArrVD,
                             const std::string &fieldName);
  // Check a field-array transfer site (w.arr[i] / w.arr[i].a) against the
  // qualifying-loop allow-sites; apply the transition when allowed and
  // return whether it was performed.
  bool tryTransferArrayField(const Expr *Site, const VarDecl *HostVD,
                             const std::string &FieldPath, bool IsAssign);
  // Record a transfer-forbidden diagnostic for ArrVD at Loc.
  void emitArrayElemForbidden(SourceLocation Loc, const VarDecl *ArrVD);

  void SetHandlingCallExpr() {
    isHandlingCallExpr = true;
  }
};
} // namespace

void TransferFunctions::VisitStmt(Stmt *S) {
  for (auto *C : S->children()) {
    if (C) {
      Visit(C);
    }
  }
}

void TransferFunctions::VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
  // Owned-element stack arrays and _Owned _ArrayElem bases:
  // element ownership transfer is only allowed inside a qualifying for-loop,
  // and the transition is applied to the whole-array aggregate state here.
  bool IsArrElemPtr = false;
  const VarDecl *ArrVD = peelArrayBase(ASE, IsArrElemPtr);
  if (ArrVD && IsArrayTransferBase(ArrVD, IsArrElemPtr)) {
    if (op == Move || op == Assign) {
      // A null aggregate releases nothing (free(null) is a no-op): no
      // qualifying-loop site is needed for a null element move-out.
      if (op == Move && stat.arrayAggregateNull(ArrVD, "")) {
        Visit(ASE->getIdx());
        return;
      }
      bool Allowed = isArrayElemTransferAllowed(ASE, ArrVD, IsArrElemPtr);
      if (!Allowed) {
        Visit(ASE->getIdx());
        return;
      }
      applyArrayElemTransition(ASE->getExprLoc(), ArrVD, "", op == Assign);
    } else if (op == GetAddr) {
      // Borrowing (mutable or immutable) a _Owned array that has lost its
      // ownership is forbidden: the for-loop may already have moved every
      // element out (manual: cannot borrow a moved _Owned value).
      bool Moved = false;
      if (stat.BOPStatus.count(ArrVD))
        Moved = stat.is(ArrVD, Ownership::Status::Moved);
      else if (stat.SStatus.count(ArrVD))
        Moved = stat.SOwnedOwnedFields[ArrVD].empty();
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
  // so peelArrayBase above cannot see it). PeelHostAndFieldPath yields the
  // host and the array-field path "arr[]"; the field is tracked as an owned
  // array aggregate of the host, same rules as a local owned-element array.
  {
    const VarDecl *HostVD = nullptr;
    std::string FieldPath;
    if (GetOwnedArrayField(ASE, HostVD, FieldPath)) {
      if (op == Move || op == Assign) {
        if (!tryTransferArrayField(ASE, HostVD, FieldPath, op == Assign)) {
          Visit(ASE->getIdx());
          return;
        }
      } else if (op == GetAddr) {
          // Borrowing (mutable or immutable) an array field whose aggregate
          // has lost its ownership is forbidden (mirror of the local-array
          // rule): the qualifying loop may already have moved every element
          // out.
          bool Moved = false;
          if (stat.SStatus.count(HostVD))
            Moved = !stat.SOwnedOwnedFields[HostVD].count(FieldPath);
          if (Moved) {
            OwnershipDiagInfo DI(ASE->getExprLoc(),
                                 OwnershipDiagKind::InvalidUseOfMoved,
                                 HostVD->getNameAsString());
            reporter.addDiagInfo(DI);
          } else if (isAddrMut) {
            // A mutable borrow (&_Mut w.arr[i]) exposes the field to writes
            // through the borrow, so promote the aggregate to owned.
            stat.setArrayFieldOwned(HostVD, FieldPath);
          }
        }
        Visit(ASE->getIdx());
        return;
      }
  }

  string suffix;
  Expr *E = ASE;
  while (ArraySubscriptExpr *ase = dyn_cast<ArraySubscriptExpr>(E)) {
    suffix += "*";
    E = ase->getBase()->IgnoreParenImpCasts();
    ASE = ase;
  }

  if (MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
    auto memberField = getMemberFullField(ME);
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(memberField.first)) {
      VisitDeclRefExpr(DRE, memberField.second + suffix);
      return;
    }
  }
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    VisitDeclRefExpr(DRE, suffix);
    return;
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
  // Inside a qualifying loop, transfers recorded in AllowedTransferExprs are
  // permitted; for nested subscripts any enclosing subscript in the chain may
  // have been recorded (e.g. the outer loop records a[i][j]).
  bool Allowed = OS.LoopInfo.AllowedTransferExprs.count(Site);
  if (!Allowed) {
    for (const Expr *E = Site; E;) {
      if (OS.LoopInfo.AllowedTransferExprs.count(E)) {
        Allowed = true;
        break;
      }
      if (const ArraySubscriptExpr *A = dyn_cast<ArraySubscriptExpr>(E))
        E = A->getBase()->IgnoreParenImpCasts();
      else
        break;
    }
  }
  if (!Allowed)
    emitArrayElemForbidden(Site->getExprLoc(), ArrVD);
  return Allowed;
}

// True when the owned field `fieldName` of the struct-array `ArrVD` is itself
// an array type. Field-level tracking treats such a field as one aggregate, so
// element-granularity use-after-move cannot be distinguished here; it is
// skipped (element tracking is a future refinement).
static bool isArrayField(const VarDecl *ArrVD, const std::string &fieldName) {
  QualType Ty = ArrVD->getType();
  while (const auto *AT = Ty->getAsArrayTypeUnsafe())
    Ty = AT->getElementType();
  // An owned struct-pointer host (`struct S *_Owned p`) tracks the pointee's
  // fields like a local struct-array host (mirror of IsOwnedArrayField).
  if (Ty->isPointerType())
    Ty = Ty->getPointeeType();
  const RecordType *RT = Ty->getAs<RecordType>();
  if (!RT)
    return false;
  for (const FieldDecl *FD : RT->getDecl()->fields())
    if (FD->getNameAsString() == fieldName)
      return FD->getType()->isArrayType();
  return false;
}

void TransferFunctions::applyArrayElemTransition(SourceLocation Loc,
                                                  const VarDecl *ArrVD,
                                                  const std::string &fieldName,
                                                  bool IsAssign) {
  if (IsAssign)
    applyArrayElemAssign(Loc, ArrVD, fieldName);
  else
    applyArrayElemMoveOut(Loc, ArrVD, fieldName);
}

bool TransferFunctions::tryTransferArrayField(const Expr *Site,
                                              const VarDecl *HostVD,
                                              const std::string &FieldPath,
                                              bool IsAssign) {
  // A null element releases nothing (free(null) is a no-op): no transfer is
  // needed and the site does not need to qualify -- e.g. the null branch of a
  // `if (!s[i].arr[i]) safe_free(...)`, whose field index may share the host
  // index variable (s[i].arr[i]) and would otherwise be rejected as a
  // partial diagonal release.
  if (!IsAssign && stat.arrayAggregateNull(HostVD, FieldPath))
    return true;
  if (!isArrayElemTransferAllowed(Site, HostVD, /*IsArrElemPtr=*/false))
    return false;
  applyArrayElemTransition(Site->getExprLoc(), HostVD, FieldPath, IsAssign);
  return true;
}

void TransferFunctions::applyArrayElemAssign(SourceLocation Loc,
                                             const VarDecl *ArrVD,
                                             const std::string &fieldName) {
  // Assigning to an element of an aggregate that still holds ownership
  // silently leaks the previous value; report it (mirror of the scalar
  // "cannot assign to owned value" rule). This covers both a null clear
  // (`= nullptr` discards the owned value) and a non-null assignment. Only
  // an exactly-owned (already initialized) aggregate triggers this: a
  // possibly-uninitialized aggregate reached through a loop back edge must
  // not.
    bool IsNullAssign = CurRHS && CurRHS->isNullExpr(OS.ctx);
    bool HoldsOwned = false;
    if (fieldName.empty()) {
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
          fieldName.empty() ? ArrVD->getNameAsString() + "[]" : fieldName;
      OwnershipDiagInfo DI(Loc, OwnershipDiagKind::ArrayElemAssignOwned,
                           Display);
      reporter.addDiagInfo(DI);
    }
  if (IsNullAssign) {
    if (fieldName.empty())
      stat.setArrayElemNull(ArrVD);
    else
      stat.setArrayFieldNull(ArrVD, fieldName);
  } else {
    if (fieldName.empty())
      stat.setArrayElemOwned(ArrVD);
    else
      stat.setArrayFieldOwned(ArrVD, fieldName);
  }
}

void TransferFunctions::applyArrayElemMoveOut(SourceLocation Loc,
                                              const VarDecl *ArrVD,
                                              const std::string &fieldName) {
  // Move-out: detect use-after-move. With the qualifying-loop header no longer
  // merging the back-edge, the aggregate reaches this point with the clean
  // pre-loop state, so a second move-out of the same aggregate / field is a
  // real use-after-move.
  bool AlreadyMoved = false;
  if (fieldName.empty()) {
    AlreadyMoved = stat.arrayAggregateMoved(ArrVD);
  } else {
    // A pure array-field path ("arr[]" -- an _Owned pointer element array
    // stored as a struct member) tracks element ownership, so a second
    // move-out is a real use-after-move. Only *nested* array-field paths
    // ("arr[].a" -- a member of a struct-element array) are aggregates with
    // no element granularity and are exempt.
    bool PureArrayField =
        fieldName.find("[]") != std::string::npos &&
        fieldName.find("[]") == fieldName.size() - 2;
    if (stat.SStatus.count(ArrVD))
      AlreadyMoved = !stat.SOwnedOwnedFields[ArrVD].count(fieldName) &&
                     !stat.SNullOwnedFields[ArrVD].count(fieldName) &&
                     !isArrayField(ArrVD, fieldName) &&
                     (PureArrayField ||
                      fieldName.find("[]") == std::string::npos);
    else if (stat.OPSStatus.count(ArrVD))
      AlreadyMoved = !stat.OPSOwnedOwnedFields[ArrVD].count(fieldName) &&
                     !stat.OPSNullOwnedFields[ArrVD].count(fieldName) &&
                     !isArrayField(ArrVD, fieldName) &&
                     (PureArrayField ||
                      fieldName.find("[]") == std::string::npos);
  }
  // A move-out of an uninitialized aggregate / field is a use of an
  // uninitialized value (mirror of the scalar cast-uninit check), not a
  // use-after-move. Use `is` (bit test), not `has` -- `has` means "some
  // *other* bit is set besides this one", which is false for a state that is
  // exactly Uninitialized.
  bool Uninit = stat.arrayAggregateUninit(ArrVD);
  if (Uninit) {
    std::string Display =
        fieldName.empty() ? ArrVD->getNameAsString() + "[]" : fieldName;
    OwnershipDiagInfo DI(Loc, OwnershipDiagKind::InvalidCastUninit, Display);
    reporter.addDiagInfo(DI);
    return;
  }
  if (AlreadyMoved) {
    OwnershipDiagInfo DI(Loc, OwnershipDiagKind::InvalidUseOfMoved,
                         ArrVD->getNameAsString());
    reporter.addDiagInfo(DI);
    return;
  }
  // A must-be-null aggregate (all elements initialized to null) owns nothing:
  // moving a null element out does not consume ownership (same rule as for
  // scalar _Owned pointers, 87dbd70f), so the aggregate stays Null instead of
  // becoming Moved. This allows e.g. consume(a[i]) on an all-null array to be
  // repeated, and a later borrow sees Null (may own) rather than Moved.
  bool NullTransfer = stat.arrayAggregateNull(ArrVD, fieldName);
  if (!NullTransfer) {
    if (fieldName.empty())
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
    return "loop body must not return or goto early";
  case NonQualifyingLoopReason::BoundMismatch:
    return "loop bound must equal the array length";
  case NonQualifyingLoopReason::ElemAccessMismatch:
    return "array accesses in the body must use the loop index (a[i])";
  case NonQualifyingLoopReason::PathInconsistent:
    return "if/else branches must transfer the element the same way";
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

void TransferFunctions::VisitMemberExpr(MemberExpr *ME) {
  auto memberField = getMemberFullField(ME);

  // Struct-array member access: s[i].f / s[i].a[j]. Ownership transfer of the
  // member is only allowed inside a qualifying loop and updates the field's
  // aggregate state.
  bool IsArrElemPtr = false;
  const VarDecl *ArrVD = peelArrayBase(ME, IsArrElemPtr);
  if (ArrVD &&
      IsArrayTransferBase(ArrVD, IsArrElemPtr)) {
    if (op == Move || op == Assign) {
      // Null member releases nothing (free(null) is a no-op).
      if (op == Move && stat.arrayAggregateNull(ArrVD, memberField.second))
        return;
      bool Allowed = isArrayElemTransferAllowed(ME, ArrVD, IsArrElemPtr);
      if (!Allowed)
        return;
      applyArrayElemTransition(ME->getExprLoc(), ArrVD, memberField.second, op == Assign);
    }
    return;
  }

  // Nested struct-array field (w.arr[i].a): array-field paths follow the
  // qualifying-loop rules like ordinary arrays.
  const VarDecl *HostVD = nullptr;
  std::string FieldPath;
  if (PeelHostAndFieldPath(ME, HostVD, FieldPath) &&
      IsOwnedArrayFieldPath(HostVD, FieldPath)) {
    if (op == Move || op == Assign) {
      tryTransferArrayField(ME, HostVD, FieldPath, op == Assign);
    } else if (op == GetAddr) {
      // Same borrow rule as the local array: borrowing an array-field path
      // whose aggregate has lost ownership is forbidden.
      bool Moved = false;
      if (stat.SStatus.count(HostVD))
        Moved = !stat.SOwnedOwnedFields[HostVD].count(FieldPath);
      if (Moved) {
        OwnershipDiagInfo DI(ME->getExprLoc(),
                             OwnershipDiagKind::InvalidUseOfMoved,
                             HostVD->getNameAsString());
        reporter.addDiagInfo(DI);
      } else if (isAddrMut) {
        stat.setArrayFieldOwned(HostVD, FieldPath);
      }
    }
    return;
  }

  // manipulate struct member expr assign
  if (op == Assign) {
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(memberField.first)) {
      HandleDREAssign(DRE, memberField.second);
    }
  }

  // manipulate struct member expr use
  if (op == Move || op == GetAddr) {
    // Peel a leading deref so `(*p).f` behaves like `p->f`.
    if (const DeclRefExpr *DRE = getRootDREFromMemberBase(memberField.first))
      HandleDREUse(DRE, memberField.second);
  }
}

void TransferFunctions::VisitDeclRefExpr(DeclRefExpr *DRE) {
  if (op == Assign) {
    HandleDREAssign(DRE, "");
  }

  if (op == Move || op == GetAddr) {
    HandleDREUse(DRE, "");
  }
}

void TransferFunctions::VisitDeclRefExpr(const DeclRefExpr *DRE,
                                         string fieldName) {
  if (op == Assign) {
    HandleDREAssign(DRE, fieldName);
  }

  if (op == Move || op == GetAddr) {
    HandleDREUse(DRE, fieldName);
  }
}

void TransferFunctions::VisitUnaryOperator(UnaryOperator *UO) {
  string suffix;
  Expr *E = UO;
  while (UO->getOpcode() == UO_Deref) {
    if (ImplicitCastExpr *ICE = dyn_cast<ImplicitCastExpr>(E)) {
      E = ICE->getSubExpr();
    } else if (UnaryOperator *uo = dyn_cast<UnaryOperator>(E)) {
      UO = uo;
      suffix += "*";
      E = UO->getSubExpr();
    } else {
      break;
    }
    E = E->IgnoreParens();
  }

  if (MemberExpr *ME = dyn_cast<MemberExpr>(E)) {
    auto memberField = getMemberFullField(ME);
    if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(memberField.first)) {
      VisitDeclRefExpr(DRE, memberField.second + suffix);
      return;
    }
  }
  if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(E)) {
    VisitDeclRefExpr(DRE, suffix);
    return;
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
    op = Assign;
  }
  Visit(UO->getSubExpr());
  op = None;
}

void TransferFunctions::VisitBinaryOperator(BinaryOperator *BO) {
  if (BO->isAssignmentOp()) {
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
      stat.setToNull(LHS);
    } else if (IsCastFromVoidPointer(RHS)) {
      stat.setToAllMoved(LHS);
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
    }
    Visit(BO->getLHS());
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

  op = None;
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

void TransferFunctions::VisitCallExpr(CallExpr *CE) {
  if (!isHandlingCallExpr && !OS.ArgCalls.count(CE))
    return;

  isHandlingCallExpr = false;

  for (auto it = CE->arg_begin(), ei = CE->arg_end(); it != ei; ++it) {
    Expr *Arg = *it;
    if (IsCastFromVoidPointer(Arg) && Arg->getType()->hasOwnedFields()) {
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
        const VarDecl *HostVD = nullptr;
        std::string FieldPath;
        if (PeelHostAndFieldPath(ArgCore, HostVD, FieldPath) &&
            !FieldPath.empty())
          ArrVD = HostVD;
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
      CSCE->getType().isOwnedQualified()) {

    // ignore explicit/implicit casts, get canonical expr
    const Expr *InnerE = CSCE->getSubExpr()->IgnoreParenCastsSafe();
    // The value of a comma expression is its RHS; keep peeling nested comma RHS.
    while (const BinaryOperator *BO = dyn_cast<BinaryOperator>(InnerE)) {
      if (BO->getOpcode() != BO_Comma)
        break;
      InnerE = BO->getRHS()->IgnoreParenCastsSafe();
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
      auto memberField = getMemberFullField(ME);
      // Struct-array member move-out: (void *_Owned)s[i].f.
      bool IsArrElemPtr = false;
      const VarDecl *ArrVD = peelArrayBase(ME, IsArrElemPtr);
      if (ArrVD &&
          IsArrayTransferBase(ArrVD, IsArrElemPtr)) {
        // Null member releases nothing (free(null) is a no-op).
        if (!stat.arrayAggregateNull(ArrVD, memberField.second)) {
          bool Allowed = isArrayElemTransferAllowed(ME, ArrVD, IsArrElemPtr);
          if (Allowed)
            applyArrayElemTransition(CSCE->getExprLoc(), ArrVD,
                                     memberField.second, /*IsAssign=*/false);
        }
        return;
      }
      // Nested struct-array field (e.g. (void *_Owned)w.arr[0].a): peel the
      // host variable and the "arr[].a" path. Array-field paths (containing
      // `[]`) follow the qualifying-loop rules like ordinary arrays; plain
      // nested fields use the regular field move-out.
      const VarDecl *HostVD = nullptr;
      std::string FieldPath;
      if (PeelHostAndFieldPath(ME, HostVD, FieldPath)) {
        if (IsOwnedArrayFieldPath(HostVD, FieldPath)) {
          tryTransferArrayField(ME, HostVD, FieldPath, /*IsAssign=*/false);
        } else {
          SmallVector<OwnershipDiagInfo> diags =
              stat.checkCastField(HostVD, ME->getExprLoc(), FieldPath);
          reporter.addDiags(diags);
        }
        return;
      }
      if (const DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(memberField.first)) {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkCastField(dyn_cast<VarDecl>(DRE->getDecl()),
                                DRE->getLocation(), memberField.second);
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
        if (!stat.arrayAggregateNull(ArrVD, "")) {
          bool Allowed = isArrayElemTransferAllowed(ASE, ArrVD, IsArrElemPtr);
          if (Allowed)
            applyArrayElemTransition(CSCE->getExprLoc(), ArrVD, "",
                                     /*IsAssign=*/false);
        }
        return;
      }
      // Struct-field owned-element array: (void *_Owned)w.arr[i].
      {
        const VarDecl *HostVD = nullptr;
        std::string FieldPath;
        if (GetOwnedArrayField(ASE, HostVD, FieldPath)) {
          if (!stat.arrayAggregateNull(HostVD, FieldPath)) {
            bool Allowed = isArrayElemTransferAllowed(ASE, HostVD,
                                                      /*IsArrElemPtr=*/false);
            if (Allowed)
              applyArrayElemTransition(CSCE->getExprLoc(), HostVD, FieldPath,
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
      // Count dereferences and find the base DeclRefExpr
      string fieldName;
      const Expr *E = UO;
      while (const UnaryOperator *U = dyn_cast<UnaryOperator>(E)) {
        if (U->getOpcode() == UO_Deref) {
          fieldName += "*";
          E = U->getSubExpr()->IgnoreParenImpCasts();
        } else {
          break;
        }
      }
      if (const DeclRefExpr *DRE =
              dyn_cast<DeclRefExpr>(E->IgnoreParenImpCasts())) {
        const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl());
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkCastField(VD, DRE->getLocation(), fieldName);
        reporter.addDiags(diags);
      }
    }
    // @code
    // (void * owned)mkNested()
    // @endcode
    else if (InnerE->getType()->hasOwnedFields()) {
      QualType InnerTy = InnerE->getType();
      SmallVector<OwnershipDiagInfo> diags;
      diags.push_back(OwnershipDiagInfo(
          InnerE->getExprLoc(), OwnershipDiagKind::InvalidCastFieldOwned,
          InnerTy.getAsString(),
          InnerTy->getPointeeType().getAsString() + " is"));
      reporter.addDiags(diags);
      Visit(CSCE->getSubExpr());
    }
    // if the canonical node is not handled, continue traverse to avoid breaking visit
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
        stat.init(VD);
      }
      if (Expr *Init = VD->getInit()) {
        // if has init expr, change the status of VD from UNINIT to OWNED or NULL
        if (VQT->isPointerType() && VQT.isOwnedQualified()) {
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
            if (ILE->getNumInits() == 0 || isAllNullInits(OS.ctx, ILE, ElemTy))
              stat.setArrayElemNull(VD);
            else
              stat.setArrayElemOwned(VD);
          } else {
            stat.setArrayElemOwned(VD);
          }
        } else if (VQT->isRecordType() && VQT->hasOwnedFields() &&
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

void TransferFunctions::HandleInitListExpr(VarDecl *VD, RecordDecl *RD, InitListExpr *ILE, std::string fullFieldName) {
  // unexplicitly initialized fields are implicitly initialized automatically
  Expr **Inits = ILE->getInits();
  // In semantic InitListExpr, omitted fields are ImplicitValueInitExpr both for
  // `{}` and for partial initializers like `{.a = 1}`. Only `{}` null-inits the
  // whole nested record family here; partial omission should keep leak checking.
  bool IsEmptyInitList = ILE->isSemanticForm() && ILE->getSyntacticForm() &&
                         ILE->getSyntacticForm()->getNumInits() == 0;

  auto markAsNull = [this, VD](const string &fieldName) {
    stat.SOwnedOwnedFields[VD].erase(fieldName);
    stat.SNullOwnedFields[VD].insert(fieldName);
  };

  auto markPrefixAsNull = [this, VD, &markAsNull](const string &prefix) {
    auto prefixStrs = findPrefixStrings(stat.SAllOwnedFields[VD], prefix);
    for (const string &str : prefixStrs)
      markAsNull(str);
  };

  for (const auto &FD : RD->fields()) {
    Expr *FieldInit = Inits[FD->getFieldIndex()];
    std::string memberField = FD->getNameAsString();
    std::string newFullFieldName =
        fullFieldName.empty() ? memberField : fullFieldName + "." + memberField;
    bool IsTrackedOwnedField = stat.SAllOwnedFields[VD].count(newFullFieldName);
    bool IsImplicitValueInit = isa<ImplicitValueInitExpr>(FieldInit);

    // allow ImplicitValueInit, e.g. struct S s = {0}
    if (FieldInit->isNullExpr(OS.ctx) || IsImplicitValueInit) {
      if (IsTrackedOwnedField) {
        // For `int *_Owned _Nullable *_Owned _Nullable f`, null-init also
        // covers deref-derived keys like "f*" (the internal key for `*f`).
        markAsNull(newFullFieldName);
        markPrefixAsNull(newFullFieldName + ".");
        markPrefixAsNull(newFullFieldName + "*");
      } else if (IsImplicitValueInit && IsEmptyInitList) {
        // For `struct Outer o = {};`, the record field itself (e.g. "inner")
        // may not be tracked, but its owned descendants ("inner.f") are.
        markPrefixAsNull(newFullFieldName + ".");
      }
      continue;
    }

    if (InitListExpr *FieldILE = dyn_cast<InitListExpr>(FieldInit)) {
      QualType QT = FieldInit->getType().getCanonicalType();
      if (QT->isRecordType() && QT->hasOwnedFields()) {
        RecordDecl *FieldRD = dyn_cast<RecordType>(QT)->getDecl();
        HandleInitListExpr(VD, FieldRD, FieldILE, newFullFieldName);
      }
      continue;
    }

    if (IsCastFromVoidPointer(FieldInit) && IsTrackedOwnedField) {
      stat.SOwnedOwnedFields[VD].insert(newFullFieldName);
      stat.SNullOwnedFields[VD].erase(newFullFieldName);
      auto allPrefixStrs =
          findPrefixStrings(stat.SAllOwnedFields[VD], newFullFieldName + ".");
      for (const string &str : allPrefixStrs) {
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
    if (IsCastFromVoidPointer(RV) && RV->getType()->hasOwnedFields()) {
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

void TransferFunctions::HandleDREAssign(const DeclRefExpr *DRE,
                                        string fullFieldName) {
  // for a DeclRefExpr assign, there is 6 situations:
  // 1. assign a struct owned pointer as a whole, i.e. fullFieldName = ""
  // 2. assign a struct owned pointer's field, i.e. fullFieldName != ""
  // 3. assign a struct as a whole
  // 4. assign a struct's owned field
  // 5. assign a basic owned pointer as a whole
  // 6. assign a basic owned pointer's owned field
  if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
    // situation 1 and 2
    if (stat.OPSStatus.count(VD)) {
      if (fullFieldName == "") {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkOPSAssign(VD, DRE->getLocation());
        reporter.addDiags(diags);
      } else if (fullFieldName == "*") {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkOPSDerefAssign(VD, DRE->getLocation());
        reporter.addDiags(diags);
      } else {
        if (fullFieldName[fullFieldName.size() - 1] == '*') {
          fullFieldName[fullFieldName.size() - 1] = '.';
          if (stat.OPSAllOwnedFields[VD].count(fullFieldName.substr(0, fullFieldName.size() - 1))) {
            SmallVector<OwnershipDiagInfo> diags =
                stat.checkOPSFieldAssign(VD, DRE->getLocation(), fullFieldName);
            reporter.addDiags(diags);
          }
          fullFieldName[fullFieldName.size() - 1] = '*';
        }
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkOPSFieldAssign(VD, DRE->getLocation(), fullFieldName);
        reporter.addDiags(diags);
      }
    }

    // situation 3 and 4
    if (stat.SStatus.count(VD)) {
      if (fullFieldName == "") {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkSAssign(VD, DRE->getLocation());
        reporter.addDiags(diags);
      } else {
        if (fullFieldName[fullFieldName.size() - 1] == '*') {
          fullFieldName[fullFieldName.size() - 1] = '.';
          if (stat.SAllOwnedFields[VD].count(fullFieldName.substr(0, fullFieldName.size() - 1))) {
            SmallVector<OwnershipDiagInfo> diags =
                stat.checkSFieldAssign(VD, DRE->getLocation(), fullFieldName);
            reporter.addDiags(diags);
          }
          fullFieldName[fullFieldName.size() - 1] = '*';
        }
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkSFieldAssign(VD, DRE->getLocation(), fullFieldName);
        reporter.addDiags(diags);
      }
    }

    // situation 5 and 6
    if (stat.BOPStatus.count(VD)) {
      if (fullFieldName == "") {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkBOPAssign(VD, DRE->getLocation());
        reporter.addDiags(diags);
      } else {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkBOPFieldAssign(VD, DRE->getLocation(), fullFieldName);
        reporter.addDiags(diags);
      }
    }
  }
}

void TransferFunctions::HandleDREUse(const DeclRefExpr *DRE,
                                     string fullFieldName) {
  // for a DeclRefExpr use, there is 4 situations:
  // 1. use a struct owned pointer as a whole, i.e. fullFieldName = ""
  // 2. use a struct owned pointer's field, i.e. fullFieldName != ""
  // 3. use a struct as a whole
  // 4. use a struct's owned field
  // 5. use a basic owned pointer as a whole
  // 6. use a basic owned pointer's owned field
  if (const VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
    // situation 1 and 2
    if (stat.OPSStatus.count(VD)) {
      if (fullFieldName == "") {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkOPSUse(VD, DRE->getLocation(), op == GetAddr, false, isAddrMut);
        isAddrMut = false;
        reporter.addDiags(diags);
      } else if (fullFieldName == "*") {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkOPSUse(VD, DRE->getLocation(), op == GetAddr, true, false);
        reporter.addDiags(diags);
      } else {
        if (fullFieldName[fullFieldName.size() - 1] == '*') {
          fullFieldName[fullFieldName.size() - 1] = '.';
          if (stat.OPSAllOwnedFields[VD].count(fullFieldName.substr(0, fullFieldName.size() - 1))) {
            SmallVector<OwnershipDiagInfo> diags = stat.checkOPSFieldUse(
                VD, DRE->getLocation(), fullFieldName, op == GetAddr);
            reporter.addDiags(diags);
          }
          fullFieldName[fullFieldName.size() - 1] = '*';
        }
        SmallVector<OwnershipDiagInfo> diags = stat.checkOPSFieldUse(
            VD, DRE->getLocation(), fullFieldName, op == GetAddr);
        reporter.addDiags(diags);
      }
    }

    // situation 3 and 4
    if (stat.SStatus.count(VD)) {
      if (fullFieldName == "") {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkSUse(VD, DRE->getLocation(), op == GetAddr, isAddrMut);
        isAddrMut = false;
        reporter.addDiags(diags);
      } else {
        if (fullFieldName[fullFieldName.size() - 1] == '*') {
          fullFieldName[fullFieldName.size() - 1] = '.';
          if (stat.SAllOwnedFields[VD].count(fullFieldName.substr(0, fullFieldName.size() - 1))) {
            SmallVector<OwnershipDiagInfo> diags = stat.checkSFieldUse(
                VD, DRE->getLocation(), fullFieldName, op == GetAddr);
            reporter.addDiags(diags);
          }
          fullFieldName[fullFieldName.size() - 1] = '*';
        }
        SmallVector<OwnershipDiagInfo> diags = stat.checkSFieldUse(
            VD, DRE->getLocation(), fullFieldName, op == GetAddr);
        reporter.addDiags(diags);
      }
    }

    // situation 5 and 6
    if (stat.BOPStatus.count(VD)) {
      if (fullFieldName == "") {
        SmallVector<OwnershipDiagInfo> diags =
            stat.checkBOPUse(VD, DRE->getLocation(), op == GetAddr, isAddrMut);
        isAddrMut = false;
        reporter.addDiags(diags);
      } else {
        SmallVector<OwnershipDiagInfo> diags = stat.checkBOPFieldUse(
            VD, DRE->getLocation(), fullFieldName, op == GetAddr);
        reporter.addDiags(diags);
      }
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
  bool ArrElemIsArray = ArrElem && PeelArrayElemBase(ArrElem) != nullptr;
  if (!ArrElemIsArray && ArrElem) {
    // Struct-field array element: w.arr[i] / o.w.arr[i]. The base is a
    // member access, which PeelArrayElemBase cannot unwrap; recognise it via
    // the host + "arr[]" path so the null-check branch nulls the field
    // aggregate (same as the local-array null-check).
    const VarDecl *HostVD = nullptr;
    std::string FieldPath;
    if (PeelHostAndFieldPath(ArrElem, HostVD, FieldPath) &&
        IsOwnedArrayFieldPath(HostVD, FieldPath))
      ArrElemIsArray = true;
  }

  if (cur->succ_begin()[0] == block) {
    // block is True branch
    for (const Expr *E : Info.nullCheckedExprs)
      status.setToNull(E);
    if (ArrElemIsArray && !ArrElemNonNull)
      status.setToNull(ArrElem);
  } else if (cur->succ_begin()[1] == block) {
    // block is False branch, inversely set present checked exprs to null
    for (const Expr *E : Info.presentCheckedExprs)
      status.setToNull(E);
    if (ArrElemIsArray && ArrElemNonNull)
      status.setToNull(ArrElem);
  }
}

Ownership::OwnershipStatus
OwnershipImpl::runOnBlock(const CFGBlock *block,
                          Ownership::OwnershipStatus status,
                          OwnershipDiagReporter &reporter, bool isDestructor) {
  TransferFunctions TF(*this, status, reporter);

  for (CFGBlock::const_iterator it = block->begin(), ei = block->end();
       it != ei; ++it) {
    const CFGElement &elem = *it;

    if (elem.getAs<CFGStmt>()) {
      const Stmt *S = elem.castAs<CFGStmt>().getStmt();
      if (isa<DeclStmt>(S) || isa<CallExpr>(S) ||
          (isa<BinaryOperator>(S) &&
           dyn_cast<BinaryOperator>(S)->isAssignmentOp()) ||
          (isa<UnaryOperator>(S) &&
           dyn_cast<UnaryOperator>(S)->isIncrementDecrementOp()) ||
          isa<ReturnStmt>(S)) {
        // Handling CallExpr iff it is a CFG stmt.
        if (isa<CallExpr>(S)) {
          // Nested calls inside an enclosing call's argument list are
          // analyzed inline by that enclosing call (see CollectArgumentCalls)
          // skip their hoisted CFGStmt elements.
          if (ArgCalls.count(S))
            continue;
          TF.SetHandlingCallExpr();
        }
        TF.Visit(const_cast<Stmt *>(S));
      }
    }

    if (elem.getAs<CFGLifetimeEnds>()) {
      const Stmt *S = elem.castAs<CFGLifetimeEnds>().getTriggerStmt();
      const VarDecl *VD = elem.castAs<CFGLifetimeEnds>().getVarDecl();
      TF.VisitLifetimeEnds(const_cast<VarDecl *>(VD), S->getEndLoc(),
                           isDestructor);
    }
  }

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
    classifyOwnedArrayLoops(ctx, &fd, LoopInfo, reporter);

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

  // Mark all owned parameter Owned at the entry
  for (ParmVarDecl *PVD : fd.parameters()) {
    if (IsTrackedType(PVD->getType())) {
      if (const VarDecl *VD = dyn_cast<VarDecl>(PVD)) {
        OS->blocksEndStatus[entry].init(VD);
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
