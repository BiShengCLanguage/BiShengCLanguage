// BSC experimental features
def err_bsc_exp_sema : Error<"%0 is an experimental BSC feature; enable with -fbsc-experimental">;

// BSC instance member warnings and errors.
def note_no_this_parameter : Note<"found the following member function %0 which doesn't have a 'this' parameter">;
def err_no_instance_member : Error<
  "no instance member named %0 in %1; did you mean to use '::' instead of '%select{.|->}2'?">;
def err_cvrqualified_member_type_unsupported : Error<
  "type of member function is wrong, CVR qualified type is unsupported, but found '%0'">;
def err_this_type_unsupported : Error<
  "type of parameter 'this' is wrong, expected 'This*' or 'This' or '%0*' or '%0', but found '%1'">;
def err_incompatible_pointer_cast : Error<
  "incompatible conversion from pointer type '%0' to non-pointer type '%1' in member function call">;
def err_incompatible_const_cast : Error<
  "incompatible conversion from const type '%0' to non-const type '%1' in member function call">;
def err_incompatible_owned_cast : Error<
  "incompatible conversion from non-_Owned type '%0' to _Owned type '%1' in member function call">;

// BSC _Async/_Await warnings and errors.
def err_async_non_function : Error<
  "'_Async' can only appear on functions">;
def err_async_main : Error<"'main' is not allowed to be declared _Async">;
def err_await_invalid_scope : Error<
  "_Await expression is not allowed to appear in %0">;
def err_async_func_unsupported : Error<
  "_Async function does not support %0">;
def err_not_a_async_call : Error<
  "the _Await expression's external function declaration must be modified by _Async or return 'Future' type">;
def err_function_not_found : Error <
  "'%0' was not found; include %1 before using the '_Async' keyword">;
def err_invalid_async_function : Error <
  "function which returns 'Future' type should not be modified by '_Async'">;

// BSC ownership warnings and errors.
def err_nested_owned_borrow_type_check : Error<
  "%select{"
  "type of %2 cannot be qualified by '%1'|"
  "type of %2 cannot be qualified by '%1' because %3 contains a '%1' type|"
  "type of %2 cannot be qualified by '%1' (even indirectly) because %3 contains a '%1' type}0">;
def err_owned_funcPtr_incompatible : Error<"cannot cast %0 to %1">;
def err_owned_qualcheck_incompatible : Error<
  "incompatible _Owned types, cannot cast %0 to %1">;
def err_bsc_move_to_raw_not_owned : Error<
  "argument must be an _Owned pointer type (have %0)">;
def err_bsc_take_from_raw_not_raw : Error<
  "argument must be a raw pointer type (have %0)">;
def err_bsc_take_from_raw_function_pointer : Error<
  "__take_from_raw does not support function pointer type %0">;
def err_bsc_move_array_to_raw_not_owned_array : Error<
  "argument must be an _Owned _ArrayElem pointer type (have %0)">;
def note_bsc_move_to_raw_use_move_array_to_raw : Note<
  "use '__move_array_to_raw' for an _Owned _ArrayElem pointer">;
def note_bsc_move_array_to_raw_use_move_to_raw : Note<
  "use '__move_to_raw' for an _Owned pointer that is not _ArrayElem">;
def err_bsc_take_array_from_raw_not_raw : Error<
  "argument must be a raw pointer type (have %0)">;
def err_bsc_take_array_from_raw_function_pointer : Error<
  "__take_array_from_raw does not support function pointer type %0">;
def err_owned_raw_cast_disallowed : Error<
  "cannot cast between _Owned and raw pointer; use __move_to_raw or __take_from_raw for ownership transfer">;
def err_owned_array_raw_cast_disallowed : Error<
  "cannot cast between _Owned _ArrayElem and raw pointer; use __move_array_to_raw or __take_array_from_raw for ownership transfer">;
def err_bsc_op_not_supported : Error<"%0 is not supported on %1">;
def err_owned_temporary_memLeak : Error<
  "the _Owned value of '%0' is discarded and would leak">;
def err_owned_member_access_in_return : Error<
  "cannot use a member of '%0' in a return expression because '%0' is destroyed before the return; copy the member to a local first">;
def err_funcPtr_incompatible : Error<
  "incompatible _Borrow function pointer types, cannot cast %0 to %1">;
