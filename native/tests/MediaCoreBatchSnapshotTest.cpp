#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include <gtest/gtest.h>
#include <memory>

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
  ASSERT_TRUE(snapshotReads > 0);
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
