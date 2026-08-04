# BSC-Specific Diagnostics by Feature

This reference lists the diagnostics that BiSheng C adds on top of standard Clang, grouped by language feature.

Codes follow a `FEATURE-NNN` scheme: each feature numbers its errors independently starting from `001`; warnings use a `FEATURE-WNNN` series. Notes are not coded — they only appear attached to an error or warning, and are listed in the **Notes** column of the row that emits them.

---

## OWN — owned (46 errors, 1 warning)

| Code | Diagnostic | Message | Notes |
|------|------------|---------|-------|
| OWN-001 | err_ownership_use_moved | use of moved value: `%0` | — |
| OWN-002 | err_ownership_use_partially_moved | use of partially moved value: `%0`, %1 moved | — |
| OWN-003 | err_ownership_use_all_moved | use of all moved value: `%0` | — |
| OWN-004 | err_ownership_assign_owned | assign to _Owned value: `%0` | — |
| OWN-005 | err_ownership_assign_partially_moved | assign to partially moved value: `%0`, %1 moved | — |
| OWN-006 | err_ownership_assign_possibly_partially_moved | assign to possibly partially moved value: `%0`, %1 possibly moved | — |
| OWN-007 | err_ownership_assign_all_moved | assign to all moved value: `%0` | — |
| OWN-008 | err_ownership_assign_field_owned | assign to part of _Owned value: `%0` | — |
| OWN-009 | err_ownership_assign_field_moved | assign to part of moved value: `%0` | — |
| OWN-010 | err_ownership_assign_field_subfield_owned | assign to subfield _Owned value: `%0`, %1 _Owned | — |
| OWN-011 | err_ownership_cast_moved | invalid cast to `void * _Owned` of moved or uninitialized value: `%0` | — |
| OWN-012 | err_ownership_cast_owned | invalid cast to `void * _Owned` of _Owned value: `%0` | — |
| OWN-013 | err_ownership_cast_subfield_owned | invalid cast to `void * _Owned` of not all moved value: `%0`, %1 _Owned | — |
| OWN-014 | err_ownership_memory_leak | memory leak of value: `%0` | — |
| OWN-015 | err_ownership_memory_leak_field | field memory leak of value: `%0`, %1 leak | — |
| OWN-016 | err_ownership_owned_struct_patially_moved | partially moved _Owned struct: `%0` at scope end, %1 moved | — |
| OWN-017 | err_ownership_owned_struct_not_properly_freed | destructor for `%0` incorrect, %1 of _Owned type and needs to be handled manually | — |
| OWN-018 | err_ownership_cast_pass_to_arg_or_ret | cannot pass or return a cast from `void *_Owned` to %0 because it would use a moved value | — |
| OWN-019 | err_nested_owned_borrow_type_check | type of %2 cannot be qualified by `%1` (3-variant select) | — |
| OWN-020 | err_owned_qualcheck_incompatible | incompatible _Owned types, cannot cast %0 to %1 | — |
| OWN-021 | err_owned_temporary_memLeak | memory leak because temporary variable `%0` is _Owned or indirect _Owned type | — |
| OWN-022 | err_owned_qualifier_non_pointer | type of %1 cannot be qualified by `%0` | — |
| OWN-023 | err_owned_and_borrow_conflict | cannot combine `_Owned` and `_Borrow` qualifiers on the same type | — |
| OWN-024 | err_typecheck_invalid_owned_binOp | invalid operands to binary expression (%0 and %1) | — |
| OWN-025 | err_bsc_ptr_inc_dec | `'++'`/`'--'` is not supported on `%1` | fires for `++`/`--` on _Owned pointers and on non-_ArrayElem _Borrow pointers, regardless of safe zone; `note_bsc_ptr_declared_here` — pointer `%0` declared here; `note_bsc_ptr_inc_dec_fix_named` — declare `%1` as a raw pointer for pointer arithmetic (prefixed with "use `_Borrow _ArrayElem` or " when the _Borrow was initialized from `&arr[i]`); `note_bsc_ptr_inc_dec_fix_anon` — use a raw pointer for pointer arithmetic (when the operand is not a `DeclRefExpr`) |
| OWN-026 | err_typecheck_invalid_owned_arrsub | _Owned pointer type (%0) do not support ArraySubscript operate | — |
| OWN-027 | err_owned_raw_cast_disallowed | cannot cast between _Owned and raw pointer; use `__move_to_raw` or `__take_from_raw` for ownership transfer | — |
| OWN-028 | err_owned_array_raw_cast_disallowed | cannot cast between _Owned _ArrayElem and raw pointer; use `__move_array_to_raw` or `__take_array_from_raw` for ownership transfer | — |
| OWN-029 | err_bsc_move_to_raw_not_owned | argument must be an _Owned pointer type (have %0) | `note_bsc_move_to_raw_use_move_array_to_raw` — use `__move_array_to_raw` for an _Owned _ArrayElem pointer |
| OWN-030 | err_bsc_take_from_raw_not_raw | argument must be a raw pointer type (have %0) | — |
| OWN-031 | err_bsc_take_from_raw_function_pointer | `__take_from_raw` does not support function pointer type %0 | — |
| OWN-032 | err_bsc_move_array_to_raw_not_owned_array | argument must be an _Owned _ArrayElem pointer type (have %0) | `note_bsc_move_array_to_raw_use_move_to_raw` — use `__move_to_raw` for an _Owned pointer that is not _ArrayElem |
| OWN-033 | err_bsc_take_array_from_raw_not_raw | argument must be a raw pointer type (have %0) | — |
| OWN-034 | err_bsc_take_array_from_raw_function_pointer | `__take_array_from_raw` does not support function pointer type %0 | — |
| OWN-035 | err_arrayelem_requires_safe_pointer | `_ArrayElem` must be used with `_Owned` or `_Borrow` together to qualify a pointer type | — |
| OWN-036 | err_arrayelem_invalid_pointee | pointee type of %0 cannot be _Owned or _Borrow pointers or contain _Owned or _Borrow qualified fields | — |
| OWN-037 | err_bsc_qualifier_in_knr_function | type with `%0` semantics is not allowed in a K&R-style function definition | — |
| OWN-038 | err_incompatible_owned_cast | incompatible conversion from non _Owned type `%0` to _Owned type `%1` in member function call | — |
| OWN-039 | err_need_explicit_constructor_owned_struct | need explicit constructor because %0 has private field | — |
| OWN-040 | err_owned_struct_destructor_name | expected the _Owned struct name after `~` to name the enclosing _Owned struct | — |
| OWN-041 | err_owned_struct_destructor_body | destructor must have a function body | — |
| OWN-042 | err_owned_struct_in_function_scope | _Owned struct cannot be defined in function scope; move the definition to file scope | — |
| OWN-043 | err_destructor_call | destructor %0 cannot be called directly | — |
| OWN-044 | err_assignment_in_destructor | `this` cannot be moved in destructor | — |
| OWN-045 | err_tag_name | must ignore `_Owned struct` tag before struct name | — |
| OWN-046 | err_inconsistent_tag_name | inconsistent tag before struct name | — |
| **OWN-W001** | warn_destructor_execute | the destructor may be not executed | — |

