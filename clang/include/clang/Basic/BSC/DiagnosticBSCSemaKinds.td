// BSC experimental features
def err_bsc_exp_sema : Error<"%0 is an experimental BSC feature; enable with -fbsc-experimental">,
  BSCOutOfScope;

// BSC instance member warnings and errors.
def note_no_this_parameter : Note<"found the following member function %0 which doesn't have a 'this' parameter">;
def err_no_instance_member : Error<
  "no instance member named %0 in %1; did you mean to use '::' instead of '%select{.|->}2'?">,
  BSCOutOfScope;
def err_cvrqualified_member_type_unsupported : Error<
  "type of member function is wrong, CVR qualified type is unsupported, but found '%0'">,
  BSCOutOfScope;
def err_this_type_unsupported : Error<
  "type of parameter 'this' is wrong, expected 'This*' or 'This' or '%0*' or '%0', but found '%1'">,
  BSCOutOfScope;
def err_incompatible_pointer_cast : Error<
  "incompatible conversion from pointer type '%0' to non-pointer type '%1' in member function call">,
  BSCOutOfScope;
def err_incompatible_const_cast : Error<
  "incompatible conversion from const type '%0' to non-const type '%1' in member function call">,
  BSCOutOfScope;
def err_incompatible_owned_cast : Error<
  "incompatible conversion from non-_Owned type '%0' to _Owned type '%1' in member function call">,
  BSCDiag<"OWN", 38>;

// BSC _Async/_Await warnings and errors.
def err_async_non_function : Error<
  "'_Async' can only appear on functions">,
  BSCOutOfScope;
def err_async_main : Error<"'main' is not allowed to be declared _Async">,
  BSCOutOfScope;
def err_await_invalid_scope : Error<
  "_Await expression is not allowed to appear in %0">,
  BSCOutOfScope;
def err_async_func_unsupported : Error<
  "_Async function does not support %0">,
  BSCOutOfScope;
def err_not_a_async_call : Error<
  "the _Await expression's external function declaration must be modified by _Async or return 'Future' type">,
  BSCOutOfScope;
def err_function_not_found : Error <
  "'%0' was not found; include %1 before using the '_Async' keyword">,
  BSCOutOfScope;
def err_invalid_async_function : Error <
  "function which returns 'Future' type should not be modified by '_Async'">,
  BSCOutOfScope;

// BSC ownership warnings and errors.
def err_nested_owned_borrow_type_check : Error<
  "%select{"
  "type of %2 cannot be qualified by '%1'|"
  "type of %2 cannot be qualified by '%1' because %3 contains a '%1' type|"
  "type of %2 cannot be qualified by '%1' (even indirectly) because %3 contains a '%1' type}0">,
  BSCDiag<"OWN", 19>;
def err_owned_funcPtr_incompatible : Error<"cannot cast %0 to %1">,
  BSCOutOfScope;
def err_owned_qualcheck_incompatible : Error<
  "incompatible _Owned types, cannot cast %0 to %1">,
  BSCDiag<"OWN", 20>;
def err_bsc_move_to_raw_not_owned : Error<
  "argument must be an _Owned pointer type (have %0)">,
  BSCDiag<"OWN", 29,
          "`note_bsc_move_to_raw_use_move_array_to_raw` — use `__move_array_to_raw` for an _Owned _ArrayElem pointer">;
def err_bsc_take_from_raw_not_raw : Error<
  "argument must be a raw pointer type (have %0)">,
  BSCDiag<"OWN", 30>;
def err_bsc_take_from_raw_function_pointer : Error<
  "__take_from_raw does not support function pointer type %0">,
  BSCDiag<"OWN", 31>;
def err_bsc_move_array_to_raw_not_owned_array : Error<
  "argument must be an _Owned _ArrayElem pointer type (have %0)">,
  BSCDiag<"OWN", 32,
          "`note_bsc_move_array_to_raw_use_move_to_raw` — use `__move_to_raw` for an _Owned pointer that is not _ArrayElem">;
def note_bsc_move_to_raw_use_move_array_to_raw : Note<
  "use '__move_array_to_raw' for an _Owned _ArrayElem pointer">;
def note_bsc_move_array_to_raw_use_move_to_raw : Note<
  "use '__move_to_raw' for an _Owned pointer that is not _ArrayElem">;
def err_bsc_take_array_from_raw_not_raw : Error<
  "argument must be a raw pointer type (have %0)">,
  BSCDiag<"OWN", 33>;
def err_bsc_take_array_from_raw_function_pointer : Error<
  "__take_array_from_raw does not support function pointer type %0">,
  BSCDiag<"OWN", 34>;
def err_owned_raw_cast_disallowed : Error<
  "cannot cast between _Owned and raw pointer; use __move_to_raw or __take_from_raw for ownership transfer">,
  BSCDiag<"OWN", 27>;
def err_owned_array_raw_cast_disallowed : Error<
  "cannot cast between _Owned _ArrayElem and raw pointer; use __move_array_to_raw or __take_array_from_raw for ownership transfer">,
  BSCDiag<"OWN", 28>;
