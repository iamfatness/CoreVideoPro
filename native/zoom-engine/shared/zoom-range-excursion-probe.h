#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

// Cheap, per-subscription evidence at the SDK callback boundary. Samples the
// same Y positions before and after the existing range normalizer. An isolated
// one-frame excursion is reported only after the following frame restores the
// baseline; this never changes a pixel or delays publication to the source bus.
class ZoomRangeExcursionProbe {
public:
    struct Sample {
        double rawMean = 0;
        double publishedMean = 0;
        std::uint32_t rawBelow16 = 0;
        std::uint32_t rawAbove235 = 0;
        std::uint32_t sampled = 0;
        bool sdkLimited = false;
    };
    struct Excursion { Sample before, flash, after; };

    std::optional<Excursion> observe(const std::uint8_t *rawY,
                                     const std::uint8_t *publishedY,
                                     std::size_t yLen, bool sdkLimited) {
        if (!rawY || !publishedY || !yLen) return std::nullopt;
        Sample current;
        std::uint64_t rawSum = 0, publishedSum = 0;
        for (std::size_t i = 0; i < yLen; i += 64) {
            const auto raw = rawY[i];
            rawSum += raw;
            publishedSum += publishedY[i];
            current.rawBelow16 += raw < 16;
            current.rawAbove235 += raw > 235;
            ++current.sampled;
        }
        current.rawMean = static_cast<double>(rawSum) / current.sampled;
        current.publishedMean = static_cast<double>(publishedSum) / current.sampled;
        current.sdkLimited = sdkLimited;
        std::optional<Excursion> result;
        if (seen_ >= 2 && std::abs(before_.publishedMean - current.publishedMean) < 2.0 &&
            flash_.publishedMean - (before_.publishedMean + current.publishedMean) / 2.0 > 4.0) {
            result = Excursion{before_, flash_, current};
        }
        before_ = flash_;
        flash_ = current;
        if (seen_ < 2) ++seen_;
        return result;
    }

private:
    Sample before_, flash_;
    unsigned seen_ = 0;
};
