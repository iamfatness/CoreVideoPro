#include "graphics/GraphicsInstances.h"
#include "graphics/GraphicsTemplate.h"
#include "gtest/gtest.h"
#include <chrono>
#include <thread>

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

TEST(GraphicsInstances, AuthoringPreviewUsesInstalledDefinitionWithoutCreatingOrCuingAnInstance) {
  GraphicsInstances state{"test-core"};
  const auto before = state.snapshot().stringify();
  const auto draft = Json::Object{{"templateId", "corporate-lower-third"}, {"templateVersion", 1},
      {"fields", Json::Object{{"name", "Private Unicode — 日本語"}, {"title", ""}}}};
  const auto content = state.authoringPreview(draft);
  ASSERT_TRUE(content.has_value()); ASSERT_NE(content->graphicsTemplate, nullptr);
  EXPECT_TRUE(content->hasGraphicsContentHash);
  EXPECT_EQ(before, state.snapshot().stringify());
  EXPECT_FALSE(state.hasCue());
  auto invalid = draft; invalid["templateVersion"] = 2; EXPECT_FALSE(state.authoringPreview(invalid));
  invalid = draft; invalid["style"] = Json::Object{{"width", .8}}; EXPECT_FALSE(state.authoringPreview(invalid));
}

TEST(GraphicsInstances, TypedExactEntryScientificNumbersReachTheNativeCommandWithoutWireLoss) {
  GraphicsInstances state{"test-core"};
  const auto command = Json::parse(R"({"type":"graphics-instance-create","graphics":{"schemaVersion":1e0,
    "requestId":"scientific-entry","clientId":"operator","coreEpoch":"test-core","instanceId":"small-offset",
    "templateId":"compact-solid","templateVersion":1E+0,"scope":"global","fields":{"name":"Guest"},"style":{"safeOffsetX":1E-5}}})");
  ASSERT_TRUE(command.has_value()); ASSERT_TRUE(state.apply(*command));
  const auto& instance = state.snapshot().get("instances")->asArray().front();
  EXPECT_NEAR(1e-5, instance.get("style")->getNumber("safeOffsetX"), 1e-12);
  for (const auto* bad : {"1e", "1e+", "1E-", "1e999"}) EXPECT_FALSE(Json::parse(bad));
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

TEST(GraphicsInstances, UnsupportedTargetOnProgramTriggerCannotBeIgnored) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(create(state)); ready(state, 1);
  EXPECT_FALSE(state.apply(request("graphics-instance-trigger", {{"action", "show"}, {"target", "preview"}})));
  CompositorRenderPlan plan;
  state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
  EXPECT_EQ("invalid-trigger-target", state.snapshot().get("lastResult")->getString("reason"));
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
  ProgramFrame eligible; eligible.graphicsFacts.push_back({"featured",1,"composition-eligible",""}); state.observe(eligible);
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
  ProgramFrame eligible; eligible.graphicsFacts.push_back({"featured",2,"composition-eligible",""}); state.observe(eligible);
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

TEST(GraphicsInstances, PrivateEditsDoNotReplaceAcceptedCueAndShowFromCueNamesItsExactRevision) {
  GraphicsInstances state{"test-core"}; ASSERT_TRUE(create(state)); ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-cue")));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {{"target", "cue"},
      {"fields", Json::Object{{"name", "Private edit"}, {"title", ""}}}})));
  ready(state, 2);
  CompositorRenderPlan preview; state.append(preview, true);
  ASSERT_EQ(1u, preview.layers.size()); EXPECT_EQ("Guest", preview.layers.front().overlay.title);
  EXPECT_EQ(1., row(state).getNumber("cueRevision")); EXPECT_EQ(2., row(state).getNumber("revision"));
  EXPECT_EQ("Private edit", row(state).get("fields")->getString("name"));
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"target", "cue"}, {"action", "show"}})));
  EXPECT_EQ(1., row(state).getNumber("programRevision"));
  EXPECT_EQ("Guest", row(state).get("programFields")->getString("name"));
  EXPECT_FALSE(state.apply(request("graphics-instance-trigger", {{"target", "cue"}, {"action", "show"}, {"expectedRevision", 2}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-cue", {{"expectedRevision", 2}})));
  EXPECT_EQ(2., row(state).getNumber("cueRevision"));
  EXPECT_EQ(1., row(state).getNumber("programRevision"));
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"target", "cue"}, {"action", "show"}, {"expectedRevision", 2}})));
  EXPECT_EQ("", row(state).get("programFields")->getString("title"));
}

