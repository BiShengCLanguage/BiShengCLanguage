//===--- BSCDiagDocsBase.inc - minimal BSCDiagDocs.td for error tests ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A minimal stand-in for clang/Basic/BSC/BSCDiagDocs.td that provides the
// metadata classes and the BSCDiagDocumentation record but no features, so the
// -gen-bsc-diag-docs error paths can be tested deterministically.
//
//===----------------------------------------------------------------------===//

class BSCDiagFeature<string name, string title, int order,
                     string sectionNote = "", string sectionFooter = ""> {
  string Name = name;
  string Title = title;
  int Order = order;
  string SectionNote = sectionNote;
  string SectionFooter = sectionFooter;
}

class BSCDiag<string feature, int number, string doc = "",
              string sectionFooter = ""> {
  string BSCFeature = feature;
  int BSCNumber = number;
  string BSCDoc = doc;
  string SectionFooter = sectionFooter;
}

class BSCOutOfScope {}

def BSCDiagDocumentation {
  string Intro = "";
  string OutOfScope = "";
}
