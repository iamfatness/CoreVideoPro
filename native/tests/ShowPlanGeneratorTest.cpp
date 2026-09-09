#include "core/ShowPlanGenerator.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <type_traits>

using namespace corevideo::core;
namespace {
SourceRegistry::Snapshot roster() {
  SourceRegistry::Snapshot r;
  r.registryEpoch = "registry"; r.revision = 12;
  r.persons = {{{"person-a"}, "Same name", 2}, {{"person-b"}, "Same name", 1}};
  for (const auto& id : {"a", "b", "c"}) {
    SourceRegistry::Source s;
    s.token = {{id}, {std::string("instance-") + id}, "process-1", 3};
    s.personId = PersonId{id == std::string("a") ? "person-a" : "person-b"};
    s.personGeneration = id == std::string("a") ? 2 : 1;
    s.hasPublication = true;
    r.sources.push_back(s);
  }
  return r;
}
ShowRouteTarget fixed(const char* id, std::uint64_t generation = 3) {
  return {ShowRouteKind::FixedSource, ShowSourceRef{id, std::string("instance-") + id, "process-1", generation}, std::nullopt};
}
ShowRouteTarget person(const char* id, std::uint64_t generation = 1) {
  return {ShowRouteKind::FollowPerson, std::nullopt, ShowEntityRef{id, generation}};
}
ShowStateSnapshot show() {
  ShowStateSnapshot s; s.authorityEpoch = "authority"; s.revision = 7;
  s.data.inputs["second"] = {1, "Second", fixed("b")};
  s.data.inputs["first"] = {1, "First", fixed("a")};
  s.data.inputOrder = {"second", "first"};
  ShowSceneIntent scene;
  scene.routes["back"].target = fixed("a");
  scene.routes["front"].target = {};
  scene.layerOrder = {"front", "back"};
  s.data.scenes["scene"] = scene;
  s.data.program = ShowEntityRef{"scene", 1};
  s.data.outputs["output"] = {};
  s.data.audioRoutes["audio"] = {1, fixed("a"), {"output", 1}, -6000, false};
  s.data.isoSelections["iso-fixed"] = {1, fixed("a")};
  s.data.isoSelections["iso-follow"] = {1, person("person-a", 2)};
  s.data.isoOrder = {"iso-follow", "iso-fixed"};
  return s;
}
}
TEST(ShowPlanGenerator, DeterministicProjectionPreservesIntentOrderAndStamps) {
  const auto s = show(); auto r = roster();
  const auto first = generateShowPlans(s, r);
  std::reverse(r.sources.begin(), r.sources.end());
  std::reverse(r.persons.begin(), r.persons.end());
  EXPECT_TRUE(*first == *generateShowPlans(s, r));
  static_assert(std::is_const_v<std::remove_reference_t<decltype(*first)>>);
  EXPECT_EQ(first->render.stamp.controlRevision, 7ULL);
  EXPECT_EQ(first->audio.stamp.registryRevision, 12ULL);
  EXPECT_EQ(first->output.stamp.authorityEpoch, "authority");
  EXPECT_EQ(first->render.inputs.at(0).input.id, "second");
  EXPECT_EQ(first->render.program.layers.at(0).route.id, "front");
  EXPECT_EQ(first->render.program.layers.at(1).route.id, "back");
  EXPECT_EQ(first->output.isoSelections.at(0).selection.id, "iso-follow");
  EXPECT_EQ(first->render.preview.status, PlannedBindingStatus::Blank);
}