TEST(GraphicsInstances, PendingCueRetainsItsExplicitRevisionAcrossNewPrivateEdits) {
  GraphicsInstances state{"test-core"}; ASSERT_TRUE(create(state));
  ASSERT_TRUE(state.apply(request("graphics-instance-cue")));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {{"target", "cue"},
      {"fields", Json::Object{{"name", "Later private edit"}}}})));
  CompositorRenderPlan program; state.append(program, false);
  ASSERT_EQ(2u, program.graphicsPreparations.size());
  EXPECT_NE(program.graphicsPreparations[0].layerId, program.graphicsPreparations[1].layerId);
  ready(state, 2);
  EXPECT_EQ(0., row(state).getNumber("cueRevision"));
  EXPECT_TRUE(row(state).get("cuePending")->asBool());
  EXPECT_FALSE(state.apply(request("graphics-instance-trigger", {{"target", "cue"}, {"action", "show"}})));
  ready(state, 1);
  EXPECT_EQ(1., row(state).getNumber("cueRevision"));
  EXPECT_EQ(2., row(state).getNumber("revision"));
  EXPECT_FALSE(row(state).get("cuePending")->asBool());
  EXPECT_EQ("Guest", row(state).get("cueFields")->getString("name"));
}

TEST(GraphicsInstances, FailedCueReplacementKeepsAcceptedCueAndLateReadinessCannotRecueIt) {
  GraphicsInstances state{"test-core"}; ASSERT_TRUE(create(state)); ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-cue")));
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {{"target", "cue"},
      {"fields", Json::Object{{"name", "Replacement"}}}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-cue", {{"expectedRevision", 2}})));
  ProgramFrame failed; failed.graphicsFacts.push_back({"featured", 2, "failed", "font-unavailable"});
  state.observe(failed);
  EXPECT_EQ(1., row(state).getNumber("cueRevision"));
  EXPECT_EQ(2., row(state).getNumber("cueRequestedRevision"));
  EXPECT_EQ("failed", row(state).getString("cuePreparation"));
  EXPECT_FALSE(row(state).get("cuePending")->asBool());
  ready(state, 2);
  EXPECT_EQ(1., row(state).getNumber("cueRevision")); // completion is not another Cue action
  CompositorRenderPlan preview; state.append(preview, true);
  ASSERT_EQ(1u, preview.layers.size()); EXPECT_EQ("Guest", preview.layers.front().overlay.title);
  ASSERT_TRUE(state.apply(request("graphics-instance-cue", {{"expectedRevision", 2}})));
  EXPECT_EQ(2., row(state).getNumber("cueRevision"));
}

TEST(GraphicsInstances, SubjectReplacementInvalidatesAcceptedCueWithoutAReplay) {
  GraphicsInstances state{"test-core"}; auto frames = sourceFrames();
  state.observeZoomSources({guest()}, 7, frames);
  ASSERT_TRUE(createBound(state)); ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-cue")));
  EXPECT_EQ(1., row(state).getNumber("cueRevision"));
  state.observeZoomSources({guest(2)}, 7, sourceFrames(2));
  EXPECT_EQ(0., row(state).getNumber("cueRevision"));
  EXPECT_EQ("source-identity-changed", row(state).getString("cueWarning"));
  ready(state, 1);
  EXPECT_EQ(0., row(state).getNumber("cueRevision"));
  EXPECT_FALSE(state.apply(request("graphics-instance-trigger", {{"target", "cue"}, {"action", "show"}})));
}

TEST(GraphicsInstances, RetiringAnOldSubjectCannotCancelAnExplicitPendingCueForAnotherValidSubject) {
  GraphicsInstances state{"test-core"};
  auto other = guest(); other.id = 55;
  auto otherFrame = sourceFrames().front(); otherFrame.participantId = "55";
  auto frames = sourceFrames(); frames.push_back(otherFrame);
  state.observeZoomSources({guest(), other}, 7, frames);
  ASSERT_TRUE(createBound(state)); ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-cue")));
  const auto nextBinding = *state.snapshot().get("sources")->asArray()[1].get("identity");
  ASSERT_TRUE(state.apply(request("graphics-instance-update", {{"target", "cue"}, {"binding", nextBinding},
      {"fields", Json::Object{{"name", "Other guest"}}}})));
  ASSERT_TRUE(state.apply(request("graphics-instance-cue", {{"expectedRevision", 2}})));
  frames = sourceFrames(2); frames.push_back(otherFrame);
  state.observeZoomSources({guest(2), other}, 7, frames);
  EXPECT_EQ(0., row(state).getNumber("cueRevision"));
  EXPECT_TRUE(row(state).get("cuePending")->asBool());
  EXPECT_EQ("preparing", row(state).getString("cuePreparation"));
  ready(state, 2);
  EXPECT_EQ(2., row(state).getNumber("cueRevision"));
  EXPECT_EQ("zoom:55", row(state).get("cueBinding")->getString("sourceId"));
}

TEST(GraphicsInstances, DeclarativeDefinitionIsPinnedAndCannotAcceptUnconsumedStyleProperties) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(state.apply(request("graphics-instance-create", {{"templateId", "corporate-lower-third"}, {"templateVersion", 1},
      {"scope", "global"}, {"fields", Json::Object{{"name", "Guest"}, {"title", ""}}}})));
  ASSERT_EQ(9u, state.snapshot().get("templates")->asArray().size());
  EXPECT_EQ(64u, state.snapshot().get("templates")->asArray()[3].getString("manifestHash").size());
  EXPECT_EQ("slide", row(state).get("animations")->get("in")->getString("type"));
  EXPECT_EQ(400., row(state).get("animations")->get("in")->getNumber("durationMs"));
  EXPECT_FALSE(state.apply(request("graphics-instance-update", {{"target", "cue"}, {"style", Json::Object{{"width", .7}}}})));
  EXPECT_EQ(1., row(state).getNumber("revision"));
  ASSERT_TRUE(state.apply(request("graphics-instance-cue")));
  CompositorRenderPlan plan; plan.width = 320; plan.height = 180; state.append(plan, false);
  ASSERT_EQ(1u, plan.graphicsPreparations.size());
  EXPECT_TRUE(plan.graphicsPreparations.front().overlay.graphicsTemplate != nullptr);
  EXPECT_TRUE(std::abs(plan.graphicsPreparations.front().rect.width - 1050. / 1920) < .0001);
  EXPECT_TRUE(std::abs(plan.graphicsPreparations.front().rect.height - 176. / 1080) < .0001);
}

