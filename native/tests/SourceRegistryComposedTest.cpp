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
}  // namespace
