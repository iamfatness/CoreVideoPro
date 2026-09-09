#include "core/SourceRegistry.h"
#include <gtest/gtest.h>
#include <atomic>
#include <limits>
#include <thread>

namespace {
using Registry = corevideo::core::SourceRegistry;
Registry::Registration participant(const std::string& id, const std::string& handle = "42") {
  Registry::Registration result;
  result.sourceId = {id};
  result.processEpoch = "meeting-1";
  result.externalId = handle;
  result.displayName = "Alex";
  return result;
}
Registry::Format format() { return {1920, 1080, 60, 1, "I420"}; }
}

TEST(SourceRegistry, DuplicateNamesDoNotBindOrMergePeopleAndSources) {
  Registry registry("authority");
  registry.upsertPerson({{"person-b"}, "Alex"});
  registry.upsertPerson({{"person-a"}, "Alex"});
  const auto matches = registry.peopleNamed("Alex");
  ASSERT_EQ(matches.size(), 2u);
  EXPECT_EQ(matches[0].value, "person-a");
  EXPECT_EQ(matches[1].value, "person-b");
  EXPECT_TRUE(registry.peopleNamed("missing").empty());
  EXPECT_EQ(registry.add(participant("source-a")).result, Registry::Result::Applied);
  auto bound = participant("source-b", "43");
  bound.personId = corevideo::core::PersonId{"person-b"};
  bound.personGeneration = 1;
  EXPECT_EQ(registry.add(bound).result, Registry::Result::Applied);
  const auto snapshot = registry.snapshot();
  ASSERT_EQ(snapshot->sources.size(), 2u);
  EXPECT_FALSE(snapshot->sources[0].personId.has_value());
  ASSERT_TRUE(snapshot->sources[1].personId.has_value());
  EXPECT_EQ(snapshot->sources[1].personId->value, "person-b");
  bound.sourceId = {"source-c"};
  bound.externalId = "44";
  bound.personId = corevideo::core::PersonId{"unknown-person"};
  bound.personGeneration = 1;
  EXPECT_EQ(registry.add(bound).result, Registry::Result::Invalid);
}

TEST(SourceRegistry, ReconnectResetsTransientStateAndRejectsEveryOldMutation) {
  Registry registry("authority");
  registry.upsertPerson({{"durable-person"}, "Alex"});
  auto registration = participant("camera");
  registration.personId = corevideo::core::PersonId{"durable-person"};
  registration.personGeneration = 1;
  const auto original = registry.add(registration);
  ASSERT_TRUE(original.token.has_value());
  registry.setSubscription(*original.token, true, true);
  registry.publish(*original.token, 100, 1000, format());
  const auto before = registry.snapshot();
  registration.processEpoch = "meeting-2";
  const auto replacement = registry.replace(*original.token, registration);
  ASSERT_TRUE(replacement.token.has_value());
  EXPECT_EQ(replacement.token->generation, 2u);
  EXPECT_TRUE(replacement.token->instanceId.value != original.token->instanceId.value);
  EXPECT_EQ(registry.publish(*original.token, 101, 1100, format()), Registry::Result::Stale);
  EXPECT_EQ(registry.setAvailability(*original.token, Registry::Availability::Departed), Registry::Result::Stale);
  EXPECT_EQ(registry.setSubscription(*original.token, false, false), Registry::Result::Stale);
  EXPECT_EQ(registry.replace(*original.token, registration).result, Registry::Result::Stale);
  const auto after = registry.snapshot();
  EXPECT_TRUE(before->sources[0].hasPublication); // Old snapshot cannot change.
  EXPECT_FALSE(after->sources[0].hasPublication);
  EXPECT_FALSE(after->sources[0].format.has_value());
  EXPECT_FALSE(after->sources[0].subscriptionObserved);
  EXPECT_EQ(after->sources[0].personId->value, "durable-person");
  EXPECT_EQ(after->sources[0].personGeneration, 1ULL);
  EXPECT_EQ(registry.publish(*replacement.token, 0, 1, format()), Registry::Result::Applied);
}

