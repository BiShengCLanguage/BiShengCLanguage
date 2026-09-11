// RUN: not clang-tblgen -gen-bsc-diag-docs -I%S -I%S/../../../../TableGen %s -o /dev/null 2>&1 | FileCheck %s
include "DiagnosticBase.inc"
include "BSCDiagDocsBase.inc"

def BSCFeatureAAA : BSCDiagFeature<"AAA", "aaa", 0>;
// Codes are rendered as FEATURE-NNN, so numbers must fit in three digits.
def err_bsc_test : Error<"x">, BSCDiag<"AAA", 1000>;
// CHECK: bsc-diag-docs-number-range.td:[[@LINE-1]]:1: error: BSC diagnostic number 1000 is out of range; expected 1..999
