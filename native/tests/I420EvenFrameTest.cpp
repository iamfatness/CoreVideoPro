// The macOS engine's per-frame I420 preparation (mac Tiles Task 5, N1 review):
// crop to even dimensions and, when the frame is limited range, expand ONCE
// into a reusable scratch frame that every SHM target then memcpys inside its
// seqlock window — the Windows I420RangeNormalizer shape.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "i420-even-frame.h"

namespace {

struct Planes {
  std::vector<uint8_t> y, u, v;
};

// A src_w x src_h I420 frame, planes tightly packed at src_w and ceil(src_w/2).
Planes makeFrame(uint32_t src_w, uint32_t src_h, uint8_t yBase, uint8_t uv) {
  Planes p;
  const uint32_t cw = (src_w + 1) / 2, ch = (src_h + 1) / 2;
  p.y.resize(static_cast<size_t>(src_w) * src_h);
  for (size_t i = 0; i < p.y.size(); ++i) p.y[i] = static_cast<uint8_t>(yBase + i);
  p.u.assign(static_cast<size_t>(cw) * ch, uv);
  p.v.assign(static_cast<size_t>(cw) * ch, uv);
  return p;
}

}  // namespace

TEST(I420EvenFrame, PackCropsAnOddWidthFrameUsingTheSourceStride) {
  // 5x3 source -> 4x2 even crop. Y row stride 5, chroma stride 3.
  Planes src = makeFrame(5, 3, 0, 0);
  for (size_t i = 0; i < src.u.size(); ++i) { src.u[i] = static_cast<uint8_t>(100 + i); src.v[i] = static_cast<uint8_t>(200 + i); }
  std::vector<uint8_t> dst(4 * 2 + 2 * 2, 0xEE);
  i420_pack_even(dst.data(), src.y.data(), src.u.data(), src.v.data(), 4, 2, 5);
  const std::vector<uint8_t> expectY = {0, 1, 2, 3, 5, 6, 7, 8};
  EXPECT_EQ(std::vector<uint8_t>(dst.begin(), dst.begin() + 8), expectY);
  EXPECT_EQ(dst[8], 100);   // U row 0, col 0
  EXPECT_EQ(dst[9], 101);   // U row 0, col 1 (col 2 cropped)
  EXPECT_EQ(dst[10], 200);  // V row 0, col 0
  EXPECT_EQ(dst[11], 201);
}

TEST(I420EvenFrame, ALimitedFrameIsExpandedOnceIntoTheScratchFrame) {
  Planes src = makeFrame(4, 4, 0, 128);
  src.y.assign(16, 16);
  src.y[5] = 235;
  src.u.assign(4, 16);
  src.v.assign(4, 240);
  I420EvenFrame frame;
  const uint8_t* out = frame.prepare(src.y.data(), src.u.data(), src.v.data(), 4, 4, 4,
                                     /*limited=*/true, /*max_y_len=*/1 << 20);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(frame.size(), 16u + 4u + 4u);
  EXPECT_EQ(out[0], 0);     // luma 16 -> 0
  EXPECT_EQ(out[5], 255);   // luma 235 -> 255
  EXPECT_EQ(out[16], 0);    // chroma 16 -> 0
  EXPECT_EQ(out[20], 255);  // chroma 240 -> 255
  // The SDK's planes are never written.
  EXPECT_EQ(src.y[0], 16);
}

TEST(I420EvenFrame, AFullRangeFrameIsPackedUnchanged) {
  Planes src = makeFrame(4, 4, 16, 77);
  I420EvenFrame frame;
  const uint8_t* out = frame.prepare(src.y.data(), src.u.data(), src.v.data(), 4, 4, 4,
                                     /*limited=*/false, 1 << 20);
  ASSERT_NE(out, nullptr);
  for (size_t i = 0; i < 16; ++i) EXPECT_EQ(out[i], src.y[i]) << i;
  EXPECT_EQ(out[16], 77);
  EXPECT_EQ(out[23], 77);
}

