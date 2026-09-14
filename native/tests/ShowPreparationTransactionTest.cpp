#include "core/ShowPreparationTransaction.h"
#include "core/AtomicTakeCoordinator.h"
#include <gtest/gtest.h>
#include <functional>
#include <map>
#include <stdexcept>

using namespace corevideo::core;
namespace {
using Status = ShowPreparationTransaction::Status;
using State = ShowPreparationEvidence::State;
struct FakePreparer final : IShowResourcePreparer {
  std::map<std::string, State> states;
  std::map<std::string, int> calls;
  int cancellations{0};
  bool staleToken{false}, staleRequirement{false}, throwPoll{false}, omitLease{false};
  bool staleOwnerEpoch{false}, staleOwnerInstance{false}, staleStamp{false};
  std::function<void(const ShowPreparationToken&)> duringPoll;
  ShowPreparationEvidence poll(const ShowPreparationToken& token,
                               const ShowPreparationRequirement& requirement) override {
    ++calls[requirement.id];
    if (duringPoll) duringPoll(token);
    if (throwPoll) throw std::runtime_error("preparation failed");
    ShowPreparationEvidence result{token, requirement, states[requirement.id], {}};
    if (staleToken) ++result.token.transactionGeneration;
    if (staleRequirement) ++result.requirement.generation;
    if (staleOwnerEpoch) result.requirement.ownerEpoch = "retired-owner";
    if (staleOwnerInstance) result.requirement.ownerInstanceId = "retired-instance";
    if (staleStamp) ++result.token.stamp.registryRevision;
    if (result.state == State::Ready && !omitLease) result.lease = std::make_shared<const int>(7);
    return result;
  }
  void cancel(const ShowPreparationToken&) noexcept override { ++cancellations; }
};
ShowPreparationPlan plan(std::uint64_t base = 0) {
  return {{"show", base, 1}, "plan-" + std::to_string(base + 1), base + 1,
    {{ShowPreparationRequirement::Kind::SourceSubscription, "camera", 2, "zoom-process", "camera-instance"},
     {ShowPreparationRequirement::Kind::GpuResource, "canvas", 1, "gpu-process", "canvas-instance"},
     {ShowPreparationRequirement::Kind::OutputReservation, "recorder", 3, "writer-process", "recorder-instance"}},
    {"show", "registry", base, 5, "eligibility-v1"}};
}
bool applyAndCommit(ShowPreparationTransaction& owner) {
  const auto ready = owner.snapshot(); const auto& t = ready.token;
  AtomicTakeCoordinator::Request r; r.authorityEpoch = t.base.epoch; r.operationId = "apply";
  r.fingerprint.expectedRevision = t.base.revision; r.fingerprint.mediaGeneration = t.base.generation;
  r.fingerprint.mediaProcessEpoch = "media"; r.fingerprint.expectedPlanStamp = t.stamp;
  r.fingerprint.expectedPlanId = t.planId; r.fingerprint.preparation = t;
  AtomicTakeCoordinator take(t.base.epoch, t.base.revision, 0, 2,
    [](const auto&, auto) { return AtomicTakeCoordinator::ApplyResult{true, {}}; });
  if (take.prepare(t.base.epoch, r.fingerprint, ready.certificate) != AtomicTakeCoordinator::Error::None ||
      !take.take(r).outcome.applied) return false;
  return owner.commit(t, t.base, {t.base.epoch, t.planRevision, t.base.generation}, ready.certificate);
}
void allReady(const std::shared_ptr<FakePreparer>& p) {
  for (const auto& id : {"camera", "canvas", "recorder"}) p->states[id] = State::Ready;
}
}
TEST(ShowPreparationTransaction, RequiresAllResourcesAndPollReplayDoesNotPrepareTwice) {
  auto p = std::make_shared<FakePreparer>();
  ShowPreparationTransaction owner({"show", 0, 1}, p);
  const auto started = owner.begin(plan());
  EXPECT_EQ(started.status, Status::Preparing);
  p->states["camera"] = State::Ready;
  EXPECT_EQ(owner.poll().status, Status::Preparing);
  EXPECT_FALSE(owner.snapshot().applied);
  allReady(p);
  const auto done = owner.poll();
  ASSERT_TRUE(done.certificate);
  EXPECT_FALSE(done.applied);
  EXPECT_EQ(done.status, Status::Ready);
  EXPECT_EQ(done.certificate->plan->revision, 1ULL);
  EXPECT_EQ(done.certificate->leases.size(), 3U);
  EXPECT_EQ(p->calls["camera"], 1);
  const auto calls = p->calls;
  EXPECT_EQ(owner.poll().certificate.get(), done.certificate.get());
  EXPECT_EQ(owner.begin(plan()).token, started.token);
  EXPECT_EQ(p->calls, calls);
}
TEST(ShowPreparationTransaction, FailurePreservesPreviouslyAppliedRevision) {
  auto p = std::make_shared<FakePreparer>(); allReady(p);
  ShowPreparationTransaction owner({"show", 0, 1}, p);
  owner.begin(plan()); owner.poll();
  ASSERT_TRUE(applyAndCommit(owner));
  const auto original = owner.snapshot().applied;
  p->states["recorder"] = State::Failed;
  owner.begin(plan(1)); const auto failed = owner.poll();
  EXPECT_EQ(failed.status, Status::Failed);
  EXPECT_EQ(failed.applied.get(), original.get());
  EXPECT_EQ(failed.applied->plan->revision, 1ULL);
  EXPECT_EQ(p->cancellations, 1);
}
TEST(ShowPreparationTransaction, ReentrantCancellationDoesNotDeadlockOrPublishLateReady) {
  auto p = std::make_shared<FakePreparer>(); allReady(p);
  ShowPreparationTransaction owner({"show", 0, 1}, p);
  const auto token = owner.begin(plan()).token;
  p->duringPoll = [&](const ShowPreparationToken& t) {
    EXPECT_EQ(owner.snapshot().status, Status::Preparing);
    EXPECT_EQ(owner.cancel(t).status, Status::Cancelled);
  };
  EXPECT_EQ(owner.poll().status, Status::Cancelled);
  EXPECT_FALSE(owner.snapshot().applied);
  EXPECT_EQ(p->calls["camera"], 1);
  EXPECT_EQ(p->calls["canvas"], 0);
  EXPECT_EQ(owner.cancel(token).status, Status::Cancelled);
  EXPECT_EQ(p->cancellations, 1);
}
TEST(ShowPreparationTransaction, AuthorityGenerationChangeFencesInFlightReadiness) {
  auto p = std::make_shared<FakePreparer>(); allReady(p);
  ShowPreparationTransaction owner({"show", 0, 1}, p);
  owner.begin(plan());
  p->duringPoll = [&](const ShowPreparationToken&) {
    EXPECT_TRUE(owner.advanceAuthority({"show", 0, 1}, {"show", 0, 2}));
  };
  EXPECT_EQ(owner.poll().status, Status::Stale);
  EXPECT_FALSE(owner.snapshot().applied);
  EXPECT_EQ(owner.begin(plan()).status, Status::Stale);
  EXPECT_FALSE(applyAndCommit(owner));
}
TEST(ShowPreparationTransaction, RejectsMismatchedEvidenceAndReadyWithoutOwnership) {
  for (int mode = 0; mode < 4; ++mode) {
    auto p = std::make_shared<FakePreparer>(); allReady(p);
    p->staleToken = mode == 0; p->staleRequirement = mode == 1;
    p->omitLease = mode == 2; p->throwPoll = mode == 3;
    ShowPreparationTransaction owner({"show", 0, 1}, p);
    owner.begin(plan()); EXPECT_EQ(owner.poll().status, Status::Failed);
    EXPECT_FALSE(owner.snapshot().applied); EXPECT_EQ(p->cancellations, 1);
  }
}
TEST(ShowPreparationTransaction, InputCopyIsImmutableAndStateAdmissionIsBounded) {
  auto p = std::make_shared<FakePreparer>(); allReady(p);
  ShowPreparationTransaction owner({"show", 0, 1}, p, 3);
  auto request = plan(); const auto started = owner.begin(request);
  request.requirements[0].generation = 99;
  EXPECT_EQ(owner.begin(request).status, Status::Busy);
  const auto done = owner.poll(); ASSERT_TRUE(done.certificate);
  EXPECT_FALSE(done.applied);
  EXPECT_EQ(done.certificate->plan->requirements[0].generation, 2ULL);
  auto oversized = plan(1); oversized.requirements.push_back(oversized.requirements[0]);
  EXPECT_EQ(owner.begin(oversized).status, Status::Invalid);
  auto wrongRevision = plan(1); wrongRevision.revision = 5;
  EXPECT_EQ(owner.begin(wrongRevision).status, Status::Invalid);
  auto wrongAuthority = plan(1); wrongAuthority.base.epoch = "other";
  wrongAuthority.stamp.authorityEpoch = "other";
  EXPECT_EQ(owner.begin(wrongAuthority).status, Status::Stale);
  EXPECT_EQ(owner.cancel(started.token).applied.get(), done.applied.get());
}
TEST(ShowPreparationTransaction, ConcurrentPollAttemptDoesNotDuplicatePreparation) {
  auto p = std::make_shared<FakePreparer>(); allReady(p);
  ShowPreparationTransaction owner({"show", 0, 1}, p);
  owner.begin(plan());
  p->duringPoll = [&](const ShowPreparationToken&) { EXPECT_EQ(owner.poll().status, Status::Preparing); };
  EXPECT_EQ(owner.poll().status, Status::Ready);
  EXPECT_EQ(p->calls["camera"], 1); EXPECT_EQ(p->calls["canvas"], 1); EXPECT_EQ(p->calls["recorder"], 1);
}
TEST(ShowPreparationTransaction, OldCancellationCannotCancelANewerTransaction) {
  auto p = std::make_shared<FakePreparer>(); allReady(p);
  ShowPreparationTransaction owner({"show", 0, 1}, p);
  const auto old = owner.begin(plan()).token; owner.cancel(old);
  auto replacement = plan(); replacement.id = "new-plan";
  const auto fresh = owner.begin(replacement).token;
  EXPECT_TRUE(fresh.transactionGeneration > old.transactionGeneration);
  EXPECT_EQ(owner.cancel(old).status, Status::Stale);
  EXPECT_EQ(owner.poll().status, Status::Ready);
}