TEST(SourceRegistry, DepartureRequiresExplicitReplacementAndScopesReusedHandles) {
  Registry registry("authority");
  const auto first = registry.add(participant("old-source"));
  ASSERT_TRUE(first.token.has_value());
  EXPECT_EQ(registry.add(participant("ambiguous-source")).result, Registry::Result::Conflict);
  EXPECT_EQ(registry.setAvailability(*first.token, Registry::Availability::Departed), Registry::Result::Applied);
  EXPECT_EQ(registry.setAvailability(*first.token, Registry::Availability::Available), Registry::Result::Stale);
  EXPECT_EQ(registry.publish(*first.token, 1, 100, format()), Registry::Result::Stale);
  EXPECT_EQ(registry.add(participant("old-source")).result, Registry::Result::Conflict);
  EXPECT_EQ(registry.add(participant("new-person-source")).result, Registry::Result::Applied);
  EXPECT_EQ(registry.replace(*first.token, participant("old-source")).result, Registry::Result::Conflict);
  auto nextMeeting = participant("old-source");
  nextMeeting.processEpoch = "meeting-2";
  EXPECT_EQ(registry.replace(*first.token, nextMeeting).result, Registry::Result::Applied);
}

TEST(SourceRegistry, DeviceReplacementAndCameraShareAreSeparateInstances) {
  Registry registry("authority");
  auto device = participant("device-slot");
  device.kind = Registry::Kind::Device;
  device.externalId = "device-path-a";
  const auto first = registry.add(device);
  ASSERT_TRUE(first.token.has_value());
  device.externalId = "device-path-b";
  const auto next = registry.replace(*first.token, device);
  ASSERT_TRUE(next.token.has_value());
  EXPECT_EQ(registry.publish(*first.token, 1, 1, format()), Registry::Result::Stale);
  EXPECT_EQ(registry.snapshot()->sources[0].externalId, "device-path-b");
  EXPECT_EQ(registry.add(participant("camera")).result, Registry::Result::Applied);
  auto share = participant("share");
  share.kind = Registry::Kind::ParticipantShare;
  EXPECT_EQ(registry.add(share).result, Registry::Result::Applied);
  auto forged = *next.token;
  forged.processEpoch = "old-process";
  EXPECT_EQ(registry.publish(forged, 1, 1, format()), Registry::Result::Stale);
}

TEST(SourceRegistry, OutOfOrderPublicationCannotRefreshOrChangeFormat) {
  Registry registry("authority");
  const auto added = registry.add(participant("camera"));
  ASSERT_TRUE(added.token.has_value());
  EXPECT_EQ(registry.publish(*added.token, 10, 100, format()), Registry::Result::Applied);
  auto changed = format(); changed.width = 640;
  EXPECT_EQ(registry.publish(*added.token, 10, 200, changed), Registry::Result::Stale);
  EXPECT_EQ(registry.publish(*added.token, 11, 99, changed), Registry::Result::Stale);
  EXPECT_EQ(registry.snapshot()->sources[0].format->width, 1920);
  EXPECT_EQ(registry.snapshot()->sources[0].lastPublicationNs, 100);
  registry.setAvailability(*added.token, Registry::Availability::Unavailable);
  EXPECT_FALSE(registry.snapshot()->sources[0].hasPublication);
  EXPECT_FALSE(registry.snapshot()->sources[0].format.has_value());
  EXPECT_EQ(registry.publish(*added.token, 12, 200, changed), Registry::Result::Stale);
  EXPECT_EQ(registry.setSubscription(*added.token, true, true), Registry::Result::Invalid);
}

TEST(SourceRegistry, ConcurrentReplacementHasExactlyOneWinnerAndFencesOldPublisher) {
  Registry registry("authority");
  const auto first = registry.add(participant("camera"));
  ASSERT_TRUE(first.token.has_value());
  std::atomic<int> winners{0};
  const auto replace = [&] {
    if (registry.replace(*first.token, participant("camera")).result == Registry::Result::Applied) ++winners;
  };
  std::thread a(replace), b(replace);
  a.join(); b.join();
  EXPECT_EQ(winners.load(), 1);
  std::atomic<int> accepted{0};
  std::thread stale([&] {
    for (uint64_t i = 1; i < 100; ++i)
      if (registry.publish(*first.token, i, static_cast<int64_t>(i), format()) == Registry::Result::Applied) ++accepted;
  });
  for (int i = 0; i < 100; ++i) {
    const auto snapshot = registry.snapshot();
    EXPECT_EQ(snapshot->sources[0].token.generation, 2u);
    EXPECT_FALSE(snapshot->sources[0].hasPublication);
  }
  stale.join();
  EXPECT_EQ(accepted.load(), 0);
}

