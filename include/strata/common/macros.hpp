#pragma once

#include <expected>
#include <utility>

// STRATA_TRY: the "?"-operator for Result<T>. Evaluates a Result-returning
// expression once; on failure it returns std::unexpected(error) from the
// enclosing function; on success it binds the unwrapped value to `decl`.
//
//   Result<int> parse_two(std::string_view a, std::string_view b) {
//     STRATA_TRY(int x, parse(a));   // returns early if parse(a) failed
//     STRATA_TRY(int y, parse(b));
//     return x + y;
//   }
//
// The __LINE__-suffixed temporary avoids shadowing across nested uses. We move
// out of both the error and the value so the macro is efficient for move-only
// payloads.

#define STRATA_DETAIL_CONCAT_(a, b) a##b
#define STRATA_DETAIL_CONCAT(a, b) STRATA_DETAIL_CONCAT_(a, b)
#define STRATA_DETAIL_TMP STRATA_DETAIL_CONCAT(strata_try_tmp_, __LINE__)

#define STRATA_TRY(decl, expr)                                            \
  auto STRATA_DETAIL_TMP = (expr);                                        \
  if (!STRATA_DETAIL_TMP.has_value()) {                                   \
    return std::unexpected(std::move(STRATA_DETAIL_TMP).error());         \
  }                                                                       \
  decl = *std::move(STRATA_DETAIL_TMP)

// STRATA_RETURN_IF_ERROR: same, for Status-returning calls where there is no
// value to bind.
#define STRATA_RETURN_IF_ERROR(expr)                                      \
  do {                                                                    \
    auto STRATA_DETAIL_TMP = (expr);                                      \
    if (!STRATA_DETAIL_TMP.has_value()) {                                 \
      return std::unexpected(std::move(STRATA_DETAIL_TMP).error());       \
    }                                                                     \
  } while (false)
