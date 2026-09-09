#include "core/AtomicTakeCoordinator.h"
#include <gtest/gtest.h>
#include <atomic>
#include <condition_variable>
#include <stdexcept>
#include <thread>

namespace {
using Take = corevideo::core::AtomicTakeCoordinator;
Take::Request request(const std::string& id = "take-1") {
  return {"show-1", id, {10, 7, "media-1", 3, {}, "prepared-1", 1}};
}
}

TEST(AtomicTakeCoordinator, PreparationAndAllFingerprintFieldsFenceApply) {
  int calls = 0;
  Take owner("show-1", 10, 7, 8, [&](const auto&, auto) { ++calls; return Take::ApplyResult{true, {}}; });
  const auto original = request();
  EXPECT_EQ(owner.take(original).outcome.error, Take::Error::NotPrepared);
  EXPECT_EQ(owner.prepare("show-1", original.fingerprint), Take::Error::None);
  for (int field = 0; field < 5; ++field) {
    auto altered = original;
    if (field == 0) altered.fingerprint.mediaGeneration++;
    if (field == 1) altered.fingerprint.mediaProcessEpoch = "media-2";
    if (field == 2) altered.fingerprint.preparationToken = "different";
    if (field == 3) altered.fingerprint.preparationRevision++;
    if (field == 4) altered.fingerprint.transition = {Take::Transition::Kind::Fade, 100'000'000};
    EXPECT_EQ(owner.take(altered).outcome.error, Take::Error::NotPrepared);
  }
  EXPECT_EQ(calls, 0);
  const auto result = owner.take(original);
  EXPECT_TRUE(result.outcome.accepted);
  EXPECT_TRUE(result.outcome.applied);
  EXPECT_FALSE(result.outcome.rendered);
  EXPECT_FALSE(result.outcome.delivered);
  EXPECT_EQ(result.outcome.resultRevision, 11u);
  EXPECT_EQ(calls, 1);
  auto changed = original; changed.fingerprint.transition = {Take::Transition::Kind::Fade, 1};
  EXPECT_EQ(owner.take(changed).outcome.error, Take::Error::OperationConflict);
  EXPECT_TRUE(owner.take(original).replayed);
  EXPECT_EQ(calls, 1);
}

TEST(AtomicTakeCoordinator, EverySupportedEffectIsPartOfTheTakeFingerprint) {
  for (const auto kind : {Take::Transition::Kind::Fade, Take::Transition::Kind::Dip,
                          Take::Transition::Kind::Wipe}) {
    Take owner("show-1", 10, 7, 8,
               [](const auto&, auto) { return Take::ApplyResult{true, {}}; });
    auto r = request();
    if (kind == Take::Transition::Kind::Fade)
      r.fingerprint.transition = {kind, 300'000'000, "", ""};
    else if (kind == Take::Transition::Kind::Dip)
      r.fingerprint.transition = {kind, 300'000'000, "", "#000000"};
    else
      r.fingerprint.transition = {kind, 300'000'000, "left-to-right", ""};
    EXPECT_EQ(owner.prepare("show-1", r.fingerprint), Take::Error::None);
    EXPECT_TRUE(owner.take(r).outcome.applied);
    auto changed = r;
    if (kind == Take::Transition::Kind::Wipe) changed.fingerprint.transition.direction = "right-to-left";
    else changed.fingerprint.transition.durationNs++;
    EXPECT_EQ(owner.take(changed).outcome.error, Take::Error::OperationConflict);
  }
}

TEST(AtomicTakeCoordinator, RejectsMalformedTransitionParameters) {
  Take owner("show-1", 10, 7, 8,
             [](const auto&, auto) { return Take::ApplyResult{true, {}}; });
  auto r = request();
  r.fingerprint.transition = {Take::Transition::Kind::Wipe, 300'000'000, "diagonal", ""};
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint), Take::Error::Invalid);
  r.fingerprint.transition = {Take::Transition::Kind::Dip, 300'000'000, "", "black"};
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint), Take::Error::Invalid);
  r.fingerprint.transition = {Take::Transition::Kind::Fade, 0, "", ""};
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint), Take::Error::Invalid);
}

