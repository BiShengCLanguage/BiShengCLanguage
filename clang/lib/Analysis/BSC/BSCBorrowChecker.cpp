//===- BSCBorrowChecker.cpp - Borrow Check for Source CFGs -*- BSC --*--------//
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

#if ENABLE_BSC

#include "clang/Analysis/Analyses/BSC/BSCBorrowChecker.h"
#include "clang/AST/BSC/TypeBSC.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace clang;
using namespace clang::borrow;

//===----------------------------------------------------------------------===//
//                         Stream output functions
//===----------------------------------------------------------------------===//

namespace clang {
namespace borrow {

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const RegionName &RN) {
  return OS << "RegionName { " << RN.Name << " }";
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              const RegionVariable &RV) {
  return OS << "RegionVariable { index: " << RV.index << " }";
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Point P) {
  return OS << "BB" << P.blockID << '/' << P.index;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Region &R) {
  OS << "{ ";
  llvm::interleaveComma(R.points, OS, [&](Point P) { OS << P; });
  if (!R.points.empty() && !R.endRegions.empty())
    OS << ", ";
  llvm::interleaveComma(R.endRegions, OS, [&](const RegionName &RN) {
    OS << "End(" << RN.Name << ")";
  });
  OS << " }";
  return OS;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Constraint &C) {
  return OS << "Constraint { " << C.sub << " : " << C.sup << " @ " << C.point
            << " }";
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Place *P) {
  switch (P->getKind()) {
  case Place::Kind::Var:
    OS << P->getName();
    break;
  case Place::Kind::Field:
    assert(P->getBase() && "field place should have a base");
    if (P->getBase()->getKind() == Place::Kind::Deref)
      OS << '(' << P->getBase() << ')';
    else
      OS << P->getBase();
    OS << '.' << P->getName();
    break;
  case Place::Kind::Deref:
    assert(P->getBase() && "deref place should have a base");
    OS << '*' << P->getBase();
    break;
  case Place::Kind::Index:
    assert(P->getBase() && "index place should have a base");
    OS << P->getBase() << "[_]";
    break;
  }
  return OS;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Loan &L) {
  OS << "Loan {\n";
  OS << "  point: " << L.point << '\n';
  OS << "  place: " << L.place << '\n';
  OS << "  kind: " << (L.kind == BorrowKind::Mut ? "Mut" : "Shared") << '\n';
  OS << "  region: " << L.region << '\n';
  OS << "}";
  return OS;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Ty *T) {
  switch (T->getKind()) {
  case Ty::TyKind::Base:
    OS << T->getQualType().getAsString();
    break;
  case Ty::TyKind::Pointer:
    OS << "Pointer(" << T->getQualType().getAsString();
    if (T->isBorrowPointer())
      OS << ", region=" << T->getRegion().Name;
    OS << ", pointee=" << T->getPointee() << ")";
    break;
  case Ty::TyKind::Struct:
    OS << "Struct(" << T->getQualType().getAsString() << ", regions=[";
    llvm::interleaveComma(T->getRegionParams(), OS,
                          [&](const RegionName *RN) { OS << RN->Name; });
    OS << "])";
    break;
  case Ty::TyKind::Array:
    OS << "Array(" << T->getQualType().getAsString()
       << ", element=" << T->getElement() << ")";
    break;
  }
  return OS;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const ActionNoop *AN) {
  (void)AN;
  return OS << "ActionNoop";
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const ActionCall *AC) {
  OS << "ActionCall(return_ty=" << AC->ReturnTy << ", result=";
  if (AC->Result)
    OS << AC->Result;
  else
    OS << "<none>";

  OS << ", args=[";
  llvm::interleaveComma(AC->Args, OS, [&](const ActionCall::Arg &Arg) {
    OS << "{param_ty=";
    if (Arg.ParamTy)
      OS << Arg.ParamTy;
    else
      OS << "<none>";
    OS << ", value=";
    if (Arg.Value)
      OS << Arg.Value;
    else
      OS << "<none>";
    OS << "}";
  });
  OS << "])";
  return OS;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const ActionReturn *AR) {
  OS << "ActionReturn(return_ty=" << AR->ReturnTy << ", value=";
  if (AR->Value)
    OS << AR->Value;
  else
    OS << "<none>";
  OS << ")";
  return OS;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              const ActionBorrow *AB) {
  OS << "ActionBorrow(dest=" << AB->Dest << ", ";
  OS << (AB->BK == BorrowKind::Mut ? "mut" : "shared");
  OS << ", ty=" << AB->BorrowTy << ", source=" << AB->Source << ")";
  return OS;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              const ActionInit *AI) {
  OS << "ActionInit(dest=" << AI->Dest << ", sources=[";
  llvm::interleaveComma(AI->Sources, OS,
                        [&](const Place *P) { OS << P; });
  return OS << "])";
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              const ActionAssign *AA) {
  return OS << "ActionAssign(dest=" << AA->Dest
            << ", source=" << AA->Source << ")";
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              const ActionAggregate *AA) {
  OS << "ActionAggregate(kind=";
  switch (AA->AK) {
  case ActionAggregate::AggregateKind::Copy: {
    OS << "copy, dest=" << AA->Dest << ", source=" << AA->CopySource
       << ", implicit_reborrows=[";
    llvm::interleaveComma(
        AA->ImplicitReborrows, OS,
        [&](const ActionAggregate::ImplicitReborrow &Reborrow) {
          OS << "{dest_ty=" << Reborrow.DestTy
             << ", borrowed_place=" << Reborrow.BorrowedPlace << "}";
        });
    return OS << "])";
  }
  case ActionAggregate::AggregateKind::Init: {
    OS << "init, dest=" << AA->Dest << ", initializers=[";
    llvm::interleaveComma(
        AA->Initializers, OS,
        [&](const ActionAggregate::Initializer &Initializer) {
          OS << "{dest_ty=" << Initializer.DestTy
             << ", value=" << Initializer.Value << "}";
        });
    return OS << "])";
  }
  }
  llvm_unreachable("unknown aggregate action kind");
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const ActionUse *AU) {
  OS << "ActionUse(places=[";
  llvm::interleaveComma(AU->Places, OS,
                        [&](const Place *P) { OS << P; });
  OS << "])";
  return OS;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              const ActionStorageDead *ASD) {
  return OS << "ActionStorageDead(place=" << ASD->P << ")";
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, const Action *A) {
  switch (A->getKind()) {
  case Action::Noop:
    OS << llvm::cast<ActionNoop>(A);
    break;
  case Action::Call:
    OS << llvm::cast<ActionCall>(A);
    break;
  case Action::Return:
    OS << llvm::cast<ActionReturn>(A);
    break;
  case Action::Borrow:
    OS << llvm::cast<ActionBorrow>(A);
    break;
  case Action::Init:
    OS << llvm::cast<ActionInit>(A);
    break;
  case Action::Assign:
    OS << llvm::cast<ActionAssign>(A);
    break;
  case Action::Aggregate:
    OS << llvm::cast<ActionAggregate>(A);
    break;
  case Action::Use:
    OS << llvm::cast<ActionUse>(A);
    break;
  case Action::StorageDead:
    OS << llvm::cast<ActionStorageDead>(A);
    break;
  }
  return OS;
}

} // namespace borrow
} // namespace clang

namespace {
bool IsTrackedTypeImpl(const ASTContext &Ctx, QualType type,
                       llvm::SmallPtrSetImpl<const RecordDecl *> &visited) {
  type = type.getCanonicalType();
  if (type->isPointerType() && type.isOwnedQualified()) {
    return IsTrackedTypeImpl(Ctx, type->getPointeeType(), visited);
  }
  if (type->isArrayType()) {
    if (const ArrayType *AT = Ctx.getAsArrayType(type)) {
      return IsTrackedTypeImpl(Ctx, AT->getElementType(), visited);
    }
  }
  if (type.isBorrowQualified())
    return true;
  if (const RecordType *RT = type->getAs<RecordType>()) {
    if (const RecordDecl *RD = RT->getDecl()->getDefinition()) {
      if (!visited.insert(RD).second) {
        return false;
      }
      for (const FieldDecl *FD : RD->fields()) {
        if (IsTrackedTypeImpl(Ctx, FD->getType(), visited)) {
          return true;
        }
      }
    }
  }
  return false;
}

bool IsTrackedType(const ASTContext &Ctx, QualType type) {
  llvm::SmallPtrSet<const RecordDecl *, 8> visited;
  return IsTrackedTypeImpl(Ctx, type, visited);
}

} // namespace

namespace clang {
namespace borrow {

bool Place::equals(const Place *Other) const {
  switch (K) {
  case Kind::Var:
    return Other->K == Kind::Var && D == Other->D;
  case Kind::Field:
    return Other->K == Kind::Field && D == Other->D &&
           Base->equals(Other->Base);
  case Kind::Deref:
  case Kind::Index:
    return (Other->K == Kind::Deref || Other->K == Kind::Index) &&
           Base->equals(Other->Base);
  }
  llvm_unreachable("unknown place kind");
}

llvm::SmallVector<const Place *> Place::prefixes() const {
  llvm::SmallVector<const Place *> Prefixes;
  const Place *Current = this;
  while (Current) {
    Prefixes.push_back(Current);
    Current = Current->getBase();
  }
  return Prefixes;
}

llvm::SmallVector<const Place *> Place::supportingPrefixes() const {
  llvm::SmallVector<const Place *> Prefixes;
  const Place *Current = this;
  while (Current) {
    Prefixes.push_back(Current);
    if ((Current->getKind() == Kind::Deref ||
         Current->getKind() == Kind::Index) &&
        Current->getBase()->getType()->isBorrowPointer() &&
        Current->getBase()->getType()->getBorrowKind() ==
            BorrowKind::Shared)
      return Prefixes;

    Current = Current->getBase();
  }
  return Prefixes;
}

Ty *Ty::Create(const Environment &Env, QualType QT,
               llvm::ArrayRef<const RegionName *> Regions) {
  const ASTContext &Ctx = Env.Ctx;
  unsigned NextRegion = 0;
  bool UseTrackedRegions =
      Regions.empty() || !Regions.front()->isUntracked();

  // Recursively build the `Ty` tree, referencing regions from `Regions` in
  // outermost-borrow-first, depth-first order. `NextRegion` is the shared
  // cursor advanced as tracked borrow layers are laid down. Beyond a raw
  // pointer, the same type shape is retained using untracked regions.
  auto Rec = [&](auto &Self, QualType Type, bool UseTrackedRegions) -> Ty * {
    Type = Type.getCanonicalType();

    if (Type->isPointerType()) {
      if (Type.isBorrowQualified()) {
        const RegionName *RN = nullptr;
        if (UseTrackedRegions) {
          assert(NextRegion < Regions.size() &&
                 "borrow pointer should have a precomputed region");
          RN = Regions[NextRegion++];
        } else {
          RN = &RegionName::GetUntracked();
        }
        Ty *Pointee =
            Self(Self, Type->getPointeeType(), UseTrackedRegions);
        return Ty::CreatePointer(Ctx, Type, RN, Pointee);
      }

      if (Type.isOwnedQualified()) {
        Ty *Pointee =
            Self(Self, Type->getPointeeType(), UseTrackedRegions);
        return Ty::CreatePointer(Ctx, Type, nullptr, Pointee);
      }

      Ty *Pointee = Self(Self, Type->getPointeeType(), false);
      return Ty::CreatePointer(Ctx, Type, nullptr, Pointee);
    }

    if (Type->isArrayType()) {
      if (const ArrayType *AT = Ctx.getAsArrayType(Type)) {
        Ty *Element =
            Self(Self, AT->getElementType(), UseTrackedRegions);
        return Ty::CreateArray(Ctx, Type, Element);
      }
    }

    if (const RecordType *RT = Type->getAs<RecordType>()) {
      const RecordDecl *Definition = RT->getDecl()->getDefinition();
      unsigned NumRegions =
          Definition ? Env.getRecordRegionLayout(Definition).getNumRegions()
                     : 0;
      llvm::SmallVector<const RegionName *, 2> Params;
      if (UseTrackedRegions) {
        assert(NextRegion + NumRegions <= Regions.size() &&
               "struct should have precomputed regions");
        llvm::ArrayRef<const RegionName *> TrackedParams =
            Regions.slice(NextRegion, NumRegions);
        Params.append(TrackedParams.begin(), TrackedParams.end());
        NextRegion += NumRegions;
      } else {
        Params.assign(NumRegions, &RegionName::GetUntracked());
      }
      return Ty::CreateStruct(Ctx, Type, Params);
    }

    return Ty::CreateBase(Ctx, Type);
  };

  Ty *Result = Rec(Rec, QT, UseTrackedRegions);
  assert((!UseTrackedRegions || NextRegion == Regions.size()) &&
         "unused precomputed regions");
  return Result;
}

Ty *Ty::CreateField(const Environment &Env, const Ty *BaseTy,
                    const FieldDecl *FD) {
  assert(BaseTy->getKind() == TyKind::Struct &&
         "field base should have a struct type");
  llvm::ArrayRef<unsigned> RegionIndices = Env.getFieldRegionIndices(FD);
  llvm::ArrayRef<const RegionName *> BaseRegions = BaseTy->getRegionParams();
  llvm::SmallVector<const RegionName *, 2> FieldRegions;
  FieldRegions.reserve(RegionIndices.size());
  for (unsigned Index : RegionIndices) {
    assert(Index < BaseRegions.size() &&
           "field region index should be within the record layout");
    FieldRegions.push_back(BaseRegions[Index]);
  }
  return Create(Env, FD->getType(), FieldRegions);
}

class RegionGenerator : public RecursiveASTVisitor<RegionGenerator> {
  friend class Environment;

private:
  const ASTContext &Ctx;
  Environment::RegionMap RegionMap;
  Environment::CallExprParamRegionMap CallExprParamRegions;
  RecordRegionLayoutMap RecordRegionLayouts;
  Environment::FreeRegionList FreeRegions;

