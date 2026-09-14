#include "core/DesiredShowCheckpoint.h"
#include <gtest/gtest.h>
using namespace corevideo::core;
namespace {
using Checkpoint=DesiredShowCheckpoint;
Checkpoint::Capture capture() {
  Checkpoint::Capture c;c.legacyAuthorityEpoch="legacy";c.legacyRevision=7;
  c.program={true,std::nullopt,std::nullopt};c.preview=c.program;
  c.scenes.emplace();c.outputs.emplace();
  c.knownEmptyDomains=Checkpoint::emptyOnlyDomains;
  return c;
}
}
TEST(DesiredShowCheckpoint, ExplicitEmptyDomainsAreCompleteButUncapturedAreNot) {
  auto c=capture();const auto complete=Checkpoint::project(c);
  EXPECT_TRUE(complete.validBasis);EXPECT_TRUE(complete.complete);
  EXPECT_EQ(complete.desired.expectedRevision,7ULL);
  EXPECT_TRUE(complete.desired.program.supplied);EXPECT_FALSE(complete.desired.program.scene);
  c.knownEmptyDomains=0;
  const auto partial=Checkpoint::project(c);
  EXPECT_FALSE(partial.complete);EXPECT_EQ(partial.unsupportedDomains,Checkpoint::emptyOnlyDomains);
  EXPECT_FALSE(partial.desired.isoSelections.has_value());
}
TEST(DesiredShowCheckpoint, MissingGenerationsAndSdkOnlyPinsRemainUnsupported) {
  auto c=capture();Checkpoint::Scene scene;scene.id="scene";
  c.scenes->push_back(scene);c.program={true,"scene",1};
  auto result=Checkpoint::project(c);
  EXPECT_EQ(result.reasons[2],Checkpoint::Reason::MissingGeneration);
  EXPECT_FALSE(result.desired.scenes);EXPECT_FALSE(result.desired.program.supplied);
  c.scenes->at(0).generation=1;
  Checkpoint::Route route;route.id="route";route.generation=1;
  route.target={ShowRouteKind::FixedSource,ShowSourceRef{"zoom:42","","",1},std::nullopt};
  c.scenes->at(0).orderedRoutes.push_back(route);
  result=Checkpoint::project(c);
  EXPECT_EQ(result.reasons[2],Checkpoint::Reason::MissingSourceIdentity);
  EXPECT_FALSE(result.complete);
}
TEST(DesiredShowCheckpoint, PreservesExactOrderedBasicSceneAndIndependentOutputRequest) {
  auto c=capture();Checkpoint::Scene scene;scene.id="scene";scene.generation=3;
  Checkpoint::Route front;front.id="front";front.generation=2;
  Checkpoint::Route back;back.id="back";back.generation=4;
  back.target={ShowRouteKind::FixedSource,ShowSourceRef{"source","instance","process",9},std::nullopt};
  scene.orderedRoutes={front,back};c.scenes->push_back(scene);c.preview={true,"scene",3};
  c.outputs->push_back({"recorder",2,ShowOutputIntent::Kind::Recorder,true,false});
  const auto result=Checkpoint::project(c);
  ASSERT_TRUE(result.complete);ASSERT_TRUE(result.desired.scenes);
  EXPECT_EQ(result.desired.scenes->at(0).layers.at(0).id,"front");
  EXPECT_EQ(result.desired.scenes->at(0).layers.at(1).intent.target.source->instanceId,"instance");
  EXPECT_FALSE(result.desired.program.scene);EXPECT_TRUE(result.desired.outputs->at(0).intent.requested);
  EXPECT_TRUE(result.desired.audioRoutes->empty());
}
TEST(DesiredShowCheckpoint, RichPoliciesAreNotSilentlyDiscarded) {
  auto c=capture();Checkpoint::Scene scene;scene.id="scene";scene.generation=1;
  Checkpoint::Route route;route.id="route";route.generation=1;route.unsupportedPolicies=true;
  scene.orderedRoutes.push_back(route);c.scenes->push_back(scene);
  const auto result=Checkpoint::project(c);
  EXPECT_EQ(result.reasons[2],Checkpoint::Reason::UnsupportedPolicy);
  EXPECT_FALSE(result.desired.scenes);EXPECT_FALSE(result.complete);
  EXPECT_TRUE(result.desired.program.supplied); // Explicit blank remains truthful.
}
TEST(DesiredShowCheckpoint, BasisCapacityAndInvalidEmptyDeclarationsFailClosed) {
  auto c=capture();c.legacyRevision.reset();EXPECT_FALSE(Checkpoint::project(c).validBasis);
  c=capture();c.legacyAuthorityEpoch.clear();EXPECT_FALSE(Checkpoint::project(c).validBasis);
  c=capture();c.knownEmptyDomains|=static_cast<uint32_t>(Checkpoint::Domain::Scenes);
  EXPECT_FALSE(Checkpoint::project(c).validBasis);
  c=capture();Checkpoint::Scene scene;scene.id="scene";scene.generation=1;scene.label=std::string(100,'x');
  c.scenes->push_back(scene);EXPECT_FALSE(Checkpoint::project(c,4096,50).validBasis);
}