TEST(SourceRegistry, ReplacedProcessEpochRetiresAllSourcesAndFencesEveryCallback) {
  Registry registry("authority");
  const auto camera = registry.add(participant("camera", "42"));
  auto shareRegistration = participant("share", "42");
  shareRegistration.kind = Registry::Kind::ParticipantShare;
  const auto share = registry.add(shareRegistration);
  ASSERT_TRUE(camera.token.has_value());
  ASSERT_TRUE(share.token.has_value());
  EXPECT_EQ(registry.setSubscription(*camera.token, true, true), Registry::Result::Applied);
  const auto before = registry.snapshot()->revision;
  EXPECT_EQ(registry.retireProcessEpoch("meeting-1"), Registry::Result::Applied);
  const auto retired = registry.snapshot();
  EXPECT_EQ(retired->revision, before + 1);
  ASSERT_EQ(retired->sources.size(), 2u);
  for (const auto& source : retired->sources) {
    EXPECT_EQ(source.availability, Registry::Availability::Departed);
    EXPECT_FALSE(source.subscriptionRequested);
    EXPECT_FALSE(source.subscriptionObserved);
  }
  EXPECT_EQ(registry.publish(*camera.token, 1, 1, format()), Registry::Result::Stale);
  EXPECT_EQ(registry.setSubscription(*share.token, true, true), Registry::Result::Stale);
  EXPECT_EQ(registry.retireProcessEpoch("meeting-1"), Registry::Result::Unchanged);
  EXPECT_EQ(registry.add(participant("late-source", "99")).result, Registry::Result::Invalid);
  EXPECT_EQ(registry.replace(*camera.token, participant("camera", "77")).result, Registry::Result::Invalid);

  auto current = participant("camera", "77");
  current.processEpoch = "meeting-2";
  EXPECT_EQ(registry.replace(*camera.token, current).result, Registry::Result::Applied);

  EXPECT_EQ(registry.retireProcessEpoch("future-meeting"), Registry::Result::Applied);
  auto delayed = participant("delayed", "88");
  delayed.processEpoch = "future-meeting";
  EXPECT_EQ(registry.add(delayed).result, Registry::Result::Invalid);
}

TEST(SourceRegistry, SemanticNoOpsDoNotAdvanceRegistryRevision) {
  Registry registry("authority");
  EXPECT_EQ(registry.upsertPerson({{"person-a"}, "Alex"}), Registry::Result::Applied);
  const auto added = registry.add(participant("camera"));
  ASSERT_TRUE(added.token.has_value());
  const auto revision = registry.snapshot()->revision;
  EXPECT_EQ(registry.upsertPerson({{"person-a"}, "Alex"}), Registry::Result::Unchanged);
  EXPECT_EQ(registry.setAvailability(*added.token, Registry::Availability::Available), Registry::Result::Unchanged);
  EXPECT_EQ(registry.setSubscription(*added.token, false, false), Registry::Result::Unchanged);
  EXPECT_EQ(registry.snapshot()->revision, revision);
}

TEST(SourceRegistry, PersonGenerationIsExplicitAndCannotMoveBackward) {
  Registry registry("authority");
  EXPECT_EQ(registry.upsertPerson({{"person-a"}, "Alex", 2}), Registry::Result::Applied);
  EXPECT_EQ(registry.upsertPerson({{"person-a"}, "Renamed", 1}), Registry::Result::Stale);
  EXPECT_EQ(registry.snapshot()->persons[0].displayName, "Alex");
  EXPECT_EQ(registry.upsertPerson({{"person-a"}, "Renamed", 2}), Registry::Result::Applied);
  EXPECT_EQ(registry.snapshot()->persons[0].generation, 2ULL);
}

