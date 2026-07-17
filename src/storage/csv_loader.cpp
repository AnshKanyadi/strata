#include "strata/storage/csv_loader.hpp"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "strata/common/error.hpp"
#include "strata/data/string_ref.hpp"
#include "strata/data/vector.hpp"
#include "strata/util/date.hpp"

namespace strata {
namespace {

// Parse an entire field, rejecting trailing garbage ("12x" fails).
//
// Integers go through std::from_chars. Floating point does NOT: libc++'s
// floating-point std::from_chars carries an OS-availability annotation (it is
// marked unavailable on the macOS versions the CI runner targets), so using it
// fails to compile there. We parse doubles via std::strtod on a null-terminated
// copy instead. Fields are short numeric tokens; we assume the C locale's '.'
// decimal separator, which holds for our data and is never changed at runtime.
template <class T>
bool ParseExact(std::string_view f, T& out) {
  if constexpr (std::is_floating_point_v<T>) {
    if (f.empty()) return false;
    char buf[64];
    if (f.size() >= sizeof(buf)) return false;  // no realistic numeric token is this long
    std::memcpy(buf, f.data(), f.size());
    buf[f.size()] = '\0';
    char* parse_end = nullptr;
    errno = 0;
    const double value = std::strtod(buf, &parse_end);
    if (parse_end != buf + f.size() || errno == ERANGE) return false;  // partial parse / overflow
    out = static_cast<T>(value);
    return true;
  } else {
    const char* const end = f.data() + f.size();
    const auto [ptr, ec] = std::from_chars(f.data(), end, out);
    return ec == std::errc{} && ptr == end;
  }
}

Result<std::int32_t> ParseDate(std::string_view f) {
  const auto dash1 = f.find('-');
  const auto dash2 = (dash1 == std::string_view::npos) ? dash1 : f.find('-', dash1 + 1);
  if (dash1 == std::string_view::npos || dash2 == std::string_view::npos) {
    return Err(ErrorCode::kParseError, "expected YYYY-MM-DD");
  }
  int y = 0, m = 0, d = 0;
  if (!ParseExact(f.substr(0, dash1), y) || !ParseExact(f.substr(dash1 + 1, dash2 - dash1 - 1), m) ||
      !ParseExact(f.substr(dash2 + 1), d)) {
    return Err(ErrorCode::kParseError, "non-numeric date component");
  }
  if (m < 1 || m > 12 || d < 1 || d > 31) {
    return Err(ErrorCode::kParseError, "date field out of range");
  }
  return DaysFromCivil(y, m, d);
}

bool ParseBool(std::string_view f, std::uint8_t& out) {
  if (f == "1" || f == "t" || f == "T" || f == "true" || f == "TRUE") {
    out = 1;
    return true;
  }
  if (f == "0" || f == "f" || f == "F" || f == "false" || f == "FALSE") {
    out = 0;
    return true;
  }
  return false;
}

Status ParseField(std::string_view f, TypeId type, Vector& col, std::size_t row) {
  switch (type) {
    case TypeId::kBool: {
      std::uint8_t b = 0;
      if (!ParseBool(f, b)) return Err(ErrorCode::kParseError, "invalid BOOL");
      col.Set<std::uint8_t>(row, b);
      return Ok();
    }
    case TypeId::kInt32: {
      std::int32_t v = 0;
      if (!ParseExact(f, v)) return Err(ErrorCode::kParseError, "invalid INT32");
      col.Set<std::int32_t>(row, v);
      return Ok();
    }
    case TypeId::kInt64: {
      std::int64_t v = 0;
      if (!ParseExact(f, v)) return Err(ErrorCode::kParseError, "invalid INT64");
      col.Set<std::int64_t>(row, v);
      return Ok();
    }
    case TypeId::kDouble: {
      double v = 0;
      if (!ParseExact(f, v)) return Err(ErrorCode::kParseError, "invalid DOUBLE");
      col.Set<double>(row, v);
      return Ok();
    }
    case TypeId::kDate: {
      Result<std::int32_t> r = ParseDate(f);
      if (!r) return std::unexpected(std::move(r).error());
      col.Set<std::int32_t>(row, *r);
      return Ok();
    }
    case TypeId::kVarchar: {
      col.Set<StringRef>(row, col.AddString(f));
      return Ok();
    }
  }
  return Err(ErrorCode::kInternal, "unknown TypeId");  // unreachable; satisfies -Wreturn-type
}

// Split a line on `delim` into field views. A single trailing empty field (the
// artifact of TPC-H .tbl's terminating '|') is dropped when `trailing` is set.
void SplitLine(std::string_view line, char delim, bool trailing,
               std::vector<std::string_view>& out) {
  out.clear();
  std::size_t start = 0;
  for (std::size_t i = 0; i <= line.size(); ++i) {
    if (i == line.size() || line[i] == delim) {
      out.emplace_back(line.data() + start, i - start);
      start = i + 1;
    }
  }
  if (trailing && !out.empty() && out.back().empty()) out.pop_back();
}

}  // namespace

Result<ColumnarTable> LoadDelimited(std::istream& in, Schema schema, const LoadOptions& opts) {
  ColumnarTable table(std::move(schema));
  const std::span<const TypeId> types = table.column_types();
  const std::size_t ncols = types.size();

  DataChunk chunk;
  chunk.Initialize(types, kVectorSize);
  std::size_t row = 0;

  auto flush = [&]() {
    if (row == 0) return;
    chunk.SetSize(row);
    table.AppendChunk(std::move(chunk));
    chunk.Initialize(types, kVectorSize);  // reuse the moved-from chunk
    row = 0;
  };

  std::vector<std::string_view> fields;
  fields.reserve(ncols + 1);
  std::string line;
  std::size_t line_no = 0;
  bool header_pending = opts.has_header;

  while (std::getline(in, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.pop_back();  // tolerate CRLF
    if (header_pending) {
      header_pending = false;
      continue;
    }
    if (line.empty()) continue;

    SplitLine(line, opts.delimiter, opts.trailing_delimiter, fields);
    if (fields.size() != ncols) {
      return Err(ErrorCode::kParseError, "line " + std::to_string(line_no) + ": expected " +
                                             std::to_string(ncols) + " fields, got " +
                                             std::to_string(fields.size()));
    }
    for (std::size_t c = 0; c < ncols; ++c) {
      const std::string_view fld = fields[c];
      if (fld == opts.null_token) {
        chunk.column(c).SetNull(row);
        continue;
      }
      const Status st = ParseField(fld, types[c], chunk.column(c), row);
      if (!st) {
        return Err(st.error().code, "line " + std::to_string(line_no) + ", col " +
                                        std::to_string(c + 1) + ": " + st.error().message);
      }
    }
    ++row;
    if (row == kVectorSize) flush();
  }
  flush();
  return table;
}

Result<ColumnarTable> LoadDelimitedFile(const std::string& path, Schema schema,
                                        const LoadOptions& opts) {
  std::ifstream in(path);
  if (!in) return Err(ErrorCode::kIoError, "cannot open file: " + path);
  return LoadDelimited(in, std::move(schema), opts);
}

}  // namespace strata
