#include "core/LegacyCaptureLedger.h"
#include <gtest/gtest.h>
#include <barrier>
#include <thread>
using namespace corevideo::core;
namespace {
LegacyCaptureLedger::Config config() { LegacyCaptureLedger::Config c;c.authorityEpoch="capture-epoch";return c; }
DesiredShowCheckpoint::Capture capture() {
  DesiredShowCheckpoint::Capture c;c.program={true,std::nullopt,std::nullopt};c.preview=c.program;
  c.scenes.emplace();c.outputs.emplace();c.knownEmptyDomains=DesiredShowCheckpoint::emptyOnlyDomains;
  DesiredShowCheckpoint::Scene scene;scene.id="scene";
  DesiredShowCheckpoint::Route route;route.id="route";scene.orderedRoutes.push_back(route);
  c.scenes->push_back(scene);c.preview.sceneId="scene";return c;
}
}
TEST(LegacyCaptureLedger, AllocatesEpochRevisionAndSemanticGenerationsWithoutCallerGuesses) {
  LegacyCaptureLedger ledger(config());auto c=capture();
  const auto first=ledger.publish(c);ASSERT_EQ(first.status,LegacyCaptureLedger::Status::Changed);
  EXPECT_EQ(first.snapshot->revision,1ULL);EXPECT_EQ(first.snapshot->authorityEpoch,"capture-epoch");
  EXPECT_EQ(*first.snapshot->capture.scenes->at(0).generation,1ULL);
  EXPECT_TRUE(first.snapshot->projection.complete);
  c.legacyAuthorityEpoch="untrusted";c.legacyRevision=999;c.scenes->at(0).generation=999;
  EXPECT_EQ(ledger.publish(c).status,LegacyCaptureLedger::Status::Unchanged);
  c.scenes->at(0).orderedRoutes[0].visible=false;
  const auto changed=ledger.publish(c);ASSERT_EQ(changed.status,LegacyCaptureLedger::Status::Changed);
  EXPECT_EQ(*changed.snapshot->capture.scenes->at(0).generation,2ULL);
  EXPECT_EQ(*changed.snapshot->capture.scenes->at(0).orderedRoutes[0].generation,2ULL);
  EXPECT_EQ(*first.snapshot->capture.scenes->at(0).generation,1ULL);
}
TEST(LegacyCaptureLedger, UncueRetainsGenerationButExplicitDeletePreventsReuse) {
  LegacyCaptureLedger ledger(config());auto c=capture();ledger.publish(c);
  auto uncued=c;uncued.scenes->clear();uncued.preview.sceneId.reset();
  ASSERT_EQ(ledger.publish(uncued).status,LegacyCaptureLedger::Status::Changed);
  auto restored=ledger.publish(c);ASSERT_EQ(restored.status,LegacyCaptureLedger::Status::Changed);
  EXPECT_EQ(*restored.snapshot->capture.scenes->at(0).generation,1ULL);
  ASSERT_EQ(ledger.publish(uncued,{{LegacyCaptureLedger::Retirement::Kind::Scene,"scene",""}}).status,LegacyCaptureLedger::Status::Changed);
  restored=ledger.publish(c);ASSERT_EQ(restored.status,LegacyCaptureLedger::Status::Changed);
  EXPECT_EQ(*restored.snapshot->capture.scenes->at(0).generation,2ULL);
  EXPECT_EQ(*restored.snapshot->capture.scenes->at(0).orderedRoutes[0].generation,2ULL);
}
TEST(LegacyCaptureLedger, UnsupportedPinsPoliciesAndUncapturedDomainsStayExplicit) {
  LegacyCaptureLedger ledger(config());auto c=capture();c.knownEmptyDomains=0;
  c.scenes->at(0).orderedRoutes[0].target={ShowRouteKind::FixedSource,ShowSourceRef{"zoom:42","","",1},std::nullopt};
  const auto result=ledger.publish(c);ASSERT_EQ(result.status,LegacyCaptureLedger::Status::Changed);
  EXPECT_FALSE(result.snapshot->projection.complete);
  EXPECT_EQ(result.snapshot->projection.reasons[2],DesiredShowCheckpoint::Reason::MissingSourceIdentity);
  EXPECT_FALSE(result.snapshot->projection.desired.isoSelections);
  EXPECT_FALSE(result.snapshot->projection.desired.program.scene);
}
TEST(LegacyCaptureLedger, CapacityPreservesPriorPublicationAndTombstones) {
  auto cfg=config();cfg.maxGenerationMarks=2;LegacyCaptureLedger ledger(cfg);auto c=capture();
  const auto first=ledger.publish(c);ASSERT_EQ(first.status,LegacyCaptureLedger::Status::Changed);
  c.outputs->push_back({"output",{},ShowOutputIntent::Kind::Recorder,true,false});
  EXPECT_EQ(ledger.publish(c).status,LegacyCaptureLedger::Status::Capacity);
  EXPECT_EQ(ledger.snapshot().get(),first.snapshot.get());
  c=capture();c.scenes->at(0).label=std::string(9*1024*1024,'x');
  EXPECT_EQ(ledger.publish(std::move(c)).status,LegacyCaptureLedger::Status::Capacity);
  EXPECT_EQ(ledger.snapshot()->revision,1ULL);
}
TEST(LegacyCaptureLedger, OutputRequestsAreNeverDerivedFromCapturePresence) {
  LegacyCaptureLedger ledger(config());auto c=capture();
  c.outputs->push_back({"recorder",{},ShowOutputIntent::Kind::Recorder,false,false});
  auto first=ledger.publish(c);ASSERT_EQ(first.status,LegacyCaptureLedger::Status::Changed);
  EXPECT_FALSE(first.snapshot->projection.desired.outputs->at(0).intent.requested);
  c.outputs->at(0).requested=true;
  auto next=ledger.publish(c);ASSERT_EQ(next.status,LegacyCaptureLedger::Status::Changed);
  EXPECT_TRUE(next.snapshot->projection.desired.outputs->at(0).intent.requested);
  EXPECT_EQ(next.snapshot->projection.desired.outputs->at(0).intent.generation,2ULL);
}

