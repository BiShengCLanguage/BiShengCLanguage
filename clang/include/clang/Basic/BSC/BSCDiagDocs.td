//===- BSCDiagDocs.td - BSC diagnostic documentation metadata ----------*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Metadata used by `clang-tblgen -gen-bsc-diag-docs` to generate the BSC
// diagnostic reference `bsc-errors.md`.
//
// Every BSC error or warning must carry either BSCDiag<> (documented, with a
// stable FEATURE-NNN code) or BSCOutOfScope (intentionally not documented).
// The generator fails otherwise, so a new diagnostic cannot silently go
// undocumented.  That is the whole point of this file: the reference is a build
// artifact rather than a hand-maintained document.
//
//===----------------------------------------------------------------------===//

/// A feature group (section) in the BSC diagnostic reference.  `order`
/// controls the section order in the generated document.  `sectionFooter` is
/// emitted after any per-diagnostic footnotes; it is kept for section-level
/// footnotes that are not tied to a single diagnostic (currently unused).
class BSCDiagFeature<string name, string title, int order,
                     string sectionNote = "", string sectionFooter = ""> {
  string Name = name;
  string Title = title;
  int Order = order;
  string SectionNote = sectionNote;
  string SectionFooter = sectionFooter;
}

/// Documents a BSC diagnostic in bsc-errors.md.
/// - feature must name a BSCDiagFeature.
/// - number is stable and unique per (feature, severity): errors render as
///   FEATURE-NNN, warnings as FEATURE-WNNN.  Numbers are never reused.
/// - doc is optional markdown for the "Notes" column.
/// - sectionFooter is optional markdown emitted as a footnote at the end of
///   the diagnostic's feature section, after the table and before the
///   feature's own SectionFooter.
class BSCDiag<string feature, int number, string doc = "",
              string sectionFooter = ""> {
  string BSCFeature = feature;
  int BSCNumber = number;
  string BSCDoc = doc;
  string SectionFooter = sectionFooter;
}

/// Marks a BSC diagnostic that is intentionally not listed in bsc-errors.md.
class BSCOutOfScope {}

def BSCFeatureOWN : BSCDiagFeature<"OWN", "owned", 0>;

def BSCFeatureBOR : BSCDiagFeature<"BOR", "borrow", 1,
  "Borrow-check diagnostics and their associated notes are listed below. "
  "`err_lifetime_may_not_live_long` is emitted by constraint solving without an "
  "attached note.">;

def BSCFeatureINIT : BSCDiagFeature<"INIT", "initialization", 2>;

def BSCFeatureMISC : BSCDiagFeature<"MISC", "declaration / dispatch consistency", 3>;

def BSCFeatureSZONE : BSCDiagFeature<"SZONE", "safe zone", 4>;

def BSCFeatureNONNULL : BSCDiagFeature<"NONNULL", "nonnull pointer", 5>;

def BSCFeatureNULLABLE : BSCDiagFeature<"NULLABLE", "nullable pointer", 6>;

def BSCDiagDocumentation {
  string Intro =
    "# BSC-Specific Diagnostics by Feature\n"
    "\n"
    "This reference lists the diagnostics that BiSheng C adds on top of standard "
    "Clang, grouped by language feature.  It is generated from the BSC diagnostic "
    "definitions under `clang/include/clang/Basic/BSC/DiagnosticBSC*.td` by "
    "`clang-tblgen -gen-bsc-diag-docs`; do not edit it by hand.  Rebuild it with "
    "the `bsc-errors-md` target.\n"
    "\n"
    "Codes follow a `FEATURE-NNN` scheme: each feature numbers its errors "
    "independently starting from `001`, and warnings use a `FEATURE-WNNN` series. "
    "Codes are stable and are never reused or renumbered when a diagnostic is "
    "retired.  The Diagnostic column names the `diag::` enumerator; the Message "
    "column is that diagnostic's wording as written in the `.td` file, with `%N` "
    "placeholders left in place.  Notes are listed per row in the Notes column. ";
  string OutOfScope =
    "Out-of-scope BSC features (not coded here): traits, async/await, generic, "
    "constexpr, operator overload, instance member functions.  These contribute "
    "several more errors, one warning (`warn_type_has_not_impl_trait`), and one "
    "note (`note_no_this_parameter`); they are marked `BSCOutOfScope` in the `.td` "
    "sources and are intentionally excluded from this reference.\n";
}
