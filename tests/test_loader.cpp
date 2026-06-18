#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

#include "strata/common/error.hpp"
#include "strata/storage/csv_loader.hpp"
#include "strata/util/date.hpp"

namespace strata {
namespace {

Schema MakeSchema() {
  return Schema({{"id", TypeId::kInt32},
                 {"name", TypeId::kVarchar},
                 {"price", TypeId::kDouble},
                 {"d", TypeId::kDate}});
}

TEST(Loader, BasicCsv) {
  std::istringstream in("1,apple,1.50,2021-01-01\n2,banana,0.25,2021-06-15\n");
  auto r = LoadDelimited(in, MakeSchema(), CsvOptions());
  ASSERT_TRUE(r.has_value()) << (r ? std::string{} : r.error().message);
  const ColumnarTable& t = *r;
  EXPECT_EQ(t.row_count(), 2u);
  EXPECT_EQ(t.chunk_count(), 1u);
  ASSERT_EQ(t.schema().size(), 4u);
  const DataChunk& c = t.chunk(0);
  EXPECT_EQ(c.column(0).Get<std::int32_t>(0), 1);
  EXPECT_EQ(c.column(1).Get<StringRef>(0).view(), std::string_view{"apple"});
  EXPECT_DOUBLE_EQ(c.column(2).Get<double>(1), 0.25);
  EXPECT_EQ(c.column(3).Get<std::int32_t>(0), DaysFromCivil(2021, 1, 1));
  EXPECT_EQ(c.column(3).Get<std::int32_t>(1), DaysFromCivil(2021, 6, 15));
}

TEST(Loader, EmptyFieldsBecomeNull) {
  std::istringstream in("1,,,\n");  // id present; name/price/date empty => NULL
  auto r = LoadDelimited(in, MakeSchema(), CsvOptions());
  ASSERT_TRUE(r.has_value());
  const DataChunk& c = r->chunk(0);
  EXPECT_FALSE(c.column(0).IsNull(0));
  EXPECT_TRUE(c.column(1).IsNull(0));
  EXPECT_TRUE(c.column(2).IsNull(0));
  EXPECT_TRUE(c.column(3).IsNull(0));
}

TEST(Loader, TblDelimiterAndTrailingDelimiter) {
  Schema s({{"a", TypeId::kInt32}, {"b", TypeId::kVarchar}});
  std::istringstream in("1|hello|\n2|world|\n");  // pipe-delimited, trailing '|'
  auto r = LoadDelimited(in, s, TblOptions());
  ASSERT_TRUE(r.has_value()) << (r ? std::string{} : r.error().message);
  EXPECT_EQ(r->row_count(), 2u);
  EXPECT_EQ(r->chunk(0).column(1).Get<StringRef>(1).view(), std::string_view{"world"});
}

TEST(Loader, HeaderRowSkipped) {
  Schema s({{"a", TypeId::kInt32}});
  std::istringstream in("a\n10\n20\n");
  auto r = LoadDelimited(in, s, LoadOptions{.has_header = true});
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->row_count(), 2u);
  EXPECT_EQ(r->chunk(0).column(0).Get<std::int32_t>(0), 10);
}

TEST(Loader, WrongFieldCountIsParseError) {
  Schema s({{"a", TypeId::kInt32}, {"b", TypeId::kInt32}});
  std::istringstream in("1,2\n3\n");  // second row has one field
  auto r = LoadDelimited(in, s, CsvOptions());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kParseError);
}

TEST(Loader, TrailingGarbageInNumberIsParseError) {
  Schema s({{"a", TypeId::kInt32}});
  std::istringstream in("12x\n");
  auto r = LoadDelimited(in, s, CsvOptions());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kParseError);
}

TEST(Loader, BadDateIsParseError) {
  Schema s({{"d", TypeId::kDate}});
  std::istringstream in("2021-13-01\n");  // month 13
  auto r = LoadDelimited(in, s, CsvOptions());
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kParseError);
}

TEST(Loader, MultipleChunksOverVectorSize) {
  Schema s({{"a", TypeId::kInt32}});
  std::ostringstream gen;
  const std::size_t rows = kVectorSize * 2 + 5;  // forces three chunks (2048,2048,5)
  for (std::size_t i = 0; i < rows; ++i) gen << i << "\n";
  std::istringstream in(gen.str());
  auto r = LoadDelimited(in, s, CsvOptions());
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->row_count(), rows);
  EXPECT_EQ(r->chunk_count(), 3u);
}

}  // namespace
}  // namespace strata
