# The engine core (everything except the CLI entry point). As the project grows
# this library accretes the vector/datachunk/operator modules; for P0 it is the
# error type and the SIMD preflight.
add_library(strata_core STATIC
  common/error.cpp
  simd/cpu_features.cpp
  data/types.cpp
  data/validity.cpp
  data/string_heap.cpp
  data/selection_vector.cpp
  data/vector.cpp
  data/data_chunk.cpp
  storage/csv_loader.cpp
  exec/result_collector.cpp)

target_include_directories(strata_core PUBLIC
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_BINARY_DIR}/generated") # for the configured version.hpp

target_compile_features(strata_core PUBLIC cxx_std_23)

# Warnings are PRIVATE (our code is held to them; consumers aren't forced to be).
# Sanitizers are PUBLIC so every consumer is instrumented and linked identically.
# Highway is PUBLIC because cpu_features.hpp's implementation needs it and we may
# expose SIMD types from headers later.
target_link_libraries(strata_core
  PRIVATE strata_warnings_strict
  PUBLIC  strata_sanitizers hwy::hwy)

# The CLI / preflight binary.
add_executable(strata strata_cli.cpp)
target_link_libraries(strata PRIVATE strata_core strata_warnings_strict)