  void CreateRegions(Environment::RegionMapKeyTy Key, unsigned Count) {
    if (Count == 0)
      return;
    bool Inserted =
        RegionMap.try_emplace(Key, RegionName::CreateN(Count)).second;
    assert(Inserted && "region map key should be generated only once");
  }

  void CreateCallExprRegions(CallExpr *CE) {
    // Canonicalization strips BSC qualifiers required by call-site Ty values.
    QualType CalleeTy = CE->getCallee()->getType();
    if (CalleeTy->isFunctionPointerType())
      CalleeTy = CalleeTy->getPointeeType();
    const FunctionType *FT = CalleeTy->castAs<FunctionType>();

    if (const FunctionProtoType *FPT = dyn_cast<FunctionProtoType>(FT)) {
      RegionName SharedRegion = RegionName::Create();
      unsigned NumReturnRegions = ComputeNumRegions(
          Ctx, FT->getReturnType(), RecordRegionLayouts);
      if (NumReturnRegions != 0) {
        RegionMap.try_emplace(CE, 1, SharedRegion);
      }

      Environment::CallExprParamRegionMapValueTy ParamRegions;
      ParamRegions.reserve(FPT->getNumParams());
      for (QualType ParamTy : FPT->param_types()) {
        unsigned NumParamRegions =
            ComputeNumRegions(Ctx, ParamTy, RecordRegionLayouts);
        Environment::RegionMapValueTy Regions;
        if (NumParamRegions != 0) {
          Regions.push_back(SharedRegion);
          for (unsigned I = 1; I < NumParamRegions; ++I)
            Regions.push_back(RegionName::Create());
        }
        ParamRegions.push_back(std::move(Regions));
      }

      CallExprParamRegions.try_emplace(CE, std::move(ParamRegions));
    }
  }

  void CreateFunctionSignatureRegions(const FunctionDecl &FD) {
    // Every tracked parameter shares its outer region. Inner regions remain
    // parameter-specific and are marked free below.
    RegionName SharedRegion = RegionName::Create();
    // Note: To keep SharedRegion non-free when the return type is untracked,
    // enable this unconditional insertion and remove the conditional one at
    // the end of the function.
    // FreeRegions.push_back(SharedRegion);
    bool HasMultiLevelParam = false;
    unsigned NumReturnRegions =
        ComputeNumRegions(Ctx, FD.getReturnType(), RecordRegionLayouts);

    for (ParmVarDecl *PVD : FD.parameters()) {
      unsigned NumParamRegions =
          ComputeNumRegions(Ctx, PVD->getType(), RecordRegionLayouts);
      if (NumParamRegions == 0)
        continue;

      HasMultiLevelParam |= NumParamRegions >= 2;

      Environment::RegionMapValueTy ParamRegions;
      ParamRegions.push_back(SharedRegion);
      for (unsigned I = 1; I < NumParamRegions; ++I) {
        RegionName InnerRegion = RegionName::Create();
        ParamRegions.push_back(InnerRegion);
        FreeRegions.push_back(std::move(InnerRegion));
      }

      RegionMap.try_emplace(PVD, std::move(ParamRegions));
    }

    if (HasMultiLevelParam || NumReturnRegions != 0)
      FreeRegions.insert(FreeRegions.begin(), SharedRegion);
  }

public:
  RecordRegionLayoutMap &getRecordRegionLayouts() {
    return RecordRegionLayouts;
  }

  RegionGenerator(const ASTContext &Ctx, const FunctionDecl &fd) : Ctx(Ctx) {
    CreateFunctionSignatureRegions(fd);
    TraverseStmt(fd.getBody());
  }

  // The prologue rewrites the semantic InitListExpr and may move a shared
  // initializer into a synthetic statement while the syntactic form still
  // references it. Region queries and actions use the semantic form, so walk
  // that form exactly once and avoid generating the same region twice.
  bool TraverseInitListExpr(InitListExpr *ILE,
                            DataRecursionQueue *Queue = nullptr) {
    InitListExpr *Semantic =
        ILE->isSemanticForm() ? ILE : ILE->getSemanticForm();
    assert(Semantic && "initializer list should have a semantic form");
    for (Stmt *Child : Semantic->children()) {
      if (!TraverseStmt(Child, Queue))
        return false;
    }
    return true;
  }

  bool VisitVarDecl(VarDecl *VD) {
    CreateRegions(
        VD, ComputeNumRegions(Ctx, VD->getType(), RecordRegionLayouts));
    return true;
  }

  bool VisitCStyleCastExpr(CStyleCastExpr *CSCE) {
    if (CSCE->getType().isBorrowQualified()) {
      CreateRegions(CSCE, 1);
    }
    return true;
  }

  bool VisitUnaryOperator(UnaryOperator *UO) {
    switch (UO->getOpcode()) {
    case UO_AddrConst:
    case UO_AddrMut:
    case UO_AddrConstDeref:
    case UO_AddrMutDeref:
      CreateRegions(UO, 1);
      break;
    default:
      break;
    }
    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    CreateCallExprRegions(CE);
    return true;
  }
};

inline Variance variance(BorrowKind BK);
Variance xform(Variance Ctx, Variance V);
inline Variance glb(Variance A, Variance B);

class DefVarianceAnalysis
    : public RecursiveASTVisitor<DefVarianceAnalysis> {
  friend class Environment;

  /// Constrain TargetRecord[TargetIndex] with Ambient, optionally transformed
  /// by SourceRecord[SourceIndex].
  struct DefVarianceConstraint {
    const RecordDecl *TargetRecord;
    unsigned TargetIndex;
    Variance Ambient;
    const RecordDecl *SourceRecord;
    unsigned SourceIndex;

    DefVarianceConstraint(const RecordDecl *TargetRecord,
                          unsigned TargetIndex, Variance Ambient,
                          const RecordDecl *SourceRecord = nullptr,
                          unsigned SourceIndex = 0)
        : TargetRecord(TargetRecord), TargetIndex(TargetIndex),
          Ambient(Ambient), SourceRecord(SourceRecord),
          SourceIndex(SourceIndex) {}
  };

  const ASTContext &Ctx;
  RecordRegionLayoutMap &RecordRegionLayouts;
  Environment::DefVarianceMap DefVarianceMap;
  llvm::SmallVector<DefVarianceConstraint> Constraints;

  void CollectFunctionType(const FunctionType *FT) {
    CollectType(FT->getReturnType());
    if (const auto *FPT = dyn_cast<FunctionProtoType>(FT)) {
      for (QualType ParamTy : FPT->param_types())
        CollectType(ParamTy);
    }
  }

  void CollectType(QualType Type) {
    Type = Type.getCanonicalType();

    if (Type->isPointerType()) {
      if (Type.isBorrowQualified() || Type.isOwnedQualified())
        CollectType(Type->getPointeeType());
      return;
    }

    if (Type->isArrayType()) {
      if (const ArrayType *AT = Ctx.getAsArrayType(Type))
        CollectType(AT->getElementType());
      return;
    }

    if (const RecordType *RT = Type->getAs<RecordType>()) {
      if (const RecordDecl *Definition = RT->getDecl()->getDefinition()) {
        const auto *CanonicalRecord =
            cast<RecordDecl>(Definition->getCanonicalDecl());
        if (DefVarianceMap.find(CanonicalRecord) == DefVarianceMap.end()) {
          const RecordRegionLayout &Layout = GetOrCreateRecordRegionLayout(
              Ctx, Definition, RecordRegionLayouts);
          Environment::DefVarianceMapValueTy Variances;
          Variances.assign(Layout.getNumRegions(), Variance::Bi);
          DefVarianceMap.try_emplace(CanonicalRecord, std::move(Variances));

          // Insert before visiting fields so recursive records terminate.
          for (const FieldDecl *FD : Definition->fields())
            CollectType(FD->getType());
        }
      }
    }
  }

  void AddTypeConstraints(const RecordDecl *TargetRecord, QualType Type,
                          Variance Ambient,
                          llvm::ArrayRef<unsigned> RegionIndices,
                          unsigned &NextIndex) {
    Type = Type.getCanonicalType();

    if (Type->isPointerType()) {
      if (Type.isBorrowQualified()) {
        // A borrow lifetime uses the ambient variance; mutability applies only
        // when descending into the referent.
        unsigned TargetIndex = RegionIndices[NextIndex++];
        Constraints.emplace_back(TargetRecord, TargetIndex, Ambient);

        BorrowKind BK =
            Type.isConstBorrow() ? BorrowKind::Shared : BorrowKind::Mut;
        AddTypeConstraints(TargetRecord, Type->getPointeeType(),
                           xform(Ambient, variance(BK)), RegionIndices,
                           NextIndex);
        return;
      }

      if (Type.isOwnedQualified())
        AddTypeConstraints(TargetRecord, Type->getPointeeType(), Ambient,
                           RegionIndices, NextIndex);
      return;
    }

    if (Type->isArrayType()) {
      if (const ArrayType *AT = Ctx.getAsArrayType(Type))
        AddTypeConstraints(TargetRecord, AT->getElementType(), Ambient,
                           RegionIndices, NextIndex);
      return;
    }

    const RecordType *RT = Type->getAs<RecordType>();
    if (!RT)
      return;
    const RecordDecl *Definition = RT->getDecl()->getDefinition();
    if (!Definition)
      return;
    const auto *SourceRecord =
        cast<RecordDecl>(Definition->getCanonicalDecl());
    auto SourceIt = DefVarianceMap.find(SourceRecord);
    if (SourceIt == DefVarianceMap.end())
      return;

    // A nested record contributes its declaration-site variances through the
    // current field's region projection.
    for (unsigned I = 0; I < SourceIt->second.size(); ++I) {
      unsigned TargetIndex = RegionIndices[NextIndex++];
      Constraints.emplace_back(TargetRecord, TargetIndex, Ambient,
                               SourceRecord, I);
    }
  }

  void BuildConstraints() {
    for (const auto &Entry : DefVarianceMap) {
      const RecordDecl *Definition = Entry.first->getDefinition();
      const RecordRegionLayout &Layout = GetOrCreateRecordRegionLayout(
          Ctx, Definition, RecordRegionLayouts);
      for (const FieldDecl *FD : Definition->fields()) {
        llvm::ArrayRef<unsigned> RegionIndices =
            Layout.getFieldRegionIndices(FD);
        unsigned NextIndex = 0;
        AddTypeConstraints(Entry.first, FD->getType(), Variance::Co,
                           RegionIndices, NextIndex);
        assert(NextIndex == RegionIndices.size() &&
               "variance constraints should consume the field layout");
      }
    }
  }

  void Solve() {
    // Starting from Bi, repeatedly apply constraints until every inferred
    // variance reaches its greatest fixed point.
    bool Changed = true;
    while (Changed) {
      Changed = false;
      for (const DefVarianceConstraint &Constraint : Constraints) {
        Variance Candidate = Constraint.Ambient;
        if (Constraint.SourceRecord) {
          Candidate = xform(
              Constraint.Ambient,
              DefVarianceMap.find(Constraint.SourceRecord)
                  ->second[Constraint.SourceIndex]);
        }

        Variance &Target =
            DefVarianceMap.find(Constraint.TargetRecord)
                ->second[Constraint.TargetIndex];
        Variance New = glb(Target, Candidate);
        if (New != Target) {
          Target = New;
          Changed = true;
        }
      }
    }
  }

public:
  DefVarianceAnalysis(const ASTContext &Ctx, const FunctionDecl &FD,
                      RecordRegionLayoutMap &RecordRegionLayouts)
      : Ctx(Ctx), RecordRegionLayouts(RecordRegionLayouts) {
    CollectType(FD.getReturnType());
    for (const ParmVarDecl *PVD : FD.parameters())
      CollectType(PVD->getType());
    TraverseStmt(FD.getBody());
    BuildConstraints();
    Solve();
  }

  bool VisitVarDecl(VarDecl *VD) {
    CollectType(VD->getType());
    return true;
  }

  bool VisitExpr(Expr *E) {
    CollectType(E->getType());
    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    QualType CalleeTy = CE->getCallee()->getType();
    if (CalleeTy->isFunctionPointerType())
      CalleeTy = CalleeTy->getPointeeType();
    if (const auto *FT = CalleeTy->getAs<FunctionType>())
      CollectFunctionType(FT);
    return true;
  }
};

Environment::Environment(const FunctionDecl &fd, const CFG &cfg,
                         const ASTContext &Ctx, RegionGenerator &RG,
                         DefVarianceAnalysis &DVA)
    : fd(fd), cfg(cfg), Ctx(Ctx), regionMap(std::move(RG.RegionMap)),
      callExprParamRegions(std::move(RG.CallExprParamRegions)),
      recordRegionLayouts(std::move(RG.RecordRegionLayouts)),
      defVarianceMap(std::move(DVA.DefVarianceMap)),
      freeRegions(std::move(RG.FreeRegions)) {
#if DEBUG_PRINT
  printRegionMap();
#endif
}

llvm::SmallVector<const RegionName *, 2>
Environment::getRegions(RegionMapKeyTy Key) const {
  llvm::SmallVector<const RegionName *, 2> Regions;
  auto It = regionMap.find(Key);
  if (It == regionMap.end())
    return Regions;

  Regions.reserve(It->second.size());
  for (const RegionName &RN : It->second)
    Regions.push_back(&RN);
  return Regions;
}

llvm::SmallVector<const RegionName *, 2>
Environment::getCallExprParamRegions(const CallExpr *CE, unsigned I) const {
  llvm::SmallVector<const RegionName *, 2> Regions;
  auto It = callExprParamRegions.find(CE);
  if (It == callExprParamRegions.end() || I >= It->second.size())
    return Regions;

  Regions.reserve(It->second[I].size());
  for (const RegionName &RN : It->second[I])
    Regions.push_back(&RN);
  return Regions;
}

llvm::ArrayRef<Variance>
Environment::getDefVariances(const RecordDecl *RD) const {
  const RecordDecl *CanonicalRD = cast<RecordDecl>(RD->getCanonicalDecl());
  auto It = defVarianceMap.find(CanonicalRD);
  if (It == defVarianceMap.end())
    return {};
  return It->second;
}

const RecordRegionLayout &
Environment::getRecordRegionLayout(const RecordDecl *RD) const {
  return GetOrCreateRecordRegionLayout(Ctx, RD, recordRegionLayouts);
}

llvm::ArrayRef<unsigned>
Environment::getFieldRegionIndices(const FieldDecl *FD) const {
  return getRecordRegionLayout(FD->getParent()).getFieldRegionIndices(FD);
}

#if DEBUG_PRINT
void Environment::printRegionMapKey(RegionMapKeyTy Key) const {
  if (Key.is<const Decl *>()) {
    const Decl *D = Key.get<const Decl *>();
    llvm::outs() << "Decl(" << D->getDeclKindName();
    if (const auto *ND = dyn_cast<NamedDecl>(D)) {
      if (!ND->getName().empty())
        llvm::outs() << " " << ND->getName();
    }
    llvm::outs() << ")";
    return;
  }

  const Stmt *S = Key.get<const Stmt *>();
  llvm::outs() << "Stmt(" << S->getStmtClassName() << ")";
}

void Environment::printRegionMapValue(const RegionMapValueTy &Regions) const {
  llvm::outs() << "[";
  for (unsigned I = 0, E = Regions.size(); I != E; ++I) {
    if (I != 0)
      llvm::outs() << ", ";
    llvm::outs() << Regions[I];
  }
  llvm::outs() << "]";
}

void Environment::printRegionMap() const {
  llvm::outs() << "========== RegionMap ==========\n";
  if (regionMap.empty()) {
    llvm::outs() << "  <empty>\n";
    return;
  }

  for (const auto &Entry : regionMap) {
    llvm::outs() << "  ";
    printRegionMapKey(Entry.first);
    llvm::outs() << " => ";
    printRegionMapValue(Entry.second);
    llvm::outs() << "\n";
  }
}
#endif

inline Variance variance(BorrowKind BK) {
  if (BK == BorrowKind::Mut)
    return Variance::In;
  return Variance::Co;
}

inline Variance invert(Variance V) {
  switch (V) {
  case Variance::Co:
    return Variance::Contra;
  case Variance::Contra:
    return Variance::Co;
  case Variance::In:
    return Variance::In;
  case Variance::Bi:
    return Variance::Bi;
  }
  llvm_unreachable("unknown variance");
}

Variance xform(Variance Ctx, Variance V) {
  switch (Ctx) {
  case Variance::Co:
    return V;
  case Variance::Contra:
    return invert(V);
  case Variance::In:
    return Variance::In;
  case Variance::Bi:
    return Variance::Bi;
  }
  llvm_unreachable("unknown variance");
}

/// Compute the greatest lower bound in the variance lattice:
///
///          Bi (top)
///         /  \
///       Co  Contra
///         \  /
///          In (bottom)
inline Variance glb(Variance A, Variance B) {
  if (A == B)
    return A;
  if (A == Variance::Bi)
    return B;
  if (B == Variance::Bi)
    return A;
  return Variance::In;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &OS, Variance V) {
  switch (V) {
  case Variance::Co:
    return OS << "Covariant";
  case Variance::Contra:
    return OS << "Contravariant";
  case Variance::In:
    return OS << "Invariant";
  case Variance::Bi:
    return OS << "Bivariant";
  }
  llvm_unreachable("unknown variance");
}

} // namespace borrow
} // namespace clang

