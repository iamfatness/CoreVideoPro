#include "graphics/GraphicsInstances.h"
#include "gtest/gtest.h"

using corevideo::graphics::GraphicsInstances;
using corevideo::rpc::Json;
using corevideo::modules::CompositorRenderPlan;
using corevideo::modules::ProgramFrame;

namespace {
Json request(const std::string& type, Json::Object fields = {}) {
  fields["schemaVersion"] = 1;
  static int sequence = 0;
  fields["requestId"] = "test-request-" + std::to_string(++sequence);
  fields["clientId"] = "test-client";
  fields["coreEpoch"] = "test-core";
  fields["instanceId"] = "featured";
  if (!fields.count("expectedRevision")) fields["expectedRevision"] = 1;
  return Json::Object{{"type", type}, {"graphics", std::move(fields)}};
}
bool create(GraphicsInstances& state) {
  return state.apply(request("graphics-instance-create", {
      {"templateId", "compact-solid"}, {"templateVersion", 1}, {"scope", "global"},
      {"fields", Json::Object{{"name", "Guest"}, {"title", "Engineer"}}}}));
}
void ready(GraphicsInstances& state, uint64_t revision) {
  ProgramFrame frame;
  auto resource = std::make_shared<corevideo::graphics::GraphicsPreparedResource>();
  resource->native = std::make_shared<int>(1);
  frame.graphicsFacts.push_back({"featured", revision, "ready", "", resource});
  state.observe(frame);
}
const Json& row(const GraphicsInstances& state) {
  return state.snapshot().get("instances")->asArray().front();
}
corevideo::modules::ZoomEngineParticipant guest(uint64_t generation = 1) {
  corevideo::modules::ZoomEngineParticipant participant;
  participant.id = 44; participant.displayName = "Guest"; participant.hasVideo = true;
  participant.sourceGeneration = generation;
  return participant;
}
std::vector<corevideo::modules::VideoFrame> sourceFrames(uint64_t generation = 1, uint64_t pixelEpoch = 1) {
  corevideo::modules::VideoFrame frame;
  frame.participantId = "44"; frame.sourceGeneration = generation; frame.sourceEpoch = pixelEpoch;
  frame.width = frame.pixelWidth = 1; frame.height = frame.pixelHeight = 1; frame.pixelStride = 4;
  frame.pixels = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{20, 40, 60, 255});
  return {frame};
}
Json binding(const GraphicsInstances& state) {
  return *state.snapshot().get("sources")->asArray().front().get("identity");
}
bool createBound(GraphicsInstances& state) {
  return state.apply(request("graphics-instance-create", {
      {"templateId", "compact-solid"}, {"templateVersion", 1}, {"scope", "global"},
      {"binding", binding(state)}, {"fields", Json::Object{{"name", "Guest"}, {"title", ""}}}}));
}
CompositorRenderPlan sourcePlan() {
  CompositorRenderPlan plan;
  corevideo::modules::CompositorRenderPlanLayer source;
  source.sourceId = "zoom:44"; source.participantId = "44"; source.kind = "participant-video";
  plan.layers.push_back(source);
  return plan;
}
}

TEST(GraphicsInstances, CueDoesNotAffectProgramAndShowWaitsForCompletePreparation) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state));
  ASSERT_TRUE(state.apply(request("graphics-instance-cue")));
  CompositorRenderPlan plan;
  state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
  ASSERT_EQ(1u, plan.graphicsPreparations.size());
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  plan = {}; state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
  ready(state, 1);
  plan = {}; state.append(plan, false);
  ASSERT_EQ(1u, plan.layers.size());
  EXPECT_EQ("Guest", plan.layers[0].overlay.title);
  EXPECT_EQ("Engineer", plan.layers[0].overlay.org);
}

TEST(GraphicsInstances, DraftAndAtomicProgramUpdateKeepIndependentRevisions) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state));
  ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {
      {"target", "cue"}, {"fields", Json::Object{{"name", "New guest"}, {"title", ""}}}})));
  ready(state, 2);
  CompositorRenderPlan plan;
  state.append(plan, false);
  ASSERT_EQ(1u, plan.layers.size());
  EXPECT_EQ("Guest", plan.layers[0].overlay.title);
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {
      {"expectedRevision", 2}, {"target", "program"}, {"fields", Json::Object{{"name", "Final guest"}}}})));
  plan = {}; state.append(plan, false);
  EXPECT_EQ("Guest", plan.layers[0].overlay.title);
  ready(state, 3);
  plan = {}; state.append(plan, false);
  EXPECT_EQ("Final guest", plan.layers[0].overlay.title);
  EXPECT_EQ("", plan.layers[0].overlay.org);
}

