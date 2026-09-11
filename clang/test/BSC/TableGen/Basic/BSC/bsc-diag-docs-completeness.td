// RUN: not clang-tblgen -gen-bsc-diag-docs -I%S -I%S/../../../../TableGen %s -o /dev/null 2>&1 | FileCheck %s
//
// The enclosing directory is named `Basic/BSC` so that the emitter's "is this a
// BSC diagnostics file?" check (source path contains "/Basic/BSC/") fires.
// Every BSC error/warning must be classified, so an untagged one is rejected.
include "DiagnosticBase.inc"
include "BSCDiagDocsBase.inc"

def err_bsc_unannotated : Error<"x">;
// CHECK: bsc-diag-docs-completeness.td:[[@LINE-1]]:1: error: BSC diagnostic 'err_bsc_unannotated' has neither BSCDiag<> nor BSCOutOfScope metadata; add one in clang/include/clang/Basic/BSC/