TEST(ShowPreparationTransaction, AuthorityRevisionNeverRegressesAndRetiredEpochCannotReturn) {
  auto p = std::make_shared<FakePreparer>();
  ShowPreparationTransaction owner({"show-a", 10, 1}, p);
  EXPECT_FALSE(owner.advanceAuthority({"show-a", 10, 1}, {"show-a", 9, 2}));
  EXPECT_TRUE(owner.advanceAuthority({"show-a", 10, 1}, {"show-b", 0, 1}));
  EXPECT_FALSE(owner.advanceAuthority({"show-b", 0, 1}, {"show-a", 11, 3}));
  EXPECT_TRUE(owner.advanceAuthority({"show-b", 0, 1}, {"show-c", 0, 1}));
  EXPECT_FALSE(owner.advanceAuthority({"show-c", 0, 1}, {"show-b", 1, 2}));
}

TEST(ShowPreparationTransaction, RetiredAuthorityEpochAdmissionIsBounded) {
  auto p = std::make_shared<FakePreparer>();
  ShowPreparationTransaction owner({"show-a", 0, 1}, p, 64, 2);
  EXPECT_TRUE(owner.advanceAuthority({"show-a", 0, 1}, {"show-b", 0, 1}));
  EXPECT_TRUE(owner.advanceAuthority({"show-b", 0, 1}, {"show-c", 0, 1}));
  EXPECT_FALSE(owner.advanceAuthority({"show-c", 0, 1}, {"show-d", 0, 1}));
  EXPECT_EQ(owner.snapshot().status, Status::Stale);
}

