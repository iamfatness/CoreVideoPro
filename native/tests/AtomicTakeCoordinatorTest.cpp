#include "core/AtomicTakeCoordinator.h"
#include <gtest/gtest.h>
#include <atomic>
#include <barrier>
#include <condition_variable>
#include <stdexcept>
#include <thread>

namespace {
using Take = corevideo::core::AtomicTakeCoordinator;
std::shared_ptr<const corevideo::core::PreparedShowCertificate> certificate(const Take::Fingerprint& f) {
  using namespace corevideo::core;
  if (!validShowPreparationToken(f.preparation)) return {};
  struct Ready final : IShowResourcePreparer {
    ShowPreparationEvidence poll(const ShowPreparationToken& t, const ShowPreparationRequirement& r) override {
      return {t, r, ShowPreparationEvidence::State::Ready, std::make_shared<const int>(1)};
    }
    void cancel(const ShowPreparationToken&) noexcept override {}
  };
  ShowPreparationTransaction owner(f.preparation.base, std::make_shared<Ready>());
  ShowPreparationPlan p{f.preparation.base, f.preparation.planId, f.preparation.planRevision,
    {{ShowPreparationRequirement::Kind::GpuResource, "gpu", 1, "media-1", "gpu-1"}}, f.preparation.stamp};
  for (uint64_t i = 1; i < f.preparation.transactionGeneration; ++i) { auto r = owner.begin(p); owner.cancel(r.token); }
  owner.begin(p); return owner.poll().certificate;
}
Take::Request request(const std::string& id = "take-1") {
  Take::Request r;
  r.authorityEpoch = "show-1"; r.operationId = id;
  r.fingerprint.expectedRevision = 10; r.fingerprint.previewRevision = 7;
  r.fingerprint.mediaProcessEpoch = "media-1"; r.fingerprint.mediaGeneration = 3;
  r.fingerprint.expectedPlanStamp = {"show-1", "registry-1", 10, 5, "eligibility-v1"};
  r.fingerprint.expectedPlanId = "plan-1";
  r.fingerprint.preparation = {{"show-1", 10, 3}, "plan-1", 11, 1, r.fingerprint.expectedPlanStamp};
  return r;
}
}

TEST(AtomicTakeCoordinator, PreparationAndAllFingerprintFieldsFenceApply) {
  int calls = 0;
  Take owner("show-1", 10, 7, 8, [&](const auto&, auto) { ++calls; return Take::ApplyResult{true, {}}; });
  const auto original = request();
  EXPECT_EQ(owner.take(original).outcome.error, Take::Error::NotPrepared);
  EXPECT_EQ(owner.prepare("show-1", original.fingerprint, certificate(original.fingerprint)), Take::Error::None);
  for (int field = 0; field < 5; ++field) {
    auto altered = original;
    if (field == 0) altered.fingerprint.mediaGeneration++;
    if (field == 1) altered.fingerprint.mediaProcessEpoch = "media-2";
    if (field == 2) {
      altered.fingerprint.preparation.planId = "different";
      altered.fingerprint.expectedPlanId = "different";
    }
    if (field == 3) altered.fingerprint.preparation.transactionGeneration++;
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
    EXPECT_EQ(owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint)), Take::Error::None);
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
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint)), Take::Error::Invalid);
  r.fingerprint.transition = {Take::Transition::Kind::Dip, 300'000'000, "", "black"};
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint)), Take::Error::Invalid);
  r.fingerprint.transition = {Take::Transition::Kind::Fade, 0, "", ""};
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint)), Take::Error::Invalid);
}

TEST(AtomicTakeCoordinator, CallbackCannotRetargetFinalizationByMutatingCallerRequest) {
  auto r = request();
  Take owner("show-1", 10, 7, 8, [&](Take::Request, auto) {
    r.operationId = "mutated";
    return Take::ApplyResult{true, {}};
  });
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint)), Take::Error::None);
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
  owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint));
  const auto result = owner.take(r);
  EXPECT_TRUE(result.outcome.applied);
  EXPECT_TRUE(result.outcome.rendered);
  EXPECT_TRUE(result.outcome.delivered);
}