TEST(SourceRegistry, PublicationTimestampSupportsLongRunningMonotonicClocks) {
  Registry registry("authority");
  const auto added = registry.add(participant("camera"));
  ASSERT_TRUE(added.token.has_value());
  constexpr int64_t afterOneHundredFourDays = 9'007'199'254'740'993LL;
  EXPECT_EQ(registry.publish(*added.token, 1, afterOneHundredFourDays, format()),
            Registry::Result::Applied);
  EXPECT_EQ(registry.snapshot()->sources[0].lastPublicationNs, afterOneHundredFourDays);
  EXPECT_EQ(registry.publish(*added.token, 2, (std::numeric_limits<int64_t>::max)(), format()),
            Registry::Result::Applied);
  EXPECT_EQ(registry.snapshot()->sources[0].lastPublicationNs,
            (std::numeric_limits<int64_t>::max)());
  EXPECT_EQ(registry.publish(*added.token, 2, -1, format()), Registry::Result::Invalid);
}

TEST(SourceRegistry, ProcessRetirementSerializesAgainstAddAndReplace) {
  for (int iteration = 0; iteration < 100; ++iteration) {
    Registry addRegistry("authority-add");
    std::thread add([&] { addRegistry.add(participant("camera")); });
    std::thread retire([&] { addRegistry.retireProcessEpoch("meeting-1"); });
    add.join();
    retire.join();
    const auto added = addRegistry.snapshot();
    for (const auto& source : added->sources)
      EXPECT_NE(source.availability, Registry::Availability::Available);

    Registry replaceRegistry("authority-replace");
    const auto original = replaceRegistry.add(participant("camera"));
    ASSERT_TRUE(original.token.has_value());
    std::thread replace([&] { replaceRegistry.replace(*original.token, participant("camera", "99")); });
    std::thread retireReplacement([&] { replaceRegistry.retireProcessEpoch("meeting-1"); });
    replace.join();
    retireReplacement.join();
    const auto replaced = replaceRegistry.snapshot();
    ASSERT_EQ(replaced->sources.size(), 1u);
    EXPECT_EQ(replaced->sources[0].availability, Registry::Availability::Departed);
  }
}

TEST(SourceRegistry, IdentityAndEpochTombstoneAdmissionIsBounded) {
  Registry registry("authority", 1, 1, 1);
  EXPECT_EQ(registry.upsertPerson({{"person-a"}, "A"}), Registry::Result::Applied);
  EXPECT_EQ(registry.upsertPerson({{"person-b"}, "B"}), Registry::Result::Exhausted);
  EXPECT_EQ(registry.add(participant("camera-a", "1")).result, Registry::Result::Applied);
  auto second = participant("camera-b", "2");
  second.processEpoch = "meeting-2";
  EXPECT_EQ(registry.add(second).result, Registry::Result::Exhausted);
  EXPECT_EQ(registry.retireProcessEpoch("meeting-1"), Registry::Result::Applied);
  EXPECT_EQ(registry.retireProcessEpoch("meeting-2"), Registry::Result::Exhausted);
  EXPECT_EQ(registry.snapshot()->sources[0].availability, Registry::Availability::Departed);
}

TEST(SourceRegistry, CameraReturnClearsReadinessButPreservesPublicationWatermarks) {
  Registry registry("authority");
  const auto added = registry.add(participant("camera"));
  ASSERT_TRUE(added.token.has_value());
  const auto token = *added.token;
  EXPECT_EQ(registry.publish(token, 10, 100, format()), Registry::Result::Applied);
  EXPECT_EQ(registry.setSubscription(token, true, true), Registry::Result::Applied);
  EXPECT_EQ(registry.setAvailability(token, Registry::Availability::Unavailable), Registry::Result::Applied);
  const auto unavailable = registry.snapshot();
  EXPECT_FALSE(unavailable->sources[0].hasPublication);
  EXPECT_FALSE(unavailable->sources[0].format.has_value());
  EXPECT_FALSE(unavailable->sources[0].subscriptionObserved);
  EXPECT_EQ(unavailable->sources[0].publicationSequence, 10ULL);
  EXPECT_EQ(unavailable->sources[0].lastPublicationNs, 100);
  EXPECT_EQ(registry.setAvailability(token, Registry::Availability::Available), Registry::Result::Applied);
  const auto revision = registry.snapshot()->revision;
  EXPECT_EQ(registry.publish(token, 9, 101, format()), Registry::Result::Stale);
  EXPECT_EQ(registry.publish(token, 10, 101, format()), Registry::Result::Stale);
  EXPECT_EQ(registry.publish(token, 11, 99, format()), Registry::Result::Stale);
  EXPECT_EQ(registry.snapshot()->revision, revision);
  EXPECT_FALSE(registry.snapshot()->sources[0].hasPublication);
  EXPECT_EQ(registry.publish(token, 11, 100, format()), Registry::Result::Applied);
  EXPECT_TRUE(registry.snapshot()->sources[0].hasPublication);
  EXPECT_EQ(registry.setAvailability(token, Registry::Availability::Departed), Registry::Result::Applied);
  EXPECT_EQ(registry.snapshot()->sources[0].publicationSequence, 11ULL);
  EXPECT_FALSE(registry.snapshot()->sources[0].hasPublication);
}

