#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace corevideo::modules {

// #652: the operator hears this output beside the Program display. A 200 ms
// endpoint request plus a 60 ms standing ring was measured 239 ms behind the
// Program texture on GoXLR Game. Keep the decision portable and the WASAPI
// adapter limited to applying it; the same-run clap harness is the hardware gate.
struct MonitorLatencyPolicy {
  // In shared mode, zero asks WASAPI for its minimum supported latency. A
  // 20 ms nonzero request still negotiated 44.5 ms on the GoXLR endpoint.
  static constexpr std::int64_t endpointBuffer100ns() { return 0; }

  static constexpr std::size_t ringTargetFrames(int sampleRate) {
    return static_cast<std::size_t>(std::max(1, sampleRate / 50));  // one 20 ms audio tick
  }

  static constexpr std::uint32_t sharedPeriodFrames(int sampleRate, std::uint32_t fundamental,
                                                    std::uint32_t minimum, std::uint32_t maximum) {
    if (sampleRate <= 0 || fundamental == 0 || minimum == 0 || maximum < minimum) return 0;
    const auto target = static_cast<std::uint32_t>(std::max(1, sampleRate / 100));  // 10 ms
    const auto rounded = ((target + fundamental - 1) / fundamental) * fundamental;
    return std::clamp(rounded, minimum, maximum);
  }
};

}  // namespace corevideo::modules
