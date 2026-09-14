#include "core/AtomicTakeRpcAdapter.h"
#include <gtest/gtest.h>
#include <stdexcept>
namespace {
namespace rpc = corevideo::rpc;
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


TEST(AtomicTakeRpcAdapter, DisabledNeverCallsAuthorityAndRequiresOwnerForOptIn) {
  using namespace corevideo::core;
  AtomicTakeRpcAdapter adapter;
  EXPECT_FALSE(adapter.capabilities().get("atomicTake")->asBool());
  const auto response = adapter.handle(*AtomicTakeJsonCodec::encodeRequest(request()));
  EXPECT_EQ(response.getString("error"), "capability-disabled");
  EXPECT_TRUE(response.get("outcome") == nullptr);
  bool rejected = false;
  try { AtomicTakeRpcAdapter invalid({true}, {}); } catch (const std::invalid_argument&) { rejected = true; }
  EXPECT_TRUE(rejected);
  int calls = 0;
  auto owner = std::make_shared<Take>("show-1", 10, 7, 8, [&](auto, auto) { ++calls; return Take::ApplyResult{true,{}}; });
  AtomicTakeRpcAdapter disabled({}, owner);
  (void)disabled.handle(*AtomicTakeJsonCodec::encodeRequest(request()));
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(owner->snapshot().revision, 10ULL);
}
TEST(AtomicTakeRpcAdapter, AppliedReplyDoesNotInventMediaAndReplayPreservesObservedStages) {
  using namespace corevideo::core;
  int calls = 0;
  auto owner = std::make_shared<Take>("show-1", 10, 7, 8, [&](auto, auto) { ++calls; return Take::ApplyResult{true,{}}; });
  const auto r = request();
  ASSERT_EQ(owner->prepare(r.authorityEpoch, r.fingerprint, certificate(r.fingerprint)), Take::Error::None);
  AtomicTakeRpcAdapter adapter({true}, owner);
  const auto wire = *AtomicTakeJsonCodec::encodeRequest(r);
  auto reply = adapter.handle(wire);
  auto outcome = AtomicTakeJsonCodec::decodeOutcome(*reply.get("outcome"));
  ASSERT_TRUE(outcome.has_value());
  EXPECT_TRUE(outcome->accepted); EXPECT_TRUE(outcome->applied);
  EXPECT_FALSE(outcome->rendered); EXPECT_FALSE(outcome->delivered);
  EXPECT_EQ(outcome->resultRevision, 11ULL);
  EXPECT_EQ(owner->observe(r, 11, true), Take::Error::NotApplied);
  ASSERT_EQ(owner->observe(r, 11, false), Take::Error::None);
  ASSERT_EQ(owner->observe(r, 11, true), Take::Error::None);
  reply = adapter.handle(wire);
  outcome = AtomicTakeJsonCodec::decodeOutcome(*reply.get("outcome"));
  ASSERT_TRUE(outcome.has_value());
  EXPECT_TRUE(outcome->rendered); EXPECT_TRUE(outcome->delivered);
  EXPECT_TRUE(reply.get("replayed")->asBool()); EXPECT_EQ(calls, 1);
}
TEST(AtomicTakeRpcAdapter, RejectsClientEvidenceAndStalePreviewWithoutApply) {
  using namespace corevideo::core;
  int calls = 0;
  auto owner = std::make_shared<Take>("show-1", 10, 8, 8, [&](auto, auto) { ++calls; return Take::ApplyResult{true,{}}; });
  AtomicTakeRpcAdapter adapter({true}, owner);
  const auto reply = adapter.handle(*AtomicTakeJsonCodec::encodeRequest(request()));
  const auto outcome = AtomicTakeJsonCodec::decodeOutcome(*reply.get("outcome"));
  ASSERT_TRUE(outcome.has_value()); EXPECT_EQ(outcome->error, Take::Error::StalePreview);
  EXPECT_FALSE(outcome->accepted); EXPECT_EQ(calls, 0);
  EXPECT_EQ(adapter.handle(rpc::Json::Object{{"type","observe"},{"delivered",true}}).getString("error"), "invalid-request");
  EXPECT_EQ(adapter.handle(rpc::Json::Object{{"type","take"}}).getString("error"), "invalid-request");
}

TEST(AtomicTakeRpcAdapter, InFlightReplayReportsAcceptedPendingOnly) {
  using namespace corevideo::core;
  AtomicTakeRpcAdapter* adapter = nullptr;
  rpc::Json pending;
  auto owner = std::make_shared<Take>("show-1", 10, 7, 8, [&](const auto& r, auto) {
    pending = adapter->handle(*AtomicTakeJsonCodec::encodeRequest(r));
    return Take::ApplyResult{true,{}};
  });
  const auto r = request();
  ASSERT_EQ(owner->prepare(r.authorityEpoch, r.fingerprint, certificate(r.fingerprint)), Take::Error::None);
  AtomicTakeRpcAdapter boundary({true}, owner); adapter = &boundary;
  const auto applied = boundary.handle(*AtomicTakeJsonCodec::encodeRequest(r));
  EXPECT_TRUE(applied.get("ok")->asBool());
  const auto outcome = AtomicTakeJsonCodec::decodeOutcome(*pending.get("outcome"));
  ASSERT_TRUE(outcome.has_value()); EXPECT_TRUE(outcome->pending); EXPECT_TRUE(outcome->accepted);
  EXPECT_FALSE(outcome->applied); EXPECT_FALSE(outcome->rendered); EXPECT_FALSE(outcome->delivered);
  EXPECT_TRUE(pending.get("replayed")->asBool()); EXPECT_TRUE(pending.get("reconcileRequired")->asBool());
}
