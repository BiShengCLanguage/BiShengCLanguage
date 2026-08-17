//===--- SemaBSCTrait.cpp - Semantic Analysis for BSC Trait ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for bishengc trait
//
//===----------------------------------------------------------------------===//

#if ENABLE_BSC

#include "TypeLocBuilder.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/BSC/DeclBSC.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Sema/Designator.h"
#include "clang/Sema/Initialization.h"
#include "clang/Sema/SemaInternal.h"

using namespace clang;
using namespace sema;

namespace {
/// If a tag with this name is already in scope, return a declaration suitable
/// for note_previous_definition; otherwise nullptr.
NamedDecl *findConflictingTraitDesugarTagDecl(Sema &S,
                                              const std::string &TraitName) {
  LookupResult R(S, &S.getASTContext().Idents.get(TraitName), SourceLocation(),
                 Sema::LookupTagName, Sema::NotForRedeclaration);
  S.LookupName(R, S.TUScope);
  if (R.empty())
    return nullptr;
  return R.getRepresentativeDecl()->getUnderlyingDecl();
}

/// Create the desugared struct RecordDecl for \p TD, optionally as a class
/// template matching the trait's template head, and push it (or the template)
/// onto the scope chain. Does not start the struct definition.
static RecordDecl *createTraitDesugarStructAndPush(Sema &S, TraitDecl *TD,
                                                   const std::string &StructName) {
  ASTContext &Ctx = S.getASTContext();
  DeclContext *DC = S.CurContext;
  SourceLocation NameLoc = TD->getLocation();
  SourceLocation StartLoc = TD->getBeginLoc();
  IdentifierInfo &II = Ctx.Idents.get(StructName);

  TraitTemplateDecl *TTD = TD->getDescribedTraitTemplate();
  TemplateParameterList *TParams = nullptr;
  const bool DelayTypeCreation = TTD != nullptr;
  if (TTD)
    TParams = TTD->getTemplateParameters();

  RecordDecl *RD = RecordDecl::Create(Ctx, TTK_Struct, DC, StartLoc, NameLoc,
                                      &II, nullptr, DelayTypeCreation);
  RD->setLexicalDeclContext(DC);

  Scope *Outer = S.getCurScope();
  while ((Outer->getFlags() & Scope::TemplateParamScope) != 0)
    Outer = Outer->getParent();

  if (DelayTypeCreation) {
    ClassTemplateDecl *CTD =
        ClassTemplateDecl::Create(Ctx, DC, StartLoc, &II, TParams, RD);
    S.PushOnScopeChains(CTD, Outer);
    RD->setDescribedClassTemplate(CTD);
    QualType T = CTD->getInjectedClassNameSpecialization();
    T = Ctx.getInjectedClassNameType(RD, T);
    assert(T->isDependentType() && "Class template type is not dependent?");
    CTD->setLexicalDeclContext(DC);
  } else {
    S.PushOnScopeChains(RD, Outer);
  }
  return RD;
}
} // namespace

bool Sema::CheckTraitDesugarTagNamesAvailable(TraitDecl *TD) {
  const std::string Names[] = {TD->getDesugarVtableRecordName(),
                               TD->getDesugarTraitRecordName(),
                               TD->getDesugarOwnedTraitRecordName(),
                               TD->getDesugarBorrowTraitRecordName()};
  for (const std::string &N : Names) {
    if (NamedDecl *Prev = findConflictingTraitDesugarTagDecl(*this, N)) {
      Diag(TD->getLocation(), diag::err_redefinition) << TD;
      Diag(Prev->getLocation(), diag::note_previous_definition);
      return false;
    }
  }
  return true;
}

// When we see a trait like:
// trait I {
//   int g(This *this);
// };
// We generate two structs in ast:
// |--RecordDecl  struct Trait_I_Vtable
//  `---FieldDecl  g 'int (*)(void *this)'
// |--RecordDecl  struct Trait_I
//  `---FieldDecl  data (*)(void)
//  `---FieldDecl  vtable struct (*)Trait_I_Vtable
RecordDecl *Sema::ActOnDesugarVtableRecord(TraitDecl *TD) {
  RecordDecl *TraitVtableRD = createTraitDesugarStructAndPush(
      *this, TD, TD->getDesugarVtableRecordName());

  TraitVtableRD->startDefinition();
  for (TraitDecl::field_iterator FieldIt = TD->field_begin();
       FieldIt != TD->field_end(); ++FieldIt) {
    QualType FT = FieldIt->getType();
    if (auto *FPT = FT->getAs<FunctionProtoType>()) {
      SmallVector<QualType, 4> Args;
      for (unsigned i = 0; i < FPT->getNumParams(); i++) {
        QualType T = FPT->getParamType(i);
        if (T->isPointerType() &&
            T->getPointeeType().getCanonicalType().getTypePtr() ==
                Context.ThisTy.getTypePtr()) {
          QualType ThisPT = Context.getQualifiedType(
              Context.VoidTy, T->getPointeeType().getLocalQualifiers());
          ThisPT = Context.getPointerType(ThisPT);
          ThisPT = Context.getQualifiedType(ThisPT, T.getLocalQualifiers());
          Args.push_back(ThisPT);
        } else {
          Args.push_back(T);
        }
      }
      SourceLocation BL = FieldIt->getBeginLoc();
      SourceLocation EL = FieldIt->getEndLoc();
      IdentifierInfo *Name = TD->getIdentifier();
      QualType FunctionTy = BuildFunctionType(FPT->getReturnType(), Args, BL,
                                              Name, FPT->getExtProtoInfo());
      QualType PT = BuildPointerType(FunctionTy, BL, Name);

      // Set SourceLocation and Param information for TypeSourceInfo to use
      // during serialization
      TypeSourceInfo *TInfo = Context.getTrivialTypeSourceInfo(PT);
      UnqualTypeLoc CurrTL = TInfo->getTypeLoc().getUnqualifiedLoc();
      CurrTL.getAs<PointerTypeLoc>().setStarLoc(BL);
      CurrTL = CurrTL.getNextTypeLoc().getUnqualifiedLoc();
      FunctionTypeLoc FTL = CurrTL.getAs<FunctionTypeLoc>();
      FTL.setLocalRangeBegin(BL);
      FTL.setLocalRangeEnd(EL);
      FTL.setLParenLoc(BL);
      FTL.setRParenLoc(EL);

      FieldDecl *NewFD = FieldDecl::Create(Context, TraitVtableRD, BL, EL,
                                           FieldIt->getIdentifier(), PT, TInfo,
                                           nullptr, false, ICIS_NoInit);
      NewFD->setAccess(AS_public);
      TraitVtableRD->addDecl(NewFD);
    }
  }
  TraitVtableRD->completeDefinition();
  Context.BSCDesugaredMap[TD].push_back(TraitVtableRD);
  return TraitVtableRD;
}

