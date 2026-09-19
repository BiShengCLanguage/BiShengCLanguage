// RUN: %clang_cc1 -fsyntax-only -verify -x bsc %s
// Negative tests for heterogeneous function pointer typedef redeclarations

// Test 1: Parameter count mismatch
_Unsafe typedef void (*BadFuncPtr1)(int* p); // expected-note {{previous declaration is here}}
_Safe typedef void (*BadFuncPtr1)(int* _Owned p, int* _Owned q); // expected-error {{redeclaration of 'BadFuncPtr1' takes 2 parameters instead of 1}}

// Test 2: Return type mismatch (different base types)
_Unsafe typedef int* (*BadFuncPtr2)(void); // expected-note {{previous declaration had return type 'int *'}}
_Safe typedef float* _Owned (*BadFuncPtr2)(void); // expected-error {{redeclaration of 'BadFuncPtr2' has incompatible return type 'float *_Owned'}}

// Test 3: Parameter type mismatch (different base types)
_Unsafe typedef void (*BadFuncPtr3)(int* p); // expected-note {{previous declaration had parameter of type 'int *'}}
_Safe typedef void (*BadFuncPtr3)(float* _Owned p); // expected-error {{redeclaration of 'BadFuncPtr3' has incompatible parameter type 'float *_Owned'}}

// Test 4: Pointer level mismatch
_Unsafe typedef int* (*BadFuncPtr4)(void); // expected-note {{previous declaration had return type 'int *'}}
_Safe typedef int** _Owned (*BadFuncPtr4)(void); // expected-error {{redeclaration of 'BadFuncPtr4' has incompatible return type 'int **_Owned'}}

// Test 5: Variadic mismatch
_Unsafe typedef void (*BadFuncPtr5)(int* p, ...); // expected-note {{previous declaration is here}}
_Safe typedef void (*BadFuncPtr5)(int* _Owned p); // expected-error {{redeclaration of 'BadFuncPtr5' is not variadic, previous declaration is variadic}}

// Test 6: Nested pointer target type mismatch
_Unsafe typedef int** (*BadFuncPtr6)(void); // expected-note {{previous declaration had return type 'int **'}}
_Safe typedef float** _Owned (*BadFuncPtr6)(void); // expected-error {{redeclaration of 'BadFuncPtr6' has incompatible return type 'float **_Owned'}}

// Test 7: Homogeneous but incompatible (different parameter count)
_Unsafe typedef void (*BadFuncPtr7)(int* p); // expected-note {{previous definition is here}}
_Unsafe typedef void (*BadFuncPtr7)(int* q, int* r); // expected-error {{typedef redefinition with different types ('void (*)(int *, int *)' vs 'void (*)(int *)')}}

// Test 8: Heterogeneous function redeclaration where the function-pointer
// parameter types have mismatched base types (int* vs float*) — must still error.
typedef _Safe  int (*good_cmp_safe)(const void * _Borrow, const void * _Borrow);
typedef        int (*bad_cmp_unsafe)(int *, float *); // different param types

_Safe  void bad_sort(void * _Borrow base, unsigned int n, good_cmp_safe compar); // expected-note {{previous declaration had parameter of type 'good_cmp_safe' (aka '_Safe int (*)(const void *_Borrow, const void *_Borrow)')}}
void bad_sort(void * base, unsigned int n, bad_cmp_unsafe compar); // expected-error {{redeclaration of 'bad_sort' has incompatible parameter type 'bad_cmp_unsafe' (aka 'int (*)(int *, float *)')}}

// Test 9: Heterogeneous function redeclaration where the function-pointer
// parameter types have different parameter counts — must still error.
typedef _Safe  int (*cmp3_safe)(const void * _Borrow, const void * _Borrow);
typedef        int (*cmp3_unsafe)(const void *); // one fewer param

_Safe  void bad_sort2(void * _Borrow base, unsigned int n, cmp3_safe compar); // expected-note {{previous declaration had parameter of type 'cmp3_safe' (aka '_Safe int (*)(const void *_Borrow, const void *_Borrow)')}}
void bad_sort2(void * base, unsigned int n, cmp3_unsafe compar); // expected-error {{redeclaration of 'bad_sort2' has incompatible parameter type 'cmp3_unsafe' (aka 'int (*)(const void *)')}}

// Test 10: _Safe typedef must not add _ArrayElem to an unsafe `_Owned` parameter.
_Unsafe typedef void (*BadFuncPtr10)(int* _Owned p); // expected-note {{previous declaration had parameter of type 'int *_Owned'}}
_Safe typedef void (*BadFuncPtr10)(int* _Owned _ArrayElem p); // expected-error {{redeclaration of 'BadFuncPtr10' has incompatible parameter type 'int *_Owned _ArrayElem'}}

// Test 11: _Safe typedef must not add _ArrayElem to an unsafe `_Owned` return type.
_Unsafe typedef int* _Owned (*BadFuncPtr11)(void); // expected-note {{previous declaration had return type 'int *_Owned'}}
_Safe typedef int* _Owned _ArrayElem (*BadFuncPtr11)(void); // expected-error {{redeclaration of 'BadFuncPtr11' has incompatible return type 'int *_Owned _ArrayElem'}}

// Test 11a: _Safe typedef must not add _ArrayElem to an unsafe `_Borrow` parameter.
_Unsafe typedef void (*BadFuncPtr11a)(int* _Borrow p); // expected-note {{previous declaration had parameter of type 'int *_Borrow'}}
_Safe typedef void (*BadFuncPtr11a)(int* _Borrow _ArrayElem p); // expected-error {{redeclaration of 'BadFuncPtr11a' has incompatible parameter type 'int *_Borrow _ArrayElem'}}

// Test 11b: _Safe typedef must not add _ArrayElem to an unsafe `_Borrow` return type
// (parameters include _Borrow so a _Borrow return is allowed).
_Unsafe typedef int* _Borrow (*BadFuncPtr11b)(int* _Borrow p); // expected-note {{previous declaration had return type 'int *_Borrow'}}
_Safe typedef int* _Borrow _ArrayElem (*BadFuncPtr11b)(int* _Borrow p); // expected-error {{redeclaration of 'BadFuncPtr11b' has incompatible return type 'int *_Borrow _ArrayElem'}}

// Test 12: _Safe typedef must not drop `_ArrayElem` from an unsafe `_Borrow _ArrayElem` parameter.
_Unsafe typedef int* _Borrow _ArrayElem (*BadFuncPtr12)(int* _Borrow _ArrayElem p); // expected-note {{previous declaration had return type 'int *_Borrow _ArrayElem'}}
_Safe typedef int* _Borrow (*BadFuncPtr12)(int* _Borrow p); // expected-error {{redeclaration of 'BadFuncPtr12' has incompatible return type 'int *_Borrow'}}

// Test 13: _Safe typedef must not drop `_ArrayElem` from an unsafe `_Borrow _ArrayElem` return type.
_Unsafe typedef int* _Borrow _ArrayElem (*BadFuncPtr13)(int* _Borrow _ArrayElem p); // expected-note {{previous declaration had return type 'int *_Borrow _ArrayElem'}}
_Safe typedef int* _Borrow (*BadFuncPtr13)(int* _Borrow _ArrayElem p); // expected-error {{redeclaration of 'BadFuncPtr13' has incompatible return type 'int *_Borrow'}}
