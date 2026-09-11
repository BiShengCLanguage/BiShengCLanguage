// RUN: not clang-tblgen -gen-bsc-diag-docs -I%S -I%S/../../../../TableGen %s -o /dev/null 2>&1 | FileCheck %s
include "DiagnosticBase.inc"
include "BSCDiagDocsBase.inc"

// Section footnotes are checked for dangling note references too, not just the
// Notes column.
def BSCFeatureAAA : BSCDiagFeature<"AAA", "aaa", 0>;
def err_bsc_test : Error<"x">,
    BSCDiag<"AAA", 1, "", "see `note_missing` for details">;
// CHECK: bsc-diag-docs-footer-note-ref.td:[[@LINE-2]]:1: error: references 'note_missing', which is not a Note definition