RecordDecl *Sema::ActOnDesugarTraitRecord(TraitDecl *TD,
                                          RecordDecl *TraitVtableRD,
                                          bool addOwned,
                                          bool addBorrow) {
  SourceLocation NameLoc = TD->getLocation();
  SourceLocation StartLoc = TD->getBeginLoc();
  std::string TraitName = addOwned    ? TD->getDesugarOwnedTraitRecordName()
                          : addBorrow ? TD->getDesugarBorrowTraitRecordName()
                                      : TD->getDesugarTraitRecordName();
  RecordDecl *TraitRD = createTraitDesugarStructAndPush(*this, TD, TraitName);

  TraitRD->startDefinition();
  std::string DataName = "data";
  std::string VtableName = "vtable";
  QualType DataPT = Context.getPointerType(Context.VoidTy);
  if (addOwned)
    DataPT.addOwned();
  if (addBorrow)
    DataPT.addBorrow();
  QualType RecordTy = Context.getRecordType(TraitVtableRD);
  ClassTemplateDecl *CTD = TraitVtableRD->getDescribedClassTemplate();
  if (CTD)
    RecordTy = CTD->getInjectedClassNameSpecialization();
  RecordTy = Context.getElaboratedType(ETK_Struct, nullptr, RecordTy);
  QualType VtablePT = Context.getPointerType(RecordTy);
  TypeSourceInfo *TInfo = Context.getTrivialTypeSourceInfo(VtablePT);

  FieldDecl *DataFD = FieldDecl::Create(
      Context, TraitRD, StartLoc, NameLoc, &Context.Idents.get(DataName),
      DataPT, Context.getTrivialTypeSourceInfo(DataPT), nullptr, false,
      ICIS_NoInit);
  FieldDecl *VtableFD = FieldDecl::Create(
      Context, TraitRD, StartLoc, NameLoc, &Context.Idents.get(VtableName),
      VtablePT, TInfo, nullptr, false, ICIS_NoInit);
  DataFD->setAccess(AS_public);
  VtableFD->setAccess(AS_public);
  TraitRD->addDecl(DataFD);
  TraitRD->addDecl(VtableFD);
  TraitRD->completeDefinition();
  TraitRD->setDesugaredTraitDecl(TD);
  Context.BSCDesugaredMap[TD].push_back(TraitRD);
  return TraitRD;
}

static std::string TypeAsString(QualType T) {
  // For trait types, build the symbol fragment directly from the declaration
  // name, using the stable internal prefix "trait_" regardless of the
  // user-facing keyword spelling.
  if (const TraitType *TT = dyn_cast<TraitType>(T))
    return "trait_" + TT->getDecl()->getNameAsString();
  PrintingPolicy PrintPolicy = LangOptions();
  SplitQualType T_split = T.split();
  std::string ExtendedTypeStr = T.getAsString(T_split, PrintPolicy);
  for (int i = ExtendedTypeStr.length() - 1; i >= 0; i--) {
    if (ExtendedTypeStr[i] == ' ') {
      ExtendedTypeStr.replace(i, 1, "_");
    } else if (ExtendedTypeStr[i] == '*') {
      // Since '*' is not allowed to appear in identifier,
      // we replace it with 'P'.
      // FIXME: it may conflict with user defined type Char_P.
      ExtendedTypeStr.replace(i, 1, "P");
    }
  }
  return ExtendedTypeStr;
}

ImplTraitDecl *Sema::BuildImplTraitDecl(Scope *S, Declarator &TypeDeclarator,
                                        Declarator &TraitDeclarator,
                                        TraitDecl *TD) {
  TypeSourceInfo *TypeInfo = GetTypeForDeclarator(TypeDeclarator, S);
  QualType ImplQT = TypeInfo->getType();
  TypeSourceInfo *TraitInfo = GetTypeForDeclarator(TraitDeclarator, S);
  QualType TraitQT =
      TraitInfo->getType()->getLocallyUnqualifiedSingleStepDesugaredType();
  std::string Name =
      GetNameForDeclarator(TraitDeclarator).getName().getAsString();
  if (auto TST = dyn_cast<TemplateSpecializationType>(TraitQT)) {
    for (auto it = TST->begin(); it != TST->end(); ++it) {
      if (!it->getAsType().isNull())
        Name += "_" + TypeAsString(it->getAsType());
    }
  }

  ImplTraitDecl *ITD = ImplTraitDecl::Create(
      Context, CurContext, TraitDeclarator.getBeginLoc(),
      TypeDeclarator.getBeginLoc(), &Context.Idents.get(Name), ImplQT, TypeInfo,
      StorageClass::SC_None);
  ITD->setTraitDecl(TD);
  CurContext->addDecl(ITD);
  return ITD;
}

void Sema::ActOnFinishTraitMemberSpecification(Decl *TagDecl) {
  if (!TagDecl)
    return;
  AdjustDeclIfTemplate(TagDecl);

  cast<TraitDecl>(TagDecl)->completeDefinition();
}

ExprResult Sema::AddAfterStructTrait(ExprResult ULE, SourceLocation DSLoc,
                                     std::string ID) {
  CXXScopeSpec SS;
  SourceLocation TemplateKWLoc;
  UnqualifiedId Name;
  IdentifierInfo *VId = &Context.Idents.get(ID);
  Name.setIdentifier(VId, DSLoc);
  ULE = ActOnMemberAccessExpr(getCurScope(), ULE.get(), DSLoc, tok::period, SS,
                              TemplateKWLoc, Name, nullptr);
  return ULE;
}

Expr *Sema::ConvertParmTraitToStructTrait(Expr *UO, QualType ProtoArgType,
                                          SourceLocation DSLoc) {
  QualType T = UO->getType();
  const PointerType *PT = dyn_cast_or_null<PointerType>(T.getTypePtr());
  TraitDecl *TD = TryDesugarTrait(ProtoArgType);
  QualType QT = CompleteTraitType(ProtoArgType)->getPointeeType();
  if (!PT || !TD) {
    Diag(DSLoc, diag::err_type_has_not_impl_trait) << QT << T;
    return nullptr;
  }
  // For "impl trait TR for struct S",
  // this might be a ElaboratedType for "struct S"
  T = PT->getPointeeType().getCanonicalType();
  VarDecl *LookupVar = TD->getTypeImpledVarDecl(T);
  if (!LookupVar) {
    Diag(DSLoc, diag::err_type_has_not_impl_trait) << QT << T;
    return nullptr;
  }
  LookupVar->setIsUsed();

  QualType VoidPT = Context.getPointerType(Context.VoidTy);
  if ((ProtoArgType.isOwnedQualified() || ProtoArgType->isMoveSemanticType()) &&
      UO->getType().isOwnedQualified())
    VoidPT.addOwned();
  else if (ProtoArgType->hasBorrowFields() && UO->getType().isBorrowQualified())
    VoidPT.addBorrow();
  QualType VtablePT = QualType();
  RecordDecl *RD = ProtoArgType->getAsRecordDecl();
  if (!RD) {
    Diag(DSLoc, diag::err_type_has_not_impl_trait) << QT << T;
    return nullptr;
  }
  for (auto Field : RD->fields()) {
    if (Field->getNameAsString() == "vtable")
      VtablePT = Field->getType();
  }
  if (VtablePT.isNull())
    return ExprError().get();
  QualType VtableTy = VtablePT->getPointeeType();

  ExprResult TraitData =
      ImplicitCastExpr::Create(Context, VoidPT, CK_BitCast, UO, nullptr,
                               VK_PRValue, FPOptionsOverride());
  Designation Desig;
  Desig.AddDesignator(
      Designator::getField(&Context.Idents.get("data"), DSLoc, DSLoc));
  TraitData = ActOnDesignatedInitializer(Desig, DSLoc, false, TraitData);
  DeclRefExpr *VtableRef =
      DeclRefExpr::Create(Context, NestedNameSpecifierLoc(), SourceLocation(),
                          LookupVar, false, DSLoc, VtableTy, VK_LValue);
  ExprResult UOVtable =
      UnaryOperator::Create(Context, VtableRef, UO_AddrOf, VtablePT, VK_PRValue,
                            OK_Ordinary, DSLoc, false, FPOptionsOverride());
  Designation Desig1;
  Desig1.AddDesignator(
      Designator::getField(&Context.Idents.get("vtable"), DSLoc, DSLoc));
  UOVtable = ActOnDesignatedInitializer(Desig1, DSLoc, false, UOVtable);
  std::vector<Expr *> Exprs = {TraitData.get(), UOVtable.get()};
  MutableArrayRef<Expr *> initExprs = MutableArrayRef<Expr *>(Exprs);
  ExprResult Result = ActOnInitList(DSLoc, initExprs, DSLoc);
  dyn_cast<InitListExpr>(Result.getAs<Expr>())->setDesugaredIndex(Exprs.size());
  TypeSourceInfo *TInfo = Context.getTrivialTypeSourceInfo(ProtoArgType);
  TypeResult TR = CreateParsedType(ProtoArgType, TInfo);
  Result = ActOnCompoundLiteral(DSLoc, TR.get(), DSLoc, Result.get());
  return Result.get();
}

