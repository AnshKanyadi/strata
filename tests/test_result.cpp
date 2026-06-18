#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "strata/common/error.hpp"
#include "strata/common/macros.hpp"
#include "strata/common/result.hpp"

namespace strata {
namespace {

Result<int> ParsePositive(int v) {
  if (v < 0) return Err(ErrorCode::kInvalidArgument, "negative");
  return v;
}

TEST(Result, HoldsValueOnSuccess) {
  Result<int> r = ParsePositive(7);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 7);
}

TEST(Result, HoldsErrorOnFailure) {
  Result<int> r = ParsePositive(-1);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(r.error().message, "negative");
}

TEST(Result, ErrorCodeToStringIsStable) {
  EXPECT_STREQ(ToString(ErrorCode::kParseError), "ParseError");
  EXPECT_STREQ(ToString(ErrorCode::kOutOfMemory), "OutOfMemory");
  EXPECT_STREQ(ToString(ErrorCode::kInternal), "Internal");
}

TEST(Result, MonadicTransformChains) {
  Result<int> r = ParsePositive(4).transform([](int x) { return x * 10; });
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 40);
}

// --- STRATA_TRY -----------------------------------------------------------
Result<int> AddTwoPositives(int a, int b) {
  STRATA_TRY(int x, ParsePositive(a)); // returns early if a < 0
  STRATA_TRY(int y, ParsePositive(b));
  return x + y;
}

TEST(StrataTry, UnwrapsOnSuccess) {
  Result<int> r = AddTwoPositives(2, 3);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 5);
}

TEST(StrataTry, PropagatesFirstError) {
  Result<int> r = AddTwoPositives(-9, 3);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, ErrorCode::kInvalidArgument);
}

// --- Status (Result<void>) ------------------------------------------------
Status RequireEven(int v) {
  if (v % 2 != 0) return Err(ErrorCode::kOutOfRange, "odd");
  return Ok();
}

Status RequireBothEven(int a, int b) {
  STRATA_RETURN_IF_ERROR(RequireEven(a));
  STRATA_RETURN_IF_ERROR(RequireEven(b));
  return Ok();
}

TEST(Status, OkAndErrorPropagation) {
  EXPECT_TRUE(RequireBothEven(2, 4).has_value());
  EXPECT_FALSE(RequireBothEven(2, 5).has_value());
}

// Move-only payloads must thread through STRATA_TRY without copies.
Result<std::string> MakeOwned(bool ok) {
  if (!ok) return Err(ErrorCode::kInternal, "no");
  return std::string("payload");
}

Result<std::size_t> LengthOf(bool ok) {
  STRATA_TRY(std::string s, MakeOwned(ok));
  return s.size();
}

TEST(StrataTry, WorksWithMoveOnlyFlow) {
  EXPECT_EQ(*LengthOf(true), 7u);
  EXPECT_FALSE(LengthOf(false).has_value());
}

} // namespace
} // namespace strata
