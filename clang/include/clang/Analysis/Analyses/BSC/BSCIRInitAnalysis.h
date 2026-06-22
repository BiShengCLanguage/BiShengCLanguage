//===- BSCIRInitAnalysis.h - Initialization analysis on BSCIR -*- C++ -*---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// P2795-style initialization analysis on BSCIR. In _Safe zones, tracks
// definite initialization of ALL types (not just _Owned). Reports use of
// uninitialized and possibly-uninitialized values.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ANALYSIS_ANALYSES_BSC_BSCIRINITANALYSIS_H
#define LLVM_CLANG_ANALYSIS_ANALYSES_BSC_BSCIRINITANALYSIS_H

#if ENABLE_BSC

#include "clang/Analysis/Analyses/BSC/BSCIR.h"
#include "clang/Analysis/Analyses/BSC/BSCIRDataflow.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include <map>

namespace clang {
namespace bscir {

//===----------------------------------------------------------------------===//
// Init Analysis Lattice
//===----------------------------------------------------------------------===//

/// Initialization state for a single local or place.
enum class InitState : uint8_t {
  Uninitialized, // Definitely not initialized
  MaybeInit,     // Initialized on some paths but not all
  Initialized    // Definitely initialized on all paths
};

/// Lattice for initialization analysis.
/// Tracks which locals/places are definitely initialized.
struct InitLattice {
  /// Per-local initialization state.
  llvm::DenseMap<LocalId, InitState> LocalStates;

  /// Per-field initialization state for struct locals.
  /// Key is a FieldPath (base local + recursive field indices).
  std::map<FieldPath, InitState> FieldStates;

  /// For callee-side ensure_init verification: tracks whether *param has been
  /// initialized, keyed by the parameter's LocalId.
  llvm::DenseMap<LocalId, InitState> EnsureInitDerefStates;

  /// Re-pointed ensure_init[_if_ret] params and the re-point site(s). Gates
  /// deref-write promotion (the write hits the new pointee) and feeds the
  /// at-return note.
  llvm::DenseMap<LocalId, SmallVector<SourceLocation, 2>> ReassignedParams;

  /// Caller-side pending init: `ret = f(&x[.field])` with ensure_init_if_ret(V)
  /// records {x[.field], ret, V} until a SwitchInt edge resolves it.
  /// OutFieldIndices is empty when the whole local was addressed.
  struct PendingCondInit {
    LocalId OutParamLocal;
    SmallVector<unsigned, 2> OutFieldIndices;
    LocalId RetLocal;
    int CondValue;
    bool operator==(const PendingCondInit &O) const {
      return OutParamLocal == O.OutParamLocal &&
             OutFieldIndices == O.OutFieldIndices &&
             RetLocal == O.RetLocal && CondValue == O.CondValue;
    }
  };
  SmallVector<PendingCondInit, 2> PendingCondInits;

  /// Locals holding a known integer constant (the builder lowers `-1` to a
  /// temp before the comparison sees it). int64_t: the compared/returned
  /// value is arbitrary-width, unlike the range-limited cond.
  llvm::DenseMap<LocalId, int64_t> KnownConstants;

  /// Tracks that a local is the boolean result of a comparison.
  /// E.g., _tmp = (ret == 0) records {_tmp → {ret, 0, true}}.
  struct ComparisonFact {
    LocalId ComparedLocal;
    int64_t ComparedValue; // arbitrary-width; see KnownConstants
    bool IsEq; // true for ==, false for !=
    bool operator==(const ComparisonFact &O) const {
      return ComparedLocal == O.ComparedLocal &&
             ComparedValue == O.ComparedValue && IsEq == O.IsEq;
    }
    // Used by DenseMap<LocalId, ComparisonFact>::operator== in
    // InitLattice::operator== (its value comparison calls operator!=).
    bool operator!=(const ComparisonFact &O) const { return !(*this == O); }
  };
  llvm::DenseMap<LocalId, ComparisonFact> ComparisonFacts;