TEST(ShowPlanGenerator, RegistryRestartCannotRebindExactPinAtReusedGeneration) {
  auto s = show(); auto r = roster();
  s.data.overlays["name"] = {1, fixed("a").source, "Title", true};
  const auto original = generateShowPlans(s, r);
  EXPECT_EQ(original->render.overlays[0].binding.status, PlannedBindingStatus::Resolved);
  for (bool changeProcess : {false, true}) {
    auto restarted = r;
    restarted.registryEpoch = "replacement-registry";
    // Keep stable ID and numeric generation identical: either changed concrete
    // identity field alone must fence the old pin across every media projection.
    if (changeProcess) restarted.sources[0].token.processEpoch = "process-2";
    else restarted.sources[0].token.instanceId.value = "instance-replaced";
    const auto plans = generateShowPlans(s, restarted);
    EXPECT_EQ(plans->render.program.layers[1].binding.status, PlannedBindingStatus::Missing);
    EXPECT_EQ(plans->render.overlays[0].binding.status, PlannedBindingStatus::Missing);
    EXPECT_EQ(plans->audio.routes[0].binding.status, PlannedBindingStatus::Missing);
    EXPECT_EQ(plans->output.isoSelections[1].videoBinding.status, PlannedBindingStatus::Missing);
    EXPECT_EQ(plans->output.isoSelections[1].audioBinding.status, PlannedBindingStatus::Missing);
    // Explicit durable person-following keeps its distinct reconnect semantics.
    EXPECT_EQ(plans->output.isoSelections[0].videoBinding.status, PlannedBindingStatus::Resolved);
  }
}
TEST(ShowPlanGenerator, ReconnectFollowsPersonButDoesNotSubstituteFixedIncarnation) {
  auto s = show(); auto r = roster();
  const auto old = generateShowPlans(s, r);
  r.sources[0].token = {{"a"}, {"replacement"}, "process-2", 4}; ++r.revision;
  const auto next = generateShowPlans(s, r);
  EXPECT_EQ(next->output.isoSelections.at(1).videoBinding.status, PlannedBindingStatus::Missing);
  ASSERT_TRUE(next->output.isoSelections.at(0).videoBinding.source.has_value());
  EXPECT_EQ(next->output.isoSelections.at(0).videoBinding.source->generation, 4ULL);
  EXPECT_EQ(next->output.isoSelections.at(0).videoBinding.source->processEpoch, "process-2");
  EXPECT_EQ(old->output.isoSelections.at(0).videoBinding.source->instanceId, "instance-a");
  r.sources[0].availability = SourceRegistry::Availability::Departed;
  EXPECT_EQ(generateShowPlans(s, r)->output.isoSelections.at(0).videoBinding.status, PlannedBindingStatus::Missing);
}
TEST(ShowPlanGenerator, DuplicateNamesNeverSelectAndDuplicatePersonSourcesAreAmbiguous) {
  auto s = show(); auto r = roster();
  s.data.inputs.at("first").target = person("person-b");
  EXPECT_EQ(generateShowPlans(s, r)->render.inputs.at(1).binding.status, PlannedBindingStatus::Ambiguous);
  s.data.inputs.at("first").target = person("person-a", 2);
  EXPECT_EQ(generateShowPlans(s, r)->render.inputs.at(1).binding.source->sourceId, "a");
  s.data.inputs.at("first").target = person("person-a", 1);
  EXPECT_EQ(generateShowPlans(s, r)->render.inputs.at(1).binding.status, PlannedBindingStatus::Missing);
  s.data.inputs.at("first").target = person("Same name");
  EXPECT_EQ(generateShowPlans(s, r)->render.inputs.at(1).binding.status, PlannedBindingStatus::Missing);
}
TEST(ShowPlanGenerator, TilesRequireRosterOptInAndKeepReservedBlankAndExcludedSources) {
  auto s = show(); auto r = roster();
  ShowTilesIntent tiles;
  tiles.reservedSlots[0] = std::nullopt;
  tiles.reservedSlots[2] = ShowEntityRef{"first", 1};
  tiles.excludedInputs.insert({"second", 1});
  s.data.tiles["tiles"] = tiles;
  const auto initial = generateShowPlans(s, r);
  ASSERT_EQ(initial->render.tiles.at(0).slots.size(), 2U);
  EXPECT_EQ(initial->render.tiles.at(0).slots.at(0).binding.status, PlannedBindingStatus::Blank);
  EXPECT_TRUE(initial->render.tiles.at(0).slots.at(0).reserved);
  s.data.tiles.at("tiles").allowRosterAdditions = true;
  const auto expanded = generateShowPlans(s, r);
  ASSERT_EQ(expanded->render.tiles.at(0).slots.size(), 3U);
  EXPECT_EQ(expanded->render.tiles.at(0).slots.at(1).slot, 1U);
  EXPECT_EQ(expanded->render.tiles.at(0).slots.at(1).binding.source->sourceId, "c");
  EXPECT_EQ(expanded->render.tiles.at(0).slots.at(2).slot, 2U);
  s.data.tiles.at("tiles").autoFill = false;
  EXPECT_EQ(generateShowPlans(s, r)->render.tiles.at(0).slots.size(), 2U);
}
TEST(ShowPlanGenerator, BlankVideoDoesNotMuteIndependentAudioAndSelectorsRemainUnresolved) {
  auto s = show(); const auto r = roster();
  s.data.scenes.at("scene").routes.at("back").target = {};
  s.data.inputs.at("first").target = {ShowRouteKind::ActiveSpeaker, std::nullopt, std::nullopt};
  const auto plans = generateShowPlans(s, r);
  EXPECT_EQ(plans->render.program.layers.at(1).binding.status, PlannedBindingStatus::Blank);
  EXPECT_EQ(plans->audio.routes.at(0).binding.status, PlannedBindingStatus::Resolved);
  EXPECT_FALSE(plans->audio.routes.at(0).intent.muted);
  EXPECT_EQ(plans->audio.routes.at(0).eligibility, PlannedAudioEligibility::UnknownCapability);
  EXPECT_EQ(*plans->audio.routes.at(0).binding.sourceKind, SourceRegistry::Kind::ParticipantVideo);
  EXPECT_EQ(plans->audio.routes.at(0).intent.gainMilliDb, -6000);
  EXPECT_EQ(plans->render.inputs.at(1).binding.status, PlannedBindingStatus::RequiresSelection);
  s.data.program->generation = 2;
  EXPECT_EQ(generateShowPlans(s, r)->render.program.status, PlannedBindingStatus::Missing);
}