static bool IsImplTraitDeclIllegal(Sema &S, QualType TraitQT, QualType &ImplQT,
                                   SourceLocation TypeLoc, TraitDecl *TD,
                                   QualType OriginTraitTy) {
  CXXScopeSpec SS;

  if (ImplQT.getCanonicalType()->isRecordType()) {
    const RecordDecl *RD =
        ImplQT.getCanonicalType()->castAs<RecordType>()->getDecl();
    if (isa<ClassTemplateSpecializationDecl>(RD)) {
      S.Diag(TypeLoc, diag::err_impl_trait_for_instantiated_type);
      return true;
    }
  }

  IdentifierInfo *FunctionID = nullptr;
  RecordDecl *VT = TD->getVtable();
  const TemplateSpecializationType *TST =
      TraitQT->getAs<TemplateSpecializationType>();
  if (TST) {
    ClassTemplateDecl *CTD = VT->getDescribedClassTemplate();
    void *InsertPos = nullptr;
    TemplateArgumentListInfo Args(TypeLoc, TypeLoc);
    for (auto T : TST->template_arguments())
      Args.addArgument(TemplateArgumentLoc(
          T, S.Context.getTrivialTypeSourceInfo(T.getAsType(), TypeLoc)));
    SmallVector<TemplateArgument, 4> Converted;
    S.CheckTemplateArgumentList(CTD, CTD->getBeginLoc(), Args, false, Converted,
                                /*UpdateArgsWithConversions=*/true);
    if (ClassTemplateSpecializationDecl *CTSD =
            CTD->findSpecialization(Converted, InsertPos))
      VT = CTSD;
  }

  for (RecordDecl::field_iterator FieldIt = VT->field_begin();
       FieldIt != VT->field_end(); ++FieldIt) {
    FunctionID = FieldIt->getIdentifier();
    QualType TraitQT = FieldIt->getType()->getPointeeType();
    const FunctionProtoType *TraitTy = TraitQT->getAs<FunctionProtoType>();
    BSCMethodDecl *MD = nullptr;
    DeclContext *DC = nullptr;
    if(S.getASTContext().BSCDeclContextMap.find(ImplQT.getCanonicalType().getTypePtr()) != 
       S.getASTContext().BSCDeclContextMap.end()){
      DC = S.getASTContext().BSCDeclContextMap[ImplQT.getCanonicalType().getTypePtr()];
    }
    if (DC) {
      DeclContext::lookup_result DR = DC->lookup(FunctionID);
      for (NamedDecl *D : DR)
        if (D)
          MD = dyn_cast<BSCMethodDecl>(D);
    }
    // Check whether function prototypes match in traitBody and type's member
    // funcs
    QualType MethodQT = MD->getType();
    const FunctionProtoType *MethodTy = MethodQT->getAs<FunctionProtoType>();
    bool TypeDiagFlag = false;
    if (!MD->getHasThisParam() ||
        !S.Context.hasSameFunctionTypeIgnoringPtrSizes(
            TraitTy->getReturnType(), MethodTy->getReturnType()))
      TypeDiagFlag = true;
    else {
      int n = TraitTy->getNumParams();
      int m = MethodTy->getNumParams();
      if (n == m)
        for (int i = 1; i < n; ++i) {
          QualType T1 = TraitTy->getParamType(i);
          QualType T2 = MethodTy->getParamType(i);
          if (!S.Context.hasSameFunctionTypeIgnoringPtrSizes(T1, T2))
            TypeDiagFlag = true;
        }
      else
        TypeDiagFlag = true;
    }
    if (TypeDiagFlag) {
      S.Diag(TypeLoc, diag::err_trait_impl_function_type_conflict)
          << FieldIt->getIdentifier() << OriginTraitTy;
      return true;
    }
  }
  return false;
}

QualType Sema::CompleteTraitType(QualType QT) {
  TraitDecl *TD = TryDesugarTrait(QT);
  if (!TD || !IsDesugaredFromTraitType(QT)) {
    return QT;
  }
  QualType TraitQT = Context.getTraitType(TD);
  // We convert 'struct __Trait_F' to 'Train F *',
  // so the number of pointers initialized is 1.
  int PointerNum = 1;
  while (QT->isPointerType()) {
    QT = QT->getPointeeType();
    PointerNum++;
  }
  if (TraitTemplateDecl *TTD = TD->getDescribedTraitTemplate()) {
    TemplateArgumentListInfo Args(TTD->getBeginLoc(), TTD->getEndLoc());
    const TemplateSpecializationType *TST =
        dyn_cast<TemplateSpecializationType>(
            QT->getLocallyUnqualifiedSingleStepDesugaredType());
    if (!TST)
      return QT;
    for (auto T : TST->template_arguments())
      Args.addArgument(TemplateArgumentLoc(
          T,
          Context.getTrivialTypeSourceInfo(T.getAsType(), TTD->getBeginLoc())));
    TraitQT = CheckTemplateIdType(TemplateName(TTD), TTD->getBeginLoc(), Args);
    if (!TraitQT.isNull())
      TraitQT = Context.getElaboratedType(ETK_Trait, nullptr, TraitQT);
  }
  for (int i = 0; i < PointerNum; i++)
    TraitQT = Context.getPointerType(TraitQT);
  return TraitQT;
}

QualType Sema::CompleteRecordType(RecordDecl *RD, TypeSourceInfo *TInfo) {
  return CompleteRecordType(RD, TInfo->getTypeLoc().getBeginLoc(),
                            TInfo->getTypeLoc().getEndLoc(), TInfo->getType());
}

QualType Sema::CompleteRecordType(RecordDecl *RD, SourceLocation BL,
                                  SourceLocation EL, QualType QT) {
  ClassTemplateDecl *CTD = RD->getDescribedClassTemplate();
  TemplateArgumentListInfo Args(BL, EL);
  while (QT->isPointerType())
    QT = QT->getPointeeType();
  while (isa<TemplateSpecializationType>(QT) || isa<TypedefType>(QT)) {
    // In this case:
    //   typedef G<T> = trait F<T>;
    //   impl G<int> for int;
    // QT is a TemplateSpecializationType but not TypedefType.
    if (const TemplateSpecializationType *TT =
            dyn_cast<TemplateSpecializationType>(QT)) {
      if (TT->isTypeAlias())
        QT = TT->getAliasedType();
      else
        break;
      // In this case:
      //   typedef G = trait F<int>;
      //   impl G for int;
      // QT is a TypedefType.
    } else if (const TypedefType *TT = dyn_cast<TypedefType>(QT))
      QT = TT->desugar();
  }
  // Remove ElaboratedType
  const TemplateSpecializationType *TST = dyn_cast<TemplateSpecializationType>(
      QT->getLocallyUnqualifiedSingleStepDesugaredType());
  if (!TST)
    return QualType();
  for (auto T : TST->template_arguments())
    Args.addArgument(TemplateArgumentLoc(
        T, Context.getTrivialTypeSourceInfo(T.getAsType(), BL)));
  QualType TraitQT = CheckTemplateIdType(TemplateName(CTD), BL, Args);
  if (TraitQT.isNull())
    return QualType();
  if (RequireCompleteType(BL, TraitQT,
                          diag::err_typecheck_decl_incomplete_type))
    return QualType();
  return TraitQT;
}