TEST(GraphicsInstances, HideAndClearCancelPendingShowsAndStaleReadinessCannotReveal) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state));
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "hide"}})));
  ready(state, 1);
  CompositorRenderPlan plan;
  state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {
      {"target", "cue"}, {"fields", Json::Object{{"name", "Other"}}}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}, {"expectedRevision", 2}})));
  ready(state, 1);
  plan = {}; state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
  ASSERT_TRUE(state.apply(request("graphics-program-clear", {{"channel", "all"}})));
  ready(state, 2);
  plan = {}; state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
}

TEST(GraphicsInstances, SameFieldsAreNoOpAndInvalidPatchIsAtomic) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {
      {"target", "cue"}, {"fields", Json::Object{{"name", "Guest"}}}})));
  EXPECT_EQ(1., row(state).getNumber("revision"));
  EXPECT_FALSE(state.apply(request("graphics-instance-update", {
      {"target", "cue"}, {"fields", Json::Object{{"name", "Must not leak"}, {"title", 17}}}})));
  EXPECT_EQ("Guest", row(state).get("fields")->getString("name"));
  EXPECT_EQ(1., row(state).getNumber("revision"));
  EXPECT_FALSE(state.apply(request("graphics-instance-update", {
      {"target", "cue"}, {"expectedRevision", 8}})));
}

TEST(GraphicsInstances, SceneAndSourceScopesPreventPreviousGuestLabelOnNewScene) {
  GraphicsInstances state{"test-core"};
  const auto frames = sourceFrames();
  state.observeZoomSources({guest()}, 7, frames);
  ASSERT_TRUE(state.apply(request("graphics-instance-create", {
      {"templateId", "broadcast"}, {"templateVersion", 1}, {"scope", "scene"}, {"sceneId", "interview"},
      {"binding", binding(state)}, {"fields", Json::Object{{"name", "A"}}}})));
  ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  CompositorRenderPlan plan;
  plan.sceneId = "interview";
  state.append(plan, false, frames);
  EXPECT_TRUE(plan.layers.empty());
  corevideo::modules::CompositorRenderPlanLayer source;
  source.sourceId = "zoom:44"; source.participantId = "44"; source.kind = "participant-video";
  plan.layers.push_back(source);
  state.append(plan, false, frames);
  ASSERT_EQ(2u, plan.layers.size());
  plan.sceneId = "elsewhere"; plan.layers.resize(1);
  state.append(plan, false, frames);
  EXPECT_EQ(1u, plan.layers.size());
}

TEST(GraphicsInstances, PreparationFailureKeepsPreviousValidProgramRevision) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state)); ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {
      {"target", "program"}, {"fields", Json::Object{{"name", "Replacement"}}}})));
  ProgramFrame frame;
  frame.graphicsFacts.push_back({"featured", 2, "failed", "font-unavailable"});
  state.observe(frame);
  CompositorRenderPlan plan;
  state.append(plan, false);
  ASSERT_EQ(1u, plan.layers.size());
  EXPECT_EQ("Guest", plan.layers[0].overlay.title);
  EXPECT_EQ("failed", row(state).getString("preparation"));
}

TEST(GraphicsInstances, RetriedRequestsDoNotRepeatAndReusedIdsAreRefused) {
  GraphicsInstances state{"test-core"};
  const auto createRequest = request("graphics-instance-create", {
      {"templateId", "compact-solid"}, {"templateVersion", 1}, {"scope", "global"}});
  ASSERT_TRUE(state.apply(createRequest));
  ASSERT_TRUE(state.apply(createRequest));
  EXPECT_EQ(1u, state.snapshot().get("instances")->asArray().size());
  auto changed = createRequest.get("graphics")->asObject();
  changed["scope"] = "scene";
  EXPECT_FALSE(state.apply(Json::Object{{"type", "graphics-instance-create"}, {"graphics", changed}}));
  EXPECT_EQ("request-id-reused", state.snapshot().get("lastResult")->getString("reason"));
  changed["coreEpoch"] = "expired-core";
  EXPECT_FALSE(state.apply(Json::Object{{"type", "graphics-instance-create"}, {"graphics", changed}}));
  EXPECT_EQ("core-epoch-conflict", state.snapshot().get("lastResult")->getString("reason"));
}

