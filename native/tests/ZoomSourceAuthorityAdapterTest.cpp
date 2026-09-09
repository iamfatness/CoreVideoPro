#include "core/ZoomSourceAuthorityAdapter.h"
#include <gtest/gtest.h>
#include <algorithm>
using namespace corevideo::core;
namespace {
using Adapter = ZoomSourceAuthorityAdapter;
Adapter::Observation initial() {
  Adapter::Observation o; o.processEpoch = "epoch-a"; o.sequence = 1;
  o.people = {{"p-a", "Same", 1}, {"p-b", "Same", 1}};
  Adapter::Source a; a.id = "source-a"; a.externalId = "42"; a.personId = "p-a"; a.personGeneration = 1;
  a.name = "Same"; a.subscriptionRequested = a.subscriptionObserved = true;
  a.publication = Adapter::Publication{1, 100, 1920, 1080, 60, 1, "I420"};
  auto b = a; b.id = "source-b"; b.externalId = "43"; b.personId = "p-b";
  o.sources = {a, b}; return o;
}
}
TEST(ZoomSourceAuthorityAdapter, ReorderedRosterAndSameNamesDoNotRebind) {
  Adapter adapter("authority"); auto o = initial();
  const auto first = adapter.sync(o);
  ASSERT_EQ(first.status, Adapter::Status::Applied);
  EXPECT_FALSE(first.audioCapabilityKnown);
  std::reverse(o.people.begin(), o.people.end()); std::reverse(o.sources.begin(), o.sources.end());
  EXPECT_EQ(adapter.sync(o).status, Adapter::Status::Unchanged);
  ++o.sequence;
  const auto next = adapter.sync(o);
  EXPECT_EQ(next.snapshot->revision, first.snapshot->revision);
  EXPECT_EQ(next.snapshot->sources[0].personId->value, "p-a");
  EXPECT_EQ(next.snapshot->sources[1].personId->value, "p-b");
  EXPECT_EQ(next.snapshot->sources[0].token.instanceId.value, first.snapshot->sources[0].token.instanceId.value);
}
TEST(ZoomSourceAuthorityAdapter, LeaveRejoinRequiresNewIncarnationAndRejectsOldRoster) {
  Adapter adapter("authority"); auto o = initial();
  const auto first = adapter.sync(o);
  auto absent = o; absent.sequence = 2; absent.sources.erase(absent.sources.begin());
  EXPECT_EQ(adapter.sync(absent).status, Adapter::Status::Applied);
  o.sequence = 3;
  EXPECT_EQ(adapter.sync(o).status, Adapter::Status::Stale);
  o.sources[0].incarnation = 2;
  const auto rejoined = adapter.sync(o);
  ASSERT_EQ(rejoined.status, Adapter::Status::Applied);
  EXPECT_EQ(rejoined.snapshot->sources[0].token.generation, 2ULL);
  EXPECT_NE(rejoined.snapshot->sources[0].token.instanceId.value, first.snapshot->sources[0].token.instanceId.value);
  EXPECT_EQ(adapter.sync(absent).status, Adapter::Status::Stale);
}
TEST(ZoomSourceAuthorityAdapter, CameraOffReturnKeepsIdentityAndPublicationWatermark) {
  Adapter adapter("authority"); auto o = initial();
  const auto first = adapter.sync(o);
  ++o.sequence; o.sources[0].videoAvailable = false; o.sources[0].subscriptionObserved = false; o.sources[0].publication.reset();
  ASSERT_EQ(adapter.sync(o).status, Adapter::Status::Applied);
  EXPECT_FALSE(adapter.snapshot()->sources[0].hasPublication);
  ++o.sequence; o.sources[0].videoAvailable = true;
  ASSERT_EQ(adapter.sync(o).status, Adapter::Status::Applied);
  EXPECT_FALSE(adapter.snapshot()->sources[0].hasPublication);
  ++o.sequence; o.sources[0].videoAvailable = true; o.sources[0].subscriptionObserved = true;
  o.sources[0].publication = Adapter::Publication{2, 200, 1920, 1080, 60, 1, "I420"};
  const auto returned = adapter.sync(o);
  ASSERT_EQ(returned.status, Adapter::Status::Applied);
  EXPECT_EQ(returned.snapshot->sources[0].token.instanceId.value, first.snapshot->sources[0].token.instanceId.value);
  EXPECT_EQ(returned.snapshot->sources[0].publicationSequence, 2ULL);
  ++o.sequence; o.sources[0].publication->sequence = 1;
  EXPECT_EQ(adapter.sync(o).status, Adapter::Status::Stale);
}
TEST(ZoomSourceAuthorityAdapter, EpochReplacementFencesLateCallbacksAndReusesStableId) {
  Adapter adapter("authority"); auto o = initial(); adapter.sync(o);
  auto next = o; next.processEpoch = "epoch-b"; next.sequence = 0;
  const auto result = adapter.sync(next);
  ASSERT_EQ(result.status, Adapter::Status::Applied);
  EXPECT_EQ(result.snapshot->sources[0].token.sourceId.value, "source-a");
  EXPECT_EQ(result.snapshot->sources[0].token.generation, 2ULL);
  EXPECT_EQ(result.snapshot->sources[0].token.processEpoch, "epoch-b");
  o.sequence = 999;
  EXPECT_EQ(adapter.sync(o).status, Adapter::Status::Stale);
}
TEST(ZoomSourceAuthorityAdapter, InvalidDuplicatesAndCapacityFailBeforeMutation) {
  Adapter adapter("authority", 2, 2, 1); auto o = initial();
  auto duplicate = o; duplicate.sources[1].externalId = duplicate.sources[0].externalId;
  EXPECT_EQ(adapter.sync(duplicate).status, Adapter::Status::Invalid);
  EXPECT_EQ(adapter.snapshot()->revision, 0ULL);
  ASSERT_EQ(adapter.sync(o).status, Adapter::Status::Applied);
  const auto revision = adapter.snapshot()->revision;
  ++o.sequence; o.people.push_back({"third", "Same", 1});
  EXPECT_EQ(adapter.sync(o).status, Adapter::Status::Capacity);
  EXPECT_EQ(adapter.snapshot()->revision, revision);
  o = initial(); o.processEpoch = "epoch-b";
  ASSERT_EQ(adapter.sync(o).status, Adapter::Status::Applied);
  o.processEpoch = "epoch-c";
  EXPECT_EQ(adapter.sync(o).status, Adapter::Status::Capacity);
}
TEST(ZoomSourceAuthorityAdapter, SimultaneousExternalHandleSwapIsOrderIndependent) {
  Adapter adapter("authority"); auto o = initial(); adapter.sync(o);
  ++o.sequence; std::swap(o.sources[0].externalId, o.sources[1].externalId);
  o.sources[0].incarnation = o.sources[1].incarnation = 2;
  const auto result = adapter.sync(o);
  ASSERT_EQ(result.status, Adapter::Status::Applied);
  EXPECT_EQ(result.snapshot->sources[0].externalId, "43");
  EXPECT_EQ(result.snapshot->sources[1].externalId, "42");
}

TEST(ZoomSourceAuthorityAdapter, RenameUpdatesMetadataWithoutReplacingSource) {
  Adapter adapter("authority"); auto observation = initial();
  const auto before = adapter.sync(observation);
  ++observation.sequence; observation.sources[0].name = "Renamed";
  const auto changed = adapter.sync(observation);
  ASSERT_EQ(changed.status, Adapter::Status::Applied);
  EXPECT_EQ(changed.snapshot->sources[0].displayName, "Renamed");
  EXPECT_EQ(changed.snapshot->sources[0].token.instanceId.value, before.snapshot->sources[0].token.instanceId.value);
  EXPECT_EQ(changed.snapshot->sources[0].token.generation, before.snapshot->sources[0].token.generation);
  EXPECT_EQ(changed.snapshot->sources[0].publicationSequence, before.snapshot->sources[0].publicationSequence);
  EXPECT_TRUE(changed.snapshot->sources[0].subscriptionObserved);
  EXPECT_EQ(changed.snapshot->revision, before.snapshot->revision + 1);
  ++observation.sequence;
  EXPECT_EQ(adapter.sync(observation).status, Adapter::Status::Unchanged);
}
