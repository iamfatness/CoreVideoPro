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
