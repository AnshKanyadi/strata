# The engine core (everything except the CLI entry point). As the project grows
# this library accretes the vector/datachunk/operator modules; for P0 it is the
# error type and the SIMD preflight.
add_library(strata_core STATIC
  common/error.cpp
  simd/cpu_features.cpp
  simd/kernels.cc
  data/types.cpp
  data/validity.cpp
  data/string_heap.cpp
  data/selection_vector.cpp
  data/vector.cpp
  data/data_chunk.cpp
  data/column_ops.cpp
  storage/csv_loader.cpp
  exec/expression.cpp
  exec/filter.cpp
  exec/project.cpp
  exec/result_collector.cpp)

target_include_directories(strata_core PUBLIC
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_BINARY_DIR}/generated") # for the configured version.hpp

# `src` on the include path so Highway's foreach_target.h can re-include the
# kernel TU via `#include "simd/kernels.cc"` (HWY_TARGET_INCLUDE). PRIVATE: only
# the kernel build needs it; it does not leak to consumers.
target_include_directories(strata_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

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
