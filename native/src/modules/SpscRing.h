#pragma once

// Lock-free single-producer/single-consumer ring buffer of float frames
// (interleaved stereo), the shared primitive of the pull-model audio output
// architecture (docs/audio-pull-monitor-spec.md §3/§5).
//
// Contract: exactly ONE producer thread calls push(); exactly ONE consumer
// thread calls pop(). Indices are monotonic frame counters (never wrap the
// integer in any realistic session: 2^63 frames @48k ≈ 6M years); the slot
// index is counter % capacity. Producer publishes with release, consumer
// acquires — the classic SPSC discipline, no locks, no syscalls, real-time
// safe on both sides.

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace corevideo::modules {

class SpscRing {
 public:
  explicit SpscRing(size_t capacityFrames = 16384)
      : capacity_(capacityFrames), data_(capacityFrames * 2, 0.0f) {}

  size_t capacityFrames() const { return capacity_; }

  // Frames currently readable. Safe from either thread (approximate from the
  // "other" side, exact from the calling side — fine for depth telemetry).
  size_t depthFrames() const {
    const size_t w = write_.load(std::memory_order_acquire);
    const size_t r = read_.load(std::memory_order_acquire);
    return w - r;
  }

  // Producer: append up to `frames` stereo frames; returns frames accepted
  // (less than requested when full — caller counts the drop, never blocks).
  size_t push(const float* interleavedStereo, size_t frames) {
    const size_t w = write_.load(std::memory_order_relaxed);
    const size_t r = read_.load(std::memory_order_acquire);
    const size_t free = capacity_ - (w - r);
    const size_t accept = frames < free ? frames : free;
    for (size_t i = 0; i < accept; ++i) {
      const size_t slot = ((w + i) % capacity_) * 2;
      data_[slot] = interleavedStereo[i * 2];
      data_[slot + 1] = interleavedStereo[i * 2 + 1];
    }
    write_.store(w + accept, std::memory_order_release);
    return accept;
  }

  // Consumer: fill exactly `frames` stereo frames; ring-dry tail is filled
  // with silence. Returns the number of REAL frames delivered (callers count
  // frames - returned as dry frames).
  size_t pop(float* interleavedStereo, size_t frames) {
    const size_t r = read_.load(std::memory_order_relaxed);
    const size_t w = write_.load(std::memory_order_acquire);
    const size_t availableFrames = w - r;
    const size_t real = frames < availableFrames ? frames : availableFrames;
    for (size_t i = 0; i < real; ++i) {
      const size_t slot = ((r + i) % capacity_) * 2;
      interleavedStereo[i * 2] = data_[slot];
      interleavedStereo[i * 2 + 1] = data_[slot + 1];
    }
    if (real < frames) {
      std::memset(interleavedStereo + real * 2, 0, (frames - real) * 2 * sizeof(float));
    }
    read_.store(r + real, std::memory_order_release);
    return real;
  }

  // Consumer-side reset (e.g., stream restart): discard everything buffered.
  void clear() { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }

 private:
  const size_t capacity_;
  std::vector<float> data_;
  std::atomic<size_t> write_{0};
  std::atomic<size_t> read_{0};
};

}  // namespace corevideo::modules
