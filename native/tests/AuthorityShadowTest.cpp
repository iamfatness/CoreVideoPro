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
namespace {
AuthorityShadow::Record versionCheckpoint(uint64_t seq, uint64_t previewVersion) {
  auto r = checkpoint(seq);
  ShowSceneIntent program; ShowLayerIntent layer; layer.x = 10;
  program.routes.emplace("layer", layer); program.layerOrder = {"layer"};
  auto preview = program; preview.routes.at("layer").x = static_cast<int>(previewVersion * 10);
  r.desired.scenes->push_back({"scene", 1, "", {{"layer", layer}}});
  r.desired.program.scene = r.desired.preview.scene = ShowEntityRef{"scene", 1};
  r.sceneVersions.emplace();
  r.sceneVersions->program = SceneVersionRef{"shadow", "scene", 1, 1};
  r.sceneVersions->preview = SceneVersionRef{"shadow", "scene", 1, previewVersion};
  r.sceneVersions->definitions.push_back({*r.sceneVersions->program, program});
  if (previewVersion != 1) r.sceneVersions->definitions.push_back({*r.sceneVersions->preview, preview});
  return r;
}
}
TEST(AuthorityShadow, VersionedEvidenceRetainsProgramAcrossSameIdPreviewEdits) {
  auto c = config(); c.versionedScenes = true; AuthorityShadow shadow(c);
  shadow.submit(versionCheckpoint(1, 2)); shadow.processOne(); auto first = shadow.latest(); ASSERT_TRUE(first);
  ASSERT_TRUE(first->sceneVersions);
  shadow.submit(versionCheckpoint(2, 3)); shadow.processOne(); auto next = shadow.latest(); ASSERT_TRUE(next);
  EXPECT_EQ(first->plans->render.preview.layers.at(0).intent.x, 20);
  EXPECT_EQ(next->plans->render.preview.layers.at(0).intent.x, 30);
  EXPECT_EQ(next->plans->render.program.layers.at(0).intent.x, 10);
  EXPECT_EQ(first->plans->render.program.versionLease.get(), next->plans->render.program.versionLease.get());
  EXPECT_EQ(first->plans->render.stamp.controlRevision, next->plans->render.stamp.controlRevision);
  EXPECT_FALSE(first->plans->render.stamp == next->plans->render.stamp);
  EXPECT_EQ(shadow.diagnostics().compared, 0ULL);
}
TEST(AuthorityShadow, VersionedModeRejectsMissingPayloadAndCannotDowngrade) {
  auto c = config(); c.versionedScenes = true; AuthorityShadow shadow(c);
  shadow.submit(versionCheckpoint(1, 2)); shadow.processOne(); ASSERT_TRUE(shadow.latest());
  auto missing = versionCheckpoint(2, 2); missing.sceneVersions->definitions.pop_back();
  shadow.submit(missing); shadow.processOne(); EXPECT_FALSE(shadow.latest());
  auto downgrade = versionCheckpoint(3, 2); downgrade.sceneVersions.reset();
  EXPECT_EQ(shadow.submit(downgrade), AuthorityShadow::Admission::ModeMismatch);
  EXPECT_FALSE(shadow.processOne()); EXPECT_FALSE(shadow.latest());
  shadow.submit(versionCheckpoint(4, 2)); shadow.processOne(); ASSERT_TRUE(shadow.latest());
  auto stale = versionCheckpoint(5, 2); stale.sceneVersions->program->authorityEpoch = "old";
  shadow.submit(stale); shadow.processOne(); EXPECT_FALSE(shadow.latest());
}
TEST(AuthorityShadow, VersionedStoreCapacityFailurePreservesPreviouslyLeasedEvidence) {
  auto c = config(); c.versionedScenes = true; c.sceneVersionLimits.versions = 2; AuthorityShadow shadow(c);
  shadow.submit(versionCheckpoint(1, 2)); shadow.processOne(); auto held = shadow.latest(); ASSERT_TRUE(held);
  shadow.submit(versionCheckpoint(2, 3)); shadow.processOne(); EXPECT_FALSE(shadow.latest());
  EXPECT_EQ(shadow.diagnostics().status, AuthorityShadow::Status::AdapterInvalid);
  EXPECT_EQ(held->plans->render.preview.layers.at(0).intent.x, 20);
  EXPECT_EQ(held->plans->render.program.layers.at(0).intent.x, 10);
}
TEST(AuthorityShadow, FreshOversizedVersionBatchDoesNotPoisonLaterCheckpoint) {
  auto c = config(); c.versionedScenes = true; c.sceneVersionLimits.versions = 1; AuthorityShadow shadow(c);
  shadow.submit(versionCheckpoint(1, 2)); shadow.processOne(); EXPECT_FALSE(shadow.latest());
  auto retry = versionCheckpoint(2, 1);
  // Changing the formerly attempted first payload proves it was not retained.
  retry.sceneVersions->definitions[0].scene.routes.at("layer").x = 77;
  shadow.submit(std::move(retry)); shadow.processOne(); auto accepted = shadow.latest(); ASSERT_TRUE(accepted);
  EXPECT_EQ(accepted->plans->render.program.layers.at(0).intent.x, 77);
}
TEST(AuthorityShadow, VersionPayloadRetainedCapacityCountsAgainstQueueBudget) {
  auto c = config(); c.versionedScenes = true; c.maxBytes = 8192; AuthorityShadow shadow(c);
  auto record = versionCheckpoint(1, 2);
  record.sceneVersions->definitions.front().scene.layerOrder.front().reserve(10000);
  EXPECT_EQ(shadow.submit(std::move(record)), AuthorityShadow::Admission::Oversized);
  EXPECT_FALSE(shadow.latest());
}
namespace {
void addTransactionalSource(AuthorityShadow::Record& record, uint64_t generation = 1) {
  ZoomSourceAuthorityAdapter::Source source;
  source.id = "camera"; source.externalId = "42"; source.instanceId = "instance-" + std::to_string(generation);
  source.incarnation = generation; record.sources.sources.push_back(source);
  record.desired.inputs->push_back({"input", {1, "", {ShowRouteKind::FixedSource,
      ShowSourceRef{source.id, source.instanceId, record.sources.processEpoch, generation}, {}}}});
}
}
TEST(AuthorityShadow, RejectedSceneCheckpointDoesNotConsumeSourceOrShowGenerations) {
  auto c = config(); c.versionedScenes = true; AuthorityShadow shadow(c);
  auto first = versionCheckpoint(1, 1); addTransactionalSource(first);
  shadow.submit(first); shadow.processOne(); auto before = shadow.latest(); ASSERT_TRUE(before);
  auto failed = versionCheckpoint(2, 2); addTransactionalSource(failed, 2);
  failed.desired.scenes->at(0).generation = 2;
  failed.desired.program.scene->generation = failed.desired.preview.scene->generation = 2;
  // Scene payload generations remain1, so this fails after source and show staging.
  shadow.submit(failed); shadow.processOne(); EXPECT_FALSE(shadow.latest());
  EXPECT_EQ(shadow.diagnostics().lastProcessedSequence, 1ULL);
  auto retry = versionCheckpoint(3, 1); addTransactionalSource(retry);
  shadow.submit(retry); shadow.processOne(); auto recovered = shadow.latest(); ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->plans->render.stamp.controlRevision, before->plans->render.stamp.controlRevision);
  EXPECT_EQ(recovered->plans->render.stamp.registryRevision, before->plans->render.stamp.registryRevision);
  EXPECT_EQ(recovered->plans->render.program.versionLease.get(), before->plans->render.program.versionLease.get());
}
TEST(AuthorityShadow, RejectedShowCheckpointDoesNotRetirePriorSourceEpoch) {
  AuthorityShadow shadow(config());
  auto first = checkpoint(1); addTransactionalSource(first);
  first.desired.inputs->at(0).intent.generation = 2;
  shadow.submit(first); shadow.processOne(); auto before = shadow.latest(); ASSERT_TRUE(before);
  auto failed = checkpoint(2); failed.sources.processEpoch = "new-zoom"; addTransactionalSource(failed, 2);
  // Regress input generation2->1: projector accepts structure, owner rejects CAS semantics.
  shadow.submit(failed); shadow.processOne(); EXPECT_FALSE(shadow.latest());
  auto retry = checkpoint(3); addTransactionalSource(retry); retry.desired.inputs->at(0).intent.generation = 2;
  shadow.submit(retry); shadow.processOne(); auto recovered = shadow.latest(); ASSERT_TRUE(recovered);
  EXPECT_EQ(recovered->plans->render.stamp, before->plans->render.stamp);
}
TEST(AuthorityShadow, DisabledSceneModeRejectsSidecarBeforeBudgetAccounting) {
  AuthorityShadow shadow(config()); auto record = versionCheckpoint(1, 1);
  record.sceneVersions->definitions.reserve(100000);
  EXPECT_EQ(shadow.submit(std::move(record)), AuthorityShadow::Admission::ModeMismatch);
  EXPECT_EQ(shadow.diagnostics().oversized, 0ULL); EXPECT_EQ(shadow.diagnostics().queueDepth, 0U);
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
  source.subscriptionRequested = true;
  source.subscriptionObserved = true;
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

TEST(AuthorityShadow, ExactFrameComparisonIsOptInAndRequiresCoherentCapturedEvidence) {
  auto c = config(); c.exactFrameComparison = true;
  AuthorityShadow shadow(c);
  ShadowExactSourceFrames frames;
  ExactRouteSourceRef id{"camera", "instance", "zoom", "camera", 1};
  frames.reconcile("zoom",1,{{id,1,true}});
  corevideo::modules::VideoFrame frame;
  frame.participantId = "42"; frame.width = frame.i420Width = 4; frame.height = frame.i420Height = 4;
  frame.i420 = std::make_shared<const std::vector<uint8_t>>(24,128);
  corevideo::modules::SourceFrameEvidence proof;
  proof.identity = {"camera","instance","zoom",1}; proof.publicationFence = 1;
  proof.publicationSequence = 1; proof.observedNs = 1; proof.payload = frame.i420;
  frame.exactSourceEvidence = std::make_shared<const corevideo::modules::SourceFrameEvidence>(proof);
  ASSERT_EQ(frames.publish(frame),ShadowExactSourceFrames::Status::Applied);
  auto make = [&](uint64_t sequence) {
    auto r = checkpoint(sequence);
    ZoomSourceAuthorityAdapter::Source source;
    source.id = "camera"; source.instanceId = "instance"; source.externalId = "42";
    source.publication = ZoomSourceAuthorityAdapter::Publication{1,1,4,4,{},{},"I420"};
    r.sources.sources.push_back(source);
    ShowInputIntent input; input.target = {ShowRouteKind::FixedSource,ShowSourceRef{"camera","instance","zoom",1},{}};
    r.desired.inputs->push_back({"input",input});
    r.exactFrames = frames.checkpoint();
    return r;
  };
  auto r = make(1);
  auto disabledConfig = config(); AuthorityShadow disabled(disabledConfig);
  EXPECT_EQ(disabled.submit(r),AuthorityShadow::Admission::ModeMismatch);
  EXPECT_EQ(disabled.diagnostics().queueDepth,0U);
  EXPECT_FALSE(disabled.latest());
  auto noSidecar = r; noSidecar.exactFrames.reset();
  EXPECT_EQ(shadow.submit(noSidecar),AuthorityShadow::Admission::ModeMismatch);
  EXPECT_EQ(shadow.diagnostics().queueDepth,0U);
  disabled.submit(noSidecar); disabled.processOne();
  ASSERT_TRUE(disabled.latest());
  EXPECT_EQ(disabled.latest()->exactFrames.state,AuthorityShadow::ExactFrameComparison::State::Disabled);
  shadow.submit(r); shadow.processOne(); ASSERT_TRUE(shadow.latest());
  EXPECT_EQ(shadow.latest()->exactFrames.eligible,1ULL);
  EXPECT_EQ(shadow.diagnostics().matched,0ULL); // No legacy/output comparison implied.
  auto old = r.exactFrames;
  frames.reconcile("zoom",2,{{id,2,false}});
  frames.reconcile("zoom",3,{{id,3,true}});
  r = make(3); shadow.submit(r); shadow.processOne();
  EXPECT_EQ(shadow.latest()->exactFrames.missing,1ULL);
  r = make(4); r.exactFrames = old; shadow.submit(r); shadow.processOne();
  EXPECT_EQ(shadow.latest()->exactFrames.state,AuthorityShadow::ExactFrameComparison::State::Unavailable);
  EXPECT_EQ(shadow.latest()->exactFrames.eligible,0ULL);
  // A valid captured frame can age out without any roster/format mutation.
  frames.reconcile("zoom",5,{{id,3,true}});
  proof.publicationFence = 3; proof.publicationSequence = 2;
  frame.exactSourceEvidence = std::make_shared<const corevideo::modules::SourceFrameEvidence>(proof);
  ASSERT_EQ(frames.publish(frame),ShadowExactSourceFrames::Status::Applied);
  r = make(5); r.basis.capturedAtNs = 500;
  r.sources.sources[0].publication->sequence = 2;
  shadow.submit(r); shadow.processOne();
  EXPECT_EQ(shadow.latest()->exactFrames.state,AuthorityShadow::ExactFrameComparison::State::Complete);
  EXPECT_EQ(shadow.latest()->exactFrames.expired,1ULL);
  EXPECT_EQ(shadow.latest()->exactFrames.eligible,0ULL);
  frames.reconcile("replacement",1,{});
  r = checkpoint(6); r.basis.capturedAtNs = 501; r.sources.processEpoch = "replacement"; r.sources.sequence = r.basis.sourceSequence = 1;
  ShowInputIntent stale; stale.target = {ShowRouteKind::FixedSource,ShowSourceRef{"camera","instance","zoom",1},{}};
  r.desired.inputs->push_back({"input",stale}); r.exactFrames = frames.checkpoint();
  shadow.submit(r); shadow.processOne(); ASSERT_TRUE(shadow.latest());
  EXPECT_EQ(shadow.latest()->exactFrames.state,AuthorityShadow::ExactFrameComparison::State::Complete);
  EXPECT_EQ(shadow.latest()->exactFrames.evaluated,1ULL);
  EXPECT_EQ(shadow.latest()->exactFrames.missing,1ULL);
}
TEST(AuthorityShadow, ExactFrameMetadataCountsAgainstQueueBudget) {
  auto c = config(); c.maxBytes = 8192; c.exactFrameComparison = true;
  AuthorityShadow shadow(c); ShadowExactSourceFrames frames;
  std::vector<ShadowExactSourceFrames::CurrentSource> sources;
  for (int i = 0; i < 100; ++i) sources.push_back({{"source"+std::to_string(i),"instance","zoom","camera",1},1,true});
  frames.reconcile("zoom",1,std::move(sources));
  auto r = checkpoint(); r.exactFrames = frames.checkpoint();
  EXPECT_EQ(shadow.submit(r),AuthorityShadow::Admission::Oversized);
  EXPECT_FALSE(shadow.latest());
}
