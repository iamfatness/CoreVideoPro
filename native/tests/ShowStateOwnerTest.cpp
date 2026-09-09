#include "core/ShowStateOwner.h"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <type_traits>

using namespace corevideo::core;
namespace {
ShowStateData exampleShow() {
  ShowStateData data;
  data.inputs["input-a"] = {1, "Camera", {ShowRouteKind::FixedSource, ShowEntityRef{"source-a", 3}}};
  data.inputOrder = {"input-a"};
  data.isoSelections["iso-a"] = {1, data.inputs.at("input-a").target};
  data.isoOrder = {"iso-a"};
  ShowSceneIntent scene;
  scene.routes["route-a"].target = data.inputs.at("input-a").target;
  scene.layerOrder = {"route-a"};
  data.scenes["scene-a"] = scene;
  data.preview = ShowEntityRef{"scene-a", 1};
  data.outputs["program-output"] = {};
  data.audioRoutes["audio-a"] = {1, data.inputs.at("input-a").target, {"program-output", 1}, 0, false};
  data.overlays["overlay-a"] = {1, ShowEntityRef{"source-a", 3}, "Title", false};
  return data;
}
}

TEST(ShowStateOwner, PublishesOneRevisionForWholeSemanticTransaction) {
  ShowStateOwner owner("authority-a");
  const auto blank = owner.snapshot();
  EXPECT_EQ(blank->revision, 0ULL);
  EXPECT_FALSE(blank->data.preview.has_value());
  const auto changed = owner.replace("authority-a", 0, exampleShow());
  EXPECT_EQ(changed.status, ShowStateUpdateStatus::Changed);
  EXPECT_EQ(changed.snapshot->revision, 1ULL);
  EXPECT_EQ(changed.snapshot->data.inputs.size(), 1U);
  EXPECT_EQ(changed.snapshot->data.isoSelections.size(), 1U);
  EXPECT_EQ(changed.snapshot->data.audioRoutes.size(), 1U);
  EXPECT_EQ(changed.snapshot->data.overlays.size(), 1U);
  const auto repeat = owner.replace("authority-a", 1, exampleShow());
  EXPECT_EQ(repeat.status, ShowStateUpdateStatus::Unchanged);
  EXPECT_EQ(repeat.snapshot.get(), changed.snapshot.get());
  EXPECT_EQ(owner.replace("authority-a", 0, exampleShow()).status, ShowStateUpdateStatus::Conflict);
  EXPECT_EQ(owner.replace("authority-b", 1, exampleShow()).status, ShowStateUpdateStatus::Conflict);
}
TEST(ShowStateOwner, ReadersAndCallerCopiesCannotMutatePublishedSnapshot) {
  ShowStateOwner owner("authority-a");
  auto candidate = exampleShow();
  const auto original = owner.replace("authority-a", 0, candidate).snapshot;
  static_assert(std::is_const_v<std::remove_reference_t<decltype(*original)>>);
  candidate.inputs.at("input-a").label = "Changed";
  candidate.inputs.at("input-a").generation++;
  candidate.program = candidate.preview;
  const auto next = owner.replace("authority-a", 1, candidate).snapshot;
  EXPECT_EQ(original->revision, 1ULL);
  EXPECT_EQ(original->data.inputs.at("input-a").label, "Camera");
  EXPECT_FALSE(original->data.program.has_value());
  EXPECT_TRUE(next->data.program.has_value());
  EXPECT_EQ(next->revision, 2ULL);
}
TEST(ShowStateOwner, MissingFixedSourceNeverFallsBackAndBlankIsDistinct) {
  const std::set<ShowEntityRef> available{{"source-a", 4}, {"other", 3}};
  EXPECT_EQ(classifyShowRoute({}, available), ShowRouteResolution::Blank);
  const ShowRouteTarget fixed{ShowRouteKind::FixedSource, ShowEntityRef{"source-a", 3}};
  EXPECT_EQ(classifyShowRoute(fixed, available), ShowRouteResolution::Missing);
  EXPECT_EQ(classifyShowRoute(fixed, {{"source-a", 3}}), ShowRouteResolution::Available);
  EXPECT_EQ(classifyShowRoute({ShowRouteKind::ActiveSpeaker, std::nullopt}, available), ShowRouteResolution::RequiresSelection);
  ShowStateOwner owner("authority-a");
  EXPECT_EQ(owner.replace("authority-a", 0, exampleShow()).status, ShowStateUpdateStatus::Changed);
  // Desired missing source bindings survive independently of roster availability.
  EXPECT_EQ(owner.snapshot()->data.inputs.at("input-a").target.source->generation, 3ULL);
}
TEST(ShowStateOwner, RejectsDanglingSceneAndAmbiguousRouteWithoutPublication) {
  ShowStateOwner owner("authority-a");
  auto candidate = exampleShow();
  candidate.preview->generation = 2;
  EXPECT_EQ(owner.replace("authority-a", 0, candidate).status, ShowStateUpdateStatus::Invalid);
  candidate = exampleShow();
  candidate.inputs.at("input-a").target.kind = ShowRouteKind::Blank;
  EXPECT_EQ(owner.replace("authority-a", 0, candidate).status, ShowStateUpdateStatus::Invalid);
  candidate = exampleShow();
  candidate.scenes.at("scene-a").layerOrder = {"missing-route"};
  EXPECT_EQ(owner.replace("authority-a", 0, candidate).status, ShowStateUpdateStatus::Invalid);
  candidate = exampleShow();
  candidate.audioRoutes.at("audio-a").destination.generation = 2;
  EXPECT_EQ(owner.replace("authority-a", 0, candidate).status, ShowStateUpdateStatus::Invalid);
  EXPECT_EQ(owner.snapshot()->revision, 0ULL);
}
TEST(ShowStateOwner, ConcurrentWritersCannotLoseAcceptedUpdates) {
  ShowStateOwner owner("authority-a");
  std::atomic<int> accepted{0};
  std::atomic<bool> invalid{false};
  std::vector<std::thread> writers;
  for (int worker = 0; worker < 6; ++worker) writers.emplace_back([&, worker] {
    for (int iteration = 0; iteration < 25; ++iteration) {
      for (;;) {
        const auto before = owner.snapshot();
        auto candidate = before->data;
        const auto id = "input-" + std::to_string(worker);
        if (!candidate.inputs.contains(id)) candidate.inputOrder.push_back(id);
        auto& input = candidate.inputs[id];
        input.label = std::to_string(iteration);
        input.generation = static_cast<std::uint64_t>(iteration + 1);
        const auto result = owner.replace(before->authorityEpoch, before->revision, std::move(candidate));
        if (result.status == ShowStateUpdateStatus::Conflict) continue;
        if (result.status != ShowStateUpdateStatus::Changed) invalid = true;
        ++accepted;
        break;
      }
    }
  });
  for (auto& worker : writers) worker.join();
  EXPECT_FALSE(invalid.load());
  EXPECT_EQ(accepted.load(), 150);
  const auto final = owner.snapshot();
  EXPECT_EQ(final->revision, 150ULL);
  EXPECT_EQ(final->data.inputs.size(), 6U);
  for (const auto& [id, input] : final->data.inputs) EXPECT_EQ(input.label, "24");
}
TEST(ShowStateOwner, ConcurrentSameRevisionHasExactlyOneWinner) {
  ShowStateOwner owner("authority-a");
  std::atomic<int> winners{0};
  std::atomic<int> conflicts{0};
  std::vector<std::thread> writers;
  for (int i = 0; i < 8; ++i) writers.emplace_back([&, i] {
    auto candidate = exampleShow();
    candidate.inputs.at("input-a").label = std::to_string(i);
    const auto result = owner.replace("authority-a", 0, std::move(candidate));
    if (result.status == ShowStateUpdateStatus::Changed) ++winners;
    if (result.status == ShowStateUpdateStatus::Conflict) ++conflicts;
  });
  for (auto& writer : writers) writer.join();
  EXPECT_EQ(winners.load(), 1);
  EXPECT_EQ(conflicts.load(), 7);
  EXPECT_EQ(owner.snapshot()->revision, 1ULL);
}