TEST(AtomicTakeCoordinator, ObservationsRequireExactAppliedIdentityAndDoNotInventDelivery) {
  Take owner("show-1", 10, 7, 8, [](const auto&, auto) { return Take::ApplyResult{true, {}}; });
  const auto r = request(); owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint)); owner.take(r);
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
  const auto r = request(); owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint)); owner.take(r);
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
  r.fingerprint.preparation.base.revision = max - 1;
  r.fingerprint.preparation.planRevision = max;
  r.fingerprint.expectedPlanStamp.controlRevision = max - 1;
  r.fingerprint.preparation.stamp = r.fingerprint.expectedPlanStamp;
  owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint));
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
  const auto r = request(); owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint));
  EXPECT_EQ(owner.take(r).outcome.error, Take::Error::ApplyFailed);
  EXPECT_EQ(owner.take(r).outcome.failure.size(), 512u);
  EXPECT_TRUE(owner.take(r).replayed);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(owner.snapshot().revision, 10u);
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint)), Take::Error::NotPrepared);
  auto older = r.fingerprint; older.preparation.transactionGeneration = 0;
  EXPECT_EQ(owner.prepare("show-1", older, certificate(older)), Take::Error::Invalid);
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
  const auto r = request(); owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint));
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
  retry.fingerprint.preparation.transactionGeneration = 2;
  retry.fingerprint.preparation.base.revision = 11;
  retry.fingerprint.preparation.planRevision = 12;
  retry.fingerprint.expectedPlanStamp.controlRevision = 11;
  retry.fingerprint.preparation.stamp = retry.fingerprint.expectedPlanStamp;
  EXPECT_EQ(owner.prepare("show-1", retry.fingerprint, certificate(retry.fingerprint)), Take::Error::None);
  EXPECT_TRUE(owner.take(retry).outcome.applied);
  EXPECT_EQ(calls.load(), 2); // Busy did not consume the distinct operation ID.
}

TEST(AtomicTakeCoordinator, TypedCertificateRejectsDifferentAuthorityControlAndPlanBasis) {
  int calls = 0;
  Take owner("show-1", 10, 7, 8, [&](const auto&, auto) { ++calls; return Take::ApplyResult{true, {}}; });
  for (int mismatch = 0; mismatch < 6; ++mismatch) {
    auto r = request();
    if (mismatch == 0) r.fingerprint.preparation.base.epoch = "old-show";
    if (mismatch == 1) r.fingerprint.preparation.stamp.registryEpoch = "other-registry";
    if (mismatch == 2) ++r.fingerprint.preparation.stamp.registryRevision;
    if (mismatch == 3) ++r.fingerprint.preparation.planRevision;
    if (mismatch == 4) r.fingerprint.expectedPlanId = "other-plan";
    if (mismatch == 5) r.fingerprint.preparation.stamp.eligibilityIdentity += "-different";
    EXPECT_EQ(owner.prepare("show-1", r.fingerprint, certificate(r.fingerprint)), Take::Error::Invalid);
  }
  EXPECT_EQ(calls, 0);
}

TEST(AtomicTakeCoordinator, UsesIssuedPreparationCertificateWithoutDoubleRevisionIncrement) {
  struct Ready final : corevideo::core::IShowResourcePreparer {
    corevideo::core::ShowPreparationEvidence poll(const corevideo::core::ShowPreparationToken& t,
        const corevideo::core::ShowPreparationRequirement& r) override {
      return {t, r, corevideo::core::ShowPreparationEvidence::State::Ready, std::make_shared<const int>(1)};
    }
    void cancel(const corevideo::core::ShowPreparationToken&) noexcept override {}
  };
  auto r = request();
  corevideo::core::ShowPreparationTransaction preparation({"show-1", 10, 3}, std::make_shared<Ready>());
  corevideo::core::ShowPreparationPlan plan{{"show-1", 10, 3}, "plan-1", 11,
      {{corevideo::core::ShowPreparationRequirement::Kind::GpuResource, "program", 3, "media-1", "gpu-instance"}},
      r.fingerprint.expectedPlanStamp};
  preparation.begin(plan);
  const auto ready = preparation.poll();
  EXPECT_EQ(ready.status, corevideo::core::ShowPreparationTransaction::Status::Ready);
  r.fingerprint.preparation = ready.token;
  int calls = 0;
  Take owner("show-1", 10, 7, 8, [&](const auto& req, auto revision) {
    ++calls;
    EXPECT_TRUE(req.fingerprint.preparation == ready.token);
    EXPECT_EQ(revision, ready.token.planRevision);
    return Take::ApplyResult{true, {}};
  });
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint, ready.certificate), Take::Error::None);
  EXPECT_EQ(owner.take(r).outcome.resultRevision, 11u);
  EXPECT_EQ(owner.snapshot().revision, 11u);
  EXPECT_TRUE(owner.take(r).replayed);
  EXPECT_EQ(calls, 1);
}