VarDecl *Sema::DesugarImplTrait(ImplTraitDecl *ITD, Declarator &TypeDeclarator,
                                Declarator &TraitDeclarator,
                                SourceLocation TypeLoc) {
  Scope *S = getCurScope();
  return DesugarImplTrait(
      ITD->getTraitDecl(), ITD->getLocation(), TraitDeclarator.getBeginLoc(),
      TraitDeclarator.getEndLoc(), ITD->getSourceRange(),
      GetTypeForDeclarator(TypeDeclarator, S)->getType().getCanonicalType(),
      GetTypeForDeclarator(TraitDeclarator, S)->getType(), TypeLoc, true);
}

VarDecl *Sema::DesugarImplTrait(TraitDecl *TD, SourceLocation TraitLoc,
                                SourceLocation TraitLocBegin,
                                SourceLocation TraitLocEnd,
                                SourceRange TraitImplRange, QualType ImplQT,
                                QualType OriginTraitTy, SourceLocation TypeLoc,
                                bool Initialize) {
  CXXScopeSpec SS;
  Scope *S = getCurScope();
  DeclContext *DC = CurContext;

  RecordDecl *TraitVT = TD->getVtable();
  QualType TraitQT = TraitVT->getTypeForDecl()->getCanonicalTypeInternal();
  if (TraitVT->getDescribedClassTemplate())
    TraitQT =
        CompleteRecordType(TraitVT, TraitLocBegin, TraitLocEnd, OriginTraitTy);
  if (TraitQT.isNull())
    return nullptr;
  TraitQT = Context.getElaboratedType(ETK_Struct, nullptr, TraitQT);
  VarDecl *LookupVar = TD->getTypeImpledVarDecl(ImplQT);
  // If we have the same ImplTraitDecl before, return nullptr
  if (LookupVar)
    return nullptr;

  std::string ImplTraitName = "__" + TypeAsString(ImplQT) + "_" +
                              TypeAsString(OriginTraitTy.getCanonicalType());

  IdentifierInfo *ITII = &Context.Idents.get(ImplTraitName);
  StorageClass SC = clang::SC_None;
  VarDecl *NewVD = VarDecl::Create(
      Context, DC, TraitImplRange.getBegin(), TraitImplRange.getEnd(), ITII,
      TraitQT, Context.getTrivialTypeSourceInfo(TraitQT), SC);
  NewVD->setLexicalDeclContext(CurContext);

  if (Initialize) {
    SmallVector<Expr *, 12> InitExprs;
    for (RecordDecl::field_iterator FieldIt = TraitVT->field_begin();
         FieldIt != TraitVT->field_end(); ++FieldIt) {
      IdentifierInfo *II = FieldIt->getIdentifier();
      DeclContext *LookupDC = nullptr;
      const Type *BasedType = ImplQT.getCanonicalType().getTypePtr();
      if (Context.BSCDeclContextMap.find(BasedType) !=
          Context.BSCDeclContextMap.end()) {
        LookupDC = Context.BSCDeclContextMap[BasedType];
      }
      DeclContext::lookup_result Decls;
      if (LookupDC)
        Decls = LookupDC->lookup(DeclarationName(II));
      BSCMethodDecl *MD = nullptr;
      for (DeclContext::lookup_result::iterator I = Decls.begin(),
                                                E = Decls.end();
           I != E; ++I) {
        if (isa<BSCMethodDecl>(*I) &&
            dyn_cast<BSCMethodDecl>(*I)->isDefined()) {
          MD = dyn_cast<BSCMethodDecl>(*I);
          break;
        }
      }

      if (MD) {
        QualType FT = MD->getType();
        assert(FT->isFunctionProtoType());
        const FunctionProtoType *FPT = FT->getAs<FunctionProtoType>();
        SmallVector<QualType, 16> ParamTys;
        assert(FPT->getNumParams() >= 1 &&
               FPT->getParamType(0)->isPointerType());
        QualType VoidPT = Context.getQualifiedType(
            Context.VoidTy,
            FPT->getParamType(0)->getPointeeType().getLocalQualifiers());
        VoidPT = Context.getPointerType(VoidPT);
        VoidPT = Context.getQualifiedType(
            VoidPT, FPT->getParamType(0).getLocalQualifiers());
        ParamTys.push_back(VoidPT);

        for (unsigned int i = 1; i < FPT->getNumParams(); i++) {
          ParamTys.push_back(FPT->getParamType(i));
        }
        QualType FuncTy = Context.getFunctionType(
            FPT->getReturnType(), ParamTys, FPT->getExtProtoInfo());
        QualType ParenTy = Context.getParenType(FuncTy);
        QualType PointerTy = Context.getPointerType(ParenTy);
        TypeSourceInfo *TInfo = Context.getTrivialTypeSourceInfo(PointerTy);
        ExprResult Res = BuildDeclRefExpr(MD, MD->getType(), VK_LValue,
                                          TraitImplRange.getBegin());
        Res.get()->HasBSCScopeSpec = true;
        Res = ImplicitCastExpr::Create(
            Context, Context.getPointerType(Res.get()->getType()),
            CK_FunctionToPointerDecay, Res.get(), nullptr, VK_PRValue,
            FPOptionsOverride());
        Res.get()->IsDesugaredCastExpr = true;
        Res = BuildCStyleCastExpr(TraitImplRange.getBegin(), TInfo,
                                  TraitImplRange.getEnd(), Res.get())
                  .get();
        Designation Desig;
        Desig.AddDesignator(Designator::getField(II, TraitImplRange.getBegin(),
                                                 TraitImplRange.getBegin()));
        Res = ActOnDesignatedInitializer(Desig, TraitImplRange.getBegin(),
                                         false, Res);
        InitExprs.push_back(Res.get());
      } else {
        Diag(TypeLoc, diag::err_trait_impl) << II << OriginTraitTy << ImplQT;
        return nullptr;
      }
    }

    ExprResult ER = BuildInitList(TraitImplRange.getBegin(), InitExprs,
                                  TraitImplRange.getEnd());
    InitListExpr *ILE = dyn_cast<InitListExpr>(ER.get());
    AddInitializerToDecl(NewVD, ILE, false);
  } else {
    // Add external keyword
    NewVD->setStorageClass(SC_Extern);
  }
  PushOnScopeChains(NewVD, S, true);

  if (IsImplTraitDeclIllegal(*this, TraitQT, ImplQT, TypeLoc, TD,
                             OriginTraitTy))
    return nullptr;

  TD->MapInsert(ImplQT, NewVD);
  return NewVD;
}

QualType Sema::DesugarTraitToStructTrait(TraitDecl *TD, QualType T,
                                         SourceLocation Loc) {
  RecordDecl *RD = nullptr;
  if (T.isOwnedQualified())
    RD = TD->getOwnedTrait();
  else if (T.isBorrowQualified())
    RD = TD->getBorrowTrait();
  else
    RD = TD->getTrait();
  if (!RD) {
    Diag(Loc, diag::err_trait_is_undefined) << Context.getTagDeclType(TD);
    return T;
  }
  QualType RT = QualType();
  QualType InnerTy = T;
  while (InnerTy->isPointerType())
    InnerTy = InnerTy->getPointeeType();
  while (isa<TemplateSpecializationType>(InnerTy) ||
         isa<TypedefType>(InnerTy)) {
    if (const TemplateSpecializationType *TT =
            dyn_cast<TemplateSpecializationType>(InnerTy)) {
      if (TT->isTypeAlias())
        InnerTy = TT->getAliasedType();
      else
        break;
    } else if (const TypedefType *TT = dyn_cast<TypedefType>(InnerTy))
      InnerTy = TT->desugar();
  }
  // Remove ElaboratedType
  if (dyn_cast<TemplateSpecializationType>(
          InnerTy->getLocallyUnqualifiedSingleStepDesugaredType())) {
    TypeSourceInfo *TInfo = Context.getTrivialTypeSourceInfo(InnerTy, Loc);
    RT = CompleteRecordType(RD, TInfo);
  } else {
    RT = RD->getTypeForDecl()->getCanonicalTypeInternal();
  }
  if (RT.isNull()) {
    return T;
  }
  RT = Context.getElaboratedType(ETK_Struct, nullptr, RT);
  assert(T->isPointerType() && "Trait must be of pointer type");
  T = T->getPointeeType();
  while (T->isPointerType()) {
    RT = Context.getPointerType(RT);
    T = T->getPointeeType();
  }

  return RT;
}