def err_mut_expr_unmodifiable  : Error<"the expression after '&_Mut' must be modifiable">;
def err_mut_or_const_expr_func : Error<"%0 cannot be applied to a %select{function|function pointer}1">;
def err_safe_mut : Error<"global or static variables are not allowed to be mutably borrowed within the safe zone">;
def err_move_borrow : Error<"cannot take ownership of a borrowed value">;
def err_typecheck_invalid_borrow_not_pointer : Error<"'_Borrow' can only be applied to a pointer type; %0 is not a pointer">;
def err_borrow_qualcheck_incompatible : Error<
  "incompatible _Borrow types, cannot cast %0 to %1">;
def err_borrow_qualcheck_compare : Error<
  "incompatible _Borrow types, %0 and %1 cannot be compared">;
def err_typecheck_borrow_func : Error<"return type cannot be _Borrow-qualified because no function parameter is _Borrow-qualified">;
def err_typecheck_multi_level_borrow_func : Error<"function return type cannot contain multiple borrow regions">;
def err_typecheck_borrow_subscript : Error<"subscript of _Borrow pointer is not allowed">;
def err_owned_and_borrow_conflict : Error<"cannot combine '_Owned' and '_Borrow' qualifiers on the same type">;
def err_bsc_qualifier_in_knr_function : Error<
  "type with '%0' semantics is not allowed in a K&R-style function definition">;
def err_owned_qualifier_non_pointer : Error<
  "type of %1 cannot be qualified by '%0'">;
def err_arrayelem_requires_safe_pointer : Error<
  "'_ArrayElem' must be combined with '_Owned' or '_Borrow' to qualify a pointer type">;


// BSC trait warnings and errors.
def err_variables_not_trait_pointer : Error<"only _Trait pointer type is allowed to be declared">;
def err_trait_impl : Error<"function %0 in %1 is not implemented for %2">;
def err_trait_impl_function_type_conflict : Error<
  "type of %0 conflicts with that of %1">;
def err_type_has_not_impl_trait : Error<
  "expected a pointer type which has implemented %0, found %1">;
def err_trait_member_not_func : Error<
  "only function declarations can be _Trait member">;
def warn_type_has_not_impl_trait : Warning<
  "expected a pointer type which has implemented %0, found %1">,
  InGroup<BSCTraitMissing>;
def err_trait_is_undefined : Error<"%0 must be defined before it can be used">;
def err_impl_trait_for_instantiated_type : Error<
  "cannot _Impl _Trait for an instantiated type">;

// BSC generic warnings and errors.
def err_invalid_struct_definition : Error <
  "struct definition in generic fuction is not supported in BSC">;
def err_extended_type_not_instantiated_type : Error<
  "extended type of a BSC member function cannot be an instantiated type">;
def err_conflicting_function_declarations : Error<
  "conflicting function declarations for %0">;
  
// BSC constexpr warnings and errors.
def err_constexpr_func_unsupported : Error<
  "BSC constexpr function does not support %0">;
def err_constexpr_func_unsupported_type : Error<
  "BSC constexpr function does not support type %0">;
def err_constexpr_var_unsupported_type : Error<
  "BSC constexpr variable does not support type %0">;
def err_constexpr_if_cond_expr_unsupported_type : Error<
  "BSC constexpr if condition expression does not support type %0">;

// BSC safe scope warnings and errors.
def err_unsafe_cast : Error<"conversion from type %0 to %1 is forbidden in the safe zone">;
def err_unsafe_implicit_cast : Error<"implicit conversion from type %0 to %1 is forbidden in the safe zone; use explicit cast or other means instead">;
def note_unsafe_cast_implicit_conversion : Note<
  "source type %0 is implicit converted from type %1">;
def note_unsafe_cast_non_trivial_pointee_type : Note<
  "source pointee type %0 is not a trivial data type">;
def note_inc_dec_void_in_safe_zone : Note<
  "prefix/postfix '++' and '--' in safe zone produce void; use only for side effect">;
def err_safe_zone_ptr_arithmetic : Error<
  "use of %select{'++'|'--'}0 on a raw pointer is not supported in the safe "
  "zone; consider wrapping it in '_Unsafe { ... }'">;
def note_bsc_ptr_declared_here : Note<
  "pointer %0 declared here">;