TEST(SourceRegistry, ZeroPublicationIsAcceptedOnceEvenAcrossCameraAvailabilityChanges) {
  Registry registry("authority");
  const auto added = registry.add(participant("camera"));
  ASSERT_TRUE(added.token.has_value());
  const auto token = *added.token;
  registry.setAvailability(token, Registry::Availability::Unavailable);
  registry.setAvailability(token, Registry::Availability::Available);
  EXPECT_EQ(registry.publish(token, 0, 0, format()), Registry::Result::Applied);
  registry.setAvailability(token, Registry::Availability::Unavailable);
  registry.setAvailability(token, Registry::Availability::Available);
  EXPECT_EQ(registry.publish(token, 0, 0, format()), Registry::Result::Stale);
  EXPECT_EQ(registry.publish(token, 1, 0, format()), Registry::Result::Applied);
  const auto replacement = registry.replace(token, participant("camera"));
  ASSERT_TRUE(replacement.token.has_value());
  EXPECT_EQ(registry.publish(*replacement.token, 0, 0, format()), Registry::Result::Applied);
}

TEST(SourceRegistry, ExistingPersonUpdatesRemainAdmissibleAtCapacity) {
  Registry registry("authority", 1, 1, 1);
  EXPECT_EQ(registry.upsertPerson({{"person-a"}, "A", 1}), Registry::Result::Applied);
  const auto initial = registry.snapshot();
  EXPECT_EQ(registry.upsertPerson({{"person-a"}, "Renamed", 1}), Registry::Result::Applied);
  EXPECT_EQ(registry.upsertPerson({{"person-a"}, "Renamed", 2}), Registry::Result::Applied);
  const auto updated = registry.snapshot();
  EXPECT_EQ(updated->revision, initial->revision + 2);
  ASSERT_EQ(updated->persons.size(), 1u);
  EXPECT_EQ(updated->persons[0].displayName, "Renamed");
  EXPECT_EQ(updated->persons[0].generation, 2ULL);
  EXPECT_EQ(registry.upsertPerson({{"person-a"}, "Renamed", 2}), Registry::Result::Unchanged);
  EXPECT_EQ(registry.upsertPerson({{"person-a"}, "Old", 1}), Registry::Result::Stale);
  EXPECT_EQ(registry.upsertPerson({{"person-b"}, "B", 1}), Registry::Result::Exhausted);
  EXPECT_EQ(registry.snapshot()->revision, updated->revision);
}