---

## BOR — borrow (20 errors)

Every borrow-check error is emitted with one note from `BSCBorrowChecker.h::flushDiagnostics`. Confirmed against the source.

| Code | Diagnostic | Message | Notes |
|------|------------|---------|-------|
| BOR-001 | err_borrow_assign_when_borrowed | cannot assign to `%0` because it is borrowed | `note_borrowed_here` — ``%0`` is borrowed here |
| BOR-002 | err_borrow_move_when_borrowed | cannot move out of `%0` because it is borrowed | `note_borrowed_here` |
| BOR-003 | err_borrow_use_when_mut_borrowed | cannot use `%0` because it was mutably borrowed | `note_borrowed_here` |
| BOR-004 | err_borrow_mut_borrow_more_than_once | cannot borrow `%0` as mutable more than once at a time | `note_first_mut_borrow_occurs_here` — first mut borrow occurs here |
| BOR-005 | err_borrow_immut_borrow_when_mut_borrowed | cannot borrow `%0` as immutable because it is also borrowed as mutable | `note_mutable_borrow_occurs_here` — mutable borrow occurs here |
| BOR-006 | err_borrow_mut_borrow_when_immut_borrowed | cannot borrow `%0` as mutable because it is also borrowed as immutable | `note_immutable_borrow_occurs_here` — immutable borrow occurs here |
| BOR-007 | err_borrow_not_live_long | `%0` does not live long enough | `note_dropped_while_borrowed` — ``%0`` dropped here while still borrowed |
| BOR-008 | err_borrow_on_borrow | %0 on a `_Borrow` qualified type is not allowed | — |
| BOR-009 | err_borrow_qualcheck_incompatible | incompatible _Borrow types, cannot cast %0 to %1 | — |
| BOR-010 | err_borrow_qualcheck_compare | incompatible _Borrow types, %0 and %1 cannot be compared | — |
| BOR-011 | err_typecheck_invalid_borrow_not_pointer | _Borrow type requires a pointer or reference (%0 is invalid) | — |
| BOR-012 | err_typecheck_borrow_func | no _Borrow qualified type found in the function parameters, the return type is not allowed to be _Borrow qualified | — |
| BOR-013 | err_typecheck_borrow_subscript | subscript of _Borrow pointer is not allowed | — |
| BOR-014 | err_move_borrow | _Borrow type does not allow move ownership | — |
| BOR-015 | err_mut_expr_unmodifiable | the expression after `&_Mut` must be modifiable | — |
| BOR-016 | err_mut_or_const_expr_func | `%0` for function pointer is not allowed | — |
| BOR-017 | err_safe_mut | global or static variables are not allowed to be mutably borrowed within the safe zone | — |
| BOR-018 | err_mut_borrow_string_literal | cannot take mutable borrow of string literal with `&_Mut`; string literals are immutable | — |
| BOR-019 | err_mut_borrow_string_literal_indirect | cannot take mutable borrow into string literal storage; string literals are immutable | — |
| BOR-020 | err_pass_string_literal_to_mut_borrow | cannot pass string literal to parameter of type %0; string literals are immutable | — |