def note_bsc_ptr_inc_dec_fix_named : Note<
  "%select{|use '_Borrow _ArrayElem' or }0declare %1 as a raw pointer for pointer arithmetic">;
def note_bsc_ptr_inc_dec_fix_anon : Note<
  "use a raw pointer for pointer arithmetic">;
def err_return_inc_dec_void_in_safe_zone : Error<
  "result of '++' or '--' cannot be used as return value in safe zone; move the increment/decrement out of the return statement (or add an explicit cast to void to suppress)">;
def err_unsafe_fun_cast : Error<"conversion from type %0 to %1 is forbidden">;
def note_unsafe_to_safe_function_pointer : Note<
  "assigning an unsafe function pointer to a safe function pointer type is not allowed">;
def note_safe_to_unsafe_function_no_unsafe_decl : Note<
  "function %0 has no _Unsafe declaration; add an _Unsafe declaration to enable assignment to an _Unsafe function pointer">;
def err_safe_function : Error<"%0 is forbidden in a _Safe function">;
def err_safe_global_var : Error<"defining mutable global variables is not allowed within the safe zone"> ;
def err_bsc_generic_heterogeneous_redecl : Error<"generic function %0 cannot have both _Safe and _Unsafe declarations">;
def err_bsc_incompatible_heterogeneous_redecl_type : Error<
  "redeclaration of %0 has incompatible "
  "%select{parameter|return}1 type %2">;
def err_bsc_incompatible_heterogeneous_redecl_param_count : Error<
  "redeclaration of %0 takes %1 parameter%s1 instead of %2">;
def err_bsc_incompatible_heterogeneous_redecl_variadic : Error<
  "redeclaration of %0 %select{is not|is}1 variadic, "
  "previous declaration %select{is|is not}1 variadic">;
def note_bsc_redecl_previous : Note<
  "previous declaration had %select{parameter of type|return type}0 %1">;
def err_bsc_no_matching_heterogeneous_function_call : Error<
  "no matching declaration of %0 for call type %1">;
def err_bsc_no_matching_heterogeneous_function_assign : Error<
  "no matching declaration of %0 for assignment to %1">;
def note_bsc_heterogeneous_candidate : Note<
  "candidate declaration has type %0">;
def note_bsc_heterogeneous_candidate_arg_mismatch : Note<
  "argument %0 of type %1 doesn't match parameter type %2">;
def note_bsc_heterogeneous_candidate_arg_count : Note<
  "call passes %0 argument%s0 but candidate takes %1">;
def err_safe_string_init_too_long : Error<
  "too-long initializer-string for char array is forbidden in safe zone; array size %0 cannot hold string of length %1 (including null terminator)">;
def note_safe_string_init_too_long_hint : Note<
  "adjust the length of the array or string, or wrap it with '_Unsafe'">;

// BSC _Owned struct
def err_invalid_member_use_in_class_method : Error <
  "invalid implicit use of member %0 in struct member function">;
def err_static_class_func : Error<
  "function declared in struct scope cannot have 'static' storage class">;
  def err_need_explicit_constructor_owned_struct : Error<"%0 requires an explicit constructor because it has a _Private field">;

// BSC Destructor
def invalid_param_for_destructor : Error<"the parameter of destructor must be '%0 this'">;
def invalid_param_num_for_destructor : Error<"destructor can only have one parameter">;
def err_destructor_call : Error<"destructor %0 cannot be called directly">;
def err_assignment_in_destructor : Error<"'this' cannot be moved in destructor">;
def warn_destructor_execute : Warning<
  "destructor of %0 may not run because the switch can jump over its initialization">,
InGroup<BSCOwnedStruct>;
def err_owned_struct_destructor_name : Error<"expected the _Owned struct name after '~' to name the enclosing _Owned struct">;
def err_owned_struct_destructor_body : Error<"destructor must have a function body">;

// BSC Ownership Analysis errors.
// use
def err_ownership_use_moved : Error<
  "use of moved value: '%0'">;
def err_ownership_use_partially_moved : Error<
  "use of partially moved value: '%0', %1 moved">;
def err_ownership_use_all_moved: Error<
  "use of '%0' after all its fields were moved">;
def err_ownership_use_uninit : Error<
  "use of uninitialized value: '%0'">;
