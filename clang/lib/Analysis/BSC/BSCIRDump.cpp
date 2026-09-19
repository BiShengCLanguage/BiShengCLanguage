//===- BSCIRDump.cpp - Pretty-printing for BSCIR -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Pretty-printing for BSCIR Body, Blocks, Statements, and Terminators.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/Analysis/Analyses/BSC/BSCIRDump.h"
#include "clang/AST/Attr.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/Type.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::bscir;

//===----------------------------------------------------------------------===//
// Helper: Safe zone to string
//===----------------------------------------------------------------------===//

static const char *safeZoneStr(bool IsSafe) {
  return IsSafe ? " [safe]" : " [unsafe]";
}

//===----------------------------------------------------------------------===//
// Helper: Type to string
//===----------------------------------------------------------------------===//

static std::string typeStr(QualType Ty) {
  if (Ty.isNull())
    return "void";
  return Ty.getAsString();
}

//===----------------------------------------------------------------------===//
// Place
//===----------------------------------------------------------------------===//

void clang::bscir::dumpPlace(const Place &P, llvm::raw_ostream &OS) {
  OS << P.toString();
}

//===----------------------------------------------------------------------===//
// Operand
//===----------------------------------------------------------------------===//

void clang::bscir::dumpOperand(const Operand &O, llvm::raw_ostream &OS) {
  switch (O.K) {
  case Operand::Copy:
    OS << "copy(";
    dumpPlace(O.getPlace(), OS);
    OS << ")";
    break;
  case Operand::Move:
    OS << "move(";
    dumpPlace(O.getPlace(), OS);
    OS << ")";
    break;
  case Operand::Constant:
    if (O.getConstVal().isInt()) {
      OS << "const ";
      llvm::SmallString<16> Str;
      O.getConstVal().getInt().toString(
          Str, 10, O.getConstVal().getInt().isSigned());
      OS << Str;
    } else if (O.getConstVal().isFloat()) {
      OS << "const ";
      llvm::SmallString<16> Str;
      O.getConstVal().getFloat().toString(Str);
      OS << Str << "f";
    } else if (O.hasString()) {
      OS << "const \"";
      for (unsigned char C : O.getStringVal()) {
        switch (C) {
        case '\\': OS << "\\\\"; break;
        case '"': OS << "\\\""; break;
        case '\n': OS << "\\n"; break;
        case '\t': OS << "\\t"; break;
        case '\r': OS << "\\r"; break;
        case '\0': OS << "\\0"; break;
        default:
          if (C >= 32 && C < 127)
            OS << C;
          else {
            static const char Hex[] = "0123456789ABCDEF";
            OS << "\\x" << Hex[(C >> 4) & 0xF] << Hex[C & 0xF];
          }
          break;
        }
      }
      OS << "\"";
    } else {
      OS << "const(";
      if (!O.getConstTy().isNull())
        OS << typeStr(O.getConstTy());
      else
        OS << "?";
      OS << ")";
    }
    break;
  }
}

//===----------------------------------------------------------------------===//
// Rvalue
//===----------------------------------------------------------------------===//