TEST(GraphicsInstances, RemovedIdentityCannotAdoptItsPreviousGenerationPreparation) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state));
  ASSERT_TRUE(state.apply(request("graphics-instance-remove")));
  ASSERT_TRUE(create(state));
  EXPECT_EQ(2., row(state).getNumber("revision"));
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}, {"expectedRevision", 2}})));
  ready(state, 1);
  CompositorRenderPlan plan;
  state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
  ready(state, 2);
  plan = {}; state.append(plan, false);
  ASSERT_EQ(1u, plan.layers.size());
}

TEST(GraphicsInstances, StylePatchPreservesExistingValuesAndRejectsWrongTypes) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {
      {"target", "cue"}, {"style", Json::Object{{"nameSize", 65}}}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {
      {"expectedRevision", 2}, {"target", "cue"}, {"style", Json::Object{{"titleSize", 36}}}})));
  ready(state, 3);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}, {"expectedRevision", 3}})));
  CompositorRenderPlan plan;
  state.append(plan, false);
  ASSERT_EQ(1u, plan.layers.size());
  EXPECT_EQ(65., plan.layers[0].overlay.appearance.nameSize);
  EXPECT_EQ(36., plan.layers[0].overlay.appearance.titleSize);
  EXPECT_FALSE(state.apply(request("graphics-instance-update", {
      {"expectedRevision", 3}, {"target", "cue"}, {"style", Json::Object{{"nameSize", "huge"}}}})));
  EXPECT_EQ(3., row(state).getNumber("revision"));
}

TEST(GraphicsInstances, EditingCueCannotRetargetAnEarlierPendingProgramShow) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state));
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {
      {"target", "cue"}, {"fields", Json::Object{{"name", "Private draft"}}}})));
  ready(state, 1);
  ready(state, 2);
  CompositorRenderPlan plan;
  state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
  EXPECT_EQ("hidden", row(state).getString("visibility"));
}

TEST(GraphicsInstances, UnsupportedPreviewTargetOnProgramTriggerCannotBeIgnored) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state)); ready(state, 1);
  EXPECT_FALSE(state.apply(request("graphics-instance-trigger", {{"action", "show"}, {"target", "cue"}})));
  CompositorRenderPlan plan;
  state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
  EXPECT_EQ("unsupported-request-field", state.snapshot().get("lastResult")->getString("reason"));
}

TEST(GraphicsInstances, MissingRequiredPrimaryTextCannotReplaceAValidProgramRevision) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state)); ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  EXPECT_FALSE(state.apply(request("graphics-instance-update", {
      {"target", "program"}, {"fields", Json::Object{{"name", ""}}}})));
  EXPECT_EQ(1., row(state).getNumber("revision"));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {
      {"target", "cue"}, {"fields", Json::Object{{"name", ""}}}})));
  EXPECT_EQ("failed", row(state).getString("preparation"));
  CompositorRenderPlan plan;
  state.append(plan, false);
  EXPECT_TRUE(plan.graphicsPreparations.empty());
  ASSERT_EQ(1u, plan.layers.size());
  EXPECT_EQ("Guest", plan.layers[0].overlay.title);
}

TEST(GraphicsInstances, NativeAnimationUsesMonotonicTimeAndReversesWithoutAnOpacityJump) {
  double now = 100;
  GraphicsInstances state{"test-core", [&] { return now; }};
  const Json transitions = Json::Object{
      {"in", Json::Object{{"type", "slide"}, {"direction", "left"}, {"durationMs", 1000}, {"easing", "linear"}}},
      {"out", Json::Object{{"type", "fade"}, {"durationMs", 1000}}}};
  ASSERT_TRUE(state.apply(request("graphics-instance-create", {
      {"templateId", "compact-solid"}, {"templateVersion", 1}, {"scope", "global"},
      {"fields", Json::Object{{"name", "Guest"}}}, {"animations", transitions}})));
  ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  now = 600;
  CompositorRenderPlan plan; state.append(plan, false);
  ASSERT_EQ(1u, plan.layers.size());
  EXPECT_EQ(.5f, plan.layers[0].opacity);
  const auto midwayX = plan.layers[0].rect.x;
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "hide"}})));
  plan = {}; state.append(plan, false);
  EXPECT_EQ(.5f, plan.layers[0].opacity); EXPECT_EQ(midwayX, plan.layers[0].rect.x);
  EXPECT_EQ("exiting", row(state).getString("visibility"));
  now = 850;
  plan = {}; state.append(plan, false);
  const auto exitX = plan.layers[0].rect.x;
  EXPECT_EQ(.375f, plan.layers[0].opacity);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  plan = {}; state.append(plan, false);
  EXPECT_EQ(.375f, plan.layers[0].opacity); EXPECT_EQ(exitX, plan.layers[0].rect.x);
  now = 1850; state.observe({});
  plan = {}; state.append(plan, false);
  EXPECT_EQ(1.f, plan.layers[0].opacity);
  EXPECT_EQ(1., row(state).getNumber("revision")); // reversing is not a content edit
}

