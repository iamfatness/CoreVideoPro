#include "modules/IsolatedOutputSender.h"
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <thread>
#include <mutex>

namespace {
using namespace corevideo::modules;
class ProbeSender final : public IOutputSender {
 public:
  explicit ProbeSender(bool block) : blocking(block) {}
  OutputSenderSession sync(const std::vector<std::string>&, const ProgramFrame*, double,
      const std::vector<OutputDestinationSettings>&, const std::vector<float>*, int, int) override {
    std::unique_lock<std::mutex> lock(mutex);
    ++videos; cv.notify_all();
    if (blocking) cv.wait(lock, [&] { return released; });
    return {};
  }
  void submitAudio(const std::vector<float>&, int, int) override {
    std::lock_guard<std::mutex> lock(mutex); ++audios; cv.notify_all();
  }
  bool await(int videoCount, int audioCount) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(2), [&] { return videos >= videoCount && audios >= audioCount; });
  }
  void interrupt(const std::string&) override {
    std::lock_guard<std::mutex> lock(mutex); released = true; cv.notify_all();
  }
  OutputSenderSession fail(const std::string&, const std::string&, double) override { return {}; }
  OutputSenderSession recover(const std::string&, double, const std::string&) override { return {}; }
  OutputSenderSession session() const override { return {}; }
 private:
  bool blocking, released = false;
  int videos = 0, audios = 0;
  std::mutex mutex;
  std::condition_variable cv;
};
}
TEST(IsolatedOutputSender, BlockedDestinationDoesNotBlockOtherVideoAndAudio) {
  auto blocked = std::make_unique<ProbeSender>(true);
  auto healthy = std::make_unique<ProbeSender>(false);
  auto* blockedProbe = blocked.get(); auto* healthyProbe = healthy.get();
  std::vector<std::unique_ptr<IOutputSender>> children;
  children.push_back(std::move(blocked)); children.push_back(std::move(healthy));
  auto sender = createIsolatedOutputSender(std::move(children), {"rtmp", "srt", "ndi"});
  ProgramFrame frame; frame.frameNumber = 1;
  const auto initial = sender->sync({"rtmp", "srt", "ndi"}, &frame, 0);
  const bool enteredBlocked = blockedProbe->await(1, 0);
  sender->submitAudio(std::vector<float>(960 * 2, 0.25f), 2, 48000);
  const bool healthyProgress = healthyProbe->await(1, 1);
  sender->interrupt("rtmp");
  EXPECT_TRUE(enteredBlocked);
  EXPECT_TRUE(initial.warnings.empty());
  EXPECT_EQ(initial.senders.front().status, "starting");
  EXPECT_TRUE(healthyProgress);
}

namespace {
class AacCaptureSender final : public IOutputSender {
 public:
  struct Seen {
    std::mutex mutex;
    std::vector<int64_t> anchorFrames;
  };
  explicit AacCaptureSender(std::shared_ptr<Seen> seen) : seen_(std::move(seen)) {}
  bool acceptsSharedAac() const override { return true; }
  void submitEncodedAudio(const ProgramAacPacket& packet) override {
    std::lock_guard<std::mutex> lock(seen_->mutex);
    seen_->anchorFrames.push_back(packet.anchorFrameNumber);
  }
  OutputSenderSession sync(const std::vector<std::string>&, const ProgramFrame*, double,
      const std::vector<OutputDestinationSettings>&, const std::vector<float>*, int, int) override { return {}; }
  OutputSenderSession fail(const std::string&, const std::string&, double) override { return {}; }
  OutputSenderSession recover(const std::string&, double, const std::string&) override { return {}; }
  OutputSenderSession session() const override { return {}; }

 private:
  std::shared_ptr<Seen> seen_;
};
}  // namespace

// The live core's composite anchors the shared AAC clock on the SAME Program
// tick the hardware encoder stamps pictures with, not on the render count.
TEST(IsolatedOutputSender, SharedAacAnchorsOnTheProgramTick) {
  auto seen = std::make_shared<AacCaptureSender::Seen>();
  std::vector<std::unique_ptr<IOutputSender>> children;
  children.push_back(std::make_unique<AacCaptureSender>(seen));
  auto sender = createIsolatedOutputSender(std::move(children), {"rtmp"});
  ProgramFrame frame;
  frame.frameNumber = 500;
  frame.programTick = 612;
  (void)sender->sync({"rtmp"}, &frame, 0);
  for (int block = 0; block < 10; ++block) {
    sender->submitAudio(std::vector<float>(960 * 2, 0.0f), 2, 48000);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(seen->mutex);
      if (!seen->anchorFrames.empty()) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  std::lock_guard<std::mutex> lock(seen->mutex);
#if defined(__APPLE__) && !COREVIDEO_STUB && COREVIDEO_HAS_VT_GPU_ENCODER
  // This build has the AudioToolbox shared AAC: no packet is a failure.
  ASSERT_FALSE(seen->anchorFrames.empty()) << "no shared AAC packet reached the destination";
#endif
  if (seen->anchorFrames.empty()) {
    std::fprintf(stderr, "[  SKIPPED ] IsolatedOutputSender.SharedAacAnchorsOnTheProgramTick "
                         "(no shared AAC encoder in this build) - this test did NOT run\n");
    return;
  }
  EXPECT_EQ(seen->anchorFrames.front(), 612);
}
