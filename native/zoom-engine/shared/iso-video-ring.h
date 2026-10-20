#pragma once
#include "engine-ipc.h"
#include <atomic>
#include <chrono>
#include <cstring>

// Additive ISO-only transport. The existing latest-picture SHM ABI is unchanged.
// Four full-resolution slots provide bounded catch-up without stalling SDK video.
namespace zoom_iso {
constexpr uint32_t magic = 0x43564952, slots = 4;
constexpr size_t payload = kMaxVideoShmYLen * 3 / 2;
struct alignas(8) Header { uint32_t magicValue, version, epoch, slotCount, written, reserved; };
struct alignas(8) Slot { uint32_t publication, reserved; int64_t captured100ns; ShmFrameHeader frame; };
constexpr size_t stride = sizeof(Slot) + payload;
constexpr size_t bytes = sizeof(Header) + slots * stride;
#if defined(__cpp_lib_atomic_ref) && __cpp_lib_atomic_ref >= 201806L
static_assert(std::atomic_ref<uint32_t>::is_always_lock_free);
inline uint32_t load(const uint32_t& value) { return std::atomic_ref<uint32_t>(const_cast<uint32_t&>(value)).load(std::memory_order_acquire); }
inline void store(uint32_t& value, uint32_t next) { std::atomic_ref<uint32_t>(value).store(next, std::memory_order_release); }
inline void beginWrite(uint32_t& value, uint32_t next) { std::atomic_ref<uint32_t>(value).store(next, std::memory_order_seq_cst); }
#else
// Apple libc++ on the supported runner has no atomic_ref yet. These lock-free
// compiler operations also work on read-only mapped pages (unlike CAS loads).
static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr));
inline uint32_t load(const uint32_t& value) { return __atomic_load_n(&value, __ATOMIC_ACQUIRE); }
inline void store(uint32_t& value, uint32_t next) { __atomic_store_n(&value, next, __ATOMIC_RELEASE); }
inline void beginWrite(uint32_t& value, uint32_t next) { __atomic_store_n(&value, next, __ATOMIC_SEQ_CST); }
#endif
inline Slot* slot(void* view, uint32_t index) { return reinterpret_cast<Slot*>(static_cast<uint8_t*>(view) + sizeof(Header) + (index % slots) * stride); }
inline const Slot* slot(const void* view, uint32_t index) { return slot(const_cast<void*>(view), index); }
inline void initialize(void* view, uint32_t epoch) {
  auto& header = *static_cast<Header*>(view);
  store(header.epoch, 0);
  header.magicValue = magic; header.version = 1; header.slotCount = slots;
  store(header.written, 0);
  for (uint32_t i = 0; i < slots; ++i) store(slot(view, i)->publication, 0);
  store(header.epoch, epoch);
}
inline bool write(void* view, size_t size, uint32_t epoch, uint32_t sourceSequence,
                  uint32_t width, uint32_t height, const uint8_t* y, const uint8_t* u, const uint8_t* v,
                  int64_t captured100ns) {
  const size_t ylen = size_t(width) * height;
  if (!view || size < bytes || !epoch || !sourceSequence || (sourceSequence & 1) ||
      !width || !height || (width & 1) || (height & 1) || ylen > kMaxVideoShmYLen || !y || !u || !v) return false;
  auto& header = *static_cast<Header*>(view);
  if (load(header.epoch) != epoch) initialize(view, epoch);
  const auto counter = load(header.written);
  if (counter >= UINT32_MAX / 2 - 1) return false; // fail explicitly before sequence wrap
  auto* target = slot(view, counter);
  beginWrite(target->publication, counter * 2 + 1);
  target->captured100ns = captured100ns;
  target->frame = {sourceSequence, width, height, static_cast<uint32_t>(ylen)};
  auto* pixels = reinterpret_cast<uint8_t*>(target) + sizeof(Slot);
  std::memcpy(pixels, y, ylen); std::memcpy(pixels + ylen, u, ylen / 4); std::memcpy(pixels + ylen * 5 / 4, v, ylen / 4);
  store(target->publication, counter * 2 + 2);
  store(header.written, counter + 1);
  return true;
}
}
