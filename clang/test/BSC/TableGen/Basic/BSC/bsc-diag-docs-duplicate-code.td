// RUN: not clang-tblgen -gen-bsc-diag-docs -I%S -I%S/../../../../TableGen %s -o /dev/null 2>&1 | FileCheck %s
include "DiagnosticBase.inc"
include "BSCDiagDocsBase.inc"

def BSCFeatureAAA : BSCDiagFeature<"AAA", "aaa", 0>;
// Codes are the stable handles of the reference, so two diagnostics must not
// share one.  The record order is unspecified, so the reported location is not
// checked.
def err_bsc_dup_a : Error<"first">, BSCDiag<"AAA", 1>;
def err_bsc_dup_b : Error<"second">, BSCDiag<"AAA", 1>;
// CHECK: error: duplicate BSC diagnostic code 'AAA-001'