namespace {
/// Given a statement, returns the corresponding (defs, uses).
///
/// The `defs` contains variables whose current value is completely
/// overwritten, and the `uses` contains variables whose current value is used.
/// Note that a variable may exist in both sets.
class DefUse : public clang::StmtVisitor<DefUse> {
  enum { None, Def, Use } Action;
  bool isAssign = false;
  llvm::SmallVector<VarDecl *> defs;
  llvm::SmallVector<VarDecl *> uses;

public:
  DefUse(Stmt *S) {
    Action = isa<Expr>(S) ? Use : None;
    Visit(S);
  }

  const llvm::SmallVector<VarDecl *> &getDefs() const { return defs; }
  const llvm::SmallVector<VarDecl *> &getUses() const { return uses; }

  void VisitBinaryOperator(BinaryOperator *BO);
  void VisitBinAssign(BinaryOperator *BO);
  void VisitCallExpr(CallExpr *CE);
  void VisitDeclRefExpr(DeclRefExpr *DRE);
  void VisitDeclStmt(DeclStmt *DS);
  void VisitMemberExpr(MemberExpr *ME);
  void VisitReturnStmt(ReturnStmt *RS);
  void VisitStmt(Stmt *S);
  void VisitStmtExpr(StmtExpr *SE);
  void VisitUnaryDeref(UnaryOperator *UO);
  void VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *UE);
  void VisitUnaryOperator(UnaryOperator *UO);
  void VisitArraySubscriptExpr(ArraySubscriptExpr *ASE);
};
} // namespace

void DefUse::VisitBinaryOperator(BinaryOperator *BO) {
  // Logical operands are represented by separate CFG elements after the
  // prologue wraps them in StmtExprs.
  if (BO->isLogicalOp())
    return;

  auto Opcode = BO->getOpcode();
  if ((Opcode >= BO_Mul && Opcode <= BO_Shr) ||
      (Opcode >= BO_And && Opcode <= BO_LOr) ||
      (Opcode >= BO_LT && Opcode <= BO_NE)) {
    Action = Use;
    Visit(BO->getLHS());
    Visit(BO->getRHS());
  } else if (Opcode >= BO_MulAssign && Opcode <= BO_OrAssign) {
    Action = Def;
    Visit(BO->getLHS());
    Action = Use;
    Visit(BO->getLHS());
    Visit(BO->getRHS());
  }
}

void DefUse::VisitBinAssign(BinaryOperator *BO) {
  Action = Def;
  llvm::SaveAndRestore<bool> save_is_assign(isAssign, true);
  Visit(BO->getLHS());
  Action = Use;
  Visit(BO->getRHS());
}

void DefUse::VisitCallExpr(CallExpr *CE) {
  Action = Use;
  if (!CE->getDirectCallee()) {
    Visit(CE->getCallee());
  }
  for (Expr *E : CE->arguments()) {
    Visit(E);
  }
}

void DefUse::VisitDeclRefExpr(DeclRefExpr *DRE) {
  if (VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
    if (Action == Def) {
      defs.push_back(VD);
    } else if (Action == Use) {
      uses.push_back(VD);
    }
  }
}

void DefUse::VisitDeclStmt(DeclStmt *DS) {
  Decl *D = DS->getSingleDecl();
  if (VarDecl *VD = dyn_cast<VarDecl>(D)) {
    defs.push_back(VD);
    if (VD->hasInit()) {
      Action = Use;
      Visit(VD->getInit());
    }
  }
}

void DefUse::VisitMemberExpr(MemberExpr *ME) {
  /// When you have `p = ...`, which variable is reassigned?
  /// If `p` is `x`, then `x` is. Otherwise, nothing.
  /// When you have `p = ...`, which variable is read?
  /// If `p` is `x.a`, then `x` is. Otherwise, nothing.
  if (Action == Def && isAssign)
    Action = Use;
  if (Action == Use)
    Visit(ME->getBase());
}

void DefUse::VisitReturnStmt(ReturnStmt *RS) {
  Action = Use;
  if (Expr *RV = RS->getRetValue()) {
    Visit(RV);
  }
}

void DefUse::VisitStmt(Stmt *S) {
  for (auto *C : S->children()) {
    if (C)
      Visit(C);
  }
}

// A StmtExpr body is represented by separate CFG elements. At the enclosing
// CFG site, only its non-void result is consumed.
void DefUse::VisitStmtExpr(StmtExpr *SE) {
  if (SE->getType()->isVoidType())
    return;
  Visit(SE->getSubStmt()->getStmtExprResult());
}

void DefUse::VisitUnaryDeref(UnaryOperator *UO) {
  Action = Use;
  Visit(UO->getSubExpr());
}

void DefUse::VisitUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *UE) {
  (void)UE;
}

void DefUse::VisitUnaryOperator(UnaryOperator *UO) {
  if (UO->isIncrementDecrementOp()) {
    Action = Def;
    Visit(UO->getSubExpr());
    Action = Use;
    Visit(UO->getSubExpr());
  } else {
    Visit(UO->getSubExpr());
  }
}

void DefUse::VisitArraySubscriptExpr(ArraySubscriptExpr *ASE) {
  if (Action == Use || Action == Def) {
    // Subscript reads its base pointer even on the assignment LHS (`p[i] = …`).
    Action = Use;
    Visit(ASE->getBase());
    Action = Use;
    Visit(ASE->getIdx());
  } else {
    VisitStmt(ASE);
  }
}

namespace clang {
namespace borrow {
/// Builds a borrow-check place from an expression known to denote a memory
/// location.
class PlaceBuilder {
  const ASTContext &Ctx;
  const Environment &Env;

  const Place *BuildVar(const ValueDecl *VD, QualType QT, SourceLocation Loc) {
    Ty *T = Ty::Create(Env, QT, Env.getRegions(VD));
    return Place::CreateVar(Ctx, VD, T, Loc);
  }

  const Place *BuildVar(const DeclRefExpr *DRE) {
    const ValueDecl *VD = DRE->getDecl();
    return BuildVar(VD, DRE->getType(), DRE->getLocation());
  }

  const Place *BuildField(const MemberExpr *ME) {
    const FieldDecl *FD = dyn_cast<FieldDecl>(ME->getMemberDecl());

    const Place *Base = ME->isArrow()
                            ? BuildDeref(ME->getBase(), ME->getOperatorLoc())
                            : Build(ME->getBase());
    return BuildField(Base, FD, ME->getMemberLoc());
  }

  const Place *BuildIndex(const ArraySubscriptExpr *ASE) {
    const Place *Base = Build(ASE->getBase());
    return BuildIndex(Base, ASE->getExprLoc());
  }

public:
  PlaceBuilder(const ASTContext &Ctx, const Environment &Env)
      : Ctx(Ctx), Env(Env) {}

  const Place *BuildField(const Place *Base, const FieldDecl *FD,
                          SourceLocation Loc) {
    Ty *T = Ty::CreateField(Env, Base->getType(), FD);
    return Place::CreateField(Ctx, Base, FD, T, Loc);
  }

  const Place *BuildIndex(const Place *Base, SourceLocation Loc) {
    const Ty *BaseTy = Base->getType();

    if (BaseTy->getKind() == Ty::TyKind::Array)
      return Place::CreateIndex(Ctx, Base, BaseTy->getElement(), Loc);

    if (BaseTy->getKind() == Ty::TyKind::Pointer)
      return Place::CreateIndex(Ctx, Base, BaseTy->getPointee(), Loc);

    llvm_unreachable("unexpected base type for array subscript place");
  }