void clang::bscir::dumpRvalue(const Rvalue &R, const Body &B,
                              llvm::raw_ostream &OS) {
  switch (R.K) {
  case Rvalue::Use:
    dumpOperand(R.getUse().Op, OS);
    break;
  case Rvalue::Ref: {
    const auto &Ref = R.getRef();
    if (Ref.BK == BorrowKind::Mut)
      OS << "&_Mut ";
    else
      OS << "&_Const ";
    dumpPlace(Ref.P, OS);
    if (Ref.IsReborrow)
      OS << " (reborrow)";
    if (Ref.RegionId != 0)
      OS << " <'r" << Ref.RegionId << ">";
    break;
  }
  case Rvalue::AddressOf:
    OS << "&";
    dumpPlace(R.getAddrOf().P, OS);
    break;
  case Rvalue::BinaryOp: {
    const auto &Bin = R.getBinOp();
    OS << "BinOp(";
    OS << BinaryOperator::getOpcodeStr(Bin.Op).str();
    OS << ", ";
    dumpOperand(Bin.LHS, OS);
    OS << ", ";
    dumpOperand(Bin.RHS, OS);
    OS << ")";
    break;
  }
  case Rvalue::UnaryOp: {
    const auto &Un = R.getUnOp();
    OS << "UnaryOp(";
    OS << UnaryOperator::getOpcodeStr(Un.Op).str();
    OS << ", ";
    dumpOperand(Un.Sub, OS);
    OS << ")";
    break;
  }
  case Rvalue::Aggregate: {
    const auto &Agg = R.getAgg();
    OS << "Aggregate(";
    if (Agg.Decl)
      OS << Agg.Decl->getNameAsString();
    else
      OS << "?";
    OS << ", {";
    for (unsigned I = 0; I < Agg.Fields.size(); ++I) {
      if (I > 0)
        OS << ", ";
      dumpOperand(Agg.Fields[I], OS);
    }
    OS << "})";
    break;
  }
  case Rvalue::Array: {
    const auto &Arr = R.getArray();
    OS << "Array(" << typeStr(Arr.ElementTy) << ", {";
    for (unsigned I = 0; I < Arr.Elements.size(); ++I) {
      if (I > 0)
        OS << ", ";
      dumpOperand(Arr.Elements[I], OS);
    }
    OS << "})";
    break;
  }
  case Rvalue::Cast: {
    const auto &C = R.getCast();
    OS << "Cast(";
    dumpOperand(C.Op, OS);
    OS << " as " << typeStr(C.Ty) << ")";
    break;
  }
  case Rvalue::NullPtr:
    OS << "null(" << typeStr(R.getTypeData().Ty) << ")";
    break;
  case Rvalue::SizeOf:
    OS << "sizeof(" << typeStr(R.getTypeData().Ty) << ")";
    break;
  }
}

//===----------------------------------------------------------------------===//
// Statement
//===----------------------------------------------------------------------===//

void clang::bscir::dumpStatement(const Statement &S, const Body &B,
                                 llvm::raw_ostream &OS) {
  OS << "    ";
  switch (S.K) {
  case Statement::Assign: {
    const auto &A = S.getAssign();
    dumpPlace(A.Dest, OS);
    OS << " = ";
    dumpRvalue(A.Src, B, OS);
    break;
  }
  case Statement::StorageLive:
    OS << "StorageLive(_" << S.getStorageLocal().Index << ")";
    break;
  case Statement::StorageDead:
    OS << "StorageDead(_" << S.getStorageLocal().Index << ")";
    break;
  case Statement::Nop:
    OS << "nop";
    break;
  }
  OS << safeZoneStr(S.IsSafe);
  OS << "\n";
}

//===----------------------------------------------------------------------===//
// Terminator
//===----------------------------------------------------------------------===//

