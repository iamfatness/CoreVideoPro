// VcamHelperTransport: the core -> sink-helper frame ring and status block
// (plan 2026-11-16-mac-vcam-provider-arrival-fix, Task 3 design B). Exercised
// over an ordinary heap region, so it runs on every platform.
#include "modules/VcamHelperTransport.h"
#include "modules/VirtualCameraFrame.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

using namespace corevideo::modules;

namespace {

struct Region {
  VcamHelperShared* shared = nullptr;
  std::size_t bytes = 0;
  void* raw = nullptr;
  Region(int width, int height) {
    const auto slot = static_cast<std::uint32_t>(nv12FrameSize(width, height));
    bytes = vcamHelperRegionBytes(kVcamHelperSlots, slot);
    raw = ::operator new(bytes, std::align_val_t(4096));
    std::memset(raw, 0, bytes);
    shared = static_cast<VcamHelperShared*>(raw);
    formatVcamHelperRegion(shared, width, height, 60, slot);
  }
  ~Region() { ::operator delete(raw, std::align_val_t(4096)); }
};

std::vector<std::uint8_t> frame(int w, int h, std::uint8_t fill) {
  return std::vector<std::uint8_t>(nv12FrameSize(w, h), fill);
}

}  // namespace

TEST(VcamHelperTransport, AFormattedRegionIsValidAndAForeignOneIsRefused) {
  Region region(64, 36);
  EXPECT_TRUE(vcamHelperRegionValid(region.shared, region.bytes));
  EXPECT_FALSE(vcamHelperRegionValid(region.shared, region.bytes - 1)) << "a truncated mapping";
  EXPECT_FALSE(vcamHelperRegionValid(nullptr, region.bytes));
  region.shared->version = kVcamHelperVersion + 1;
  EXPECT_FALSE(vcamHelperRegionValid(region.shared, region.bytes)) << "another build's layout";
  region.shared->version = kVcamHelperVersion;
  region.shared->slotBytes = 16;
  EXPECT_FALSE(vcamHelperRegionValid(region.shared, region.bytes)) << "a slot smaller than one frame";
}

TEST(VcamHelperTransport, FramesComeOutInOrderWithTheirSequenceAndBytes) {
  Region region(64, 36);
  for (int i = 1; i <= 3; ++i) {
    const auto f = frame(64, 36, static_cast<std::uint8_t>(i));
    ASSERT_TRUE(vcamRingTryWrite(region.shared, f.data(), f.size(), 100 + i, 1000 * i));
  }
  EXPECT_EQ(vcamRingDepth(region.shared), 3u);
  for (int i = 1; i <= 3; ++i) {
    VcamRingFrame out;
    ASSERT_TRUE(vcamRingPeek(region.shared, out));
    EXPECT_EQ(out.meta.programSequence, 100 + i);
    EXPECT_EQ(out.meta.writtenAtNs, 1000 * i);
    EXPECT_EQ(out.nv12[0], i);
    EXPECT_EQ(out.nv12[nv12FrameSize(64, 36) - 1], i);
    vcamRingPop(region.shared);
  }
  VcamRingFrame none;
  EXPECT_FALSE(vcamRingPeek(region.shared, none));
}

// Spec section 4 law 1: a full ring drops the NEW frame and never touches a
// frame the helper has not taken yet.
TEST(VcamHelperTransport, AFullRingRefusesTheNewFrameAndKeepsEveryHeldOne) {
  Region region(64, 36);
  for (std::uint32_t i = 0; i < kVcamHelperSlots; ++i) {
    const auto f = frame(64, 36, static_cast<std::uint8_t>(10 + i));
    ASSERT_TRUE(vcamRingTryWrite(region.shared, f.data(), f.size(), i + 1, 0));
  }
  const auto extra = frame(64, 36, 99);
  EXPECT_FALSE(vcamRingTryWrite(region.shared, extra.data(), extra.size(), 5, 0));
  for (std::uint32_t i = 0; i < kVcamHelperSlots; ++i) {
    VcamRingFrame out;
    ASSERT_TRUE(vcamRingPeek(region.shared, out));
    EXPECT_EQ(out.nv12[0], 10 + i) << "slot " << i << " was overwritten";
    vcamRingPop(region.shared);
  }
  EXPECT_TRUE(vcamRingTryWrite(region.shared, extra.data(), extra.size(), 5, 0)) << "room again after a pop";
}

TEST(VcamHelperTransport, AFrameThatDoesNotFitASlotIsRefused) {
  Region region(64, 36);
  const auto big = frame(128, 72, 1);
  EXPECT_FALSE(vcamRingTryWrite(region.shared, big.data(), big.size(), 1, 0));
  EXPECT_EQ(vcamRingDepth(region.shared), 0u);
}

// One producer thread, one consumer thread, every frame filled with one byte
// value: a frame read with mixed values was rewritten while it was readable.
TEST(VcamHelperTransport, NoFrameIsTornOrReorderedAcrossThreads) {
  Region region(64, 36);
  const std::size_t bytes = nv12FrameSize(64, 36);
  constexpr int kFrames = 20000;
  std::atomic<bool> producerDone{false};
  std::atomic<int> written{0};
  std::thread producer([&] {
    std::vector<std::uint8_t> f(bytes);
    for (int i = 1; i <= kFrames;) {
      std::memset(f.data(), i & 0xff, bytes);
      if (vcamRingTryWrite(region.shared, f.data(), bytes, i, 0)) { ++i; written.fetch_add(1); }
      else std::this_thread::yield();
    }
    producerDone.store(true);
  });
  int torn = 0, outOfOrder = 0, read = 0;
  std::int64_t last = 0;
  while (read < kFrames) {
    VcamRingFrame out;
    if (!vcamRingPeek(region.shared, out)) {
      if (producerDone.load() && vcamRingDepth(region.shared) == 0) break;
      std::this_thread::yield();
      continue;
    }
    const std::uint8_t expected = static_cast<std::uint8_t>(out.meta.programSequence & 0xff);
    for (std::size_t b = 0; b < bytes; b += 97) if (out.nv12[b] != expected) { ++torn; break; }
    if (out.meta.programSequence != last + 1) ++outOfOrder;
    last = out.meta.programSequence;
    vcamRingPop(region.shared);
    ++read;
  }
  producer.join();
  EXPECT_EQ(read, kFrames);
  EXPECT_EQ(torn, 0);
  EXPECT_EQ(outOfOrder, 0);
}

TEST(VcamHelperTransport, AStateCarriesItsWarningAndLongWarningsAreCut) {
  Region region(64, 36);
  EXPECT_EQ(vcamHelperReadState(region.shared), VcamHelperState::Starting);
  vcamHelperPublishState(region.shared, VcamHelperState::Failed, "CAMERA_EXTENSION_NOT_FOUND: not installed");
  EXPECT_EQ(vcamHelperReadState(region.shared), VcamHelperState::Failed);
  EXPECT_EQ(vcamHelperReadWarning(region.shared), "CAMERA_EXTENSION_NOT_FOUND: not installed");
  vcamHelperPublishState(region.shared, VcamHelperState::Failed, std::string(1000, 'x'));
  EXPECT_EQ(vcamHelperReadWarning(region.shared).size(), kVcamHelperWarningBytes - 1);
}