TEST(SourceRegistry, DisplayNameMetadataPreservesIncarnationAndFencesOldTokens) {
  Registry registry("authority");
  const auto added = registry.add(participant("camera"));
  ASSERT_TRUE(added.token.has_value());
  registry.setSubscription(*added.token, true, true);
  registry.publish(*added.token, 5, 100, format());
  const auto before = registry.snapshot();
  EXPECT_EQ(registry.setDisplayName(*added.token, "New name"), Registry::Result::Applied);
  const auto renamed = registry.snapshot();
  EXPECT_EQ(renamed->revision, before->revision + 1);
  EXPECT_EQ(renamed->sources[0].token.instanceId.value, before->sources[0].token.instanceId.value);
  EXPECT_EQ(renamed->sources[0].token.generation, before->sources[0].token.generation);
  EXPECT_EQ(renamed->sources[0].publicationSequence, 5ULL);
  EXPECT_TRUE(renamed->sources[0].subscriptionObserved);
  EXPECT_EQ(before->sources[0].displayName, "Alex");
  EXPECT_EQ(registry.setDisplayName(*added.token, "New name"), Registry::Result::Unchanged);
  EXPECT_EQ(registry.snapshot()->revision, renamed->revision);
  EXPECT_EQ(registry.setDisplayName(*added.token, std::string(4097, 'x')), Registry::Result::Invalid);
  EXPECT_EQ(registry.setDisplayName(*added.token, ""), Registry::Result::Applied);
  const auto replacement = registry.replace(*added.token, participant("camera"));
  ASSERT_TRUE(replacement.token.has_value());
  const auto replacedRevision = registry.snapshot()->revision;
  EXPECT_EQ(registry.setDisplayName(*added.token, "Stale name"), Registry::Result::Stale);
  EXPECT_EQ(registry.snapshot()->revision, replacedRevision);
  EXPECT_EQ(registry.snapshot()->sources[0].displayName, "Alex");
  registry.retireProcessEpoch("meeting-1");
  EXPECT_EQ(registry.setDisplayName(*replacement.token, "Late name"), Registry::Result::Stale);
}

TEST(SourceRegistry, ProviderInstanceIdsAreExactBoundedAndUniqueWhileLive) {
  Registry registry("authority");
  auto a = participant("a", "1");
  a.instanceId = corevideo::core::SourceInstanceId{"provider-instance"};
  const auto first = registry.add(a);
  ASSERT_TRUE(first.token.has_value());
  EXPECT_EQ(first.token->instanceId.value, "provider-instance");
  auto b = participant("b", "2"); b.instanceId = a.instanceId;
  const auto revision = registry.snapshot()->revision;
  EXPECT_EQ(registry.add(b).result, Registry::Result::Conflict);
  b.instanceId = corevideo::core::SourceInstanceId{""};
  EXPECT_EQ(registry.add(b).result, Registry::Result::Invalid);
  b.instanceId = corevideo::core::SourceInstanceId{std::string(513, 'x')};
  EXPECT_EQ(registry.add(b).result, Registry::Result::Invalid);
  EXPECT_EQ(registry.snapshot()->revision, revision);
  b.instanceId = corevideo::core::SourceInstanceId{std::string(512, 'x')};
  const auto second = registry.add(b);
  ASSERT_TRUE(second.token.has_value());
  b.instanceId = a.instanceId;
  EXPECT_EQ(registry.replace(*second.token, b).result, Registry::Result::Conflict);
  registry.setAvailability(*first.token, Registry::Availability::Unavailable);
  EXPECT_EQ(registry.replace(*second.token, b).result, Registry::Result::Conflict);
  registry.setAvailability(*first.token, Registry::Availability::Departed);
  EXPECT_EQ(registry.replace(*second.token, b).result, Registry::Result::Applied);
}

TEST(SourceRegistry, RegistryAllocatedInstanceCannotCollideWithProviderInstance) {
  Registry registry("authority");
  auto a = participant("a", "1");
  a.instanceId = corevideo::core::SourceInstanceId{"authority:2"};
  ASSERT_EQ(registry.add(a).result, Registry::Result::Applied);
  EXPECT_EQ(registry.add(participant("b", "2")).result, Registry::Result::Conflict);
}

TEST(SourceRegistry, ProviderGenerationAcceptsInitialAndForwardGapsButNeverRollsBack) {
  Registry registry("authority"); auto registration = participant("camera");
  registration.requestedGeneration = 7;
  const auto first = registry.add(registration);
  ASSERT_TRUE(first.token.has_value());
  EXPECT_EQ(first.token->generation, 7ULL);
  registration.requestedGeneration = 7;
  EXPECT_EQ(registry.replace(*first.token, registration).result, Registry::Result::Stale);
  registration.requestedGeneration = 6;
  EXPECT_EQ(registry.replace(*first.token, registration).result, Registry::Result::Stale);
  registration.requestedGeneration = 0;
  EXPECT_EQ(registry.replace(*first.token, registration).result, Registry::Result::Invalid);
  registration.requestedGeneration = 9007199254740992ULL;
  EXPECT_EQ(registry.replace(*first.token, registration).result, Registry::Result::Invalid);
  registration.requestedGeneration = 10;
  const auto next = registry.replace(*first.token, registration);
  ASSERT_TRUE(next.token.has_value());
  EXPECT_EQ(next.token->generation, 10ULL);
  registration.requestedGeneration.reset();
  const auto allocated = registry.replace(*next.token, registration);
  ASSERT_TRUE(allocated.token.has_value());
  EXPECT_EQ(allocated.token->generation, 11ULL);
  auto invalid = participant("other", "99"); invalid.requestedGeneration = 0;
  EXPECT_EQ(registry.add(invalid).result, Registry::Result::Invalid);
}

