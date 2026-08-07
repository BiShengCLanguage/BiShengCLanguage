#if ENABLE_BSC

#include <cstring>

#include "TreeTransform.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Sema/DeclSpec.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Scope.h"
#include "clang/Sema/Template.h"
#include "llvm/ADT/SmallString.h"

using namespace clang;
using namespace sema;

class CheckExprMemoryLeak : public RecursiveASTVisitor<CheckExprMemoryLeak> {
  Sema &SemaRef;

public:
  explicit CheckExprMemoryLeak(Sema &SemaRef) : SemaRef(SemaRef) {}

  bool VisitMemberExpr(const MemberExpr *ME) {
    if (auto DRE = dyn_cast_or_null<DeclRefExpr>(ME->getBase())) {
      bool isOwned = DRE->getType().getCanonicalType()->isOwnedStructureType();
      if (isOwned) {
        SemaRef.Diag(DRE->getBeginLoc(), diag::err_owned_member_access_in_return)
            << DRE->getNameInfo().getAsString();
        return false;
      }
    }
    return true;
  }
};

void Sema::CheckReturnStmtMemoryLeak(Expr *E) {
  CheckExprMemoryLeak CheckExprMemoryLeakObj(*this);
  CheckExprMemoryLeakObj.TraverseStmt(E);
}