static int CountPointerLevels(QualType T) {
  int PointerLevels = 0;
  while (T->isPointerType()) {
    T = T->getPointeeType();
    ++PointerLevels;
  }
  return PointerLevels;
}

// In this function, we aim to assign the Trait_xxx struct,
// for example, we have desugared as follows:
//`struct Trait_I {
//`  void *data;
//`  void *vtable;
//`}
// now we parsing a stmt like: trait I *i = &s; (s is a struct S instance)
// or: trait I *i = (trait I*)&s; (s is a struct S instance)
// we should give the Trait_I assignment like:
//`struct Trait_I trait_i = {
//  .data = &s;
//  .vtable = *struct_S_Trait_I_Vtable;
//`}
VarDecl *Sema::ActOnDesugarTraitInstance(Decl *D) {
  VarDecl *VD = dyn_cast<VarDecl>(D);
  if (!VD)
    return nullptr;
  QualType QT = VD->getType();
  QualType OriginQT = VD->getType();
  TraitDecl *TD = nullptr;
  if (QT->isPointerType()) {
    TD = TryDesugarTrait(QT);
    if (TD) {
      RecordDecl *LookupTrait = nullptr;
      if (QT.isOwnedQualified())
        LookupTrait = TD->getOwnedTrait();
      else if (QT.isBorrowQualified())
        LookupTrait = TD->getBorrowTrait();
      else
        LookupTrait = TD->getTrait();
      if (!LookupTrait)
        return nullptr;
      if (LookupTrait->getDescribedClassTemplate()) {
        QT = CompleteRecordType(LookupTrait, VD->getTypeSourceInfo());
        // Error recovery: e.g. TU already has errors; need a non-null base type
        // before wrapping pointer levels (_Trait F<float>** ...).
        if (QT.isNull())
          QT = Context.getRecordType(LookupTrait);
      } else {
        QT = Context.getRecordType(LookupTrait);
      }
    }
    OriginQT = OriginQT->getPointeeType();
  }
  if (!QT.isNull())
    QT = Context.getElaboratedType(ETK_Struct, nullptr, QT);
  QualType TmpT = OriginQT.getCanonicalType();
  while (TmpT->isPointerType()) {
    if (QT.isNull())
      return nullptr;
    QT = Context.getPointerType(QT);
    TmpT = TmpT->getPointeeType();
  }

  if (TD == nullptr || QT.isNull())
    return nullptr;
  // build expr: .data = &s;
  if (TD->getVtable() == nullptr) {
    Diag(VD->getLocation(), diag::err_typecheck_decl_incomplete_type) << QT;
    return nullptr;
  }

  VarDecl *NewVD = VarDecl::Create(Context, CurContext, D->getBeginLoc(),
                                   D->getLocation(), VD->getIdentifier(), QT,
                                   Context.getTrivialTypeSourceInfo(QT),
                                   StorageClass::SC_None);

  PushOnScopeChains(NewVD, getCurScope(), true);
  Expr *Init = VD->getInit();
  if (Init == nullptr) // trait I *a;
    return NewVD;

  if (Init->containsErrors())
    return NewVD;

  Expr *UO = Init;
  if (CastExpr *Cexpr = dyn_cast<CastExpr>(Init)) {
    UO = Cexpr->getSubExpr();
  }

  QualType T = UO->getType();
  QualType InnerTy = T;
  while (InnerTy->isPointerType())
    InnerTy = InnerTy->getPointeeType();

  QualType TraitTy;
  if (VD->getType().isOwnedQualified())
    TraitTy = QualType(TD->getOwnedTrait()->getTypeForDecl(), 0);
  else if (VD->getType().isBorrowQualified())
    TraitTy = QualType(TD->getBorrowTrait()->getTypeForDecl(), 0);
  else
    TraitTy = QualType(TD->getTrait()->getTypeForDecl(), 0);
  if (dyn_cast<InjectedClassNameType>(TraitTy)) {
    QualType CompletedTrait =
        CompleteRecordType(TD->getTrait(), NewVD->getTypeSourceInfo());
    if (!CompletedTrait.isNull())
      TraitTy = CompletedTrait;
  }
  // @code
  // trait I **a;
  // trait I **b = a;
  // trait I **c = (trait F **)a;
  // @endcode
  if (!TraitTy.isNull() && Context.hasSameType(InnerTy, TraitTy)) {
    if (CountPointerLevels(Context.getPointerType(OriginQT)) !=
        CountPointerLevels(CompleteTraitType(T))) {
      Diag(UO->getBeginLoc(),
           diag::err_typecheck_convert_incompatible_pointer)
          << Context.getPointerType(OriginQT) << CompleteTraitType(T)
          << 4  // operation type: 4 = initializing
          << 0; // hint type: 0 = no additional hint
      return nullptr;
    }
    AddInitializerToDecl(NewVD, UO, false);
    return NewVD;
  }
  const PointerType *PT = dyn_cast_or_null<PointerType>(T.getTypePtr());
  if (!PT) {
    Diag(UO->getBeginLoc(), diag::err_type_has_not_impl_trait)
        << OriginQT << CompleteTraitType(T);
    return nullptr;
  }

  bool ExpIsNullPointer =
      Init->isNullPointerConstant(Context, Expr::NPC_ValueDependentIsNull);
  T = PT->getPointeeType().getCanonicalType();
  VarDecl *LookupVar = TD->getTypeImpledVarDecl(T);
  if (!LookupVar && !ExpIsNullPointer) {
    Diag(UO->getBeginLoc(), diag::err_type_has_not_impl_trait) << OriginQT << T;
    return nullptr;
  }

  std::vector<Expr *> Exprs;
  if (ExpIsNullPointer) {
    // @code
    // trait I **a = NULL;
    // @endcode
    if (QT->isPointerType()) {
      AddInitializerToDecl(NewVD, UO, false);
      return NewVD;
    }
    // @code
    // trait I *a = NULL;
    // @endcode
    RecordDecl *LookupVtable = TD->getVtable();
    QualType VtableTy = Context.getRecordType(LookupVtable);
    if (LookupVtable->getDescribedClassTemplate()) {
      VtableTy = CompleteRecordType(LookupVtable, VD->getTypeSourceInfo());
      VtableTy = Context.getElaboratedType(ETK_Struct, nullptr, VtableTy);
    }
    QualType VtablePT = Context.getPointerType(VtableTy);
    ImplicitCastExpr *TraitVtable =
        ImplicitCastExpr::Create(Context, VtablePT, CK_BitCast, UO, nullptr,
                                 VK_PRValue, FPOptionsOverride());
    Exprs = {UO, TraitVtable};
  } else {
    QualType VoidPT = Context.getPointerType(Context.VoidTy);
    if (VD->getType().isOwnedQualified())
      VoidPT.addOwned();
    if (VD->getType().isBorrowQualified())
      VoidPT.addBorrow();
    ImplicitCastExpr *TraitData =
        ImplicitCastExpr::Create(Context, VoidPT,
                                 /* CastKind=*/CK_BitCast,
                                 /* Expr=*/UO,
                                 /* CXXCastPath=*/nullptr,
                                 /* ExprValueKind=*/VK_PRValue,
                                 /* FPFeatures */ FPOptionsOverride());

    QualType VtableTy = LookupVar->getType();
    QualType VtablePT = Context.getPointerType(VtableTy);
    DeclRefExpr *VtableRef = DeclRefExpr::Create(
        Context, NestedNameSpecifierLoc(), SourceLocation(), LookupVar, false,
        SourceLocation(), VtableTy, VK_LValue);
    UnaryOperator *UOVtable = UnaryOperator::Create(
        Context, VtableRef, UO_AddrOf, VtablePT, VK_PRValue, OK_Ordinary,
        SourceLocation(), false, FPOptionsOverride());
    Exprs = {TraitData, UOVtable};
  }

  MutableArrayRef<Expr *> initExprs = MutableArrayRef<Expr *>(Exprs);
  ExprResult TraitInit =
      ActOnInitList(SourceLocation(), initExprs, SourceLocation());
  AddInitializerToDecl(NewVD, TraitInit.get(), false);
  return NewVD;
}

