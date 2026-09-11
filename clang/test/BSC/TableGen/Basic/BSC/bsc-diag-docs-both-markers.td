// RUN: not clang-tblgen -gen-bsc-diag-docs -I%S -I%S/../../../../TableGen %s -o /dev/null 2>&1 | FileCheck %s
include "DiagnosticBase.inc"
include "BSCDiagDocsBase.inc"

def BSCFeatureAAA : BSCDiagFeature<"AAA", "aaa", 0>;
// A diagnostic is either documented or intentionally out of scope, never both.
def err_bsc_test : Error<"x">, BSCDiag<"AAA", 1>, BSCOutOfScope;
// CHECK: bsc-diag-docs-both-markers.td:[[@LINE-1]]:1: error: 'err_bsc_test' has both BSCDiag<> and BSCOutOfScope
