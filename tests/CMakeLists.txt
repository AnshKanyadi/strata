include(GoogleTest)

add_executable(strata_tests
  test_result.cpp
  test_cpu_features.cpp
  test_validity.cpp
  test_string.cpp
  test_selection_vector.cpp
  test_vector.cpp
  test_data_chunk.cpp)

target_link_libraries(strata_tests
  PRIVATE strata_core strata_warnings GTest::gtest_main)

# Register each TEST() with CTest. PRE_TEST defers discovery to test time so it
# works even when the test binary is sanitizer-instrumented.
gtest_discover_tests(strata_tests DISCOVERY_MODE PRE_TEST)