TEST(AtomicTakeCoordinator, PendingTokenCannotPrepareAndFailedTakeCanRetryFromSameBase) {
  using namespace corevideo::core;
  struct Ready final : IShowResourcePreparer {
    ShowPreparationEvidence poll(const ShowPreparationToken& t, const ShowPreparationRequirement& r) override {
      return {t, r, ShowPreparationEvidence::State::Ready, std::make_shared<const int>(1)};
    }
    void cancel(const ShowPreparationToken&) noexcept override {}
  };
  auto r = request();
  ShowPreparationTransaction preparation(r.fingerprint.preparation.base, std::make_shared<Ready>());
  ShowPreparationPlan p{r.fingerprint.preparation.base, "plan-1", 11,
    {{ShowPreparationRequirement::Kind::GpuResource, "gpu", 3, "media-1", "gpu-instance"}}, r.fingerprint.expectedPlanStamp};
  auto pending = preparation.begin(p);
  r.fingerprint.preparation = pending.token;
  int attempts = 0;
  Take owner("show-1", 10, 7, 8, [&](const auto&, auto) { return Take::ApplyResult{++attempts > 1, "first failed"}; });
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint, pending.certificate), Take::Error::NotPrepared);
  auto ready = preparation.poll();
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint, ready.certificate), Take::Error::None);
  EXPECT_EQ(owner.take(r).outcome.error, Take::Error::ApplyFailed);
  EXPECT_EQ(owner.snapshot().revision, 10ULL);
  preparation.cancel(ready.token);
  pending = preparation.begin(p); ready = preparation.poll();
  r.operationId = "retry"; r.fingerprint.preparation = pending.token;
  std::weak_ptr<const void> lease = ready.certificate->leases[0];
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint, ready.certificate), Take::Error::None);
  EXPECT_TRUE(owner.take(r).outcome.applied);
  EXPECT_TRUE(preparation.commit(ready.token, p.base, {"show-1", 11, 3}, ready.certificate));
  EXPECT_EQ(preparation.snapshot().status, ShowPreparationTransaction::Status::Applied);
  ready = {}; pending = {};
  EXPECT_FALSE(lease.expired());
  EXPECT_TRUE(owner.take(r).replayed);
}

TEST(AtomicTakeCoordinator, CertificateLeasesRemainOwnedUntilOperationEviction) {
  auto r = request(); auto issued = certificate(r.fingerprint);
  ASSERT_TRUE(issued);
  std::weak_ptr<const void> lease = issued->leases[0];
  Take owner("show-1", 10, 7, 8, [](const auto&, auto) { return Take::ApplyResult{true, {}}; });
  EXPECT_EQ(owner.prepare("show-1", r.fingerprint, issued), Take::Error::None);
  issued.reset();
  EXPECT_FALSE(lease.expired());
  EXPECT_TRUE(owner.take(r).outcome.applied);
  EXPECT_FALSE(lease.expired());
  EXPECT_EQ(owner.evict("show-1", r.operationId), Take::Error::None);
  EXPECT_TRUE(lease.expired());
}

TEST(AtomicTakeCoordinator, CancellationAndApplyingHaveExactlyOneWinner) {
  using namespace corevideo::core;
  struct Ready final : IShowResourcePreparer {
    ShowPreparationEvidence poll(const ShowPreparationToken& t, const ShowPreparationRequirement& r) override {
      return {t, r, ShowPreparationEvidence::State::Ready, std::make_shared<const int>(1)};
    }
    void cancel(const ShowPreparationToken&) noexcept override {}
  };
  for (bool cancelFirst : {false, true}) {
    auto r = request(); ShowPreparationTransaction preparation(r.fingerprint.preparation.base, std::make_shared<Ready>());
    ShowPreparationPlan p{r.fingerprint.preparation.base, "plan-1", 11, {}, r.fingerprint.expectedPlanStamp};
    preparation.begin(p); const auto ready = preparation.poll(); r.fingerprint.preparation = ready.token;
    int calls = 0;
    Take owner("show-1", 10, 7, 8, [&](const auto&, auto) {
      ++calls;
      EXPECT_EQ(preparation.cancel(ready.token).status, ShowPreparationTransaction::Status::Busy);
      return Take::ApplyResult{true, {}};
    });
    ASSERT_EQ(owner.prepare("show-1", r.fingerprint, ready.certificate), Take::Error::None);
    EXPECT_EQ(owner.prepare("show-1", r.fingerprint, ready.certificate), Take::Error::None);
    if (cancelFirst) EXPECT_EQ(preparation.cancel(ready.token).status, ShowPreparationTransaction::Status::Cancelled);
    const auto result = owner.take(r);
    EXPECT_EQ(calls, cancelFirst ? 0 : 1);
    EXPECT_EQ(result.outcome.applied, !cancelFirst);
    if (cancelFirst) EXPECT_EQ(result.outcome.error, Take::Error::NotPrepared);
    else EXPECT_TRUE(preparation.commit(ready.token, p.base, {"show-1", 11, 3}, ready.certificate));
  }
}