TEST(DesiredShowCheckpoint, NamedBusWrongGenerationDoesNotClaimSupportedReference) {
  auto c=capture(); Checkpoint::Scene scene; scene.id="scene"; scene.generation=3;
  c.scenes->push_back(scene);
  c.program={true,"scene",2}; c.preview={true,"scene",3};
  const auto result=Checkpoint::project(c);
  EXPECT_TRUE(result.validBasis);
  EXPECT_FALSE(result.complete);
  EXPECT_EQ(result.reasons[0],Checkpoint::Reason::Invalid);
  EXPECT_EQ(result.unsupportedDomains,static_cast<uint32_t>(Checkpoint::Domain::Program));
  EXPECT_FALSE(result.desired.program.supplied);
  EXPECT_FALSE(result.desired.program.scene);
  EXPECT_TRUE(result.desired.preview.supplied);
  ASSERT_TRUE(result.desired.preview.scene);
  EXPECT_EQ(result.desired.preview.scene->generation,3ULL);
  ASSERT_TRUE(result.desired.scenes);
  EXPECT_EQ(result.desired.scenes->size(),1u);
}

TEST(DesiredShowCheckpoint, CombinedSceneRouteOutputAdmissionHonorsExactEntityLimit) {
  auto c=capture(); Checkpoint::Scene scene; scene.id="scene"; scene.generation=1;
  Checkpoint::Route route; route.id="route"; route.generation=1;
  scene.orderedRoutes.push_back(route); c.scenes->push_back(scene);
  c.outputs->push_back({"program",1,ShowOutputIntent::Kind::Program,true,false});
  const auto exact=Checkpoint::project(c,3);
  EXPECT_TRUE(exact.validBasis); EXPECT_TRUE(exact.complete);
  ASSERT_TRUE(exact.desired.scenes); ASSERT_TRUE(exact.desired.outputs);
  EXPECT_EQ(exact.desired.scenes->at(0).layers.size(),1u);
  EXPECT_EQ(exact.desired.outputs->size(),1u);
  const auto shortLimit=Checkpoint::project(c,2);
  EXPECT_FALSE(shortLimit.validBasis); EXPECT_FALSE(shortLimit.complete);
  EXPECT_EQ(shortLimit.supportedDomains,0u);
  EXPECT_EQ(shortLimit.unsupportedDomains,Checkpoint::allDomains);
  EXPECT_FALSE(shortLimit.desired.scenes); EXPECT_FALSE(shortLimit.desired.outputs);
  c.outputs->push_back({"recorder",1,ShowOutputIntent::Kind::Recorder,true,false});
  EXPECT_FALSE(Checkpoint::project(c,3).validBasis);
  EXPECT_TRUE(Checkpoint::project(c,4).complete);
}