  const Place *BuildDeref(const Place *Base, SourceLocation Loc) {
    const Ty *BaseTy = Base->getType();

    // Canonicalize `*arr` as `arr[_]` so an array element has one Place form.
    if (BaseTy->getKind() == Ty::TyKind::Array)
      return Place::CreateIndex(Ctx, Base, BaseTy->getElement(), Loc);

    if (BaseTy->getKind() == Ty::TyKind::Pointer) {
      // Dereferencing a function pointer still accesses the pointer value, not
      // a separate memory place for the function.
      if (BaseTy->getPointee()->getQualType()->isFunctionType())
        return Base;
      return Place::CreateDeref(Ctx, Base, BaseTy->getPointee(), Loc);
    }

    llvm_unreachable("unexpected base type for dereference place");
  }

  /// Build the place denoted by `*PointerOperand`. For pointer arithmetic, the
  /// pointer operand is the place root; the offset is materialized separately
  /// by the borrow-checker prologue so its uses are still visible.
  const Place *BuildDeref(const Expr *PointerOperand, SourceLocation Loc) {
    return BuildDeref(Build(PointerOperand), Loc);
  }

  const Place *Build(const VarDecl *VD, SourceLocation Loc) {
    return BuildVar(VD, VD->getType(), Loc);
  }

  const Place *Build(const Expr *E) {
    E = E->IgnoreParenImpCastsSafe();

    switch (E->getStmtClass()) {
    case Stmt::ArraySubscriptExprClass:
      return BuildIndex(cast<ArraySubscriptExpr>(E));
    // Pointer arithmetic changes the address offset but preserves the abstract
    // place root. The prologue materializes the offset separately, so follow
    // only the pointer operand here.
    case Stmt::BinaryOperatorClass: {
      const auto *BO = cast<BinaryOperator>(E);
      if (BO->getType()->isPointerType() &&
          (BO->getOpcode() == BO_Add || BO->getOpcode() == BO_Sub)) {
        const Expr *PointerOperand =
            BO->getLHS()->getType()->isPointerType() ? BO->getLHS()
                                                     : BO->getRHS();
        return Build(PointerOperand);
      }
      break;
    }
    case Stmt::DeclRefExprClass:
      return BuildVar(cast<DeclRefExpr>(E));
    // Values without an addressable storage location have no place.
    case Stmt::CharacterLiteralClass:
    case Stmt::CXXNullPtrLiteralExprClass:
    case Stmt::FloatingLiteralClass:
    case Stmt::ImplicitValueInitExprClass:
    case Stmt::IntegerLiteralClass:
    case Stmt::StringLiteralClass:
    case Stmt::UnaryExprOrTypeTraitExprClass:
      return nullptr;
    case Stmt::MemberExprClass:
      return BuildField(cast<MemberExpr>(E));
    case Stmt::UnaryOperatorClass: {
      const auto *UO = cast<UnaryOperator>(E);
      if (UO->getOpcode() == UO_Deref)
        return BuildDeref(UO->getSubExpr(), UO->getOperatorLoc());
      break;
    }
    default:
      break;
    }
    llvm_unreachable("unexpected expression for place");
  }
};
} // namespace borrow
} // namespace clang

namespace {

/// Lowers one CFG action site for the new action generation pipeline.
///
/// The concrete action representation is intentionally left open until the new
/// region/type query path is wired in.
class ActionGenerator : public ConstStmtVisitor<ActionGenerator> {
  const ASTContext &Ctx;
  const Environment &Env;
  PlaceBuilder PB;
  const Place *Dest = nullptr;
  const Place *StorageDeadPlace = nullptr;
  BorrowKind BK = BorrowKind::Mut;
  const Ty *BorrowTy = nullptr;
  const Ty *ReturnTy = nullptr;
  const CallExpr *Callee = nullptr;
  std::vector<ActionCall::Arg> Args;
  ActionAggregate::AggregateKind AK =
      ActionAggregate::AggregateKind::Copy;
  const Place *AggregateSource = nullptr;
  std::vector<ActionAggregate::Initializer> AggregateInitializers;
  std::vector<ActionAggregate::ImplicitReborrow> ImplicitReborrows;
  // The enclosing semantic node selects the action kind. Leaf expressions
  // only provide the source place and must not infer Assign versus Use from
  // whether a destination happens to have been built.
  Action::ActionKind Kind = Action::Noop;
  std::vector<const Place *> Sources;
  std::vector<const Action *> actions;

  /// Collect the mutable reborrows implicit in a whole-aggregate copy.
  /// Aggregate fields and elements are copied by value, while a pointer is a
  /// recursion boundary. A mutable borrow pointer therefore contributes one
  /// reborrow of its pointee and traversal stops at that pointer.
  void CollectImplicitReborrows(const Ty *DestTy, const Place *Source) {
    const Ty *SourceTy = Source->getType();
    assert(DestTy->getKind() == SourceTy->getKind() &&
           "aggregate source and destination type shapes should match");

    switch (DestTy->getKind()) {
    case Ty::TyKind::Pointer:
      if (DestTy->isBorrowPointer() &&
          DestTy->getBorrowKind() == BorrowKind::Mut) {
        assert(SourceTy->isBorrowPointer() &&
               SourceTy->getBorrowKind() == BorrowKind::Mut &&
               "mutable destination borrow should have a mutable source");
        const Place *BorrowedPlace =
            PB.BuildDeref(Source, Source->getLocation());
        ImplicitReborrows.push_back({DestTy, BorrowedPlace});
      }
      return;
    case Ty::TyKind::Struct: {
      const RecordType *RT = DestTy->getQualType()->getAs<RecordType>();
      assert(RT && "struct Ty should have a record QualType");
      for (const FieldDecl *FD : RT->getDecl()->getDefinition()->fields()) {
        if (!IsTrackedType(Ctx, FD->getType()))
          continue;
        const Ty *DestFieldTy = Ty::CreateField(Env, DestTy, FD);
        const Place *SourceField =
            PB.BuildField(Source, FD, Source->getLocation());
        CollectImplicitReborrows(DestFieldTy, SourceField);
      }
      return;
    }
    case Ty::TyKind::Array: {
      assert(SourceTy->getKind() == Ty::TyKind::Array &&
             "array destination should have an array source");
      const Place *SourceElement =
          PB.BuildIndex(Source, Source->getLocation());
      CollectImplicitReborrows(DestTy->getElement(), SourceElement);
      return;
    }
    case Ty::TyKind::Base:
      return;
    }
    llvm_unreachable("unknown type kind");
  }

  void BuildAction() {
    switch (Kind) {
    case Action::Init:
      assert(Dest && "initialization action should have a destination");
      actions.push_back(ActionInit::Create(Ctx, Dest, Sources));
      return;
    case Action::Assign:
      assert(Dest && "assignment action should have a destination");
      // A tracked assignment starts as Assign. If evaluating its RHS produces
      // no source place, no region relation or reborrow is needed; retain only
      // the destination write as an initialization action.
      if (Sources.empty()) {
        actions.push_back(ActionInit::Create(Ctx, Dest, Sources));
        return;
      }
      assert(Sources.size() == 1 &&
             "assignment action should have one source place");
      actions.push_back(ActionAssign::Create(Ctx, Dest, Sources.front()));
      return;
    case Action::Aggregate:
      assert(Dest && "aggregate action should have a destination");
      switch (AK) {
      case ActionAggregate::AggregateKind::Copy:
        assert(AggregateSource &&
               "aggregate copy should have a source place");
        actions.push_back(ActionAggregate::CreateCopy(
            Ctx, Dest, AggregateSource, ImplicitReborrows));
        return;
      case ActionAggregate::AggregateKind::Init:
        actions.push_back(ActionAggregate::CreateInit(
            Ctx, Dest, AggregateInitializers));
        return;
      }
      llvm_unreachable("unknown aggregate action kind");
    case Action::Use:
      actions.push_back(ActionUse::Create(Ctx, Sources));
      return;
    case Action::Borrow:
      assert(BorrowTy && Sources.size() == 1 &&
             "borrow action should have a type and one source");
      actions.push_back(
          ActionBorrow::Create(Ctx, Dest, BK, BorrowTy, Sources.front()));
      return;
    case Action::Return: {
      assert(ReturnTy && Sources.size() <= 1 &&
             "return action should have a type and at most one source");
      const Place *Value = Sources.empty() ? nullptr : Sources.front();
      actions.push_back(ActionReturn::Create(Ctx, ReturnTy, Value));
      return;
    }
    case Action::Call:
      assert(Callee && ReturnTy &&
             "call action should have a call site and return type");
      actions.push_back(ActionCall::Create(Ctx, Callee, ReturnTy, Dest, Args));
      return;
    case Action::StorageDead:
      assert(StorageDeadPlace &&
             "storage-dead action should have a place");
      actions.push_back(ActionStorageDead::Create(Ctx, StorageDeadPlace));
      return;
    case Action::Noop:
      actions.push_back(ActionNoop::Create(Ctx));
      return;
    }
  }

public:
  void VisitStmt(const Stmt *S) {
    for (auto *C : S->children()) {
      if (C)
        Visit(C);
    }
  }

  void VisitArraySubscriptExpr(const ArraySubscriptExpr *ASE) {
    Sources.push_back(PB.Build(ASE));
  }

  void VisitBinaryOperator(const BinaryOperator *BO) {
    if (BO->isMultiplicativeOp() || BO->isShiftOp() ||
        BO->isRelationalOp() || BO->isEqualityOp() || BO->isBitwiseOp()) {
      Kind = Action::Use;
      Visit(BO->getLHS());
      Visit(BO->getRHS());
      return;
    }

    // `+=` and `-=` are handled separately because they may update a pointer.
    // Other compound assignments read the old LHS value before overwriting it,
    // so collect the LHS as both the destination and a source.
    if (BO->isCompoundAssignmentOp() &&
        BO->getOpcode() != BO_AddAssign &&
        BO->getOpcode() != BO_SubAssign) {
      Kind = Action::Use;
      Dest = PB.Build(BO->getLHS());
      Sources.push_back(Dest);
      Visit(BO->getRHS());
      return;
    }

    llvm_unreachable("unsupported binary operator");
  }

  // Generate the call action and collect its result and arguments. An indirect
  // callee is additionally emitted as a separate use action.
  void VisitCallExpr(const CallExpr *CE) {
    Kind = Action::Call;
    Callee = CE;

    if (!CE->getDirectCallee())
      actions.push_back(ActionUse::Create(Ctx, {PB.Build(CE->getCallee())}));

    // Canonicalization strips BSC qualifiers required by parameter Ty values.
    QualType CalleeTy = CE->getCallee()->getType();
    if (CalleeTy->isFunctionPointerType())
      CalleeTy = CalleeTy->getPointeeType();
    const FunctionType *FT = CalleeTy->castAs<FunctionType>();
    const FunctionProtoType *FPT = dyn_cast<FunctionProtoType>(FT);

    ReturnTy = Ty::Create(Env, FT->getReturnType(), Env.getRegions(CE));

    Args.reserve(CE->getNumArgs());
    for (unsigned I = 0; I < CE->getNumArgs(); ++I) {
      const Ty *ParamTy = nullptr;
      if (FPT && I < FPT->getNumParams()) {
        QualType ParamQT = FPT->getParamType(I);
        ParamTy = Ty::Create(Env, ParamQT,
                             Env.getCallExprParamRegions(CE, I));
      }
      Args.push_back({ParamTy, PB.Build(CE->getArg(I))});
    }
  }

  void VisitCompoundLiteralExpr(const CompoundLiteralExpr *CLE) {
    Visit(CLE->getInitializer());
  }

  void VisitCStyleCastExpr(const CStyleCastExpr *CSCE) {
    QualType TargetTy = CSCE->getType();
    const Expr *SubExpr = CSCE->getSubExpr();

    if (TargetTy.isBorrowQualified()) {
      // Casting to a borrow pointer reborrows the pointed-to place.
      Kind = Action::Borrow;
      BK = TargetTy.isConstBorrow() ? BorrowKind::Shared : BorrowKind::Mut;
      Sources.push_back(PB.BuildDeref(SubExpr, CSCE->getExprLoc()));
      BorrowTy = Ty::CreatePointer(
          Ctx, TargetTy, Env.getRegions(CSCE).front(),
          Sources.back()->getType());
      return;
    }

    if (Kind == Action::Noop)
      Kind = Action::Use;
    Visit(SubExpr);
  }

  void VisitDeclRefExpr(const DeclRefExpr *DRE) {
    Sources.push_back(PB.Build(DRE));
  }

  void VisitDeclStmt(const DeclStmt *DS) {
    // Note: the construction of CFG ensures that there is only one declaration
    // in the DeclStmt.
    const Decl *D = DS->getSingleDecl();
    if (const VarDecl *VD = dyn_cast<VarDecl>(D)) {
      if (!VD->hasInit()) {
        Kind = Action::Noop;
        return;
      }

      QualType DestTy = VD->getType();
      bool IsTracked = IsTrackedType(Ctx, DestTy);
      bool IsAggregate =
          DestTy->getAs<RecordType>() || DestTy->isArrayType();

      // Tracked aggregates start as whole-place copies. Init-list and other
      // specialized initializers may replace this action while being visited.
      if (IsTracked && IsAggregate) {
        Kind = Action::Aggregate;
        AK = ActionAggregate::AggregateKind::Copy;
      } else if (IsTracked) {
        Kind = Action::Assign;
      } else {
        Kind = Action::Init;
      }

      Dest = PB.Build(VD, VD->getLocation());
      Visit(VD->getInit());

      // Collect implicit reborrows.
      if (Kind == Action::Aggregate &&
          AK == ActionAggregate::AggregateKind::Copy) {
        assert(Sources.size() == 1 &&
               "aggregate copy should have one source place");
        AggregateSource = Sources.front();
        CollectImplicitReborrows(Dest->getType(), AggregateSource);
      }
    }
  }

