#pragma once

// Shared-memory layout between the core and its virtual-camera sink helper
// process (macOS; see VcamHelperRespawnPolicy.h for why the sink is out of
// process). Portable and header-only so the ring is unit-tested everywhere.
//
// One region, created by the core and inherited by the helper as a file
// descriptor (never a named object: nothing to leak, nothing another process
// can open by name):
//
//   [ VcamHelperShared header | slot 0 | slot 1 | ... | slot N-1 ]
//
// Frames: a single-producer single-consumer ring of finished NV12 frames. The
// core (producer) copies a frame into the next free slot and publishes
// writeSeq; the helper (consumer) copies it out into a CVPixelBuffer and
// publishes readSeq. A slot is never rewritten while the helper can still read
// it, so a torn frame cannot happen by construction (spec section 4 law 1). A
// full ring drops the NEW frame and the producer counts it; nothing blocks.
//
// Status: plain atomics the helper writes and the core reads. `warning` is
// written before the state that makes it readable and never changes while that
// state stands.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace corevideo::modules {

inline constexpr std::uint32_t kVcamHelperMagic = 0x43565648;  // "CVVH"
inline constexpr std::uint32_t kVcamHelperVersion = 1;
inline constexpr std::uint32_t kVcamHelperSlots = 4;
inline constexpr std::size_t kVcamHelperHeaderBytes = 16384;  // one arm64 page; slots start page-aligned
inline constexpr std::size_t kVcamHelperWarningBytes = 256;

enum class VcamHelperState : std::uint32_t { Starting = 0, Live = 1, Failed = 2, Stopped = 3 };
enum class VcamDeviceSeen : std::uint32_t { NotObserved = 0, Yes = 1, No = 2 };

struct VcamSlotMeta {
  std::int64_t programSequence = 0;
  std::int64_t writtenAtNs = 0;  // producer host clock at hand-off
};

struct VcamHelperShared {
  // Written once by the core before the helper starts.
  std::uint32_t magic;
  std::uint32_t version;
  std::uint32_t slotCount;
  std::uint32_t slotBytes;
  std::int32_t width;
  std::int32_t height;
  std::int32_t fps;
  std::uint32_t reserved;

  alignas(64) std::atomic<std::uint64_t> writeSeq;  // producer (core)
  alignas(64) std::atomic<std::uint64_t> readSeq;   // consumer (helper)

  // Helper -> core.
  alignas(64) std::atomic<std::uint32_t> state;
  std::atomic<std::uint32_t> deviceSeen;
  std::atomic<std::uint64_t> framesReceived;   // taken off the ring
  std::atomic<std::uint64_t> framesPublished;  // accepted by the CoreMediaIO sink queue
  std::atomic<std::uint64_t> sequenceGaps;
  std::atomic<std::uint64_t> queueFullDrops;
  // Hand-off (core wrote the slot) to the helper taking it up.
  std::atomic<std::uint64_t> transportLastNs;
  std::atomic<std::uint64_t> transportMaxNs;
  std::atomic<std::uint64_t> transportSumNs;
  // The helper's own work on one frame (pixel-buffer copy + sink enqueue).
  std::atomic<std::uint64_t> publishLastNs;
  std::atomic<std::uint64_t> publishMaxNs;
  std::atomic<std::uint64_t> publishSumNs;
  char warning[kVcamHelperWarningBytes];

  VcamSlotMeta meta[kVcamHelperSlots];
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "the ring needs lock-free 64-bit atomics");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "the status block needs lock-free 32-bit atomics");
static_assert(sizeof(VcamHelperShared) <= kVcamHelperHeaderBytes, "the header must fit its reserved page");

inline std::size_t vcamHelperRegionBytes(std::uint32_t slotCount, std::uint32_t slotBytes) {
  return kVcamHelperHeaderBytes + static_cast<std::size_t>(slotCount) * slotBytes;
}

inline std::uint8_t* vcamHelperSlot(VcamHelperShared* shared, std::uint64_t sequence) {
  return reinterpret_cast<std::uint8_t*>(shared) + kVcamHelperHeaderBytes +
         static_cast<std::size_t>(sequence % shared->slotCount) * shared->slotBytes;
}

