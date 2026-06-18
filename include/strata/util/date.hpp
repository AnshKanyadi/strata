#pragma once

#include <cstdint>

namespace strata {

// DATE is stored as an int32 count of days since 1970-01-01 (= day 0). The two
// conversions below are Howard Hinnant's branch-light proleptic-Gregorian
// algorithms (http://howardhinnant.github.io/date_algorithms.html), rewritten
// in signed int32 throughout so they stay clean under -Wconversion/-Wsign-conversion.
//
// Scope/honesty: these implement the proleptic Gregorian calendar arithmetic.
// They do NOT validate that the input is a real calendar date (e.g. month 13 or
// day 40 produce a value rather than an error); the loader validates ranges
// before calling. int32 days comfortably covers years ~ -5,000,000..+5,000,000,
// far beyond any date we handle.

// Days from civil (year, month[1-12], day[1-31]) to the epoch-day count.
constexpr std::int32_t DaysFromCivil(std::int32_t y, std::int32_t m, std::int32_t d) noexcept {
  y -= (m <= 2) ? 1 : 0;
  const std::int32_t era = (y >= 0 ? y : y - 399) / 400;
  const std::int32_t yoe = y - era * 400;                                  // [0, 399]
  const std::int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const std::int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  return era * 146097 + doe - 719468;
}

struct Civil {
  std::int32_t year;
  std::int32_t month;  // 1..12
  std::int32_t day;    // 1..31
};

// Inverse of DaysFromCivil — used to print DATE values back as YYYY-MM-DD.
constexpr Civil CivilFromDays(std::int32_t z) noexcept {
  z += 719468;
  const std::int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::int32_t doe = z - era * 146097;                                  // [0, 146096]
  const std::int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
  const std::int32_t y = yoe + era * 400;
  const std::int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);           // [0, 365]
  const std::int32_t mp = (5 * doy + 2) / 153;                               // [0, 11]
  const std::int32_t d = doy - (153 * mp + 2) / 5 + 1;                       // [1, 31]
  const std::int32_t m = mp + (mp < 10 ? 3 : -9);                            // [1, 12]
  return Civil{y + (m <= 2 ? 1 : 0), m, d};
}

}  // namespace strata
