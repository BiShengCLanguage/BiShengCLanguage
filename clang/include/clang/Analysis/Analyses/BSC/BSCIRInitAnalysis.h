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

/// What an `&`-origin argument addresses, leading Deref resolved away.
struct AddressedPlace {
  bool Recognised = false;
  bool Pointee = false; // came through a Deref, so a re-point invalidates it
  FieldPath Path;       // Base always set; Indices empty for a whole object
  bool operator==(const AddressedPlace &O) const {
    return Recognised == O.Recognised && Pointee == O.Pointee && Path == O.Path;
  }
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
  struct PendingCondInit {
    AddressedPlace Out;
    LocalId RetLocal;
    /// The cond value as RetLocal's type holds it, converted along with copies.
    int64_t CondValue;
    /// The callee's return type: converting back to it cannot change the value.
    QualType RetTy;
    bool operator==(const PendingCondInit &O) const {
      return Out == O.Out && RetLocal == O.RetLocal &&
             CondValue == O.CondValue && RetTy == O.RetTy;
    }
  };
  SmallVector<PendingCondInit, 2> PendingCondInits;

  /// What the return slot (_0) holds on this path.
  struct ReturnValue {
    enum Kind { None, Constant, Local, Unknown } K = None;
    int64_t Const = 0;  // arbitrary-width, unlike the range-limited cond
    LocalId Src{0};     // the local _0 copies, for delegation credit
    SourceLocation Loc; // the `_0 = ...` assignment
    bool operator==(const ReturnValue &O) const {
      return K == O.K && Const == O.Const && Src == O.Src;
    }
  };
  ReturnValue RetValue;

  /// Tracks that a local is the boolean result of a comparison.
  /// E.g., _tmp = (ret == 0) records {_tmp → {ret, 0, true}}.
  struct ComparisonFact {
    LocalId ComparedLocal;
    int64_t ComparedValue; // arbitrary-width, unlike the range-limited cond
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
           RetValue == Other.RetValue;
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
      : B(B), CheckAllZones(CheckAllZones) {
    collectContractPointerFlow();
  }

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

  /// What a value may hold of the contract parameters: their pointers, their
  /// own addresses, or something else.
  struct ContractPointerFlow {
    SmallVector<LocalId, 1> Params;
    SmallVector<LocalId, 1> ParamAddresses;
    bool Other = false;
  };

  /// Per temp, from every assignment in the body.
  llvm::DenseMap<LocalId, ContractPointerFlow> TempFlows;

  void collectContractPointerFlow();

  /// What \p Src may hold, given the flows collected so far.
  ContractPointerFlow flowOf(const Rvalue &Src) const;

  /// The local whose pointer \p Src hands on, if any.
  llvm::Optional<LocalId> pointerHandedOn(const Rvalue &Src) const;

  /// The parameter at \p Id when it carries ensure_init or ensure_init_if_ret.
  const ParmVarDecl *contractParamDecl(LocalId Id) const;
  bool isContractParam(LocalId Id) const { return contractParamDecl(Id); }

  /// Contract parameters whose pointer \p L may hold.
  SmallVector<LocalId, 1> mayHoldParam(LocalId L) const;

  /// The contract parameter whose pointer \p L holds on every path, if any.
  llvm::Optional<LocalId> surelyHoldsParam(LocalId L) const;

  AddressedPlace classifyAddressedPlace(const Place &P) const;

  /// Which place of ours the callee's contract covers for argument \p I.
  AddressedPlace classifyContractArg(const Terminator::CallData &CD,
                                     unsigned I) const;

  /// Mark what \p Addressed names as initialized.
  void markAddressedPlaceInit(InitLattice &State,
                              const AddressedPlace &Addressed,
                              bool CreditPointeeOfLocal, bool &Changed) const;

  /// Get the struct pointee type for an ensure_init param, or null QualType.
  QualType getEnsureInitPointeeType(LocalId Id) const;

  /// Cond value of an ensure_init_if_ret param at \p Id, or None. Derived
  /// from the parameter's attribute (a per-decl constant, not lattice state).
  llvm::Optional<int> getIfRetCondValue(LocalId Id) const;

  /// State of a whole local; a local with no entry reads as uninitialized.
  InitState getInitState(const InitLattice &State, LocalId Id) const;

  /// Set a whole local's state, flagging \p Changed when it moves.
  void setInitState(InitLattice &State, LocalId Id, InitState S,
                    bool *Changed = nullptr) const;

  /// State of `*Id`, or None when \p Id is not a contract parameter.
  llvm::Optional<InitState> getDerefState(const InitLattice &State,
                                          LocalId Id) const;

  /// Mark a local and every field below it initialized.
  void markLocalFullyInit(InitLattice &State, LocalId Id, bool &Changed) const;

  /// Mark the whole object \p Base names initialized, in whichever map holds it.
  void markWholeObjectInit(InitLattice &State, LocalId Base,
                           bool &Changed) const;

  /// Report a contract failure, with the re-point sites on this path as notes.
  void reportContract(const InitLattice &State, LocalId ParamId,
                      InitDiagKind K, SourceLocation Loc, int CondValue,
                      SmallVectorImpl<InitDiagInfo> &Diags) const;

