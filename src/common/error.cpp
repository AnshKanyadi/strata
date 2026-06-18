#include "strata/common/error.hpp"

namespace strata {

const char* ToString(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::kInvalidArgument: return "InvalidArgument";
    case ErrorCode::kNotImplemented:  return "NotImplemented";
    case ErrorCode::kIoError:         return "IoError";
    case ErrorCode::kParseError:      return "ParseError";
    case ErrorCode::kTypeMismatch:    return "TypeMismatch";
    case ErrorCode::kOutOfRange:      return "OutOfRange";
    case ErrorCode::kOutOfMemory:     return "OutOfMemory";
    case ErrorCode::kInternal:        return "Internal";
  }
  return "Unknown"; // unreachable for valid enum values; satisfies the compiler.
}

} // namespace strata