NamedDecl *Sema::ActOnTraitMemberDeclarator(Scope *S, Declarator &D) {
  D.setIsTraitMember(true);
  MultiTemplateParamsArg TemplateParams;
  NamedDecl *Member = HandleDeclarator(S, D, TemplateParams);
  return Member;
}

// Desugars complex type declarations in the following code example:
// @code
// struct S<T> { trait F* a; };
// struct S<int> s = { &x };
// @endcode
void Sema::ActOnDesugarTraitExprInStruct(InitListExpr *IList, Expr *expr,
                                         QualType ElemType, unsigned &Index,
                                         DesignatedInitExpr **DIE) {
  if (Index < IList->getDesugaredIndex()) {
    return;
  }
  TraitDecl *TD = TryDesugarTrait(ElemType);
  if (!TD) {
    QualType InnerTy = ElemType;
    while (InnerTy->isPointerType())
      InnerTy = InnerTy->getPointeeType();
    TD = TryDesugarTrait(InnerTy);
    if (!TD)
      return;
  }

  Expr *UO = expr;
  if (CastExpr *Cexpr = dyn_cast<CastExpr>(UO)) {
    while (CastExpr *Tmp = dyn_cast<CastExpr>(Cexpr->getSubExpr()))
      Cexpr = Tmp;
    UO = Cexpr->getSubExpr();
  }

  // Since the index is calculated from 0, we need to add 1 to indicate that the
  // first index has been desugared, and so on.
  IList->setDesugaredIndex(Index + 1);
  QualType T = UO->getType();
  // If the right value is trait pointer type , there is no need to desugar
  if (TD == TryDesugarTrait(T) || Context.hasSameType(ElemType, T)) {
    if (DIE && *DIE)
      (*DIE)->setInit(UO);
    IList->updateInit(Context, Index, UO);
    return;
  }

  SourceLocation SL = UO->getBeginLoc();
  const PointerType *PT = dyn_cast_or_null<PointerType>(T.getTypePtr());
  if (!PT) {
    Diag(SL, diag::err_type_has_not_impl_trait)
        << CompleteTraitType(ElemType)->getPointeeType() << T;
    return;
  }

  T = PT->getPointeeType().getCanonicalType();
  VarDecl *LookupVar = TD->getTypeImpledVarDecl(T);
  bool ExpIsNullPointer =
      UO->isNullPointerConstant(Context, Expr::NPC_ValueDependentIsNull);
  std::vector<Expr *> Exprs;
  if (ExpIsNullPointer) {
    RecordDecl *LookupVtable = TD->getVtable();
    QualType VtableTy = Context.getRecordType(LookupVtable);
    if (LookupVtable->getDescribedClassTemplate()) {
      VtableTy = CompleteRecordType(
          LookupVtable, Context.getTrivialTypeSourceInfo(ElemType, SL));
      VtableTy = Context.getElaboratedType(ETK_Struct, nullptr, VtableTy);
    }
    QualType VtablePT = Context.getPointerType(VtableTy);
    ImplicitCastExpr *TraitVtable =
        ImplicitCastExpr::Create(Context, VtablePT, CK_BitCast, UO, nullptr,
                                 VK_PRValue, FPOptionsOverride());
    Exprs = {UO, TraitVtable};
  } else if (LookupVar) {
    LookupVar->setIsUsed();
    QualType VtableTy = LookupVar->getType();
    QualType VtablePT = Context.getPointerType(VtableTy);
    DeclRefExpr *VtableRef = DeclRefExpr::Create(
        Context, NestedNameSpecifierLoc(), SourceLocation(), LookupVar, false,
        UO->getBeginLoc(), VtableTy, VK_LValue);
    Expr *UOVtable = UnaryOperator::Create(
        Context, VtableRef, UO_AddrOf, VtablePT, VK_PRValue, OK_Ordinary,
        UO->getBeginLoc(), false, FPOptionsOverride());
    Exprs = {UO, UOVtable};
  } else {
    Diag(UO->getBeginLoc(), diag::err_type_has_not_impl_trait)
        << CompleteTraitType(ElemType)->getPointeeType() << T;
    return;
  }
  MutableArrayRef<Expr *> initExprs = MutableArrayRef<Expr *>(Exprs);
  ExprResult TraitInit =
      ActOnInitList(SourceLocation(), initExprs, SourceLocation());

  if (DIE && *DIE) {
    Designation Desig;
    for (unsigned i = 0; i < (*DIE)->size(); i++)
      Desig.AddDesignator(Designator::getField(
          (*DIE)->getDesignator(i)->getFieldName(), SL, SL));
    TraitInit = ActOnDesignatedInitializer(Desig, SL, false, TraitInit);
    *DIE = dyn_cast<DesignatedInitExpr>(TraitInit.getAs<Expr>());
  }
  IList->updateInit(Context, Index, TraitInit.getAs<Expr>());
}

TraitDecl *Sema::TryDesugarTrait(QualType T) {
  TraitDecl *TD = nullptr;
  if (T.isNull())
    return TD;

  QualType TraitTy = T;
  if (T->isPointerType()) {
    TraitTy = T->getPointeeType();
    while (TraitTy->isPointerType())
      TraitTy = TraitTy->getPointeeType();
    while (isa<TemplateSpecializationType>(TraitTy) ||
           isa<TypedefType>(TraitTy)) {
      if (const TemplateSpecializationType *TempTST =
              dyn_cast<TemplateSpecializationType>(TraitTy)) {
        if (TempTST->isTypeAlias())
          TraitTy = TempTST->getAliasedType();
        else
          break;
      } else if (const TypedefType *TT = dyn_cast<TypedefType>(TraitTy))
        TraitTy = TT->desugar();
    }
    // Remove ElaboratedType
    const TemplateSpecializationType *TST =
        dyn_cast<TemplateSpecializationType>(
            TraitTy->getLocallyUnqualifiedSingleStepDesugaredType());
    if (TST) {
      TemplateDecl *TempT = TST->getTemplateName().getAsTemplateDecl();
      TD = dyn_cast_or_null<TraitDecl>(TempT->getTemplatedDecl());
    } else {
      TD = dyn_cast_or_null<TraitDecl>(TraitTy->getAsTagDecl());
    }
  }
  if (!TD) {
    if (RecordDecl *RD = TraitTy->getAsRecordDecl()) {
      if (auto *TST = dyn_cast_or_null<TemplateSpecializationType>(TraitTy)) {
        TemplateDecl *TempT = TST->getTemplateName().getAsTemplateDecl();
        RD = dyn_cast_or_null<RecordDecl>(TempT->getTemplatedDecl());
      }
      if (RD)
        TD = RD->getDesugaredTraitDecl();
    }
  }
  return TD;
}

bool Sema::IsDesugaredFromTraitType(QualType T) {
  while (T->isPointerType())
    T = T->getPointeeType();
  if (RecordDecl *RD = T->getAsRecordDecl()) {
    if (auto *TST = dyn_cast_or_null<TemplateSpecializationType>(T)) {
      TemplateDecl *TempT = TST->getTemplateName().getAsTemplateDecl();
      RD = dyn_cast_or_null<RecordDecl>(TempT->getTemplatedDecl());
    }
    if (RD->getDesugaredTraitDecl())
      return true;
  }
  return false;
}