def err_bsc_op_not_supported : Error<"%0 is not supported on %1">,
  BSCDiag<"OWN", 25,
          "Combined diag: %0 is the operation — pointer arithmetic (`'+'`, `'-'`, `'+='`, `'-='`, `'++'`, `'--'`) on an _Owned or non-_ArrayElem _Borrow pointer operand (regardless of safe zone; only `_Borrow _ArrayElem` pointers do arithmetic), any other binary operator (`'*'`, `'&'`, …) on an _Owned pointer operand, or `array subscript` on an _Owned pointer without `_ArrayElem` (a `_Borrow` one gets `err_typecheck_borrow_subscript`). Arithmetic notes: `note_bsc_ptr_declared_here` — pointer `%0` declared here; `note_bsc_ptr_arith_fix_named` — declare `%1` as a raw pointer for pointer arithmetic (prefixed with \"use `_Borrow _ArrayElem` or \" when the _Borrow was initialized from `&arr[i]`); `note_bsc_ptr_arith_fix_anon` — use a raw pointer for pointer arithmetic (when the operand is not a `DeclRefExpr`)",
          "(`err_bsc_ptr_inc_dec` also fires for `++`/`--` on non-_ArrayElem _Borrow pointers; filed under **OWN** as `OWN-025`. _Borrow pointers qualified with `_ArrayElem` are explicitly allowed.)">;
def err_owned_temporary_memLeak : Error<
  "the _Owned value of '%0' is discarded and would leak">,
  BSCDiag<"OWN", 21,
          "%0 is the pretty-printed expression (e.g. `mk()`)">;
def err_owned_member_access_in_return : Error<
  "cannot use a member of '%0' in a return expression because '%0' is destroyed before the return; copy the member to a local first">,
  BSCDiag<"OWN", 47,
          "Split out of OWN-021: destructors are injected before the return statement, so the member read would touch a destroyed value">;
def err_funcPtr_incompatible : Error<
  "incompatible _Borrow function pointer types, cannot cast %0 to %1">,
  BSCOutOfScope;
def err_mut_expr_unmodifiable  : Error<"the expression after '&_Mut' must be modifiable">,
  BSCDiag<"BOR", 15>;
def err_mut_or_const_expr_func : Error<"%0 cannot be applied to a %select{function|function pointer}1">,
  BSCDiag<"BOR", 16>;
def err_safe_mut : Error<"global or static variables are not allowed to be mutably borrowed within the safe zone">,
  BSCDiag<"BOR", 17>;
def err_move_borrow : Error<"cannot take ownership of a borrowed value">,
  BSCDiag<"BOR", 14,
          "`note_array_param_adjusted_borrow_arrayelem` — array parameter `%0` is adjusted to a `_Borrow _ArrayElem` pointer; its elements are borrowed and cannot be moved out (fires when the offending value is rooted at an owned-element array formal)">;
def note_array_param_adjusted_borrow_arrayelem : Note<
  "array parameter '%0' is adjusted to a '_Borrow _ArrayElem' pointer; its elements are borrowed and cannot be moved out">;
def err_typecheck_invalid_borrow_not_pointer : Error<"'_Borrow' can only be applied to a pointer type; %0 is not a pointer">,
  BSCDiag<"BOR", 11>;
def err_borrow_qualcheck_incompatible : Error<
  "incompatible _Borrow types, cannot cast %0 to %1">,
  BSCDiag<"BOR", 9>;
def err_borrow_qualcheck_compare : Error<
  "incompatible _Borrow types, %0 and %1 cannot be compared">,
  BSCDiag<"BOR", 10>;
def err_typecheck_borrow_func : Error<"return type cannot be _Borrow-qualified because no function parameter is _Borrow-qualified">,
  BSCDiag<"BOR", 12>;
def err_typecheck_multi_level_borrow_func : Error<"function return type cannot contain multiple borrow regions">,
  BSCOutOfScope;
def err_typecheck_borrow_subscript : Error<"subscript of _Borrow pointer is not allowed">,
  BSCDiag<"BOR", 13>;
def err_owned_and_borrow_conflict : Error<"cannot combine '_Owned' and '_Borrow' qualifiers on the same type">,
  BSCDiag<"OWN", 23>;
def err_bsc_qualifier_in_knr_function : Error<
  "type with '%0' semantics is not allowed in a K&R-style function definition">,
  BSCDiag<"OWN", 37>;
def err_owned_qualifier_non_pointer : Error<
  "type of %1 cannot be qualified by '%0'">,
  BSCDiag<"OWN", 22>;
def err_arrayelem_requires_safe_pointer : Error<
  "'_ArrayElem' must be combined with '_Owned' or '_Borrow' to qualify a pointer type">,
  BSCDiag<"OWN", 35>;


// BSC trait warnings and errors.
def err_variables_not_trait_pointer : Error<"only _Trait pointer type is allowed to be declared">,
  BSCOutOfScope;
def err_trait_impl : Error<"function %0 in %1 is not implemented for %2">,
  BSCOutOfScope;
def err_trait_impl_function_type_conflict : Error<
  "type of %0 conflicts with that of %1">,
  BSCOutOfScope;
def err_type_has_not_impl_trait : Error<
  "expected a pointer type which has implemented %0, found %1">,
  BSCOutOfScope;
def err_trait_member_not_func : Error<
  "only function declarations can be _Trait member">,
  BSCOutOfScope;
def warn_type_has_not_impl_trait : Warning<
  "expected a pointer type which has implemented %0, found %1">,
  BSCOutOfScope,
  InGroup<BSCTraitMissing>;
def err_trait_is_undefined : Error<"%0 must be defined before it can be used">,
  BSCOutOfScope;
def err_impl_trait_for_instantiated_type : Error<
  "cannot _Impl _Trait for an instantiated type">,
  BSCOutOfScope;

// BSC generic warnings and errors.
def err_invalid_struct_definition : Error <
  "struct definition in generic fuction is not supported in BSC">,
  BSCOutOfScope;
