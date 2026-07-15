#include "modules/AsyncOutputSender.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace {
using namespace corevideo::modules;

class BlockingOutputSender final : public IOutputSender {
 public:
  std::atomic<int> activeSyncs{0};
  std::atomic<int> stopSyncs{0};
  std::atomic<int> interrupts{0};

  OutputSenderSession sync(
      const std::vector<std::string>& destinations,
      const ProgramFrame*,
      double,
      const std::vector<OutputDestinationSettings>&,
      const std::vector<float>*,
      int,
      int) override {
    const bool active = std::find(destinations.begin(), destinations.end(), "rtmp") != destinations.end();
    if (active) {
      ++activeSyncs;
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [&] { return released_; });
      released_ = false;
    } else {
      ++stopSyncs;
    }
    OutputSenderSession result;
    result.status = active ? "live" : "idle";
    result.activeSenderCount = active ? 1 : 0;
    return result;
  }

  OutputSenderSession fail(const std::string&, const std::string&, double) override { return {}; }
  OutputSenderSession recover(const std::string&, double, const std::string&) override { return {}; }
  OutputSenderSession session() const override { return {}; }

  void interrupt(const std::string& destination) override {
    if (destination != "rtmp") {
      return;
    }
    ++interrupts;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool released_ = false;
};
}  // namespace

TEST(AsyncOutputSender, BlockedNetworkWriteDoesNotBlockCallerAndStopInterruptsIt) {
  auto inner = std::make_unique<BlockingOutputSender>();
  auto* raw = inner.get();
  AsyncOutputSender sender(std::move(inner));
  ProgramFrame frame;
  frame.frameNumber = 1;

  const auto start = std::chrono::steady_clock::now();
  sender.sync({"rtmp"}, &frame, 0.0);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  EXPECT_LT(elapsed.count(), 100);

  for (int attempt = 0; attempt < 100 && raw->activeSyncs.load() == 0; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(raw->activeSyncs.load(), 1);

  sender.sync({}, &frame, 20.0);
  ASSERT_TRUE(sender.drainForTest(std::chrono::seconds(1)));
  EXPECT_GE(raw->interrupts.load(), 1);
  EXPECT_EQ(raw->stopSyncs.load(), 1);
}

TEST(AsyncOutputSender, DropsStalePendingSyncsToLatest) {
  auto inner = std::make_unique<BlockingOutputSender>();
  auto* raw = inner.get();
  AsyncOutputSender sender(std::move(inner));
  ProgramFrame frame;
  sender.sync({"rtmp"}, &frame, 0.0);
  for (int attempt = 0; attempt < 100 && raw->activeSyncs.load() == 0; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (int i = 1; i <= 20; ++i) {
    frame.frameNumber = i;
    sender.sync({"rtmp"}, &frame, i * 20.0);
  }
  EXPECT_GT(sender.droppedSyncs(), 0u);
  sender.interrupt("rtmp");
}