def err_ownership_use_possibly_uninit : Error<
  "use of possibly uninitialized value: '%0'">;

// return value initialization
def err_return_uninit : Error<
  "non-void function '%0' does not return a value%select{| in all control paths}1">;

// ensure_init contract verification
def err_ensure_init_not_init : Error<
  "'*%0' not initialized at return in __attribute__((ensure_init)) function">;
def err_ensure_init_maybe_not_init : Error<
  "'*%0' may not be initialized on all paths at return in __attribute__((ensure_init)) function">;
def err_ensure_init_reassigned : Error<
  "__attribute__((ensure_init)) not satisfied: '%0' is reassigned before '*%0' "
  "is initialized, so '*%0' is not initialized at return">;
def err_ensure_init_ptr_aliased : Error<
  "__attribute__((%select{ensure_init|ensure_init_if_ret}1)) parameter '%0' "
  "cannot be reassigned or aliased before '*%0' is initialized">;
def note_ensure_init_ptr_reassigned_here : Note<
  "'%0' was re-pointed here without initializing '*%0' first">;
def err_ensure_init_deref_read_uninit : Error<
  "use of uninitialized '*%0' in "
  "__attribute__((%select{ensure_init|ensure_init_if_ret}1)) function">;
def err_ensure_init_funcptr_incompatible : Error<
  "incompatible function pointer types: target expects __attribute__((ensure_init)) on parameter %0 but source does not have it">;

// ensure_init_if_ret contract verification
def err_ensure_init_if_ret_not_init : Error<
  "'*%0' not initialized at return in __attribute__((ensure_init_if_ret(%1))) function">;
def err_ensure_init_if_ret_maybe_not_init : Error<
  "'*%0' may not be initialized on all paths at return in __attribute__((ensure_init_if_ret(%1))) function">;
def err_ensure_init_if_ret_reassigned : Error<
  "__attribute__((ensure_init_if_ret(%1))) not satisfied: '%0' is reassigned "
  "before '*%0' is initialized, so '*%0' is not guaranteed initialized when "
  "returning %1">;
def err_ensure_init_if_ret_non_const_return : Error<
  "cannot verify __attribute__((ensure_init_if_ret(%1))) contract for '*%0': "
  "return value is not an integer constant, so the runtime value may equal "
  "the cond value — '*%0' must be initialized on this path">;
def err_ensure_init_if_ret_conflicts_ensure_init : Error<
  "__attribute__((ensure_init_if_ret)) cannot be combined with "
  "__attribute__((ensure_init)) on the same parameter">;
def err_ensure_init_if_ret_duplicate : Error<
  "__attribute__((ensure_init_if_ret)) specified more than once with "
  "conflicting arguments on the same parameter">;
def err_ensure_init_if_ret_arg_not_integer_literal : Error<
  "__attribute__((ensure_init_if_ret)) argument must be an integer literal">;
def err_ensure_init_if_ret_bad_return_type : Error<
  "__attribute__((ensure_init_if_ret)) requires an integer or '_Bool' "
  "return type, but function returns %0">;
def err_ensure_init_if_ret_redecl_mismatch : Error<
  "conflicting __attribute__((ensure_init_if_ret)) on parameter %0 "
  "in redeclaration of %1">;
def err_ensure_init_if_ret_unsafe_without_safe : Error<
  "__attribute__((ensure_init_if_ret(%0))) on parameter %1 of _Unsafe "
  "declaration requires the matching _Safe declaration to carry the same "
  "attribute with the same argument">;
def err_ensure_init_redecl_mismatch : Error<
  "conflicting __attribute__((ensure_init)) on parameter %0 "
  "in redeclaration of %1">;
def err_ensure_init_unsafe_without_safe : Error<
  "__attribute__((ensure_init)) on parameter %0 of _Unsafe "
  "declaration requires the matching _Safe declaration to carry the same "
  "attribute">;
def err_ensure_init_if_ret_cond_out_of_range : Error<
  "__attribute__((ensure_init_if_ret)) argument %0 is outside the range "
  "[%1, %2] supported in function pointer types">;
def err_ensure_init_if_ret_funcptr_missing : Error<
  "incompatible function pointer types: target expects "
  "__attribute__((ensure_init_if_ret)) on parameter %0 but source does "
  "not have it">;