TEST(ShowPreparationTransaction, ResourceOwnerAndCompletePlanStampFenceReadyEvidence) {
  for (int mismatch = 0; mismatch < 3; ++mismatch) {
    auto p = std::make_shared<FakePreparer>(); allReady(p);
    ShowPreparationTransaction owner({"show", 0, 1}, p);
    owner.begin(plan());
    p->staleOwnerEpoch = mismatch == 0;
    p->staleOwnerInstance = mismatch == 1;
    p->staleStamp = mismatch == 2;
    EXPECT_EQ(owner.poll().status, Status::Failed);
    EXPECT_FALSE(owner.snapshot().applied);
    EXPECT_EQ(p->cancellations, 1);
  }
}

TEST(ShowPreparationTransaction, RejectsWrongBasisAndUnboundedResourceOwnerIdentity) {
  auto p = std::make_shared<FakePreparer>();
  ShowPreparationTransaction owner({"show", 0, 1}, p);
  auto request = plan(); request.stamp.controlRevision = 1;
  EXPECT_EQ(owner.begin(request).status, Status::Invalid);
  request = plan(); request.stamp.authorityEpoch = "other";
  EXPECT_EQ(owner.begin(request).status, Status::Invalid);
  request = plan(); request.requirements[0].ownerInstanceId.clear();
  EXPECT_EQ(owner.begin(request).status, Status::Invalid);
  request = plan(); request.requirements[0].ownerEpoch.assign(257, 'x');
  EXPECT_EQ(owner.begin(request).status, Status::Invalid);
  EXPECT_TRUE(p->calls.empty());
}