TEST(AtomicTakeCoordinator, CallbackCannotRetargetFinalizationByMutatingCallerRequest) {
  auto r = request();
  Take owner("show-1", 10, 7, 8, [&](Take::Request, auto) {
    r.operationId = "mutated";
    return Take::ApplyResult{true, {}};
  });
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint), Take::Error::None);
  const auto outcome = owner.take(r).outcome;
  EXPECT_TRUE(outcome.applied);
  EXPECT_EQ(outcome.operationId, "take-1");
  EXPECT_FALSE(owner.snapshot().applying);
}

TEST(AtomicTakeCoordinator, ObservationsRacingApplyAreRetainedUntilSuccess) {
  Take* pointer = nullptr;
  Take owner("show-1", 10, 7, 8, [&](Take::Request applying, auto revision) {
    EXPECT_EQ(pointer->observe(applying, revision, false), Take::Error::None);
    EXPECT_EQ(pointer->observe(applying, revision, true), Take::Error::None);
    const auto pending = pointer->take(applying);
    EXPECT_TRUE(pending.outcome.pending);
    EXPECT_FALSE(pending.outcome.rendered);
    return Take::ApplyResult{true, {}};
  });
  pointer = &owner;
  const auto r = request();
  owner.prepare("show-1", r.fingerprint);
  const auto result = owner.take(r);
  EXPECT_TRUE(result.outcome.applied);
  EXPECT_TRUE(result.outcome.rendered);
  EXPECT_TRUE(result.outcome.delivered);
}

TEST(AtomicTakeCoordinator, ObservationsRequireExactAppliedIdentityAndDoNotInventDelivery) {
  Take owner("show-1", 10, 7, 8, [](const auto&, auto) { return Take::ApplyResult{true, {}}; });
  const auto r = request(); owner.prepare("show-1", r.fingerprint); owner.take(r);
  EXPECT_EQ(owner.observe(r, 11, true), Take::Error::NotApplied);
  auto stale = r; stale.fingerprint.mediaGeneration++;
  EXPECT_EQ(owner.observe(stale, 11, false), Take::Error::StaleObservation);
  EXPECT_EQ(owner.observe(r, 10, false), Take::Error::StaleObservation);
  EXPECT_EQ(owner.observe(r, 11, false), Take::Error::None);
  EXPECT_FALSE(owner.take(r).outcome.delivered);
  EXPECT_EQ(owner.observe(r, 11, true), Take::Error::None);
  EXPECT_TRUE(owner.take(r).outcome.delivered);
  EXPECT_EQ(owner.updatePreview("show-1", 11, 8), Take::Error::None);
  EXPECT_EQ(owner.take(r).outcome.resultRevision, 11u); // Original result, not latest state.
}

TEST(AtomicTakeCoordinator, PreviewRevisionCannotMoveBackward) {
  Take owner("show-1", 10, 7, 8,
             [](const auto&, auto) { return Take::ApplyResult{true, {}}; });
  EXPECT_EQ(owner.updatePreview("show-1", 10, 6), Take::Error::StalePreview);
  EXPECT_EQ(owner.snapshot().revision, 10ULL);
  EXPECT_EQ(owner.snapshot().previewRevision, 7ULL);
}

TEST(AtomicTakeCoordinator, EvictionPreservesBoundedProtectionAndOldEpochNeverReplays) {
  int calls = 0;
  Take owner("show-1", 10, 7, 1, [&](const auto&, auto) { ++calls; return Take::ApplyResult{true, {}}; });
  const auto r = request(); owner.prepare("show-1", r.fingerprint); owner.take(r);
  EXPECT_EQ(owner.take(request("other")).outcome.error, Take::Error::Capacity);
  EXPECT_TRUE(owner.take(r).replayed);
  EXPECT_EQ(owner.evict("show-1", r.operationId), Take::Error::None);
  EXPECT_EQ(owner.snapshot().detailedResults, 0u);
  EXPECT_EQ(owner.snapshot().tombstones, 1u);
  EXPECT_EQ(owner.take(r).outcome.error, Take::Error::OperationExpired);
  EXPECT_EQ(owner.take(request("other")).outcome.error, Take::Error::Capacity);
  auto old = r; old.authorityEpoch = "old-show";
  EXPECT_EQ(owner.take(old).outcome.error, Take::Error::AuthorityEpoch);
  EXPECT_EQ(calls, 1);
}

