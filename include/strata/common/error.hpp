#pragma once

#include <string>
#include <utility>

namespace strata {

// A coarse classification of failures. The kind is for programmatic handling;
// the human-readable detail lives in Error::message.
enum class ErrorCode {
  kInvalidArgument,
  kNotImplemented,
  kIoError,
  kParseError,
  kTypeMismatch,
  kOutOfRange,
  kOutOfMemory,
  kInternal,
};

// Stable, allocation-free name for an ErrorCode (defined in error.cpp).
const char* ToString(ErrorCode code) noexcept;

// The error half of Result<T> (see result.hpp). Deliberately a plain value
// type: it carries a code plus an owned message, copies/moves cheaply, and
// crucially does NOT use C++ exceptions — errors propagate as return values so
// the per-batch hot path never pays for exception machinery. Exceptions remain
// acceptable at the query/setup boundary (parsing, planning, allocation).
struct Error {
  ErrorCode code = ErrorCode::kInternal;
  std::string message;

  Error() = default;
  Error(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}
};

} // namespace strata