TEST(GraphicsInstances, RepeatedShowAndContentUpdateDoNotRestartNativeEntrance) {
  double now = 0;
  GraphicsInstances state{"test-core", [&] { return now; }};
  ASSERT_TRUE(create(state));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {{"target", "cue"},
      {"animations", Json::Object{{"in", Json::Object{{"type", "fade"}, {"durationMs", 1000}}}}}})));
  ready(state, 2);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"expectedRevision", 2}, {"action", "show"}})));
  now = 400;
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"expectedRevision", 2}, {"action", "show"}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {{"expectedRevision", 2}, {"target", "program"},
      {"fields", Json::Object{{"title", "Updated"}}}})));
  ready(state, 3);
  now = 500;
  CompositorRenderPlan plan; state.append(plan, false);
  EXPECT_EQ(.5f, plan.layers[0].opacity);
  EXPECT_EQ("Updated", plan.layers[0].overlay.org);
  now = 1000; state.observe({});
  plan = {}; state.append(plan, false);
  EXPECT_EQ(1.f, plan.layers[0].opacity);
}

TEST(GraphicsInstances, HideRetiresAtNativeTransitionEndWhileClearCutsImmediately) {
  double now = 0;
  GraphicsInstances state{"test-core", [&] { return now; }};
  ASSERT_TRUE(create(state));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {{"target", "cue"},
      {"animations", Json::Object{{"out", Json::Object{{"type", "fade"}, {"durationMs", 1000}}}}}})));
  ready(state, 2);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"expectedRevision", 2}, {"action", "show"}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"expectedRevision", 2}, {"action", "hide"}})));
  now = 999; state.observe({});
  EXPECT_EQ("exiting", row(state).getString("visibility"));
  EXPECT_EQ(2., row(state).getNumber("programRevision"));
  now = 1000; state.observe({});
  EXPECT_EQ("hidden", row(state).getString("visibility"));
  EXPECT_EQ(0., row(state).getNumber("programRevision"));
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"expectedRevision", 2}, {"action", "show"}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"expectedRevision", 2}, {"action", "clear"}})));
  CompositorRenderPlan plan; state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
  now = 2000; ready(state, 2);
  plan = {}; state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
}

TEST(GraphicsInstances, InvalidTransitionCannotMutateTheAcceptedRevision) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state));
  for (const auto& transition : std::vector<Json>{
      Json::Object{{"type", "script"}}, Json::Object{{"type", "fade"}, {"durationMs", 3001}},
      Json::Object{{"type", "fade"}, {"easing", "bounce"}}, Json::Object{{"type", "slide"}, {"direction", "diagonal"}},
      Json::Object{{"type", "fade"}, {"durationMs", -1}}, Json::Object{{"type", "fade"}, {"customCurve", true}}}) {
    EXPECT_FALSE(state.apply(request("graphics-instance-update", {{"target", "cue"},
        {"animations", Json::Object{{"in", transition}}}, {"fields", Json::Object{{"name", "Must not leak"}}}})));
    EXPECT_EQ(1., row(state).getNumber("revision"));
    EXPECT_EQ("Guest", row(state).get("fields")->getString("name"));
  }
}

TEST(GraphicsInstances, ResolutionUpgradePreservesSubjectIdentityWithoutSerializingPixelEpochs) {
  GraphicsInstances state{"test-core"};
  auto frames = sourceFrames();
  state.observeZoomSources({guest()}, 7, frames);
  const auto before = binding(state).stringify();
  ASSERT_TRUE(createBound(state)); ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  frames = sourceFrames(1, UINT64_MAX);
  state.observeZoomSources({guest()}, 7, frames);
  EXPECT_EQ(before, binding(state).stringify());
  EXPECT_EQ(1., row(state).getNumber("revision"));
  auto plan = sourcePlan(); state.append(plan, false, frames);
  EXPECT_EQ(2u, plan.layers.size());
}

