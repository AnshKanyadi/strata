#pragma once

#include <istream>
#include <string>
#include <utility>

#include "strata/common/result.hpp"
#include "strata/storage/columnar_table.hpp"

namespace strata {

// Options for the delimited loader.
struct LoadOptions {
  char delimiter = ',';
  bool trailing_delimiter = false;  // TPC-H .tbl ends every line with the delimiter
  bool has_header = false;          // skip the first line
  std::string null_token = {};      // a field equal to this becomes NULL (default "" => empty is NULL)
};

inline LoadOptions CsvOptions() { return LoadOptions{}; }
inline LoadOptions TblOptions() {
  LoadOptions o;
  o.delimiter = '|';
  o.trailing_delimiter = true;
  return o;
}

// Parse a delimited stream into a ColumnarTable using the caller-supplied schema.
//
// IMPORTANT (see ADR 0007 / docs/LIMITATIONS.md): this is a SIMPLE delimited
// parser, NOT an RFC-4180 CSV parser — it does not handle quoted fields,
// delimiters/newlines embedded in a field, or escape sequences. That is correct
// for TPC-H .tbl data (which contains none) and adequate for simple CSV. Numeric
// parsing uses std::from_chars (locale-independent, rejects trailing garbage);
// DATE is parsed from YYYY-MM-DD into int32 epoch-days. A field equal to
// `null_token` (default the empty string) becomes NULL. Errors (I/O, parse,
// wrong field count) are returned via Result — this is the setup boundary.
Result<ColumnarTable> LoadDelimited(std::istream& in, Schema schema,
                                    const LoadOptions& opts = {});

Result<ColumnarTable> LoadDelimitedFile(const std::string& path, Schema schema,
                                        const LoadOptions& opts = {});

}  // namespace strata
