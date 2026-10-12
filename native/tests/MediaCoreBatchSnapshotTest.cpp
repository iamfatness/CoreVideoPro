#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include <gtest/gtest.h>
#include <memory>
#include <atomic>
#include <future>
#include <latch>
#include <thread>
#include "rpc/JsonRpcServer.h"

namespace {
// Observe a real snapshot dependency rather than adding production-only counters.
class CountingEncoder final : public corevideo::modules::IEncoderSink {
 public:
  void configureRecording(const corevideo::modules::RecordingSessionRequest&) override {}
  corevideo::modules::OutputSession start(const std::vector<std::string>&,
                                         const std::vector<std::string>&) override { return {}; }
  void submit(const corevideo::modules::ProgramFrame&) override {}
  corevideo::modules::OutputSession session() const override { ++reads; return {}; }
  mutable int reads = 0;
};
}

namespace {
class StalledStatusEncoder final : public corevideo::modules::IEncoderSink {
 public:
  void configureRecording(const corevideo::modules::RecordingSessionRequest&) override {}
  corevideo::modules::OutputSession start(const std::vector<std::string>&,
      const std::vector<std::string>&) override { return {}; }
  void submit(const corevideo::modules::ProgramFrame&) override {}
  corevideo::modules::OutputSession session() const override {
    if (stall.exchange(false)) { entered.count_down(); release.wait(); }
    corevideo::modules::OutputSession result;
    result.encoderName = "owned-observation";
    return result;
  }
  mutable std::atomic<bool> stall{false};
  mutable std::latch entered{1}, release{1};
};
}

TEST(MediaCoreBatchSnapshot, StatusAndRenderContinueWhileAudioOwnerQueryIsStalled) {
  auto modules = corevideo::modules::createStubModules();
  auto encoder = std::make_unique<StalledStatusEncoder>();
  auto* stalled = encoder.get(); modules.encoder = std::move(encoder);
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker(); std::mutex coreMutex;
  core.renderAudioOutputTick(coreMutex); // establish real owner observation
  stalled->stall = true;
  std::thread owner([&] { core.renderAudioOutputTick(coreMutex); });
  stalled->entered.wait();
  auto reader = std::async(std::launch::async, [&] {
    std::lock_guard<std::mutex> lock(coreMutex);
    corevideo::rpc::JsonRpcServer server(core);
    const auto before = core.sessionState();
    const auto health = server.handle(corevideo::rpc::Json::Object{{"type", "health"}});
    const auto profile = server.handle(corevideo::rpc::Json::Object{{"type", "handshake"}});
    core.renderDisplayTick();
    const auto after = core.sessionState();
    return std::make_pair(before.getString("encoder"), after.getNumber("programFrameCount"));
  });
  const auto ready = reader.wait_for(std::chrono::seconds(2));
  // Always release/join, even if the old nested-lock dependency regresses.
  stalled->release.count_down(); owner.join();
  ASSERT_EQ(ready, std::future_status::ready);
  const auto result = reader.get();
  EXPECT_EQ(result.first, "owned-observation");
  EXPECT_GT(result.second, 0);
  // Stub progress proves lock separation, not installed GPU/pixel acceptance.
}

TEST(MediaCoreBatchSnapshot, CapturesOnceAfterOrderedMutations) {
  auto modules = corevideo::modules::createStubModules();
  auto encoder = std::make_unique<CountingEncoder>();
  auto* counts = encoder.get();
  modules.encoder = std::move(encoder);
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();
  counts->reads = 0;
  const auto snapshot = core.sessionState();
  const int snapshotReads = counts->reads;
  // Live status reads consume the owner publication, including before the
  // first audio tick; they must not query the encoder to fill missing facts.
  ASSERT_EQ(snapshotReads, 0);
  counts->reads = 0;
  const auto result = core.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "first"}},
      corevideo::rpc::Json::Object{{"type", "set-overlay-asset"}, {"overlayId", "key"}, {"text", "Name"}},
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "last"}},
      corevideo::rpc::Json::Object{{"type", "set-overlay-asset"}, {"overlayId", "key"},
          {"enabled", false}, {"keyPhase", "hidden"}}});
  EXPECT_EQ(counts->reads, snapshotReads);
  EXPECT_EQ(result.getString("sceneId"), "last");
  EXPECT_EQ(result.getNumber("overlayCount"), 0);
  EXPECT_EQ(result.getNumber("programFrameCount"), snapshot.getNumber("programFrameCount"));

  counts->reads = 0;
  const auto direct = core.applyCommand(corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", "direct"}});
  EXPECT_EQ(counts->reads, snapshotReads);
  EXPECT_EQ(direct.getString("sceneId"), "direct");
}
