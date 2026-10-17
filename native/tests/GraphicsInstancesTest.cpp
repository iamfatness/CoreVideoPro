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
  frame.graphicsFacts.push_back({"featured", revision, "ready", ""});
  state.observe(frame);
}
const Json& row(const GraphicsInstances& state) {
  return state.snapshot().get("instances")->asArray().front();
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
  ASSERT_TRUE(state.apply(request("graphics-instance-create", {
      {"templateId", "broadcast"}, {"templateVersion", 1}, {"scope", "scene"}, {"sceneId", "interview"},
      {"sourceId", "guest-a"}, {"fields", Json::Object{{"name", "A"}}}})));
  ready(state, 1);
  ASSERT_TRUE(state.apply(request("graphics-instance-trigger", {{"action", "show"}})));
  CompositorRenderPlan plan;
  plan.sceneId = "interview";
  state.append(plan, false);
  EXPECT_TRUE(plan.layers.empty());
  corevideo::modules::CompositorRenderPlanLayer source;
  source.sourceId = "guest-a"; source.kind = "participant-video";
  plan.layers.push_back(source);
  state.append(plan, false);
  ASSERT_EQ(2u, plan.layers.size());
  plan.sceneId = "elsewhere"; plan.layers.resize(1);
  state.append(plan, false);
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
