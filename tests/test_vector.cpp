#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>

#include "strata/data/vector.hpp"

namespace strata {
namespace {

TEST(Vector, Int32RoundTripAndFastPathPreserved) {
  Vector v(TypeId::kInt32);
  EXPECT_EQ(v.type(), TypeId::kInt32);
  EXPECT_EQ(v.kind(), VectorKind::kFlat);
  EXPECT_EQ(v.capacity(), kVectorSize);

  for (std::size_t i = 0; i < kVectorSize; ++i) {
    v.Set<std::int32_t>(i, static_cast<std::int32_t>(i) - 1000);
  }
  for (std::size_t i = 0; i < kVectorSize; ++i) {
    EXPECT_EQ(v.Get<std::int32_t>(i), static_cast<std::int32_t>(i) - 1000);
  }
  // Writing values (no NULLs) must not have allocated a validity mask.
  EXPECT_TRUE(v.validity().AllValid());
}

TEST(Vector, OtherFixedWidthTypes) {
  Vector d(TypeId::kDouble);
  d.Set<double>(0, 3.5);
  d.Set<double>(1, -2.25);
  EXPECT_DOUBLE_EQ(d.Get<double>(0), 3.5);
  EXPECT_DOUBLE_EQ(d.Get<double>(1), -2.25);

  Vector l(TypeId::kInt64);
  l.Set<std::int64_t>(0, std::int64_t{1} << 40);
  EXPECT_EQ(l.Get<std::int64_t>(0), std::int64_t{1} << 40);

  Vector dt(TypeId::kDate);  // int32 days
  dt.Set<std::int32_t>(0, 19'000);
  EXPECT_EQ(dt.Get<std::int32_t>(0), 19'000);
}

TEST(Vector, BoolStoredAsByte) {
  Vector b(TypeId::kBool);
  EXPECT_EQ(PhysicalSize(TypeId::kBool), 1u);
  b.Set<std::uint8_t>(0, 1);
  b.Set<std::uint8_t>(1, 0);
  EXPECT_EQ(static_cast<int>(b.Get<std::uint8_t>(0)), 1);
  EXPECT_EQ(static_cast<int>(b.Get<std::uint8_t>(1)), 0);
}

TEST(Vector, NullHandling) {
  Vector v(TypeId::kInt32);
  v.Set<std::int32_t>(0, 42);
  EXPECT_FALSE(v.IsNull(0));
  v.SetNull(0);
  EXPECT_TRUE(v.IsNull(0));
  EXPECT_FALSE(v.validity().AllValid());
}

TEST(Vector, ConstantVector) {
  Vector c = Vector::Constant(TypeId::kInt32);
  EXPECT_TRUE(c.IsConstant());
  EXPECT_EQ(c.kind(), VectorKind::kConstant);
  EXPECT_EQ(c.capacity(), 1u);
  c.Set<std::int32_t>(0, 777);
  EXPECT_EQ(c.ConstantValue<std::int32_t>(), 777);
  EXPECT_FALSE(c.ConstantIsNull());
  c.SetNull(0);
  EXPECT_TRUE(c.ConstantIsNull());
}

TEST(Vector, VarcharStoreAndRead) {
  Vector v(TypeId::kVarchar);
  v.Set<StringRef>(0, v.AddString("short"));
  v.Set<StringRef>(1, v.AddString("this is a longer string than twelve bytes"));
  EXPECT_EQ(v.Get<StringRef>(0).view(), std::string_view{"short"});
  EXPECT_EQ(v.Get<StringRef>(1).view(),
            std::string_view{"this is a longer string than twelve bytes"});
}

TEST(Vector, MoveTransfersStorage) {
  Vector v(TypeId::kInt32);
  v.Set<std::int32_t>(0, 5);
  const Vector moved = std::move(v);
  EXPECT_EQ(moved.type(), TypeId::kInt32);
  EXPECT_EQ(moved.Get<std::int32_t>(0), 5);
}

}  // namespace
}  // namespace strata