  void VisitInitListExpr(const InitListExpr *ILE) {
    if (!IsTrackedType(Ctx, ILE->getType())) {
      for (const Expr *Init : ILE->inits())
        Visit(Init);
      return;
    }

    assert(Dest && "tracked initializer list should have a destination");
    Kind = Action::Aggregate;
    AK = ActionAggregate::AggregateKind::Init;

    // Non-place initializers carry no borrow information and are omitted.
    // Handle struct initialization.
    if (const RecordType *RT = ILE->getType()->getAs<RecordType>()) {
      const RecordDecl *RD = RT->getDecl()->getDefinition();
      unsigned InitIndex = 0;
      for (const FieldDecl *FD : RD->fields()) {
        if (FD->isUnnamedBitfield())
          continue;
        if (InitIndex == ILE->getNumInits())
          break;
        const Expr *Init = ILE->getInit(InitIndex++);
        if (const Place *Value = PB.Build(Init))
          AggregateInitializers.push_back(
              {Ty::CreateField(Env, Dest->getType(), FD), Value});
      }
      return;
    }

    // Handle array initialization.
    if (ILE->getType()->isArrayType()) {
      for (const Expr *Init : ILE->inits()) {
        if (const Place *Value = PB.Build(Init))
          AggregateInitializers.push_back(
              {Dest->getType()->getElement(), Value});
      }
      return;
    }

    llvm_unreachable("unexpected tracked initializer-list type");
  }

  void VisitMemberExpr(const MemberExpr *ME) {
    Sources.push_back(PB.Build(ME));
  }

  void VisitReturnStmt(const ReturnStmt *RS) {
    Kind = Action::Return;
    unsigned NumReturnRegions =
        ComputeNumRegions(Ctx, Env.fd.getReturnType());
    // A tracked return has at most one region and shares the first free region
    // with the outermost region of every tracked parameter.
    assert(NumReturnRegions <= Env.freeRegions.size() &&
           "return type should have precomputed free regions");
    llvm::SmallVector<const RegionName *, 2> ReturnRegions;
    for (unsigned I = 0; I < NumReturnRegions; ++I)
      ReturnRegions.push_back(&Env.freeRegions[I]);
    ReturnTy = Ty::Create(Env, Env.fd.getReturnType(), ReturnRegions);

    if (const Expr *RV = RS->getRetValue())
      Sources.push_back(PB.Build(RV));
  }

  // A StmtExpr body is represented by separate CFG elements. Do not extract
  // those child actions again when the StmtExpr is an operand of this site;
  // only its non-void result is consumed here.
  void VisitStmtExpr(const StmtExpr *SE) {
    if (SE->getType()->isVoidType())
      return;
    Visit(SE->getSubStmt()->getStmtExprResult());
  }

  // Skip visiting UE's children to avoid treating them as accesses.
  // For example, `sizeof(x)` is not an access to `x`.
  void VisitUnaryExprOrTypeTraitExpr(const UnaryExprOrTypeTraitExpr *UE) {
    (void)UE;
  }

  void VisitUnaryOperator(const UnaryOperator *UO) {
    switch (UO->getOpcode()) {
    case UO_Plus:
    case UO_Minus:
    case UO_Not:
    case UO_LNot:
      if (Kind == Action::Noop)
        Kind = Action::Use;
      Visit(UO->getSubExpr());
      return;
    default:
      return;
    }
  }

  void VisitBinAdd(const BinaryOperator *BO) {
    if (BO->getType().isBorrowQualified()) {
      // Only the pointer operand contributes regions to borrow pointer
      // arithmetic.
      Visit(BO->getLHS()->getType()->isPointerType() ? BO->getLHS()
                                                     : BO->getRHS());
      return;
    }

    Visit(BO->getLHS());
    Visit(BO->getRHS());
  }

  void VisitBinAddAssign(const CompoundAssignOperator *CAO) {
    Dest = PB.Build(CAO->getLHS());
    Sources.push_back(Dest);
    if (CAO->getLHS()->getType().isBorrowQualified()) {
      // The prologue materializes the non-borrow operand, so its evaluation is
      // checked at the temporary declaration.
      Kind = Action::Assign;
      return;
    }

    Kind = Action::Init;
    Visit(CAO->getRHS());
  }

  void VisitBinAssign(const BinaryOperator *BO) {
    QualType DestTy = BO->getLHS()->getType();
    bool IsTracked = IsTrackedType(Ctx, DestTy);
    bool IsAggregate =
        DestTy->getAs<RecordType>() || DestTy->isArrayType();

    // Tracked aggregates start as whole-place copies. A compound literal may
    // replace this with element-wise aggregate initialization while visiting
    // the right-hand side.
    if (IsTracked && IsAggregate) {
      Kind = Action::Aggregate;
      AK = ActionAggregate::AggregateKind::Copy;
    } else if (IsTracked) {
      Kind = Action::Assign;
    } else {
      Kind = Action::Init;
    }

    Dest = PB.Build(BO->getLHS());
    Visit(BO->getRHS());

    // Collect implicit reborrows.
    if (Kind == Action::Aggregate &&
        AK == ActionAggregate::AggregateKind::Copy) {
      assert(Sources.size() == 1 &&
             "aggregate copy should have one source place");
      AggregateSource = Sources.front();
      CollectImplicitReborrows(Dest->getType(), AggregateSource);
    }
  }

  void VisitBinComma(const BinaryOperator *BO) {
    (void)BO;
    llvm_unreachable(
        "comma operator should be lowered by BorrowCheckerPrologue");
  }

  void VisitBinLAnd(const BinaryOperator *BO) {
    (void)BO;
  }

  void VisitBinLOr(const BinaryOperator *BO) {
    (void)BO;
  }

  void VisitBinSub(const BinaryOperator *BO) {
    if (BO->getType().isBorrowQualified()) {
      // Only the pointer operand contributes regions to borrow pointer
      // arithmetic.
      Visit(BO->getLHS());
      return;
    }

    Visit(BO->getLHS());
    Visit(BO->getRHS());
  }

  void VisitBinSubAssign(const CompoundAssignOperator *CAO) {
    Dest = PB.Build(CAO->getLHS());
    Sources.push_back(Dest);
    if (CAO->getLHS()->getType().isBorrowQualified()) {
      // The prologue materializes the non-borrow operand, so its evaluation is
      // checked at the temporary declaration.
      Kind = Action::Assign;
      return;
    }

    Kind = Action::Init;
    Visit(CAO->getRHS());
  }

  void VisitUnaryAddrConst(const UnaryOperator *UO) {
    Kind = Action::Borrow;
    BK = BorrowKind::Shared;
    Sources.push_back(PB.Build(UO->getSubExpr()));
    BorrowTy = Ty::CreatePointer(
        Ctx, UO->getType(), Env.getRegions(UO).front(),
        Sources.back()->getType());
  }

  void VisitUnaryAddrConstDeref(const UnaryOperator *UO) {
    Kind = Action::Borrow;
    BK = BorrowKind::Shared;
    Sources.push_back(
        PB.BuildDeref(UO->getSubExpr(), UO->getOperatorLoc()));
    BorrowTy = Ty::CreatePointer(
        Ctx, UO->getType(), Env.getRegions(UO).front(),
        Sources.back()->getType());
  }

  void VisitUnaryAddrMut(const UnaryOperator *UO) {
    Kind = Action::Borrow;
    BK = BorrowKind::Mut;
    Sources.push_back(PB.Build(UO->getSubExpr()));
    BorrowTy = Ty::CreatePointer(
        Ctx, UO->getType(), Env.getRegions(UO).front(),
        Sources.back()->getType());
  }

  void VisitUnaryAddrMutDeref(const UnaryOperator *UO) {
    Kind = Action::Borrow;
    BK = BorrowKind::Mut;
    Sources.push_back(
        PB.BuildDeref(UO->getSubExpr(), UO->getOperatorLoc()));
    BorrowTy = Ty::CreatePointer(
        Ctx, UO->getType(), Env.getRegions(UO).front(),
        Sources.back()->getType());
  }

  void VisitUnaryAddrOf(const UnaryOperator *UO) {
    if (Kind == Action::Noop)
      Kind = Action::Use;
    Sources.push_back(PB.Build(UO->getSubExpr()));
  }

  void VisitUnaryDeref(const UnaryOperator *UO) {
    Sources.push_back(
        PB.BuildDeref(UO->getSubExpr(), UO->getOperatorLoc()));
  }

  // Model the operand's read-modify-write and, when the surrounding expression
  // needs the result, the value assigned to its destination. A postfix
  // expression produces the old value first, while a prefix expression
  // produces the value after the modification.
  void VisitUnaryPostDec(const UnaryOperator *UO) {
    const Place *P = PB.Build(UO->getSubExpr());
    if (Dest)
      actions.push_back(ActionAssign::Create(Ctx, Dest, P));
    actions.push_back(ActionAssign::Create(Ctx, P, P));
    Kind = Action::Noop;
  }

  void VisitUnaryPostInc(const UnaryOperator *UO) {
    const Place *P = PB.Build(UO->getSubExpr());
    if (Dest)
      actions.push_back(ActionAssign::Create(Ctx, Dest, P));
    actions.push_back(ActionAssign::Create(Ctx, P, P));
    Kind = Action::Noop;
  }

  void VisitUnaryPreDec(const UnaryOperator *UO) {
    const Place *P = PB.Build(UO->getSubExpr());
    actions.push_back(ActionAssign::Create(Ctx, P, P));
    if (Dest)
      actions.push_back(ActionAssign::Create(Ctx, Dest, P));
    Kind = Action::Noop;
  }

  void VisitUnaryPreInc(const UnaryOperator *UO) {
    const Place *P = PB.Build(UO->getSubExpr());
    actions.push_back(ActionAssign::Create(Ctx, P, P));
    if (Dest)
      actions.push_back(ActionAssign::Create(Ctx, Dest, P));
    Kind = Action::Noop;
  }

  ActionGenerator(const ASTContext &Ctx, const Environment &Env, const Stmt *S,
                  const VarDecl *LifetimeEndsVD, SourceLocation LifetimeEndsLoc)
      : Ctx(Ctx), Env(Env), PB(Ctx, Env) {
    assert((!S || !LifetimeEndsVD) &&
           "statement and lifetime-end variable are mutually exclusive");

    if (LifetimeEndsVD) {
      Kind = Action::StorageDead;
      StorageDeadPlace = PB.Build(LifetimeEndsVD, LifetimeEndsLoc);
    } else if (S) {
      Visit(S);
    }
  }

  std::vector<const Action *> takeActions() {
    BuildAction();
    return std::move(actions);
  }
};
} // namespace

//===----------------------------------------------------------------------===//
//                         Operations on RegionName
//===----------------------------------------------------------------------===//

unsigned RegionName::Cnt = 0;

const RegionName &RegionName::GetUntracked() {
  static const RegionName RN(std::string(NamePrefix) + "untracked");
  return RN;
}

//===----------------------------------------------------------------------===//
//                            Achors for Actions
//===----------------------------------------------------------------------===//

void Action::anchor() {}
void ActionNoop::anchor() {}
void ActionInit::anchor() {}
void ActionBorrow::anchor() {}
void ActionAssign::anchor() {}
void ActionAggregate::anchor() {}
void ActionUse::anchor() {}
void ActionCall::anchor() {}
void ActionReturn::anchor() {}
void ActionStorageDead::anchor() {}

//===----------------------------------------------------------------------===//
//                       Query functions on Environment
//===----------------------------------------------------------------------===//

/// Given a point, returns the set of all its successor points in the CFG.
llvm::SmallVector<Point> Environment::SuccessorPoints(Point point) const {
  llvm::SmallVector<Point> Succs;

  const CFGBlock *block = *(cfg.nodes_begin() + point.blockID);
  if (point.index != block->size()) {
    Succs.push_back(point.Next(block));
  } else {
    llvm::DenseSet<const CFGBlock *> succs(block->succ_begin(),
                                           block->succ_end());
    bool changed = true;
    while (changed) {
      changed = false;
      size_t oldSize = succs.size();
      // Note: to avoid iterator invalidation
      llvm::DenseSet<const CFGBlock *> newBlocks;
      for (const CFGBlock *succ : succs) {
        if (succ && succ->empty()) {
          newBlocks.insert(succ->succ_begin(), succ->succ_end());
        }
      }
      succs.insert(newBlocks.begin(), newBlocks.end());
      if (succs.size() != oldSize)
        changed = true;
    }
    for (const CFGBlock *succ : succs) {
      if (succ && !succ->empty())
        Succs.push_back(Point::MakeFirstPoint(succ));
    }
  }

  return Succs;
}

