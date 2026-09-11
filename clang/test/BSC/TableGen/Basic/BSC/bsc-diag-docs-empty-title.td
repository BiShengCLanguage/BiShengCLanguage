// RUN: not clang-tblgen -gen-bsc-diag-docs -I%S -I%S/../../../../TableGen %s -o /dev/null 2>&1 | FileCheck %s
include "DiagnosticBase.inc"
include "BSCDiagDocsBase.inc"

// A feature without a title would produce a malformed section heading.
def BSCFeatureAAA : BSCDiagFeature<"AAA", "", 0>;
// CHECK: bsc-diag-docs-empty-title.td:[[@LINE-1]]:1: error: BSC diagnostic feature 'AAA' must have a non-empty Title