def err_extended_type_not_instantiated_type : Error<
  "extended type of a BSC member function cannot be an instantiated type">,
  BSCOutOfScope;
def err_conflicting_function_declarations : Error<
  "conflicting function declarations for %0">,
  BSCOutOfScope;
def err_bsc_generic_variable : Error<
  "generic variable %0 is not supported; generic parameters can only be "
  "applied to functions, structs, unions and typealiases">,
  BSCDiag<"MISC", 8>;

// BSC constexpr warnings and errors.
def err_constexpr_func_unsupported : Error<
  "BSC constexpr function does not support %0">,
  BSCOutOfScope;
def err_constexpr_func_unsupported_type : Error<
  "BSC constexpr function does not support type %0">,
  BSCOutOfScope;
def err_constexpr_var_unsupported_type : Error<
  "BSC constexpr variable does not support type %0">,
  BSCOutOfScope;
def err_constexpr_if_cond_expr_unsupported_type : Error<
  "BSC constexpr if condition expression does not support type %0">,
  BSCOutOfScope;

// BSC safe scope warnings and errors.
def err_unsafe_cast : Error<"conversion from type %0 to %1 is forbidden in the safe zone">,
  BSCDiag<"SZONE", 5,
          "`note_inc_dec_void_in_safe_zone` — prefix/postfix `++`/`--` in safe zone produce void; use only for side effect (emitted when src is `void`); `note_unsafe_cast_non_trivial_pointee_type` — source pointee %0 is not a trivial data type (emitted for non-trivial `T* borrow → void* borrow`); `note_unsafe_cast_implicit_conversion` — source type %0 is implicit converted from type %1",
          "`note_inc_dec_void_in_safe_zone` is also attached to several **non-BSC** Clang errors when the offending expression sits in a safe zone (`err_typecheck_subscript_not_integer`, `err_typecheck_convert_incompatible`, `err_typecheck_statement_requires_scalar`). Those parent diagnostics are out of scope for this table.">;
def err_unsafe_implicit_cast : Error<"implicit conversion from type %0 to %1 is forbidden in the safe zone; use explicit cast or other means instead">,
  BSCDiag<"SZONE", 6,
          "`note_unsafe_cast_implicit_conversion`">;
def note_unsafe_cast_implicit_conversion : Note<
  "source type %0 is implicit converted from type %1">;
def note_unsafe_cast_non_trivial_pointee_type : Note<
  "source pointee type %0 is not a trivial data type">;
def note_inc_dec_void_in_safe_zone : Note<
  "prefix/postfix '++' and '--' in safe zone produce void; use only for side effect">;
def err_safe_zone_ptr_arithmetic : Error<
  "use of %select{'++'|'--'}0 on a raw pointer is not supported in the safe "
  "zone; consider wrapping it in '_Unsafe { ... }'">,
  BSCDiag<"SZONE", 12,
          "fires only for raw pointers (`_Owned`/`_Borrow` cases hit OWN-025 first); `note_bsc_ptr_declared_here` — pointer `%0` declared here (emitted when operand is a `DeclRefExpr`)">;
def note_bsc_ptr_declared_here : Note<
  "pointer %0 declared here">;
def note_bsc_ptr_arith_fix_named : Note<
  "%select{|use '_Borrow _ArrayElem' or }0declare %1 as a raw pointer for pointer arithmetic">;
def note_bsc_ptr_arith_fix_anon : Note<
  "use a raw pointer for pointer arithmetic">;
def err_return_inc_dec_void_in_safe_zone : Error<
  "result of '++' or '--' cannot be used as return value in safe zone; move the increment/decrement out of the return statement (or add an explicit cast to void to suppress)">,
  BSCDiag<"SZONE", 10>;
def err_unsafe_fun_cast : Error<"conversion from type %0 to %1 is forbidden">,
  BSCDiag<"SZONE", 7,
          "`note_unsafe_to_safe_function_pointer` — assigning an unsafe function pointer to a safe function pointer type is not allowed; `note_safe_to_unsafe_function_no_unsafe_decl` — function %0 has no _Unsafe declaration; add an _Unsafe declaration to enable assignment to an _Unsafe function pointer">;
def note_unsafe_to_safe_function_pointer : Note<
  "assigning an unsafe function pointer to a safe function pointer type is not allowed">;
def note_safe_to_unsafe_function_no_unsafe_decl : Note<
  "function %0 has no _Unsafe declaration; add an _Unsafe declaration to enable assignment to an _Unsafe function pointer">;
def err_safe_function : Error<"%0 is forbidden in a _Safe function">,
  BSCDiag<"SZONE", 8>;
def err_safe_global_var : Error<"defining mutable global variables is not allowed within the safe zone">,
  BSCDiag<"SZONE", 9> ;
def err_bsc_generic_mixed_mode_redecl : Error<"generic function %0 cannot have both _Safe and _Unsafe declarations">,
  BSCDiag<"MISC", 1>;
def err_bsc_unsafe_safe_refinement_type : Error<
  "redeclaration of %0 has incompatible "
  "%select{parameter|return}1 type %2">,
  BSCDiag<"MISC", 2,
          "`note_bsc_redecl_previous` — previous declaration had %select{parameter of type\\|return type}0 %1">;
def err_bsc_unsafe_safe_refinement_param_count : Error<
  "redeclaration of %0 takes %1 parameter%s1 instead of %2">,
  BSCDiag<"MISC", 3,
          "`note_previous_declaration`">;