TEST(AtomicTakeCoordinator, SimultaneousCancellationAndTakeHaveOneCertificateWinner) {
  using namespace corevideo::core;
  struct Ready final : IShowResourcePreparer {
    std::atomic<int> cancellations{0};
    ShowPreparationEvidence poll(const ShowPreparationToken& t, const ShowPreparationRequirement& r) override {
      return {t, r, ShowPreparationEvidence::State::Ready, std::make_shared<const int>(1)};
    }
    void cancel(const ShowPreparationToken&) noexcept override { ++cancellations; }
  };
  // The scheduler may choose either winner. Correctness cannot depend on seeing
  // both outcomes in a run; the ordered test above covers each branch explicitly.
  for (int iteration = 0; iteration < 64; ++iteration) {
    auto provider = std::make_shared<Ready>();
    auto r = request();
    ShowPreparationTransaction preparation(r.fingerprint.preparation.base, provider);
    ShowPreparationPlan p{r.fingerprint.preparation.base, "plan-1", 11,
        {{ShowPreparationRequirement::Kind::GpuResource, "gpu", 3, "media-1", "gpu-instance"}},
        r.fingerprint.expectedPlanStamp};
    preparation.begin(p);
    const auto ready = preparation.poll();
    r.fingerprint.preparation = ready.token;
    std::barrier start(3);
    std::mutex completionMutex;
    std::condition_variable completionCv;
    bool cancellationFinished = false;
    int applyCalls = 0;
    Take owner("show-1", 10, 7, 8, [&](const auto&, auto) {
      ++applyCalls;
      // If Take wins, keep its certificate Applying until the competing cancel
      // has observed it. This tests the actual CAS race without timing sleeps.
      std::unique_lock lock(completionMutex);
      completionCv.wait(lock, [&] { return cancellationFinished; });
      return Take::ApplyResult{true, {}};
    });
    ASSERT_EQ(owner.prepare("show-1", r.fingerprint, ready.certificate), Take::Error::None);
    Take::Reply taken;
    ShowPreparationTransaction::Result cancelled;
    std::thread takeThread([&] { start.arrive_and_wait(); taken = owner.take(r); });
    std::thread cancelThread([&] {
      start.arrive_and_wait();
      cancelled = preparation.cancel(ready.token);
      { std::lock_guard lock(completionMutex); cancellationFinished = true; }
      completionCv.notify_one();
    });
    start.arrive_and_wait();
    takeThread.join();
    cancelThread.join();
    const bool cancellationWon = cancelled.status == ShowPreparationTransaction::Status::Cancelled;
    EXPECT_EQ(cancelled.status, cancellationWon ? ShowPreparationTransaction::Status::Cancelled
                                               : ShowPreparationTransaction::Status::Busy);
    EXPECT_EQ(applyCalls, cancellationWon ? 0 : 1);
    EXPECT_EQ(provider->cancellations.load(), cancellationWon ? 1 : 0);
    EXPECT_EQ(taken.outcome.applied, !cancellationWon);
    EXPECT_FALSE(taken.outcome.pending);
    EXPECT_EQ(owner.snapshot().revision, cancellationWon ? 10ULL : 11ULL);
    EXPECT_EQ(ready.certificate->state(), cancellationWon ? PreparedShowCertificate::State::Revoked
                                                        : PreparedShowCertificate::State::Applied);
    EXPECT_EQ(preparation.commit(ready.token, p.base, {"show-1", 11, 3}, ready.certificate), !cancellationWon);
    const auto repeat = owner.take(r);
    EXPECT_EQ(applyCalls, cancellationWon ? 0 : 1);
    if (cancellationWon) {
      EXPECT_EQ(taken.outcome.error, Take::Error::NotPrepared);
      EXPECT_EQ(repeat.outcome.error, Take::Error::NotPrepared);
    } else {
      EXPECT_TRUE(repeat.replayed);
      EXPECT_TRUE(repeat.outcome.applied);
    }
  }
}