TEST(SourceRegistry, DecisionRevisionIgnoresFrameTrafficAndMetadataButTracksEligibility) {
  Registry registry("authority");
  registry.upsertPerson({{"person"}, "Name", 1});
  EXPECT_EQ(registry.snapshot()->decisionRevision, 1ULL);
  registry.upsertPerson({{"person"}, "Renamed", 1});
  EXPECT_EQ(registry.snapshot()->decisionRevision, 1ULL);
  registry.upsertPerson({{"person"}, "Renamed", 2});
  EXPECT_EQ(registry.snapshot()->decisionRevision, 2ULL);
  const auto added = registry.add(participant("camera"));
  ASSERT_TRUE(added.token.has_value());
  EXPECT_EQ(registry.snapshot()->decisionRevision, 3ULL);
  registry.setSubscription(*added.token, true, true);
  EXPECT_EQ(registry.snapshot()->decisionRevision, 3ULL);
  registry.publish(*added.token, 0, 0, format());
  EXPECT_EQ(registry.snapshot()->decisionRevision, 4ULL);
  const auto auditBefore = registry.snapshot()->revision;
  for (uint64_t frame = 1; frame <= 60; ++frame)
    EXPECT_EQ(registry.publish(*added.token, frame, static_cast<int64_t>(frame), format()), Registry::Result::Applied);
  EXPECT_EQ(registry.snapshot()->decisionRevision, 4ULL);
  EXPECT_EQ(registry.snapshot()->revision, auditBefore + 60);
  registry.setAvailability(*added.token, Registry::Availability::Unavailable);
  EXPECT_EQ(registry.snapshot()->decisionRevision, 5ULL);
  registry.setAvailability(*added.token, Registry::Availability::Available);
  EXPECT_EQ(registry.snapshot()->decisionRevision, 6ULL);
  registry.publish(*added.token, 61, 61, format());
  EXPECT_EQ(registry.snapshot()->decisionRevision, 7ULL);
  const auto replaced = registry.replace(*added.token, participant("camera"));
  ASSERT_TRUE(replaced.token.has_value());
  EXPECT_EQ(registry.snapshot()->decisionRevision, 8ULL);
  registry.retireProcessEpoch("meeting-1");
  EXPECT_EQ(registry.snapshot()->decisionRevision, 9ULL);
  registry.retireProcessEpoch("meeting-1");
  EXPECT_EQ(registry.snapshot()->decisionRevision, 9ULL);
}
TEST(SourceRegistry, IdentityAndFormatTextAreBoundedBeforeRetention) {
  Registry registry("authority");
  EXPECT_EQ(registry.upsertPerson({{std::string(513,'p')},"",1}),Registry::Result::Invalid);
  EXPECT_EQ(registry.upsertPerson({{"person"},std::string(4097,'n'),1}),Registry::Result::Invalid);
  auto registration = participant(std::string(513,'s'));
  EXPECT_EQ(registry.add(registration).result,Registry::Result::Invalid);
  registration = participant("camera"); registration.externalId = std::string(513,'e');
  EXPECT_EQ(registry.add(registration).result,Registry::Result::Invalid);
  registration = participant("camera"); const auto added = registry.add(registration);
  ASSERT_TRUE(added.token.has_value());
  auto oversizedFormat = format(); oversizedFormat.pixelFormat = std::string(129,'f');
  EXPECT_EQ(registry.publish(*added.token,1,1,oversizedFormat),Registry::Result::Invalid);
}