def err_bsc_unsafe_safe_refinement_variadic : Error<
  "redeclaration of %0 %select{is not|is}1 variadic, "
  "previous declaration %select{is|is not}1 variadic">,
  BSCDiag<"MISC", 4,
          "`note_previous_declaration`">;
def note_bsc_redecl_previous : Note<
  "previous declaration had %select{parameter of type|return type}0 %1">;
def err_bsc_no_matching_mixed_mode_function_call : Error<
  "no matching declaration of %0 for call type %1">,
  BSCDiag<"MISC", 5,
          "One note per candidate redecl callable from the caller's safe zone: `note_bsc_mixed_mode_candidate_arg_count` — call passes %0 argument%s0 but candidate takes %1 (param count mismatch); `note_bsc_mixed_mode_candidate_arg_mismatch` — argument %0 of type %1 doesn't match parameter type %2 (per-argument mismatch); `note_bsc_mixed_mode_candidate` — candidate declaration has type %0 (fallback for other mismatches). When the caller is in a safe zone, only `_Safe` redecls are visited, so no notes are emitted if every redecl is `_Unsafe`.">;
def err_bsc_no_matching_mixed_mode_function_assign : Error<
  "no matching declaration of %0 for assignment to %1">,
  BSCDiag<"MISC", 6,
          "`note_bsc_mixed_mode_candidate` — candidate declaration has type %0 (one per redecl visible from the destination's safe-zone context).">;
def note_bsc_mixed_mode_candidate : Note<
  "candidate declaration has type %0">;
def note_bsc_mixed_mode_candidate_arg_mismatch : Note<
  "argument %0 of type %1 doesn't match parameter type %2">;
def note_bsc_mixed_mode_candidate_arg_count : Note<
  "call passes %0 argument%s0 but candidate takes %1">;
def err_safe_string_init_too_long : Error<
  "too-long initializer-string for char array is forbidden in safe zone; array size %0 cannot hold string of length %1 (including null terminator)">,
  BSCDiag<"SZONE", 11,
          "`note_safe_string_init_too_long_hint` — adjust the length of the array or string, or wrap it with `_Unsafe`">;
def note_safe_string_init_too_long_hint : Note<
  "adjust the length of the array or string, or wrap it with '_Unsafe'">;

// BSC _Owned struct
def err_invalid_member_use_in_class_method : Error <
  "invalid implicit use of member %0 in struct member function">,
  BSCOutOfScope;
def err_static_class_func : Error<
  "function declared in struct scope cannot have 'static' storage class">,
  BSCOutOfScope;
  def err_need_explicit_constructor_owned_struct : Error<"%0 requires an explicit constructor because it has a _Private field">,
  BSCDiag<"OWN", 39>;

// BSC Destructor
def invalid_param_for_destructor : Error<"the parameter of destructor must be '%0 this'">,
  BSCOutOfScope;
def invalid_param_num_for_destructor : Error<"destructor can only have one parameter">,
  BSCOutOfScope;
def err_destructor_call : Error<"destructor %0 cannot be called directly">,
  BSCDiag<"OWN", 43>;
def err_assignment_in_destructor : Error<"'this' cannot be moved in destructor">,
  BSCDiag<"OWN", 44>;
def warn_destructor_execute : Warning<
  "destructor of %0 may not run because the switch can jump over its initialization">,
  BSCDiag<"OWN", 1,
          "Fires only for an `_Owned struct` variable declared in a switch's top-level block; the same hazard via `goto` is currently not diagnosed">,
InGroup<BSCOwnedStruct>;
def err_owned_struct_destructor_name : Error<"expected the _Owned struct name after '~' to name the enclosing _Owned struct">,
  BSCDiag<"OWN", 40>;
def err_owned_struct_destructor_body : Error<"destructor must have a function body">,
  BSCDiag<"OWN", 41>;

// BSC Ownership Analysis errors.
// use
def err_ownership_use_moved : Error<
  "use of moved value: '%0'">,
  BSCDiag<"OWN", 1>;
def err_ownership_use_partially_moved : Error<
  "use of partially moved value: '%0', %1 moved">,
  BSCDiag<"OWN", 2>;
def err_ownership_use_all_moved: Error<
  "use of '%0' after all its fields were moved">,
  BSCDiag<"OWN", 3>;
def err_ownership_use_uninit : Error<
  "use of uninitialized value: '%0'">,
  BSCDiag<"INIT", 1>;
def err_ownership_use_possibly_uninit : Error<
  "use of possibly uninitialized value: '%0'">,
  BSCDiag<"INIT", 2>;

// return value initialization
def err_return_uninit : Error<
  "non-void function '%0' does not return a value%select{| in all control paths}1">,
  BSCDiag<"INIT", 5,
          "Anchored at the function's closing brace when the failing return is the implicit fall-off return">;

// ensure_init contract verification
def err_ensure_init_not_init : Error<
  "'*%0' not initialized at return in __attribute__((ensure_init)) function">,
  BSCDiag<"INIT", 7,
          "`note_ensure_init_ptr_reassigned_here` — `%0` was re-pointed here without initializing `*%0` first (fires when the failing path included a re-point)">;
def err_ensure_init_maybe_not_init : Error<
  "'*%0' may not be initialized on all paths at return in __attribute__((ensure_init)) function">,
  BSCDiag<"INIT", 8,
          "`note_ensure_init_ptr_reassigned_here` — same trigger as INIT-007">;