//===----------------------------------------------------------------------===//
//             Operation functions on InferenceContext and DFS
//===----------------------------------------------------------------------===//

RegionVariable InferenceContext::AddVar(RegionName Name) {
  size_t index = definitions.size();
#if DEBUG_PRINT
  llvm::outs() << Name << " => " << RegionVariable(index) << '\n';
#endif
  definitions.push_back(VarDefinition(Name, Region(), false));
  return RegionVariable(index);
}

void InferenceContext::AddLivePoint(RegionVariable RV, Point P) {
  VarDefinition &definition = definitions[RV.index];
  if (definition.name.isUntracked())
    return;
#if DEBUG_PRINT
  llvm::outs() << "AddLivePoint: " << RV << " @ " << P << '\n';
#endif
  if (definition.value.AddPoint(P)) {
    if (definition.capped) {
      llvm_unreachable("Free region should not grow anymore!");
    }
  }
}

void InferenceContext::AddEndRegion(RegionVariable RV, RegionName RN) {
  VarDefinition &definition = definitions[RV.index];
  if (definition.name.isUntracked())
    return;
#if DEBUG_PRINT
  llvm::outs() << "AddEndRegion: " << RV << " @ End(" << RN.Name << ")\n";
#endif
  if (definition.value.AddEndRegion(RN)) {
    if (definition.capped) {
      llvm_unreachable("Free region should not grow anymore!");
    }
  }
}

void InferenceContext::AddOutLives(RegionVariable Sup, RegionVariable Sub,
                                   Point P, SourceLocation DiagLoc) {
  // A raw-pointer boundary erases lifetime relations on either side.
  if (definitions[Sup.index].name.isUntracked() ||
      definitions[Sub.index].name.isUntracked())
    return;
#if DEBUG_PRINT
  llvm::outs() << "AddOutLives: " << Sub << " : " << Sup << " @ " << P
               << '\n';
#endif
  constraints.push_back(Constraint(Sub, Sup, P, DiagLoc));
}

/// Inference algorithm, which is implemented based on fixed-point iteration.
///
/// During fixed-point iteration, the algorithm solves inference constraints
/// and updates the regions of region variables.
llvm::SmallVector<BorrowDiagInfo>
InferenceContext::Solve(const Environment &env) {
  llvm::SmallVector<BorrowDiagInfo> Diags;
  bool changed = true;
  DFS dfs(env);
  while (changed) {
    changed = false;

    for (Constraint &constraint : constraints) {
      const Region &Sup = definitions[constraint.sup.index].value;
      VarDefinition &SubDef = definitions[constraint.sub.index];
#if DEBUG_PRINT
      llvm::outs() << "constraint: " << constraint << '\n';
      llvm::outs() << "  sup (before): " << Sup << '\n';
      llvm::outs() << "  sub (before): " << SubDef.value << '\n';
#endif

      // DFS from the start point of constraint.
      if (dfs.Copy(Sup, SubDef.value, constraint.point)) {
        changed = true;

        if (SubDef.capped) {
          Diags.push_back(BorrowDiagInfo(BorrowDiagKind::LifetimeNotLong,
                                         constraint.diagLoc));
        }
      }

#if DEBUG_PRINT
      llvm::outs() << "  sub (after) : " << SubDef.value << '\n';
      llvm::outs() << "  changed     : " << (changed ? "true" : "false")
                   << '\n';
#endif
    }
#if DEBUG_PRINT
    llvm::outs() << '\n';
#endif
  }
  return Diags;
}

/// Update `To` using `From`， starting DFS from `StartPoint` until the visited
/// is not in `From` or has already been visited.
bool DFS::Copy(const Region &From, Region &To, Point StartPoint) {
  bool changed = false;

  stack.clear();
  visited.clear();

  stack.push_back(StartPoint);
  while (!stack.empty()) {
    Point p = stack.back();
    stack.pop_back();

#if DEBUG_PRINT
    llvm::outs() << "    dfs: p=" << p << '\n';
#endif

    if (!From.MayContain(p)) {
#if DEBUG_PRINT
      llvm::outs() << "      not in From-Region\n";
#endif
      continue;
    }

    if (!visited.insert(p).second) {
#if DEBUG_PRINT
      llvm::outs() << "      already visited\n";
#endif
      continue;
    }

    changed |= To.AddPoint(p);

    llvm::SmallVector<Point> SuccessorPoints = env.SuccessorPoints(p);
    if (SuccessorPoints.empty()) {
      for (const RegionName &RN : From.getEndRegions()) {
        changed |= To.AddEndRegion(RN);
      }
    } else {
      stack.insert(stack.end(), SuccessorPoints.begin(), SuccessorPoints.end());
    }
  }

  return changed;
}

//===----------------------------------------------------------------------===//
//                           Liveness computations
//===----------------------------------------------------------------------===//

/// Iterates until a fixed point, computing live variables on the entry of each
/// basic block.
///
/// Note that an empty callback is sufficient when computing liveness.
void Liveness::Compute() {
  LivenessFact fact;
  bool changed = true;
  while (changed) {
    changed = false;

    for (const CFGBlock *B : env.cfg.const_nodes()) {
      SimulateBlock(fact, B,
                    [](auto _p, auto _a, auto _s, auto _v, auto _l) {});
      changed |= SetFrom(liveness[B], fact);
    }
  }
}

template <typename CB>
void Liveness::SimulateBlock(LivenessFact &fact, const CFGBlock *Block,
                             CB callback) {
  fact.clear();

  // Everything live in a successor is live at the exit of the block.
  for (auto succ : Block->succs()) {
    if (succ)
      SetFrom(fact, liveness[succ]);
  }

  // Walk backwards through the actions.
  for (CFGBlock::const_reverse_iterator it = Block->rbegin(),
                                        ei = Block->rend();
       it != ei; ++it) {
    const CFGElement &elem = *it;
    const Stmt *S = nullptr;
    const VarDecl *LifetimeEndsVD = nullptr;
    SourceLocation LifetimeEndsLoc;

    if (elem.getAs<CFGStmt>()) {
      S = elem.castAs<CFGStmt>().getStmt();
      // Get the def-use information of a given statement.
      DefUse DU(const_cast<Stmt *>(S));
      const llvm::SmallVector<VarDecl *> &defs = DU.getDefs();
      const llvm::SmallVector<VarDecl *> &uses = DU.getUses();

      // Anything we write to is no longer live.
      for (VarDecl *def : defs) {
        Kill(fact, def);
      }

      // Any variables we read from, we make live.
      for (VarDecl *use : uses) {
        Gen(fact, use);
      }
    }

    // There is no need to handle CFGLifetimeEnds when calculating liveness,
    // while it's necessary to handle CFGLifetimeEnds when populating inference,
    // so we need to get the `LifetimeEndsVD` and `LifetimeEndsLoc` here.
    if (elem.getAs<CFGLifetimeEnds>()) {
      LifetimeEndsVD = elem.castAs<CFGLifetimeEnds>().getVarDecl();
      LifetimeEndsLoc =
          elem.castAs<CFGLifetimeEnds>().getTriggerStmt()->getEndLoc();
    }

    Point point = Point::Create(Block, it);

    callback(point, S, fact, LifetimeEndsVD, LifetimeEndsLoc);
  }
}

/// Invokes callback once for each statement with:
///   a. the point of the statement;
///   b. the statement itself;
///   c. the set of live variables on entry to the statement.
///
/// Note that all constraints will be generated after the walk.
template <typename CB> void Liveness::Walk(CB callback) {
  LivenessFact fact;

  for (const CFGBlock *B : env.cfg.const_nodes()) {
    SimulateBlock(fact, B, callback);
  }
}

/// Given a LivenessFact, return all precomputed regions associated with its
/// live variables.
std::set<RegionName> Liveness::LiveRegions(const LivenessFact &liveFact) {
  std::set<RegionName> Regions;
  for (VarDecl *VD : liveFact)
    for (const RegionName *RN : env.getRegions(VD))
      Regions.insert(*RN);
  return Regions;
}

//===----------------------------------------------------------------------===//
//                         LoansInScope computations
//===----------------------------------------------------------------------===//

LoansInScope::LoansInScope(const Environment &env, const RegionCheck &rc)
    : env(env), rc(rc) {
  // Collect the full set of loans, including explicit loans and implicit
  // mutable reborrows from aggregate copies.
  for (const CFGBlock *Block : env.cfg.const_reverse_nodes()) {
    for (CFGBlock::const_iterator It = Block->begin(), End = Block->end();
         It != End; ++It) {
      Point P = Point::Create(Block, It);
      for (const Action *A : rc.getActionMap().at(P)) {
        if (A->getKind() == Action::Borrow) {
          const ActionBorrow *AB = llvm::cast<ActionBorrow>(A);
          const Region &R = rc.getRegion(AB->BorrowTy->getRegion());
          loans.push_back(Loan(P, AB->Source, AB->BK, R));
        } else if (A->getKind() == Action::Aggregate) {
          const ActionAggregate *AA = llvm::cast<ActionAggregate>(A);
          for (const ActionAggregate::ImplicitReborrow &Reborrow :
               AA->ImplicitReborrows) {
            const Region &R = rc.getRegion(Reborrow.DestTy->getRegion());
            loans.push_back(
                Loan(P, Reborrow.BorrowedPlace, BorrowKind::Mut, R));
          }
        }
      }
    }
  }

#if DEBUG_PRINT
  llvm::outs() << "loans: [\n";
  for (const Loan &loan : loans) {
    llvm::outs() << loan << ",\n";
  }
  llvm::outs() << "]\n";
#endif

  // Make a convenient hash map for getting the index of a loan based on where
  // it appears.
  for (const auto &IndexedLoan : llvm::enumerate(loans)) {
    loansByPoint[IndexedLoan.value().point].push_back(IndexedLoan.index());
  }

  // Iterates until a fixed point.
  Compute();
}

llvm::SmallVector<unsigned>
LoansInScope::LoansKilledByWriteTo(const Place *P) const {
  llvm::SmallVector<unsigned> LoanIndexes;

  // When an assignment like `a.b.c = ...` occurs, we kill all
  // the loans for `a.b.c` or some subplace like `a.b.c.d`, since
  // the place no longer evaluates to the same thing.
  for (const auto &IndexedLoan : llvm::enumerate(loans)) {
    for (const Place *Prefix : IndexedLoan.value().place->prefixes()) {
      if (Prefix->equals(P)) {
        LoanIndexes.push_back(IndexedLoan.index());
        break;
      }
    }
  }

  return LoanIndexes;
}

template <typename CB>
void LoansInScope::SimulateBlock(LoansFact &fact, const CFGBlock *Block,
                                 CB callback) {
  fact.clear();

  // Everything live at end of a pred is live at the entry of the block.
  for (auto pred : Block->preds()) {
    SetFrom(fact, loansInScopeAfterBlock[pred]);
  }

  // Walk forwards through the actions on by one.
  for (CFGBlock::const_iterator it = Block->begin(), ei = Block->end();
       it != ei; ++it) {
    Point point = Point::Create(Block, it);

    // kill any loans where `point` is not in their region
    for (unsigned loanIndex : LoansNotInScopeAt(point)) {
      Kill(fact, loanIndex);
    }

    // callback at start of the action
    callback(point, fact);

    // bring the loan into scope after the borrow
    if (loansByPoint.find(point) != loansByPoint.end()) {
      Gen(fact, loansByPoint[point]);
    }

    // Figure out which place is overwritten by this action; this may cancel out
    // some loans.
    const auto &actions = rc.getActionMap().at(point);
    for (const Action *action : actions) {
      llvm::Optional<const Place *> overwritten = action->OverWrites();
      if (overwritten.hasValue()) {
        const Place *overwrittenPlace = overwritten.getValue();
        for (unsigned loanIndex : LoansKilledByWriteTo(overwrittenPlace)) {
          Kill(fact, loanIndex);
        }
      }
    }
  }
}

/// Iterates until a fixed point, computing the loans in scope after each block
/// terminates.
void LoansInScope::Compute() {
  LoansFact fact;
  bool changed = true;
  while (changed) {
    changed = false;

    for (const CFGBlock *B : env.cfg.const_reverse_nodes()) {
      SimulateBlock(fact, B, [](auto _p, auto _s) {});
      changed |= SetFrom(loansInScopeAfterBlock[B], fact);
    }
  }
}

/// Invoke `callback` with the loans in scope at each point.
template <typename CB> void LoansInScope::Walk(CB callback) {
  llvm::SmallVector<Loan> loans;
  LoansFact fact;

  for (const CFGBlock *B : env.cfg.const_reverse_nodes()) {
    SimulateBlock(fact, B, [&](Point point, LoansFact &fact) {
      // Convert from the LoansFact into a vector of loans.
      loans.clear();
      for (const auto &IndexedLoan : llvm::enumerate(this->loans)) {
        if (fact.find(IndexedLoan.index()) != fact.end())
          loans.push_back(IndexedLoan.value());
      }

      // Invoke the callback, check actions at each point according to `loans`.
      callback(point, loans);
    });
  }
}