TEST(ShowPreparationTransaction, ReadyRequiresExplicitCommitAndCanBeRepreparedAtUncommittedBase) {
  auto p = std::make_shared<FakePreparer>(); allReady(p);
  ShowPreparationTransaction owner({"show", 0, 1}, p);
  const auto pending = owner.begin(plan());
  EXPECT_FALSE(pending.certificate);
  const auto ready = owner.poll();
  ASSERT_TRUE(ready.certificate);
  EXPECT_EQ(ready.status, Status::Ready);
  EXPECT_FALSE(ready.applied);
  EXPECT_EQ(owner.cancel(ready.token).status, Status::Cancelled);
  const auto retry = owner.begin(plan());
  EXPECT_EQ(retry.status, Status::Preparing);
  EXPECT_TRUE(retry.token.transactionGeneration > ready.token.transactionGeneration);
  const auto prepared = owner.poll();
  ASSERT_TRUE(prepared.certificate);
  EXPECT_TRUE(applyAndCommit(owner));
  const auto committed = owner.snapshot();
  EXPECT_EQ(committed.status, Status::Applied);
  EXPECT_EQ(committed.certificate.get(), prepared.certificate.get());
  ASSERT_TRUE(committed.applied);
  EXPECT_EQ(committed.applied->plan->revision, 1ULL);
}

TEST(ShowPreparationTransaction, MissingEligibilityIdentityCannotIssueCertificate) {
  auto p = std::make_shared<FakePreparer>(); allReady(p);
  ShowPreparationTransaction owner({"show", 0, 1}, p);
  auto invalid = plan(); invalid.stamp.eligibilityIdentity.clear();
  EXPECT_EQ(owner.begin(invalid).status, Status::Invalid);
  EXPECT_FALSE(owner.snapshot().certificate);
}

TEST(ShowPreparationTransaction, UnrelatedAdvanceRevokesReadyWithoutCommittingAndInvalidResultRedacts) {
  auto p = std::make_shared<FakePreparer>(); allReady(p);
  ShowPreparationTransaction owner({"show", 0, 1}, p);
  owner.begin(plan()); const auto ready = owner.poll();
  auto invalid = plan(); invalid.id.clear();
  const auto bad = owner.begin(invalid);
  EXPECT_EQ(bad.status, Status::Invalid); EXPECT_FALSE(bad.certificate);
  EXPECT_TRUE(owner.advanceAuthority({"show", 0, 1}, {"show", 1, 1}));
  EXPECT_EQ(owner.snapshot().status, Status::Stale);
  EXPECT_FALSE(owner.snapshot().applied);
  EXPECT_EQ(ready.certificate->state(), PreparedShowCertificate::State::Revoked);
  EXPECT_FALSE(owner.commit(ready.token, ready.token.base, {"show", 1, 1}, ready.certificate));
}
