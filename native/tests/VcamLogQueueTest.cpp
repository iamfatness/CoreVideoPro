#include <gtest/gtest.h>
#ifdef _WIN32
#include "../virtualcam-dll/VcamLogQueue.h"
#include <chrono>
#include <string>

namespace {
struct BlockedLog {
  HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::atomic<unsigned> calls{0};
  ~BlockedLog() { CloseHandle(entered); CloseHandle(release); }
  static bool write(std::string_view, void* context) {
    auto& state = *static_cast<BlockedLog*>(context);
    ++state.calls; SetEvent(state.entered); WaitForSingleObject(state.release, INFINITE);
    return true;
  }
};
struct ReleaseLog {
  BlockedLog& sink; corevideo::virtualcam::VcamLogQueue& queue;
  ~ReleaseLog() { SetEvent(sink.release); queue.waitForCallbacks(); }
};
}

TEST(VcamLogQueue, BlockedFileSinkCannotBlockOrGrowTheMediaQueue) {
  using corevideo::virtualcam::VcamLogQueue;
  BlockedLog sink;
  VcamLogQueue queue(&BlockedLog::write, &sink);
  ReleaseLog release{sink, queue};
  queue.write("first");
  ASSERT_EQ(WaitForSingleObject(sink.entered, 2000), WAIT_OBJECT_0);
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000; ++i) queue.write("media still runs");
  EXPECT_TRUE(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500));
  EXPECT_EQ(queue.dropped(), 1000u - VcamLogQueue::Capacity);
  EXPECT_EQ(sink.calls.load(), 1u);
  SetEvent(sink.release); queue.waitForCallbacks();
  EXPECT_EQ(sink.calls.load(), 1u + VcamLogQueue::Capacity);
}

TEST(VcamLogQueue, TruncationAndSinkFailureAreBoundedAndObservable) {
  using corevideo::virtualcam::VcamLogQueue;
  size_t bytes = 0;
  VcamLogQueue queue([](std::string_view message, void* context) {
    *static_cast<size_t*>(context) = message.size(); return false;
  }, &bytes);
  queue.write(std::string(4096, 'x')); queue.waitForCallbacks();
  EXPECT_EQ(bytes, VcamLogQueue::MessageBytes - 1);
  EXPECT_EQ(queue.truncated(), 1u);
  EXPECT_EQ(queue.failures(), 1u);
}
#endif
