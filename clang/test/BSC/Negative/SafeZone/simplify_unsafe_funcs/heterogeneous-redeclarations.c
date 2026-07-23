// RUN: %clang_cc1 -fsyntax-only -verify -x bsc %s
// Negative tests for heterogeneous function redeclarations (Manual sections 7.1-7.4)
// Heterogeneous redeclarations: one _Safe, one _Unsafe declaration of the same function

_Unsafe int test_param_count1(int a); // expected-note {{previous declaration is here}}
_Safe int test_param_count1(int a, int b); // expected-error {{redeclaration of 'test_param_count1' takes 2 parameters instead of 1}}

_Unsafe void test_param_count2(int a, int b, int c); // expected-note {{previous declaration is here}}
_Safe void test_param_count2(int a); // expected-error {{redeclaration of 'test_param_count2' takes 1 parameter instead of 3}}

// Variadic mismatch (_Safe declarations cannot be variadic in BSC, so the
// only reachable case is a non-variadic _Safe redeclaration of a variadic
// _Unsafe declaration).
_Unsafe void test_variadic_mismatch(int a, ...); // expected-note {{previous declaration is here}}
_Safe void test_variadic_mismatch(int a); // expected-error {{redeclaration of 'test_variadic_mismatch' is not variadic, previous declaration is variadic}}

_Unsafe int test_return_type1(void); // expected-note {{previous declaration had return type 'int'}}
_Safe float test_return_type1(void); // expected-error {{redeclaration of 'test_return_type1' has incompatible return type 'float'}}

_Unsafe void* test_return_type2(void); // expected-note {{previous declaration had return type 'void *'}}
_Safe int* _Owned test_return_type2(void); // expected-error {{redeclaration of 'test_return_type2' has incompatible return type 'int *_Owned'}}

_Safe void test_owned_borrow(int* _Owned p); // expected-note {{previous declaration is here}}
_Safe void test_owned_borrow(int* _Borrow p); // expected-error {{conflicting types for 'test_owned_borrow'}}

_Safe int* _Owned test_owned_vs_borrow(int* _Owned p); // expected-note {{previous declaration is here}}
_Safe int* _Borrow test_owned_vs_borrow(int* _Borrow p); // expected-error {{conflicting types for 'test_owned_vs_borrow'}}

_Unsafe const int* test_const_compat1(int* p); // expected-note {{previous declaration had parameter of type 'int *'}}
_Safe const int* _Owned test_const_compat1(const int* _Owned p); // expected-error {{redeclaration of 'test_const_compat1' has incompatible parameter type 'const int *_Owned'}}

_Unsafe int* test_const_compat2(const int* p); // expected-note {{previous declaration had parameter of type 'const int *'}}
_Safe int* _Owned test_const_compat2(int* _Owned p); // expected-error {{redeclaration of 'test_const_compat2' has incompatible parameter type 'int *_Owned'}}

_Unsafe volatile int* test_volatile_compat(int* p);  // expected-note {{previous declaration had parameter of type 'int *'}}
_Safe volatile int* _Owned test_volatile_compat(volatile int* _Owned p); // expected-error {{redeclaration of 'test_volatile_compat' has incompatible parameter type 'volatile int *_Owned'}}

_Unsafe T generic_func<T>(T x); // expected-note {{previous declaration is here}}
_Safe T generic_func<T>(T x); // expected-error {{generic function 'generic_func' cannot have both _Safe and _Unsafe declarations}}


_Unsafe void test_param_type(int x); // expected-note {{previous declaration had parameter of type 'int'}}
_Safe void test_param_type(float x); // expected-error {{redeclaration of 'test_param_type' has incompatible parameter type 'float'}}

// Different pointer target types
_Unsafe int* test_ptr_target(int* p); // expected-note {{previous declaration had return type 'int *'}}
_Safe float* _Owned test_ptr_target(float* _Owned p); // expected-error {{redeclaration of 'test_ptr_target' has incompatible return type 'float *_Owned'}}

_Unsafe int* test_ptr_level1(int* p); // expected-note {{previous declaration had return type 'int *'}}
_Safe int** _Owned test_ptr_level1(int** _Owned p); // expected-error {{redeclaration of 'test_ptr_level1' has incompatible return type 'int **_Owned'}}

_Unsafe int*** test_ptr_level2(int*** p); // expected-note {{previous declaration had return type 'int ***'}}
_Safe int** _Owned test_ptr_level2(int** _Owned p); // expected-error {{redeclaration of 'test_ptr_level2' has incompatible return type 'int **_Owned'}}

