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
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallSet.h"
#include <string>

namespace clang {

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
  
    OwnershipDiagInfo(SourceLocation Loc, OwnershipDiagKind Kind,
                      std::string Name)
        : Loc(Loc), Kind(Kind), Name(Name), Fields(""),
          Location(SourceLocation()) {}

    OwnershipDiagInfo(SourceLocation Loc, OwnershipDiagKind Kind, QualType QT)
        : Loc(Loc), Kind(Kind), Name(""), Fields(""),
          Location(SourceLocation()), Type(QT) {}

    OwnershipDiagInfo(SourceLocation Loc, OwnershipDiagKind Kind,
                      std::string Name, std::string fields)
        : Loc(Loc), Kind(Kind), Name(Name), Fields(fields),
          Location(SourceLocation()) {}
  
    OwnershipDiagInfo(SourceLocation Loc, OwnershipDiagKind Kind,
                      std::string Name, std::string fields,
                      SourceLocation location)
        : Loc(Loc), Kind(Kind), Name(Name), Fields(fields), Location(location) {}
  
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

    // struct status, e.g. struct S s
    using SOwnedField = llvm::SmallSet<std::string, 10>;
    llvm::DenseMap<const VarDecl *, OwnershipSet> SStatus;
    llvm::DenseMap<const VarDecl *, SOwnedField> SAllOwnedFields;
    llvm::DenseMap<const VarDecl *, SOwnedField> SOwnedOwnedFields;
    llvm::DenseMap<const VarDecl *, SOwnedField> SNullOwnedFields;

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
          SStatus(0), SAllOwnedFields(0), SOwnedOwnedFields(0), BOPStatus(0),
          BOPAllOwnedFields(0), BOPOwnedOwnedFields(0) {}

    OwnershipStatus(llvm::DenseMap<const VarDecl *, OwnershipSet> opss,
                    llvm::DenseMap<const VarDecl *, OPSOwnedField> opsaof,
                    llvm::DenseMap<const VarDecl *, OPSOwnedField> opsoof,
                    llvm::DenseMap<const VarDecl *, OwnershipSet> ss,
                    llvm::DenseMap<const VarDecl *, OPSOwnedField> saof,
                    llvm::DenseMap<const VarDecl *, SOwnedField> soof,
                    llvm::DenseMap<const VarDecl *, OPSOwnedField> snof,
                    llvm::DenseMap<const VarDecl *, OwnershipSet> bops,
                    llvm::DenseMap<const VarDecl *, BOPOwnedField> bopaof,
                    llvm::DenseMap<const VarDecl *, BOPOwnedField> bopoof)
        : OPSStatus(opss), OPSAllOwnedFields(opsaof),
          OPSOwnedOwnedFields(opsoof), SStatus(ss), SAllOwnedFields(saof),
          SOwnedOwnedFields(soof), SNullOwnedFields(snof), BOPStatus(bops),
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
        S.Diag(DI.Loc, getOwnershipDiagID(DI.Kind)) << DI.Name;
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

void runOwnershipAnalysis(const FunctionDecl &fd, const CFG &cfg,
                          AnalysisDeclContext &ac,
                          OwnershipDiagReporter &reporter,
                          ASTContext &ctx);

} // end namespace clang

#endif // ENABLE_BSC

#endif // LLVM_CLANG_ANALYSIS_ANALYSES_BSCOWNERSHIP_H