/// ActOnCXXMemberDeclarator - This is invoked when a C++ class member
/// declarator is parsed. 'AS' is the access specifier, 'BW' specifies the
/// bitfield width if there is one, 'InitExpr' specifies the initializer if
/// one has been parsed.
NamedDecl *
Sema::ActOnBSCMemberDeclarator(Scope *S, AccessSpecifier AS, Declarator &D,
                               MultiTemplateParamsArg TemplateParameterLists,
                               Expr *BW) {
  const DeclSpec &DS = D.getDeclSpec();
  DeclarationNameInfo NameInfo = GetNameForDeclarator(D);
  DeclarationName Name = NameInfo.getName();
  SourceLocation Loc = NameInfo.getLoc();
  // For anonymous bitfields, the location should point to the type.
  if (Loc.isInvalid())
    Loc = D.getBeginLoc();

  Expr *BitWidth = static_cast<Expr *>(BW);

  assert(isa<RecordDecl>(CurContext));
  assert(!DS.isFriendSpecified());

  bool isFunc = D.isDeclarationOfFunction();

  bool isInstField = ((DS.getStorageClassSpec() == DeclSpec::SCS_unspecified ||
                       DS.getStorageClassSpec() == DeclSpec::SCS_mutable) &&
                      !isFunc);
  if (DS.hasConstexprSpecifier() && isInstField) {
    SemaDiagnosticBuilder B =
        Diag(DS.getConstexprSpecLoc(), diag::err_invalid_constexpr_member);
    SourceLocation ConstexprLoc = DS.getConstexprSpecLoc();
    B << 0 << 0;
    if (D.getDeclSpec().getTypeQualifiers() & DeclSpec::TQ_const)
      B << FixItHint::CreateRemoval(ConstexprLoc);
    else {
      B << FixItHint::CreateReplacement(ConstexprLoc, "const");
      D.getMutableDeclSpec().ClearConstexprSpec();
      const char *PrevSpec;
      unsigned DiagID;
      bool Failed = D.getMutableDeclSpec().SetTypeQual(
          DeclSpec::TQ_const, ConstexprLoc, PrevSpec, DiagID, getLangOpts());
      (void)Failed;
      assert(!Failed && "Making a constexpr member const shouldn't fail");
    }
  }

  NamedDecl *Member;
  if (isInstField) {
    CXXScopeSpec &SS = D.getCXXScopeSpec();

    // Data members must have identifiers for names.
    if (!Name.isIdentifier()) {
      Diag(Loc, diag::err_bad_variable_name) << Name;
      return nullptr;
    }

    IdentifierInfo *II = Name.getAsIdentifierInfo();

    // Member field could not be with "template" keyword.
    // So TemplateParameterLists should be empty in this case.
    if (TemplateParameterLists.size()) {
      TemplateParameterList *TemplateParams = TemplateParameterLists[0];
      if (TemplateParams->size()) {
        // There is no such thing as a member field template.
        Diag(D.getIdentifierLoc(), diag::err_template_member)
            << II
            << SourceRange(TemplateParams->getTemplateLoc(),
                           TemplateParams->getRAngleLoc());
      } else {
        // There is an extraneous 'template<>' for this member.
        Diag(TemplateParams->getTemplateLoc(),
             diag::err_template_member_noparams)
            << II
            << SourceRange(TemplateParams->getTemplateLoc(),
                           TemplateParams->getRAngleLoc());
      }
      return nullptr;
    }

    if (D.getName().getKind() == UnqualifiedIdKind::IK_TemplateId) {
      Diag(D.getIdentifierLoc(), diag::err_member_with_template_arguments)
          << II
          << SourceRange(D.getName().TemplateId->LAngleLoc,
                         D.getName().TemplateId->RAngleLoc)
          << D.getName().TemplateId->LAngleLoc;
      D.SetIdentifier(II, Loc);
    }

    if (SS.isSet() && !SS.isInvalid()) {
      // The user provided a superfluous scope specifier inside a class
      // definition:
      //
      // class X {
      //   int X::member;
      // };
      if (DeclContext *DC = computeDeclContext(SS, false))
        diagnoseQualifiedDeclaration(SS, DC, Name, D.getIdentifierLoc(),
                                     D.getName().getKind() ==
                                         UnqualifiedIdKind::IK_TemplateId);
      else
        Diag(D.getIdentifierLoc(), diag::err_member_qualification)
            << Name << SS.getRange();

      SS.clear();
    }

    Member = HandleField(S, cast<RecordDecl>(CurContext), Loc, D, BitWidth,
                         ICIS_NoInit, AS);
    if (!Member)
      return nullptr;
  } else {
    Member = HandleDeclarator(S, D, TemplateParameterLists);
    if (!Member)
      return nullptr;

    // Non-instance-fields can't have a bitfield.
    if (BitWidth) {
      if (Member->isInvalidDecl()) {
        // don't emit another diagnostic.
      } else if (isa<VarDecl>(Member) || isa<VarTemplateDecl>(Member)) {
        // C++ 9.6p3: A bit-field shall not be a static member.
        // "static member 'A' cannot be a bit-field"
        Diag(Loc, diag::err_static_not_bitfield)
            << Name << BitWidth->getSourceRange();
      } else if (isa<TypedefDecl>(Member)) {
        // "typedef member 'x' cannot be a bit-field"
        Diag(Loc, diag::err_typedef_not_bitfield)
            << Name << BitWidth->getSourceRange();
      } else {
        // A function typedef ("typedef int f(); f a;").
        // C++ 9.6p3: A bit-field shall have integral or enumeration type.
        Diag(Loc, diag::err_not_integral_type_bitfield)
            << Name << cast<ValueDecl>(Member)->getType()
            << BitWidth->getSourceRange();
      }

      BitWidth = nullptr;
      Member->setInvalidDecl();
    }

    NamedDecl *NonTemplateMember = Member;
    if (FunctionTemplateDecl *FunTmpl = dyn_cast<FunctionTemplateDecl>(Member))
      NonTemplateMember = FunTmpl->getTemplatedDecl();
    else if (VarTemplateDecl *VarTmpl = dyn_cast<VarTemplateDecl>(Member))
      NonTemplateMember = VarTmpl->getTemplatedDecl();

    Member->setAccess(AS);

    // If we have declared a member function template or static data member
    // template, set the access of the templated declaration as well.
    if (NonTemplateMember != Member)
      NonTemplateMember->setAccess(AS);
  }

  CheckOverrideControl(Member);

  assert((Name || isInstField) && "No identifier for non-field ?");

  if (isInstField) {
    FieldDecl *FD = cast<FieldDecl>(Member);

    if (!Diags.isIgnored(diag::warn_unused_private_field, FD->getLocation())) {
      // Remember all explicit private FieldDecls that have a name, no side
      // effects and are not part of a dependent type declaration.
      if (!FD->isImplicit() && FD->getDeclName() &&
          FD->getAccess() == AS_private && !FD->hasAttr<UnusedAttr>() &&
          !FD->getParent()->isDependentContext())
        UnusedPrivateFields.insert(FD);
    }
  }

  return Member;
}

