//===--- ParseExprBSC.cpp - BSC Expression Parsing ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Expression parsing implementation for BSC.
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "clang/Parse/Parser.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/Parse/RAIIObjectsForParser.h"

using namespace clang;

ExprResult Parser::ParseOptionalBSCScopeSpecifier(
    CastParseKind ParseKind, bool isAddressOfOperand, bool &NotCastExpr,
    TypeCastState isTypeCast, bool isVectorLiteral, bool *NotPrimaryExpression,
    bool HasBSCScopeSpec) {
  CXXScopeSpec SS;
  if (Tok.is(tok::annot_template_id) && NextToken().is(tok::coloncolon)) {
    TemplateIdAnnotation *TemplateId = takeTemplateIdAnnotation(Tok);
    SourceLocation CCLoc = NextToken().getLocation();
    ASTTemplateArgsPtr TemplateArgsPtr(TemplateId->getTemplateArgs(),
                                        TemplateId->NumArgs);
    Actions.ActOnCXXNestedNameSpecifier(
        getCurScope(), SS, TemplateId->TemplateKWLoc, TemplateId->Template,
        TemplateId->TemplateNameLoc, TemplateId->LAngleLoc, TemplateArgsPtr,
        TemplateId->RAngleLoc, CCLoc, /*EnteringContext*/ false);
  }
  ParsingDeclSpec DS(*this);
  ParseBSCScopeSpecifiers(DS);
  ParsedAttributes EmptyDeclSpecAttrs(AttrFactory);
  ParsingDeclarator D(*this, DS, EmptyDeclSpecAttrs, DeclaratorContext::File);

  BSCScopeSpec BSS;
  BSS.setBeginLoc(DS.getBeginLoc());
  QualType T =
      Actions.ConvertBSCScopeSpecToType(D, DS.getBeginLoc(), false, BSS, DS);
  // For generic member functions, if the generic 'T' comes from a scope when
  // called, we need to obtain the NestedNameSpecifier from the scope and store
  // it in QualType to facilitate the creation of 'DependentScopeDeclRefExpr'
  // ast nodes.
  auto *TST = dyn_cast_or_null<TemplateSpecializationType>(T.getCanonicalType().getTypePtr());
  if (Tok.is(tok::coloncolon) && TST) {
    TagDecl *TD = dyn_cast_or_null<TagDecl>(Actions.getASTContext().BSCDeclContextMap[TST]);
    if (TD)
      TD->setQualifierInfo(SS.getWithLocInContext(Actions.getASTContext()));
  }
  HasBSCScopeSpec = TryConsumeToken(tok::coloncolon);
  D.getBSCScopeSpec() = BSS;

  if (Tok.isNot(tok::identifier)) {
    Diag(Tok, diag::err_expected_unqualified_id) << 0;
    return ExprError();
  }
  return ParseCastExpression(ParseKind, isAddressOfOperand, NotCastExpr,
                             isTypeCast, isVectorLiteral, NotPrimaryExpression,
                             T, HasBSCScopeSpec, DS.getBeginLoc()
#if ENABLE_BSC
                             ,
                             SS
#endif
  );
}

bool Parser::IsBSCStaticMemberFunctionCallInTemplateArgumentList() {
  if (Tok.isOneOf(tok::eof, tok::semi, tok::l_brace, tok::equal))
    return false;

  int LessCount = 0;
  int i = 0;
  Token CurrTok = Tok;
  Token NextTok = PP.LookAhead(i);
  while (!NextTok.isOneOf(tok::semi, tok::l_brace, tok::eof, tok::equal)) {
    if (CurrTok.is(tok::less))
      LessCount++;
    if (CurrTok.is(tok::greater))
      LessCount--;
    if (CurrTok.is(tok::greatergreater))
      LessCount = LessCount - 2;
    if (LessCount < 0) return false;
    if (!LessCount) {
      if (CurrTok.is(tok::coloncolon))
        return true;
      if (NextTok.isOneOf(tok::comma, tok::greater))
        return false;
    }
    CurrTok = NextTok;
    NextTok = PP.LookAhead(++i);
  }
  return false;
}

