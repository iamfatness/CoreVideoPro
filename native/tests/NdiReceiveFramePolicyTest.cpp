#include <gtest/gtest.h>

#include "modules/NdiReceiveFramePolicy.h"

TEST(NdiReceiveFramePolicy, CopiesSdkOwnedStrideAndOpaqueAlphaIntoBusPixels) {
  const uint8_t source[] = {
      1, 2, 3, 0, 4, 5, 6, 0, 99, 99, 99, 99,
      7, 8, 9, 0, 10, 11, 12, 0, 88, 88, 88, 88};
  auto pixels = corevideo::modules::copyNdiBgra(source, 2, 2, 12, true);
  ASSERT_NE(pixels, nullptr);
  EXPECT_EQ(*pixels, (std::vector<uint8_t>{1, 2, 3, 255, 4, 5, 6, 255,
                                           7, 8, 9, 255, 10, 11, 12, 255}));
  EXPECT_FALSE(corevideo::modules::copyNdiBgra(source, 2, 2, 7, false));
  EXPECT_FALSE(corevideo::modules::copyNdiBgra(source, 8192, 8192, 32768, false));
}

TEST(NdiReceiveFramePolicy, InterleavesPlanarPcmBeforeSdkBufferIsFreed) {
  const float source[] = {0.1f, 0.2f, 0.3f, 0.8f, 0.7f, 0.6f};
  const auto pcm = corevideo::modules::interleaveNdiAudio(source, 2, 3, 12);
  EXPECT_EQ(pcm, (std::vector<float>{0.1f, 0.8f, 0.2f, 0.7f, 0.3f, 0.6f}));
  EXPECT_TRUE(corevideo::modules::interleaveNdiAudio(source, 2, 3, 8).empty());
}