def err_ensure_init_incomplete_pointee : Error<
  "__attribute__((%select{ensure_init|ensure_init_if_ret}0)) parameter %1 "
  "points to incomplete type %2 in a function definition">,
  BSCDiag<"INIT", 33>;
def err_ensure_init_reassigned : Error<
  "__attribute__((ensure_init)) not satisfied: '%0' is reassigned before '*%0' "
  "is initialized, so '*%0' is not initialized at return">,
  BSCDiag<"INIT", 27,
          "`note_ensure_init_ptr_reassigned_here`; used in place of INIT-007/008 when the failing path's cause is a re-point">;
def err_ensure_init_ptr_aliased : Error<
  "__attribute__((%select{ensure_init|ensure_init_if_ret}1)) parameter '%0' "
  "cannot be reassigned or aliased before '*%0' is initialized">,
  BSCDiag<"INIT", 9,
          "Fires for aliasing the param into a named local, whether written `q = p`, `q = &*p` or `q = &p`; re-pointing is deferred to INIT-007/008 with a note instead">;
def note_ensure_init_ptr_reassigned_here : Note<
  "'%0' was re-pointed here without initializing '*%0' first">;
def err_ensure_init_deref_read_uninit : Error<
  "use of uninitialized '*%0' in "
  "__attribute__((%select{ensure_init|ensure_init_if_ret}1)) function">,
  BSCDiag<"INIT", 10,
          "Covers every form that loads the pointee: `*p`, `p[i]`, a call argument or branch condition, and the prefix an assignment's destination walks through, as in `**p = 1`. Taking an address is not a load, so `&p->f` is not reported">;
def err_ensure_init_funcptr_incompatible : Error<
  "incompatible function pointer types: target expects __attribute__((ensure_init)) on parameter %0 but source does not have it">,
  BSCDiag<"INIT", 11>;

// ensure_init_if_ret contract verification
def err_ensure_init_if_ret_not_init : Error<
  "'*%0' not initialized at return in __attribute__((ensure_init_if_ret(%1))) function">,
  BSCDiag<"INIT", 16,
          "`note_ensure_init_ptr_reassigned_here` — fires when the failing matching-return path included a re-point">;
def err_ensure_init_if_ret_maybe_not_init : Error<
  "'*%0' may not be initialized on all paths at return in __attribute__((ensure_init_if_ret(%1))) function">,
  BSCDiag<"INIT", 17,
          "`note_ensure_init_ptr_reassigned_here` — same trigger as INIT-016">;
def err_ensure_init_if_ret_reassigned : Error<
  "__attribute__((ensure_init_if_ret(%1))) not satisfied: '%0' is reassigned "
  "before '*%0' is initialized, so '*%0' is not guaranteed initialized when "
  "returning %1">,
  BSCDiag<"INIT", 28,
          "`note_ensure_init_ptr_reassigned_here`; used in place of INIT-016/017 when the cause is a re-point">;
def err_ensure_init_if_ret_non_const_return : Error<
  "cannot verify __attribute__((ensure_init_if_ret(%1))) contract for '*%0': "
  "return value is not an integer constant, so the runtime value may equal "
  "the cond value — '*%0' must be initialized on this path">,
  BSCDiag<"INIT", 18,
          "`note_ensure_init_ptr_reassigned_here` — same trigger as INIT-016">;
def err_ensure_init_if_ret_conflicts_ensure_init : Error<
  "__attribute__((ensure_init_if_ret)) cannot be combined with "
  "__attribute__((ensure_init)) on the same parameter">,
  BSCDiag<"INIT", 19>;
def err_ensure_init_if_ret_duplicate : Error<
  "__attribute__((ensure_init_if_ret)) specified more than once with "
  "conflicting arguments on the same parameter">,
  BSCDiag<"INIT", 29,
          "An exact repeat instead warns via the generic `attribute … is already applied`">;
def err_ensure_init_if_ret_arg_not_integer_literal : Error<
  "__attribute__((ensure_init_if_ret)) argument must be an integer literal">,
  BSCDiag<"INIT", 20>;
def err_ensure_init_if_ret_bad_return_type : Error<
  "__attribute__((ensure_init_if_ret)) requires an integer or '_Bool' "
  "return type, but function returns %0">,
  BSCDiag<"INIT", 21>;
def err_ensure_init_if_ret_redecl_mismatch : Error<
  "conflicting __attribute__((ensure_init_if_ret)) on parameter %0 "
  "in redeclaration of %1">,
  BSCDiag<"INIT", 22,
          "`note_previous_declaration` — previous declaration is here">;
def err_ensure_init_if_ret_unsafe_without_safe : Error<
  "__attribute__((ensure_init_if_ret(%0))) on parameter %1 of _Unsafe "
  "declaration requires the matching _Safe declaration to carry the same "
  "attribute with the same argument">,
  BSCDiag<"INIT", 23,
          "`note_previous_declaration` — previous declaration is here">;
def err_ensure_init_redecl_mismatch : Error<
  "conflicting __attribute__((ensure_init)) on parameter %0 "
  "in redeclaration of %1">,
  BSCDiag<"INIT", 30,
          "`note_previous_declaration` — previous declaration is here">;
def err_ensure_init_unsafe_without_safe : Error<
  "__attribute__((ensure_init)) on parameter %0 of _Unsafe "
  "declaration requires the matching _Safe declaration to carry the same "
  "attribute">,
  BSCDiag<"INIT", 31,
          "`note_previous_declaration` — previous declaration is here">;