bool Parser::IsBSCStaticMemberFunctionCall() {
  assert(Tok.isOneOf(tok::identifier, tok::kw_union, tok::kw_enum, tok::kw_struct));
  if (NextToken().is(tok::eof))
    return false;
  Token NextTok = Tok.is(tok::identifier) ? PP.LookAhead(0) : PP.LookAhead(1);
  if (NextTok.is(tok::coloncolon))
    return true;
  if (NextTok.is(tok::less)) {
    if (Tok.isOneOf(tok::kw_struct, tok::kw_union)) {
      ParsedAttributes Attributes(AttrFactory);
      TryParseBSCGenericClassSpecifier(Attributes);
      if (Tok.is(tok::annot_template_id) && NextToken().is(tok::coloncolon))
        return true;
    } else {
      // Use lookahead to find the matching '>' and check if '::' follows,
      // without consuming any tokens.  Previous code consumed the identifier
      // and called AnnotateTemplateIdToken here, which left the token stream
      // mutated when '::' was not found, causing a crash.
      int Offset = 1; // LookAhead(0) is '<', start scanning from LookAhead(1)
      int Depth = 1;
      bool Found = false;
      while (Depth > 0) {
        Token T = PP.LookAhead(Offset);
        if (T.is(tok::eof))
          break;
        if (T.is(tok::less))
          Depth++;
        else if (T.is(tok::greater)) {
          Depth--;
          if (Depth == 0) {
            Found = true;
            break;
          }
        } else if (T.is(tok::greatergreater)) {
          Depth -= 2;
          if (Depth <= 0) {
            Found = (Depth == 0);
            break;
          }
        }
        Offset++;
      }
      if (Found && PP.LookAhead(Offset + 1).is(tok::coloncolon)) {
        // Now we know '::' follows — consume and annotate.
        IdentifierInfo &II = *Tok.getIdentifierInfo();
        TemplateTy Template;
        UnqualifiedId TemplateName;
        TemplateName.setIdentifier(&II, Tok.getLocation());
        CXXScopeSpec SS;
        bool MemberOfUnknownSpecialization;
        if (TemplateNameKind TNK =
                Actions.isTemplateName(getCurScope(), SS,
                                       /*hasTemplateKeyword=*/false, TemplateName,
                                       /*ObjectType=*/nullptr,
                                       /*EnteringContext=*/false, Template,
                                       MemberOfUnknownSpecialization)) {
          if (TNK == TNK_Type_template) {
            ConsumeToken();
            if (AnnotateTemplateIdToken(Template, TNK, SS, SourceLocation(),
                                        TemplateName, false))
              return false;
          }
        }
        if (Tok.is(tok::annot_template_id) && NextToken().is(tok::coloncolon))
          return true;
      }
    }
  }
  return false;
}

bool Parser::IsSupportedOverloadType(OverloadedOperatorKind Op) {
  switch (Op) {
  case OO_Plus:
  case OO_Minus:
  case OO_Star:
  case OO_Slash:
  case OO_Percent:
  case OO_Caret:
  case OO_Amp:
  case OO_Pipe:
  case OO_Tilde:
  case OO_LessLess:
  case OO_GreaterGreater:
  case OO_Less:
  case OO_LessEqual:
  case OO_Greater:
  case OO_GreaterEqual:
  case OO_EqualEqual:
  case OO_ExclaimEqual:
  case OO_Arrow:
  case OO_Subscript:
    return true;
  default:
    return false;
  }
}

/// ParseSafeExpression:In BSC grammar, use of the '_Safe' or '_Unsafe' keyword to
/// modify parenthetical expressions is permitted, such as `int a = _Safe
/// (funcall())`.
///
/// safe-expression
///   '_Safe' ParenExpression
///   '_Unsafe' ParenExpression
ExprResult Parser::ParseSafeExpression() {
  SafeZoneSpecifier SafeZoneSpec = SZ_None;
  SourceLocation SafeLoc;
  if (Tok.is(tok::kw__Safe)) {
    SafeZoneSpec = SZ_Safe;
    SafeLoc = ConsumeToken();
  } else if (Tok.is(tok::kw__Unsafe)) {
    SafeZoneSpec = SZ_Unsafe;
    SafeLoc = ConsumeToken();
  }
  if (Tok.isNot(tok::l_paren)) {
    Diag(Tok, diag::err_expected_lparen_after) << "_Safe";
    return ExprError();
  }
  struct ScopeSafeZoneInfo newInfo = {SafeZoneSpec == SZ_Safe, SafeLoc};
  struct ScopeSafeZoneInfo oldInfo = getCurScopeSafeZoneInfo();
  setCurScopeSafeZoneInfo(newInfo);

  ParenParseOption ParenExprType = SimpleExpr;
  ParsedType CastTy;
  SourceLocation LParenLoc = Tok.getLocation();
  SourceLocation RParenLoc;
  ExprResult SubExpr =
      ParseParenExpression(ParenExprType, false, false, CastTy, RParenLoc);
  if (SubExpr.isInvalid()) {
    setCurScopeSafeZoneInfo(oldInfo);
    return ExprError();
  }

  ExprResult Expr = Actions.ActOnSafeExpr(SafeZoneSpec, LParenLoc, RParenLoc,
                                          SubExpr.get());
  setCurScopeSafeZoneInfo(oldInfo);
  return Expr;
}

