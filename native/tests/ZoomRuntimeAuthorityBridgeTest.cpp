#include "core/ZoomRuntimeAuthorityBridge.h"
#include <gtest/gtest.h>
#include <limits>
using namespace corevideo::core;
using Runtime = corevideo::modules::ZoomEngineRuntime;
namespace {
Runtime::AuthorityObservation observation() {
  Runtime::AuthorityObservation input;
  input.processEpoch = "helper-1"; input.processGeneration = 1; input.sequence = 4;
  Runtime::AuthoritySource camera;
  camera.participantId = 42; camera.sourceId = "camera"; camera.instanceId = "camera-instance";
  camera.generation = 1; camera.available = true; camera.subscriptionRequested = true;
  camera.publication = Runtime::AuthorityPublication{7, 123, 1920, 1080, "I420", std::nullopt};
  auto share = camera; share.sourceId = "share"; share.instanceId = "share-instance";
  share.kind = Runtime::AuthoritySource::Kind::Share; share.generation = 2;
  input.sources = {camera, share}; return input;
}
}
TEST(ZoomRuntimeAuthorityBridge, RealUnknownRatePublicationRetainsExactIdentityWithoutPeople) {
  const auto input = observation(); const auto result = ZoomRuntimeAuthorityBridge::convert(input);
  ASSERT_EQ(result.status, ZoomRuntimeAuthorityBridge::Status::Converted);
  ASSERT_TRUE(result.observation);
  EXPECT_TRUE(result.observation->people.empty());
  ZoomSourceAuthorityAdapter adapter("registry");
  const auto accepted = adapter.sync(*result.observation);
  ASSERT_EQ(accepted.status, ZoomSourceAuthorityAdapter::Status::Applied);
  ASSERT_EQ(accepted.snapshot->sources.size(), 2U);
  const auto& source = accepted.snapshot->sources[0];
  EXPECT_EQ(source.token.instanceId.value, "camera-instance");
  EXPECT_EQ(source.token.generation, 1ULL);
  EXPECT_EQ(source.token.processEpoch, "helper-1");
  EXPECT_EQ(source.externalId, "42");
  EXPECT_FALSE(source.personId);
  EXPECT_TRUE(source.hasPublication);
  EXPECT_FALSE(source.format->fpsNumerator);
  EXPECT_FALSE(source.subscriptionObserved.has_value());
  EXPECT_EQ(source.lastPublicationNs, 123);
  EXPECT_EQ(adapter.sync(*result.observation).status, ZoomSourceAuthorityAdapter::Status::Unchanged);
}
TEST(ZoomRuntimeAuthorityBridge, InvalidCompleteObservationNeverReturnsAPartialRoster) {
  for (int mutation = 0; mutation < 12; ++mutation) {
    auto input = observation();
    switch (mutation) {
      case 0: input.valid = false; break;
      case 1: input.sources[1].sourceId = input.sources[0].sourceId; break;
      case 2: input.sources[1].instanceId = input.sources[0].instanceId; break;
      case 3: input.sources[1].kind = Runtime::AuthoritySource::Kind::Camera; break;
      case 4: input.sources[1].participantId = 0; break;
      case 5: input.sources[1].generation = 0; break;
      case 6: input.sources[1].publication->fpsNumerator = 0; break;
      case 7: input.sources[1].publication->width = std::numeric_limits<uint32_t>::max(); break;
      case 8: input.sources[1].available = false; break;
      case 9: input.sources[1].instanceId.assign(513, 'x'); break;
      case 10: input.sources[1].publication->observedNs = -1; break;
      case 11: input.processEpoch.clear(); break;
    }
    const auto result = ZoomRuntimeAuthorityBridge::convert(input);
    EXPECT_EQ(result.status, ZoomRuntimeAuthorityBridge::Status::Invalid);
    EXPECT_FALSE(result.observation);
  }
  EXPECT_EQ(ZoomRuntimeAuthorityBridge::convert(observation(), 1).status, ZoomRuntimeAuthorityBridge::Status::Capacity);
  auto unsupported = observation(); unsupported.sources[0].durablePersonId = "unverified-person";
  EXPECT_EQ(ZoomRuntimeAuthorityBridge::convert(unsupported).status, ZoomRuntimeAuthorityBridge::Status::UnsupportedDurableIdentity);
}
TEST(ZoomRuntimeAuthorityBridge, HelperReplacementAndKnownRateSurviveMapping) {
  ZoomSourceAuthorityAdapter adapter("registry"); auto input = observation();
  adapter.sync(*ZoomRuntimeAuthorityBridge::convert(input).observation);
  input.processEpoch = "helper-2"; input.processGeneration = 2; ++input.sequence;
  for (auto& source : input.sources) {
    source.instanceId += "-replacement"; source.generation += 2;
    source.publication->fpsNumerator = 60; source.subscriptionObserved = true;
  }
  const auto result = adapter.sync(*ZoomRuntimeAuthorityBridge::convert(input).observation);
  ASSERT_EQ(result.status, ZoomSourceAuthorityAdapter::Status::Applied);
  EXPECT_EQ(result.snapshot->sources[0].token.processEpoch, "helper-2");
  EXPECT_EQ(result.snapshot->sources[0].format->fpsNumerator, std::optional<int>{60});
  EXPECT_EQ(result.snapshot->sources[0].subscriptionObserved, std::optional<bool>{true});
}
