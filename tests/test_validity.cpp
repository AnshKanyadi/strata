#include <gtest/gtest.h>

#include <cstddef>

#include "strata/data/validity.hpp"

namespace strata {
namespace {

TEST(Validity, FreshIsAllValidAndUnallocated) {
  Validity v(2048);
  EXPECT_TRUE(v.AllValid());
  EXPECT_EQ(v.data(), nullptr);  // the fast path: zero allocation
  for (std::size_t i = 0; i < 2048u; ++i) EXPECT_TRUE(v.RowIsValid(i));
  EXPECT_EQ(v.CountValid(2048u), 2048u);  // all valid without ever touching a mask
}

TEST(Validity, SetInvalidAllocatesAndLeavesFastPath) {
  Validity v(2048);
  v.SetInvalid(100);
  EXPECT_FALSE(v.AllValid());
  EXPECT_NE(v.data(), nullptr);
  EXPECT_FALSE(v.RowIsValid(100));
  EXPECT_TRUE(v.RowIsValid(99));
  EXPECT_TRUE(v.RowIsValid(101));
  EXPECT_EQ(v.CountValid(2048u), 2047u);
}

TEST(Validity, AllNull) {
  Validity v(128);
  for (std::size_t i = 0; i < 128u; ++i) v.SetInvalid(i);
  EXPECT_EQ(v.CountValid(128u), 0u);
  for (std::size_t i = 0; i < 128u; ++i) EXPECT_FALSE(v.RowIsValid(i));
}

TEST(Validity, MixedAcrossWordBoundaries) {
  Validity v(200);
  std::size_t invalid = 0;
  for (std::size_t i = 0; i < 200u; ++i) {
    if (i % 3 == 0) {
      v.SetInvalid(i);
      ++invalid;
    }
  }
  EXPECT_EQ(v.CountValid(200u), 200u - invalid);
  for (std::size_t i = 0; i < 200u; ++i) EXPECT_EQ(v.RowIsValid(i), (i % 3 != 0));
}

TEST(Validity, CountValidRespectsPartialFinalWord) {
  Validity v(2048);
  v.SetInvalid(0);
  EXPECT_EQ(v.CountValid(1u), 0u);    // only bit 0, which is null
  EXPECT_EQ(v.CountValid(10u), 9u);   // partial word, includes the null
  EXPECT_EQ(v.CountValid(64u), 63u);  // exactly one full word
  EXPECT_EQ(v.CountValid(65u), 64u);  // one full word + one bit
}

TEST(Validity, SetValidAfterInvalidDoesNotReTightenAllValid) {
  Validity v(128);
  v.SetInvalid(5);
  EXPECT_FALSE(v.RowIsValid(5));
  v.SetValid(5);
  EXPECT_TRUE(v.RowIsValid(5));
  EXPECT_EQ(v.CountValid(128u), 128u);
  // Documented behavior: once a mask is allocated, AllValid() stays false.
  EXPECT_FALSE(v.AllValid());
}

TEST(Validity, ResetReturnsToFastPath) {
  Validity v(128);
  v.SetInvalid(5);
  v.Reset();
  EXPECT_TRUE(v.AllValid());
  EXPECT_EQ(v.data(), nullptr);
  EXPECT_TRUE(v.RowIsValid(5));
}

}  // namespace
}  // namespace strata
