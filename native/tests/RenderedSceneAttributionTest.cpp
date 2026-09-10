#include "core/MediaCore.h"
#include "core/RenderedSceneAttributionPolicy.h"
#include "core/TakeRecordPolicy.h"
#include "modules/Interfaces.h"
#include "modules/ZoomSubscriptionChurnPolicy.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace {

using corevideo::core::MediaCore;
using corevideo::core::RenderedSceneAttributionPolicy;
using corevideo::core::TakeRecordPolicy;
using corevideo::modules::ZoomSubscriptionChurnPolicy;

// Emulates the shipping D3D program-buffer adapter closely enough to observe the
// snapshot's rendered-scene attribution: render() attaches the plan it was given
// as renderPlanEvidence (exactly as D3D11CompositorAdapter does at its
// `if (buffered)` submit), and the delivery side republishes it as `latest` with
// an advancing deliverySequence.
//
// `deliver = false` reproduces the shape the live show hit: the buffer keeps
// handing back the SAME delivered frame because the export it pairs `latest_`
// with was busy (or a packet expired and cleared it). Program keeps compositing;
// the peek stops advancing.
class DeliveringCompositor final : public corevideo::modules::ICompositor {
 public:
  std::string rendererName() const override { return "attribution-test"; }
  void configureProgramBuffer(int frames) override { depth = frames; }
  int programBufferFrames() const override { return depth; }
  corevideo::modules::ProgramFrame render(
      const corevideo::modules::CompositorRenderPlan& plan,
      const std::vector<corevideo::modules::VideoFrame>&) override {
    corevideo::modules::ProgramFrame frame;
    frame.frameNumber = ++produced;
    frame.renderPlanId = plan.renderPlanId;
    frame.gpuComposed = true;
    frame.renderPlanEvidence =
        std::make_shared<const corevideo::modules::CompositorRenderPlan>(plan);
    if (deliver) {
      latest = frame;
      latest.deliverySequence = ++deliverySequence;
      hasLatest = true;
    }
    return frame;
  }
  bool latestDeliveredProgramFrame(corevideo::modules::ProgramFrame& out) const override {
    if (!hasLatest) return false;
    out = latest;
    return true;
  }
  int depth = 0, produced = 0;
  long long deliverySequence = 0;
  bool deliver = true, hasLatest = false;
  corevideo::modules::ProgramFrame latest;
};

void loadTilesScene(MediaCore& core, const char* sceneId, const std::vector<std::string>& members) {
  corevideo::rpc::Json::Array memberJson;
  for (const auto& member : members) memberJson.push_back(corevideo::rpc::Json{member});
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json{corevideo::rpc::Json::Object{
          {"type", corevideo::rpc::Json{"load-scene-graph"}},
          {"sceneId", corevideo::rpc::Json{sceneId}},
          {"routes", corevideo::rpc::Json{corevideo::rpc::Json::Array{}}},
          {"tiles", corevideo::rpc::Json{corevideo::rpc::Json::Object{
              {"layerId", corevideo::rpc::Json{std::string("tiles:") + sceneId}},
              {"members", corevideo::rpc::Json{memberJson}},
              {"style", corevideo::rpc::Json{corevideo::rpc::Json::Object{
                  {"fillMode", corevideo::rpc::Json{"auto"}},
                  {"backgroundColor", corevideo::rpc::Json{"#101418"}}}}}}}}}}});
}

const corevideo::rpc::Json* programFrameNode(const corevideo::rpc::Json& snapshot) {
  return snapshot.get("programFrame");
}

std::string renderedSceneId(const MediaCore& core) {
  const auto snapshot = core.sessionState();
  const auto* node = programFrameNode(snapshot);
  if (!node) return "<missing>";
  return node->getString("sceneId");
}

std::string attribution(const MediaCore& core) {
  const auto snapshot = core.sessionState();
  const auto* node = programFrameNode(snapshot);
  if (!node) return "<missing>";
  return node->getString("sceneIdAttribution");
}

}  // namespace

// ---------------------------------------------------------------------------
// The defect: the program frame's scene id did not follow a Take.
// ---------------------------------------------------------------------------

TEST(RenderedSceneAttribution, TheRenderedSceneIdFollowsATake) {
  auto modules = corevideo::modules::createStubModules();
  modules.compositor = std::make_unique<DeliveringCompositor>();
  MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  loadTilesScene(core, "scene-a", {"zoom:1", "zoom:2"});
  core.renderDisplayTick();
  EXPECT_EQ(renderedSceneId(core), "scene-a");
  EXPECT_EQ(attribution(core), "live");

  loadTilesScene(core, "scene-b", {"zoom:1", "zoom:2"});
  core.renderDisplayTick();
  EXPECT_EQ(renderedSceneId(core), "scene-b");
  EXPECT_EQ(attribution(core), "live");
}

