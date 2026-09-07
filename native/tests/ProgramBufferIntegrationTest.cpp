#include "core/MediaCore.h"
#include <gtest/gtest.h>
#include <deque>
#include <future>
#include <thread>

namespace {
using namespace corevideo;
class BufferedCompositor final : public modules::ICompositor {
 public:
  std::string rendererName() const override { return "buffer-test"; }
  void configureProgramBuffer(int frames) override { depth = frames; }
  int programBufferFrames() const override { return depth; }
  modules::ProgramFrame render(const modules::CompositorRenderPlan& plan,
      const std::vector<modules::VideoFrame>&) override {
    lastSkipCpuReadback = plan.skipCpuReadback;
    modules::ProgramFrame frame;
    frame.frameNumber = ++produced;
    frame.renderPlanId = plan.renderPlanId;
    frame.gpuComposed = true;
    return frame;
  }
  bool latestDeliveredProgramFrame(modules::ProgramFrame& out) const override {
    if (!latest.frameNumber) return false;
    out = latest;
    return true;
  }
  bool takeDeliveredProgramFrame(modules::ProgramFrame& out, int) override {
    if (packets.empty()) return false;
    out = packets.front(); packets.pop_front(); return true;
  }
  bool takeVcamNv12Shared(std::shared_ptr<const std::vector<uint8_t>>&, int&, int&) override {
    ++legacyTapReads; return false;
  }
  int depth = 0, produced = 0, legacyTapReads = 0;
  bool lastSkipCpuReadback = false;
  modules::ProgramFrame latest;
  std::deque<modules::ProgramFrame> packets;
};
class BufferEncoder final : public modules::IEncoderSink {
 public:
  void configureRecording(const modules::RecordingSessionRequest& request) override { requests.push_back(request); }
  modules::OutputSession start(const std::vector<std::string>&, const std::vector<std::string>&) override { return {}; }
  void submit(const modules::ProgramFrame& frame) override { frames.push_back(frame); }
  modules::OutputSession session() const override { return {}; }
  std::vector<modules::ProgramFrame> frames;
  std::vector<modules::RecordingSessionRequest> requests;
};
class BufferSender final : public modules::IOutputSender {
 public:
  modules::OutputSenderSession sync(const std::vector<std::string>& destinations,
      const modules::ProgramFrame*, double, const std::vector<modules::OutputDestinationSettings>&,
      const std::vector<float>*, int, int) override { last = destinations; ++calls; return {}; }
  modules::OutputSenderSession fail(const std::string&, const std::string&, double) override { return {}; }
  modules::OutputSenderSession recover(const std::string&, double, const std::string&) override { return {}; }
  modules::OutputSenderSession session() const override { return {}; }
  std::vector<std::string> last;
  int calls = 0;
};
class PresentationTimeSource final : public modules::IMediaFrameSource {
 public:
  std::vector<modules::VideoFrame> pollMediaFrames(
      const std::vector<modules::CompositorRenderPlanLayer>&, int64_t) override { return {}; }
  std::vector<modules::VideoFrame> pollMediaFramesAt100ns(
      const std::vector<modules::CompositorRenderPlanLayer>&, int64_t timestamp100ns) override {
    selectedTimes.push_back(timestamp100ns);
    return {};
  }
  std::vector<int64_t> selectedTimes;
};
}

