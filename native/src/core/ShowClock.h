#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace corevideo::core {

// One immutable A/V timebase for a show generation. All timestamps are from the
// caller's monotonic clock. Rounding is performed against the absolute anchor,
// so fractional frame periods never accumulate drift.
class ShowClock final {
 public:
  struct Config {
    std::string clockId;
    std::uint64_t generation{1};
    std::int64_t anchorNs{0};
    std::uint32_t frameRateNumerator{60};
    std::uint32_t frameRateDenominator{1};
    std::uint32_t audioSampleRate{48'000};
  };
  struct AudioRange {
    std::int64_t firstSample{0};
    std::int64_t sampleCount{0};
    bool operator==(const AudioRange&) const = default;
  };

  explicit ShowClock(Config config);
  [[nodiscard]] const Config& config() const { return config_; }
  [[nodiscard]] std::int64_t slotAt(std::int64_t timestampNs) const;
  [[nodiscard]] std::optional<std::int64_t> slotStartNs(std::int64_t slot) const;
  [[nodiscard]] std::optional<std::int64_t> deliveryDeadlineNs(
      std::int64_t slot, std::uint32_t bufferFrames) const;
  [[nodiscard]] std::int64_t audioSampleAt(std::int64_t timestampNs) const;
  [[nodiscard]] std::optional<AudioRange> audioRangeForSlot(std::int64_t slot) const;
  // A discontinuity creates a new immutable generation at an explicit anchor.
  [[nodiscard]] std::optional<ShowClock> restart(std::int64_t anchorNs) const;

 private:
  static constexpr std::uint64_t kMaxSafeInteger = 9'007'199'254'740'991ULL;
  [[nodiscard]] std::optional<std::int64_t> offsetForUnits(
      std::int64_t units, std::uint64_t rateNumerator,
      std::uint64_t periodNumerator) const;
  Config config_;
};

}  // namespace corevideo::core