def err_ensure_init_if_ret_funcptr_cond_mismatch : Error<
  "incompatible function pointer types: target expects "
  "__attribute__((ensure_init_if_ret(%0))) on parameter %1 but source has "
  "__attribute__((ensure_init_if_ret(%2)))">;

// __assume_initialized
def err_assume_init_bad_arg : Error<
  "__assume_initialized requires '&' expression as argument">;
def err_assume_init_array_subscript : Error<
  "__assume_initialized argument cannot contain an array subscript">;
def note_assume_init_array_subscript_hint : Note<
  "the init analysis tracks arrays as whole units (no per-element state); "
  "address the enclosing array instead (e.g. '&x.arr' for '&x.arr[i]')">;
def err_assume_init_complex_arg : Error<
  "unsupported __assume_initialized argument">;
def note_assume_init_complex_arg_hint : Note<
  "the argument must be '&' applied to a variable, struct field access, "
  "or pointer dereference">;

// __assume_null
def err_assume_null_not_nullable : Error<
  "__assume_null requires a _Nullable pointer argument">;
def err_assume_null_complex_arg : Error<
  "unsupported __assume_null argument">;
def note_assume_null_complex_arg_hint : Note<
  "the argument must be a variable or struct field access">;

// __forget
def err_forget_not_owned : Error<
  "__forget requires an _Owned pointer argument">;
def err_forget_complex_arg : Error<
  "unsupported __forget argument">;
def note_forget_complex_arg_hint : Note<
  "the argument must be a variable or struct field access">;

// assign
def err_ownership_assign_owned : Error<
  "cannot assign to '%0' because it still holds an _Owned value; move or free it first">;
def err_ownership_assign_partially_moved : Error<
  "cannot assign to '%0': %1 %select{|possibly }2moved out and the remaining _Owned fields would leak">;
def err_ownership_assign_all_moved : Error<
  "cannot assign to '%0': all fields were moved but '%0' still owns its allocation">;
// field assign
def err_ownership_assign_field_uninit : Error<
  "cannot assign to a field of '%0' because '%0' is uninitialized">;
def err_ownership_assign_field_owned : Error<
  "cannot assign to '%0' because it still holds an _Owned value; move or free it first">;
def err_ownership_assign_field_moved : Error<
  "cannot assign to '%0' because the value it belongs to was moved">;
def err_ownership_assign_field_subfield_owned : Error<
  "cannot assign to '%0': %1 still _Owned and would leak">;

// cast to 'void * _Owned'
def err_ownership_cast_moved : Error<
  "cannot cast '%0' to 'void * _Owned' because it was moved">;
def err_ownership_cast_uninit: Error<
  "cannot cast '%0' to 'void * _Owned' because it is uninitialized">;
def err_ownership_cast_subfield_owned : Error<
  "cannot cast '%0' to 'void * _Owned': %1 still _Owned; move it out first">;
// memory leak
def err_ownership_memory_leak_field : Error<
  "memory leak because %1 not freed or moved before '%0' goes out of scope">;
def err_ownership_memory_leak : Error<
  "memory leak because '%0' is not freed or moved before it goes out of scope">;
def err_ownership_array_elem_transfer_forbidden : Error<
  "cannot transfer ownership of array element `%0` outside a qualifying for-loop; use a full-range for-loop or safe_swap">;
def err_ownership_array_elem_path_inconsistent : Error<
  "ownership state of array element `%0` is inconsistent across paths in for-loop body">;
def err_ownership_array_elem_assign_owned : Error<
  "cannot assign to '%0' because it still holds an _Owned value; move or free it first">;
def note_ownership_array_elem_not_qualifying : Note<
  "for-loop is not a qualifying for-loop: %0">;
def err_ownership_owned_struct_partially_moved : Error<
  "partially moved _Owned struct: '%0' at scope end, %1 moved">;
def err_ownership_owned_struct_not_properly_freed : Error<
  "%1 not freed in the destructor of '%0'; _Owned fields must be freed or moved manually">;
def err_ownership_cast_pass_to_arg_or_ret: Error<
  "cannot pass or return a cast from 'void * _Owned' to %0 because it would use a moved value">;

// New BSC Borrow Checker errors.
def err_borrow_assign_when_borrowed : Error<
  "cannot assign to '%0' because it is borrowed">;
