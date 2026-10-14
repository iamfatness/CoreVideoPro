#include "rpc/ResourceEventLane.h"
#include <gtest/gtest.h>
#include "core/ResourceEventStaging.h"
#include "core/MediaCore.h"
#include "core/CoreStateCapture.h"
#include "rpc/JsonRpcServer.h"
#include <future>
#include <thread>
#include <atomic>
#include <sstream>

using corevideo::rpc::ResourceEventLane;

TEST(ResourceEventLane, InitialRosterCannotDiscardProgramOrOtherSurfaceAnnouncements) {
  ResourceEventLane lane;
  EXPECT_TRUE(lane.push("program", "program-handle").admitted);
  for (int i=0; i<10; ++i) EXPECT_TRUE(lane.push("participant-" + std::to_string(i), "surface").admitted);
  EXPECT_TRUE(lane.push("preview", "preview-handle").admitted);
  EXPECT_TRUE(lane.push("multiview", "wall-handle").admitted);
  EXPECT_EQ(lane.size(), 13u); EXPECT_EQ(lane.refused(), 0u);
  EXPECT_EQ(lane.popFront().message, "program-handle");
}
TEST(ResourceEventLane, ReplacementRetiresOutsideCallerLockAndKeepsLatestAnnouncementOrder) {
  ResourceEventLane lane;
  EXPECT_TRUE(lane.push("program", "old").admitted);
  EXPECT_TRUE(lane.push("preview", "preview").admitted);
  auto result = lane.push("program", "new");
  EXPECT_TRUE(result.admitted); EXPECT_EQ(result.retired, "old");
  EXPECT_EQ(lane.size(), 2u); EXPECT_EQ(lane.coalesced(), 1u);
  EXPECT_EQ(lane.popFront().message, "preview");
  EXPECT_EQ(lane.popFront().message, "new"); EXPECT_EQ(lane.bytes(), 0u);
}
TEST(ResourceEventLane, PressureRefusesNewKeysWithoutEvictingExistingResourceIdentity) {
  ResourceEventLane lane;
  for (int i=0; i<ResourceEventLane::Capacity; ++i) EXPECT_TRUE(lane.push(std::to_string(i), "handle").admitted);
  EXPECT_FALSE(lane.push("overflow", "not-admitted").admitted);
  EXPECT_TRUE(lane.push("0", "replacement").admitted);
  EXPECT_EQ(lane.size(), ResourceEventLane::Capacity); EXPECT_EQ(lane.refused(), 1u);
  EXPECT_FALSE(lane.push("0", std::string(ResourceEventLane::ByteBudget + 1, 'x')).admitted);
  EXPECT_LE(lane.bytes(), ResourceEventLane::ByteBudget);
}

TEST(ResourceEventStaging, SaturationDoesNotBuildOrEvictAcceptedBarriersAndDrainRetiresOutsideOwner) {
  using Staging = corevideo::core::ResourceEventStaging;
  Staging stage;
  int built = 0;
  std::weak_ptr<const corevideo::rpc::Json::Object> lifetime;
  for (size_t i = 0; i < Staging::Capacity; ++i) {
    EXPECT_TRUE(stage.admit(Staging::Kind::Participant, 8192, [&] {
      ++built;
      corevideo::rpc::Json event = corevideo::rpc::Json::Object{{"generation", double(i)}};
      if (i == 0) lifetime = std::get<corevideo::rpc::Json::ObjectValue>(event.value());
      return event;
    }));
  }
  for (int i = 0; i < 1000; ++i)
    EXPECT_FALSE(stage.admit(Staging::Kind::Participant, 8192, [&] { ++built; return corevideo::rpc::Json(true); }));
  EXPECT_EQ(built, 64); EXPECT_EQ(stage.facts().refused, 1000u);
  auto owned = stage.drain();
  ASSERT_EQ(owned.size(), 64u); EXPECT_FALSE(lifetime.expired());
  for (size_t i = 0; i < owned.size(); ++i) EXPECT_EQ(owned[i].getNumber("generation"), double(i));
  EXPECT_EQ(stage.facts().bytes, 0u);
  owned.clear(); EXPECT_TRUE(lifetime.expired());
  EXPECT_TRUE(stage.admit(Staging::Kind::Program, Staging::ByteBudget, [] { return corevideo::rpc::Json(true); }));
  EXPECT_FALSE(stage.admit(Staging::Kind::Program, 1, [&] { ++built; return corevideo::rpc::Json(true); }));
  EXPECT_EQ(built, 64);
}