TEST(ShowStateOwner, EntityChangesRequireHigherGenerationAndDeletionKeepsTombstones) {
  ShowStateOwner owner("authority-a");
  auto data = exampleShow();
  ASSERT_EQ(owner.replace("authority-a", 0, data).status, ShowStateUpdateStatus::Changed);
  data.inputs.at("input-a").label = "New";
  EXPECT_EQ(owner.replace("authority-a", 1, data).status, ShowStateUpdateStatus::Invalid);
  data.inputs.at("input-a").generation = 2;
  ASSERT_EQ(owner.replace("authority-a", 1, data).status, ShowStateUpdateStatus::Changed);
  data.inputs.clear(); data.inputOrder.clear();
  ASSERT_EQ(owner.replace("authority-a", 2, data).status, ShowStateUpdateStatus::Changed);
  data.inputs["input-a"] = exampleShow().inputs.at("input-a");
  data.inputOrder = {"input-a"};
  EXPECT_EQ(owner.replace("authority-a", 3, data).status, ShowStateUpdateStatus::Invalid);
  data.inputs.at("input-a").generation = 2;
  EXPECT_EQ(owner.replace("authority-a", 3, data).status, ShowStateUpdateStatus::Invalid);
  data.inputs.at("input-a").generation = 3;
  EXPECT_EQ(owner.replace("authority-a", 3, data).status, ShowStateUpdateStatus::Changed);
}
TEST(ShowStateOwner, SceneAndLocalRouteTombstonesPreventStaleBusAliasing) {
  ShowStateOwner owner("authority-a");
  auto data = exampleShow();
  ASSERT_EQ(owner.replace("authority-a", 0, data).status, ShowStateUpdateStatus::Changed);
  data.scenes.clear(); data.preview.reset();
  ASSERT_EQ(owner.replace("authority-a", 1, data).status, ShowStateUpdateStatus::Changed);
  data.scenes = exampleShow().scenes;
  data.scenes.at("scene-a").generation = 2;
  // Even recreating the parent cannot reuse a retired scene-local route generation.
  EXPECT_EQ(owner.replace("authority-a", 2, data).status, ShowStateUpdateStatus::Invalid);
  data.scenes.at("scene-a").routes.at("route-a").generation = 2;
  data.preview = ShowEntityRef{"scene-a", 1};
  EXPECT_EQ(owner.replace("authority-a", 2, data).status, ShowStateUpdateStatus::Invalid);
  data.preview->generation = 2;
  EXPECT_EQ(owner.replace("authority-a", 2, data).status, ShowStateUpdateStatus::Changed);
}
TEST(ShowStateOwner, OrderAndTilesMembershipAreExplicitAndValidated) {
  ShowStateOwner owner("authority-a");
  auto data = exampleShow();
  data.inputs["input-b"] = {};
  data.inputOrder.push_back("input-b");
  ShowTilesIntent tiles;
  tiles.allowRosterAdditions = false;
  tiles.includedInputs.insert({"input-a", 1});
  tiles.excludedInputs.insert({"input-b", 1});
  tiles.reservedSlots[0] = ShowEntityRef{"input-a", 1};
  tiles.reservedSlots[1] = ShowEntityRef{"input-b", 1};
  tiles.reservedSlots[2] = std::nullopt;
  data.tiles["tiles-a"] = tiles;
  ASSERT_EQ(owner.replace("authority-a", 0, data).status, ShowStateUpdateStatus::Changed);
  data.inputOrder = {"input-b", "input-a"};
  ASSERT_EQ(owner.replace("authority-a", 1, data).status, ShowStateUpdateStatus::Changed);
  EXPECT_FALSE(owner.snapshot()->data.tiles.at("tiles-a").reservedSlots.at(2).has_value());
  data.inputOrder = {"input-a", "input-a"};
  EXPECT_EQ(owner.replace("authority-a", 2, data).status, ShowStateUpdateStatus::Invalid);
  data.inputOrder = {"input-a", "input-b"};
  data.tiles.at("tiles-a").generation++;
  data.tiles.at("tiles-a").includedInputs.insert({"input-b", 1});
  ASSERT_EQ(owner.replace("authority-a", 2, data).status, ShowStateUpdateStatus::Changed);
  ASSERT_TRUE(owner.snapshot()->data.tiles.at("tiles-a").reservedSlots.at(1).has_value());
  EXPECT_EQ(owner.snapshot()->data.tiles.at("tiles-a").reservedSlots.at(1)->id, "input-b");
  data.tiles.at("tiles-a").generation++;
  data.tiles.at("tiles-a").excludedInputs.erase({"input-b", 1});
  ASSERT_EQ(owner.replace("authority-a", 3, data).status, ShowStateUpdateStatus::Changed);
  EXPECT_TRUE(owner.snapshot()->data.tiles.at("tiles-a").includedInputs.contains({"input-b", 1}));
  data.tiles.at("tiles-a").reservedSlots[3] = ShowEntityRef{"input-a", 1};
  EXPECT_EQ(owner.replace("authority-a", 4, data).status, ShowStateUpdateStatus::Invalid);
}
TEST(ShowStateOwner, OrderedIsoAndPersonIntentSurviveMissingVideoWithoutMutingAudio) {
  ShowStateOwner owner("authority-a");
  auto data = exampleShow();
  data.isoSelections["iso-follow"] =
      {1, {ShowRouteKind::FollowPerson, std::nullopt, ShowEntityRef{"person-a", 1}}};
  data.isoOrder.push_back("iso-follow");
  data.inputs.at("input-a").target = {ShowRouteKind::FollowPerson, std::nullopt, ShowEntityRef{"person-a", 1}};
  data.scenes.at("scene-a").routes.at("route-a").target = {};
  ASSERT_EQ(owner.replace("authority-a", 0, data).status, ShowStateUpdateStatus::Changed);
  const auto state = owner.snapshot();
  EXPECT_EQ(state->data.isoOrder.at(1), "iso-follow");
  EXPECT_EQ(state->data.isoSelections.at("iso-a").target.kind, ShowRouteKind::FixedSource);
  EXPECT_EQ(state->data.isoSelections.at("iso-follow").target.kind, ShowRouteKind::FollowPerson);
  EXPECT_FALSE(state->data.audioRoutes.at("audio-a").muted);
  EXPECT_EQ(state->data.audioRoutes.at("audio-a").source.kind, ShowRouteKind::FixedSource);
  EXPECT_EQ(classifyShowRoute(state->data.inputs.at("input-a").target, {}), ShowRouteResolution::RequiresSelection);
  std::swap(data.isoOrder[0], data.isoOrder[1]);
  EXPECT_EQ(owner.replace("authority-a", 1, data).status, ShowStateUpdateStatus::Changed);
  data.isoOrder.push_back(data.isoOrder.front());
  EXPECT_EQ(owner.replace("authority-a", 2, data).status, ShowStateUpdateStatus::Invalid);
}

