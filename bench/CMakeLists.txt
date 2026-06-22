add_executable(strata_bench bench_smoke.cpp)

# benchmark_main transitively links benchmark::benchmark; listing both makes the
# Apple linker warn about a duplicate archive, so we link only _main here.
target_link_libraries(strata_bench
  PRIVATE strata_core strata_warnings benchmark::benchmark_main)

# Scalar-vs-SIMD kernel microbenchmarks (P3). Numbers -> docs/BENCHMARKS.md.
add_executable(strata_bench_kernels bench_kernels.cpp)
target_link_libraries(strata_bench_kernels
  PRIVATE strata_core strata_warnings benchmark::benchmark_main)
