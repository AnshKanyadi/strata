add_executable(strata_bench bench_smoke.cpp)

# benchmark_main transitively links benchmark::benchmark; listing both makes the
# Apple linker warn about a duplicate archive, so we link only _main here.
target_link_libraries(strata_bench
  PRIVATE strata_core strata_warnings benchmark::benchmark_main)