TEST(ShowStateOwner, RosterAdmissionIsExplicitAndGenerationHistoryIsBounded) {
  ShowStateOwner owner("authority-a", 2);
  ShowStateData data;
  data.inputs["input-a"] = {};
  data.inputOrder = {"input-a"};
  ASSERT_EQ(owner.replace("authority-a", 0, data).status, ShowStateUpdateStatus::Changed);
  data.inputs.clear();
  data.inputOrder.clear();
  ASSERT_EQ(owner.replace("authority-a", 1, data).status, ShowStateUpdateStatus::Changed);
  data.inputs["input-b"] = {};
  data.inputOrder = {"input-b"};
  ASSERT_EQ(owner.replace("authority-a", 2, data).status, ShowStateUpdateStatus::Changed);
  data.inputs.clear();
  data.inputOrder.clear();
  ASSERT_EQ(owner.replace("authority-a", 3, data).status, ShowStateUpdateStatus::Changed);
  data.inputs["input-c"] = {};
  data.inputOrder = {"input-c"};
  EXPECT_EQ(owner.replace("authority-a", 4, data).status, ShowStateUpdateStatus::CapacityExceeded);
  EXPECT_EQ(owner.snapshot()->revision, 4ULL);

  ShowStateOwner tilesOwner("tiles-authority");
  auto tilesData = exampleShow();
  tilesData.tiles["tiles-a"] = {};
  ASSERT_EQ(tilesOwner.replace("tiles-authority", 0, tilesData).status,
            ShowStateUpdateStatus::Changed);
  EXPECT_FALSE(tilesOwner.snapshot()->data.tiles.at("tiles-a").allowRosterAdditions);
  tilesData.tiles.at("tiles-a").generation = 2;
  tilesData.tiles.at("tiles-a").allowRosterAdditions = true;
  EXPECT_EQ(tilesOwner.replace("tiles-authority", 1, tilesData).status,
            ShowStateUpdateStatus::Changed);
}
