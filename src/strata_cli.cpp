// Strata command-line entry point. For P0 the only job is a preflight that
// prints the build identity and, importantly, the SIMD instruction set Highway
// will dispatch to, plus VECTOR_SIZE. Every benchmark we publish later must be
// reproducible against this output, so it doubles as provenance.
//
// Output uses <iostream> rather than std::print/std::println: libc++'s <print>
// carries an OS-availability annotation and is unavailable on the macOS versions
// the CI runner targets (the same gating that affects std::from_chars). See
// src/storage/csv_loader.cpp.

#include <cstddef>
#include <iostream>
#include <string_view>

#include "strata/config.hpp"
#include "strata/simd/cpu_features.hpp"
#include "strata/version.hpp"

namespace {

void PrintVersion() {
  const strata::simd::SimdInfo simd = strata::simd::DetectSimd();

  std::cout << "Strata " << strata::kVersion << '\n'
            << "  compiler     : " << strata::kCompilerId << ' ' << strata::kCompilerVersion << '\n'
            << "  build type   : " << strata::kBuildType << '\n'
            << "  sanitizer    : " << strata::kSanitizer << '\n'
            << "  VECTOR_SIZE  : " << strata::kVectorSize << '\n'
            << "  SIMD dispatch: " << simd.dispatched_target << '\n';

  std::cout << "  SIMD targets : ";
  for (std::size_t i = 0; i < simd.supported_targets.size(); ++i) {
    std::cout << (i == 0 ? "" : ", ") << simd.supported_targets[i];
  }
  std::cout << '\n';
}

void PrintHelp() {
  std::cout << "Strata - a vectorized columnar query engine (validated against DuckDB)\n"
            << "usage: strata [--version | --help]\n";
}

}  // namespace

int main(int argc, char** argv) {
  const std::string_view arg = (argc > 1) ? argv[1] : "--version";
  if (arg == "--help" || arg == "-h") {
    PrintHelp();
    return 0;
  }
  // Default (and explicit --version) prints the preflight.
  PrintVersion();
  return 0;
}