TEST(GraphicsInstances, OperationResultPinsAcceptedRevisionAndScopePassesKeepCaptionsDownstream) {
  GraphicsInstances state{"test-core"}; ASSERT_TRUE(create(state));
  EXPECT_EQ(1,state.snapshot().get("lastResult")->getNumber("currentRevision"));
  EXPECT_EQ("featured",state.snapshot().get("lastResult")->getString("instanceId"));
  ready(state,1); ASSERT_TRUE(state.apply(request("graphics-instance-trigger",{{"action","show"}})));
  CompositorRenderPlan plan; corevideo::modules::CompositorRenderPlanLayer video,caption;
  video.order=100000; video.layerId="take-in:video";
  caption.hasOverlayContent=true; caption.overlay.isCaption=true; caption.layerId="caption";
  plan.layers={caption,video};
  state.append(plan,false,{},GraphicsInstances::ScopePass::Scene);
  EXPECT_EQ(2u,plan.layers.size()); EXPECT_TRUE(plan.graphicsPreparations.empty());
  state.append(plan,false,{},GraphicsInstances::ScopePass::Global);
  std::stable_sort(plan.layers.begin(),plan.layers.end(),[](const auto& a,const auto& b){return a.order<b.order;});
  ASSERT_EQ(3u,plan.layers.size()); EXPECT_EQ("take-in:video",plan.layers[0].layerId);
  EXPECT_EQ("graphics:featured",plan.layers[1].layerId); EXPECT_EQ("caption",plan.layers[2].layerId);
  EXPECT_EQ(100000,plan.layers[0].order); EXPECT_EQ(100001,plan.layers[1].order); EXPECT_EQ(100002,plan.layers[2].order);
}

