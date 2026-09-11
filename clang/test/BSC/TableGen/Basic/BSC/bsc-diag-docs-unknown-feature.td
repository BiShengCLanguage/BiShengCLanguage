// RUN: not clang-tblgen -gen-bsc-diag-docs -I%S -I%S/../../../../TableGen %s -o /dev/null 2>&1 | FileCheck %s
include "DiagnosticBase.inc"
include "BSCDiagDocsBase.inc"

def BSCFeatureAAA : BSCDiagFeature<"AAA", "aaa", 0>;
// A BSCDiag<> feature argument must name a BSCDiagFeature.
def err_bsc_test : Error<"x">, BSCDiag<"ZZZ", 1>;
// CHECK: bsc-diag-docs-unknown-feature.td:[[@LINE-1]]:1: error: unknown BSC diagnostic feature 'ZZZ' (add a BSCDiagFeature for it)
