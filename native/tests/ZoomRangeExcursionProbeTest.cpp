#include "zoom-range-excursion-probe.h"
#include <gtest/gtest.h>
#include <vector>

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

// CI DIAGNOSTIC (throwaway branch, never merge): MSVC 19.52 fails the two
// positive cases above while clang passes at -O0..-O3. Dump the inputs to the
// excursion decision so the miscompiled step is named.
#include <cstdio>
TEST(ZoomRangeExcursionProbeDiag, DumpDecisionInputs) {
    std::vector<std::uint8_t> full(6400, 32), compressed(6400, 43);
    const std::vector<std::uint8_t> *frames[] = {&full, &compressed, &full};
    ZoomRangeExcursionProbe probe;
    for (int i = 0; i < 3; ++i) {
        const auto &f = *frames[i];
        std::uint64_t sum = 0; std::uint32_t n = 0;
        for (std::size_t k = 0; k < f.size(); k += 64) { sum += f[k]; ++n; }
        const auto ev = probe.observe(f.data(), f.data(), f.size(), false, false);
        std::fprintf(stderr, "[diag] obs=%d inlineSum=%llu n=%u inlineMean=%.6f event=%d",
                     i, static_cast<unsigned long long>(sum), n, double(sum) / n, int(ev.has_value()));
        if (ev) std::fprintf(stderr, " before=%.6f flash=%.6f after=%.6f sampled=%u/%u/%u",
                             ev->before.publishedMean, ev->flash.publishedMean, ev->after.publishedMean,
                             ev->before.sampled, ev->flash.sampled, ev->after.sampled);
        std::fprintf(stderr, "\n");
    }
    std::fprintf(stderr, "[diag] abs(0.0)=%.3f abs(-11.0)=%.3f\n", std::abs(0.0), std::abs(-11.0));
    // Repeat with every intermediate exposed through a volatile copy of the probe logic.
    volatile double b = 32.0, fl = 43.0, c = 32.0;
    std::fprintf(stderr, "[diag] cond1=%d cond2=%d\n", int(std::abs(b - c) < 2.0),
                 int(std::abs(fl - (b + c) / 2.0) > 4.0));
}

// Round 2: which call shape loses the first two observe() side effects?
namespace {
template <typename F> int runShape(F prime) {
    ZoomRangeExcursionProbe probe;
    std::vector<std::uint8_t> full(6400, 32), compressed(6400, 43);
    prime(probe, full, compressed);
    const auto ev = probe.observe(full.data(), full.data(), full.size(), false, false);
    return int(ev.has_value());
}
}
TEST(ZoomRangeExcursionProbeDiag, CallShapes) {
    using V = std::vector<std::uint8_t>;
    const int inlineMacro = runShape([](ZoomRangeExcursionProbe &p, V &f, V &c) {
        EXPECT_FALSE(p.observe(f.data(), f.data(), f.size(), false, false));
        EXPECT_FALSE(p.observe(c.data(), c.data(), c.size(), false, false));
    });
    const int castDiscard = runShape([](ZoomRangeExcursionProbe &p, V &f, V &c) {
        (void)static_cast<bool>(p.observe(f.data(), f.data(), f.size(), false, false));
        (void)static_cast<bool>(p.observe(c.data(), c.data(), c.size(), false, false));
    });
    const int voidDiscard = runShape([](ZoomRangeExcursionProbe &p, V &f, V &c) {
        (void)p.observe(f.data(), f.data(), f.size(), false, false);
        (void)p.observe(c.data(), c.data(), c.size(), false, false);
    });
    const int stored = runShape([](ZoomRangeExcursionProbe &p, V &f, V &c) {
        auto a = p.observe(f.data(), f.data(), f.size(), false, false);
        auto b = p.observe(c.data(), c.data(), c.size(), false, false);
        std::fprintf(stderr, "[diag2] stored a=%d b=%d\n", int(a.has_value()), int(b.has_value()));
    });
    std::fprintf(stderr, "[diag2] inlineMacro=%d castDiscard=%d voidDiscard=%d stored=%d (expect all 1)\n",
                 inlineMacro, castDiscard, voidDiscard, stored);
}
