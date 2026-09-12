#include "core/OutputLifecyclePolicy.h"
#include "modules/Interfaces.h"

#include <optional>

#include <gtest/gtest.h>

namespace {

using corevideo::core::ActiveOutputObservation;
using corevideo::core::kProducingProgressStaleMs;
using corevideo::core::OutputLifecyclePolicy;
using corevideo::core::publishedRecordingStatus;
using corevideo::core::publishedRecordingWriterStatus;
using corevideo::core::SenderLifecyclePolicy;
using corevideo::core::SenderObservation;

ActiveOutputObservation producing(int64_t nowMs, int64_t lastProgressMs) {
  ActiveOutputObservation observation;
  observation.startApplied = true;
  observation.everProgressed = true;
  observation.lastProgressMs = lastProgressMs;
  observation.nowMs = nowMs;
  return observation;
}

// THE defect this policy exists for: an accepted Start is an acknowledgement.
TEST(OutputLifecyclePolicy, AnAcceptedRequestIsNeverProducing) {
  ActiveOutputObservation observation;
  observation.nowMs = 10'000;
  const auto decision = OutputLifecyclePolicy::evaluateActive(observation);
  EXPECT_EQ(decision.state, "requested");
  EXPECT_EQ(decision.health, "unknown");
  EXPECT_FALSE(OutputLifecyclePolicy::claimsCompletion(decision.state));
}

TEST(OutputLifecyclePolicy, AnOpenedWriterWithNoOutputYetIsOnlyPreparing) {
  ActiveOutputObservation observation;
  observation.startApplied = true;
  observation.nowMs = 10'000;
  EXPECT_EQ(OutputLifecyclePolicy::evaluateActive(observation).state, "preparing");
}

TEST(OutputLifecyclePolicy, FreshProgressIsWhatMakesADestinationProducing) {
  const auto decision = OutputLifecyclePolicy::evaluateActive(producing(10'000, 9'800));
  EXPECT_EQ(decision.state, "producing");
  EXPECT_EQ(decision.health, "healthy");
}

TEST(OutputLifecyclePolicy, ProgressExactlyAtTheBudgetIsStillFresh) {
  EXPECT_EQ(OutputLifecyclePolicy::evaluateActive(
                producing(10'000, 10'000 - kProducingProgressStaleMs)).state,
            "producing");
}

// The wedged-writer case, decided without a writer.
TEST(OutputLifecyclePolicy, ProducingDecaysToInterruptedWhenProgressGoesStale) {
  const auto decision = OutputLifecyclePolicy::evaluateActive(
      producing(10'000, 10'000 - kProducingProgressStaleMs - 1));
  EXPECT_EQ(decision.state, "interrupted");
  EXPECT_EQ(decision.health, "degraded");
}

TEST(OutputLifecyclePolicy, ADecayedDestinationReturnsToProducingWhenProgressResumes) {
  auto observation = producing(10'000, 5'000);
  EXPECT_EQ(OutputLifecyclePolicy::evaluateActive(observation).state, "interrupted");
  observation.lastProgressMs = 9'999;
  EXPECT_EQ(OutputLifecyclePolicy::evaluateActive(observation).state, "producing");
}

TEST(OutputLifecyclePolicy, AWarningDegradesHealthWithoutFakingTheState) {
  auto observation = producing(10'000, 9'900);
  observation.degraded = true;
  const auto decision = OutputLifecyclePolicy::evaluateActive(observation);
  EXPECT_EQ(decision.state, "producing");
  EXPECT_EQ(decision.health, "degraded");
}

TEST(OutputLifecyclePolicy, ABackwardClockNeverAccusesAHealthyDestination) {
  EXPECT_EQ(OutputLifecyclePolicy::evaluateActive(producing(10'000, 10'500)).state, "producing");
}

TEST(OutputLifecyclePolicy, StopBeginsWithoutClaimingCompletion) {
  EXPECT_EQ(OutputLifecyclePolicy::stopping().state, "stopping");
  EXPECT_EQ(OutputLifecyclePolicy::finalizing().state, "finalizing");
  EXPECT_FALSE(OutputLifecyclePolicy::claimsCompletion(OutputLifecyclePolicy::stopping().state));
  EXPECT_FALSE(OutputLifecyclePolicy::claimsCompletion(OutputLifecyclePolicy::finalizing().state));
  EXPECT_FALSE(OutputLifecyclePolicy::isTerminal(OutputLifecyclePolicy::stopping().state));
  EXPECT_FALSE(OutputLifecyclePolicy::isTerminal(OutputLifecyclePolicy::finalizing().state));
}

TEST(OutputLifecyclePolicy, OnlyAFinalizeThatWroteMediaCompletes) {
  EXPECT_EQ(OutputLifecyclePolicy::finalized(true).state, "completed");
  EXPECT_EQ(OutputLifecyclePolicy::finalized(false).state, "failed");
  EXPECT_EQ(OutputLifecyclePolicy::finalized(false).health, "failed");
}

// The published legacy fields are projections, so they cannot contradict the
// lifecycle the way `recordingStatus_ = "stopped"` used to contradict it for
// the whole finalize window.
TEST(OutputLifecyclePolicy, PublishedStatusNeverReportsStoppedBeforeCompletion) {
  EXPECT_EQ(publishedRecordingStatus("requested", "unknown"), "starting");
  EXPECT_EQ(publishedRecordingStatus("preparing", "unknown"), "starting");
  EXPECT_EQ(publishedRecordingStatus("producing", "healthy"), "recording");
  EXPECT_EQ(publishedRecordingStatus("producing", "degraded"), "warning");
  EXPECT_EQ(publishedRecordingStatus("stopping", "unknown"), "stopping");
  EXPECT_EQ(publishedRecordingStatus("finalizing", "unknown"), "stopping");
  EXPECT_EQ(publishedRecordingStatus("completed", "healthy"), "stopped");
  EXPECT_EQ(publishedRecordingStatus("failed", "failed"), "failed");
  EXPECT_EQ(publishedRecordingStatus("interrupted", "degraded"), "interrupted");
  for (const char* state : {"requested", "preparing", "producing", "stopping", "finalizing",
                            "interrupted", "failed"})
    EXPECT_NE(publishedRecordingStatus(state, "unknown"), "stopped") << state;
}

TEST(OutputLifecyclePolicy, PublishedWriterStatusNamesTheFinalizeWindow) {
  EXPECT_EQ(publishedRecordingWriterStatus("stopping"), "finalizing");
  EXPECT_EQ(publishedRecordingWriterStatus("finalizing"), "finalizing");
  EXPECT_EQ(publishedRecordingWriterStatus("producing"), "writing");
  EXPECT_EQ(publishedRecordingWriterStatus("interrupted"), "stalled");
  EXPECT_EQ(publishedRecordingWriterStatus("completed"), "stopped");
}

// The retired names remain readable so a newer consumer can read an older core.
TEST(OutputLifecyclePolicy, LegacyStateNamesStillProject) {
  EXPECT_EQ(publishedRecordingStatus("live", "healthy"), "recording");
  EXPECT_EQ(publishedRecordingStatus("starting", "unknown"), "starting");
  EXPECT_EQ(publishedRecordingWriterStatus("live"), "writing");
}

SenderObservation liveSender(int64_t nowMs, int64_t lastProgressMs) {
  SenderObservation observation;
  observation.status = "live";
  observation.desiredActive = true;
  observation.framesSent = 100;
  observation.everProduced = true;
  observation.lastProgressMs = lastProgressMs;
  observation.nowMs = nowMs;
  return observation;
}

TEST(SenderLifecyclePolicy, AConfiguredSenderWithNoFramesIsPreparingNotProducing) {
  SenderObservation observation;
  observation.status = "starting";
  observation.desiredActive = true;
  observation.nowMs = 10'000;
  EXPECT_EQ(SenderLifecyclePolicy::evaluate(observation).state, "preparing");
}

TEST(SenderLifecyclePolicy, ASendingDestinationIsProducingOnlyWhileFramesKeepLanding) {
  EXPECT_EQ(SenderLifecyclePolicy::evaluate(liveSender(10'000, 9'900)).state, "producing");
  // The stream died mid-show. The adapter still says "live"; the lifecycle does not.
  const auto stalled = SenderLifecyclePolicy::evaluate(liveSender(20'000, 9'900));
  EXPECT_EQ(stalled.state, "interrupted");
  EXPECT_EQ(stalled.health, "degraded");
}

TEST(SenderLifecyclePolicy, AFailedSenderIsTerminalAndNeverFinalized) {
  auto observation = liveSender(10'000, 9'900);
  observation.status = "failed";
  const auto decision = SenderLifecyclePolicy::evaluate(observation);
  EXPECT_EQ(decision.state, "failed");
  EXPECT_EQ(decision.health, "failed");
  EXPECT_FALSE(SenderLifecyclePolicy::finalizedFor(decision, true));
}

// `lastError` is sticky history, not current state: the SRT/RTMP adapters keep
// their first-tick "waiting for composed BGRA program pixels" string on a sender
// that is genuinely streaming. Rig-observed on this very change.
TEST(SenderLifecyclePolicy, AStickyLastErrorDoesNotFailAProducingDestination) {
  auto observation = liveSender(10'000, 9'900);
  observation.hasError = true;
  observation.destinationHealth = "ok";
  EXPECT_EQ(SenderLifecyclePolicy::evaluate(observation).state, "producing");
}

TEST(SenderLifecyclePolicy, AFailedDestinationHealthFailsTheDestination) {
  auto observation = liveSender(10'000, 9'900);
  observation.destinationHealth = "failed";
  EXPECT_EQ(SenderLifecyclePolicy::evaluate(observation).state, "failed");
}

TEST(SenderLifecyclePolicy, AStoppedSenderCompletesOnlyIfItEverSent) {
  auto observation = liveSender(10'000, 9'900);
  observation.status = "stopped";
  const auto sent = SenderLifecyclePolicy::evaluate(observation);
  EXPECT_EQ(sent.state, "completed");
  EXPECT_TRUE(SenderLifecyclePolicy::finalizedFor(sent, true));

  observation.everProduced = false;
  observation.framesSent = 0;
  const auto never = SenderLifecyclePolicy::evaluate(observation);
  EXPECT_EQ(never.state, "completed");
  EXPECT_EQ(never.health, "unknown");
  EXPECT_FALSE(SenderLifecyclePolicy::finalizedFor(never, false));
}

TEST(SenderLifecyclePolicy, AWarningWhileSendingDegradesButKeepsProducing) {
  auto observation = liveSender(10'000, 9'900);
  observation.status = "warning";
  const auto decision = SenderLifecyclePolicy::evaluate(observation);
  EXPECT_EQ(decision.state, "producing");
  EXPECT_EQ(decision.health, "degraded");
}

TEST(SenderLifecyclePolicy, ADegradedDestinationHealthDegradesWithoutFailing) {
  auto observation = liveSender(10'000, 9'900);
  observation.destinationHealth = "warning";
  const auto decision = SenderLifecyclePolicy::evaluate(observation);
  EXPECT_EQ(decision.state, "producing");
  EXPECT_EQ(decision.health, "degraded");
}

TEST(SenderLifecyclePolicy, AnUnconfiguredDestinationIsIdle) {
  SenderObservation observation;
  observation.status = "idle";
  observation.nowMs = 10'000;
  EXPECT_EQ(SenderLifecyclePolicy::evaluate(observation).state, "idle");
}

}  // namespace

