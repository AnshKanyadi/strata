# Strata — a vectorized, columnar, push-based analytical query engine.
# See docs/adr/0002-cxx23-homebrew-llvm-toolchain.md for the toolchain rationale.
cmake_minimum_required(VERSION 3.24) # 3.24+: FetchContent FIND_PACKAGE_ARGS.

project(Strata
  VERSION 0.0.1
  DESCRIPTION "A mini-DuckDB: a vectorized columnar query engine, validated against DuckDB"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global standard / hygiene
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # -std=c++23, never -std=gnu++23.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for clangd / clang-tidy.

# Default to Release if a single-config generator was invoked without a type,
# so a bare `cmake ..` does something sensible. Presets override this.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Benchmark/release builds want -march=native; we add it only to the Release
# config so sanitizer builds stay portable and reproducible.
string(APPEND CMAKE_CXX_FLAGS_RELEASE " -march=native")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Only the top-level project drives testing/CI wiring.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(STRATA_IS_TOP_LEVEL ON)
else()
  set(STRATA_IS_TOP_LEVEL OFF)
endif()

include(CompilerOptions) # defines strata_warnings + strata_sanitizers.
include(Dependencies)    # Highway / GoogleTest / Google Benchmark.

# ---------------------------------------------------------------------------
# Generated version header (carries build-time toolchain facts for `--version`)
# ---------------------------------------------------------------------------
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/strata/version.hpp.in"
  "${CMAKE_BINARY_DIR}/generated/strata/version.hpp"
  @ONLY)

add_subdirectory(src)

if(STRATA_IS_TOP_LEVEL)
  enable_testing()
  add_subdirectory(tests)
  add_subdirectory(bench)
endif()

message(STATUS "Strata ${PROJECT_VERSION} | ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} | "
               "build=${CMAKE_BUILD_TYPE} | sanitizer=${STRATA_SANITIZER}")
