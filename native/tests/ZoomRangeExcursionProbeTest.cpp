#include "zoom-range-excursion-probe.h"
#include <gtest/gtest.h>
#include <vector>

// MSVC 19.52 (VS 2026, runner image windows-2025-vs2026 20261116) miscompiles
// these test bodies: with three observe() calls on a local probe inlined into
// one function and the result read afterwards, the optimized build returns no
// event where /Od returns one. Proven on PR #1221 round 4 (c71b72a1): an
// optimized clone gave event=0, a `#pragma optimize("", off)` clone event=1,
// and a clone without the trailing event-> reads event=1; clang -O0..-O3 and
// MSVC 19.4x compute it correctly. The probe itself is fine: production
// (engine-video.cpp) calls observe() once per frame on a member probe, so the
// folded shape never forms there. Optimization stays off for this whole file
// so the "returns nothing" tests below can't pass vacuously.
#if defined(_MSC_VER) && !defined(__clang__) && _MSC_VER >= 1952
#pragma optimize("", off)
#endif

TEST(ZoomRangeExcursionProbe, IsolatesAOneFrameRangeLikeBrightnessChange) {
    ZoomRangeExcursionProbe probe;
    std::vector<std::uint8_t> full(6400, 32), compressed(6400, 43);
    EXPECT_FALSE(probe.observe(full.data(), full.data(), full.size(), false, false));
    EXPECT_FALSE(probe.observe(compressed.data(), compressed.data(), compressed.size(), false, false));
    const auto event = probe.observe(full.data(), full.data(), full.size(), false, false);
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
    EXPECT_FALSE(probe.observe(full.data(), full.data(), full.size(), false, false));
    EXPECT_FALSE(probe.observe(limited.data(), full.data(), full.size(), true, true));
    EXPECT_FALSE(probe.observe(full.data(), full.data(), full.size(), false, false));
}

TEST(ZoomRangeExcursionProbe, DetectsRangeLikeDarkeningOnABrightSource) {
    ZoomRangeExcursionProbe probe;
    std::vector<std::uint8_t> full(6400, 158), compressed(6400, 153);
    EXPECT_FALSE(probe.observe(full.data(), full.data(), full.size(), false, false));
    EXPECT_FALSE(probe.observe(compressed.data(), compressed.data(), compressed.size(), false, false));
    const auto event = probe.observe(full.data(), full.data(), full.size(), false, false);
    ASSERT_TRUE(event);
    EXPECT_EQ(event->before.publishedMean, 158);
    EXPECT_EQ(event->flash.publishedMean, 153);
    EXPECT_EQ(event->after.publishedMean, 158);
}

TEST(ZoomRangeExcursionProbe, IgnoresOrdinarySceneChange) {
    ZoomRangeExcursionProbe probe;
    std::vector<std::uint8_t> dark(6400, 32), bright(6400, 80);
    EXPECT_FALSE(probe.observe(dark.data(), dark.data(), dark.size(), false, false));
    EXPECT_FALSE(probe.observe(bright.data(), bright.data(), bright.size(), false, false));
    EXPECT_FALSE(probe.observe(bright.data(), bright.data(), bright.size(), false, false));
}

#if defined(_MSC_VER) && !defined(__clang__) && _MSC_VER >= 1952
#pragma optimize("", on)
#endif
