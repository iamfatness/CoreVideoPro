// The send decorator the Mac engine hands TalkbackChannels
// (zoom-engine/engine/main-queue-talkback-sdk.h). The manual queue stands in
// for dispatch_async(main); the clock is injected.
#include <gtest/gtest.h>

#include <deque>
#include <functional>
#include <thread>

#include "main-queue-talkback-sdk.h"

namespace {

struct RecordingSdk : zc::TalkbackSdk {
  struct Send { std::string ch; std::vector<int16_t> pcm; std::thread::id tid; };
  std::vector<Send> sends;
  int invites = 0, creates = 0, volumes = 0, removes = 0, destroys = 0;
  zc::TalkbackSdkEvents* events = nullptr;
  void SetEvents(zc::TalkbackSdkEvents* e) override { events = e; }
  bool MeetingSupportsTalkback() override { return true; }
  zc::TalkbackResult CreateChannels(unsigned int) override { ++creates; return zc::TalkbackCall::Ok; }
  zc::TalkbackResult InviteUsers(const std::string&, const std::vector<unsigned int>&) override {
    ++invites; return zc::TalkbackCall::Ok; }
  zc::TalkbackResult RemoveUsers(const std::string&, const std::vector<unsigned int>&) override {
    ++removes; return zc::TalkbackCall::Ok; }
  zc::TalkbackResult DestroyChannels(const std::vector<std::string>&) override {
    ++destroys; return zc::TalkbackCall::Ok; }
  zc::TalkbackResult SendAudio(const std::string& ch, const int16_t* pcm, int n) override {
    sends.push_back({ch, std::vector<int16_t>(pcm, pcm + n), std::this_thread::get_id()});
    return zc::TalkbackCall::Ok;
  }
  zc::TalkbackResult SetChannelBackgroundVolume(const std::string&, float) override {
    ++volumes; return zc::TalkbackCall::Ok; }
};

struct Rig {
  RecordingSdk inner;
  std::deque<std::function<void()>> queue;
  int64_t now = 0;
  engine_comms::MainQueueTalkbackSdk make(engine_comms::SendMode mode) {
    return engine_comms::MainQueueTalkbackSdk(
        &inner, mode, [this](std::function<void()> f) { queue.push_back(std::move(f)); },
        [this] { return now; });
  }
  void run() { while (!queue.empty()) { auto f = std::move(queue.front()); queue.pop_front(); f(); } }
};

}  // namespace

TEST(MainQueueTalkbackSdk, DirectModeForwardsSendOnTheCallerThread) {
  Rig r;
  auto sdk = r.make(engine_comms::SendMode::Direct);
  const int16_t pcm[] = {1, 2, 3};
  EXPECT_EQ(sdk.SendAudio("ch", pcm, 3), zc::TalkbackCall::Ok);
  ASSERT_EQ(r.inner.sends.size(), 1u);
  EXPECT_EQ(r.inner.sends[0].pcm, (std::vector<int16_t>{1, 2, 3}));
  EXPECT_EQ(r.inner.sends[0].tid, std::this_thread::get_id());
  EXPECT_TRUE(r.queue.empty());
}

TEST(MainQueueTalkbackSdk, MainQueueModeCopiesTheFrameAndForwardsOnlyWhenRun) {
  Rig r;
  auto sdk = r.make(engine_comms::SendMode::MainQueue);
  int16_t pcm[] = {1, 2, 3};
  EXPECT_EQ(sdk.SendAudio("ch", pcm, 3), zc::TalkbackCall::Ok);
  EXPECT_TRUE(r.inner.sends.empty()) << "nothing forwarded before the queue runs";
  pcm[0] = pcm[1] = pcm[2] = 0;  // the caller reuses its buffer
  r.run();
  ASSERT_EQ(r.inner.sends.size(), 1u);
  EXPECT_EQ(r.inner.sends[0].ch, "ch");
  EXPECT_EQ(r.inner.sends[0].pcm, (std::vector<int16_t>{1, 2, 3}));
}

TEST(MainQueueTalkbackSdk, NonSendCallsAreNeverHopped) {
  for (auto mode : {engine_comms::SendMode::Direct, engine_comms::SendMode::MainQueue}) {
    Rig r;
    auto sdk = r.make(mode);
    sdk.InviteUsers("ch", {1});
    sdk.CreateChannels(16);
    sdk.SetChannelBackgroundVolume("ch", 1.0f);
    sdk.RemoveUsers("ch", {1});
    sdk.DestroyChannels({"ch"});
    EXPECT_TRUE(r.queue.empty());
    EXPECT_EQ(r.inner.invites, 1);
    EXPECT_EQ(r.inner.creates, 1);
    EXPECT_EQ(r.inner.volumes, 1);
    EXPECT_EQ(r.inner.removes, 1);
    EXPECT_EQ(r.inner.destroys, 1);
    EXPECT_TRUE(sdk.MeetingSupportsTalkback());
  }
}

TEST(MainQueueTalkbackSdk, ShutdownDropsPostedSends) {
  Rig r;
  auto sdk = r.make(engine_comms::SendMode::MainQueue);
  const int16_t pcm[] = {1, 2, 3};
  for (int i = 0; i < 3; ++i) sdk.SendAudio("ch", pcm, 3);
  sdk.Shutdown();
  r.run();
  EXPECT_TRUE(r.inner.sends.empty());
  EXPECT_EQ(sdk.TakeHopStats().droppedAfterShutdown, 3u);
}

TEST(MainQueueTalkbackSdk, SendAfterShutdownIsNeverForwardedInEitherMode) {
  for (auto mode : {engine_comms::SendMode::Direct, engine_comms::SendMode::MainQueue}) {
    Rig r;
    auto sdk = r.make(mode);
    sdk.Shutdown();
    const int16_t pcm[] = {1};
    sdk.SendAudio("ch", pcm, 1);
    r.run();
    EXPECT_TRUE(r.inner.sends.empty());
  }
}

TEST(MainQueueTalkbackSdk, HopStatsMeasureThePostToRunDelay) {
  Rig r;
  auto sdk = r.make(engine_comms::SendMode::MainQueue);
  const int16_t pcm[] = {1};
  r.now = 0;
  sdk.SendAudio("ch", pcm, 1);
  r.now = 4000;
  r.run();
  const auto s = sdk.TakeHopStats();
  EXPECT_EQ(s.count, 1u);
  EXPECT_EQ(s.maxUs, 4000);
  EXPECT_EQ(s.p99Us, 4000);
  EXPECT_EQ(sdk.TakeHopStats().count, 0u) << "TakeHopStats resets the window";
}

TEST(MainQueueTalkbackSdk, SendModeFromEnv) {
  using engine_comms::SendMode;
  EXPECT_EQ(engine_comms::sendModeFromEnv("main"), SendMode::MainQueue);
  EXPECT_EQ(engine_comms::sendModeFromEnv("direct"), SendMode::Direct);
  // The shipped default; Task 5 (live spike) decides it and updates this line.
  EXPECT_EQ(engine_comms::sendModeFromEnv(nullptr), SendMode::Direct);
  EXPECT_EQ(engine_comms::sendModeFromEnv("bogus"), SendMode::Direct);
}