// The core formats a freshly mapped (zero-filled) region.
inline void formatVcamHelperRegion(VcamHelperShared* shared, int width, int height, int fps,
                                   std::uint32_t slotBytes) {
  shared->magic = kVcamHelperMagic;
  shared->version = kVcamHelperVersion;
  shared->slotCount = kVcamHelperSlots;
  shared->slotBytes = slotBytes;
  shared->width = width;
  shared->height = height;
  shared->fps = fps;
  shared->reserved = 0;
  shared->writeSeq.store(0, std::memory_order_relaxed);
  shared->readSeq.store(0, std::memory_order_relaxed);
  shared->state.store(static_cast<std::uint32_t>(VcamHelperState::Starting), std::memory_order_release);
}

// The helper refuses a region it cannot have been handed by this build.
inline bool vcamHelperRegionValid(const VcamHelperShared* shared, std::size_t mappedBytes) {
  if (shared == nullptr || mappedBytes < kVcamHelperHeaderBytes) return false;
  if (shared->magic != kVcamHelperMagic || shared->version != kVcamHelperVersion) return false;
  if (shared->slotCount != kVcamHelperSlots || shared->slotBytes == 0) return false;
  if (shared->width <= 0 || shared->height <= 0 || (shared->width & 1) != 0 || (shared->height & 1) != 0) return false;
  const std::size_t frame = static_cast<std::size_t>(shared->width) * shared->height * 3 / 2;
  if (shared->slotBytes < frame) return false;
  return mappedBytes >= vcamHelperRegionBytes(shared->slotCount, shared->slotBytes);
}

// Producer side. False when the ring is full (the caller drops the new frame)
// or the frame does not fit a slot.
inline bool vcamRingTryWrite(VcamHelperShared* shared, const std::uint8_t* nv12, std::size_t bytes,
                             std::int64_t programSequence, std::int64_t nowNs) {
  if (bytes == 0 || bytes > shared->slotBytes) return false;
  const std::uint64_t write = shared->writeSeq.load(std::memory_order_relaxed);
  const std::uint64_t read = shared->readSeq.load(std::memory_order_acquire);
  if (write - read >= shared->slotCount) return false;
  std::memcpy(vcamHelperSlot(shared, write), nv12, bytes);
  shared->meta[write % shared->slotCount] = VcamSlotMeta{programSequence, nowNs};
  shared->writeSeq.store(write + 1, std::memory_order_release);
  return true;
}

inline std::uint64_t vcamRingDepth(const VcamHelperShared* shared) {
  return shared->writeSeq.load(std::memory_order_acquire) - shared->readSeq.load(std::memory_order_acquire);
}

struct VcamRingFrame {
  const std::uint8_t* nv12 = nullptr;
  VcamSlotMeta meta;
};

// Consumer side: the oldest unread frame, valid until vcamRingPop().
inline bool vcamRingPeek(VcamHelperShared* shared, VcamRingFrame& out) {
  const std::uint64_t read = shared->readSeq.load(std::memory_order_relaxed);
  const std::uint64_t write = shared->writeSeq.load(std::memory_order_acquire);
  if (read == write) return false;
  out.nv12 = vcamHelperSlot(shared, read);
  out.meta = shared->meta[read % shared->slotCount];
  return true;
}

inline void vcamRingPop(VcamHelperShared* shared) {
  shared->readSeq.store(shared->readSeq.load(std::memory_order_relaxed) + 1, std::memory_order_release);
}

// Helper side: publish a state with its warning ("" for none).
inline void vcamHelperPublishState(VcamHelperShared* shared, VcamHelperState state, const std::string& warning) {
  const std::size_t n = warning.size() < kVcamHelperWarningBytes - 1 ? warning.size() : kVcamHelperWarningBytes - 1;
  std::memcpy(shared->warning, warning.data(), n);
  shared->warning[n] = '\0';
  shared->state.store(static_cast<std::uint32_t>(state), std::memory_order_release);
}

inline VcamHelperState vcamHelperReadState(const VcamHelperShared* shared) {
  return static_cast<VcamHelperState>(shared->state.load(std::memory_order_acquire));
}

// Core side: only meaningful once the state reads Failed.
inline std::string vcamHelperReadWarning(const VcamHelperShared* shared) {
  char copy[kVcamHelperWarningBytes];
  std::memcpy(copy, shared->warning, sizeof(copy));
  copy[sizeof(copy) - 1] = '\0';
  return copy;
}

inline void vcamHelperRecordMax(std::atomic<std::uint64_t>& max, std::uint64_t value) {
  if (value > max.load(std::memory_order_relaxed)) max.store(value, std::memory_order_relaxed);
}

}  // namespace corevideo::modules