TEST(GraphicsInstances, EntranceWaitsForEligibleCompositionAndReportsLatestDrawSeparately) {
  double now=0; GraphicsInstances state{"test-core",[&]{return now;}};
  ASSERT_TRUE(state.apply(request("graphics-instance-create",{{"templateId","centered-title"},{"templateVersion",1},{"scope","scene"},{"sceneId","interview"}})));
  ready(state,1); ASSERT_TRUE(state.apply(request("graphics-instance-trigger",{{"action","show"}})));
  now=60000; state.observe({}); EXPECT_EQ("awaiting-composition",row(state).getString("visibility"));
  CompositorRenderPlan plan; plan.sceneId="other"; state.append(plan,false); EXPECT_TRUE(plan.layers.empty());
  plan.sceneId="interview"; state.append(plan,false); ASSERT_EQ(1u,plan.layers.size());
  EXPECT_TRUE(plan.layers[0].graphicsAwaitingEntrance);
  EXPECT_NEAR(.125,plan.layers[0].rect.x,.00001); EXPECT_NEAR(396./1080,plan.layers[0].rect.y,.00001);
  ProgramFrame eligible; eligible.frameNumber=10; eligible.graphicsFacts.push_back({"featured",1,"composition-eligible",""}); state.observe(eligible);
  EXPECT_EQ("entering",row(state).getString("visibility")); EXPECT_FALSE(row(state).get("currentlyComposed")->asBool());
  now+=125; plan.layers.clear(); state.append(plan,false); EXPECT_FALSE(plan.layers[0].graphicsAwaitingEntrance);
  EXPECT_TRUE(plan.layers[0].opacity>0 && plan.layers[0].opacity<1);
  ProgramFrame drawn; drawn.frameNumber=11; drawn.graphicsFacts.push_back({"featured",1,"draw-submitted",""}); state.observe(drawn);
  EXPECT_TRUE(row(state).get("currentlyComposed")->asBool()); EXPECT_EQ(11,row(state).getNumber("firstDrawSubmittedFrame"));
  drawn.frameNumber=12; drawn.graphicsFacts.clear(); state.observe(drawn); EXPECT_FALSE(row(state).get("currentlyComposed")->asBool());
  EXPECT_EQ(11,row(state).getNumber("firstDrawSubmittedFrame"));
}
TEST(GraphicsInstances, PlacementIsAnExplicitAtomicTransformWithBoundsAndLegacyReset) {
  GraphicsInstances state{"test-core"}; ASSERT_TRUE(create(state)); ready(state,1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger",{{"action","show"}})));
  const Json placement=Json::Object{{"anchor","center"},{"x",120},{"y",-60},{"scale",.5}};
  ASSERT_TRUE(state.apply(request("graphics-instance-update",{{"target","program"},{"placement",placement}}))); ready(state,2);
  CompositorRenderPlan plan; state.append(plan,false); ASSERT_EQ(1u,plan.layers.size());
  EXPECT_NEAR((1-plan.layers[0].rect.width)*.5+120./1920,plan.layers[0].rect.x,.00001);
  auto bad=placement.asObject(); bad["scale"]=0;
  EXPECT_FALSE(state.apply(request("graphics-instance-update",{{"expectedRevision",2},{"target","program"},{"placement",bad}})));
  EXPECT_EQ(2,row(state).getNumber("revision"));
  ASSERT_TRUE(state.apply(request("graphics-instance-update",{{"expectedRevision",2},{"target","cue"},{"placement",Json{}}})));
  EXPECT_TRUE(row(state).get("placement")->isNull());
}

TEST(GraphicsClock, CountdownUsesMonotonicAnchorsAndRestoreStaysPaused) {
  corevideo::graphics::GraphicsClock clock; corevideo::graphics::GraphicsClockConfig config;
  config.mode="countdown"; config.durationSeconds=60; config.restoreRemainingSeconds=12.5;
  clock.control("configure",1000,&config);
  EXPECT_EQ("00:00:13",clock.sample(999999,0).getString("text")); EXPECT_EQ("paused",clock.sample(999999,0).getString("state"));
  clock.control("resume",1000); EXPECT_EQ("00:00:12",clock.sample(1500,999999999).getString("text"));
  clock.control("pause",2500); EXPECT_EQ("00:00:11",clock.sample(999999,-999999999).getString("text"));
  clock.control("resume",3000); EXPECT_EQ("finished",clock.sample(14000,0).getString("state"));
  EXPECT_EQ("hold-zero",clock.sample(14000,0).getString("finishAction"));
  clock.control("start",15000); EXPECT_EQ("finished",clock.sample(15000,0).getString("state"));
  clock.control("reset",16000); EXPECT_EQ("00:01:00",clock.sample(16000,0).getString("text")); EXPECT_EQ("paused",clock.sample(16000,0).getString("state"));
}
TEST(GraphicsClock, WallClockTimezoneAndFormatAreExplicitAndConfigurationIsStrict) {
  corevideo::graphics::GraphicsClock clock; corevideo::graphics::GraphicsClockConfig config;
  config.zone="utc"; clock.control("configure",0,&config); EXPECT_EQ("00:00:00",clock.sample(123,0).getString("text"));
  config.zone="fixed"; config.offsetMinutes=330; config.format="hm"; clock.control("configure",0,&config);
  EXPECT_EQ("05:30",clock.sample(99999,0).getString("text"));
  auto bad=config.json().asObject(); bad["offsetMinutes"]=841;
  EXPECT_FALSE(corevideo::graphics::GraphicsClockConfig::read(bad,config));
  bad=config.json().asObject(); bad.erase("mode"); bad["restoreRemainingSeconds"]=2;
  EXPECT_FALSE(corevideo::graphics::GraphicsClockConfig::read(bad,config));
}
TEST(GraphicsInstances, ClockControlIsQualifiedAndDoesNotRevealOrDriftLogicalRevision) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(state.apply(request("graphics-instance-create",{{"templateId","clock-panel"},{"templateVersion",1},{"scope","global"}})));
  corevideo::graphics::GraphicsClockConfig config; config.mode="countdown"; config.durationSeconds=3;
  ASSERT_TRUE(state.apply(request("graphics-clock-control",{{"action","configure"},{"configuration",config.json()},{"expectedClockRevision",1}})));
  const auto wait=[&](const std::string& expected) {
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do { state.observe({}); const auto* clock=row(state).get("clock"); if(clock && clock->getString("state")==expected) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while(std::chrono::steady_clock::now()<until); return false;
  };
  ASSERT_TRUE(wait("paused")); EXPECT_EQ("00:00:03",row(state).get("fields")->getString("time"));
  EXPECT_EQ(2,row(state).getNumber("clockConfigurationRevision"));
  EXPECT_FALSE(state.apply(request("graphics-clock-control",{{"action","configure"},{"configuration",config.json()},{"expectedClockRevision",1}})));
  EXPECT_EQ("clock-configuration-conflict",state.snapshot().get("lastResult")->getString("reason"));
  ASSERT_TRUE(state.apply(request("graphics-clock-control",{{"action","start"}}))); ASSERT_TRUE(wait("running"));
  EXPECT_EQ(1,row(state).getNumber("revision")); EXPECT_EQ(0,row(state).getNumber("programRevision"));
  ASSERT_TRUE(state.apply(request("graphics-clock-control",{{"action","configure"},{"configuration",config.json()},{"expectedClockRevision",2}})));
  EXPECT_EQ("unchanged",state.snapshot().get("lastResult")->getString("reason"));
  EXPECT_EQ(2,row(state).getNumber("clockConfigurationRevision")); ASSERT_TRUE(wait("running"));
  ASSERT_TRUE(state.apply(request("graphics-clock-control",{{"action","pause"}}))); ASSERT_TRUE(wait("paused"));
  EXPECT_FALSE(state.apply(request("graphics-instance-update",{{"target","cue"},{"fields",Json::Object{{"time","FAKE"}}}})));
  EXPECT_EQ("clock-time-controlled",state.snapshot().get("lastResult")->getString("reason"));
}

TEST(GraphicsInstances, EmptyAndOtherScopePassesPreserveAuthoredOrderExactly) {
  GraphicsInstances state{"test-core"}; CompositorRenderPlan plan;
  corevideo::modules::CompositorRenderPlanLayer first,second;
  first.layerId="first"; first.order=91; second.layerId="second"; second.order=-5;
  plan.layers={first,second}; state.append(plan,false);
  EXPECT_EQ(91,plan.layers[0].order); EXPECT_EQ(-5,plan.layers[1].order);
  ASSERT_TRUE(create(state)); ready(state,1); ASSERT_TRUE(state.apply(request("graphics-instance-trigger",{{"action","show"}})));
  state.append(plan,false,{},GraphicsInstances::ScopePass::Scene);
  ASSERT_EQ(2u,plan.layers.size()); EXPECT_EQ(91,plan.layers[0].order); EXPECT_EQ(-5,plan.layers[1].order);
}

TEST(GraphicsInstances, AuthoringGeometryUsesNativeFlowAndPublishesNoAcceptedInstance) {
  GraphicsInstances state{"test-core"};
  auto input=Json::Object{{"templateId","corporate-lower-third"},{"templateVersion",1},{"fields",Json::Object{{"name","Ada"},{"title",""}}}};
  ASSERT_TRUE(state.authoringPreview(input));
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  while((!state.snapshot().get("authoring") || state.snapshot().get("authoring")->isNull()) && std::chrono::steady_clock::now()<until) {state.observe({});std::this_thread::sleep_for(std::chrono::milliseconds(1));}
  const auto* authoring=state.snapshot().get("authoring");ASSERT_TRUE(authoring);EXPECT_EQ("validated",authoring->getString("state"));
  const auto* layout=authoring->get("layout");ASSERT_TRUE(layout);EXPECT_EQ(1050,layout->getNumber("width"));
  const auto& nodes=layout->get("nodes")->asArray();ASSERT_EQ(4u,nodes.size());
  const auto name=std::find_if(nodes.begin(),nodes.end(),[](const auto& n){return n.getString("id")=="name-text";});
  const auto title=std::find_if(nodes.begin(),nodes.end(),[](const auto& n){return n.getString("id")=="title-text";});
  ASSERT_NE(nodes.end(),name);ASSERT_NE(nodes.end(),title);EXPECT_EQ("text-stack",name->getString("parent"));EXPECT_EQ(24,name->getNumber("x"));EXPECT_EQ(20,name->getNumber("y"));
  EXPECT_EQ(90,title->getNumber("y"));EXPECT_FALSE(title->get("visible")->asBool());
  EXPECT_TRUE(state.snapshot().get("instances")->asArray().empty());EXPECT_FALSE(state.hasCue());
}

TEST(GraphicsInstances, NewTemplateVersionAdoptsOnlyOnExplicitPreparedUpdateAndReusesVerifiedBuiltinAssets) {
  GraphicsInstances state{"test-core"};
  ASSERT_TRUE(state.apply(request("graphics-instance-create",{{"templateId","logo-accent"},{"templateVersion",1},{"scope","global"},{"animations",Json::Object{{"in",Json::Object{{"type","none"}}}}}})));
  ready(state,1);ASSERT_TRUE(state.apply(request("graphics-instance-trigger",{{"action","show"}})));
  auto publication=std::find_if(corevideo::graphics::GraphicsTemplate::curated().begin(),corevideo::graphics::GraphicsTemplate::curated().end(),[](const auto& d){return d->id()=="logo-accent";});
  ASSERT_NE(corevideo::graphics::GraphicsTemplate::curated().end(),publication);
  auto document=(*publication)->document().asObject();document["templateVersion"]=2;
  ASSERT_TRUE(state.apply(request("graphics-template-install",{{"manifest",Json(document).stringify()}})));
  const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(3);bool installed=false;
  do {state.observe({});for(const auto& item:state.snapshot().get("templates")->asArray()) if(item.getString("templateId")=="logo-accent" && item.getNumber("templateVersion")==2) installed=true;
    if(!installed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while(!installed && std::chrono::steady_clock::now()<until);
  ASSERT_TRUE(installed);EXPECT_EQ(1,row(state).getNumber("programTemplateVersion"));EXPECT_EQ(1,row(state).getNumber("templateVersion"));
  ASSERT_TRUE(state.apply(request("graphics-instance-update",{{"target","program"},{"templateVersion",2}})));
  EXPECT_EQ(1,row(state).getNumber("programTemplateVersion"));EXPECT_EQ(2,row(state).getNumber("templateVersion"));
  ready(state,2);EXPECT_EQ(2,row(state).getNumber("programTemplateVersion"));
}

TEST(GraphicsInstances, PrivatePreviewAndProgramShareTheDefaultsOfEveryExistingLook) {
  for(const auto* preset:{"compact-solid","minimal-accent","broadcast"}) {
    GraphicsInstances state{"test-core"};const Json fields=Json::Object{{"name","Ada"},{"title",""}};
    ASSERT_TRUE(state.apply(request("graphics-instance-create",{{"templateId",preset},{"templateVersion",1},{"scope","global"},{"fields",fields}})));
    ready(state,1);ASSERT_TRUE(state.apply(request("graphics-instance-trigger",{{"action","show"}})));
    CompositorRenderPlan plan;state.append(plan,false);ASSERT_EQ(1u,plan.layers.size());
    const auto preview=state.authoringPreview(Json::Object{{"templateId",preset},{"templateVersion",1},{"fields",fields}});ASSERT_TRUE(preview);
    EXPECT_EQ(plan.layers[0].overlay.graphicsContentHash,preview->graphicsContentHash);ASSERT_TRUE(preview->graphicsAuthoringRect);
    EXPECT_EQ(plan.layers[0].rect.width,preview->graphicsAuthoringRect->width);EXPECT_EQ(plan.layers[0].rect.height,preview->graphicsAuthoringRect->height);
  }
}