// Desugars complex type declarations in the following code example:
// @code
// trait F* f = &b;
// f = NULL;
// @endcode
ExprResult Sema::ActOnTraitReassignNull(Scope *S, SourceLocation TokLoc,
                                        BinaryOperatorKind Opc, Expr *LHSExpr,
                                        Expr *RHSExpr) {
  // @code
  // trait I **a;
  // a = NULL;
  // @endcode
  SmallVector<Expr *, 2> Exprs;
  RecordDecl *RD =
      dyn_cast<RecordType>(LHSExpr->getType().getCanonicalType())->getDecl();
  for (RecordDecl::field_iterator I = RD->field_begin(), E = RD->field_end();
       I != E; ++I) {
    Expr *NewLHSExpr = BuildMemberExpr(
        LHSExpr, false, SourceLocation(), NestedNameSpecifierLoc(),
        SourceLocation(), *I, DeclAccessPair::make(*I, I->getAccess()), false,
        DeclarationNameInfo(), I->getType(), VK_LValue, OK_Ordinary);
    Exprs.push_back(BuildBinOp(S, TokLoc, Opc, NewLHSExpr, RHSExpr).get());
  }
  // @code
  // f.data = NULL, f.vtable = NULL;
  // @endcode
  return BuildBinOp(S, TokLoc, BO_Comma, Exprs[0], Exprs[1]);
}

// Handling the cast operation for trait pointers, ensuring proper type casting:
// @code
// _Trait F* f = &b;
// void *p = (void *)f;
// @endcode
ExprResult Sema::ActOnTraitPointerCast(Expr *RHSExpr) {
  const auto *RT = dyn_cast<RecordType>(RHSExpr->getType().getCanonicalType());
  if (!RT) {
    // Reject _Trait F**
    return RHSExpr;
  }
  RecordDecl *RD = RT->getDecl();
  for (RecordDecl::field_iterator I = RD->field_begin(), E = RD->field_end();
       I != E; ++I) {
    if (I->getNameAsString() == "data") {
      Expr *TraitExpr = BuildMemberExpr(
          RHSExpr, false, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *I, DeclAccessPair::make(*I, I->getAccess()), false,
          DeclarationNameInfo(), I->getType(), VK_LValue, OK_Ordinary);
      return ImplicitCastExpr::Create(Context, I->getType(), CK_LValueToRValue,
                                      TraitExpr, nullptr, VK_PRValue,
                                      FPOptionsOverride());
    }
  }
  // Unreachable
  return ExprError();
}

// Handling reassignments of variable types with trait pointers:
// @code
// trait F* f = &b;
// f = &a;
// @endcode
ExprResult Sema::ActOnTraitReassign(Scope *S, SourceLocation TokLoc,
                                    BinaryOperatorKind Opc, Expr *LHSExpr,
                                    Expr *RHSExpr) {
  Expr *Bin1 = nullptr;
  Expr *Bin2 = nullptr;
  if (RHSExpr->isNullPointerConstant(Context, Expr::NPC_ValueDependentIsNull)) {
    return ActOnTraitReassignNull(S, TokLoc, Opc, LHSExpr, RHSExpr);
  }

  bool RHSIsPointerType = RHSExpr->getType()->isPointerType();
  if (!RHSIsPointerType) {
    Diag(RHSExpr->getBeginLoc(), diag::err_type_has_not_impl_trait)
        << CompleteTraitType(LHSExpr->getType())->getPointeeType()
        << RHSExpr->getType();
    return ExprError();
  }

  QualType LTy = LHSExpr->getType();
  Expr *RE = RHSExpr->IgnoreCasts();
  QualType RTy = RE->getType();
  while (LTy->isPointerType()) {
    LTy = LTy->getPointeeType();
    if (RTy->isPointerType())
      RTy = RTy->getPointeeType();
  }

  if (!RTy->isPointerType()) {
    TraitDecl *TD = TryDesugarTrait(RTy);
    if (!TD) {
      Diag(RHSExpr->getBeginLoc(), diag::err_type_has_not_impl_trait)
          << CompleteTraitType(LHSExpr->getType())->getPointeeType()
          << RE->getType();
      return ExprError();
    }
    return BuildBinOp(S, TokLoc, Opc, LHSExpr, RE);
  }

  RecordDecl *RD = dyn_cast<RecordType>(LTy.getCanonicalType())->getDecl();
  QualType T =
      RE->getType()->getPointeeType().getUnqualifiedType().getCanonicalType();
  T.removeLocalOwned();
  for (RecordDecl::field_iterator I = RD->field_begin(), E = RD->field_end();
       I != E; ++I) {
    Expr *NewLHSExpr = BuildMemberExpr(
        LHSExpr, false, SourceLocation(), NestedNameSpecifierLoc(),
        SourceLocation(), *I, DeclAccessPair::make(*I, I->getAccess()), false,
        DeclarationNameInfo(), I->getType(), VK_LValue, OK_Ordinary);
    Expr *NewRHSExpr = nullptr;
    if (I->getNameAsString() == "data") {
      NewRHSExpr =
          ImplicitCastExpr::Create(Context, I->getType(), CK_BitCast, RE,
                                   nullptr, VK_PRValue, FPOptionsOverride());
    } else {
      TraitDecl *TD = RD->getDesugaredTraitDecl();
      RecordDecl *LookupVtable = TD->getVtable();
      VarDecl *LookupVar = TD->getTypeImpledVarDecl(T);
      if (!LookupVar) {
        Diag(RE->getBeginLoc(), diag::err_type_has_not_impl_trait)
            << CompleteTraitType(LHSExpr->getType())->getPointeeType() << T;
        return ExprError();
      }
      QualType VtableTy = Context.getRecordType(LookupVtable);
      if (LookupVtable->getDescribedClassTemplate())
        VtableTy =
            CompleteRecordType(LookupVtable, LookupVar->getTypeSourceInfo());
      VtableTy = Context.getElaboratedType(ETK_Struct, nullptr, VtableTy);
      QualType VtablePT = Context.getPointerType(VtableTy);
      DeclRefExpr *VtableRef = BuildDeclRefExpr(LookupVar, VtableTy, VK_LValue,
                                                LookupVar->getLocation());
      NewRHSExpr = UnaryOperator::Create(
          Context, VtableRef, UO_AddrOf, VtablePT, VK_PRValue, OK_Ordinary,
          SourceLocation(), false, FPOptionsOverride());
    }
    if (Bin1 == nullptr)
      Bin1 = BuildBinOp(S, TokLoc, Opc, NewLHSExpr, NewRHSExpr).get();
    else
      Bin2 = BuildBinOp(S, TokLoc, Opc, NewLHSExpr, NewRHSExpr).get();
  }
  // @code
  // f.data = &a, f.vtable = &__int_trait_T;
  // @endcode
  return BuildBinOp(S, TokLoc, BO_Comma, Bin1, Bin2);
}

bool Sema::IsTraitExpr(Expr *Expr) {
  QualType T = Expr->getType().getCanonicalType();
  while (T->isPointerType())
    T = T->getPointeeType();
  if (auto RT = dyn_cast<RecordType>(T)) {
    RecordDecl *RD = dyn_cast<RecordDecl>(RT->getDecl());
    return (RD && RD->getDesugaredTraitDecl());
  }
  return false;
}

