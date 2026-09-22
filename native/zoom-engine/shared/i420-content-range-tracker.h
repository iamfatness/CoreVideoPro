#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

// Zoom can report IsLimitedI420() for both full-swing frames and an occasional
// studio-swing frame on the same subscription. A clear population outside
// 16..235 identifies full-swing luma; a frame matching the 255->219+16 mapping
// of the previous full-swing frame identifies a studio-swing excursion. In
// ambiguous frames, retain the last decision rather than oscillating.
class I420ContentRangeTracker {
public:
    struct Decision {
        bool limited = false;
        bool transition = false;
        bool matchedCompression = false;
        std::uint32_t outside = 0;
        std::uint32_t sampled = 0;
    };

    Decision classify(const std::uint8_t *y, std::size_t yLen, bool sdkLimited) {
        Decision result;
        result.limited = sdkLimited;
        if (!y || !yLen) return result;

        const std::size_t count = (yLen + kStride - 1) / kStride;
        if (!previous_.empty() && previous_.size() != count) previous_.clear();
        sample_.resize(count);
        std::uint64_t sameError = 0;
        std::uint64_t compressedError = 0;
        for (std::size_t n = 0, i = 0; i < yLen; i += kStride, ++n) {
            const auto value = y[i];
            sample_[n] = value;
            result.outside += value < 16 || value > 235;
            if (previous_.size() == count) {
                const int prior = previous_[n];
                const int compressed = 16 + (prior * 219 + 127) / 255;
                sameError += static_cast<std::uint64_t>(std::abs(int(value) - prior));
                compressedError += static_cast<std::uint64_t>(std::abs(int(value) - compressed));
            }
        }
        result.sampled = static_cast<std::uint32_t>(count);
        const bool hasPrevious = previous_.size() == count;
        result.matchedCompression =
            hasPrevious && !previousLimited_ && sameError > count * 4 &&
            compressedError < count * 6 && compressedError * 100 < sameError * 55;

        const bool fullEvidence = result.outside >= std::max<std::size_t>(16, count / 50);
        if (result.matchedCompression) {
            result.limited = true;
        } else if (fullEvidence || !sdkLimited) {
            result.limited = false;
        } else if (hasPrevious) {
            result.limited = previousLimited_;
        }
        result.transition = !hasPrevious || result.limited != previousLimited_;
        previousLimited_ = result.limited;
        previous_.swap(sample_);
        return result;
    }

private:
    static constexpr std::size_t kStride = 64;
    std::vector<std::uint8_t> previous_;
    std::vector<std::uint8_t> sample_;
    bool previousLimited_ = false;
};
