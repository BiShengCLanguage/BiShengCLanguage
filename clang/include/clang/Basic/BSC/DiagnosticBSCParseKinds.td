// BSC experimental features
def err_bsc_exp_parse : Error<"%0 is an experimental BSC feature; enable with -fbsc-experimental">;

// BSC trait warnings and errors.
def invalid_param_for_trait_member : Error<"the first parameter of _Trait member function must be 'This* this'">;
def err_unexpected_token_for_impl_trait_decl : Error<"unexpected token for ImplTraitDecl">;
def err_trait_def_not_at_top_level : Error<"_Trait is only allowed to be defined at top-level">;
def err_invalid_trait : Error<"invalid syntax, semi not expected after '_Trait %0'">;
def err_multi_trait_call_func : Error<"%0 cannot directly call functions">;

// BSC _Owned struct
def err_tag_name : Error<
  "'_Owned struct' only appears in the type's declaration or definition; use %0 without '_Owned struct'">;
def err_inconsistent_tag_name : Error<
  "declaration of %0 as %select{'struct'|'_Owned struct'}1 conflicts with previous declaration as %select{'_Owned struct'|'struct'}1">;
def err_owned_struct_in_function_scope : Error<"_Owned struct cannot be defined in function scope; move the definition to file scope">;

// BSC generic warnings and errors.
def err_extended_type_not_generic_typealias : Error<"extended type of a BSC member function cannot be a generic typealias">;
def err_generic_typealias_not_within_function : Error<"BSC generic typealias cannot be declared within a function">;

// BSC operator overload errors.
def err_unsupport_overload_type : Error<"unsupported operator type">;

// BSC scope errors.
def err_unsupport_global_scope : Error<"unsupported global scope specifier '::'">;