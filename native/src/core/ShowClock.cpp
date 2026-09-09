#include "core/ShowClock.h"

#include <limits>
#include <stdexcept>

namespace corevideo::core {
namespace {
constexpr std::int64_t kNsPerSecond = 1'000'000'000LL;

struct MulDivResult { std::uint64_t quotient{0}, remainder{0}; };
std::optional<MulDivResult> mulDiv(std::uint64_t a, std::uint64_t b, std::uint64_t divisor) {
  if (divisor == 0) return std::nullopt;
  const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
  const auto whole = a / divisor;
  if (whole != 0 && b > maximum / whole) return std::nullopt;
  MulDivResult result{whole * b, 0};
  std::uint64_t termQuotient = 0;
  std::uint64_t termRemainder = a % divisor;
  while (b != 0) {
    if ((b & 1U) != 0) {
      if (result.quotient > maximum - termQuotient) return std::nullopt;
      result.quotient += termQuotient;
      if (result.remainder >= divisor - termRemainder) {
        result.remainder -= divisor - termRemainder;
        if (result.quotient == maximum) return std::nullopt;
        ++result.quotient;
      } else result.remainder += termRemainder;
    }
    b >>= 1U;
    if (b == 0) break;
    if (termQuotient > (maximum - 1) / 2) return std::nullopt;
    termQuotient *= 2;
    if (termRemainder >= divisor - termRemainder) {
      termRemainder -= divisor - termRemainder;
      ++termQuotient;
    } else termRemainder *= 2;
  }
  return result;
}

std::int64_t floorScaled(std::int64_t elapsed, std::uint64_t rate,
                         std::uint64_t period) {
  if (elapsed <= 0) return 0;
  const auto scaled = mulDiv(static_cast<std::uint64_t>(elapsed), rate, period);
  if (!scaled || scaled->quotient > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))
    return (std::numeric_limits<std::int64_t>::max)();
  return static_cast<std::int64_t>(scaled->quotient);
}
}  // namespace

ShowClock::ShowClock(Config config) : config_(std::move(config)) {
  if (config_.clockId.empty() || config_.clockId.size() > 512 || config_.generation == 0 ||
      config_.generation > kMaxSafeInteger || config_.anchorNs < 0 ||
      config_.frameRateNumerator == 0 || config_.frameRateNumerator > 1'000'000 ||
      config_.frameRateDenominator == 0 || config_.frameRateDenominator > 1'000'000 ||
      config_.audioSampleRate == 0 || config_.audioSampleRate > 1'000'000)
    throw std::invalid_argument("ShowClock requires bounded positive identity and rates");
}

std::int64_t ShowClock::slotAt(std::int64_t timestampNs) const {
  if (timestampNs < config_.anchorNs) return -1;
  const auto period = static_cast<std::uint64_t>(kNsPerSecond) * config_.frameRateDenominator;
  return floorScaled(timestampNs - config_.anchorNs, config_.frameRateNumerator, period);
}

std::optional<std::int64_t> ShowClock::offsetForUnits(
    std::int64_t units, std::uint64_t rate, std::uint64_t period) const {
  if (units < 0) return std::nullopt;
  const auto unsignedUnits = static_cast<std::uint64_t>(units);
  const auto scaled = mulDiv(unsignedUnits, period, rate);
  if (!scaled) return std::nullopt;
  const auto maximum = static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
  const auto rounded = scaled->quotient + (scaled->remainder == 0 ? 0 : 1);
  if (rounded > maximum || rounded < scaled->quotient) return std::nullopt;
  return static_cast<std::int64_t>(rounded);
}

std::optional<std::int64_t> ShowClock::slotStartNs(std::int64_t slot) const {
  const auto period = static_cast<std::uint64_t>(kNsPerSecond) * config_.frameRateDenominator;
  const auto offset = offsetForUnits(slot, config_.frameRateNumerator, period);
  if (!offset || *offset > (std::numeric_limits<std::int64_t>::max)() - config_.anchorNs)
    return std::nullopt;
  return config_.anchorNs + *offset;
}

std::optional<std::int64_t> ShowClock::deliveryDeadlineNs(
    std::int64_t slot, std::uint32_t bufferFrames) const {
  if (slot < 0 || (bufferFrames != 2 && bufferFrames != 3)) return std::nullopt;
  if (slot > (std::numeric_limits<std::int64_t>::max)() - bufferFrames) return std::nullopt;
  return slotStartNs(slot + bufferFrames);
}

std::int64_t ShowClock::audioSampleAt(std::int64_t timestampNs) const {
  if (timestampNs < config_.anchorNs) return -1;
  return floorScaled(timestampNs - config_.anchorNs, config_.audioSampleRate,
                     static_cast<std::uint64_t>(kNsPerSecond));
}

std::optional<ShowClock::AudioRange> ShowClock::audioRangeForSlot(std::int64_t slot) const {
  const auto firstNs = slotStartNs(slot);
  if (!firstNs || slot == (std::numeric_limits<std::int64_t>::max)()) return std::nullopt;
  const auto endNs = slotStartNs(slot + 1);
  if (!endNs) return std::nullopt;
  const auto first = audioSampleAt(*firstNs);
  const auto end = audioSampleAt(*endNs);
  if (first < 0 || end < first) return std::nullopt;
  return AudioRange{first, end - first};
}

std::optional<ShowClock> ShowClock::restart(std::int64_t anchorNs) const {
  if (anchorNs < 0 || config_.generation == kMaxSafeInteger) return std::nullopt;
  auto next = config_;
  next.anchorNs = anchorNs;
  ++next.generation;
  return ShowClock(std::move(next));
}

}  // namespace corevideo::core