TEST(RenderedSceneAttribution, AnAttributionNothingHasConfirmedIsNeverKeptAsserted) {
  auto modules = corevideo::modules::createStubModules();
  auto compositor = std::make_unique<DeliveringCompositor>();
  auto* buffer = compositor.get();
  modules.compositor = std::move(compositor);
  MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  loadTilesScene(core, "scene-a", {"zoom:1", "zoom:2"});
  core.renderDisplayTick();
  ASSERT_EQ(renderedSceneId(core), "scene-a");

  // Delivery stops advancing — the buffer keeps handing back the same frame.
  // Program is still compositing (the scene changed and its layers are built),
  // so the snapshot must not go on asserting scene-a as what Program rendered.
  buffer->deliver = false;
  loadTilesScene(core, "scene-b", {"zoom:1", "zoom:2"});
  core.renderDisplayTick();
  // A beat of delivery jitter is allowed to hold, and says so.
  EXPECT_EQ(attribution(core), "holding");
  EXPECT_EQ(renderedSceneId(core), "scene-a");

  for (int tick = 0; tick < RenderedSceneAttributionPolicy::kHoldTicks + 2; ++tick) {
    core.renderDisplayTick();
  }
  EXPECT_EQ(attribution(core), "unknown");
  EXPECT_EQ(renderedSceneId(core), "")
      << "attribution kept asserting a scene Program stopped delivering";

  // ...and it recovers the moment a real delivery arrives again.
  buffer->deliver = true;
  core.renderDisplayTick();
  EXPECT_EQ(attribution(core), "live");
  EXPECT_EQ(renderedSceneId(core), "scene-b");
}

TEST(RenderedSceneAttribution, TheAttributionFieldsArePresentBeforeAnythingHappens) {
  auto modules = corevideo::modules::createStubModules();
  MediaCore core(std::move(modules));
  const auto snapshot = core.sessionState();
  const auto* node = programFrameNode(snapshot);
  ASSERT_NE(node, nullptr);
  ASSERT_NE(node->get("sceneIdAttribution"), nullptr);
  ASSERT_NE(node->get("sceneIdAttributionTicks"), nullptr);
  ASSERT_NE(node->get("deliverySequence"), nullptr);
  ASSERT_NE(snapshot.get("takeRecords"), nullptr);
  ASSERT_NE(snapshot.get("zoomSubscriptions"), nullptr);
  // No engine in this build path: the node still exists and says so.
  EXPECT_FALSE(snapshot.get("zoomSubscriptions")->get("engine")->asBool(true));
  EXPECT_EQ(snapshot.get("takeRecords")->getNumber("count"), 0);
}

TEST(RenderedSceneAttributionPolicyRules, ADeliveryThatDidNotAdvanceIsHeldThenForgotten) {
  using Action = RenderedSceneAttributionPolicy::Action;
  EXPECT_EQ(RenderedSceneAttributionPolicy::evaluate(true, true, 0).action, Action::Follow);
  // A delivered frame with no evidence attributes nothing — and must not inherit
  // the previous frame's scene, which is the same lie one frame later.
  EXPECT_EQ(RenderedSceneAttributionPolicy::evaluate(true, false, 0).action, Action::Forget);
  EXPECT_EQ(RenderedSceneAttributionPolicy::evaluate(false, true, 1).action, Action::Hold);
  EXPECT_EQ(
      RenderedSceneAttributionPolicy::evaluate(
          false, true, RenderedSceneAttributionPolicy::kHoldTicks).action,
      Action::Hold);
  EXPECT_EQ(
      RenderedSceneAttributionPolicy::evaluate(
          false, true, RenderedSceneAttributionPolicy::kHoldTicks + 1).action,
      Action::Forget);
  EXPECT_EQ(std::string(RenderedSceneAttributionPolicy::evaluate(true, true, 0).state), "live");
  EXPECT_EQ(std::string(RenderedSceneAttributionPolicy::evaluate(false, true, 1).state), "holding");
  EXPECT_EQ(std::string(RenderedSceneAttributionPolicy::evaluate(false, true, 999).state), "unknown");
}

// ---------------------------------------------------------------------------
// The take record.
// ---------------------------------------------------------------------------

TEST(TakeRecord, ATakeIsRecordedWithBothSidesOfTheWall) {
  auto modules = corevideo::modules::createStubModules();
  modules.compositor = std::make_unique<DeliveringCompositor>();
  MediaCore core(std::move(modules));
  core.enableAudioOutputWorker();

  loadTilesScene(core, "scene-a", {"zoom:1", "zoom:2"});
  core.renderDisplayTick();
  loadTilesScene(core, "scene-b", {"zoom:1", "zoom:2"});
  core.renderDisplayTick();

  const auto snapshot = core.sessionState();
  const auto* takes = snapshot.get("takeRecords");
  ASSERT_NE(takes, nullptr);
  ASSERT_NE(takes->get("records"), nullptr);
  const auto& records = takes->get("records")->asArray();
  ASSERT_EQ(records.size(), 2u);
  const auto& second = records[1];
  EXPECT_EQ(second.getString("fromSceneId"), "scene-a");
  EXPECT_EQ(second.getString("toSceneId"), "scene-b");
  EXPECT_EQ(second.getString("fromRenderPlanId"), "scene-a:0:0");
  EXPECT_EQ(second.getString("toRenderPlanId"), "scene-b:0:0");
  EXPECT_EQ(second.getString("fromWallKey"), "scene-a:tiles:scene-a");
  EXPECT_EQ(second.getString("toWallKey"), "scene-b:tiles:scene-b");
  ASSERT_NE(second.get("fromLayerIds"), nullptr);
  ASSERT_NE(second.get("toLayerIds"), nullptr);
  EXPECT_FALSE(second.get("toLayerIds")->asArray().empty());
  // This wall was never settled in Preview, so it could not be adopted — the
  // record says "reset"/"rebuilt" rather than certifying a clean cut.
  EXPECT_EQ(second.getString("wall"), "reset");
  EXPECT_EQ(second.getString("verdict"), "rebuilt");
  EXPECT_EQ(second.getNumber("subscriptionChurnDelta"), 0);
  // The ring is bounded; a record is never pending once its frame rendered.
  EXPECT_FALSE(takes->get("pending")->asBool(true));
}

