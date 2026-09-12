#include "core/SourceRegistry.h"

#include <gtest/gtest.h>

namespace {
using corevideo::core::SourceRegistry;

SourceRegistry::Registration wallRegistration(const std::string& id) {
  SourceRegistry::Registration registration;
  registration.sourceId = {id};
  registration.kind = SourceRegistry::Kind::Composed;
  registration.displayName = "Gallery";
  // A wall lives and dies with the core process, so the core's epoch is its epoch.
  registration.processEpoch = "core-epoch-1";
  // externalId deliberately left EMPTY: a wall has no SDK handle.
  return registration;
}

// A wall has no SDK handle, and validRegistration rejects an empty externalId
// today, so add() answers Invalid and the wall can never be registered.
TEST(SourceRegistryComposed, AWallIsAdmittedWithoutAnExternalId) {
  SourceRegistry registry("registry-epoch-1");
  const auto mutation = registry.add(wallRegistration("tiles:scene-a"));
  EXPECT_EQ(mutation.result, SourceRegistry::Result::Applied);
  ASSERT_TRUE(mutation.token.has_value());
  EXPECT_EQ(mutation.token->sourceId.value, "tiles:scene-a");
}

// externalConflict matches on kind + processEpoch + externalId, so two walls
// that both have an EMPTY externalId look like duplicates of each other.
TEST(SourceRegistryComposed, TwoWallsWithNoExternalIdDoNotCollide) {
  SourceRegistry registry("registry-epoch-1");
  ASSERT_EQ(registry.add(wallRegistration("tiles:scene-a")).result,
            SourceRegistry::Result::Applied);
  const auto second = registry.add(wallRegistration("tiles:scene-b"));
  EXPECT_EQ(second.result, SourceRegistry::Result::Applied);
}

// The load-bearing honesty test. subscriptionObserved is initialised ENGAGED
// with the value false, and its own comment says nullopt means unknown - so a
// registered wall would otherwise ASSERT "subscription observed = false" into
// ShowPlanGenerator, which consumes this snapshot.
TEST(SourceRegistryComposed, AComposedWallNeverClaimsASubscriptionState) {
  SourceRegistry registry("registry-epoch-1");
  ASSERT_EQ(registry.add(wallRegistration("tiles:scene-a")).result,
            SourceRegistry::Result::Applied);

  const auto snapshot = registry.snapshot();
  ASSERT_NE(snapshot, nullptr);
  ASSERT_EQ(snapshot->sources.size(), 1U);
  const auto& wall = snapshot->sources.front();

  EXPECT_EQ(wall.kind, SourceRegistry::Kind::Composed);
  EXPECT_FALSE(wall.personId.has_value());
  EXPECT_FALSE(wall.externalId.has_value());
  EXPECT_FALSE(wall.availability.has_value());
  EXPECT_FALSE(wall.subscriptionRequested.has_value());
  EXPECT_FALSE(wall.subscriptionObserved.has_value());
}

// A capture source is unchanged: it still carries every field it always did.
TEST(SourceRegistryComposed, ACaptureSourceStillCarriesItsCaptureFields) {
  SourceRegistry registry("registry-epoch-1");
  SourceRegistry::Registration camera;
  camera.sourceId = {"camera-alice"};
  camera.kind = SourceRegistry::Kind::ParticipantVideo;
  camera.displayName = "Alice";
  camera.processEpoch = "zoom-process-1";
  camera.externalId = "alice-sdk-id";
  ASSERT_EQ(registry.add(camera).result, SourceRegistry::Result::Applied);

  // Bind the shared_ptr before taking a reference into it: `snapshot()->sources.front()`
  // as one expression leaves `source` dangling the instant the temporary shared_ptr's
  // refcount drops to zero at the semicolon.
  const auto snapshot = registry.snapshot();
  const auto& source = snapshot->sources.front();
  ASSERT_TRUE(source.externalId.has_value());
  EXPECT_EQ(*source.externalId, "alice-sdk-id");
  ASSERT_TRUE(source.availability.has_value());
  EXPECT_EQ(*source.availability, SourceRegistry::Availability::Available);
}

// Fix round 1, finding 1: retireProcessEpoch iterates every source under the
// retired epoch unconditionally (it exists to fence a dead PROVIDER PROCESS's
// late callbacks) - a wall has no provider process and no callbacks to fence,
// so it must be SKIPPED, not marked Departed. Marking it would flip its
// nullopt fields to concrete Departed/false, the exact false claim install()
// was fixed to stop making.
TEST(SourceRegistryComposed, RetiringItsEpochLeavesAWallsFieldsUntouched) {
  SourceRegistry registry("registry-epoch-1");
  ASSERT_EQ(registry.add(wallRegistration("tiles:scene-a")).result,
            SourceRegistry::Result::Applied);

  EXPECT_EQ(registry.retireProcessEpoch("core-epoch-1"), SourceRegistry::Result::Applied);

  const auto snapshot = registry.snapshot();
  ASSERT_EQ(snapshot->sources.size(), 1U);
  const auto& wall = snapshot->sources.front();
  EXPECT_FALSE(wall.availability.has_value());
  EXPECT_FALSE(wall.subscriptionRequested.has_value());
  EXPECT_FALSE(wall.subscriptionObserved.has_value());
}

// Fix round 1, finding 1 (second path): setAvailability has the identical
// unconditional `subscriptionObserved = false` on any non-Available
// transition. A composed source has no availability concept at all, so this
// call does not apply to it and must be refused rather than silently
// asserting a subscription state.
TEST(SourceRegistryComposed, SetAvailabilityOnAWallLeavesSubscriptionObservedUnclaimed) {
  SourceRegistry registry("registry-epoch-1");
  const auto added = registry.add(wallRegistration("tiles:scene-a"));
  ASSERT_EQ(added.result, SourceRegistry::Result::Applied);
  ASSERT_TRUE(added.token.has_value());

  EXPECT_EQ(registry.setAvailability(*added.token, SourceRegistry::Availability::Unavailable),
            SourceRegistry::Result::Invalid);

  const auto snapshot = registry.snapshot();
  ASSERT_EQ(snapshot->sources.size(), 1U);
  EXPECT_FALSE(snapshot->sources.front().subscriptionObserved.has_value());
}

// Fix round 1, finding 2: externalConflict's composed bypass must skip only
// the externalId clause. The instanceId collision check is identity, not an
// SDK-handle concept, and still applies to a composed registration.
TEST(SourceRegistryComposed, TwoWallsSharingAnInstanceIdStillConflict) {
  SourceRegistry registry("registry-epoch-1");
  auto first = wallRegistration("tiles:scene-a");
  first.instanceId = corevideo::core::SourceInstanceId{"shared-wall-instance"};
  ASSERT_EQ(registry.add(first).result, SourceRegistry::Result::Applied);

  auto second = wallRegistration("tiles:scene-b");
  second.instanceId = corevideo::core::SourceInstanceId{"shared-wall-instance"};
  EXPECT_EQ(registry.add(second).result, SourceRegistry::Result::Conflict);
}
}  // namespace
