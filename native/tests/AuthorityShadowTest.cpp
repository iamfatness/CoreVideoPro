#include "core/AuthorityShadow.h"
#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <stdexcept>
using namespace corevideo::core;
namespace {
AuthorityShadow::Config config() {
  AuthorityShadow::Config c; c.enabled = true; c.workerThread = false;
  c.videoFreshnessWindowNs = 100;
  c.nativeProcessEpoch = "native"; c.legacyAuthorityEpoch = "legacy";
  c.authorityEpoch = "shadow"; c.registryEpoch = "registry";
  return c;
}
AuthorityShadow::Record checkpoint(uint64_t seq = 1) {
  AuthorityShadow::Record r;
  r.basis = {"native", "legacy", seq, seq, seq, 1, static_cast<int64_t>(seq)};
  r.completeCheckpoint = true;
  r.sources.processEpoch = "zoom"; r.sources.sequence = seq;
  r.desired.authorityEpoch = "legacy"; r.desired.expectedRevision = seq;
  r.desired.inputs.emplace(); r.desired.scenes.emplace(); r.desired.tiles.emplace();
  r.desired.overlays.emplace(); r.desired.isoSelections.emplace(); r.desired.audioRoutes.emplace(); r.desired.outputs.emplace();
  r.desired.preview.supplied = r.desired.program.supplied = true;
  return r;
}
}
TEST(AuthorityShadow, DisabledByDefaultDoesNotCreateEvidence) {
  AuthorityShadow shadow({});
  EXPECT_EQ(shadow.submit(checkpoint()), AuthorityShadow::Admission::Disabled);
  EXPECT_FALSE(shadow.processOne()); EXPECT_FALSE(shadow.latest());
  EXPECT_EQ(shadow.diagnostics().captured, 0ULL);
  EXPECT_EQ(shadow.diagnostics().status, AuthorityShadow::Status::Disabled);
}
TEST(AuthorityShadow, ProcessesCompleteCheckpointsInOrderWithoutInventingComparisons) {
  AuthorityShadow shadow(config());
  ASSERT_EQ(shadow.submit(checkpoint(1)), AuthorityShadow::Admission::Accepted);
  ASSERT_EQ(shadow.submit(checkpoint(2)), AuthorityShadow::Admission::Accepted);
  ASSERT_TRUE(shadow.processOne()); ASSERT_TRUE(shadow.latest());
  EXPECT_EQ(shadow.latest()->basis.captureSequence, 1ULL);
  ASSERT_TRUE(shadow.processOne());
  EXPECT_EQ(shadow.latest()->basis.captureSequence, 2ULL);
  EXPECT_EQ(shadow.latest()->plans->render.stamp.controlRevision, 0ULL);
  EXPECT_EQ(shadow.diagnostics().compared, 0ULL); EXPECT_EQ(shadow.diagnostics().matched, 0ULL);
  EXPECT_TRUE(shadow.diagnostics().continuous);
}
TEST(AuthorityShadow, SaturationInvalidatesQueuedEvidenceUntilNewCompleteCheckpoint) {
  auto c = config(); c.maxRecords = 1; AuthorityShadow shadow(c);
  ASSERT_EQ(shadow.submit(checkpoint(1)), AuthorityShadow::Admission::Accepted);
  EXPECT_EQ(shadow.submit(checkpoint(2)), AuthorityShadow::Admission::Full);
  ASSERT_TRUE(shadow.processOne()); EXPECT_FALSE(shadow.latest());
  auto partial = checkpoint(3); partial.desired.inputs.reset();
  ASSERT_EQ(shadow.submit(partial), AuthorityShadow::Admission::Accepted);
  shadow.processOne(); EXPECT_FALSE(shadow.latest());
  ASSERT_EQ(shadow.submit(checkpoint(4)), AuthorityShadow::Admission::Accepted);
  shadow.processOne(); ASSERT_TRUE(shadow.latest());
  EXPECT_EQ(shadow.latest()->basis.captureSequence, 4ULL);
  EXPECT_EQ(shadow.diagnostics().queueDrops, 1ULL);
  EXPECT_EQ(shadow.diagnostics().maximumQueueDepth, 1U);
}
TEST(AuthorityShadow, ByteBudgetCountsRetainedCapacityAndRejectsPrivateMetadata) {
  auto c = config(); c.maxBytes = 8192; AuthorityShadow shadow(c);
  ASSERT_EQ(shadow.submit(checkpoint()), AuthorityShadow::Admission::Accepted);
  shadow.processOne(); ASSERT_TRUE(shadow.latest());
  auto big = checkpoint(2); big.sources.sources.reserve(1000);
  EXPECT_EQ(shadow.submit(std::move(big)), AuthorityShadow::Admission::Oversized);
  EXPECT_FALSE(shadow.latest());
  auto named = checkpoint(3); named.sources.people.push_back({"opaque-person", "Private name", 1});
  EXPECT_EQ(shadow.submit(std::move(named)), AuthorityShadow::Admission::PrivateMetadata);
  EXPECT_EQ(shadow.diagnostics().oversized, 1ULL);
  EXPECT_EQ(shadow.diagnostics().privateMetadata, 1ULL);
}
TEST(AuthorityShadow, InvalidAdapterAndOutOfOrderBasisCannotPublishPlans) {
  AuthorityShadow shadow(config());
  auto invalid = checkpoint();
  ZoomSourceAuthorityAdapter::Source source; source.id = "source"; source.instanceId = "provider-instance"; source.externalId = "42";
  source.personId = "unmapped"; source.personGeneration = 1;
  invalid.sources.sources.push_back(source);
  ASSERT_EQ(shadow.submit(invalid), AuthorityShadow::Admission::Accepted);
  shadow.processOne(); EXPECT_FALSE(shadow.latest());
  EXPECT_EQ(shadow.diagnostics().adapterInvalid, 1ULL);
  ASSERT_EQ(shadow.submit(checkpoint(2)), AuthorityShadow::Admission::Accepted);
  shadow.processOne(); ASSERT_TRUE(shadow.latest());
  ASSERT_EQ(shadow.submit(checkpoint(1)), AuthorityShadow::Admission::Accepted);
  shadow.processOne(); EXPECT_FALSE(shadow.latest());
}
TEST(AuthorityShadow, LegacyAuthorityEpochIsPartOfTheCapturedBasis) {
  AuthorityShadow shadow(config());
  auto wrongBasis = checkpoint(1); wrongBasis.basis.legacyAuthorityEpoch = "retired";
  ASSERT_EQ(shadow.submit(wrongBasis),AuthorityShadow::Admission::Accepted);
  shadow.processOne(); EXPECT_FALSE(shadow.latest());
  auto mismatched = checkpoint(2); mismatched.desired.authorityEpoch = "other";
  ASSERT_EQ(shadow.submit(mismatched),AuthorityShadow::Admission::Accepted);
  shadow.processOne(); EXPECT_FALSE(shadow.latest());
  ASSERT_EQ(shadow.submit(checkpoint(3)),AuthorityShadow::Admission::Accepted);
  shadow.processOne(); ASSERT_TRUE(shadow.latest());
  EXPECT_EQ(shadow.latest()->basis.legacyAuthorityEpoch,"legacy");
  EXPECT_GE(shadow.diagnostics().basisGaps,2ULL);
}
TEST(AuthorityShadow, GeneratesIndependentBlankVideoAndAudioIntent) {
  AuthorityShadow shadow(config()); auto r = checkpoint();
  ShowOutputIntent output; r.desired.outputs->push_back({"output", output});
  ShowAudioRouteIntent audio; audio.destination = {"output", 1};
  audio.source = {ShowRouteKind::FixedSource, ShowSourceRef{"missing", "missing-instance", "zoom", 1}, std::nullopt};
  r.desired.audioRoutes->push_back({"audio", audio});
  ASSERT_EQ(shadow.submit(r), AuthorityShadow::Admission::Accepted); shadow.processOne();
  ASSERT_TRUE(shadow.latest());
  EXPECT_EQ(shadow.latest()->plans->render.program.status, PlannedBindingStatus::Blank);
  EXPECT_EQ(shadow.latest()->plans->audio.routes.size(), 1U);
  EXPECT_EQ(shadow.latest()->plans->audio.routes[0].binding.status, PlannedBindingStatus::Missing);
}
TEST(AuthorityShadow, WorkerShutdownJoinsAndRejectsFurtherIntake) {
  auto c = config(); c.workerThread = true; AuthorityShadow shadow(c);
  const auto admitted = shadow.submit(checkpoint());
  // The producer never waits for a worker mutex; one contention drop is allowed.
  EXPECT_TRUE(admitted == AuthorityShadow::Admission::Accepted || admitted == AuthorityShadow::Admission::Contended);
  shadow.stop(); shadow.stop();
  EXPECT_EQ(shadow.submit(checkpoint(2)), AuthorityShadow::Admission::Stopped);
  EXPECT_FALSE(shadow.latest()); EXPECT_EQ(shadow.diagnostics().queueDepth, 0U);
  EXPECT_EQ(shadow.diagnostics().status, AuthorityShadow::Status::Stopped);
}

