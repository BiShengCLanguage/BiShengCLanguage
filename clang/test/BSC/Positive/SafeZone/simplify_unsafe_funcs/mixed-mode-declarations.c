// RUN: %clang_cc1 -fsyntax-only -verify -x bsc %s
// Positive tests for mixed mode function declarations (Manual sections 7.1-7.4)

_Unsafe int* process1(int* p);
_Safe int* _Owned process1(int* _Owned p);

// Manual 2500-2501: void* compatibility
_Unsafe void* get_data(void);
_Safe void* _Owned get_data(void);

// Manual 2508-2509: Same modifiers, identical types
_Safe int* _Owned bar(int* _Owned p);
_Safe int* _Owned bar(int* _Owned q);

// These are all compatible even with mismatched const/volatile
_Unsafe const int* get_const(void);
_Safe const int* _Owned get_const(void);

_Unsafe volatile int* get_volatile(void);
_Safe volatile int* _Owned get_volatile(void);

_Unsafe const volatile int* get_cv(void);
_Safe const volatile int* _Owned get_cv(void);

// Manual 2486: _Unsafe T* ⟷ _Safe T* _Borrow
_Unsafe void process_borrow(int* p);
_Safe void process_borrow(int* _Borrow p);

_Unsafe void take_borrow(void* p);
_Safe void take_borrow(void* _Borrow p);

// Manual 2474: Parameter count must be same
_Unsafe void multi_param(int a, int b, int c);
_Safe void multi_param(int a, int b, int c);

// No parameters
_Unsafe int no_params(void);
_Safe int no_params(void);

_Unsafe int** double_ptr(int** p);
_Safe int** _Owned double_ptr(int** _Owned p);

_Unsafe int*** triple_ptr(int*** p);
_Safe int*** _Owned triple_ptr(int*** _Owned p);

// Manual 2518-2524: Mixed mode function can have one definition
_Unsafe int* defined_func(int* p);
_Safe int* _Owned defined_func(int* _Owned p);
// Definition based on _Safe version
_Safe int* _Owned defined_func(int* _Owned p) { return p; }

// Definition based on _Unsafe version
_Unsafe float* defined_func2(float* p) { return p; }
_Safe float* _Owned defined_func2(float* _Owned p);

// Manual 2465: Multiple _Safe declarations must be compatible
_Safe void safe_multi1(int x);
_Safe void safe_multi1(int y);  // ok: same type, different param name

// Multiple _Unsafe declarations
_Unsafe void unsafe_multi1(int x);
_Unsafe void unsafe_multi1(int y);

// Non-pointer parameters are compatible
_Unsafe int add(int a, int b);
_Safe int add(int a, int b);

_Unsafe float compute(float x, float y);
_Safe float compute(float x, float y);

// Multiple parameters with mixed pointer and non-pointer
_Unsafe void complex1(int a, int* p, float b, int* q);
_Safe void complex1(int a, int* _Owned p, float b, int* _Owned q);

// Pointer to const with _Owned
_Unsafe const int* complex2(const int* p);
_Safe const int* _Owned complex2(const int* _Owned p);

// SZ_None and SZ_Unsafe are both "unsafe" — redeclarations between them are
// homogeneous and must be accepted like any standard C compatible redeclaration.
void plain_then_unsafe(int *a, int *b);
_Unsafe void plain_then_unsafe(int *a, int *b);

_Unsafe void unsafe_then_plain(int *a, int *b);
void unsafe_then_plain(int *a, int *b);

// Default nullability compatibility: raw pointer (default _Nullable) is
// compatible with explicit _Nullable in same-safety redeclarations.
void raw_default_nullable(int *p);
void raw_default_nullable(int * _Nullable p);

_Safe void safe_default_nullable(int *p);
_Safe void safe_default_nullable(int * _Nullable p);

// _Borrow pointer (default _Nonnull) is compatible with explicit _Nonnull.
void borrow_default_nonnull(int *_Borrow p);
void borrow_default_nonnull(int *_Borrow _Nonnull p);

// _Owned pointer (default _Nonnull) is compatible with explicit _Nonnull.
void owned_default_nonnull(int *_Owned p);
void owned_default_nonnull(int *_Owned _Nonnull p);

// expected-no-diagnostics