//===----------------------------------------------------------------------===//
//                      Check functions on BorrowCheck
//===----------------------------------------------------------------------===//

void BorrowCheck::CheckAction(const Action *A) {
  IsBorrow = false;
#if DEBUG_PRINT
  llvm::outs() << "CheckAction(" << A << ") @ " << point << '\n';
#endif
  // TODO: may use check shallow read to replace checkread in init, use, call and return
  switch (A->getKind()) {
  case Action::Init: {
    const ActionInit *AI = llvm::cast<ActionInit>(A);
    CheckShallowWrite(AI->Dest);
    for (const Place *P : AI->Sources) {
      QualType QT = P->getType()->getQualType();
      if (QT.isOwnedQualified() || QT->isMoveSemanticType())
        CheckMove(P);
      else
        CheckRead(P);
    }
    break;
  }
  case Action::Assign: {
    const ActionAssign *AA = llvm::cast<ActionAssign>(A);
    CheckShallowWrite(AA->Dest);
    CheckRead(AA->Source);
    break;
  }
  case Action::Aggregate: {
    const ActionAggregate *AA = llvm::cast<ActionAggregate>(A);
    CheckShallowWrite(AA->Dest);
    switch (AA->AK) {
    case ActionAggregate::AggregateKind::Copy: {
      QualType QT = AA->CopySource->getType()->getQualType();
      if (QT.isOwnedQualified() || QT->isMoveSemanticType()) {
        CheckMove(AA->CopySource);
      } else {
        CheckRead(AA->CopySource);
      }
      for (const ActionAggregate::ImplicitReborrow &Reborrow :
           AA->ImplicitReborrows) {
        CheckMutBorrow(Reborrow.BorrowedPlace);
      }
      break;
    }
    case ActionAggregate::AggregateKind::Init: {
      for (const ActionAggregate::Initializer &Initializer :
           AA->Initializers) {
        const Place *P = Initializer.Value;
        QualType QT = P->getType()->getQualType();
        if (QT.isOwnedQualified() || QT->isMoveSemanticType()) {
          CheckMove(P);
        } else {
          CheckRead(P);
        }
      }
      break;
    }
    }
    break;
  }
  case Action::Borrow: {
    const ActionBorrow *AB = llvm::cast<ActionBorrow>(A);
    CheckShallowWrite(AB->Dest);
    IsBorrow = true;
    if (AB->BK == BorrowKind::Shared)
      CheckRead(AB->Source);
    else
      CheckMutBorrow(AB->Source);
    break;
  }
  case Action::StorageDead: {
    const ActionStorageDead *ASD = llvm::cast<ActionStorageDead>(A);
    CheckStorageDead(ASD->P);
    break;
  }
  case Action::Use: {
    const ActionUse *AU = llvm::cast<ActionUse>(A);
    for (const Place *P : AU->Places) {
      QualType QT = P->getType()->getQualType();
      if (QT.isOwnedQualified() || QT->isMoveSemanticType())
        CheckMove(P);
      else
        CheckRead(P);
    }
    break;
  }
  case Action::Call: {
    const ActionCall *AC = llvm::cast<ActionCall>(A);
    if (AC->Result) {
      CheckShallowWrite(AC->Result);
    }
    for (const auto &Arg : AC->Args) {
      const Place *P = Arg.Value;
      if (!P)
        continue;
      QualType QT = P->getType()->getQualType();
      if (QT.isOwnedQualified() || QT->isMoveSemanticType())
        CheckMove(P);
      else
        CheckRead(P);
    }
    break;
  }
  case Action::Return: {
    const ActionReturn *AR = llvm::cast<ActionReturn>(A);
    if (const Place *P = AR->Value) {
      QualType QT = P->getType()->getQualType();
      if (QT.isOwnedQualified() || QT->isMoveSemanticType())
        CheckMove(P);
      else
        CheckRead(P);
    }
    break;
  }
  case Action::Noop:
    break;
  }
}

void BorrowCheck::CheckBorrows(Depth Depth, Mode AccessMode,
                               const Place *P) {
  llvm::SmallVector<const Loan *> Loans;
  switch (Depth) {
  case Depth::Shallow:
    Loans = FindLoansThatFreeze(P);
    break;
  case Depth::Deep:
    Loans = FindLoansThatIntersect(P);
    break;
  }

  for (const Loan *L : Loans) {
    switch (AccessMode) {
    case Mode::Read:
      switch (L->kind) {
      case BorrowKind::Shared:
        /* Ok */
        break;
      case BorrowKind::Mut:
        if (IsBorrow)
          reporter.ForImmutWhenMut(P->getLocation(), P,
                                   L->place->getLocation());
        else
          reporter.ForRead(P->getLocation(), P, L->place->getLocation(),
                           L->place);
        return;
      }
      break;
    case Mode::Write:
      if (Depth == Depth::Shallow)
        reporter.ForWrite(P->getLocation(), P, L->place->getLocation());
      else if (L->kind == BorrowKind::Mut)
        reporter.ForMultiMut(P->getLocation(), P, L->place->getLocation());
      else
        reporter.ForMutWhenImmut(P->getLocation(), P,
                                 L->place->getLocation());
      return;
    }
  }
}

/// Cannot move from a place `P` if:
/// - `P` is borrowed;
/// - some subplace `P.foo` is borrowed;
/// - some prefix of `P` is borrowed.
///
/// Note that it is stricter than both write and storage dead. In particular,
/// you can write to a variable `x` that contains an `&mut` value when `*x` is
/// borrowed, but you cannot move `x`. This is because moving it would make the
/// `&mut` variable in the new location, but writing (and storage dead) both
/// kill it forever.
void BorrowCheck::CheckMove(const Place *P) {
  for (const Loan *L : FindLoansThatIntersect(P))
    reporter.ForMove(P->getLocation(), P, L->place->getLocation(), L->place);
}

/// `&mut x` may mutate `x`, but it can also *read* from `x`, and mutate things
/// reachable from `x`.
void BorrowCheck::CheckMutBorrow(const Place *P) {
  CheckBorrows(Depth::Deep, Mode::Write, P);
}

/// `use(x)` may access `x` and (by going through the produced value) anything
/// reachable from `x`.
void BorrowCheck::CheckRead(const Place *P) {
  CheckBorrows(Depth::Deep, Mode::Read, P);
}

/// `x = ...` overwrites `x` (without reading it) and prevents any further
/// reads from that place.
void BorrowCheck::CheckShallowWrite(const Place *P) {
  CheckBorrows(Depth::Shallow, Mode::Write, P);
}

/// Cannot free a local variable `var` if:
/// -data interior to `var` is borrowed.
///
/// In particular, having something like `*var` borrowed is ok.
void BorrowCheck::CheckStorageDead(const Place *P) {
  for (const Loan *L : FindLoansThatFreeze(P))
    reporter.ForStorageDead(P->getLocation(), L->place,
                            L->place->getLocation());
}

/// Helper for `CheckWrite` and `CheckStorageDead`.
///
/// Finds if there is a loan that "freezes" the given place -- that is, a
/// loan that would make modifying `P` (or freeing it) illegal.
/// This is slightly more permissive than the rules around move and reads,
/// precisely because overwriting or freeing `P` makes the previous value
/// unavailable from that point on.
llvm::SmallVector<const Loan *>
BorrowCheck::FindLoansThatFreeze(const Place *P) {
  llvm::SmallVector<const Loan *> Loans;
  llvm::SmallVector<const Place *> PlacePrefixes = P->prefixes();
  for (const Loan &L : loans) {
    bool NeedToInsert = false;
    llvm::SmallVector<const Place *> FrozenPlaces = FrozenByBorrowOf(L.place);
    // If you have borrowed `a.b`, this prevents writes to `a` or `a.b`:
    for (const Place *Frozen : FrozenPlaces) {
      if (Frozen->equals(P)) {
        NeedToInsert = true;
        break;
      }
    }
    // If you have borrowed `a.b`, this prevents writes to `a.b.c`:
    for (const Place *Prefix : PlacePrefixes) {
      if (Prefix->equals(L.place)) {
        NeedToInsert = true;
        break;
      }
    }
    if (NeedToInsert)
      Loans.push_back(&L);
  }
  return Loans;
}

/// A loan L *intersects* a place P if either:
///
/// - the loan is for the place P; or,
/// - the place P can be extended to reach the data in the loan; or,
/// - the loan place can be extended to reach the data in P.
///
/// So, for example, if the place P is `a.b.c`, then:
///
/// - a loan of `a.b.c` intersects P;
/// - a loan of `a.b.c.d` intersects P, because (e.g.) after reading P
///   you have also read `a.b.c.d`;
/// - a loan of `a.b` intersects P, because you can use the
///   reference to access the data at P.
llvm::SmallVector<const Loan *>
BorrowCheck::FindLoansThatIntersect(const Place *P) {
  llvm::SmallVector<const Loan *> Loans;
  llvm::SmallVector<const Place *> PlacePrefixes = P->prefixes();
  for (const Loan &L : loans) {
    bool NeedToInsert = false;
    // Accessing `a.b.c` intersects a loan of `a.b.c`, `a.b` and `a`.
    for (const Place *Prefix : PlacePrefixes) {
      if (Prefix->equals(L.place)) {
        NeedToInsert = true;
        break;
      }
    }
    /// Accessing `a.b.c` also intersects a loan of `a.b.c.d`.
    for (const Place *Prefix : L.place->supportingPrefixes()) {
      if (Prefix->equals(P)) {
        NeedToInsert = true;
        break;
      }
    }
    if (NeedToInsert)
      Loans.push_back(&L);
  }
  return Loans;
}

/// If `P` is borrowed, returns a vector of places which -- if
/// moved or if the storage went away -- would invalidate this
/// reference.
llvm::SmallVector<const Place *>
BorrowCheck::FrozenByBorrowOf(const Place *P) {
  llvm::SmallVector<const Place *> Places;
  while (true) {
    Places.push_back(P);
    switch (P->getKind()) {
    case Place::Kind::Var:
      return Places;
    case Place::Kind::Field:
      // If you have borrowed `a.b`, then writing to `a` would overwrite `a.b`,
      // which is disallowed.
      P = P->getBase();
      break;
    case Place::Kind::Deref:
    case Place::Kind::Index: {
      const Ty *BaseTy = P->getBase()->getType();
      // If you borrowed `*r`, writing to `r` does not actually affect the
      // memory at `*r`, so we can stop iterating backwards now.
      // For owned pointer `p`, if you have borrowed `*p`, then writing to `p`
      // would overwrite `*p`, which is also disallowed.
      if (BaseTy->getKind() == Ty::TyKind::Pointer &&
          !BaseTy->isOwnedPointer())
        return Places;
      P = P->getBase();
      break;
    }
    }
  }
}

//===----------------------------------------------------------------------===//
//               Operation and query functions on RegionCheck
//===----------------------------------------------------------------------===//

RegionCheck::RegionCheck(const FunctionDecl &fd, const CFG &cfg,
                         ASTContext &Ctx, BorrowDiagReporter &Reporter,
                         RegionGenerator &RG, DefVarianceAnalysis &DVA)
    : reporter(Reporter), env(fd, cfg, Ctx, RG, DVA) {
  MapRegionNamesToRegionVariables();
  InitFreeRegions();
  GenerateActions();
}

void RegionCheck::MapRegionNamesToRegionVariables() {
  for (const auto &Entry : env.regionMap) {
    for (RegionName RN : Entry.second) {
      createRegionVariable(RN);
    }
  }

  for (const auto &Entry : env.callExprParamRegions) {
    for (const Environment::RegionMapValueTy &Regions : Entry.second) {
      for (RegionName RN : Regions) {
        createRegionVariable(RN);
      }
    }
  }

  for (RegionName RN : env.freeRegions) {
    createRegionVariable(RN);
  }
}