TEST(AuthorityShadow, DesiredLabelsAndOverlayContentRemainBoundedIntent) {
  AuthorityShadow shadow(config()); auto r = checkpoint();
  ShowInputIntent input; input.label = "Operator label";
  r.desired.inputs->push_back({"input", input});
  LegacySceneDto scene; scene.id = "scene"; scene.label = "Scene title";
  r.desired.scenes->push_back(scene);
  ShowOverlayIntent overlay; overlay.content = "Lower third content"; overlay.requestedVisible = true;
  r.desired.overlays->push_back({"overlay", overlay});
  ASSERT_EQ(shadow.submit(std::move(r)), AuthorityShadow::Admission::Accepted);
  ASSERT_TRUE(shadow.processOne()); ASSERT_TRUE(shadow.latest());
  EXPECT_EQ(shadow.latest()->plans->render.overlays.at(0).intent.content, "Lower third content");
  EXPECT_EQ(shadow.diagnostics().privateMetadata, 0ULL);
  // Diagnostics is a numeric/status-only type; desired content is not serialized.
}
TEST(AuthorityShadow, UsesExplicitFreshnessCutoffAndClampsBeforeClockOrigin) {
  const auto addVideo = [](AuthorityShadow::Record& r) {
    ZoomSourceAuthorityAdapter::Source source;
    source.id = "camera"; source.instanceId = "camera-instance"; source.externalId = "42";
    source.incarnation = 1; source.publication = ZoomSourceAuthorityAdapter::Publication{1,100,1920,1080,60,1,"I420"};
    r.sources.sources.push_back(source);
    r.desired.inputs->push_back({"camera-input",{1,"",{ShowRouteKind::FixedSource,
      ShowSourceRef{"camera","camera-instance","zoom",1},std::nullopt}}});
  };
  AuthorityShadow shadow(config()); auto r = checkpoint(); r.basis.capturedAtNs = 50; addVideo(r);
  ASSERT_EQ(shadow.submit(std::move(r)), AuthorityShadow::Admission::Accepted);
  shadow.processOne(); ASSERT_TRUE(shadow.latest());
  EXPECT_EQ(shadow.latest()->plans->render.inputs.at(0).binding.status,PlannedBindingStatus::Resolved);
  const auto freshIdentity = shadow.latest()->plans->render.stamp.eligibilityIdentity;
  r = checkpoint(2); r.basis.capturedAtNs = 250; addVideo(r);
  ASSERT_EQ(shadow.submit(std::move(r)), AuthorityShadow::Admission::Accepted);
  shadow.processOne(); ASSERT_TRUE(shadow.latest());
  EXPECT_EQ(shadow.latest()->plans->render.inputs.at(0).binding.status,PlannedBindingStatus::Stale);
  EXPECT_NE(shadow.latest()->plans->render.stamp.eligibilityIdentity,freshIdentity);
}
TEST(AuthorityShadow, EnabledModeRequiresBoundedFreshnessWindow) {
  for (const auto window : {0LL, -1LL, 60'000'000'001LL}) {
    auto c = config(); c.videoFreshnessWindowNs = window;
    bool rejected = false;
    try { AuthorityShadow shadow(c); } catch (const std::invalid_argument&) { rejected = true; }
    EXPECT_TRUE(rejected);
  }
}

TEST(AuthorityShadow, CompleteCheckpointPreservesExactProviderSourceToken) {
  AuthorityShadow shadow(config()); auto r = checkpoint(); r.basis.capturedAtNs = 200;
  ZoomSourceAuthorityAdapter::Source source;
  source.id = "camera"; source.instanceId = "provider-camera-9";
  source.externalId = "42"; source.incarnation = 9;
  source.subscriptionRequested = source.subscriptionObserved = true;
  source.publication = ZoomSourceAuthorityAdapter::Publication{1, 150, 1920, 1080, 60, 1, "I420"};
  r.sources.sources.push_back(source);
  ShowLayerIntent layer;
  layer.target = {ShowRouteKind::FixedSource, ShowSourceRef{"camera", "provider-camera-9", "zoom", 9}, std::nullopt};
  LegacySceneDto scene; scene.id = "scene"; scene.layers.push_back({"layer", layer});
  r.desired.scenes->push_back(scene); r.desired.program.scene = ShowEntityRef{"scene", 1};
  ASSERT_EQ(shadow.submit(std::move(r)), AuthorityShadow::Admission::Accepted);
  ASSERT_TRUE(shadow.processOne()); ASSERT_TRUE(shadow.latest());
  const auto& binding = shadow.latest()->plans->render.program.layers.at(0).binding;
  ASSERT_EQ(binding.status, PlannedBindingStatus::Resolved); ASSERT_TRUE(binding.source.has_value());
  EXPECT_EQ(binding.source->instanceId, "provider-camera-9");
  EXPECT_EQ(binding.source->generation, 9ULL); EXPECT_EQ(binding.source->processEpoch, "zoom");
}