TEST(AtomicTakeCoordinator, ExhaustionRejectsFreshWorkButReplaysLastAcceptedRevision) {
  constexpr uint64_t max = 9007199254740991ULL;
  Take owner("show-1", max - 1, 7, 4, [](const auto&, auto) { return Take::ApplyResult{true, {}}; });
  auto r = request(); r.fingerprint.expectedRevision = max - 1;
  owner.prepare("show-1", r.fingerprint);
  EXPECT_EQ(owner.take(r).outcome.resultRevision, max);
  auto next = r; next.operationId = "next"; next.fingerprint.expectedRevision = max;
  EXPECT_EQ(owner.take(next).outcome.error, Take::Error::RevisionExhausted);
  EXPECT_TRUE(owner.take(r).outcome.applied);
  EXPECT_TRUE(owner.take(r).replayed);
}

TEST(AtomicTakeCoordinator, FailedCallbackIsNeverRetriedAndConsumedPreparationCannotReturn) {
  int calls = 0;
  Take owner("show-1", 10, 7, 4, [&](const auto&, auto) -> Take::ApplyResult {
    ++calls; throw std::runtime_error(std::string(1000, 'x'));
  });
  const auto r = request(); owner.prepare("show-1", r.fingerprint);
  EXPECT_EQ(owner.take(r).outcome.error, Take::Error::ApplyFailed);
  EXPECT_EQ(owner.take(r).outcome.failure.size(), 512u);
  EXPECT_TRUE(owner.take(r).replayed);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(owner.snapshot().revision, 10u);
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint), Take::Error::NotPrepared);
  auto older = r.fingerprint; older.preparationRevision = 0;
  EXPECT_EQ(owner.prepare("show-1", older), Take::Error::NotPrepared);
}

TEST(AtomicTakeCoordinator, PendingDuplicateReplaysWithoutBlockingOrSecondCallback) {
  std::mutex gate; std::condition_variable cv; bool entered = false, release = false;
  std::atomic<int> calls{0};
  Take* ptr = nullptr;
  Take owner("show-1", 10, 7, 8, [&](const auto&, auto) {
    ++calls;
    const bool snapshotReadable = ptr->snapshot().applying; // Reentrant read: no owner lock held.
    std::unique_lock lock(gate); entered = true; cv.notify_all();
    cv.wait(lock, [&] { return release; });
    return Take::ApplyResult{snapshotReadable, {}};
  });
  ptr = &owner;
  const auto r = request(); owner.prepare("show-1", r.fingerprint);
  std::thread first([&] { owner.take(r); });
  {
    std::unique_lock lock(gate); cv.wait(lock, [&] { return entered; });
  }
  const auto duplicate = owner.take(r);
  const auto distinct = owner.take(request("take-2"));
  const auto eviction = owner.evict("show-1", r.operationId);
  { std::lock_guard lock(gate); release = true; } cv.notify_all(); first.join();
  EXPECT_TRUE(duplicate.replayed);
  EXPECT_TRUE(duplicate.outcome.accepted);
  EXPECT_TRUE(duplicate.outcome.pending);
  EXPECT_EQ(duplicate.outcome.operationId, r.operationId);
  EXPECT_EQ(duplicate.outcome.authorityEpoch, r.authorityEpoch);
  EXPECT_FALSE(duplicate.outcome.applied);
  EXPECT_EQ(distinct.outcome.error, Take::Error::Busy);
  EXPECT_TRUE(distinct.retryable);
  EXPECT_EQ(eviction, Take::Error::Busy);
  EXPECT_EQ(owner.take(request("take-2")).outcome.error, Take::Error::StaleRevision);
  EXPECT_EQ(calls.load(), 1);
  EXPECT_TRUE(owner.take(r).outcome.applied);
  EXPECT_FALSE(owner.take(r).outcome.pending);
  auto retry = request("take-2");
  retry.fingerprint.expectedRevision = 11;
  retry.fingerprint.preparationRevision = 2;
  retry.fingerprint.preparationToken = "prepared-2";
  EXPECT_EQ(owner.prepare("show-1", retry.fingerprint), Take::Error::None);
  EXPECT_TRUE(owner.take(retry).outcome.applied);
  EXPECT_EQ(calls.load(), 2); // Busy did not consume the distinct operation ID.
}
