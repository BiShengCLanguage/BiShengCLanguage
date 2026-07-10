// RUN: %clang_cc1 -fsyntax-only -verify -x bsc %s
// Positive tests for mixed mode function pointers
// Tests the VALID cases that should compile without errors

// Mixed mode declarations
_Unsafe void func1(void);
_Safe void func1(void);

_Unsafe int compute(int x);
_Safe int compute(int x);

_Safe void safe_only_func(void);
_Unsafe void unsafe_only_func(void);
void test_valid_assignments(void) {
  // Valid: function with both _Safe and _Unsafe declarations can be assigned
  // to either _Safe or _Unsafe (or unqualified) function pointers.

  // _Unsafe function pointer ← function that has _Unsafe declaration (via func1)
  _Unsafe void (*unsafe_ptr)(void) = nullptr;
  unsafe_ptr = unsafe_only_func;          // OK: _Unsafe -> _Unsafe
  unsafe_ptr = func1;                     // OK: func1 has _Unsafe declaration

  // Unqualified function pointer ← function that has _Unsafe declaration
  void (*unqual_ptr)(void) = nullptr;
  unqual_ptr = func1;                     // OK: func1 has _Unsafe declaration

  // Valid: _Safe pointers accept _Safe functions
  _Safe void (*safe_ptr)(void) = nullptr;
  safe_ptr = safe_only_func;              // OK: _Safe -> _Safe
  safe_ptr = func1;                       // OK: func1 has _Safe declaration

  (void)unsafe_ptr;
  (void)unqual_ptr;
  (void)safe_ptr;
}

// expected-no-diagnostics