void Sema::ActOnStartBSCMemberDeclarations(Scope *S, Decl *TagD,
                                           SourceLocation LBraceLoc) {
  AdjustDeclIfTemplate(TagD);
  RecordDecl *Record = cast<RecordDecl>(TagD);

  if (!Record->getIdentifier())
    return;

  // C++ [class]p2:
  //   [...] The class-name is also inserted into the scope of the
  //   class itself; this is known as the injected-class-name. For
  //   purposes of access checking, the injected-class-name is treated
  //   as if it were a public member name.
  RecordDecl *InjectedClassName = RecordDecl::Create(
      Context, Record->getTagKind(), CurContext, Record->getBeginLoc(),
      Record->getLocation(), Record->getIdentifier(),
      /*PrevDecl=*/nullptr,
      /*DelayTypeCreation=*/true);
  Context.getTypeDeclType(InjectedClassName, Record);
  InjectedClassName->setImplicit();
  InjectedClassName->setAccess(AS_public);
  InjectedClassName->setOwnedDecl(Record->isOwnedDecl());
  if (ClassTemplateDecl *Template = Record->getDescribedClassTemplate())
    InjectedClassName->setDescribedClassTemplate(Template);
  PushOnScopeChains(InjectedClassName, S);
  assert(InjectedClassName->isInjectedClassName() &&
         "Broken injected-class-name");
}

void Sema::ActOnFinishBSCMemberSpecification(
    Scope *S, SourceLocation RLoc, Decl *TagDecl, SourceLocation LBrac,
    SourceLocation RBrac, const ParsedAttributesView &AttrList) {
  if (!TagDecl)
    return;

  AdjustDeclIfTemplate(TagDecl);

  for (const ParsedAttr &AL : AttrList) {
    if (AL.getKind() != ParsedAttr::AT_Visibility)
      continue;
    AL.setInvalid();
    Diag(AL.getLoc(), diag::warn_attribute_after_definition_ignored) << AL;
  }

  SmallVector<Decl *, 32> FieldDecls(cast<RecordDecl>(TagDecl)->fields());

  ActOnFields(S, RLoc, TagDecl, FieldDecls, LBrac, RBrac, AttrList);

  CheckCompletedBSCClass(S, cast<RecordDecl>(TagDecl));
}

void Sema::CheckCompletedBSCClass(Scope *S, RecordDecl *Record) {
  if (!Record)
    return;

  if (Record->getIdentifier()) {
    // C++ [class.mem]p13:
    //   If T is the name of a class, then each of the following shall have a
    //   name different from T:
    //     - every member of every anonymous union that is a member of class T.
    //
    // C++ [class.mem]p14:
    //   In addition, if class T has a user-declared constructor (12.1), every
    //   non-static data member of class T shall have a name different from T.
    DeclContext::lookup_result R = Record->lookup(Record->getDeclName());
    for (DeclContext::lookup_iterator I = R.begin(), E = R.end(); I != E; ++I) {
      NamedDecl *D = (*I)->getUnderlyingDecl();
      if ((isa<FieldDecl>(D) || isa<UnresolvedUsingValueDecl>(D)) ||
          isa<IndirectFieldDecl>(D)) {
        Diag((*I)->getLocation(), diag::err_member_name_of_class)
            << D->getDeclName();
        break;
      }
    }
  }

  llvm::SmallVector<FunctionDecl *, 5> DefaultedSecondaryComparisons;

  // Check the defaulted secondary comparisons after any other member functions.
  for (FunctionDecl *FD : DefaultedSecondaryComparisons) {
    CheckExplicitlyDefaultedFunction(S, FD);
  }
}

bool Sema::IsOwnedStruct(std::string StructName, Scope *Scope) {
  auto Name = DeclarationName(&(Context.Idents).get(StructName));
  LookupResult R(*this, Name, SourceLocation(), LookupTagName);
  LookupName(R, Scope, false);
  R.suppressDiagnostics();
  if (R.getResultKind() == LookupResult::Found)
    if (const TagDecl *TD = R.getAsSingle<TagDecl>()) {
      switch (TD->getTagKind()) {
      case TTK_Struct:
        return cast<RecordDecl>(R.getFoundDecl())->isOwnedDecl();
      default:
        break;
      }
    }
  return false;
}
#endif // ENABLE_BSC