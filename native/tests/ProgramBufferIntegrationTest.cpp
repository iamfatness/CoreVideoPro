#include "core/MediaCore.h"
#include <set>
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

namespace {
// A Zoom source that hands the render gather one decoded I420 frame per selected
// ISO participant, advancing frameId every poll so nothing downstream can dedup
// the difference away.
class IsoZoomSource final : public corevideo::modules::IZoomCaptureSource {
 public:
  std::vector<corevideo::modules::VideoFrame> pollVideoFrames() override {
    ++frameId;
    std::vector<corevideo::modules::VideoFrame> frames;
    for (const auto& participantId : participants) {
      corevideo::modules::VideoFrame frame;
      frame.participantId = participantId;
      frame.width = frame.height = 2;
      frame.i420Width = frame.i420Height = 2;
      frame.i420 = std::make_shared<const std::vector<uint8_t>>(6, 128);
      frame.frameId = frameId;
      frames.push_back(std::move(frame));
    }
    return frames;
  }
  std::vector<corevideo::modules::AudioFrame> pollAudioFrames() override { return {}; }
  std::vector<std::string> participants{"host", "guest"};
  int64_t frameId = 0;
};

// Counts which submit boundary each kind of media crossed. The point of the ISO
// cadence test is WHERE the submit happened, so nothing here inspects pixels.
class IsoCountingEncoder final : public corevideo::modules::IEncoderSink {
 public:
  void configureRecording(const corevideo::modules::RecordingSessionRequest&) override {}
  corevideo::modules::OutputSession start(
      const std::vector<std::string>& destinations, const std::vector<std::string>&) override {
    if (std::find(destinations.begin(), destinations.end(), "recording") != destinations.end()) {
      session_.recordingStatus = "recording";
    }
    session_.active = true;
    session_.destinations = destinations;
    return session_;
  }
  void submit(const corevideo::modules::ProgramFrame&) override { ++programSubmits; }
  void submitIsoVideo(const std::vector<corevideo::modules::IsoSourceVideoFrame>& sources) override {
    ++isoSubmits;
    isoFramesSubmitted += sources.size();
    for (const auto& source : sources) {
      lastTimestamps100ns.push_back(source.timelineTimestamp100ns);
    }
  }
  corevideo::modules::OutputSession session() const override { return session_; }
  // The arming commands run synchronously on the direct/test path (applyCommands
  // ticks), so the counters must start from the first tick under test, not from
  // whatever setup already pushed through.
  void reset() {
    programSubmits = 0;
    isoSubmits = 0;
    isoFramesSubmitted = 0;
    lastTimestamps100ns.clear();
  }
  int programSubmits = 0;
  int isoSubmits = 0;
  size_t isoFramesSubmitted = 0;
  std::vector<int64_t> lastTimestamps100ns;

 private:
  corevideo::modules::OutputSession session_;
};

struct IsoCadenceRig {
  std::unique_ptr<corevideo::core::MediaCore> core;
  IsoCountingEncoder* encoder = nullptr;
};

IsoCadenceRig makeRecordingIsoRig() {
  auto modules = corevideo::modules::createStubModules();
  modules.zoom = std::make_unique<IsoZoomSource>();
  auto encoder = std::make_unique<IsoCountingEncoder>();
  IsoCadenceRig rig;
  rig.encoder = encoder.get();
  modules.encoder = std::move(encoder);
  rig.core = std::make_unique<corevideo::core::MediaCore>(std::move(modules));
  const corevideo::rpc::Json isoIds =
      corevideo::rpc::Json::Array{std::string("zoom:host"), std::string("zoom:guest")};
  (void)rig.core->applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-recording-targets"},
      {"targetFolder", "Recordings/CoreVideo Pro/tests"},
      {"filenamePrefix", "iso-cadence"},
      {"format", "mp4"},
      {"isoSourceIds", isoIds}});
  (void)rig.core->applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-recording-session"}, {"sessionId", "iso-cadence"}, {"isoSourceIds", isoIds}});
  (void)rig.core->applyCommand(corevideo::rpc::Json::Object{
      {"type", "start-program-output"},
      {"destinations", corevideo::rpc::Json::Array{std::string("recording")}},
      {"isoSourceIds", isoIds}});
  return rig;
}
}  // namespace