TEST(ResourceEventStaging, OversizedSourceFootprintRefusesBeforeConstructionAndSelectiveDrainPreservesOrder) {
  using Staging = corevideo::core::ResourceEventStaging;
  Staging stage; bool built = false;
  corevideo::modules::ParticipantSharedTexture source;
  source.participantId.assign(Staging::ByteBudget, '\x01');
  EXPECT_FALSE(stage.admit(Staging::Kind::Participant, Staging::bound(source), [&] { built = true; return corevideo::rpc::Json(true); }));
  EXPECT_FALSE(built);
  stage.admit(Staging::Kind::Program, 8192, [] { return corevideo::rpc::Json(1); });
  stage.admit(Staging::Kind::Participant, 8192, [] { return corevideo::rpc::Json(2); });
  stage.admit(Staging::Kind::Preview, 8192, [] { return corevideo::rpc::Json(3); });
  auto participant = stage.drain(Staging::Kind::Participant);
  ASSERT_EQ(participant.size(), 1u); EXPECT_EQ(participant[0].asNumber(), 2);
  auto rest = stage.drain(); ASSERT_EQ(rest.size(), 2u);
  EXPECT_EQ(rest[0].asNumber(), 1); EXPECT_EQ(rest[1].asNumber(), 3);
}

TEST(ResourceEventLane, SaturatedAdmissionCancelsOnStopWithoutWaitingForBlockedWriter) {
  ResourceEventLane lane;
  for (int i = 0; i < ResourceEventLane::Capacity; ++i) lane.push(std::to_string(i), "held");
  std::mutex mutex; std::condition_variable changed; bool stopped = false;
  auto admission = std::async(std::launch::async, [&] {
    std::unique_lock lock(mutex);
    return lane.waitForCapacity("new-generation", 100, lock, changed, [&] { return stopped; });
  });
  EXPECT_EQ(admission.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
  { std::lock_guard lock(mutex); stopped = true; }
  changed.notify_all();
  ASSERT_EQ(admission.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_FALSE(admission.get()); EXPECT_EQ(lane.size(), ResourceEventLane::Capacity);
  EXPECT_EQ(lane.popFront().key, "0");
}

namespace {
class ResourceExportCompositor final : public corevideo::modules::ICompositor {
 public:
  std::string rendererName() const override { return "resource-staging-test"; }
  corevideo::modules::ProgramFrame render(const corevideo::modules::CompositorRenderPlan&,
      const std::vector<corevideo::modules::VideoFrame>&) override {
    corevideo::modules::ProgramFrame frame; frame.frameNumber = ++produced; frame.gpuComposed = true;
    frame.sharedTexture.sharedHandleHex = "0x" + std::to_string(frame.frameNumber);
    frame.sharedTexture.width = frame.sharedTexture.height = 16;
    corevideo::modules::ParticipantSharedTexture source;
    source.participantId = "guest"; source.sharedHandleHex = "0xBEEF";
    source.width = source.height = 16; source.frameNumber = produced;
    frame.participantSharedTextures.push_back(std::move(source));
    return frame;
  }
  std::atomic<int> produced{0};
};
}

TEST(ResourceEventStaging, StalledFormatterCannotBlockProgramAndLatestStructureRetriesAfterDrain) {
  using namespace corevideo;
  for (const int stallMs : {20, 40, 100, 500}) {
    auto modules = modules::createStubModules();
    modules.compositor = std::make_unique<ResourceExportCompositor>();
    core::MediaCore core(std::move(modules)); std::mutex mutex;
    core.renderDisplayTick();
    std::promise<void> entered, release; auto unblocked = release.get_future();
    auto formatter = std::async(std::launch::async, [&] {
      std::vector<rpc::Json> batch;
      { std::lock_guard lock(mutex); batch = core.drainResourceEvents(); }
      entered.set_value(); unblocked.wait();
      for (const auto& event : batch) (void)event.stringify();
      return batch.size();
    });
    const auto enteredReady = entered.get_future().wait_for(std::chrono::seconds(2));
    auto production = std::async(std::launch::async, [&] {
      const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(stallMs);
      for (int i = 0; i < 100; ++i) { std::lock_guard lock(mutex); core.renderDisplayTick(); }
      do { std::lock_guard lock(mutex); core.renderDisplayTick(); (void)core.sessionState(); }
      while (std::chrono::steady_clock::now() < until);
      std::lock_guard lock(mutex); return core.sessionState();
    });
    const auto ready = production.wait_for(std::chrono::seconds(2));
    release.set_value();
    ASSERT_EQ(enteredReady, std::future_status::ready); ASSERT_EQ(ready, std::future_status::ready);
    EXPECT_GT(formatter.get(), 0u);
    const auto snapshot = production.get();
    const auto* facts = snapshot.get("realtimeEvidence")->get("resourceEventStaging");
    ASSERT_NE(facts, nullptr); EXPECT_LE(facts->getNumber("items"), 64);
    EXPECT_LE(facts->getNumber("ownedByteBound"), double(core::ResourceEventStaging::ByteBudget));
    EXPECT_GT(facts->getNumber("refused"), 0);
    (void)core.drainResourceEvents();
    core.renderDisplayTick();
    auto latest = core.drainResourceEvents(); bool program = false;
    for (const auto& event : latest) if (event.getString("type") == "program-shared-texture") {
      program = true;
      EXPECT_EQ(event.get("texture")->getString("sharedHandleHex"), "0x" + std::to_string(int(core.sessionState().getNumber("programFrameCount"))));
    }
    EXPECT_TRUE(program);
  }
}

TEST(ResourceEventStaging, GradeAnnotationCapturesRevisionBeforeOffLockAssembly) {
  using namespace corevideo;
  core::SourceGradeState grades;
  modules::VideoFrame frame; frame.participantId = "guest"; frame.sourceEpoch = 7; grades.observe({frame});
  grades.apply(rpc::Json::Object{{"sourceId", "guest"}, {"sourceEpoch", 7}, {"expectedRevision", 0}}, {});
  auto batch = grades.captureAnnotations({rpc::Json::Object{{"sourceId", "guest"}, {"revision", 4}}});
  grades.apply(rpc::Json::Object{{"sourceId", "guest"}, {"sourceEpoch", 7}, {"expectedRevision", 1}}, {});
  auto events = std::move(batch).assemble(); ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].getNumber("appliedRevision"), 1); EXPECT_EQ(events[0].getNumber("revision"), 4);
}

