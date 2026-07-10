// RUN: %clang_cc1 -fsyntax-only -verify -x bsc %s
// Simplified negative tests for heterogeneous function pointer assignment

// Test 1: Owned/_Borrow mismatch - no matching _Safe declaration
_Unsafe void take_owned(int* p);
_Safe void take_owned(int* _Owned p);

void test1(void) {
    _Safe void (*ptr_borrow)(int* _Borrow) = 0;
    ptr_borrow = take_owned;  // expected-error {{cannot cast}}
}

// Test 2: Borrow/_Owned mismatch - no matching _Safe declaration
_Unsafe void take_borrow(int* p);
_Safe void take_borrow(int* _Borrow p);

void test2(void) {
    _Safe void (*ptr_owned)(int* _Owned) = 0;
    ptr_owned = take_borrow;  // expected-error {{cannot cast}}
}

// Test 3: Parameter count mismatch
_Unsafe void two_params(int* p1, int* p2);
_Safe void two_params(int* _Owned p1, int* _Owned p2);

void test3(void) {
    _Safe void (*ptr_one)(int* _Owned) = 0;
    ptr_one = two_params;  // expected-error {{cannot cast}}
}

// Test 4: Type mismatch in parameters
_Unsafe void int_param(int* p);
_Safe void int_param(int* _Owned p); // expected-note {{candidate declaration has type '_Safe void (int *_Owned)'}}

void test4(void) {
    _Safe void (*ptr_float)(float* _Owned) = 0;
    ptr_float = int_param;  // expected-error {{no matching declaration of 'int_param' for assignment to '_Safe void (*)(float *_Owned)'}}
}

// Test 5: void* _Owned/_Borrow mismatch in function pointer
_Unsafe void void_owned(void* p);
_Safe void void_owned(void* _Owned p);

void test5(void) {
    _Safe void (*ptr_borrow)(void* _Borrow) = 0;
    ptr_borrow = void_owned;  // expected-error {{cannot cast}}
}

// Test 6: Const mismatch - char* _Borrow vs const char* _Borrow
_Unsafe void mut_char(char* p);
_Safe void mut_char(char* _Borrow p);

void test6(void) {
    _Safe void (*ptr_const)(const char* _Borrow) = 0;
    ptr_const = mut_char;  // expected-error {{cannot cast}}
}

// Test 7: Nullability mismatch — _Nonnull param cannot be assigned to
// _Nullable (default raw) function pointer parameter (any zone).
void nonnull_func(int * _Nonnull p);

void test7(void) {
    void (*fp)(int *) = nonnull_func;  // expected-error {{conversion from type 'void (*)(int * _Nonnull)' to 'void (*)(int *)' is forbidden}}
}

// Test 8: _Owned (default _Nonnull) with explicit _Nullable mismatch.
void owned_nonnull_func(int *_Owned p);

_Safe void test8(void) {
    void (*fp)(int *_Owned _Nullable) = owned_nonnull_func;  // expected-error {{conversion from type 'void (*)(int *_Owned)' to 'void (*)(int *_Owned _Nullable)' is forbidden}}
}

// Test 9: Return type nullability mismatch.
int * _Nonnull nonnull_ret(void);

void test9(void) {
    int * _Nullable (*fp)(void) = nonnull_ret;  // expected-error {{conversion from type 'int * _Nonnull (*)(void)' to 'int * _Nullable (*)(void)' is forbidden}}
}