  bool operator==(const InitLattice &Other) const {
    return LocalStates == Other.LocalStates &&
           FieldStates == Other.FieldStates &&
           EnsureInitDerefStates == Other.EnsureInitDerefStates &&
           ReassignedParams == Other.ReassignedParams &&
           PendingCondInits == Other.PendingCondInits &&
           ComparisonFacts == Other.ComparisonFacts &&
           KnownConstants == Other.KnownConstants;
  }
};

//===----------------------------------------------------------------------===//
// Init Analysis Diagnostics
//===----------------------------------------------------------------------===//

enum class InitDiagKind {
  UseOfUninit,
  UseOfMaybeUninit,
  ReturnUninit,           // return value not initialized on all paths
  ReturnMaybeUninit,      // return value possibly not initialized on all paths
  EnsureInitNotInit,        // ensure_init param not initialized at return
  EnsureInitMaybeNotInit,   // ensure_init param possibly not initialized at return
  EnsureInitReassigned,     // ensure_init failure whose cause on this path is a re-point
  EnsureInitPtrAliased,     // ensure_init pointer reassigned or copied before *param initialized
  EnsureInitDerefReadUninit,// *param read before initialization
  EnsureInitIfRetNotInit,        // ensure_init_if_ret param uninit at constant ret==arg
  EnsureInitIfRetMaybeNotInit,   // ensure_init_if_ret param maybe-uninit at constant ret==arg
  EnsureInitIfRetReassigned,     // ensure_init_if_ret failure whose cause is a re-point
  EnsureInitIfRetNonConstReturn, // ensure_init_if_ret return value is not a constant
};

struct InitDiagInfo {
  InitDiagKind Kind;
  SourceLocation Loc;
  std::string VarName;
  int CondValue = 0; // Only for EnsureInitIfRet* kinds
  /// Attribute name for the shared diagnostics: 0 = ensure_init,
  /// 1 = ensure_init_if_ret.
  int AttrSelect = 0;
  /// Additional note locations attached to the primary diagnostic
  /// (e.g. re-point sites on the failing path).
  SmallVector<SourceLocation, 2> NoteLocs;

