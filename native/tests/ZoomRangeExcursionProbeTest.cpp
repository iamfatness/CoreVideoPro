#include "zoom-range-excursion-probe.h"
#include <gtest/gtest.h>
#include <vector>

TEST(ZoomRangeExcursionProbe, IsolatesAOneFrameRangeLikeBrightnessChange) {
    ZoomRangeExcursionProbe probe;
    std::vector<std::uint8_t> full(6400, 32), compressed(6400, 43);
    EXPECT_FALSE(probe.observe(full.data(), full.data(), full.size(), false));
    EXPECT_FALSE(probe.observe(compressed.data(), compressed.data(), compressed.size(), false));
    const auto event = probe.observe(full.data(), full.data(), full.size(), false);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->before.publishedMean, 32);
    EXPECT_EQ(event->flash.publishedMean, 43);
    EXPECT_EQ(event->after.publishedMean, 32);
    EXPECT_FALSE(event->flash.sdkLimited);
    EXPECT_EQ(event->flash.rawMean, 43);
}

TEST(ZoomRangeExcursionProbe, DistinguishesSdkLimitedInputFromPublishedPixels) {
    ZoomRangeExcursionProbe probe;
    std::vector<std::uint8_t> full(6400, 32), limited(6400, 43);
    EXPECT_FALSE(probe.observe(full.data(), full.data(), full.size(), false));
    EXPECT_FALSE(probe.observe(limited.data(), full.data(), full.size(), true));
    EXPECT_FALSE(probe.observe(full.data(), full.data(), full.size(), false));
}

TEST(ZoomRangeExcursionProbe, DetectsRangeLikeDarkeningOnABrightSource) {
    ZoomRangeExcursionProbe probe;
    std::vector<std::uint8_t> full(6400, 158), compressed(6400, 153);
    EXPECT_FALSE(probe.observe(full.data(), full.data(), full.size(), false));
    EXPECT_FALSE(probe.observe(compressed.data(), compressed.data(), compressed.size(), false));
    const auto event = probe.observe(full.data(), full.data(), full.size(), false);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->before.publishedMean, 158);
    EXPECT_EQ(event->flash.publishedMean, 153);
    EXPECT_EQ(event->after.publishedMean, 158);
}

TEST(ZoomRangeExcursionProbe, IgnoresOrdinarySceneChange) {
    ZoomRangeExcursionProbe probe;
    std::vector<std::uint8_t> dark(6400, 32), bright(6400, 80);
    EXPECT_FALSE(probe.observe(dark.data(), dark.data(), dark.size(), false));
    EXPECT_FALSE(probe.observe(bright.data(), bright.data(), bright.size(), false));
    EXPECT_FALSE(probe.observe(bright.data(), bright.data(), bright.size(), false));
}