def err_ensure_init_if_ret_cond_out_of_range : Error<
  "__attribute__((ensure_init_if_ret)) argument %0 is outside the range "
  "[%1, %2] supported in function pointer types">,
  BSCDiag<"INIT", 24,
          "The bound is set by the cond-value bits packed into `ExtParameterInfo` (signed 16-bit field)">;
def err_ensure_init_if_ret_funcptr_missing : Error<
  "incompatible function pointer types: target expects "
  "__attribute__((ensure_init_if_ret)) on parameter %0 but source does "
  "not have it">,
  BSCDiag<"INIT", 25>;
def err_ensure_init_if_ret_funcptr_cond_mismatch : Error<
  "incompatible function pointer types: target expects "
  "__attribute__((ensure_init_if_ret(%0))) on parameter %1 but source has "
  "__attribute__((ensure_init_if_ret(%2)))">,
  BSCDiag<"INIT", 26>;

// __assume_initialized
def err_assume_init_bad_arg : Error<
  "__assume_initialized requires '&' expression as argument">,
  BSCDiag<"INIT", 12>;
def err_assume_init_array_subscript : Error<
  "__assume_initialized argument cannot contain an array subscript">,
  BSCDiag<"INIT", 13,
          "`note_assume_init_array_subscript_hint` — the init analysis tracks arrays as whole units; address the enclosing array instead">;
def note_assume_init_array_subscript_hint : Note<
  "the init analysis tracks arrays as whole units (no per-element state); "
  "address the enclosing array instead (e.g. '&x.arr' for '&x.arr[i]')">;
def err_assume_init_complex_arg : Error<
  "unsupported __assume_initialized argument">,
  BSCDiag<"INIT", 14,
          "`note_assume_init_complex_arg_hint` — the argument must be `&` applied to a variable, struct field access, or pointer dereference">;
def note_assume_init_complex_arg_hint : Note<
  "the argument must be '&' applied to a variable, struct field access, "
  "or pointer dereference">;

// __assume_null
def err_assume_null_not_nullable : Error<
  "__assume_null requires a _Nullable pointer argument">,
  BSCDiag<"NULLABLE", 6,
          "A trackable pointer-typed argument that is not `_Nullable` (e.g. `_Nonnull`, or an unannotated `_Owned`/`_Borrow` pointer, which are `_NonNull` by default) is rejected. A non-pointer argument is rejected instead by NULLABLE-008">;
def err_assume_null_not_trackable : Error<
  "__assume_null requires a trackable expression as argument">,
  BSCDiag<"NULLABLE", 7,
          "The argument is not a trackable expression — a variable (identifier), a struct field access (`.` / `->`), a pointer dereference (`*`), or parens/implicit casts wrapping those. Any other shape (comma, ternary, call, explicit cast, array subscript, `++`/`--`, ...) is rejected: `__assume_null` is a pure analyzer hint (codegen emits nothing) and the dataflow can only lower a trackable path to a tracked location, so anything else would be silently dropped">;
def err_assume_null_unsupported_type : Error<
  "__assume_null requires an argument of pointer or struct type">,
  BSCDiag<"NULLABLE", 8,
          "The argument is trackable but its type is neither a pointer nor a record — e.g. an `int` variable `x`, or `*p` dereferenced to a non-pointer non-record type. Pointer arguments must additionally be `_Nullable` (NULLABLE-006); record arguments are scanned for nullable fields (NULLABLE-009 / W001 / W002)">;
def err_assume_null_nonnull_field : Error<
  "__assume_null is invalid because the struct contains a _Nonnull field">,
  BSCDiag<"NULLABLE", 9,
          "`note_assume_null_nonnull_field_hint` — a _Nonnull field cannot be assumed null; assume each _Nullable field individually if needed. The scan recurses through embedded (sub-struct / array-element) fields but NOT through a pointer field's pointee struct (a separate allocation `__assume_null(*s)` cannot reach). Error takes precedence over the NULLABLE-W001/W002 warnings">;
def note_assume_null_nonnull_field_hint : Note<
  "a _Nonnull field cannot be assumed null; assume each _Nullable field "
  "individually if needed">;
def warn_assume_null_array_nullable_field : Warning<
  "__assume_null has no effect on array field '%0' containing a _Nullable pointer">,
  BSCDiag<"NULLABLE", 1,
          "The dataflow has no per-element null state, so a `_Nullable` pointer array field (or an array whose element struct contains one, incl. nested) is not covered by `__assume_null(*s)` — the builtin neither rejects it (it is _Nullable, not _Nonnull) nor tracks it. `%0` is the full dotted field path (e.g. `in.arr`, `b.c.arr`). Mutually exclusive with NULLABLE-W002 (the array field implies a _Nullable field exists)">,
  InGroup<BSCAssumeNull>;
def warn_assume_null_no_nullable_field : Warning<
  "__assume_null has no effect: the struct does not contain any _Nullable pointer field">,
  BSCDiag<"NULLABLE", 2,
          "Fires when the struct (recursively, through embedded fields) contains no `_Nullable` pointer field at all, so `__assume_null(*s)` has nothing to assert null — the call is a no-op. A bare unannotated `int *p` field is `_Nullable` by default, so a struct with only bare pointer fields does NOT trigger this warning. Mutually exclusive with NULLABLE-W001">,
  InGroup<BSCAssumeNull>;

// __forget
def err_forget_not_owned : Error<
  "__forget requires an _Owned pointer argument">,
  BSCDiag<"OWN", 50,
          "`__forget(p)` explicitly drops ownership tracking of `p`; the operand must be an `_Owned` pointer (a non-_Owned / non-pointer operand is rejected)">;
