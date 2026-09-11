// RUN: clang-tblgen -gen-bsc-diag-docs -I%S/../../../../TableGen -I%src_include_dir %s -o - 2>&1 | \
// RUN:    FileCheck %s
//
// Checks that -gen-bsc-diag-docs renders BSCDiag<> metadata into the
// bsc-errors.md reference: feature sections, stable FEATURE-NNN / FEATURE-WNNN
// codes, the raw diagnostic text (with table pipes escaped), per-diagnostic
// section footnotes (before the feature-level footer), and the summary.

include "DiagnosticBase.inc"
include "clang/Basic/BSC/BSCDiagDocs.td"

def err_bsc_test_use : Error<"use of %select{moved|dropped}0 value: '%0'">,
    BSCDiag<"OWN", 1, "An extra `note` explaining the diagnostic">;
def warn_bsc_test_leak : Warning<"value %0 may leak">, BSCDiag<"OWN", 1>;

// A diagnostic-level footnote is rendered at the end of its feature section,
// before the feature-level footer.
def BSCFeatureTEST : BSCDiagFeature<"TEST", "test feature", 99, "",
    "Feature-level footer.">;
def err_bsc_test_foot : Error<"has a footnote">,
    BSCDiag<"TEST", 1, "", "Diagnostic-level footnote.">;

// Diagnostics that opt out are not rendered.
def err_bsc_test_ignored : Error<"ignored">, BSCOutOfScope;

// CHECK:      ## OWN — owned (1 error, 1 warning)
// CHECK:      | OWN-001 | err_bsc_test_use | use of %select{moved\|dropped}0 value: '%0' | An extra `note` explaining the diagnostic |
// CHECK:      | OWN-W001 | warn_bsc_test_leak | value %0 may leak | — |
// CHECK:      ## TEST — test feature (1 error)
// CHECK:      | TEST-001 | err_bsc_test_foot | has a footnote | — |
// CHECK:      Diagnostic-level footnote.
// CHECK:      Feature-level footer.
// CHECK:      | OWN- | owned | 1 | 1 |
// CHECK:      | TEST- | test feature | 1 | 0 |
// CHECK:      | **Total** | | **2** | **1** |
// CHECK-NOT:  err_bsc_test_ignored