TEST(GraphicsInstances, RecycledSdkIdRetiresOldLabelBeforeNewVideoAndRequiresExplicitRebinding) {
  GraphicsInstances state{"test-core"};
  auto frames = sourceFrames(); state.observeZoomSources({guest()}, 7, frames);
  const auto oldBinding = binding(state);
  ASSERT_TRUE(createBound(state)); ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  frames = sourceFrames(2); state.observeZoomSources({guest(2)}, 7, frames);
  EXPECT_EQ("hidden", row(state).getString("visibility"));
  EXPECT_EQ("source-identity-changed", row(state).getString("bindingState"));
  ready(state, 1);
  auto plan = sourcePlan(); state.append(plan, false, frames);
  EXPECT_EQ(1u, plan.layers.size());
  EXPECT_FALSE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  EXPECT_FALSE(state.apply(request("graphics-instance-update", {{"target", "cue"}, {"binding", oldBinding}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {{"target", "cue"}, {"binding", binding(state)},
      {"fields", Json::Object{{"name", "Replacement"}}}})));
  ready(state, 2);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}, {"expectedRevision", 2}})));
  plan = sourcePlan(); state.append(plan, false, frames);
  ASSERT_EQ(2u, plan.layers.size());
  EXPECT_EQ("Replacement", plan.layers.back().overlay.title);
  EXPECT_EQ("", plan.layers.back().overlay.org);
}

TEST(GraphicsInstances, MetadataRefreshCannotChangeSourceOwnedOverridesOrTriggerNewRevision) {
  GraphicsInstances state{"test-core"};
  const auto frames = sourceFrames(); state.observeZoomSources({guest()}, 7, frames);
  ASSERT_TRUE(createBound(state)); ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  auto participant = guest(); participant.displayName = "guest";
  state.observeZoomSources({participant}, 7, frames);
  EXPECT_EQ("Guest", row(state).get("fields")->getString("name"));
  EXPECT_EQ("", row(state).get("fields")->getString("title"));
  EXPECT_EQ(1., row(state).getNumber("revision"));
  EXPECT_EQ("bound", row(state).getString("bindingState"));
}

TEST(GraphicsInstances, MetadataOrAmbiguousFramesCannotStandInForTheComposedSubject) {
  GraphicsInstances state{"test-core"};
  auto frames = sourceFrames(); state.observeZoomSources({guest()}, 7, frames);
  ASSERT_TRUE(createBound(state)); ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  auto metadata = frames; metadata[0].pixels.reset();
  auto plan = sourcePlan(); state.append(plan, false, metadata);
  EXPECT_EQ(1u, plan.layers.size());
  frames.push_back(sourceFrames(2)[0]);
  plan = sourcePlan(); state.append(plan, false, frames);
  EXPECT_EQ(1u, plan.layers.size());
  plan = sourcePlan(); plan.layers.front().participantId = "99";
  state.append(plan, false, sourceFrames());
  EXPECT_EQ(1u, plan.layers.size()); // the route label alone is not its displayed participant
}

TEST(GraphicsInstances, SessionReplacementCancelsPendingRevealAndDoesNotReuseTheOldIdentity) {
  GraphicsInstances state{"test-core"};
  const auto frames = sourceFrames(); state.observeZoomSources({guest()}, 7, frames);
  const auto oldBinding = binding(state).stringify();
  ASSERT_TRUE(createBound(state));
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  state.observeZoomSources({guest()}, 8, frames);
  EXPECT_NE(oldBinding, binding(state).stringify());
  ready(state, 1);
  auto plan = sourcePlan(); state.append(plan, false, frames);
  EXPECT_EQ(1u, plan.layers.size());
  EXPECT_FALSE(row(state).get("showPending")->asBool());
}

