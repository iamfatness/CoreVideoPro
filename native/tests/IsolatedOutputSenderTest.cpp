#include "modules/IsolatedOutputSender.h"
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
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