/// BSC lowering for trait values built from a unary address-of on a
/// trait-related operand. When the expression type desugars to a \c TraitDecl
/// and the form matches (unary op, pointer type, mapped vtable \c VarDecl),
/// replace \p Res with a compound literal of the trait record type, initialized
/// with \code { (void*)trait_data, &vtable } \endcode so subsequent parsing can
/// treat it like an ordinary value.
///
/// Preconditions: the caller has already gated on BSC and non-null \p Res.
///
/// \returns \p Res unchanged if no rewrite applies or record completion fails;
///          \c ExprError() when the expression tree already contains errors;
///          the new compound-literal expression otherwise.
ExprResult Parser::TryDesugarBSCTraitAddressExpr(ExprResult Res) {
  QualType ResType = Res.get()->getType();
  TraitDecl *TD = Actions.TryDesugarTrait(ResType);
  if (!TD) return Res;

  // TODO: if containsErrors(), we should return earlier, fix it when refact trait.
  if (Res.get()->containsErrors()) return ExprError();

  UnaryOperator *UO = dyn_cast<UnaryOperator>(Res.get()->IgnoreParenCasts());
  if (!UO) return Res;

  const Type *UOT = UO->getType().getTypePtr();
  if (!UOT || !UOT->isPointerType()) return Res;

  QualType QT = UOT->getPointeeType().getCanonicalType();
  VarDecl *LookupVar = TD->getTypeImpledVarDecl(QT);
  if (!LookupVar) return Res;

  QualType VoidPT = Actions.Context.getPointerType(Actions.Context.VoidTy);
  ImplicitCastExpr *TraitData = ImplicitCastExpr::Create(
      Actions.Context, VoidPT,
      /* CastKind=*/CK_BitCast,
      /* Expr=*/UO,
      /* CXXCastPath=*/nullptr,
      /* ExprValueKind=*/VK_PRValue,
      /* FPFeatures */ FPOptionsOverride());

  RecordDecl *LookupTrait = TD->getTrait();
  QualType VtableTy = LookupVar->getType();
  QualType RecordTy = Actions.Context.getRecordType(LookupTrait);
  if (LookupTrait->getDescribedClassTemplate()) {
    TypeSourceInfo *TInfo = Actions.Context.getTrivialTypeSourceInfo(ResType, Res.get()->getBeginLoc());
    RecordTy = Actions.CompleteRecordType(LookupTrait, TInfo);
  }
  if (RecordTy.isNull()) return Res;

  RecordTy = Actions.Context.getElaboratedType(ETK_Struct, nullptr,
                                               RecordTy);
  QualType VtablePT = Actions.Context.getPointerType(VtableTy);
  DeclRefExpr *VtableRef = Actions.BuildDeclRefExpr(
      LookupVar, VtableTy, VK_LValue, LookupVar->getLocation());
  UnaryOperator *UOVtable = UnaryOperator::Create(
      Actions.Context, VtableRef, UO_AddrOf, VtablePT, VK_PRValue,
      OK_Ordinary, SourceLocation(), false, FPOptionsOverride());

  std::vector<Expr *> Exprs = {TraitData, UOVtable};
  MutableArrayRef<Expr *> initExprs = MutableArrayRef<Expr *>(Exprs);
  ExprResult TraitInit = Actions.ActOnInitList(
      SourceLocation(), initExprs, SourceLocation());
  // The following line always succeeds
  InitListExpr *ILE = dyn_cast<InitListExpr>(TraitInit.get());

  ValueDecl *VD = dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParenCasts())->getDecl();
  VarDecl *NewVD = VarDecl::Create(
      Actions.Context, Actions.CurContext, VD->getBeginLoc(),
      VD->getLocation(), VD->getIdentifier(), RecordTy,
      Actions.Context.getTrivialTypeSourceInfo(RecordTy), SC_None);
  Actions.AddInitializerToDecl(NewVD, ILE, false);
  TypeSourceInfo *TInfo = Actions.Context.getTrivialTypeSourceInfo(ILE->getType());
  Res = Actions.BuildCompoundLiteralExpr(SourceLocation(), TInfo,
                                         SourceLocation(), ILE);
  return Res;
}

#endif // ENABLE_BSC