(`err_bsc_ptr_inc_dec` also fires for `++`/`--` on non-_ArrayElem _Borrow pointers; filed under **OWN** as `OWN-025`. _Borrow pointers qualified with `_ArrayElem` are explicitly allowed.)

---

## INIT — initialization (32 errors, 1 warning)

| Code | Diagnostic | Message | Notes |
|------|------------|---------|-------|
| INIT-001 | err_ownership_use_uninit | use of uninitialized value: `%0` | — |
| INIT-002 | err_ownership_use_possibly_uninit | use of possibly uninitialized value: `%0` | — |
| INIT-003 | err_ownership_assign_field_uninit | assign to part of uninitialized value: `%0` | — |
| INIT-004 | err_ownership_cast_uninit | invalid cast to `void * _Owned` of uninit value: `%0` | — |
| INIT-005 | err_return_uninit | return value of `%0` is not initialized on all paths | — |
| INIT-006 | err_return_possibly_uninit | return value of `%0` may not be initialized on all paths | — |
| INIT-007 | err_ensure_init_not_init | `*%0` not initialized at return in `__attribute__((ensure_init))` function | `note_ensure_init_ptr_reassigned_here` — `'%0'` was re-pointed here without initializing `'*%0'` first (fires when the failing path included a re-point) |
| INIT-008 | err_ensure_init_maybe_not_init | `*%0` may not be initialized on all paths at return in `__attribute__((ensure_init))` function | `note_ensure_init_ptr_reassigned_here` — same trigger as INIT-007 |
| INIT-009 | err_ensure_init_ptr_aliased | `__attribute__((ensure_init))` parameter `%0` cannot be reassigned or aliased before `*%0` is initialized | Fires for aliasing the param into a named local; re-pointing is deferred to INIT-007/008 with a note instead |
| INIT-010 | err_ensure_init_deref_read_uninit | use of uninitialized `*%0` in `__attribute__((ensure_init))` function | — |
| INIT-011 | err_ensure_init_funcptr_incompatible | incompatible function pointer types: target expects `__attribute__((ensure_init))` on parameter %0 but source does not have it | — |
| INIT-012 | err_assume_init_bad_arg | `__assume_initialized` requires `&` expression as argument | — |
| INIT-013 | err_assume_init_array_subscript | `__assume_initialized` argument cannot contain an array subscript | `note_assume_init_array_subscript_hint` — the init analysis tracks arrays as whole units; address the enclosing array instead |
| INIT-014 | err_assume_init_complex_arg | unsupported `__assume_initialized` argument | `note_assume_init_complex_arg_hint` — the argument must be `&` applied to a variable, struct field access, or pointer dereference |
| INIT-015 | err_nonnull_init_by_default | cannot implicitly initialize `'%0'` because it contains `'_Nonnull'` pointer | `note_nonnull_init_reason` — add explicit initializer for `'_Nonnull'` pointers inside `'%0'` or declare them as `'_Nullable'` |
| INIT-016 | err_ensure_init_if_ret_not_init | `'*%0'` not initialized at return in `__attribute__((ensure_init_if_ret(%1)))` function | `note_ensure_init_ptr_reassigned_here` — fires when the failing matching-return path included a re-point |
| INIT-017 | err_ensure_init_if_ret_maybe_not_init | `'*%0'` may not be initialized on all paths at return in `__attribute__((ensure_init_if_ret(%1)))` function | `note_ensure_init_ptr_reassigned_here` — same trigger as INIT-016 |
| INIT-018 | err_ensure_init_if_ret_non_const_return | cannot verify `__attribute__((ensure_init_if_ret(%1)))` contract for `'*%0'`: return value is not an integer constant, so the runtime value may equal the cond value — `'*%0'` must be initialized on this path | `note_ensure_init_ptr_reassigned_here` — same trigger as INIT-016 |
| INIT-019 | err_ensure_init_if_ret_conflicts_ensure_init | `__attribute__((ensure_init_if_ret))` cannot be combined with `__attribute__((ensure_init))` on the same parameter | — |
| INIT-020 | err_ensure_init_if_ret_arg_not_integer_literal | `__attribute__((ensure_init_if_ret))` argument must be an integer literal | — |
| INIT-021 | err_ensure_init_if_ret_bad_return_type | `__attribute__((ensure_init_if_ret))` requires an integer or `'_Bool'` return type, but function returns %0 | — |
| INIT-022 | err_ensure_init_if_ret_redecl_mismatch | conflicting `__attribute__((ensure_init_if_ret))` on parameter %0 in redeclaration of %1 | `note_previous_declaration` — previous declaration is here |
| INIT-023 | err_ensure_init_if_ret_unsafe_without_safe | `__attribute__((ensure_init_if_ret(%0)))` on parameter %1 of `_Unsafe` declaration requires the matching `_Safe` declaration to carry the same attribute with the same argument | `note_previous_declaration` — previous declaration is here |
| INIT-024 | err_ensure_init_if_ret_cond_out_of_range | `__attribute__((ensure_init_if_ret))` argument %0 is outside the range [%1, %2] supported in function pointer types | The bound is set by the cond-value bits packed into `ExtParameterInfo` (signed 16-bit field) |
| INIT-025 | err_ensure_init_if_ret_funcptr_missing | incompatible function pointer types: target expects `__attribute__((ensure_init_if_ret))` on parameter %0 but source does not have it | — |
| INIT-026 | err_ensure_init_if_ret_funcptr_cond_mismatch | incompatible function pointer types: target expects `__attribute__((ensure_init_if_ret(%0)))` on parameter %1 but source has `__attribute__((ensure_init_if_ret(%2)))` | — |
| INIT-027 | err_ensure_init_reassigned | `__attribute__((ensure_init))` not satisfied: `%0` is reassigned before `*%0` is initialized, so `*%0` is not initialized at return | `note_ensure_init_ptr_reassigned_here`; used in place of INIT-007/008 when the failing path's cause is a re-point |
| INIT-028 | err_ensure_init_if_ret_reassigned | `__attribute__((ensure_init_if_ret(%1)))` not satisfied: `%0` is reassigned before `*%0` is initialized, so `*%0` is not guaranteed initialized when returning %1 | `note_ensure_init_ptr_reassigned_here`; used in place of INIT-016/017 when the cause is a re-point |
| INIT-029 | err_ensure_init_if_ret_duplicate | `__attribute__((ensure_init_if_ret))` specified more than once with conflicting arguments on the same parameter | An exact repeat instead warns via the generic `attribute … is already applied` |
| INIT-030 | err_ensure_init_redecl_mismatch | conflicting `__attribute__((ensure_init))` on parameter %0 in redeclaration of %1 | `note_previous_declaration` — previous declaration is here |
| INIT-031 | err_ensure_init_unsafe_without_safe | `__attribute__((ensure_init))` on parameter %0 of `_Unsafe` declaration requires the matching `_Safe` declaration to carry the same attribute | `note_previous_declaration` — previous declaration is here |
| INIT-032 | err_global_nonnull_init_by_default | cannot implicitly initialize `'%0'` because it contains `'_Nonnull'` pointer | `note_global_nonnull_init_reason` — add explicit initializer for `'_Nonnull'` pointers inside `'%0'` or declare them as `'_Nullable'` |
| **INIT-W001** | warn_ensure_init_not_addressof | `%select{ensure_init\|ensure_init_if_ret}0` effect cannot be verified when argument is not an address-of expression | Message names the attribute the parameter actually carries; also fires for `ensure_init_if_ret` parameters, including indirect calls through a function-pointer typedef |