def err_borrow_move_when_borrowed : Error<
  "cannot move out of '%0' because it is borrowed">;
def err_borrow_use_when_mut_borrowed : Error<
  "cannot use '%0' because it was mutably borrowed">;
def note_borrowed_here : Note<"'%0' is borrowed here">;
def err_borrow_mut_borrow_more_than_once : Error<
  "cannot borrow '%0' as mutable more than once at a time">;
def note_first_mut_borrow_occurs_here : Note<"first mut borrow occurs here">;
def err_borrow_not_live_long : Error<
  "'%0' does not live long enough">;
def note_dropped_while_borrowed: Note<"'%0' dropped here while still borrowed">;
def err_borrow_immut_borrow_when_mut_borrowed : Error<
  "cannot borrow '%0' as immutable because it is also borrowed as mutable">;
def note_mutable_borrow_occurs_here : Note<"mutable borrow occurs here">;
def err_borrow_mut_borrow_when_immut_borrowed : Error<
  "cannot borrow '%0' as mutable because it is also borrowed as immutable">;
def note_immutable_borrow_occurs_here : Note<"immutable borrow occurs here">;
def err_lifetime_may_not_live_long : Error<
  "lifetime may not live long enough">;

// BSC operator overload errors.
def err_unsupport_overload_fun : Error<"function unsupport overload">;
def err_operator_overload_needs_point : Error<
  "overloaded function %0 first parameter or return type must be struct or enumeration pointer type">;

// BSC Pointer Nullability Check errors.
def err_nullable_pointer_dereference : Error<
  "cannot dereference %select{a possibly-null status pointer|possibly-null status pointer '%1'}0">;
def err_pass_nullable_argument : Error<
  "cannot pass %select{a possibly-null status pointer|possibly-null status pointer '%1'}0 to a '_Nonnull' parameter">;
def err_return_nullable : Error<
  "cannot return %select{a possibly-null status pointer|possibly-null status pointer '%1'}0 as a '_Nonnull' pointer">;
def err_nullable_cast_nonnull : Error<
  "cannot cast %select{a possibly-null status pointer|possibly-null status pointer '%1'}0 to a '_Nonnull' pointer">;
def err_nullable_pointer_access_member : Error<
  "cannot access the member through %select{a possibly-null status pointer|possibly-null status pointer '%1'}0">;
def err_nonnull_assigned_by_nullable : Error<
  "cannot assign %select{a possibly-null status pointer|possibly-null status pointer '%1'}0 to a '_Nonnull' pointer">;
def err_nested_nullability_mismatch : Error<
  "nested pointer nullability mismatch: cannot use %0 as %1">;
def note_nested_pointer_nullability_mismatch : Note<
  "inner pointer nullability differs between %0 and %1">;
def err_nonnull_init_by_default : Error<
  "cannot implicitly initialize '%0' because it contains '_Nonnull' pointer">;
def note_nonnull_init_reason : Note<
  "add explicit initializer for '_Nonnull' pointers inside '%0' or declare "
  "them as '_Nullable'">;
def err_global_nonnull_init_by_default : Error<
  "cannot implicitly initialize '%0' because it contains '_Nonnull' pointer">;
def note_global_nonnull_init_reason : Note<
  "add explicit initializer for '_Nonnull' pointers inside '%0' or declare "
  "them as '_Nullable'">;

// BSC string literal borrow errors.
def err_mut_borrow_string_literal : Error<
  "cannot take mutable borrow of string literal with '&_Mut'; string literals are immutable">;
def err_mut_borrow_string_literal_indirect : Error<
  "cannot take mutable borrow into string literal storage; string literals are "
  "immutable; use '&_Const' with a 'const %0 *_Borrow' target, or a plain 'const %0 *'">;
def err_pass_string_literal_to_mut_borrow : Error<
  "cannot pass string literal to parameter of type %0; string literals are immutable">;

def err_bsc_nullptr_cast : Error<
  "cannot cast an object of type %select{'nullptr_t' to %1|%1 to 'nullptr_t'}0">;

// BSC analysis gating.
def warn_bsc_analysis_skipped : Warning<
  "function %0 was not analyzed because of earlier errors; fix the other "
  "errors and recompile">,
  InGroup<BSCAnalysisSkipped>;
