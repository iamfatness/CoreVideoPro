#include "i420-content-range-tracker.h"
#include "i420-range-expand.h"
#include <gtest/gtest.h>
#include <vector>

namespace {

std::vector<std::uint8_t> fullSwingFrame() {
    std::vector<std::uint8_t> y(6400);
    constexpr std::uint8_t values[] = {0, 32, 64, 128, 200, 255};
    for (std::size_t i = 0; i < y.size(); ++i) y[i] = values[(i / 64) % 6];
    return y;
}

std::vector<std::uint8_t> compressedFrame(const std::vector<std::uint8_t> &full) {
    std::vector<std::uint8_t> limited(full.size());
    for (std::size_t i = 0; i < full.size(); ++i)
        limited[i] = static_cast<std::uint8_t>(16 + (int(full[i]) * 219 + 127) / 255);
    return limited;
}

} // namespace

TEST(I420ContentRangeTracker, OverridesStableSdkFlagAcrossOneFrameCompression) {
    I420ContentRangeTracker tracker;
    const auto full = fullSwingFrame();
    const auto compressed = compressedFrame(full);
    const auto before = tracker.classify(full.data(), full.size(), true);
    const auto flash = tracker.classify(compressed.data(), compressed.size(), true);
    const auto after = tracker.classify(full.data(), full.size(), true);
    EXPECT_FALSE(before.limited);
    EXPECT_GT(before.outside, 16u);
    EXPECT_TRUE(flash.limited);
    EXPECT_TRUE(flash.matchedCompression);
    EXPECT_EQ(flash.outside, 0u);
    EXPECT_FALSE(after.limited);
    EXPECT_TRUE(before.transition);
    EXPECT_TRUE(flash.transition);
    EXPECT_TRUE(after.transition);
    for (std::size_t i = 0; i < full.size(); ++i)
        EXPECT_LE(std::abs(int(i420_expand_luma_sample(compressed[i])) - int(full[i])), 1);
}

TEST(I420ContentRangeTracker, RetainsFullDecisionWhenHighlightsLeaveTheScene) {
    I420ContentRangeTracker tracker;
    const auto full = fullSwingFrame();
    const std::vector<std::uint8_t> midtone(full.size(), 100);
    EXPECT_FALSE(tracker.classify(full.data(), full.size(), true).limited);
    const auto changedScene = tracker.classify(midtone.data(), midtone.size(), true);
    EXPECT_FALSE(changedScene.limited);
    EXPECT_FALSE(changedScene.matchedCompression);
}

TEST(I420ContentRangeTracker, TrustsSdkOnAmbiguousInitialLimitedFrame) {
    I420ContentRangeTracker tracker;
    const std::vector<std::uint8_t> limited(6400, 100);
    EXPECT_TRUE(tracker.classify(limited.data(), limited.size(), true).limited);
    EXPECT_TRUE(tracker.classify(limited.data(), limited.size(), true).limited);
}

TEST(I420ContentRangeTracker, ResetsWhenResolutionChanges) {
    I420ContentRangeTracker tracker;
    const auto full = fullSwingFrame();
    const std::vector<std::uint8_t> smaller(3200, 100);
    EXPECT_FALSE(tracker.classify(full.data(), full.size(), true).limited);
    EXPECT_TRUE(tracker.classify(smaller.data(), smaller.size(), true).limited);
}
