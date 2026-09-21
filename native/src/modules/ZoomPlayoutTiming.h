#pragma once
#include <chrono>
#include <cstddef>
#include <deque>
#include <utility>

namespace corevideo::modules {
// The Zoom audio feed primes three 20 ms blocks and retains two after emitting.
// The oldest sample in the emitted block is three ticks old (the retained
// two ticks plus the current 20 ms capture block). Video uses that same content
// age, independent of source/render rates.
inline constexpr std::size_t kZoomAudioPrimeTicks = 3;
inline constexpr auto kZoomVideoReserve = std::chrono::milliseconds(20 * kZoomAudioPrimeTicks);
inline constexpr std::size_t kZoomVideoQueueCapacity = 12; // reserve + phase headroom at 120 fps

// Bound both retained images and latency when the producer outruns rendering.
template<class Frame>
std::size_t pushZoomVideo(std::deque<Frame>& queued, Frame frame) {
  queued.push_back(std::move(frame));
  if (queued.size() <= kZoomVideoQueueCapacity) return 0;
  queued.pop_front();
  return 1;
}

// Select the newest frame due on the source playout clock. A faster renderer
// holds its last image; it must never spend the reserve by popping a young frame.
// Expired eligible frames are counted as loss rather than played late after a stall.
template<class Frame, class TimePoint>
std::size_t takeDueZoomVideo(std::deque<Frame>& queued, TimePoint now, Frame& current) {
  std::size_t skipped = 0;
  const auto cutoff = now - kZoomVideoReserve;
  while (queued.size() > 1 && queued[1].observedAt <= cutoff) {
    queued.pop_front();
    ++skipped;
  }
  if (!queued.empty() && queued.front().observedAt <= cutoff) {
    current = std::move(queued.front());
    queued.pop_front();
  }
  return skipped;
}
} // namespace corevideo::modules