def err_forget_complex_arg : Error<
  "unsupported __forget argument">,
  BSCDiag<"OWN", 51,
          "`note_forget_complex_arg_hint` — the argument must be a variable or struct field access. `__forget` is a pure analyzer hint (codegen emits nothing), so a compound expression (comma, ternary, call, cast, array subscript, dereference chain `*q`, ...) would be silently dropped and is rejected">;
def note_forget_complex_arg_hint : Note<
  "the argument must be a variable or struct field access">;

// assign
def err_ownership_assign_owned : Error<
  "cannot assign to '%0' because it still holds an _Owned value; move or free it first">,
  BSCDiag<"OWN", 4>;
def err_ownership_assign_partially_moved : Error<
  "cannot assign to '%0': %1 %select{|possibly }2moved out and the remaining _Owned fields would leak">,
  BSCDiag<"OWN", 5>;
def err_ownership_assign_all_moved : Error<
  "cannot assign to '%0': all fields were moved but '%0' still owns its allocation">,
  BSCDiag<"OWN", 7>;
// field assign
def err_ownership_assign_field_uninit : Error<
  "cannot assign to a field of '%0' because '%0' is uninitialized">,
  BSCDiag<"INIT", 3>;
def err_ownership_assign_field_owned : Error<
  "cannot assign to '%0' because it still holds an _Owned value; move or free it first">,
  BSCDiag<"OWN", 8>;
def err_ownership_assign_field_moved : Error<
  "cannot assign to '%0' because the value it belongs to was moved">,
  BSCDiag<"OWN", 9,
          "%0 is always the assigned place (e.g. `p.value`, `**bop`)">;
def err_ownership_assign_field_subfield_owned : Error<
  "cannot assign to '%0': %1 still _Owned and would leak">,
  BSCDiag<"OWN", 10>;

// cast to 'void * _Owned'
def err_ownership_cast_moved : Error<
  "cannot cast '%0' to 'void * _Owned' because it was moved">,
  BSCDiag<"OWN", 11>;
def err_ownership_cast_uninit: Error<
  "cannot cast '%0' to 'void * _Owned' because it is uninitialized">,
  BSCDiag<"INIT", 4,
          "The cast is a consuming hand-off; the rejection is the value's state, not a type mismatch">;
def err_ownership_cast_subfield_owned : Error<
  "cannot cast '%0' to 'void * _Owned': %1 still _Owned; move it out first">,
  BSCDiag<"OWN", 13>;
// memory leak
def err_ownership_memory_leak_field : Error<
  "memory leak because %1 not freed or moved before '%0' goes out of scope">,
  BSCDiag<"OWN", 15>;
def err_ownership_memory_leak : Error<
  "memory leak because '%0' is not freed or moved before it goes out of scope">,
  BSCDiag<"OWN", 14>;
def err_ownership_array_elem_transfer_forbidden : Error<
  "cannot transfer ownership of array element `%0` outside a qualifying for-loop; use a full-range for-loop or safe_swap">,
  BSCDiag<"OWN", 52>;
def err_ownership_array_elem_path_inconsistent : Error<
  "ownership state of array element `%0` is inconsistent across paths in for-loop body">,
  BSCDiag<"OWN", 48>;
def err_ownership_array_elem_assign_owned : Error<
  "cannot assign to '%0' because it still holds an _Owned value; move or free it first">,
  BSCDiag<"OWN", 49,
          "Fires on `arr[i] = ...` / `s[i].p = ...` / `p->arr[i] = ...` when the element (or field) still holds ownership; assigning `nullptr` to an owned element is likewise rejected (it discards the old value). Only a null / moved / uninitialized element may be (re)assigned">;
def note_ownership_array_elem_not_qualifying : Note<
  "for-loop is not a qualifying for-loop: %0">;
def err_ownership_owned_struct_partially_moved : Error<
  "partially moved _Owned struct: '%0' at scope end, %1 moved">,
  BSCDiag<"OWN", 16>;
def err_ownership_owned_struct_not_properly_freed : Error<
  "%1 not freed in the destructor of '%0'; _Owned fields must be freed or moved manually">,
  BSCDiag<"OWN", 17>;
def err_ownership_cast_pass_to_arg_or_ret: Error<
  "cannot pass or return a cast from 'void * _Owned' to %0 because it would use a moved value">,
  BSCDiag<"OWN", 18>;

// New BSC Borrow Checker errors.
def err_borrow_assign_when_borrowed : Error<
  "cannot assign to '%0' because it is borrowed">,
  BSCDiag<"BOR", 1,
          "`note_borrowed_here` — ``%0`` is borrowed here">;
def err_borrow_move_when_borrowed : Error<
  "cannot move out of '%0' because it is borrowed">,
  BSCDiag<"BOR", 2,
          "`note_borrowed_here`">;
def err_borrow_use_when_mut_borrowed : Error<
  "cannot use '%0' because it was mutably borrowed">,
  BSCDiag<"BOR", 3,
          "`note_borrowed_here`">;
def note_borrowed_here : Note<"'%0' is borrowed here">;
def err_borrow_mut_borrow_more_than_once : Error<
  "cannot borrow '%0' as mutable more than once at a time">,
  BSCDiag<"BOR", 4,
          "`note_first_mut_borrow_occurs_here` — first mut borrow occurs here">;
