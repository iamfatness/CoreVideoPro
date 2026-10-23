#include "modules/DeliveryCounterPattern.h"
#include <gtest/gtest.h>
#include <vector>

TEST(DeliveryCounterPattern, DecodesIdentityAndRejectsTearsAndShortBuffers) {
  constexpr int width = 1920, height = 1080, stride = 2048;
  std::vector<uint8_t> image(stride * height, 128);
  constexpr uint32_t identity = 0x81234567;
  const int cellWidth = (width / corevideo::modules::kDeliveryCounterCells) & ~1;
  for (int cell = 0; cell < corevideo::modules::kDeliveryCounterCells; ++cell) {
    const int x = cell * cellWidth + cellWidth / 2;
    const bool bit = corevideo::modules::deliveryCounterBit(identity, cell);
    image[16 * stride + x] = bit ? 235 : 16;
    image[(height - 16) * stride + x] = bit ? 16 : 235;
  }
  const auto decode = [&] { return corevideo::modules::decodeDeliveryCounter(image.data(), image.size(), width, height, stride); };
  ASSERT_TRUE(decode().has_value());
  EXPECT_EQ(*decode(), identity);
  EXPECT_FALSE(corevideo::modules::decodeDeliveryCounter(image.data(), 10, width, height, stride).has_value());
  image[(height - 16) * stride + 4 * cellWidth + cellWidth / 2] = image[16 * stride + 4 * cellWidth + cellWidth / 2];
  EXPECT_FALSE(decode().has_value());
}

TEST(DeliveryCounterPattern, PaintedBandsPreservePictureChromaAndStridePadding) {
  constexpr int width = 1920, height = 1080, stride = 2048;
  std::vector<uint8_t> image(stride * height * 3 / 2, 128);
  ASSERT_TRUE(corevideo::modules::paintDeliveryCounter(image.data(), image.size(), width, height, stride, 0xfedcba98));
  EXPECT_EQ(*corevideo::modules::decodeDeliveryCounter(image.data(), image.size(), width, height, stride), 0xfedcba98u);
  for (int row = 32; row < height - 32; ++row)
    for (int x = 0; x < stride; ++x) EXPECT_EQ(image[row * stride + x], 128);
  for (int row = 0; row < height; ++row)
    for (int x = width; x < stride; ++x) EXPECT_EQ(image[row * stride + x], 128);
  for (size_t i = stride * height; i < image.size(); ++i) EXPECT_EQ(image[i], 128);
  const auto before = image;
  EXPECT_FALSE(corevideo::modules::paintDeliveryCounter(image.data(), 10, width, height, stride, 1));
  EXPECT_EQ(image, before);
}

TEST(DeliveryCounterPattern, SourceInteriorBitsSurviveIndependentProgramCounterAndRejectShortGeometry) {
  constexpr int width = 1920, height = 1080, stride = 2048;
  std::vector<uint8_t> image(stride * height * 3 / 2, 128);
  using namespace corevideo::modules;
  ASSERT_TRUE(paintDeliveryCounter(image.data(), image.size(), width, height, stride, 75, true));
  ASSERT_TRUE(paintDeliveryCounter(image.data(), image.size(), width, height, stride, 1000));
  EXPECT_EQ(*decodeDeliveryCounter(image.data(), image.size(), width, height, stride), 1000u);
  const int cellWidth = (width / kDeliveryCounterCells) & ~1;
  for (int cell = 0; cell < kDeliveryCounterCells; ++cell) {
    const int x = cell * cellWidth + cellWidth / 2;
    EXPECT_EQ(image[(height / 3 + 16) * stride + x], deliveryCounterBit(75, cell) ? 235 : 16);
    EXPECT_EQ(image[(height * 2 / 3 + 16) * stride + x], deliveryCounterBit(75, cell) ? 16 : 235);
  }
  EXPECT_EQ(image[(height / 2) * stride + width / 2], 128);
  const auto before = image;
  EXPECT_FALSE(paintDeliveryCounter(image.data(), image.size(), width, 64, stride, 1, true));
  EXPECT_EQ(image, before);
}