// #468 / T2.10. Live 2026-09-10: RTMP pointed at a listener that had died, the
// egress sat in SYN_SENT, framesSent stuck at 8-10 for 20+ seconds - and the
// sender reported status "live", destinationHealth "ok" and lastResultCode
// "encoder-input-accepted" the whole time. The lifecycle DID cycle
// producing -> interrupted and the supervisor did restart it, so recovery
// worked; what never showed a problem is the pair an operator actually reads.
//
// The published fields are PROJECTIONS now, exactly like publishedRecordingStatus,
// so the adapter's last launch state can no longer contradict the evidence.
TEST(OutputLifecyclePolicy, AnUnreachableDestinationCannotPublishAHealthyStream) {
  corevideo::modules::OutputSupervisorState supervisor;
  supervisor.healthy = false;
  supervisor.restarts = 3;

  // What the adapter said on 2026-09-10, verbatim.
  EXPECT_NE(corevideo::core::publishedSenderStatus("live", "interrupted", supervisor), "live");
  EXPECT_NE(corevideo::core::publishedSenderDestinationHealth("ok", "interrupted", supervisor), "ok");
}

// The opposite error is just as bad. A genuinely streaming sender carries its
// first-tick lastError forever ("waiting for composed BGRA program pixels"),
// and CLAUDE.md is explicit that treating that as failure reports every live
// stream as broken. Health follows the lifecycle and the supervisor, nothing else.
TEST(OutputLifecyclePolicy, AProducingSenderStaysHealthyWhateverItsStickyError) {
  corevideo::modules::OutputSupervisorState supervisor;
  supervisor.healthy = true;
  EXPECT_EQ(corevideo::core::publishedSenderStatus("live", "producing", supervisor), "live");
  EXPECT_EQ(corevideo::core::publishedSenderDestinationHealth("ok", "producing", supervisor), "ok");
}

// Give-up is LOUD. The supervisor already rewrites the record to failed; the
// projection must agree rather than depend on that rewrite having happened.
TEST(OutputLifecyclePolicy, AGivenUpDestinationPublishesFailed) {
  corevideo::modules::OutputSupervisorState supervisor;
  supervisor.gaveUp = true;
  supervisor.failureClass = "terminal";
  EXPECT_EQ(corevideo::core::publishedSenderStatus("live", "producing", supervisor), "failed");
  EXPECT_EQ(corevideo::core::publishedSenderDestinationHealth("ok", "producing", supervisor), "failed");
}

// A sender with no supervisor (unit tests, the synthetic sender) must keep the
// adapter's own words: inventing health for a destination nothing is watching
// would be the same lie in the other direction.
TEST(OutputLifecyclePolicy, WithoutSupervisionTheAdapterStateIsKept) {
  EXPECT_EQ(corevideo::core::publishedSenderStatus("live", "producing", std::nullopt), "live");
  EXPECT_EQ(corevideo::core::publishedSenderDestinationHealth("ok", "", std::nullopt), "ok");
  EXPECT_EQ(corevideo::core::publishedSenderStatus("stopped", "", std::nullopt), "stopped");
}