// _Safe redeclaration may not drop qualifiers present in the unsafe declaration.
int* _Borrow test_drop_borrow_param(int* _Borrow a); // expected-note {{previous declaration had return type 'int *_Borrow'}}
_Safe int* test_drop_borrow_param(int* a); // expected-error {{redeclaration of 'test_drop_borrow_param' has incompatible return type 'int *'}}

int* _Owned test_drop_owned_param(int* _Owned a); // expected-note {{previous declaration had return type 'int *_Owned'}}
_Safe int* test_drop_owned_param(int* a); // expected-error {{redeclaration of 'test_drop_owned_param' has incompatible return type 'int *'}}

int* _Borrow test_drop_borrow_ret(int* _Borrow a); // expected-note {{previous declaration had return type 'int *_Borrow'}}
_Safe int* test_drop_borrow_ret(int* _Borrow a); // expected-error {{redeclaration of 'test_drop_borrow_ret' has incompatible return type 'int *'}}

int* _Borrow _ArrayElem test_drop_arrayelem_from_borrow_param(int* _Borrow _ArrayElem a); // expected-note {{previous declaration had return type 'int *_Borrow _ArrayElem'}}
_Safe int* _Borrow test_drop_arrayelem_from_borrow_param(int* _Borrow a); // expected-error {{redeclaration of 'test_drop_arrayelem_from_borrow_param' has incompatible return type 'int *_Borrow'}}

int* _Borrow _ArrayElem test_drop_arrayelem_from_borrow_ret(int* _Borrow _ArrayElem a); // expected-note {{previous declaration had return type 'int *_Borrow _ArrayElem'}}
_Safe int* _Borrow test_drop_arrayelem_from_borrow_ret(int* _Borrow _ArrayElem a); // expected-error {{redeclaration of 'test_drop_arrayelem_from_borrow_ret' has incompatible return type 'int *_Borrow'}}

int* _Owned test_drop_owned_ret(int* _Owned a); // expected-note {{previous declaration had return type 'int *_Owned'}}
_Safe int* test_drop_owned_ret(int* _Owned a); // expected-error {{redeclaration of 'test_drop_owned_ret' has incompatible return type 'int *'}}

int* _Owned test_add_arrayelem_owned_param(int* _Owned a); // expected-note {{previous declaration had parameter of type 'int *_Owned'}}
_Safe int* _Owned test_add_arrayelem_owned_param(int* _Owned _ArrayElem a); // expected-error {{redeclaration of 'test_add_arrayelem_owned_param' has incompatible parameter type 'int *_Owned _ArrayElem'}}

int* _Owned test_add_arrayelem_owned_ret(int* _Owned a); // expected-note {{previous declaration had return type 'int *_Owned'}}
_Safe int* _Owned _ArrayElem test_add_arrayelem_owned_ret(int* _Owned a); // expected-error {{redeclaration of 'test_add_arrayelem_owned_ret' has incompatible return type 'int *_Owned _ArrayElem'}}

int* _Borrow test_add_arrayelem_borrow_param(int* _Borrow a); // expected-note {{previous declaration had parameter of type 'int *_Borrow'}}
_Safe int* _Borrow test_add_arrayelem_borrow_param(int* _Borrow _ArrayElem a); // expected-error {{redeclaration of 'test_add_arrayelem_borrow_param' has incompatible parameter type 'int *_Borrow _ArrayElem'}}

int* _Borrow test_add_arrayelem_borrow_ret(int* _Borrow a); // expected-note {{previous declaration had return type 'int *_Borrow'}}
_Safe int* _Borrow _ArrayElem test_add_arrayelem_borrow_ret(int* _Borrow a); // expected-error {{redeclaration of 'test_add_arrayelem_borrow_ret' has incompatible return type 'int *_Borrow _ArrayElem'}}

// Nullability mismatch: _Unsafe side is _Nonnull, _Safe side is _Nullable (forbidden).
_Unsafe void test_nullability(int * _Nonnull p); // expected-note {{previous declaration had parameter of type 'int *_Nonnull'}}
_Safe void test_nullability(int * _Borrow _Nullable p); // expected-error {{redeclaration of 'test_nullability' has incompatible parameter type 'int *_Borrow _Nullable'}}

// _Owned default _Nonnull vs explicit _Nullable — explicitly conflicting
// nullability on same-safety _Owned pointers (forbidden).
void test_owned_nullable_mismatch(int *_Owned p); // expected-note {{previous declaration is here}}
void test_owned_nullable_mismatch(int *_Owned _Nullable p); // expected-error {{conflicting types for 'test_owned_nullable_mismatch'}}
