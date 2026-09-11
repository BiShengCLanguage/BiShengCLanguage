// RUN: not clang-tblgen -gen-bsc-diag-docs -I%S -I%S/../../../../TableGen %s -o /dev/null 2>&1 | FileCheck %s
include "DiagnosticBase.inc"
include "BSCDiagDocsBase.inc"

def BSCFeatureAAA : BSCDiagFeature<"AAA", "aaa", 0>;
// Despite the name, this is a warning, not a Note.  It carries BSCOutOfScope so
// that the completeness gate (this file is under Basic/BSC) does not fire on it
// before the note-reference check below.
def note_not_a_note : Warning<"still not a note">, BSCOutOfScope;
def err_bsc_test : Error<"x">, BSCDiag<"AAA", 1, "see `note_not_a_note`">;
// CHECK: bsc-diag-docs-note-ref-not-note.td:[[@LINE-1]]:1: error: references 'note_not_a_note', which is not a Note definition