ExprResult Sema::ActOnTraitCompare(Scope *S, SourceLocation TokLoc,
                                   BinaryOperatorKind Opc, Expr *LHSExpr,
                                   Expr *RHSExpr) {
  // @code
  // trait F* f = &a;
  // trait F* e = &a;
  // if (f == e) {};
  // @endcode
  bool LHSIsTrait = IsTraitExpr(LHSExpr);
  bool RHSIsTrait = IsTraitExpr(RHSExpr);
  if (LHSIsTrait && RHSIsTrait) {
    QualType LHSOriginalType = CompleteTraitType(LHSExpr->getType());
    QualType RHSOriginalType = CompleteTraitType(RHSExpr->getType());
    if (Context.hasSameType(LHSOriginalType, RHSOriginalType) == false) {
      Diag(TokLoc, diag::ext_typecheck_comparison_of_distinct_pointers)
          << LHSOriginalType << RHSOriginalType << LHSExpr->getSourceRange()
          << RHSExpr->getSourceRange();
    }
    if (LHSExpr->getType()->isPointerType() ||
        RHSExpr->getType()->isPointerType()) {
      // Check pointer levels
      int LHSPointerLevels = CountPointerLevels(LHSOriginalType);
      int RHSPointerLevels = CountPointerLevels(RHSOriginalType);
      if (LHSPointerLevels != RHSPointerLevels) {
        Diag(TokLoc, diag::err_typecheck_comparison_of_distinct_pointers)
            << LHSOriginalType << RHSOriginalType << LHSExpr->getSourceRange()
            << RHSExpr->getSourceRange();
        return ExprError();
      }
      return BuildBinOp(S, TokLoc, Opc, LHSExpr, RHSExpr);
    }
    SmallVector<Expr *, 2> NewLHSExprs;
    RecordDecl *LRD =
        dyn_cast<RecordType>(LHSExpr->getType().getCanonicalType())->getDecl();
    for (RecordDecl::field_iterator I = LRD->field_begin(),
                                    E = LRD->field_end();
         I != E; ++I) {
      Expr *NewLHSExpr = BuildMemberExpr(
          LHSExpr, false, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *I, DeclAccessPair::make(*I, I->getAccess()), false,
          DeclarationNameInfo(), I->getType(), VK_LValue, OK_Ordinary);
      NewLHSExprs.push_back(NewLHSExpr);
    }
    SmallVector<Expr *, 2> NewRHSExprs;
    RecordDecl *RRD =
        dyn_cast<RecordType>(RHSExpr->getType().getCanonicalType())->getDecl();
    for (RecordDecl::field_iterator J = RRD->field_begin(),
                                    F = RRD->field_end();
         J != F; ++J) {
      Expr *NewRHSExpr = BuildMemberExpr(
          RHSExpr, false, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *J, DeclAccessPair::make(*J, J->getAccess()), false,
          DeclarationNameInfo(), J->getType(), VK_LValue, OK_Ordinary);
      NewRHSExprs.push_back(NewRHSExpr);
    }
    Expr *Bin1 =
        BuildBinOp(S, TokLoc, Opc, NewLHSExprs[0], NewRHSExprs[0]).get();
    Expr *Bin2 =
        BuildBinOp(S, TokLoc, Opc, NewLHSExprs[1], NewRHSExprs[1]).get();

    if (Opc == BO_EQ) {
      // @code
      // f.data == e.data && f.vtable == e.vtable;
      // @endcode
      return BuildBinOp(S, TokLoc, BO_LAnd, Bin1, Bin2);
    } else if (Opc == BO_NE) {
      // @code
      // f.data != e.data || f.vtable != e.vtable;
      // @endcode
      return BuildBinOp(S, TokLoc, BO_LOr, Bin1, Bin2);
    } else {
      assert(false);
      return ExprError();
    }
  }

  RecordDecl *RD;
  Expr *BaseTrait;
  Expr *BaseExpr;
  if (LHSIsTrait) {
    RD = dyn_cast<RecordType>(LHSExpr->getType().getCanonicalType())->getDecl();
    BaseTrait = LHSExpr;
    BaseExpr = RHSExpr;
  } else if (RHSIsTrait) {
    RD = dyn_cast<RecordType>(RHSExpr->getType().getCanonicalType())->getDecl();
    BaseTrait = RHSExpr;
    BaseExpr = LHSExpr;
  } else {
    assert(false);
    return ExprError();
  }
  bool IsPointerType = BaseExpr->getType()->isPointerType();
  if (!IsPointerType) {
    Diag(TokLoc, diag::err_type_has_not_impl_trait)
        << CompleteTraitType(BaseTrait->getType())->getPointeeType()
        << BaseExpr->getType();
    return ExprError();
  }
  QualType T = BaseExpr->getType()->getPointeeType().getCanonicalType();
  TraitDecl *TD = RD->getDesugaredTraitDecl();
  VarDecl *LookupVar = TD->getTypeImpledVarDecl(T);
  if (!LookupVar && !(T->isVoidType())) {
    // If expr not ImplTraitDecl, Diagnose warning;
    Diag(TokLoc, diag::warn_type_has_not_impl_trait)
        << CompleteTraitType(BaseTrait->getType())->getPointeeType()
        << BaseExpr->getType();
  }
  Expr *TraitExpr = nullptr;
  for (RecordDecl::field_iterator I = RD->field_begin(), E = RD->field_end();
       I != E; ++I) {
    if (I->getNameAsString() == "data") {
      TraitExpr = BuildMemberExpr(
          BaseTrait, false, SourceLocation(), NestedNameSpecifierLoc(),
          SourceLocation(), *I, DeclAccessPair::make(*I, I->getAccess()), false,
          DeclarationNameInfo(), I->getType(), VK_LValue, OK_Ordinary);
      break;
    }
  }
  if (LHSIsTrait) {
    // @code
    // f.data == &a;
    // @endcode
    return BuildBinOp(S, TokLoc, Opc, TraitExpr, BaseExpr);
  } else {
    // @code
    // &a == f.data;
    // @endcode
    return BuildBinOp(S, TokLoc, Opc, BaseExpr, TraitExpr);
  }
}

// a BSC function's return type can not be trait
// if D is a FunctionDecl
//    e.g. trait F foo();         trait F<T> foo();
//         trait F<T> foo<T>();   trait F<int> foo();
// if D is a VarDecl of FunctionPointer
//    e.g. trait F (call*)();     trait F<T> (call*)();
//         trait F<T> (call*)();  trait F<int> (call*)();
// a BSC function's param type can not be trait too.
void Sema::checkBSCFunctionContainsTrait(Decl *D) {
  const Type *T = nullptr;
  if (auto *FD = dyn_cast_or_null<FunctionDecl>(D))
    T = FD->getReturnType().getCanonicalType().getTypePtr();
  else if (auto *PD = dyn_cast_or_null<ParmVarDecl>(D))
    T = PD->getType().getCanonicalType().getTypePtr();
  else if (auto *VD = dyn_cast_or_null<VarDecl>(D)) {
    QualType Ty = VD->getType();
    if (Ty->isFunctionPointerType()) {
      const Type *FT = Ty->getAs<PointerType>()
                            ->getPointeeType()
                            .getCanonicalType()
                            .getTypePtr();
      if (const FunctionProtoType *FPT = 
                FT->getAs<FunctionProtoType>()) {
        T = FPT->getReturnType().getTypePtr();
      } else if (const FunctionNoProtoType *FNPT = 
                       FT->getAs<FunctionNoProtoType>()) {
        T = FNPT->getReturnType().getTypePtr();
      } else 
        return;
    } else
      return;
  } else
    return;

  if (T->isTraitType()) {
    Diag(D->getBeginLoc(), diag::err_variables_not_trait_pointer);
    D->setInvalidDecl();
  } else if (auto *TST = dyn_cast_or_null<TemplateSpecializationType>(T)) {
    if (TemplateDecl *TempT = TST->getTemplateName().getAsTemplateDecl()) {
      if (dyn_cast_or_null<TraitDecl>(TempT->getTemplatedDecl())) {
        Diag(D->getBeginLoc(), diag::err_variables_not_trait_pointer);
        D->setInvalidDecl();
      }
    }
  }
}
#endif // ENABLE_BSC