def note_first_mut_borrow_occurs_here : Note<"first mut borrow occurs here">;
def err_borrow_not_live_long : Error<
  "'%0' does not live long enough">,
  BSCDiag<"BOR", 7,
          "`note_dropped_while_borrowed` — ``%0`` dropped here while still borrowed">;
def note_dropped_while_borrowed: Note<"'%0' dropped here while still borrowed">;
def err_borrow_immut_borrow_when_mut_borrowed : Error<
  "cannot borrow '%0' as immutable because it is also borrowed as mutable">,
  BSCDiag<"BOR", 5,
          "`note_mutable_borrow_occurs_here` — mutable borrow occurs here">;
def note_mutable_borrow_occurs_here : Note<"mutable borrow occurs here">;
def err_borrow_mut_borrow_when_immut_borrowed : Error<
  "cannot borrow '%0' as mutable because it is also borrowed as immutable">,
  BSCDiag<"BOR", 6,
          "`note_immutable_borrow_occurs_here` — immutable borrow occurs here">;
def note_immutable_borrow_occurs_here : Note<"immutable borrow occurs here">;
def err_lifetime_may_not_live_long : Error<
  "lifetime may not live long enough">,
  BSCDiag<"BOR", 21>;

// BSC operator overload errors.
def err_unsupport_overload_fun : Error<"function unsupport overload">,
  BSCOutOfScope;
def err_operator_overload_needs_point : Error<
  "overloaded function %0 first parameter or return type must be struct or enumeration pointer type">,
  BSCOutOfScope;

// BSC Pointer Nullability Check errors.
def err_nullable_pointer_dereference : Error<
  "cannot dereference %select{a possibly-null status pointer|possibly-null status pointer '%1'}0">,
  BSCDiag<"NULLABLE", 1>;
def err_pass_nullable_argument : Error<
  "cannot pass %select{a possibly-null status pointer|possibly-null status pointer '%1'}0 to a '_Nonnull' parameter">,
  BSCDiag<"NULLABLE", 2>;
def err_return_nullable : Error<
  "cannot return %select{a possibly-null status pointer|possibly-null status pointer '%1'}0 as a '_Nonnull' pointer">,
  BSCDiag<"NULLABLE", 3>;
def err_nullable_cast_nonnull : Error<
  "cannot cast %select{a possibly-null status pointer|possibly-null status pointer '%1'}0 to a '_Nonnull' pointer">,
  BSCDiag<"NONNULL", 1>;
def err_nullable_pointer_access_member : Error<
  "cannot access the member through %select{a possibly-null status pointer|possibly-null status pointer '%1'}0">,
  BSCDiag<"NULLABLE", 4>;
def err_nonnull_assigned_by_nullable : Error<
  "cannot assign %select{a possibly-null status pointer|possibly-null status pointer '%1'}0 to a '_Nonnull' pointer">,
  BSCDiag<"NONNULL", 2>;
def err_nested_nullability_mismatch : Error<
  "nested pointer nullability mismatch: cannot use %0 as %1">,
  BSCDiag<"NONNULL", 3,
          "`note_nested_pointer_nullability_mismatch` — inner pointer nullability differs between %0 and %1">;
def note_nested_pointer_nullability_mismatch : Note<
  "inner pointer nullability differs between %0 and %1">;
def err_nonnull_init_by_default : Error<
  "cannot implicitly initialize '%0' because it contains '_Nonnull' pointer">,
  BSCDiag<"INIT", 15,
          "`note_nonnull_init_reason` — add explicit initializer for `_Nonnull` pointers inside `%0` or declare them as `_Nullable`",
          "(`err_nonnull_init_by_default` is filed under **INIT** as `INIT-015`; `err_global_nonnull_init_by_default` is filed as `INIT-032`.)">;
def note_nonnull_init_reason : Note<
  "add explicit initializer for '_Nonnull' pointers inside '%0' or declare "
  "them as '_Nullable'">;
def err_global_nonnull_init_by_default : Error<
  "cannot implicitly initialize '%0' because it contains '_Nonnull' pointer">,
  BSCDiag<"INIT", 32,
          "`note_global_nonnull_init_reason` — add explicit initializer for `_Nonnull` pointers inside `%0` or declare them as `_Nullable`">;
def note_global_nonnull_init_reason : Note<
  "add explicit initializer for '_Nonnull' pointers inside '%0' or declare "
  "them as '_Nullable'">;

// BSC string literal borrow errors.
def err_mut_borrow_string_literal : Error<
  "cannot take mutable borrow of string literal with '&_Mut'; string literals are immutable">,
  BSCDiag<"BOR", 18>;
def err_mut_borrow_string_literal_indirect : Error<
  "cannot take mutable borrow into string literal storage; string literals are "
  "immutable; use '&_Const' with a 'const %0 *_Borrow' target, or a plain 'const %0 *'">,
  BSCDiag<"BOR", 19>;
def err_pass_string_literal_to_mut_borrow : Error<
  "cannot pass string literal to parameter of type %0; string literals are immutable">,
  BSCDiag<"BOR", 20>;

def err_bsc_nullptr_cast : Error<
  "cannot cast an object of type %select{'nullptr_t' to %1|%1 to 'nullptr_t'}0">,
  BSCDiag<"NULLABLE", 5>;

// BSC analysis gating.
def warn_bsc_analysis_skipped : Warning<
  "function %0 was not analyzed because of earlier errors; fix the other "
  "errors and recompile">,
  BSCOutOfScope,
  InGroup<BSCAnalysisSkipped>;
