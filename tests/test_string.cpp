#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "strata/data/string_heap.hpp"
#include "strata/data/string_ref.hpp"

namespace strata {
namespace {

TEST(StringRef, LayoutInvariants) {
  static_assert(sizeof(StringRef) == 16);
  static_assert(std::is_trivially_copyable_v<StringRef>);
  EXPECT_EQ(sizeof(StringRef), 16u);
}

TEST(StringHeap, EmptyStringIsInlinedAndAllocatesNothing) {
  StringHeap heap;
  const StringRef s = heap.Add("");
  EXPECT_TRUE(s.IsInlined());
  EXPECT_TRUE(s.empty());
  EXPECT_EQ(s.size(), 0u);
  EXPECT_EQ(s.view(), std::string_view{""});
  EXPECT_EQ(heap.AllocatedBytes(), 0u);
}

TEST(StringHeap, ShortStringIsInlined) {
  StringHeap heap;
  const StringRef s = heap.Add("hello");
  EXPECT_TRUE(s.IsInlined());
  EXPECT_EQ(s.view(), std::string_view{"hello"});
  EXPECT_EQ(heap.AllocatedBytes(), 0u);  // inlined: no heap byte used
}

TEST(StringHeap, TwelveByteBoundaryIsInlined) {
  StringHeap heap;
  const StringRef s = heap.Add("123456789012");  // exactly kInlineLength
  EXPECT_EQ(s.size(), 12u);
  EXPECT_TRUE(s.IsInlined());
  EXPECT_EQ(s.view(), std::string_view{"123456789012"});
  EXPECT_EQ(heap.AllocatedBytes(), 0u);
}

TEST(StringHeap, ThirteenBytesGoesToHeap) {
  StringHeap heap;
  const StringRef s = heap.Add("1234567890123");  // one past the inline cutoff
  EXPECT_EQ(s.size(), 13u);
  EXPECT_FALSE(s.IsInlined());
  EXPECT_EQ(s.view(), std::string_view{"1234567890123"});
  EXPECT_EQ(heap.AllocatedBytes(), 13u);
}

TEST(StringHeap, LongStringRoundTrips) {
  StringHeap heap;
  std::string big(1000, 'x');
  big[500] = 'y';
  const StringRef s = heap.Add(big);
  EXPECT_FALSE(s.IsInlined());
  EXPECT_EQ(s.size(), 1000u);
  EXPECT_EQ(s.view(), std::string_view{big});
}

TEST(StringRef, EqualityShortCircuits) {
  StringHeap h;
  // Inlined cases.
  EXPECT_EQ(h.Add("abc"), h.Add("abc"));
  EXPECT_NE(h.Add("abc"), h.Add("abd"));   // same length, last byte differs
  EXPECT_NE(h.Add("abc"), h.Add("abcd"));  // different length

  // Long strings: equal, tail-differ (prefix matches), and prefix-differ.
  const std::string a(50, 'a');
  std::string tail_diff = a;
  tail_diff[49] = 'b';
  std::string prefix_diff = a;
  prefix_diff[0] = 'z';
  EXPECT_EQ(h.Add(a), h.Add(std::string(50, 'a')));
  EXPECT_NE(h.Add(a), h.Add(tail_diff));    // exercises the full-bytes compare
  EXPECT_NE(h.Add(a), h.Add(prefix_diff));  // rejected on the 4-byte prefix
}

TEST(StringHeap, PointersStayValidAcrossManyBlockAllocations) {
  StringHeap heap;
  std::vector<StringRef> refs;
  std::vector<std::string> originals;
  for (int i = 0; i < 4000; ++i) {
    std::string v = "string-number-" + std::to_string(i);  // always > 12 bytes
    originals.push_back(v);
    refs.push_back(heap.Add(v));
  }
  // The earliest refs must remain intact after thousands of later allocations
  // forced many new blocks — i.e. committed bytes never moved.
  for (std::size_t i = 0; i < refs.size(); ++i) {
    EXPECT_EQ(refs[i].view(), std::string_view{originals[i]});
  }
}

TEST(StringHeap, OversizedStringGetsItsOwnBlock) {
  StringHeap heap;
  const std::string huge(10000, 'q');  // larger than kBlockSize (4096)
  const StringRef s = heap.Add(huge);
  EXPECT_EQ(s.view(), std::string_view{huge});
  // A normal add after the oversized one must still work.
  const std::string small(20, 'r');
  const StringRef t = heap.Add(small);
  EXPECT_EQ(t.view(), std::string_view{small});
}

}  // namespace
}  // namespace strata