TEST(I420EvenFrame, TheScratchIsReusedAtTheSameSizeAndOversizeIsRefused) {
  Planes src = makeFrame(4, 4, 16, 128);
  I420EvenFrame frame;
  const uint8_t* first = frame.prepare(src.y.data(), src.u.data(), src.v.data(), 4, 4, 4, true, 1 << 20);
  const uint8_t* second = frame.prepare(src.y.data(), src.u.data(), src.v.data(), 4, 4, 4, true, 1 << 20);
  EXPECT_EQ(first, second);  // no reallocation per frame
  EXPECT_EQ(frame.prepare(src.y.data(), src.u.data(), src.v.data(), 4, 4, 4, true, /*max_y_len=*/15), nullptr);
}

// N1 review MEDIUM: the expansion must run with the global video mutex
// RELEASED — every other feed's callback try_locks it and silently drops its
// frame while it is held. Another thread's try_lock during the prepare step is
// exactly what a second feed does.
TEST(I420EvenFrame, TheExpansionRunsWithTheVideoMutexReleasedAndRetakesIt) {
  std::mutex videoMutex;
  std::unique_lock<std::mutex> lock(videoMutex);
  Planes src = makeFrame(4, 4, 16, 128);
  I420EvenFrame frame;
  bool otherFeedGotTheLock = false;
  const uint8_t* out = i420_prepare_with_lock_released(lock, [&] {
    std::thread otherFeed([&] {
      if (videoMutex.try_lock()) { otherFeedGotTheLock = true; videoMutex.unlock(); }
    });
    otherFeed.join();
    return frame.prepare(src.y.data(), src.u.data(), src.v.data(), 4, 4, 4, true, 1 << 20);
  });
  EXPECT_TRUE(otherFeedGotTheLock) << "another feed's try_lock failed during the expansion";
  ASSERT_NE(out, nullptr);
  EXPECT_TRUE(lock.owns_lock()) << "the lock is re-taken for the SHM fan-out";
  EXPECT_EQ(out[0], 0);  // and it really expanded (16 -> 0)
}

namespace {
// Records hold state; refuses the re-take when `contended` (another thread holds it).
struct FakeLock {
  bool owned = true, contended = false;
  void unlock() { owned = false; }
  bool try_lock() { owned = !contended; return owned; }
};
}  // namespace

TEST(I420EvenFrame, AContendedRetakeOrARefusedFrameDropsTheFrameWithoutTheLock) {
  static const uint8_t byte = 0;
  FakeLock contended; contended.contended = true;
  EXPECT_EQ(i420_prepare_with_lock_released(contended, [] { return &byte; }), nullptr);
  EXPECT_FALSE(contended.owned);

  FakeLock refused;
  int prepares = 0;
  EXPECT_EQ(i420_prepare_with_lock_released(refused, [&]() -> const uint8_t* { ++prepares; return nullptr; }), nullptr);
  EXPECT_EQ(prepares, 1);
  EXPECT_FALSE(refused.owned) << "a refused frame must not re-take the lock";
}

// main-macos.mm links the Zoom SDK and is not compiled into the tests, so the
// lock placement at its call sites is pinned at the source level (the
// TheDualCaptureSlotStufferStaysDead shape): every scratch prepare in the engine
// runs inside i420_prepare_with_lock_released, i.e. with the lock released.
// A regression that calls `scratch.prepare(...)` directly under the lock fails here.
TEST(I420EvenFrame, EveryEngineScratchPrepareRunsInsideTheLockReleasingSeam) {
  std::ifstream in(std::filesystem::path(COREVIDEO_REPO_ROOT) / "native/zoom-engine/engine/main-macos.mm");
  ASSERT_TRUE(in) << "main-macos.mm not found";
  std::ostringstream buf;
  buf << in.rdbuf();
  const std::string src = buf.str();
  const std::string seam = "i420_prepare_with_lock_released(";
  size_t prepares = 0;
  // "cratch.prepare(" matches both `scratch.prepare(` and `shareScratch.prepare(`.
  const std::string call = "cratch.prepare(";
  for (size_t at = src.find(call); at != std::string::npos; at = src.find(call, at + 1)) {
    ++prepares;
    const size_t open = src.rfind(seam, at);
    ASSERT_NE(open, std::string::npos) << "a scratch prepare at offset " << at << " is outside the seam";
    // Inside the seam's lambda: the first "});" after the seam opens is also
    // the first after the prepare (no closed seam call in between).
    EXPECT_EQ(src.find("});", open), src.find("});", at))
        << "a scratch prepare at offset " << at << " is not inside the lock-releasing seam";
  }
  EXPECT_EQ(prepares, 2u) << "expected the video and the share prepare";
}
