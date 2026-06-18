#include "strata/data/types.hpp"

namespace strata {

const char* TypeName(TypeId type) noexcept {
  switch (type) {
    case TypeId::kBool:    return "BOOL";
    case TypeId::kInt32:   return "INT32";
    case TypeId::kInt64:   return "INT64";
    case TypeId::kDouble:  return "DOUBLE";
    case TypeId::kDate:    return "DATE";
    case TypeId::kVarchar: return "VARCHAR";
  }
  return "UNKNOWN";
}

}  // namespace strata
