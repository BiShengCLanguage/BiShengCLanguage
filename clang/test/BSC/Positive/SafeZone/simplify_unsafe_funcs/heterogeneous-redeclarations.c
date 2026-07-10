// RUN: %clang_cc1 -fsyntax-only -verify -x bsc %s
// expected-no-diagnostics
// Positive tests: _Safe redeclaration adding qualifiers to unsafe declaration.

// _Safe redecl may add _Borrow to a parameter that was raw in the unsafe decl.
int* f_add_borrow_param(int* a);
_Safe int* _Borrow f_add_borrow_param(int* _Borrow a);

// _Safe redecl may add _Owned to a parameter that was raw in the unsafe decl.
int* f_add_owned_param(int* a);
_Safe int* _Owned f_add_owned_param(int* _Owned a);

// _Safe redecl may add _Borrow to the return type.
int* f_add_borrow_ret(int* _Borrow a);
_Safe int* _Borrow f_add_borrow_ret(int* _Borrow a);

// _Safe redecl may add _Owned to the return type.
int* f_add_owned_ret(int* _Owned a);
_Safe int* _Owned f_add_owned_ret(int* _Owned a);

// _Safe redecl retains _Borrow on both param and return.
int* _Borrow f_keep_borrow(int* _Borrow a);
_Safe int* _Borrow f_keep_borrow(int* _Borrow a);

// _Safe redecl retains _Owned on both param and return.
int* _Owned f_keep_owned(int* _Owned a);
_Safe int* _Owned f_keep_owned(int* _Owned a);

// _Safe redecl may add `_Borrow _ArrayElem` to a raw pointer parameter/return.
int* f_add_borrow_arrayelem(int* a);
_Safe int* _Borrow _ArrayElem f_add_borrow_arrayelem(int* _Borrow _ArrayElem a);

// Heterogeneous redecls may keep `_Borrow _ArrayElem` on both sides.
int* _Borrow _ArrayElem f_keep_borrow_arrayelem(int* _Borrow _ArrayElem a);
_Safe int* _Borrow _ArrayElem f_keep_borrow_arrayelem(int* _Borrow _ArrayElem a);

// Heterogeneous redecls may keep `_Owned _ArrayElem` on both sides.
int* _Owned _ArrayElem f_keep_owned_arrayelem(int* _Owned _ArrayElem a);
_Safe int* _Owned _ArrayElem f_keep_owned_arrayelem(int* _Owned _ArrayElem a);

// Multiple parameters: safe adds qualifiers to all raw params.
void f_multi(int* a, int* b);
_Safe void f_multi(int* _Borrow a, int* _Owned b);

// Default nullability annotations: raw pointer (default _Nullable) on unsafe
// side with _Borrow (default _Nonnull) on safe side — should be OK because
// unsafe-safe refinement allows _Nullable → _Nonnull.
void f_default_nullable(int *p);
_Safe void f_default_nullable(int *_Borrow p);

// Multiple levels: raw pointer default _Nullable → _Borrow default _Nonnull.
void f_default_nullable2(int **p);
_Safe void f_default_nullable2(int **_Borrow p);
