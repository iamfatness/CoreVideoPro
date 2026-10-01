#pragma once

// Frame-sized allocations that fail by returning, not by throwing.
//
// #728 (live 2026-10-01). Windows ran low on virtual memory, one 8.3 MB frame
// buffer could not be allocated on a decoder thread, and the uncaught
// std::bad_alloc terminated the core: Program, the recording, the streams and
// the Zoom session, for one frame of one source. Every per-frame buffer in an
// ingest, capture, decode or tap path goes through these helpers, so the cost of
// a failed allocation is ONE DROPPED FRAME of that source, said out loud.
//
// Scope, on purpose: frame-sized buffers (megabytes). Those are what fail first
// when the machine is out of commit. A few-byte control block failing means the
// process cannot do anything useful and is not what these guard.

#include "core/BoundedAsyncLog.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace corevideo::core {

using FrameBytes = std::vector<std::uint8_t>;
using SharedFrameBytes = std::shared_ptr<FrameBytes>;

// make_shared<vector<uint8_t>>(args...) that returns null instead of throwing.
// Takes whatever the vector constructors take: a size, a size and a fill value,
// an iterator range, or another vector to copy or move.
template <typename... Args>
SharedFrameBytes tryMakeFrameBuffer(Args&&... args) noexcept {
  try {
    return std::make_shared<FrameBytes>(std::forward<Args>(args)...);
  } catch (const std::bad_alloc&) {
    return nullptr;
  } catch (const std::length_error&) {
    return nullptr;
  }
}

// buffer.resize(bytes) that reports failure. On failure the buffer is unchanged.
inline bool tryResizeFrameBuffer(FrameBytes& buffer, std::size_t bytes) noexcept {
  try {
    buffer.resize(bytes);
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  } catch (const std::length_error&) {
    return false;
  }
}

// One counter and one log clock per call site: declare it `static` where the
// frame is dropped. Logs the first failure and then at most every 5 seconds, so
// a starved machine cannot flood the bounded log it needs for the diagnosis.
class FrameAllocationFailures {
 public:
  explicit FrameAllocationFailures(const char* site) noexcept : site_(site) {}

  // Returns true when this failure was logged.
  bool note(std::size_t bytes) noexcept {
    return noteAt(bytes, std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
  }

  bool noteAt(std::size_t bytes, std::int64_t nowMs) noexcept {
    const auto count = count_.fetch_add(1, std::memory_order_relaxed) + 1;
    auto last = lastLogMs_.load(std::memory_order_relaxed);
    if (count != 1 && nowMs - last < kLogIntervalMs) return false;
    if (!lastLogMs_.compare_exchange_strong(last, nowMs, std::memory_order_relaxed)) return false;
    nativeLogf("[frame-alloc] OUT OF MEMORY at %s: could not allocate %zu bytes; frame dropped (total=%llu)\n",
               site_, bytes, static_cast<unsigned long long>(count));
    return true;
  }

  std::uint64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }

  static constexpr std::int64_t kLogIntervalMs = 5000;

 private:
  const char* site_;
  std::atomic<std::uint64_t> count_{0};
  std::atomic<std::int64_t> lastLogMs_{0};
};

}  // namespace corevideo::core
