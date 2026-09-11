// RUN: not clang-tblgen -gen-bsc-diag-docs -I%S -I%S/../../../../TableGen %s -o /dev/null 2>&1 | FileCheck %s
include "DiagnosticBase.inc"
include "BSCDiagDocsBase.inc"

def BSCFeatureAAA : BSCDiagFeature<"AAA", "aaa", 0>;
// Only errors and warnings are documented; metadata on a note is a mistake.
def note_bsc_test : Note<"x">, BSCDiag<"AAA", 1>;
// CHECK: bsc-diag-docs-metadata-on-note.td:[[@LINE-1]]:1: error: 'note_bsc_test' carries BSCDiag<>/BSCOutOfScope metadata but is not an error or warning
