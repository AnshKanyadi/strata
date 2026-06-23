include(GoogleTest)

add_executable(strata_tests
  test_result.cpp
  test_cpu_features.cpp
  test_validity.cpp
  test_string.cpp
  test_selection_vector.cpp
  test_vector.cpp
  test_data_chunk.cpp
  test_date.cpp
  test_loader.cpp
  test_pipeline.cpp
  test_kernels.cpp
  test_expression.cpp
  test_filter.cpp
  test_project.cpp
  test_p3_pipeline.cpp
  test_hash_aggregate.cpp
  test_hash_join.cpp
  test_sort.cpp
  test_top_n.cpp
  test_limit.cpp
  test_optimizer.cpp
  test_sql.cpp
  test_thread_pool.cpp
  test_parallel_aggregate.cpp)

target_link_libraries(strata_tests
  PRIVATE strata_core strata_warnings GTest::gtest_main)

# Register each TEST() with CTest. PRE_TEST defers discovery to test time so it
# works even when the test binary is sanitizer-instrumented.
gtest_discover_tests(strata_tests DISCOVERY_MODE PRE_TEST)