  /// Check an operand for use of uninitialized values.
  void checkOperand(const Operand &Op, const InitLattice &State,
                    SourceLocation Loc,
                    SmallVectorImpl<InitDiagInfo> &Diags) const;

  /// Indices of the arguments passed as `&place` to a contract parameter, and
  /// every argument of __assume_initialized.
  llvm::DenseSet<unsigned>
  contractArgIndices(const Terminator::CallData &CD) const;

  /// The temps those arguments pass.
  llvm::DenseSet<LocalId>
  contractArgTemps(const Terminator::CallData &CD,
                   const llvm::DenseSet<unsigned> &ContractArgs) const;

  /// Check ensure_init constraints on an assignment (aliasing).
  void checkEnsureInitAssign(const Statement &S, const InitLattice &State,
                             SmallVectorImpl<InitDiagInfo> &Diags) const;

  /// Check deref reads of ensure_init params (*out before init).
  void checkEnsureInitDerefReads(
      const Statement &S, const InitLattice &State,
      const llvm::DenseSet<LocalId> &EnsureInitArgTemps,
      SmallVectorImpl<InitDiagInfo> &Diags) const;

  /// Diagnose \p P reading the uninitialized pointee of a contract param.
  void checkEnsureInitPointeeRead(const Place &P, const InitLattice &State,
                                  SourceLocation Loc,
                                  SmallVectorImpl<InitDiagInfo> &Diags) const;

  /// Carries \p PCI through a cast of its value from \p From to \p To, or
  /// returns false when the converted value no longer identifies the original.
  bool recordFollowsCast(InitLattice::PendingCondInit &PCI, QualType From,
                         QualType To) const;

  /// What \p V becomes once converted to \p Ty.
  int64_t convertedTo(int64_t V, QualType Ty) const;

  /// What \p V becomes once converted to this function's return type.
  int64_t asReturnedValue(int64_t V) const;

  /// One value a return may yield, with the state where it was produced.
  struct ReturnArm {
    InitLattice::ReturnValue RV;
    InitLattice State;
  };

  /// The arms of the conditional expression \p PredState returns, or that
  /// state alone when it returns anything else.
  SmallVector<ReturnArm, 2>
  collectReturnArms(const DataflowResult<InitLattice> &Result,
                    const InitLattice &PredState) const;

  /// Check ensure_init contract at return.
  void checkEnsureInitAtReturn(const Terminator &T, const InitLattice &State,
                               SmallVectorImpl<InitDiagInfo> &Diags) const;

  /// Check ensure_init_if_ret contract at return (per-predecessor of return block).
  void checkEnsureInitIfRetAtReturn(
      const DataflowResult<InitLattice> &Result,
      SmallVectorImpl<InitDiagInfo> &Diags) const;

  /// Get the number of fields for a type (0 for unions and non-record types).
  static unsigned getNumFields(QualType Ty);

  /// Get init state of a specific field path.
  InitState getFieldInitState(const InitLattice &State,
                              const FieldPath &FP) const;

  /// Whether \p FP reads as initialized: no storage, or it or an ancestor is.
  bool isFieldPathCovered(const InitLattice &State, const FieldPath &FP) const;

  /// Mark a field path as initialized, then recursively promote parents.
  void markFieldInit(InitLattice &State, const FieldPath &FP,
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

  /// The type field paths under \p Id index into: the pointee for a contract
  /// parameter, the local's own type otherwise.
  QualType trackedRootType(LocalId Id) const;

  /// Call \p F(RD, FD, Depth) for each field \p Path selects under \p Base,
  /// stopping at a non-record, a missing field, or when \p F returns false.
  template <class Fn>
  void walkFieldPath(LocalId Base, ArrayRef<unsigned> Path, Fn F) const;

  /// Mark every field below \p Ty initialized.
  void markAllFieldsInit(InitLattice &State, LocalId Base, QualType Ty,
                         bool &Changed) const;

  /// Mark the whole pointee of an ensure_init pointer param as initialized:
  /// updates the deref state and, for struct pointees, all nested fields.
  /// No-op if Base is not an ensure_init pointer with tracked deref state.
  void markPointeeFullyInit(InitLattice &State, LocalId Base,
                            bool &Changed) const;

  /// Build a human-readable field-qualified name from a FieldPath.
  std::string buildFieldName(const FieldPath &FP) const;

  /// First union depth along \p FP (its index selects a variant), or None.
  llvm::Optional<unsigned> firstUnionDepth(const FieldPath &FP) const;
};

/// Run initialization analysis on a BSCIR Body and collect diagnostics.
/// Caller is responsible for emitting diagnostics via Sema::Diag().
void runInitAnalysis(const Body &B, SmallVectorImpl<InitDiagInfo> &Diags,
                     bool CheckAllZones = false);

} // namespace bscir
} // namespace clang

#endif // ENABLE_BSC
#endif // LLVM_CLANG_ANALYSIS_ANALYSES_BSC_BSCIRINITANALYSIS_H
