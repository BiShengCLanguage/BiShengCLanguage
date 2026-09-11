// RUN: not clang-tblgen -gen-bsc-diag-docs -I%S -I%S/../../../../TableGen %s -o /dev/null 2>&1 | FileCheck %s
include "DiagnosticBase.inc"
include "BSCDiagDocsBase.inc"

// Two features must not share a prefix: it would make their codes ambiguous.
def BSCFeatureFirst : BSCDiagFeature<"AAA", "first", 0>;
def BSCFeatureSecond : BSCDiagFeature<"AAA", "second", 1>;
// CHECK: bsc-diag-docs-duplicate-name.td:[[@LINE-1]]:1: error: duplicate BSC diagnostic feature name 'AAA'
