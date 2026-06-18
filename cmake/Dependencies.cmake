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

# Normalize: whether highway came from find_package (hwy::hwy) or from source,
# the rest of the build references hwy::hwy. The source build also defines it,
# so no aliasing is needed; this comment just records the invariant.