TEST(GraphicsInstances, NakedOrUnknownSourceBindingIsRejectedWithoutMutation) {
  GraphicsInstances state{"test-core"};
  const auto frames = sourceFrames(); state.observeZoomSources({guest()}, 7, frames);
  EXPECT_FALSE(state.apply(request("graphics-instance-create", {{"scope", "global"}, {"templateId", "compact-solid"},
      {"templateVersion", 1}, {"sourceId", "zoom:44"}})));
  auto unknown = binding(state).asObject(); unknown["processEpoch"] = "retired-helper";
  EXPECT_FALSE(state.apply(request("graphics-instance-create", {{"scope", "global"}, {"templateId", "compact-solid"},
      {"templateVersion", 1}, {"binding", unknown}})));
  EXPECT_TRUE(state.snapshot().get("instances")->asArray().empty());
  ASSERT_TRUE(createBound(state));
  unknown = binding(state).asObject(); unknown["unknownProperty"] = true;
  EXPECT_FALSE(state.apply(request("graphics-instance-update", {{"target", "cue"}, {"binding", unknown}})));
  EXPECT_EQ(1., row(state).getNumber("revision"));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {{"target", "cue"}, {"binding", Json{}}})));
  EXPECT_EQ("unbound", row(state).getString("bindingState"));
}

TEST(GraphicsSourceCatalog, StableRosterHasNoNewPublicationAndLegacyOrUnconfiguredSourcesStayUnqualified) {
  corevideo::graphics::GraphicsSourceCatalog catalog{"test-core"};
  const auto frames = sourceFrames();
  EXPECT_TRUE(catalog.observeZoom({guest()}, 7, frames));
  EXPECT_FALSE(catalog.observeZoom({guest()}, 7, frames));
  EXPECT_TRUE(catalog.observeZoom({guest(0)}, 7, frames));
  EXPECT_TRUE(catalog.rows().asArray().empty());
  EXPECT_FALSE(catalog.observeZoom({guest(0)}, 7, frames));
  EXPECT_EQ("unqualified-source-identity", catalog.status());
  EXPECT_TRUE(catalog.observeZoom({guest()}, 7, {}));
  EXPECT_TRUE(catalog.rows().asArray().empty());
}

TEST(GraphicsInstances, ReadyWithoutAResourceCannotRevealAndAcceptedProgramKeepsItsResourceAcrossDraftEdits) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state));
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  ProgramFrame missing; missing.graphicsFacts.push_back({"featured", 1, "ready", ""});
  state.observe(missing);
  EXPECT_EQ("failed", row(state).getString("preparation"));
  CompositorRenderPlan plan; state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
  ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  plan = {}; state.append(plan, false);
  const auto resource = plan.layers.front().graphicsPreparedResource;
  ASSERT_NE(resource, nullptr);
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {{"target", "cue"}, {"fields", Json::Object{{"name", "Private edit"}}}})));
  plan = {}; state.append(plan, false);
  EXPECT_EQ(resource, plan.layers.front().graphicsPreparedResource);
  EXPECT_EQ("Guest", plan.layers.front().overlay.title);
  EXPECT_TRUE(row(state).get("preparedResource")->isNull());
  EXPECT_TRUE(row(state).get("programResource")->isObject());
}

TEST(GraphicsInstances, AResourceForDifferentContentCannotSatisfyReadinessOrReplaceAcceptedProgram) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state)); ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  CompositorRenderPlan original; state.append(original, false);
  const auto accepted = original.layers.front().graphicsPreparedResource;
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {{"target", "program"},
      {"fields", Json::Object{{"name", "New content"}}}})));
  auto mismatched = std::make_shared<corevideo::graphics::GraphicsPreparedResource>();
  mismatched->backend = corevideo::graphics::GraphicsPreparedResource::Backend::D3D11;
  mismatched->deviceIdentity = 1; mismatched->width = 192; mismatched->height = 64;
  mismatched->native = std::make_shared<int>(1); mismatched->hasContentHash = true;
  ProgramFrame frame;
  frame.graphicsFacts.push_back({"featured", 2, "ready", "", mismatched}); state.observe(frame);
  EXPECT_EQ("failed", row(state).getString("preparation"));
  EXPECT_EQ(1., row(state).getNumber("programRevision"));
  // Even a late readiness fact naming Program's revision cannot replace its
  // resource if it carries unrelated content.
  frame.graphicsFacts.clear(); frame.graphicsFacts.push_back({"featured", 1, "ready", "", mismatched});
  state.observe(frame);
  CompositorRenderPlan current; state.append(current, false);
  ASSERT_EQ(1u, current.layers.size());
  EXPECT_TRUE(accepted == current.layers.front().graphicsPreparedResource);
}
