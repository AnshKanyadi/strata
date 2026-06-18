#include <gtest/gtest.h>

#include <cstddef>

#include "strata/data/selection_vector.hpp"

namespace strata {
namespace {

TEST(SelectionVector, IdentityByDefault) {
  SelectionVector s;
  EXPECT_TRUE(s.IsIdentity());
  EXPECT_EQ(s.data(), nullptr);
  for (std::size_t i = 0; i < 100u; ++i) EXPECT_EQ(s.Get(i), static_cast<sel_t>(i));
}

TEST(SelectionVector, OwnedSetGet) {
  SelectionVector s(4);
  EXPECT_FALSE(s.IsIdentity());
  EXPECT_EQ(s.capacity(), 4u);
  s.Set(0, 10);
  s.Set(1, 20);
  s.Set(2, 30);
  s.Set(3, 40);
  EXPECT_EQ(s.Get(0), 10u);
  EXPECT_EQ(s.Get(3), 40u);
}

TEST(SelectionVector, SliceOfIdentity) {
  SelectionVector id;
  const SelectionVector sliced = id.Slice(5, 3);  // -> {5, 6, 7}
  EXPECT_FALSE(sliced.IsIdentity());
  EXPECT_EQ(sliced.Get(0), 5u);
  EXPECT_EQ(sliced.Get(1), 6u);
  EXPECT_EQ(sliced.Get(2), 7u);
}

TEST(SelectionVector, SliceOfOwned) {
  SelectionVector s(5);
  for (sel_t i = 0; i < 5u; ++i) s.Set(i, i * 2u);  // {0,2,4,6,8}
  const SelectionVector sliced = s.Slice(1, 3);     // -> {2,4,6}
  EXPECT_EQ(sliced.Get(0), 2u);
  EXPECT_EQ(sliced.Get(1), 4u);
  EXPECT_EQ(sliced.Get(2), 6u);
}

TEST(SelectionVector, ComposeCascadesSelections) {
  // outer selects physical rows {100,101,102,103}; inner picks logical {3,1}.
  SelectionVector outer(4);
  for (sel_t i = 0; i < 4u; ++i) outer.Set(i, 100u + i);
  SelectionVector inner(2);
  inner.Set(0, 3);
  inner.Set(1, 1);
  const SelectionVector composed = outer.Compose(inner, 2);  // -> {103,101}
  EXPECT_EQ(composed.Get(0), 103u);
  EXPECT_EQ(composed.Get(1), 101u);
}

TEST(SelectionVector, ComposeOnIdentityYieldsInner) {
  SelectionVector id;
  SelectionVector inner(3);
  inner.Set(0, 7);
  inner.Set(1, 4);
  inner.Set(2, 9);
  const SelectionVector composed = id.Compose(inner, 3);
  EXPECT_EQ(composed.Get(0), 7u);
  EXPECT_EQ(composed.Get(1), 4u);
  EXPECT_EQ(composed.Get(2), 9u);
}

}  // namespace
}  // namespace strata