// S1/ISO-1 (cadence): ISO video is submitted by its OWN signalled worker, driven
// by ISO frame ARRIVAL — not by the Program video tick and not by the 20ms audio
// grid. It rode the Program tick before this, gated on `programSubmitted`, which
// made a second free-running 60Hz clock sample a 60Hz producer: measured 50.1-52.9
// fps of ISO for a ~60fps source, 45.2 for a ~240fps one (NON-MONOTONIC), with
// 15-22% of submissions rejected as duplicates. This asserts WHERE the submit
// happens and that it is arrival-driven, not how it is implemented.
TEST(ProgramBufferIntegration, IsoVideoIsSubmittedByItsOwnArrivalDrivenWorker) {
  auto rig = makeRecordingIsoRig();
  auto& core = *rig.core;
  core.setVideoOutputTickRunning(true);
  std::mutex coreMutex;
  // The arming commands tick the render path synchronously on the direct/test
  // path, so drain whatever they already queued before counting.
  core.renderIsoVideoTick();
  rig.encoder->reset();

  core.renderDisplayTick();
  const int programBefore = rig.encoder->programSubmits;
  core.renderAudioOutputTick(coreMutex);
  EXPECT_EQ(rig.encoder->isoSubmits, 0)
      << "the 20ms audio grid must not sample ISO video while the ISO worker owns it";
  EXPECT_EQ(rig.encoder->programSubmits, programBefore)
      << "Program is already owned by the video tick; the audio worker must not submit it either";

  core.renderVideoOutputTick(coreMutex);
  EXPECT_EQ(rig.encoder->isoSubmits, 0)
      << "the PROGRAM video tick must not carry ISO: that coupling is the sampling bug";
  EXPECT_EQ(rig.encoder->programSubmits, programBefore + 1)
      << "Program keeps reserved priority and is unaffected";

  core.renderIsoVideoTick();
  EXPECT_EQ(rig.encoder->isoSubmits, 1) << "the ISO worker carries ISO video";
  EXPECT_EQ(rig.encoder->isoFramesSubmitted, 2u) << "both selected ISO sources ride the same drain";
  for (const auto stamp : rig.encoder->lastTimestamps100ns) {
    EXPECT_GT(stamp, 0) << "every ISO frame must carry the arrival timeline stamp";
  }

  // Nothing new rendered: the drain has nothing to do, and must not resubmit a
  // held frame (that would put the sink's dedup in charge of correctness again).
  core.renderIsoVideoTick();
  EXPECT_EQ(rig.encoder->isoSubmits, 1) << "no new ISO frame means no new ISO submit";
}

// THE POINT OF THE WHOLE FIX: ISO does not SAMPLE, it DRAINS. Renders that happen
// between two drains must all reach the encoder — otherwise a source faster than
// the drain silently loses frames, which is exactly what the Program-paced version
// did (and why a ~240fps source wrote FEWER stem frames than a ~30fps one).
TEST(ProgramBufferIntegration, IsoVideoDrainsEveryRenderedFrameNotJustTheLatest) {
  auto rig = makeRecordingIsoRig();
  auto& core = *rig.core;
  core.setVideoOutputTickRunning(true);
  std::mutex coreMutex;
  core.renderIsoVideoTick();
  rig.encoder->reset();

  // Deliberately at the per-source pending cap (kMaxPendingIsoFramesPerSource):
  // everything queued between two drains must survive, right up to the bound.
  constexpr int kRenders = 4;
  for (int i = 0; i < kRenders; ++i) {
    core.renderDisplayTick();
  }
  core.renderIsoVideoTick();
  EXPECT_EQ(rig.encoder->isoFramesSubmitted, static_cast<size_t>(kRenders) * 2u)
      << "every distinct frame from every render must be drained, not just the last";
  // Distinct arrival stamps, so the writers can space them on the timeline
  // rather than colliding on one instant.
  std::set<int64_t> stamps(rig.encoder->lastTimestamps100ns.begin(),
                           rig.encoder->lastTimestamps100ns.end());
  EXPECT_GE(stamps.size(), 2u) << "frames from different renders must not share one stamp";
}

// The direct/test path (no video tick) must keep working exactly as before:
// videoOutputTickRunning_ is set true unconditionally in any real run, but the
// guard exists so single-threaded callers still get Program AND ISO out.
TEST(ProgramBufferIntegration, IsoVideoStillRidesTheAudioWorkerWithNoVideoTick) {
  auto rig = makeRecordingIsoRig();
  auto& core = *rig.core;
  std::mutex coreMutex;
  rig.encoder->reset();
  core.renderDisplayTick();
  core.renderAudioOutputTick(coreMutex);
  EXPECT_EQ(rig.encoder->isoSubmits, 1)
      << "with no video tick running the audio worker owns both Program and ISO";
  EXPECT_EQ(rig.encoder->isoFramesSubmitted, 2u);
  EXPECT_GT(rig.encoder->programSubmits, 0);
}
