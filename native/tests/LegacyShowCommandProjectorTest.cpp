#include "core/LegacyShowCommandProjector.h"
#include <gtest/gtest.h>

using namespace corevideo::core;
namespace {
using Error = LegacyShowProjection::Error;
ShowRouteTarget pin(std::string id, std::uint64_t generation = 1) {
  const auto identity = id;
  return {ShowRouteKind::FixedSource,ShowSourceRef{std::move(id),"instance-"+identity,"process-1",generation},std::nullopt};
}
ShowRouteTarget person(std::string id) {
  return {ShowRouteKind::FollowPerson,std::nullopt,ShowEntityRef{std::move(id),1}};
}
LegacyShowCommandDto initial() {
  LegacyShowCommandDto dto; dto.authorityEpoch = "authority";
  dto.inputs = std::vector<LegacyNamedShowIntent<ShowInputIntent>>{
    {"guest",{1,"Same display name",pin("alice-instance",2)}},
    {"host",{1,"Same display name",person("bob")}}};
  ShowLayerIntent layer; layer.target = pin("alice-instance",2);
  LegacySceneDto scene; scene.id = "scene"; scene.layers.push_back({"speaker",layer});
  dto.scenes = std::vector<LegacySceneDto>{scene};
  dto.preview = {true,ShowEntityRef{"scene",1}};
  dto.outputs = std::vector<LegacyNamedShowIntent<ShowOutputIntent>>{{"program-out",{}}};
  ShowAudioRouteIntent audio; audio.source = person("alice"); audio.destination = {"program-out",1};
  dto.audioRoutes = std::vector<LegacyNamedShowIntent<ShowAudioRouteIntent>>{{"audio",audio}};
  dto.isoSelections = std::vector<LegacyNamedShowIntent<ShowIsoSelectionIntent>>{
    {"fixed-iso",{1,pin("alice-instance",2)}},{"following-iso",{1,person("alice")}}};
  return dto;
}
ShowStateSnapshot base() {
  ShowStateSnapshot empty{"authority",0,{}};
  auto result = LegacyShowCommandProjector::project(empty,initial());
  return {"authority",1,*result.candidate};
}
LegacyShowCommandDto omitted() {
  LegacyShowCommandDto dto; dto.authorityEpoch = "authority"; dto.expectedRevision = 1; return dto;
}
}
TEST(LegacyShowCommandProjector, OmittedStateIsPreservedAndPreviewDoesNotImplicitlyTake) {
  const auto before = base();
  const auto projected = LegacyShowCommandProjector::project(before,omitted());
  ASSERT_TRUE(projected.candidate.has_value());
  EXPECT_EQ(*projected.candidate,before.data);
  EXPECT_TRUE(projected.candidate->preview.has_value());
  EXPECT_FALSE(projected.candidate->program.has_value());
  EXPECT_EQ(before.revision,1ULL); // Pure projection never publishes/increments.
}
TEST(LegacyShowCommandProjector, ExplicitClearIsAtomicAndNeverSilentlyClearsReferences) {
  const auto before = base(); auto dto = omitted();
  dto.scenes = std::vector<LegacySceneDto>{};
  EXPECT_EQ(LegacyShowCommandProjector::project(before,dto).error,Error::Invalid);
  dto.preview = {true,std::nullopt};
  const auto clear = LegacyShowCommandProjector::project(before,dto);
  ASSERT_TRUE(clear.candidate.has_value());
  EXPECT_TRUE(clear.candidate->scenes.empty());
  EXPECT_FALSE(clear.candidate->preview.has_value());
  EXPECT_EQ(clear.candidate->audioRoutes,before.data.audioRoutes);
}
TEST(LegacyShowCommandProjector, LeaveAndReplacementKeepExactPinMissingInsteadOfSubstitution) {
  const auto before = base();
  const auto projected = LegacyShowCommandProjector::project(before,omitted());
  ASSERT_TRUE(projected.candidate.has_value());
  const auto& target = projected.candidate->scenes.at("scene").routes.at("speaker").target;
  EXPECT_EQ(classifyShowRoute(target,{
    {"alice-instance","other-instance","process-1",2},
    {"bob-instance","instance-bob","process-1",2}}),ShowRouteResolution::Missing);
  EXPECT_EQ(target.source->sourceId,"alice-instance"); EXPECT_EQ(target.source->generation,2ULL);
  EXPECT_EQ(projected.candidate->isoSelections.at("following-iso").target.person->id,"alice");
  EXPECT_EQ(projected.candidate->isoSelections.at("fixed-iso").target.source->generation,2ULL);
}
TEST(LegacyShowCommandProjector, TenInputsKeepExplicitEditorialOrderNotLexicalOrder) {
  ShowStateSnapshot empty{"authority",0,{}}; auto dto = initial();
  dto.inputs->clear();
  for (int i : {9,1,8,2,7,3,6,4,5,0}) dto.inputs->push_back({"input-"+std::to_string(i),{1,"Duplicate name",person("person-"+std::to_string(i))}});
  const auto result = LegacyShowCommandProjector::project(empty,dto);
  ASSERT_TRUE(result.candidate.has_value());
  EXPECT_EQ(result.candidate->inputOrder.size(),10U);
  EXPECT_EQ(result.candidate->inputOrder.front(),"input-9");
  EXPECT_EQ(result.candidate->inputOrder.back(),"input-0");
}
TEST(LegacyShowCommandProjector, TilesCarriesChosenInputsExclusionsManualHolesAndOptIn) {
  const auto before = base(); auto dto = omitted();
  LegacyTilesDto tiles; tiles.id = "tiles"; tiles.includedInputs = {{"guest",1}};
  tiles.excludedInputs = {{"host",1}};
  tiles.reservedSlots = {{0,ShowEntityRef{"guest",1}},{1,std::nullopt},{2,ShowEntityRef{"host",1}}};
  dto.tiles = std::vector<LegacyTilesDto>{tiles};
  auto result = LegacyShowCommandProjector::project(before,dto);
  ASSERT_TRUE(result.candidate.has_value());
  EXPECT_FALSE(result.candidate->tiles.at("tiles").allowRosterAdditions);
  EXPECT_EQ(result.candidate->tiles.at("tiles").includedInputs.size(),1U);
  EXPECT_FALSE(result.candidate->tiles.at("tiles").reservedSlots.at(1).has_value());
  EXPECT_EQ(result.candidate->tiles.at("tiles").reservedSlots.at(2)->id,"host");
  dto.tiles->at(0).allowRosterAdditions = true;
  result = LegacyShowCommandProjector::project(before,dto);
  EXPECT_TRUE(result.candidate->tiles.at("tiles").allowRosterAdditions);
}
TEST(LegacyShowCommandProjector, VideoBlankNeverRewritesIndependentAudioOrIsoArming) {
  const auto before = base(); auto dto = omitted();
  auto scene = initial().scenes->at(0); scene.generation = 2;
  scene.layers[0].intent.generation = 2; scene.layers[0].intent.target = {};
  dto.scenes = std::vector<LegacySceneDto>{scene}; dto.preview = {true,ShowEntityRef{"scene",2}};
  const auto result = LegacyShowCommandProjector::project(before,dto);
  ASSERT_TRUE(result.candidate.has_value());
  EXPECT_EQ(result.candidate->scenes.at("scene").routes.at("speaker").target.kind,ShowRouteKind::Blank);
  EXPECT_EQ(result.candidate->audioRoutes,before.data.audioRoutes);
  EXPECT_EQ(result.candidate->isoSelections,before.data.isoSelections);
}
TEST(LegacyShowCommandProjector, OverlayReplacementSupportsLowerThirdShowHideAndClear) {
  const auto before = base(); auto dto = omitted();
  ShowOverlayIntent lowerThird; lowerThird.generation = 1;
  lowerThird.content = "Jamal | Host"; lowerThird.requestedVisible = true;
  dto.overlays = std::vector<LegacyNamedShowIntent<ShowOverlayIntent>>{{"lower-third",lowerThird}};
  auto result = LegacyShowCommandProjector::project(before,dto);
  ASSERT_TRUE(result.candidate.has_value());
  EXPECT_TRUE(result.candidate->overlays.at("lower-third").requestedVisible);
  dto.expectedRevision = 1;
  dto.overlays->front().intent.generation = 2;
  dto.overlays->front().intent.requestedVisible = false;
  ShowStateSnapshot withOverlay{"authority",1,*result.candidate};
  result = LegacyShowCommandProjector::project(withOverlay,dto);
  ASSERT_TRUE(result.candidate.has_value());
  EXPECT_FALSE(result.candidate->overlays.at("lower-third").requestedVisible);
  dto.expectedRevision = 1; dto.overlays = std::vector<LegacyNamedShowIntent<ShowOverlayIntent>>{};
  EXPECT_TRUE(LegacyShowCommandProjector::project(withOverlay,dto).candidate->overlays.empty());
}
TEST(LegacyShowCommandProjector, TextBudgetRejectsLargePayloadBeforeProjection) {
  ShowStateSnapshot empty{"authority",0,{}}; auto dto = initial();
  dto.inputs->front().intent.label = std::string(1024,'x');
  EXPECT_EQ(LegacyShowCommandProjector::project(empty,dto,4096,128).error,Error::Capacity);
  dto = initial();
  ShowOverlayIntent overlay; overlay.content = std::string(1024,'x');
  dto.overlays = std::vector<LegacyNamedShowIntent<ShowOverlayIntent>>{{"lower-third",overlay}};
  EXPECT_EQ(LegacyShowCommandProjector::project(empty,dto,4096,128).error,Error::Capacity);
}
TEST(LegacyShowCommandProjector, DuplicateMissingAndStaleIdentitiesRejectWholeCandidate) {
  ShowStateSnapshot empty{"authority",0,{}};
  auto duplicate = initial(); duplicate.inputs->push_back(duplicate.inputs->front());
  EXPECT_EQ(LegacyShowCommandProjector::project(empty,duplicate).error,Error::Invalid);
  auto missing = initial(); missing.inputs->front().intent.target.source->sourceId.clear();
  EXPECT_EQ(LegacyShowCommandProjector::project(empty,missing).error,Error::Invalid);
  auto layer = initial(); layer.scenes->front().layers.push_back(layer.scenes->front().layers.front());
  EXPECT_EQ(LegacyShowCommandProjector::project(empty,layer).error,Error::Invalid);
  const auto before = base(); auto stale = omitted();
  stale.inputs = initial().inputs; stale.inputs->front().intent.label = "Changed without generation";
  EXPECT_EQ(LegacyShowCommandProjector::project(before,stale).error,Error::Invalid);
  stale.inputs->front().intent.generation = 2;
  EXPECT_EQ(LegacyShowCommandProjector::project(before,stale).error,Error::None);
}
TEST(LegacyShowCommandProjector, RejectsDanglingAudioOutputAndBoundsCandidate) {
  const auto before = base(); auto dto = omitted();
  dto.outputs = std::vector<LegacyNamedShowIntent<ShowOutputIntent>>{};
  EXPECT_EQ(LegacyShowCommandProjector::project(before,dto).error,Error::Invalid);
  dto.audioRoutes = std::vector<LegacyNamedShowIntent<ShowAudioRouteIntent>>{};
  EXPECT_EQ(LegacyShowCommandProjector::project(before,dto).error,Error::None);
  EXPECT_EQ(LegacyShowCommandProjector::project(before,omitted(),1).error,Error::Capacity);
  dto = omitted(); dto.expectedRevision = 0;
  EXPECT_EQ(LegacyShowCommandProjector::project(before,dto).error,Error::Conflict);
  dto = omitted(); dto.authorityEpoch = "old-authority";
  EXPECT_EQ(LegacyShowCommandProjector::project(before,dto).error,Error::Conflict);
}