TEST(ResourceEventStaging, ProductionRpcPumpFormatsOutsideRenderLock) {
  using namespace corevideo;
  for (const int stallMs : {20, 40, 100, 500}) {
    auto modules = modules::createStubModules();
    auto compositor = std::make_unique<ResourceExportCompositor>(); auto* produced = &compositor->produced;
    modules.compositor = std::move(compositor); core::MediaCore core(std::move(modules));
    std::promise<void> releaseAssembly, releaseFormatter, entered;
    auto assembling = releaseAssembly.get_future(); auto formatting = releaseFormatter.get_future();
    std::atomic<bool> first{true};
    rpc::JsonRpcServer server(core, {}, [&](const core::CoreStateCapture& capture, core::SnapshotProjection kind) {
      assembling.wait(); return capture.assemble(kind);
    }, [&](const rpc::Json& event) {
      if (first.exchange(false)) { entered.set_value(); formatting.wait(); }
      return event.stringify();
    });
    std::istringstream input("{\"id\":1,\"type\":\"snapshot\"}\n"); std::ostringstream output;
    auto running = std::async(std::launch::async, [&] { server.run(input, output); });
    const auto entry = entered.get_future().wait_for(std::chrono::seconds(2));
    const int before = produced->load();
    const auto start = std::chrono::steady_clock::now();
    while ((std::chrono::steady_clock::now() - start < std::chrono::milliseconds(stallMs) || produced->load() <= before) &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool advanced = produced->load() > before;
    releaseFormatter.set_value(); releaseAssembly.set_value();
    running.get();
    ASSERT_EQ(entry, std::future_status::ready); EXPECT_TRUE(advanced);
    EXPECT_NE(output.str().find("program-shared-texture"), std::string::npos);
  }
}