TEST(ProgramBufferIntegration, MediaSelectionUsesScheduledContentTime) {
  auto modules = corevideo::modules::createStubModules();
  auto source = std::make_unique<PresentationTimeSource>();
  auto* observed = source.get();
  modules.mediaFrames = std::move(source);
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();
  observed->selectedTimes.clear();
  core.renderDisplayTick(0, 10'000'000'000);
  core.renderDisplayTick(1, 10'000'000'000);
  core.renderDisplayTick(60, 10'000'000'000);
  ASSERT_EQ(observed->selectedTimes.size(), 3u);
  EXPECT_EQ(observed->selectedTimes[0], 100000000);
  EXPECT_EQ(observed->selectedTimes[1], 100166666);
  EXPECT_EQ(observed->selectedTimes[2], 110000000);
}

TEST(ProgramBufferIntegration, ProgramAttributionAdvancesOnlyOnDelivery) {
  auto modules = corevideo::modules::createStubModules();
  auto compositor = std::make_unique<BufferedCompositor>();
  auto* buffer = compositor.get();
  modules.compositor = std::move(compositor);
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();
  core.renderDisplayTick();
  EXPECT_EQ(core.sessionState().getNumber("programFrameCount"), 0);
  auto plan = std::make_shared<corevideo::modules::CompositorRenderPlan>();
  plan->sceneId = "actually-delivered";
  buffer->latest.frameNumber = 7;
  buffer->latest.deliverySequence = 1;
  buffer->latest.renderPlanEvidence = plan;
  core.renderDisplayTick();
  const auto snapshot = core.sessionState();
  EXPECT_EQ(snapshot.getNumber("programFrameCount"), 7);
  ASSERT_NE(snapshot.get("programFrame"), nullptr);
  EXPECT_EQ(snapshot.get("programFrame")->getString("sceneId"), "actually-delivered");
}

TEST(ProgramBufferIntegration, OutputPreservesPacketPixelsAndScheduledTimestamp) {
  auto modules = corevideo::modules::createStubModules();
  auto compositor = std::make_unique<BufferedCompositor>();
  auto* buffer = compositor.get();
  auto encoder = std::make_unique<BufferEncoder>();
  auto* recorded = encoder.get();
  modules.compositor = std::move(compositor);
  modules.encoder = std::move(encoder);
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();
  (void)core.applyCommand(corevideo::rpc::Json::Object{{"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"recording"}}});
  core.renderDisplayTick();
  EXPECT_TRUE(buffer->lastSkipCpuReadback);
  corevideo::modules::ProgramFrame packet;
  packet.frameNumber = 41; packet.deliverySequence = 1;
  packet.timelineTimestamp100ns = 987654;
  packet.programNv12Width = 2; packet.programNv12Height = 2;
  packet.programNv12Shared = std::make_shared<const std::vector<uint8_t>>(6, 42);
  buffer->packets.push_back(packet);
  std::mutex coreMutex;
  core.renderVideoOutputTick(coreMutex);
  ASSERT_EQ(recorded->frames.size(), 1u);
  EXPECT_EQ(recorded->frames.front().frameNumber, 41);
  EXPECT_EQ(recorded->frames.front().timelineTimestamp100ns, 987654);
  EXPECT_EQ(recorded->frames.front().programNv12Shared, packet.programNv12Shared);
  EXPECT_EQ(buffer->legacyTapReads, 0);
  core.renderVideoOutputTick(coreMutex);
  EXPECT_EQ(recorded->frames.size(), 1u);
}

TEST(ProgramBufferIntegration, StopPropagatesWithoutNewFrameOrRenderLock) {
  auto modules = corevideo::modules::createStubModules();
  modules.compositor = std::make_unique<BufferedCompositor>();
  modules.encoder = std::make_unique<BufferEncoder>();
  auto sender = std::make_unique<BufferSender>();
  auto* observed = sender.get();
  modules.outputSender = std::move(sender);
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();
  (void)core.applyCommand(corevideo::rpc::Json::Object{{"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{"rtmp"}}});
  std::mutex coreMutex;
  core.renderVideoOutputTick(coreMutex);
  ASSERT_EQ(observed->last.size(), 1u);
  (void)core.applyCommand(corevideo::rpc::Json::Object{{"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{}}});
  std::unique_lock<std::mutex> renderHold(coreMutex);
  auto delivery = std::async(std::launch::async, [&] { core.renderVideoOutputTick(coreMutex); });
  const bool unblocked = delivery.wait_for(std::chrono::milliseconds(500)) == std::future_status::ready;
  renderHold.unlock();
  delivery.get();
  EXPECT_TRUE(unblocked);
  EXPECT_TRUE(observed->last.empty());
  EXPECT_EQ(observed->calls, 2);
}

TEST(ProgramBufferIntegration, RecordingCaptureEpochSurvivesSettingsAndRepeatedStart) {
  auto modules = corevideo::modules::createStubModules();
  modules.compositor = std::make_unique<BufferedCompositor>();
  auto encoder = std::make_unique<BufferEncoder>();
  auto* recorded = encoder.get();
  modules.encoder = std::move(encoder);
  corevideo::core::MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();
  const corevideo::rpc::Json start = corevideo::rpc::Json::Object{
      {"type", "start-recording-session"}, {"sessionId", "capture-epoch-test"}};
  (void)core.applyCommand(start);
  ASSERT_FALSE(recorded->requests.empty());
  const auto epoch = recorded->requests.back().captureEpoch100ns;
  EXPECT_TRUE(epoch > 0);
  (void)core.applyCommand(corevideo::rpc::Json::Object{{"type", "set-recording-targets"}, {"quality", "high"}});
  EXPECT_EQ(recorded->requests.back().captureEpoch100ns, epoch);
  const auto configurations = recorded->requests.size();
  (void)core.applyCommand(start);
  EXPECT_EQ(recorded->requests.size(), configurations);
  (void)core.applyCommand(corevideo::rpc::Json::Object{{"type", "stop-recording-session"}});
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  (void)core.applyCommand(start);
  EXPECT_TRUE(recorded->requests.back().captureEpoch100ns > epoch);
}
