// Strata command-line entry point. For P0 the only job is a preflight that
// prints the build identity and — importantly — the SIMD instruction set
// Highway will dispatch to, plus VECTOR_SIZE. Every benchmark we publish later
// must be reproducible against this output, so it doubles as provenance.

#include <print>
#include <string_view>

#include "strata/config.hpp"
#include "strata/simd/cpu_features.hpp"
#include "strata/version.hpp"

namespace {

void PrintVersion() {
  const strata::simd::SimdInfo simd = strata::simd::DetectSimd();

  std::println("Strata {}", strata::kVersion);
  std::println("  compiler     : {} {}", strata::kCompilerId, strata::kCompilerVersion);
  std::println("  build type   : {}", strata::kBuildType);
  std::println("  sanitizer    : {}", strata::kSanitizer);
  std::println("  VECTOR_SIZE  : {}", strata::kVectorSize);
  std::println("  SIMD dispatch: {}", simd.dispatched_target);

  std::print("  SIMD targets : ");
  for (std::size_t i = 0; i < simd.supported_targets.size(); ++i) {
    std::print("{}{}", i == 0 ? "" : ", ", simd.supported_targets[i]);
  }
  std::println("");
}

void PrintHelp() {
  std::println("Strata — a vectorized columnar query engine (validated against DuckDB)");
  std::println("usage: strata [--version | --help]");
}

} // namespace

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
