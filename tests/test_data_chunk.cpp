#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "strata/data/data_chunk.hpp"

namespace strata {
namespace {

TEST(DataChunk, InitializeCreatesTypedColumns) {
  const std::array<TypeId, 3> types{TypeId::kInt32, TypeId::kDouble, TypeId::kVarchar};
  DataChunk chunk;
  chunk.Initialize(types);
  EXPECT_EQ(chunk.ColumnCount(), 3u);
  EXPECT_EQ(chunk.size(), 0u);
  EXPECT_EQ(chunk.capacity(), kVectorSize);
  EXPECT_EQ(chunk.column(0).type(), TypeId::kInt32);
  EXPECT_EQ(chunk.column(1).type(), TypeId::kDouble);
  EXPECT_EQ(chunk.column(2).type(), TypeId::kVarchar);
}

TEST(DataChunk, FillAndReadAcrossColumns) {
  const std::array<TypeId, 2> types{TypeId::kInt32, TypeId::kVarchar};
  DataChunk chunk;
  chunk.Initialize(types);

  const std::size_t n = 100;
  Vector& ids = chunk.column(0);
  Vector& names = chunk.column(1);
  for (std::size_t i = 0; i < n; ++i) {
    ids.Set<std::int32_t>(i, static_cast<std::int32_t>(i));
    names.Set<StringRef>(i, names.AddString("row-" + std::to_string(i)));
  }
  chunk.SetSize(n);

  EXPECT_EQ(chunk.size(), n);
  EXPECT_EQ(ids.Get<std::int32_t>(50), 50);
  EXPECT_EQ(names.Get<StringRef>(50).view(), std::string_view{"row-50"});
}

TEST(DataChunk, ResetClearsSizeAndValidity) {
  const std::array<TypeId, 1> types{TypeId::kInt32};
  DataChunk chunk;
  chunk.Initialize(types);
  chunk.column(0).SetNull(0);
  chunk.SetSize(10);
  EXPECT_FALSE(chunk.column(0).validity().AllValid());

  chunk.Reset();
  EXPECT_EQ(chunk.size(), 0u);
  EXPECT_TRUE(chunk.column(0).validity().AllValid());  // back to the fast path
  EXPECT_EQ(chunk.column(0).kind(), VectorKind::kFlat);
}

}  // namespace
}  // namespace strata
