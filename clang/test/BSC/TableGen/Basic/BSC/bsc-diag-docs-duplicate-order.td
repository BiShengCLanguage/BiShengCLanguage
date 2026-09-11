// RUN: not clang-tblgen -gen-bsc-diag-docs -I%S -I%S/../../../../TableGen %s -o /dev/null 2>&1 | FileCheck %s
include "DiagnosticBase.inc"
include "BSCDiagDocsBase.inc"

// Two features claiming the same Order make the section order non-deterministic.
def BSCFeatureAAA : BSCDiagFeature<"AAA", "aaa", 0>;
def BSCFeatureBBB : BSCDiagFeature<"BBB", "bbb", 0>;
// CHECK: bsc-diag-docs-duplicate-order.td:[[@LINE-1]]:1: error: duplicate BSC diagnostic feature order 0 (feature '
