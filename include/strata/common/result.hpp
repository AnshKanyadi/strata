#pragma once

#include <expected>
#include <string>
#include <utility>

#include "strata/common/error.hpp"

namespace strata {

// Result<T> is the project-wide fallible-return type. We alias std::expected
// (C++23) rather than rolling our own: it gives us monadic helpers
// (and_then/transform/or_else), value()/error() accessors, and zero-overhead
// success when T fits in a register, with no exceptions in the hot path. See
// docs/adr/0002 for why std::expected (and the hand-rolled fallback we'd use
// on a toolchain that lacked it).
template <class T>
using Result = std::expected<T, Error>;

// Result<void> is valid and means "succeeded, no value".
using Status = std::expected<void, Error>;

// Construct a failure. Returns std::unexpected so it implicitly converts to any
// Result<T>/Status:  `return Err(ErrorCode::kParseError, "bad token");`
[[nodiscard]] inline std::unexpected<Error> Err(ErrorCode code, std::string message) {
  return std::unexpected<Error>(Error{code, std::move(message)});
}

// Construct the success-with-no-value sentinel for Status.
[[nodiscard]] inline Status Ok() noexcept { return Status{}; }

} // namespace strata
