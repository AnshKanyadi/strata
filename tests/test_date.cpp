#include <gtest/gtest.h>

#include <cstdint>

#include "strata/util/date.hpp"

namespace strata {
namespace {

TEST(Date, EpochIsDayZero) { EXPECT_EQ(DaysFromCivil(1970, 1, 1), 0); }

TEST(Date, KnownDays) {
  EXPECT_EQ(DaysFromCivil(1970, 1, 2), 1);
  EXPECT_EQ(DaysFromCivil(1969, 12, 31), -1);
  EXPECT_EQ(DaysFromCivil(2000, 1, 1), 10957);  // 30y*365 + 7 leap days
  EXPECT_EQ(DaysFromCivil(2021, 1, 1), 18628);
}

TEST(Date, RoundTripsBothWays) {
  for (std::int32_t days = -40000; days <= 40000; days += 1) {
    const Civil c = CivilFromDays(days);
    EXPECT_EQ(DaysFromCivil(c.year, c.month, c.day), days);
    EXPECT_GE(c.month, 1);
    EXPECT_LE(c.month, 12);
    EXPECT_GE(c.day, 1);
    EXPECT_LE(c.day, 31);
  }
}

}  // namespace
}  // namespace strata