void RegionCheck::PopulateInference(Liveness &liveness) {
  // Walk statements to generate liveness constraints, subtyping constraints
  // and reborrow constraints.
  liveness.Walk([&, this](Point point, const Stmt *S,
                          llvm::DenseSet<VarDecl *> liveOnEntry,
                          const VarDecl *, SourceLocation) -> void {
    // To start, find every variable `x` that is live. All regions in the type
    // of `x` must include `point`.
    std::set<RegionName> liveRegionsOnEntry = liveness.LiveRegions(liveOnEntry);
    for (RegionName Name : liveRegionsOnEntry) {
      RegionVariable RV = getRegionVariable(Name);
      infer.AddLivePoint(RV, point);
    }

    // Note: Since the last statement of the CFG basic block may be a
    // borrow/reborrow statement, the set of successor points needs to be
    // calculated here.
    llvm::SmallVector<Point> SuccPoints = env.SuccessorPoints(point);

    // Next, walk the actions and establish any additional constraints that may
    // arise from subtyping.
    const auto &actions = actionMap.at(point);
    for (const Action *action : actions) {
#if DEBUG_PRINT
      llvm::outs() << point << ": " << action << '\n';
#endif
      switch (action->getKind()) {
      case Action::Call: {
        const ActionCall *AC = llvm::cast<ActionCall>(action);
        SourceLocation DiagLoc = S->getBeginLoc();
        for (Point SuccPoint : SuccPoints) {
          for (const ActionCall::Arg &Arg : AC->Args) {
            if (Arg.ParamTy && Arg.Value) {
              SubTypes(Arg.Value->getType(), Arg.ParamTy, SuccPoint,
                       DiagLoc);
            }
          }
          if (AC->Result) {
            SubTypes(AC->ReturnTy, AC->Result->getType(), SuccPoint,
                     DiagLoc);
          }
        }
        break;
      }
      case Action::Return: {
        const ActionReturn *AR = llvm::cast<ActionReturn>(action);
        SourceLocation DiagLoc = S->getBeginLoc();
        if (AR->Value) {
          for (Point SuccPoint : SuccPoints) {
            SubTypes(AR->Value->getType(), AR->ReturnTy, SuccPoint, DiagLoc);
          }
        }
        break;
      }
      case Action::Borrow: {
        const ActionBorrow *AB = llvm::cast<ActionBorrow>(action);
        SourceLocation DiagLoc = S->getBeginLoc();
        for (Point SuccPoint : SuccPoints) {
          SubTypes(AB->BorrowTy, AB->Dest->getType(), SuccPoint, DiagLoc);
          AddReborrowConstraints(AB->BorrowTy->getRegion(), AB->Source,
                                 SuccPoint, DiagLoc);
        }
        break;
      }
      case Action::Assign: {
        const ActionAssign *AA = llvm::cast<ActionAssign>(action);
        SourceLocation DiagLoc = S->getBeginLoc();
        for (Point SuccPoint : SuccPoints) {
          SubTypes(AA->Source->getType(), AA->Dest->getType(), SuccPoint,
                   DiagLoc);
        }
        break;
      }
      case Action::Aggregate: {
        const ActionAggregate *AA = llvm::cast<ActionAggregate>(action);
        SourceLocation DiagLoc = S->getBeginLoc();
        for (Point SuccPoint : SuccPoints) {
          switch (AA->AK) {
          case ActionAggregate::AggregateKind::Copy:
            SubTypes(AA->CopySource->getType(), AA->Dest->getType(),
                     SuccPoint, DiagLoc);
            for (const ActionAggregate::ImplicitReborrow &Reborrow :
                 AA->ImplicitReborrows) {
              AddReborrowConstraints(Reborrow.DestTy->getRegion(),
                                     Reborrow.BorrowedPlace, SuccPoint,
                                     DiagLoc);
            }
            break;
          case ActionAggregate::AggregateKind::Init:
            for (const ActionAggregate::Initializer &Initializer :
                 AA->Initializers) {
              SubTypes(Initializer.Value->getType(), Initializer.DestTy,
                       SuccPoint, DiagLoc);
            }
            break;
          }
        }
        break;
      }
      case Action::Init:
      case Action::Use:
      case Action::StorageDead:
      case Action::Noop:
        break;
      }
    }
  });
}

void RegionCheck::Check() {
  // Compute liveness.
  Liveness liveness(env);
#if DEBUG_PRINT
  liveness.dump();
#endif
  // Add inference constraints.
  PopulateInference(liveness);

  // Solve inference constraints, reporting any errors.
  for (const BorrowDiagInfo &Diag : infer.Solve(env)) {
    assert(Diag.Kind == BorrowDiagKind::LifetimeNotLong &&
           "unexpected inference diagnostic");
    reporter.LifetimeNotLong(Diag.Location);
  }

  // Compute loans in scope at each point.
  LoansInScope LIS(env, *this);

#if DEBUG_PRINT
  llvm::outs() << "========== BorrowCk ==========\n";
#endif
  // Run the borrow check, reporting any errors.
  BorrowCk(*this, LIS);
}

void RegionCheck::createRegionVariable(RegionName RN) {
  if (regionMap.find(RN) != regionMap.end())
    return;
  RegionVariable RV = infer.AddVar(RN);
  regionMap[RN] = RV;
}

void RegionCheck::InitFreeRegions() {
  for (unsigned I = 0; I < env.freeRegions.size(); ++I) {
    RegionName RN = env.freeRegions[I];
    auto It = regionMap.find(RN);
    assert(It != regionMap.end() &&
           "free region should already have a region variable");
    RegionVariable RV = It->second;
    for (const CFGBlock *block : env.cfg.const_nodes()) {
      for (CFGBlock::const_iterator it = block->begin(), ei = block->end();
           it != ei; ++it) {
        Point point = Point::Create(block, it);
        infer.AddLivePoint(RV, point);
      }
    }
    infer.AddEndRegion(RV, RN);
    // Every inner parameter region outlives the shared signature region.
    if (I != 0)
      infer.AddEndRegion(RV, env.freeRegions.front());
    infer.CapVar(RV);
  }
}

void RegionCheck::GenerateActions() {
  for (const CFGBlock *Block : env.cfg.const_nodes()) {
    for (CFGBlock::const_iterator it = Block->begin(), ei = Block->end();
         it != ei; ++it) {
      const CFGElement &Elem = *it;
      const Stmt *S = nullptr;
      const VarDecl *LifetimeEndsVD = nullptr;
      SourceLocation LifetimeEndsLoc;

      if (llvm::Optional<CFGStmt> StmtElem = Elem.getAs<CFGStmt>()) {
        S = StmtElem->getStmt();
      } else if (llvm::Optional<CFGLifetimeEnds> LifetimeEnds =
                     Elem.getAs<CFGLifetimeEnds>()) {
        LifetimeEndsVD = LifetimeEnds->getVarDecl();
        LifetimeEndsLoc = LifetimeEnds->getTriggerStmt()->getEndLoc();
      }

      Point P = Point::Create(Block, it);
      ActionGenerator AG(env.Ctx, env, S, LifetimeEndsVD, LifetimeEndsLoc);
      bool Inserted = actionMap.emplace(P, AG.takeActions()).second;
      assert(Inserted && "new action should be generated only once");
    }
  }
}

/// Given a RegionName, returns the corresponding RegionVariable from regionMap.
/// If not existing, insert it into regionMap and return it.
RegionVariable RegionCheck::getRegionVariable(RegionName Name) {
  if (regionMap.find(Name) != regionMap.end())
    return regionMap[Name];
  RegionVariable RV = infer.AddVar(Name);
  regionMap[Name] = RV;
  return RV;
}

/// Given a RegionName, returns the corresponding Region from InferenceContext.
const Region &RegionCheck::getRegion(RegionName RN) const {
  assert(regionMap.find(RN) != regionMap.end() &&
         "no region variable ever created with this name");
  RegionVariable RV = regionMap.at(RN);
  return infer.getRegion(RV);
}

void RegionCheck::SubTypes(const Ty *Sub, const Ty *Sup, Point P,
                           SourceLocation DiagLoc) {
#if DEBUG_PRINT
  llvm::outs() << Sub << " <: " << Sup << " @ " << P << "\n";
#endif
  RelateTypes(Sub, Variance::Co, Sup, P, DiagLoc);
}

void RegionCheck::RelateTypes(const Ty *A, Variance V, const Ty *B, Point P,
                              SourceLocation DiagLoc) {
#if DEBUG_PRINT
  llvm::outs() << "RelateTypes(" << A << ", " << V << ", " << B
               << ") @ " << P << '\n';
#endif
  // `void` erases the pointee structure after outer pointer regions are
  // related.
  if ((A->getKind() == Ty::TyKind::Base &&
       A->getQualType()->isVoidType()) ||
      (B->getKind() == Ty::TyKind::Base &&
       B->getQualType()->isVoidType()))
    return;

  switch(A->getKind()) {
  case Ty::TyKind::Pointer: {
    // Pointer-to-scalar (e.g. assigning a pointer to int/_Bool) erases the
    // region structure; no region constraint applies.
    if (B->getKind() != Ty::TyKind::Pointer)
      return;
    if (A->isBorrowPointer() && B->isBorrowPointer()) {
      RegionName RegionA = A->getRegion();
      RegionName RegionB = B->getRegion();
      RelateRegions(RegionA, V, RegionB, P, DiagLoc);
      const Ty *PointeeA = A->getPointee();
      const Ty *PointeeB = B->getPointee();
      Variance ReferentV = xform(V, variance(A->getBorrowKind()));
      RelateTypes(PointeeA, ReferentV, PointeeB, P, DiagLoc);
    }
    if (A->isOwnedPointer() && B->isOwnedPointer()) {
      RelateTypes(A->getPointee(), V, B->getPointee(), P, DiagLoc);
    }
    // Stop at raw-pointer boundaries or mismatched pointer.
    return;
  }
  case Ty::TyKind::Struct: {
    assert(B->getKind() == Ty::TyKind::Struct &&
           "struct types should only relate to struct types");
    llvm::ArrayRef<const RegionName *> RegionParamsA = A->getRegionParams();
    llvm::ArrayRef<const RegionName *> RegionParamsB = B->getRegionParams();
    assert(RegionParamsA.size() == RegionParamsB.size() &&
           "struct region parameter counts should match");
    const RecordType *RecordA = A->getQualType()->castAs<RecordType>();
    llvm::ArrayRef<Variance> DefVariances =
        env.getDefVariances(RecordA->getDecl());
    for (auto Params :
         llvm::zip(RegionParamsA, RegionParamsB, DefVariances)) {
      Variance ParamV = xform(V, std::get<2>(Params));
      RelateRegions(*std::get<0>(Params), ParamV, *std::get<1>(Params), P,
                    DiagLoc);
    }
    return;
  }
  case Ty::TyKind::Array: {
    if (B->isRawPointer()) {
      return;
    }
    RelateTypes(A->getElement(), V, B->getElement(), P, DiagLoc);
    return;
  }
  case Ty::TyKind::Base:
    // do nothing
    return;
  }
  llvm_unreachable("unknown type kind");
}

void RegionCheck::RelateRegions(RegionName RegionA, Variance V,
                                RegionName RegionB, Point P,
                                SourceLocation DiagLoc) {
#if DEBUG_PRINT
  llvm::outs() << "RelateRegions: " << RegionA << V << RegionB
               << " @ " << P << '\n';
#endif
  RegionVariable RegionVarA = getRegionVariable(RegionA);
  RegionVariable RegionVarB = getRegionVariable(RegionB);
  switch (V) {
  case Variance::Co:
    infer.AddOutLives(RegionVarB, RegionVarA, P, DiagLoc);
    return;
  case Variance::Contra:
    infer.AddOutLives(RegionVarA, RegionVarB, P, DiagLoc);
    return;
  case Variance::In:
    infer.AddOutLives(RegionVarB, RegionVarA, P, DiagLoc);
    infer.AddOutLives(RegionVarA, RegionVarB, P, DiagLoc);
    return;
  case Variance::Bi:
    return;
  }
  llvm_unreachable("unknown variance");
}

/// AddReborrowConstraints - Add relations between regions that are needed to
/// ensure that reborrows live long enough.
/// Specifically, if we borrow something like `*r` for `'a`, where
/// `r: &'b i32`, then `'b: 'a` is required.
void RegionCheck::AddReborrowConstraints(RegionName RN, const Place *Source,
                                         Point P, SourceLocation DiagLoc) {
#if DEBUG_PRINT
  llvm::outs() << "AddReborrowConstraints(" << RN.Name << ", " << Source
               << ") @ " << P << '\n';
#endif
  for (const Place *Prefix : Source->supportingPrefixes()) {
    switch (Prefix->getKind()) {
    case Place::Kind::Var:
    case Place::Kind::Field:
      break;
    case Place::Kind::Deref:
    case Place::Kind::Index: {
      const Ty *BaseTy = Prefix->getBase()->getType();
      if (BaseTy->isBorrowPointer()) {
        infer.AddOutLives(getRegionVariable(RN),
                          getRegionVariable(BaseTy->getRegion()), P, DiagLoc);
      }
      break;
    }
    }
  }
}

/// Traverse each statement in the CFG, check the corresponding actions and
/// report erros according to the loans in scope information.
void clang::borrow::BorrowCk(RegionCheck &rc, LoansInScope &LIS) {
  LIS.Walk([&](Point point, const llvm::SmallVector<Loan> &loans) -> void {
    BorrowCheck borrowck(rc.getReporter(), point, loans);
#if DEBUG_PRINT
    llvm::outs() << "Point: " << point << '\n';
    llvm::outs() << "Loans: [\n";
    for (const Loan &loan : loans) {
      llvm::outs() << loan << ",\n";
    }
    llvm::outs() << "]\n";
#endif
    for (const Action *action : rc.getActionMap().at(point))
      borrowck.CheckAction(action);
  });
}

// Entry point of borrow checker.
void clang::borrow::runBorrowChecker(const FunctionDecl &fd, const CFG &cfg,
                                     ASTContext &Ctx,
                                     BorrowDiagReporter &Reporter) {
  RegionGenerator RG(Ctx, fd);
  DefVarianceAnalysis DVA(Ctx, fd, RG.getRecordRegionLayouts());
  RegionCheck RC(fd, cfg, Ctx, Reporter, RG, DVA);
  RC.Check();
}

#endif
