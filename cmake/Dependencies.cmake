# Dependency strategy (see docs/adr/0002): prefer a locally-installed package
# (Homebrew on macOS, apt on the Linux CI runner) via find_package(CONFIG); if
# absent, fall back to building a PINNED version from source via FetchContent.
# FIND_PACKAGE_ARGS makes FetchContent_MakeAvailable try find_package first,
# so the fast path (installed lib) and the reproducible path (pinned source)
# share one code path.
include(FetchContent)

# Be a good citizen: don't let our pinned deps drag in their own test/example
# trees or get held to OUR warnings-as-errors.
set(HWY_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_CONTRIB OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

# Google Highway — portable SIMD with runtime dispatch.
FetchContent_Declare(highway
  GIT_REPOSITORY https://github.com/google/highway.git
  GIT_TAG 1.4.0
  FIND_PACKAGE_ARGS NAMES hwy CONFIG)

# GoogleTest.
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  FIND_PACKAGE_ARGS NAMES GTest CONFIG)

# Google Benchmark.
FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.5
  FIND_PACKAGE_ARGS NAMES benchmark CONFIG)

FetchContent_MakeAvailable(highway googletest benchmark)

# Normalize the from-source Highway target. find_package(hwy CONFIG) exports an
# IMPORTED target hwy::hwy whose include dirs CMake already treats as SYSTEM, so
# our -Werror never polices Highway's headers on that path. The FetchContent
# fallback (used on CI when the system package has no usable CMake config)
# instead defines a bare `hwy` target and consumes its headers as NORMAL
# includes, which makes our strict warnings (-Wpedantic on __int128, -Wshadow in
# arm_neon-inl.h) fire *inside* Highway and fail the build. Fix both gaps on the
# from-source path only: alias hwy -> hwy::hwy, and mark Highway's interface
# includes SYSTEM so third-party headers are exempt from our warnings-as-errors.
if(TARGET hwy AND NOT TARGET hwy::hwy)
  add_library(hwy::hwy ALIAS hwy)
  get_target_property(_hwy_include_dirs hwy INTERFACE_INCLUDE_DIRECTORIES)
  if(_hwy_include_dirs)
    set_target_properties(hwy PROPERTIES
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_hwy_include_dirs}")
  endif()
endif()
