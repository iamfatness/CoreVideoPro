#include "core/ZoomSourceAuthorityAdapter.h"
#include "core/ShowStateOwner.h"
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
  a.instanceId = "provider-camera-a";
  a.publication = Adapter::Publication{1, 100, 1920, 1080, 60, 1, "I420"};
  auto b = a; b.id = "source-b"; b.externalId = "43"; b.personId = "p-b";
  b.instanceId = "provider-camera-b";
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
  o.sources[0].instanceId = "provider-camera-a-rejoined";
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
  for (auto& source : next.sources) source.incarnation = 2;
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
  for (auto& source : o.sources) source.incarnation = 2;
  ASSERT_EQ(adapter.sync(o).status, Adapter::Status::Applied);
  o.processEpoch = "epoch-c";
  for (auto& source : o.sources) source.incarnation = 3;
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

TEST(ZoomSourceAuthorityAdapter, CompleteObservationMatchesExactFixedRouteWithoutDiscoveryRoundTrip) {
  Adapter adapter("authority"); auto observation = initial();
  observation.sources[0].incarnation = 7;
  const auto& input = observation.sources[0];
  const ShowSourceRef fixed{input.id, input.instanceId, observation.processEpoch, input.incarnation};
  const auto result = adapter.sync(observation);
  ASSERT_EQ(result.status, Adapter::Status::Applied);
  const auto& token = result.snapshot->sources[0].token;
  const ShowSourceRef observed{token.sourceId.value, token.instanceId.value, token.processEpoch, token.generation};
  EXPECT_TRUE(fixed == observed);
}

TEST(ZoomSourceAuthorityAdapter, InstanceIdentityIsRequiredUniqueAndGenerationFenced) {
  Adapter adapter("authority"); auto observation = initial();
  auto bad = observation; bad.sources[0].instanceId.clear();
  EXPECT_EQ(adapter.sync(bad).status, Adapter::Status::Invalid);
  bad = observation; bad.sources[0].instanceId = std::string(513, 'x');
  EXPECT_EQ(adapter.sync(bad).status, Adapter::Status::Invalid);
  bad = observation; bad.sources[1].instanceId = bad.sources[0].instanceId;
  EXPECT_EQ(adapter.sync(bad).status, Adapter::Status::Invalid);
  EXPECT_EQ(adapter.snapshot()->revision, 0ULL);
  ASSERT_EQ(adapter.sync(observation).status, Adapter::Status::Applied);
  const auto revision = adapter.snapshot()->revision;
  ++observation.sequence; observation.sources[0].instanceId = "new-provider-instance";
  EXPECT_EQ(adapter.sync(observation).status, Adapter::Status::Stale);
  EXPECT_EQ(adapter.snapshot()->revision, revision);
  ++observation.sources[0].incarnation;
  const auto replaced = adapter.sync(observation);
  ASSERT_EQ(replaced.status, Adapter::Status::Applied);
  EXPECT_EQ(replaced.snapshot->sources[0].token.instanceId.value, "new-provider-instance");
  EXPECT_EQ(replaced.snapshot->sources[0].token.generation, 2ULL);
}

TEST(ZoomSourceAuthorityAdapter, ProviderIncarnationGapsAndEpochFencesRemainExact) {
  Adapter adapter("authority"); auto observation = initial();
  for (auto& source : observation.sources) source.incarnation = 2;
  ASSERT_EQ(adapter.sync(observation).status, Adapter::Status::Applied);
  ++observation.sequence; observation.sources[0].incarnation = 5;
  observation.sources[0].instanceId = "provider-a-five";
  const auto replaced = adapter.sync(observation);
  ASSERT_EQ(replaced.status, Adapter::Status::Applied);
  EXPECT_EQ(replaced.snapshot->sources[0].token.generation, 5ULL);
  const auto revision = replaced.snapshot->revision;
  observation.processEpoch = "epoch-new";
  EXPECT_EQ(adapter.sync(observation).status, Adapter::Status::Stale);
  EXPECT_EQ(adapter.snapshot()->revision, revision);
  observation.sources[0].incarnation = 6; observation.sources[1].incarnation = 3;
  const auto restarted = adapter.sync(observation);
  ASSERT_EQ(restarted.status, Adapter::Status::Applied);
  EXPECT_EQ(restarted.snapshot->sources[0].token.generation, 6ULL);
  EXPECT_EQ(restarted.snapshot->sources[0].token.processEpoch, "epoch-new");
}