TEST(TakeRecordPolicyRules, TheWallVerdictSeparatesACutFromARebuild) {
  TakeRecordPolicy::Observation observation;
  observation.hasWallAfter = false;
  EXPECT_EQ(std::string(TakeRecordPolicy::evaluate(observation).verdict), "no-wall");

  observation.hasWallAfter = true;
  observation.wallAdoptedSettled = false;
  EXPECT_EQ(std::string(TakeRecordPolicy::evaluate(observation).wall), "reset");
  EXPECT_EQ(std::string(TakeRecordPolicy::evaluate(observation).verdict), "rebuilt");

  observation.wallAdoptedSettled = true;
  EXPECT_EQ(std::string(TakeRecordPolicy::evaluate(observation).wall), "adopted-settled");
  EXPECT_EQ(std::string(TakeRecordPolicy::evaluate(observation).verdict), "cut");

  // Adopted, but the wall's live background never made the first program frame.
  observation.liveBackgroundExpected = true;
  observation.liveBackgroundEmitted = false;
  EXPECT_TRUE(TakeRecordPolicy::evaluate(observation).backgroundDropped);
  EXPECT_EQ(std::string(TakeRecordPolicy::evaluate(observation).verdict), "rebuilt");

  // Adopted with its background, but a subscription churned in the same tick.
  observation.liveBackgroundEmitted = true;
  observation.subscriptionChurnDelta = 1;
  EXPECT_TRUE(TakeRecordPolicy::evaluate(observation).subscriptionsChurned);
  EXPECT_EQ(std::string(TakeRecordPolicy::evaluate(observation).verdict), "rebuilt");

  observation.subscriptionChurnDelta = 0;
  EXPECT_EQ(std::string(TakeRecordPolicy::evaluate(observation).verdict), "cut");
}

// ---------------------------------------------------------------------------
// Subscription churn.
// ---------------------------------------------------------------------------

TEST(ZoomSubscriptionChurnPolicyRules, ResolutionCapEvictionAndDepartureAreDistinct) {
  using Change = ZoomSubscriptionChurnPolicy::Change;

  // First subscribe: the subscription starting, not churning.
  EXPECT_EQ(ZoomSubscriptionChurnPolicy::classifySubscribe({false, -1, 1, false}), Change::Initial);
  EXPECT_FALSE(ZoomSubscriptionChurnPolicy::countsAsChurn(Change::Initial));

  // Same source, same key: nothing is sent and nothing churned.
  EXPECT_EQ(ZoomSubscriptionChurnPolicy::classifySubscribe({true, 1, 1, true}), Change::None);
  EXPECT_FALSE(ZoomSubscriptionChurnPolicy::advancesGeneration(Change::None));

  // 720P -> 1080P because the source became active-speaker: a real re-subscribe.
  EXPECT_EQ(ZoomSubscriptionChurnPolicy::classifySubscribe({true, 1, 2, true}), Change::Resolution);
  EXPECT_TRUE(ZoomSubscriptionChurnPolicy::countsAsChurn(Change::Resolution));
  EXPECT_TRUE(ZoomSubscriptionChurnPolicy::advancesGeneration(Change::Resolution));

  // Subscribed again after having been dropped earlier.
  EXPECT_EQ(ZoomSubscriptionChurnPolicy::classifySubscribe({false, -1, 1, true}), Change::Resubscribe);

  // The distinction that decides whether a black wall is our bug or a departure.
  EXPECT_EQ(ZoomSubscriptionChurnPolicy::classifyRetire(true), Change::CapEviction);
  EXPECT_EQ(ZoomSubscriptionChurnPolicy::classifyRetire(false), Change::Departure);
  EXPECT_EQ(std::string(ZoomSubscriptionChurnPolicy::reason(Change::CapEviction)), "cap-eviction");
  EXPECT_EQ(std::string(ZoomSubscriptionChurnPolicy::reason(Change::Departure)), "departure");
  EXPECT_EQ(std::string(ZoomSubscriptionChurnPolicy::reason(Change::Resolution)), "resolution-change");
}