  InitDiagInfo(InitDiagKind K, SourceLocation L, StringRef Name)
      : Kind(K), Loc(L), VarName(Name.str()) {}
  InitDiagInfo(InitDiagKind K, SourceLocation L, StringRef Name, int CV)
      : Kind(K), Loc(L), VarName(Name.str()), CondValue(CV) {}
};

//===----------------------------------------------------------------------===//
// Initialization Analysis
//===----------------------------------------------------------------------===//

class InitAnalysis
    : public DataflowAnalysis<Direction::Forward, InitLattice> {
public:
  InitAnalysis(const Body &B, bool CheckAllZones = false)
      : B(B), CheckAllZones(CheckAllZones) {}

  InitLattice entryState(const Body &B) const override;
  bool transferStatement(const Statement &S, InitLattice &State) const override;
  InitLattice transferTerminator(const Terminator &T,
                                 const InitLattice &StateBeforeTerm,
                                 BasicBlockId Target) const override;
  bool merge(const InitLattice &Src, InitLattice &Dst) const override;

  /// Run the analysis and collect diagnostics.
  void run(SmallVectorImpl<InitDiagInfo> &Diags) const;

  static bool isVacuouslyInitialized(QualType Ty);

private:
  const Body &B;
  bool CheckAllZones;

  /// Get the struct pointee type for an ensure_init param, or null QualType.
  QualType getEnsureInitPointeeType(LocalId Id) const;

  /// Cond value of an ensure_init_if_ret param at \p Id, or None. Derived
  /// from the parameter's attribute (a per-decl constant, not lattice state).
  llvm::Optional<int> getIfRetCondValue(LocalId Id) const;

  /// Check if a local/place is initialized in the given state.
  InitState getInitState(const InitLattice &State, LocalId Id) const;

  /// Mark a local as initialized.
  void markInit(InitLattice &State, LocalId Id) const;

  /// Mark a local as uninitialized (e.g., on StorageDead).
  void markUninit(InitLattice &State, LocalId Id) const;

  /// Check an operand for use of uninitialized values.
  void checkOperand(const Operand &Op, const InitLattice &State,
                    SourceLocation Loc,
                    SmallVectorImpl<InitDiagInfo> &Diags) const;

  /// Collect ensure_init/assume_initialized exempt arg temps for a block.
  llvm::DenseSet<LocalId>
  collectEnsureInitArgTemps(const BasicBlock &BB) const;

  /// Check ensure_init constraints on an assignment (reassignment + aliasing).
  void checkEnsureInitAssign(
      const Statement &S, const InitLattice &State,
      llvm::DenseMap<LocalId, LocalId> &TempToEnsureInitParam,
      SmallVectorImpl<InitDiagInfo> &Diags) const;

  /// Check deref reads of ensure_init params (*out before init).
  void checkEnsureInitDerefReads(
      const Statement &S, const InitLattice &State,
      const llvm::DenseMap<LocalId, LocalId> &TempToEnsureInitParam,
      SmallVectorImpl<InitDiagInfo> &Diags) const;

  /// Collect exempt ensure_init arg indices for a call terminator.
  llvm::DenseSet<unsigned>
  collectExemptArgIndices(const Terminator::CallData &CD) const;

  /// Check ensure_init contract at return.
  void checkEnsureInitAtReturn(const Terminator &T, const InitLattice &State,
                               SmallVectorImpl<InitDiagInfo> &Diags) const;

  /// Check ensure_init_if_ret contract at return (per-predecessor of return block).
  void checkEnsureInitIfRetAtReturn(
      const DataflowResult<InitLattice> &Result,
      SmallVectorImpl<InitDiagInfo> &Diags) const;

  /// The value the return slot (_0) holds on a path into the return block.
  struct ReturnValueInfo {
    SourceLocation Loc;        // the `_0 = ...` assignment
    bool IsConstant = false;   // _0 folds to an integer constant
    int64_t ConstVal = 0;      // the folded value when IsConstant
    LocalId SourceLocal{0};    // local _0 is a copy/cast of, if any
    bool HasSourceLocal = false;
  };

  /// Find _0's value on the path into the return block via \p PredId,
  /// walking back through the cleanup-block chain (Drop/Goto) before it.
  ReturnValueInfo analyzeReturnValue(BasicBlockId PredId) const;

  /// Get the number of fields for a type (0 for unions and non-record types).
  static unsigned getNumFields(QualType Ty);

  /// Get init state of a specific field path.
  InitState getFieldInitState(const InitLattice &State,
                              const FieldPath &FP) const;

  /// Mark a field path as initialized, then recursively promote parents.
  void markFieldInit(InitLattice &State, const FieldPath &FP,
                     bool &Changed) const;

  /// Mark `P` as initialized iff it names a FieldPath (no Deref/Index in
  /// its projection chain). Silently does nothing otherwise — writing
  /// through a pointer does not initialize the pointer's containing
  /// allocation.
  void markFieldInit(InitLattice &State, const Place &P,
                     bool &Changed) const;

  /// Clear all field states for a local (on StorageLive/Dead).
  void clearFieldStates(InitLattice &State, LocalId Id, bool &Changed) const;

  /// Check if all siblings at the leaf level are Init, promote parent,
  /// then recurse upward.
  void tryPromoteParent(InitLattice &State, const FieldPath &FP,
                        bool &Changed) const;

  /// FieldPath of `P` iff `P` is a chain of Field projections within a
  /// single allocation. Returns None when `P` contains a Deref or Index,
  /// because no FieldPath names such a Place.
  llvm::Optional<FieldPath> getFieldPath(const Place &P) const;

  /// Longest leading-Field prefix of `P`, even when `P` continues past
  /// a non-Field projection. Use only for read-side state lookups that
  /// want the closest tracked location.
  llvm::Optional<FieldPath> getFieldPathPrefix(const Place &P) const;

  /// Get the QualType at a given field path prefix for a local.
  QualType getFieldType(LocalId Id, ArrayRef<unsigned> Path) const;

  /// Recursively mark all leaf fields of a struct type as Initialized.
  void markAllFieldsInit(InitLattice &State, LocalId Base,
                         QualType Ty, SmallVector<unsigned, 4> &Prefix) const;

  /// Mark the whole pointee of an ensure_init pointer param as initialized:
  /// updates the deref state and, for struct pointees, all nested fields.
  /// No-op if Base is not an ensure_init pointer with tracked deref state.
  void markPointeeFullyInit(InitLattice &State, LocalId Base,
                            bool &Changed) const;

  /// Build a human-readable field-qualified name from a FieldPath.
  std::string buildFieldName(const FieldPath &FP) const;

  /// Check if a FieldPath traverses through a union into a struct's fields.
  /// Returns true if the path enters a union variant that is a struct type
  /// and continues deeper into that struct. Sets UnionDepth to the index
  /// in FP.Indices where the union variant index sits.
  bool isUnionStructFieldPath(const FieldPath &FP,
                              unsigned &UnionDepth) const;

  /// Check if a FieldPath ends at a union variant (enters union but doesn't
  /// continue into struct fields). Used to detect writes like u.f or u.s.
  bool isUnionVariantPath(const FieldPath &FP, unsigned &UnionDepth) const;

  /// Check if there are any FieldStates entries under a union prefix.
  bool hasUnionFieldEntries(const InitLattice &State, LocalId Base,
                            ArrayRef<unsigned> UnionPrefix) const;

  /// Clear all FieldStates entries under a union prefix.
  void clearUnionFieldEntries(InitLattice &State, LocalId Base,
                              ArrayRef<unsigned> UnionPrefix,
                              bool &Changed) const;
};

/// Run initialization analysis on a BSCIR Body and collect diagnostics.
/// Caller is responsible for emitting diagnostics via Sema::Diag().
void runInitAnalysis(const Body &B, SmallVectorImpl<InitDiagInfo> &Diags,
                     bool CheckAllZones = false);

} // namespace bscir
} // namespace clang

#endif // ENABLE_BSC
#endif // LLVM_CLANG_ANALYSIS_ANALYSES_BSC_BSCIRINITANALYSIS_H
