//===- BSCIR.cpp - Core BSCIR utilities -----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Place utilities, predecessor computation, and Body helpers for BSCIR.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/Analysis/Analyses/BSC/BSCIR.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::bscir;

//===----------------------------------------------------------------------===//
// ProjectionElem
//===----------------------------------------------------------------------===//

bool ProjectionElem::operator==(const ProjectionElem &Other) const {
  if (K != Other.K)
    return false;
  switch (K) {
  case Deref:
    return true;
  case Field:
    return FieldIndex == Other.FieldIndex && FD == Other.FD;
  case Index:
    return IndexLocal == Other.IndexLocal;
  case ConstantIndex:
    return ConstIdx == Other.ConstIdx;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Place
//===----------------------------------------------------------------------===//

SmallVector<Place, 4> Place::prefixes() const {
  SmallVector<Place, 4> Result;
  Result.reserve(Projections.size() + 1);

  // Base prefix (no projections). The type for the base local is not
  // available here (would need the Body), so we fall back to Ty.
  Result.push_back(Place(Base, Ty, Loc));

  // Each prefix is a sub-view of the arena-allocated projection array.
  for (unsigned I = 0; I < Projections.size(); ++I)
    Result.push_back(
        Place(Base, Projections.slice(0, I + 1), Projections[I].ResultTy, Loc));

  return Result;
}

SmallVector<Place, 4> Place::supportingPrefixes() const {
  SmallVector<Place, 4> Result;
  Result.push_back(Place(Base, Ty, Loc));

  QualType CurTy = Ty;
  for (unsigned I = 0; I < Projections.size(); ++I) {
    // Stop at const deref (shared reference dereference)
    if (Projections[I].K == ProjectionElem::Deref) {
      if (CurTy.isConstQualified())
        break;
    }
    CurTy = Projections[I].ResultTy;
    Result.push_back(
        Place(Base, Projections.slice(0, I + 1), CurTy, Loc));
  }
  return Result;
}

Place Place::project(ProjectionElem Elem, llvm::BumpPtrAllocator &Alloc,
                     SourceLocation OverrideLoc) const {
  size_t N = Projections.size();
  auto *New = Alloc.Allocate<ProjectionElem>(N + 1);
  std::copy(Projections.begin(), Projections.end(), New);
  New[N] = Elem;
  SourceLocation NewLoc = OverrideLoc.isValid() ? OverrideLoc : Loc;
  return Place(Base, ArrayRef<ProjectionElem>(New, N + 1), Elem.ResultTy,
               NewLoc);
}

std::string Place::toString() const {
  std::string Result = "_" + std::to_string(Base.Index);
  for (const auto &Proj : Projections) {
    switch (Proj.K) {
    case ProjectionElem::Deref:
      Result = "(*" + Result + ")";
      break;
    case ProjectionElem::Field:
      if (Proj.FD) {
        if (Proj.FD->isAnonymousStructOrUnion())
          Result += "." + formatAnonymousFieldName(Proj.FieldIndex,
                                                   Proj.FD->getParent());
        else
          Result += "." + Proj.FD->getNameAsString();
      } else {
        Result += "." + std::to_string(Proj.FieldIndex);
      }
      break;
    case ProjectionElem::Index:
      Result += "[_" + std::to_string(Proj.IndexLocal.Index) + "]";
      break;
    case ProjectionElem::ConstantIndex:
      Result += "[" + std::to_string(Proj.ConstIdx) + "]";
      break;
    }
  }
  return Result;
}

bool Place::operator==(const Place &Other) const {
  if (Base != Other.Base)
    return false;
  if (Projections.size() != Other.Projections.size())
    return false;
  for (unsigned I = 0; I < Projections.size(); ++I) {
    if (!(Projections[I] == Other.Projections[I]))
      return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
// Terminator
//===----------------------------------------------------------------------===//

SmallVector<BasicBlockId, 4> Terminator::successors() const {
  SmallVector<BasicBlockId, 4> Result;
  forEachSuccessor([&](BasicBlockId Id) { Result.push_back(Id); });
  return Result;
}

//===----------------------------------------------------------------------===//
// Body
//===----------------------------------------------------------------------===//

LocalId Body::addLocal(QualType Ty, const VarDecl *VD, StringRef Name,
                       bool IsTemp, SourceLocation Loc) {
  LocalId Id = {static_cast<unsigned>(Locals.size())};
  Locals.emplace_back(Id, Ty, VD, Name, IsTemp, Loc);
  return Id;
}

LocalId Body::addTemp(QualType Ty, SourceLocation Loc) {
  return addLocal(Ty, nullptr, StringRef(), true, Loc);
}

BasicBlockId Body::addBlock() {
  BasicBlockId Id = {static_cast<unsigned>(Blocks.size())};
  Blocks.emplace_back(Id);
  return Id;
}

void Body::computePredecessors() {
  Predecessors.clear();
  // Initialize empty lists for all blocks
  for (const auto &BB : Blocks)
    Predecessors[BB.Id];

  for (const auto &BB : Blocks) {
    BB.Term.forEachSuccessor([&](BasicBlockId Succ) {
      Predecessors[Succ].push_back(BB.Id);
    });
  }
}

ArrayRef<BasicBlockId> Body::getPredecessors(BasicBlockId Id) const {
  auto It = Predecessors.find(Id);
  if (It == Predecessors.end())
    return {};
  return It->second;
}

//===----------------------------------------------------------------------===//
// SimplifyCfg
//===----------------------------------------------------------------------===//

/// Remap a BasicBlockId using the old→new map.
static void remapBlockId(BasicBlockId &Id,
                         const llvm::DenseMap<unsigned, unsigned> &Map) {
  auto It = Map.find(Id.Index);
  if (It != Map.end())
    Id.Index = It->second;
}

/// Remap all successor block IDs in a terminator.
static void remapTerminator(Terminator &T,
                            const llvm::DenseMap<unsigned, unsigned> &Map) {
  switch (T.K) {
  case Terminator::Goto:
    remapBlockId(T.getGoto().Target, Map);
    break;
  case Terminator::SwitchInt: {
    auto &SW = T.getSwitchInt();
    for (auto &Target : SW.Targets)
      remapBlockId(Target.second, Map);
    remapBlockId(SW.Otherwise, Map);
    break;
  }
  case Terminator::Call: {
    auto &C = T.getCall();
    if (!C.Diverges)
      remapBlockId(C.Successor, Map);
    break;
  }
  case Terminator::Drop:
    remapBlockId(T.getDrop().Successor, Map);
    break;
  case Terminator::Return:
  case Terminator::Unreachable:
    break;
  }
}

/// Collapse goto chains: if A→Goto(B) and B is empty with Goto(C),
/// rewrite A's target to C. Also applies to all successor references.
static bool collapseGotoChains(Body &B) {
  bool Changed = false;
  for (auto &BB : B.Blocks) {
    // Resolve each successor through its goto chain; write back once at the end
    // to avoid mutating the terminator we are reading from.
    auto followChain = [&](BasicBlockId &Target) {
      unsigned Cur = Target.Index;
      llvm::SmallDenseSet<unsigned, 8> Visited;
      while (Cur < B.Blocks.size()) {
        const BasicBlock &TB = B.Blocks[Cur];
        if (TB.Term.K != Terminator::Goto || !TB.Statements.empty())
          break;
        unsigned Next = TB.Term.getGoto().Target.Index;
        if (!Visited.insert(Next).second)
          break; // cycle
        Cur = Next;
      }
      if (Cur != Target.Index) {
        Target.Index = Cur;
        Changed = true;
      }
    };

    switch (BB.Term.K) {
    case Terminator::Goto:
      followChain(BB.Term.getGoto().Target);
      break;
    case Terminator::SwitchInt: {
      auto &SW = BB.Term.getSwitchInt();
      for (auto &Target : SW.Targets)
        followChain(Target.second);
      followChain(SW.Otherwise);
      break;
    }
    case Terminator::Call:
      if (!BB.Term.getCall().Diverges)
        followChain(BB.Term.getCall().Successor);
      break;
    case Terminator::Drop:
      followChain(BB.Term.getDrop().Successor);
      break;
    default:
      break;
    }
  }
  return Changed;
}

/// Merge blocks: if A→Goto(B) and B has exactly one predecessor (A),
/// merge B's statements and terminator into A.
static bool mergeBlocks(Body &B) {
  bool EverChanged = false;
  bool Changed;

  // Iterate until no more merges are possible. Each pass does at most one
  // predecessor recomputation instead of one per merge.
  do {
    Changed = false;
    B.computePredecessors();

    for (unsigned I = 0; I < B.Blocks.size(); ++I) {
      BasicBlock &BB = B.Blocks[I];
      if (BB.Term.K != Terminator::Goto)
        continue;
      BasicBlockId TargetId = BB.Term.getGoto().Target;
      if (TargetId.Index >= B.Blocks.size() || TargetId.Index == I)
        continue;

      auto Preds = B.getPredecessors(TargetId);
      if (Preds.size() != 1)
        continue;

      BasicBlock &Target = B.Blocks[TargetId.Index];
      // The shared return block is an invariant every analysis may rely on.
      if (Target.Term.K == Terminator::Return)
        continue;
      // Merge Target into BB
      for (auto &S : Target.Statements)
        BB.Statements.push_back(std::move(S));
      BB.Term = std::move(Target.Term);

      // Mark Target as dead (unreachable with no statements)
      Target.Statements.clear();
      Target.Term = Terminator::createUnreachable();

      Changed = true;
      // Break out to recompute predecessors before continuing
      break;
    }
    EverChanged |= Changed;
  } while (Changed);

  return EverChanged;
}

/// Simplify branches: if SwitchInt has all targets identical, replace with Goto.
static bool simplifyBranches(Body &B) {
  bool Changed = false;
  for (auto &BB : B.Blocks) {
    if (BB.Term.K != Terminator::SwitchInt)
      continue;
    const auto &SW = BB.Term.getSwitchInt();
    BasicBlockId Target = SW.Otherwise;
    bool AllSame = true;
    for (const auto &T : SW.Targets) {
      if (T.second != Target) {
        AllSame = false;
        break;
      }
    }
    if (AllSame) {
      SafeZoneSpecifier SZ = BB.Term.SafeZone;
      BB.Term = Terminator::createGoto(Target, SZ);
      Changed = true;
    }
  }
  return Changed;
}

/// Remove dead (unreachable) blocks. Reachability from block 0.
/// Compact blocks and renumber IDs.
static void removeDeadBlocks(Body &B) {
  if (B.Blocks.empty())
    return;

  // BFS from entry
  llvm::SmallDenseSet<unsigned, 32> Reachable;
  SmallVector<unsigned, 16> Worklist;
  Worklist.push_back(0);
  Reachable.insert(0);

  while (!Worklist.empty()) {
    unsigned Idx = Worklist.pop_back_val();
    if (Idx >= B.Blocks.size())
      continue;
    B.Blocks[Idx].Term.forEachSuccessor([&](BasicBlockId Succ) {
      if (Succ.Index < B.Blocks.size() && Reachable.insert(Succ.Index).second)
        Worklist.push_back(Succ.Index);
    });
  }

  if (Reachable.size() == B.Blocks.size())
    return; // nothing to remove

  // Build old→new index map and compact
  llvm::DenseMap<unsigned, unsigned> OldToNew;
  SmallVector<BasicBlock, 32> NewBlocks;
  for (unsigned I = 0; I < B.Blocks.size(); ++I) {
    if (Reachable.count(I)) {
      unsigned NewIdx = NewBlocks.size();
      OldToNew[I] = NewIdx;
      NewBlocks.push_back(std::move(B.Blocks[I]));
    }
  }

  // Renumber block IDs and successor references
  for (unsigned I = 0; I < NewBlocks.size(); ++I) {
    NewBlocks[I].Id.Index = I;
    remapTerminator(NewBlocks[I].Term, OldToNew);
  }

  B.Blocks = std::move(NewBlocks);
}

void Body::simplify() {
  // Unreachable blocks are dropped first so they never count as predecessors.
  removeDeadBlocks(*this);
  bool Changed = true;
  while (Changed) {
    Changed = false;
    Changed |= collapseGotoChains(*this);
    Changed |= mergeBlocks(*this);
    Changed |= simplifyBranches(*this);
    removeDeadBlocks(*this);
  }
}

#endif // ENABLE_BSC