---

## MISC — declaration / dispatch consistency (7 errors)

Catch-all for small-count categories that don't merit their own feature: heterogeneous `_Safe`/`_Unsafe` declarations, struct member redeclaration, and overload-style dispatch failures. The heterogeneous-redecl error was split into four kind-specific diagnostics (return type / param type / param count / variadic) and the no-matching-function error was split by context (call vs. assignment), both pointing carets at the offending token.

| Code | Diagnostic | Message | Notes |
|------|------------|---------|-------|
| MISC-001 | err_bsc_generic_heterogeneous_redecl | generic function %0 cannot have both _Safe and _Unsafe declarations | — |
| MISC-002 | err_bsc_incompatible_heterogeneous_redecl_type | redeclaration of %0 has incompatible %select{parameter\|return}1 type %2 | `note_bsc_redecl_previous` — previous declaration had %select{parameter of type\|return type}0 %1 |
| MISC-003 | err_bsc_incompatible_heterogeneous_redecl_param_count | redeclaration of %0 takes %1 parameter%s1 instead of %2 | `note_previous_declaration` |
| MISC-004 | err_bsc_incompatible_heterogeneous_redecl_variadic | redeclaration of %0 %select{is not\|is}1 variadic, previous declaration %select{is\|is not}1 variadic | `note_previous_declaration` |
| MISC-005 | err_bsc_no_matching_heterogeneous_function_call | no matching declaration of %0 for call type %1 | One note per candidate redecl callable from the caller's safe zone: `note_bsc_heterogeneous_candidate_arg_count` — call passes %0 argument%s0 but candidate takes %1 (param count mismatch); `note_bsc_heterogeneous_candidate_arg_mismatch` — argument %0 of type %1 doesn't match parameter type %2 (per-argument mismatch); `note_bsc_heterogeneous_candidate` — candidate declaration has type %0 (fallback for other mismatches). When the caller is in a safe zone, only `_Safe` redecls are visited, so no notes are emitted if every redecl is `_Unsafe`. |
| MISC-006 | err_bsc_no_matching_heterogeneous_function_assign | no matching declaration of %0 for assignment to %1 | `note_bsc_heterogeneous_candidate` — candidate declaration has type %0 (one per redecl visible from the destination's safe-zone context). |
| MISC-007 | err_struct_member_redeclared | struct member cannot be redeclared | — |

---

## SZONE — safe zone (12 errors)

| Code | Diagnostic | Message | Notes |
|------|------------|---------|-------|
| SZONE-001 | err_unsafe_action | %0 is forbidden in the safe zone | — |
| SZONE-002 | err_safe_zone_decl | `_Safe`/`_Unsafe` can only appear before on function or statement or parenthesized expression | — |
| SZONE-003 | err_union_member_access_in_safe_zone | access to union field is `_Unsafe` and requires `_Unsafe` block | — |
| SZONE-004 | err_safe_zone_case_in_nested_braces | `case` label inside a nested `{ }` in the safe zone | `note_safe_zone_case_in_nested_braces` — remove the nested `{ }`, or wrap the switch in `_Unsafe { ... }` |
| SZONE-005 | err_unsafe_cast | conversion from type %0 to %1 is forbidden in the safe zone | `note_inc_dec_void_in_safe_zone` — prefix/postfix `++`/`--` in safe zone produce void; use only for side effect (emitted when src is `void`); `note_unsafe_cast_non_trivial_pointee_type` — source pointee %0 is not a trivial data type (emitted for non-trivial `T* borrow → void* borrow`); `note_unsafe_cast_implicit_conversion` — source type %0 is implicit converted from type %1 |
| SZONE-006 | err_unsafe_implicit_cast | implicit conversion from type %0 to %1 is forbidden in the safe zone; use explicit cast or other means instead | `note_unsafe_cast_implicit_conversion` |
| SZONE-007 | err_unsafe_fun_cast | conversion from type %0 to %1 is forbidden | `note_unsafe_to_safe_function_pointer` — assigning an unsafe function pointer to a safe function pointer type is not allowed; `note_safe_to_unsafe_function_no_unsafe_decl` — assigning a safe function pointer to an unsafe function pointer type is not allowed |
| SZONE-008 | err_safe_function | %0 is forbidden in the `_Safe` function | — |
| SZONE-009 | err_safe_global_var | defining mutable global variables is not allowed within the safe zone | — |
| SZONE-010 | err_return_inc_dec_void_in_safe_zone | result of `++` or `--` cannot be used as return value in safe zone; move the increment/decrement out of the return statement (or add an explicit cast to `void` to suppress) | — |
| SZONE-011 | err_safe_string_init_too_long | too-long initializer-string for char array is forbidden in safe zone; array size %0 cannot hold string of length %1 (including null terminator) | `note_safe_string_init_too_long_hint` — adjust the length of the array or string, or wrap it with `_Unsafe` |
| SZONE-012 | err_safe_zone_ptr_arithmetic | use of `'++'`/`'--'` on raw pointer not supported in safe zone; consider wrap in `'_Unsafe { ... }'` | fires only for raw pointers (`_Owned`/`_Borrow` cases hit OWN-025 first); `note_bsc_ptr_declared_here` — pointer `%0` declared here (emitted when operand is a `DeclRefExpr`) |

`note_inc_dec_void_in_safe_zone` is also attached to several **non-BSC** Clang errors when the offending expression sits in a safe zone (`err_typecheck_subscript_not_integer`, `err_typecheck_convert_incompatible`, `err_typecheck_statement_requires_scalar`). Those parent diagnostics are out of scope for this table.

---

## NONNULL — nonnull pointer (3 errors)

| Code | Diagnostic | Message | Notes |
|------|------------|---------|-------|
| NONNULL-001 | err_nullable_cast_nonnull | cannot cast nullable pointer to nonnull type | — |
| NONNULL-002 | err_nonnull_assigned_by_nullable | nonnull pointer cannot be assigned by nullable pointer | — |
| NONNULL-003 | err_nested_nullability_mismatch | nested pointer nullability mismatch from <source> to <dest>, inner <inner_source> mismatch with <inner_dest> | — |

(`err_nonnull_init_by_default` is filed under **INIT** as `INIT-015`;
`err_global_nonnull_init_by_default` is filed as `INIT-032`.)

---

## NULLABLE — nullable pointer (5 errors)

| Code | Diagnostic | Message | Notes |
|------|------------|---------|-------|
| NULLABLE-001 | err_nullable_pointer_dereference | nullable pointer cannot be dereferenced | — |
| NULLABLE-002 | err_pass_nullable_argument | cannot pass nullable pointer argument | — |
| NULLABLE-003 | err_return_nullable | cannot return nullable pointer type | — |
| NULLABLE-004 | err_nullable_pointer_access_member | cannot access member through nullable pointer | — |
| NULLABLE-005 | err_bsc_nullptr_cast | cannot cast an object of type `nullptr_t` to %1 / %1 to `nullptr_t` | — |

---

## Summary

| Prefix     | Feature                       | Errors | Warnings |
|------------|-------------------------------|-------:|---------:|
| OWN-       | owned                         | 46     | 1        |
| BOR-       | borrow                        | 20     | 0        |
| INIT-      | initialization                | 32     | 1        |
| MISC-      | declaration / dispatch        | 7      | 0        |
| SZONE-     | safe zone                     | 12     | 0        |
| NONNULL-   | nonnull pointer               | 3      | 0        |
| NULLABLE-  | nullable pointer              | 5      | 0        |
| **Total**  |                               | **125** | **2**   |

Plus **25 BSC-specific notes**, each tied to one or more of the errors above (see the Notes column per row).

Out-of-scope BSC features (not coded here): traits, async/await, generic, constexpr, operator overload, instance member functions. These contribute several more errors, one warning (`warn_type_has_not_impl_trait`), and one note (`note_no_this_parameter`).
