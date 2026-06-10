add_libcbs_test_suite(libcbs-safety-tests)

add_libcbs_test(
  test_safety
  SUITE
    libcbs-safety-tests
  SRCS
    bishengc_safety_test.cbs
)

add_libcbs_negative_test(
  test_safe_calloc_non_zero_init
  SUITE
    libcbs-safety-tests
  SRC
    safe_calloc_non_zero_init.cbs
)