TEST(LegacyCaptureLedger, ConcurrentPublishersProduceOnlyLinearizableImmutableSnapshots) {
  using Status = LegacyCaptureLedger::Status;
  for (int iteration = 0; iteration < 32; ++iteration) {
    LegacyCaptureLedger ledger(config());
    const auto original = ledger.publish(capture());
    ASSERT_EQ(original.status, Status::Changed);
    auto left = capture(), right = capture();
    left.scenes->at(0).label = "left";
    right.scenes->at(0).label = "right";
    std::barrier start(3);
    LegacyCaptureLedger::Result results[2];
    std::thread a([&] { start.arrive_and_wait(); results[0] = ledger.publish(left); });
    std::thread b([&] { start.arrive_and_wait(); results[1] = ledger.publish(right); });
    start.arrive_and_wait();
    a.join(); b.join();
    const auto final = ledger.snapshot();
    const int changes = (results[0].status == Status::Changed ? 1 : 0) +
                        (results[1].status == Status::Changed ? 1 : 0);
    EXPECT_TRUE(changes == 1 || changes == 2);
    EXPECT_EQ(final->revision, static_cast<uint64_t>(1 + changes));
    for (int index = 0; index < 2; ++index) {
      const auto& result = results[index];
      EXPECT_TRUE(result.status == Status::Changed || result.status == Status::Conflict);
      ASSERT_TRUE(result.snapshot);
      EXPECT_TRUE(result.snapshot->revision > original.snapshot->revision);
      EXPECT_TRUE(result.snapshot->revision <= final->revision);
      EXPECT_EQ(result.snapshot->capture.legacyRevision, std::optional<uint64_t>{result.snapshot->revision});
      EXPECT_EQ(*result.snapshot->capture.scenes->at(0).generation, result.snapshot->revision);
      EXPECT_EQ(*result.snapshot->capture.scenes->at(0).orderedRoutes[0].generation, 1ULL);
      if (result.status == Status::Conflict) {
        EXPECT_EQ(result.snapshot.get(), final.get());
      } else {
        EXPECT_EQ(result.snapshot->capture.scenes->at(0).label, index == 0 ? "left" : "right");
      }
    }
    if (changes == 2) EXPECT_TRUE(results[0].snapshot->revision != results[1].snapshot->revision);
    // Neither successful replacement nor a conflicting writer mutates retained values.
    EXPECT_EQ(original.snapshot->revision, 1ULL);
    EXPECT_TRUE(original.snapshot->capture.scenes->at(0).label.empty());
    EXPECT_EQ(*original.snapshot->capture.scenes->at(0).generation, 1ULL);
  }
}

TEST(LegacyCaptureLedger, RouteOnlyRetirementFencesRecreationWithoutRetiringSiblings) {
  LegacyCaptureLedger ledger(config());
  auto full = capture();
  auto sibling = full.scenes->at(0).orderedRoutes[0]; sibling.id = "sibling";
  full.scenes->at(0).orderedRoutes.push_back(sibling);
  full.outputs->push_back({"recorder", {}, ShowOutputIntent::Kind::Recorder, false, false});
  const auto first = ledger.publish(full);
  ASSERT_EQ(first.status, LegacyCaptureLedger::Status::Changed);
  auto removed = full;
  removed.scenes->at(0).orderedRoutes.erase(removed.scenes->at(0).orderedRoutes.begin());
  const auto retired = ledger.publish(removed, {{LegacyCaptureLedger::Retirement::Kind::Route, "route", "scene"}});
  ASSERT_EQ(retired.status, LegacyCaptureLedger::Status::Changed);
  ASSERT_EQ(retired.snapshot->capture.scenes->at(0).orderedRoutes.size(), 1U);
  EXPECT_EQ(*retired.snapshot->capture.scenes->at(0).orderedRoutes[0].generation, 1ULL);
  const auto restored = ledger.publish(full);
  ASSERT_EQ(restored.status, LegacyCaptureLedger::Status::Changed);
  const auto& scene = restored.snapshot->capture.scenes->at(0);
  EXPECT_EQ(*scene.generation, 3ULL);
  EXPECT_EQ(*scene.orderedRoutes[0].generation, 2ULL);
  EXPECT_EQ(*scene.orderedRoutes[1].generation, 1ULL);
  EXPECT_EQ(*restored.snapshot->capture.outputs->at(0).generation, 1ULL);
  EXPECT_EQ(*first.snapshot->capture.scenes->at(0).orderedRoutes[0].generation, 1ULL);
  EXPECT_EQ(ledger.publish(full).status, LegacyCaptureLedger::Status::Unchanged);
}
