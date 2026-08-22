// RUN: %clang_cc1 -fsyntax-only -verify -x bsc %s
// Tests for _Safe context restrictions with mixed mode

// Unsafe-only cannot be called from _Safe
_Unsafe void unsafe_only(void);

_Safe void test_safe_calls_unsafe(void) {
  unsafe_only();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
}

// Multiple _Unsafe-only calls
_Unsafe int unsafe_compute(int x);
_Unsafe float unsafe_process(float y);
_Unsafe void unsafe_action(void);

_Safe void test_multiple_unsafe(void) {
  int a = unsafe_compute(10);  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)a;

  float b = unsafe_process(3.14f);  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)b;

  unsafe_action();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
}

// Nested _Safe contexts
_Unsafe void nested_unsafe(void);

void test_nested_safe(void) {
  _Unsafe {
    _Safe {
      nested_unsafe();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
    }
  }
}

_Safe void test_safe_with_safe_block(void) {
  _Safe {
    nested_unsafe();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  }
}

// Unsafe in conditionals
_Unsafe int unsafe_get_value(void);

_Safe void test_unsafe_conditional(int flag) {
  int result = flag ? unsafe_get_value() : 0;  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)result;
}

// Unsafe in loops
_Unsafe int unsafe_has_more(void);
_Unsafe void unsafe_process_next(void);

_Safe void test_unsafe_while(void) {
  while (unsafe_has_more()) {  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
    unsafe_process_next();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  }
}

_Safe void test_unsafe_for(void) {
  for (int i = 0; i < unsafe_has_more(); i++) {  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
    unsafe_process_next();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  }
}

// Unsafe in switch
_Unsafe int unsafe_get_code(void);

_Safe void test_unsafe_switch(void) {
  switch (unsafe_get_code()) {  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
    case 1:
      unsafe_action();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
      break;
    default:
      break;
  }
}

// Unsafe with casts
_Unsafe long unsafe_get_long(void);

_Safe void test_unsafe_cast(void) {
  int x = (int)unsafe_get_long();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)x;
}

// Unsafe in compound assignments
_Unsafe int unsafe_increment(void);

_Safe void test_unsafe_compound(void) {
  int counter = 0;
  counter += unsafe_increment();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  counter -= unsafe_increment();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)counter;
}

// Unsafe in complex expressions
_Unsafe int unsafe_op1(int x);
_Unsafe int unsafe_op2(int x);

_Safe void test_unsafe_complex(void) {
  int result = unsafe_op1(10) + unsafe_op2(20);  // expected-error {{_Unsafe function call is forbidden in the safe zone}} expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)result;
}

// Unsafe indirect calls
_Unsafe int unsafe_compute_fp(int x);

_Safe void test_unsafe_indirect(void) {
  int (*fp)(int) = unsafe_compute_fp;
  int result = fp(42);  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)result;
}

// Unsafe in macros
_Unsafe int unsafe_macro_func(int x);

#define CALL_UNSAFE_MACRO(x) unsafe_macro_func(x)

_Safe void test_unsafe_macro(void) {
  int result = CALL_UNSAFE_MACRO(123);  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)result;
}

// Unsafe in array subscripts
_Unsafe int unsafe_get_index(void);

_Safe void test_unsafe_subscript(void) {
  int arr[10] = {0};
  int value = arr[unsafe_get_index()];  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)value;
}

// Unsafe in pointer arithmetic
_Unsafe int unsafe_get_offset(void);

_Safe void test_unsafe_ptr_arith(void) {
  int arr[10] = {0};
  int* p = arr + unsafe_get_offset();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)p;
}

// Unsafe in sizeof (unevaluated context: not checked)
_Unsafe int unsafe_sizeof_func(void);

_Safe void test_unsafe_sizeof(void) {
  int size = sizeof(unsafe_sizeof_func());
  (void)size;
}

// Unsafe as arguments
_Safe void safe_consumer(int x);
_Unsafe int unsafe_producer(void);

_Safe void test_unsafe_argument(void) {
  safe_consumer(unsafe_producer());  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
}

// Unsafe in return
_Unsafe int unsafe_return_value(void);

_Safe int test_unsafe_return(void) {
  return unsafe_return_value();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
}

// Unsafe in initializers
_Unsafe int unsafe_initializer_value(void);

_Safe void test_unsafe_initializer(void) {
  int x = unsafe_initializer_value();  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)x;
}

// Unsafe in struct member access
struct Container {
  int value;
};

_Unsafe struct Container unsafe_get_container(void);

_Safe void test_unsafe_member(void) {
  int val = unsafe_get_container().value;  // expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)val;
}

// Chained _Unsafe calls
_Unsafe int unsafe_chain1(int x);
_Unsafe int unsafe_chain2(int x);
_Unsafe int unsafe_chain3(int x);

_Safe void test_chained_unsafe(void) {
  int result = unsafe_chain3(unsafe_chain2(unsafe_chain1(10)));  // expected-error {{_Unsafe function call is forbidden in the safe zone}} expected-error {{_Unsafe function call is forbidden in the safe zone}} expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)result;
}

// Unsafe in comma expressions
_Unsafe int unsafe_comma1(void);
_Unsafe int unsafe_comma2(void);

_Safe void test_unsafe_comma(void) {
  int result = (unsafe_comma1(), unsafe_comma2());  // expected-error {{_Unsafe function call is forbidden in the safe zone}} expected-error {{_Unsafe function call is forbidden in the safe zone}}
  (void)result;
}

// Safe-only functions work
_Safe void safe_only_func(void);

_Safe void test_safe_calls_safe(void) {
  safe_only_func();
}

// Mixed mode works
_Unsafe void mixed_func(void);
_Safe void mixed_func(void);

_Safe void test_safe_calls_mixed(void) {
  mixed_func();
}
