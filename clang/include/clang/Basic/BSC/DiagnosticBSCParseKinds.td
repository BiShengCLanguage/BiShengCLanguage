// BSC experimental features
def err_bsc_exp_parse : Error<"%0 is an experimental BSC feature; enable with -fbsc-experimental">,
  BSCOutOfScope;

// BSC trait warnings and errors.
def invalid_param_for_trait_member : Error<"the first parameter of _Trait member function must be 'This* this'">,
  BSCOutOfScope;
def err_unexpected_token_for_impl_trait_decl : Error<"unexpected token for ImplTraitDecl">,
  BSCOutOfScope;
def err_trait_def_not_at_top_level : Error<"_Trait is only allowed to be defined at top-level">,
  BSCOutOfScope;
def err_invalid_trait : Error<"invalid syntax, semi not expected after '_Trait %0'">,
  BSCOutOfScope;
def err_multi_trait_call_func : Error<"%0 cannot directly call functions">,
  BSCOutOfScope;

// BSC _Owned struct
def err_tag_name : Error<
  "'_Owned struct' only appears in the type's declaration or definition; use %0 without '_Owned struct'">,
  BSCDiag<"OWN", 45>;
def err_inconsistent_tag_name : Error<
  "declaration of %0 as %select{'struct'|'_Owned struct'}1 conflicts with previous declaration as %select{'_Owned struct'|'struct'}1">,
  BSCDiag<"OWN", 46,
          "`note_previous_declaration` — previous declaration is here">;
def err_owned_struct_in_function_scope : Error<"_Owned struct cannot be defined in function scope; move the definition to file scope">,
  BSCDiag<"OWN", 42>;

// BSC qualifiers.
def err_bsc_qualifier_position : Error<
  "'%0' qualifier is not allowed %select{in an array declarator|"
  "on a block pointer|after a function declarator}1">,
  BSCDiag<"MISC", 9,
          "`%0` is a BSC pointer qualifier (`_Owned` / `_Borrow` / `_ArrayElem`) written where only C qualifiers are allowed: inside an array declarator, on a block pointer (`^`), or after a function declarator">;

// BSC generic warnings and errors.
def err_extended_type_not_generic_typealias : Error<"extended type of a BSC member function cannot be a generic typealias">,
  BSCOutOfScope;
def err_generic_typealias_not_within_function : Error<"BSC generic typealias cannot be declared within a function">,
  BSCOutOfScope;

// BSC operator overload errors.
def err_unsupport_overload_type : Error<"unsupported operator type">,
  BSCOutOfScope;

// BSC scope errors.
def err_unsupport_global_scope : Error<"unsupported global scope specifier '::'">,
  BSCOutOfScope;