void clang::bscir::dumpTerminator(const Terminator &T, const Body &B,
                                  llvm::raw_ostream &OS) {
  OS << "    ";
  switch (T.K) {
  case Terminator::Goto:
    OS << "goto -> bb" << T.getGoto().Target.Index;
    break;
  case Terminator::SwitchInt: {
    const auto &SW = T.getSwitchInt();
    OS << "switchInt(";
    dumpOperand(SW.Discriminant, OS);
    OS << ") -> [";
    for (unsigned I = 0; I < SW.Targets.size(); ++I) {
      if (I > 0)
        OS << ", ";
      llvm::SmallString<16> Str;
      SW.Targets[I].first.toString(Str, 10, false);
      OS << Str << ": bb" << SW.Targets[I].second.Index;
    }
    OS << ", otherwise: bb" << SW.Otherwise.Index << "]";
    break;
  }
  case Terminator::Call: {
    const auto &C = T.getCall();
    dumpPlace(C.Dest, OS);
    OS << " = call ";
    if (C.Decl)
      OS << C.Decl->getNameAsString();
    else if (!C.CalleeName.empty())
      OS << C.CalleeName;
    else
      dumpOperand(C.Callee, OS);
    OS << "(";
    for (unsigned I = 0; I < C.Args.size(); ++I) {
      if (I > 0)
        OS << ", ";
      dumpOperand(C.Args[I], OS);
      // Check if this argument has ensure_init.
      bool IsEnsureInit = false;
      if (C.Decl && I < C.Decl->getNumParams() &&
          C.Decl->getParamDecl(I)->hasAttr<EnsureInitAttr>())
        IsEnsureInit = true;
      if (!IsEnsureInit && C.CalleeProtoType &&
          I < C.CalleeProtoType->getNumParams() &&
          C.CalleeProtoType->getExtParameterInfo(I).isEnsureInit())
        IsEnsureInit = true;
      if (IsEnsureInit)
        OS << " [ensure_init]";
    }
    OS << ")";
    if (C.Diverges)
      OS << " -> diverge";
    else
      OS << " -> bb" << C.Successor.Index;
    break;
  }
  case Terminator::Drop: {
    const auto &D = T.getDrop();
    OS << "drop(";
    dumpPlace(D.Dropped, OS);
    OS << ") -> bb" << D.Successor.Index;
    break;
  }
  case Terminator::Return:
    OS << "return";
    break;
  case Terminator::Unreachable:
    OS << "unreachable";
    break;
  }
  OS << safeZoneStr(T.IsSafe);
  OS << "\n";
}

//===----------------------------------------------------------------------===//
// BasicBlock
//===----------------------------------------------------------------------===//

void clang::bscir::dumpBlock(const BasicBlock &BB, const Body &B,
                             llvm::raw_ostream &OS) {
  OS << "  bb" << BB.Id.Index << ": {\n";
  for (const Statement &S : BB.Statements)
    dumpStatement(S, B, OS);
  dumpTerminator(BB.Term, B, OS);
  OS << "  }\n";
}

//===----------------------------------------------------------------------===//
// Body
//===----------------------------------------------------------------------===//

void clang::bscir::dumpBody(const Body &B, llvm::raw_ostream &OS) {
  OS << "fn ";
  if (B.SourceFD)
    OS << B.SourceFD->getNameAsString();
  else
    OS << "<unknown>";
  OS << "(";

  // Print parameters
  bool First = true;
  for (unsigned I = 1; I <= B.NumParams; ++I) {
    if (!First)
      OS << ", ";
    First = false;
    const LocalDecl &L = B.Locals[I];
    OS << "_" << L.Id.Index << ": " << typeStr(L.Ty);
    if (!L.Name.empty())
      OS << " /* " << L.Name << " */";
    // Check if this param has ensure_init.
    if (B.SourceFD && (I - 1) < B.SourceFD->getNumParams() &&
        B.SourceFD->getParamDecl(I - 1)->hasAttr<EnsureInitAttr>())
      OS << " [ensure_init]";
  }
  OS << ") -> " << typeStr(B.Locals[0].Ty);

  OS << safeZoneStr(B.IsSafe) << " {\n";

  // Print locals
  OS << "  // Locals:\n";
  for (const LocalDecl &L : B.Locals) {
    OS << "  //   _" << L.Id.Index << ": " << typeStr(L.Ty);
    if (!L.Name.empty())
      OS << " (" << L.Name << ")";
    if (L.IsTemp)
      OS << " [temp]";
    // Check if this local is a param with ensure_init.
    if (B.SourceFD && L.Id.Index >= 1 && L.Id.Index <= B.NumParams &&
        (L.Id.Index - 1) < B.SourceFD->getNumParams() &&
        B.SourceFD->getParamDecl(L.Id.Index - 1)->hasAttr<EnsureInitAttr>())
      OS << " [ensure_init]";
    OS << "\n";
  }
  OS << "\n";

  // Print blocks
  for (const BasicBlock &BB : B.Blocks)
    dumpBlock(BB, B, OS);

  OS << "}\n";
}

#endif // ENABLE_BSC