TEST(ShowPlanGenerator, OldPersonBindingCannotAliasNewPersonGeneration) {
  auto s = show(); auto r = roster();
  r.sources[0].personGeneration = 1;
  EXPECT_EQ(generateShowPlans(s, r)->output.isoSelections.at(0).videoBinding.status, PlannedBindingStatus::Missing);
  // Fixed identity remains independent of durable-person rebinding.
  EXPECT_EQ(generateShowPlans(s, r)->output.isoSelections.at(1).videoBinding.status, PlannedBindingStatus::Resolved);
}
TEST(ShowPlanGenerator, ExcludedReservedInputRemainsAnExplicitHole) {
  auto s = show(); const auto r = roster();
  ShowTilesIntent tiles;
  tiles.reservedSlots[0] = ShowEntityRef{"first", 1};
  tiles.excludedInputs.insert({"first", 1});
  tiles.allowRosterAdditions = true;
  s.data.tiles["tiles"] = tiles;
  const auto planned = generateShowPlans(s, r)->render.tiles.at(0);
  ASSERT_FALSE(planned.slots.empty());
  EXPECT_EQ(planned.slots.at(0).slot, 0U);
  EXPECT_TRUE(planned.slots.at(0).reserved);
  ASSERT_TRUE(planned.slots.at(0).input.has_value());
  EXPECT_EQ(planned.slots.at(0).input->id, "first");
  EXPECT_EQ(planned.slots.at(0).binding.status, PlannedBindingStatus::Excluded);
  EXPECT_FALSE(planned.slots.at(0).binding.source.has_value());
  for (const auto& slot : planned.slots)
    if (slot.binding.source) EXPECT_NE(slot.binding.source->sourceId, "a");
}

TEST(ShowPlanGenerator, VideoFreshnessAndCameraAvailabilityDoNotEraseAudioIntent) {
  auto s = show();
  auto r = roster();
  r.sources[0].lastPublicationNs = 100;
  const auto stale = generateShowPlans(s, r, {101});
  EXPECT_EQ(stale->render.stamp.videoFreshAfterNs, 101);
  EXPECT_NE(stale->render.stamp, generateShowPlans(s, r, {100})->render.stamp);
  EXPECT_EQ(stale->render.inputs.at(1).binding.status, PlannedBindingStatus::Stale);
  EXPECT_EQ(stale->output.isoSelections.at(0).videoBinding.status, PlannedBindingStatus::Stale);
  EXPECT_EQ(stale->output.isoSelections.at(0).audioBinding.status, PlannedBindingStatus::Resolved);

  r.sources[0].availability = SourceRegistry::Availability::Unavailable;
  const auto cameraOff = generateShowPlans(s, r, {0});
  EXPECT_EQ(cameraOff->render.inputs.at(1).binding.status, PlannedBindingStatus::Unavailable);
  EXPECT_EQ(cameraOff->output.isoSelections.at(0).videoBinding.status,
            PlannedBindingStatus::Unavailable);
  EXPECT_EQ(cameraOff->output.isoSelections.at(0).audioBinding.status,
            PlannedBindingStatus::Resolved);
  EXPECT_EQ(cameraOff->audio.routes.at(0).binding.status, PlannedBindingStatus::Resolved);
}
