// PR19 exit gate: "Hang, crash, malformed reply, IPC disconnect, stale
// completion, and restart cannot block supervisor/control or affect siblings."
//
// IsolatedOutputSenderTest proves exactly one of those (a blocked destination
// does not block a sibling's video and audio). This file extends that shape to
// the other five, plus the ladder and the health rule themselves.
//
// The fault host is deterministic and in-process: FakeDestinationChild can hang,
// crash, freeze its counters while still claiming to be live, answer with a
// malformed record, or report an inadmissible configuration. Timing is an
// injected clock, so the 5/10/20/40/60 s ladder is exercised in microseconds and
// nothing here sleeps.

#include "modules/AsyncOutputSender.h"
#include "modules/OutputDestinationSupervisor.h"
#include "modules/OutputDestinationSupervisorPolicy.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace {
using namespace corevideo::modules;

// ---------------------------------------------------------------------------
// The deterministic fault host
// ---------------------------------------------------------------------------
class FakeDestinationChild final : public IOutputSender {
 public:
  explicit FakeDestinationChild(std::string destination) {
    record_.destination = destination;
    record_.senderId = destination + ":program";
    record_.status = "starting";
    record_.destinationHealth = "starting";
    record_.lastResultCode = "waiting-for-frame";
  }

  OutputSenderSession sync(const std::vector<std::string>&, const ProgramFrame*, double,
                           const std::vector<OutputDestinationSettings>&, const std::vector<float>*, int,
                           int) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++syncs_;
    return sessionLocked();
  }
  void submitAudio(const std::vector<float>&, int, int) override {}
  OutputSenderSession fail(const std::string&, const std::string&, double) override { return session(); }
  OutputSenderSession recover(const std::string&, double, const std::string&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++recovers_;
    actionThread_ = std::this_thread::get_id();
    // A real adapter's recover() re-opens the transport: fresh counters, no error.
    record_.status = "starting";
    record_.destinationHealth = "starting";
    record_.lastResultCode = "recovered";
    record_.lastError.clear();
    record_.warning.clear();
    record_.framesSent = 0;
    record_.audioFramesSent = 0;
    return sessionLocked();
  }
  OutputSenderSession session() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessionLocked();
  }
  void interrupt(const std::string&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++interrupts_;
    actionThread_ = std::this_thread::get_id();
  }

  // --- fault controls -----------------------------------------------------
  void produce(int64_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    record_.framesSent += frames;
    record_.status = "live";
    record_.destinationHealth = "ok";
    record_.lastResultCode = "encoder-input-accepted";
  }
  // A crashed FFmpeg child: the adapter notices the exit and reports it.
  void crash(std::string code, std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    record_.status = "failed";
    record_.destinationHealth = "failed";
    record_.lastResultCode = std::move(code);
    record_.lastError = std::move(error);
  }
  // An inadmissible configuration: the destination will reject it again.
  void rejectConfiguration(std::string code, std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    record_.status = "warning";
    record_.destinationHealth = "warning";
    record_.lastResultCode = std::move(code);
    record_.lastError = std::move(error);
  }
  // An IPC disconnect: still claims to be live, counters stop moving.
  void disconnectSilently() {
    std::lock_guard<std::mutex> lock(mutex_);
    record_.status = "live";
    record_.destinationHealth = "ok";
    record_.lastResultCode = "encoder-input-accepted";
    // framesSent deliberately frozen.
  }
  // A malformed reply that nothing should reason about.
  void answerMalformed() {
    std::lock_guard<std::mutex> lock(mutex_);
    record_.framesSent = -1;
    record_.audioFramesSent = -1;
  }

  int recovers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recovers_;
  }
  int interrupts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return interrupts_;
  }
  std::thread::id actionThread() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return actionThread_;
  }

 private:
  OutputSenderSession sessionLocked() const {
    OutputSenderSession session;
    session.senders.push_back(record_);
    session.activeSenderCount = 1;
    session.status = record_.status;
    return session;
  }

  mutable std::mutex mutex_;
  OutputSender record_;
  int syncs_ = 0;
  int recovers_ = 0;
  int interrupts_ = 0;
  std::thread::id actionThread_{};
};

// A child that blocks inside sync() until it is interrupted — the hang case.
class HangingChild final : public IOutputSender {
 public:
  OutputSenderSession sync(const std::vector<std::string>&, const ProgramFrame*, double,
                           const std::vector<OutputDestinationSettings>&, const std::vector<float>*, int,
                           int) override {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [&] { return released_; });
    return {};
  }
  void submitAudio(const std::vector<float>&, int, int) override {}
  OutputSenderSession fail(const std::string&, const std::string&, double) override { return {}; }
  OutputSenderSession recover(const std::string&, double, const std::string&) override { return {}; }
  OutputSenderSession session() const override { return {}; }
  void interrupt(const std::string& destination) override {
    // Only its OWN destination releases it. AsyncOutputSender::sync interrupts
    // the destinations that are NOT selected on every call, so a name-agnostic
    // release here would unwedge the hang the test exists to create.
    if (destination != "rtmp") return;
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    cv_.notify_all();
  }
  bool awaitEntered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, std::chrono::seconds(2), [&] { return entered_; });
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool entered_ = false;
  bool released_ = false;
};

DestinationObservation activeObservation(std::int64_t nowMs, std::int64_t units, const char* status,
                                         const char* code) {
  DestinationObservation observation;
  observation.desiredActive = true;
  observation.observed = true;
  observation.acceptedUnits = units;
  observation.status = status;
  observation.destinationHealth = std::string(status) == "failed" ? "failed" : "ok";
  observation.lastResultCode = code;
  observation.nowMs = nowMs;
  return observation;
}

struct SupervisorFixture {
  std::int64_t now = 0;
  FakeDestinationChild* child = nullptr;
  std::unique_ptr<SupervisedOutputSender> sender;

  explicit SupervisorFixture(const std::string& destination = "rtmp") {
    auto owned = std::make_unique<FakeDestinationChild>(destination);
    child = owned.get();
    SupervisedOutputSender::Options options;
    options.clock = [this] { return now; };
    options.startThread = false;  // deterministic: evaluation happens in pumpForTest()
    sender = std::make_unique<SupervisedOutputSender>(std::move(owned), options);
  }

  DestinationSupervisorReport reportFor(const std::string& destination) const {
    for (const auto& entry : sender->report()) {
      if (entry.destination == destination) return entry;
    }
    return {};
  }
};

TEST(OutputDestinationSupervisor, ExplicitRearmRetiresTerminalAsyncObservation) {
  auto owned = std::make_unique<FakeDestinationChild>("rtmp");
  auto* child = owned.get();
  child->rejectConfiguration("codec-not-deliverable", "AV1 refused");
  auto async = std::make_unique<AsyncOutputSender>(std::move(owned));
  auto* writer = async.get();
  SupervisedOutputSender::Options options;
  options.startThread = false;
  std::int64_t now = 0;
  options.clock = [&] { return now; };
  SupervisedOutputSender sender(std::move(async), options);
  sender.sync({"rtmp"}, nullptr, 0);
  ASSERT_TRUE(writer->drainForTest(std::chrono::seconds(2)));
  sender.pumpForTest();
  ASSERT_TRUE(sender.report().front().gaveUp);
  sender.sync({}, nullptr, 20);
  ASSERT_TRUE(writer->drainForTest(std::chrono::seconds(2)));
  sender.sync({"rtmp"}, nullptr, 40);
  sender.pumpForTest();
  EXPECT_FALSE(sender.report().front().gaveUp);
  ASSERT_TRUE(writer->drainForTest(std::chrono::seconds(2)));
  sender.pumpForTest();
  EXPECT_FALSE(sender.report().front().gaveUp);
  EXPECT_EQ(child->recovers(), 1);
  child->produce(10);
  sender.sync({"rtmp"}, nullptr, 60);
  ASSERT_TRUE(writer->drainForTest(std::chrono::seconds(2)));
  sender.pumpForTest();
  EXPECT_TRUE(sender.report().front().healthy);
  EXPECT_EQ(child->recovers(), 1);  // repeated desired state is not a reset
  child->rejectConfiguration("codec-not-deliverable", "new refusal");
  sender.sync({"rtmp"}, nullptr, 80);
  ASSERT_TRUE(writer->drainForTest(std::chrono::seconds(2)));
  sender.pumpForTest();
  EXPECT_TRUE(sender.report().front().gaveUp);
}

}  // namespace

// ===========================================================================
// The pure policy: the ladder, the health rule, and terminal failures
// ===========================================================================

TEST(OutputDestinationSupervisorPolicy, AdoptsTheHouseLadderRungForRung) {
  using P = OutputDestinationSupervisorPolicy;
  EXPECT_EQ(P::backoffMsForFailureCount(0), 5000);
  EXPECT_EQ(P::backoffMsForFailureCount(1), 10000);
  EXPECT_EQ(P::backoffMsForFailureCount(2), 20000);
  EXPECT_EQ(P::backoffMsForFailureCount(3), 40000);
  EXPECT_EQ(P::backoffMsForFailureCount(4), 60000);
  EXPECT_EQ(P::backoffMsForFailureCount(9), 60000);
  EXPECT_EQ(P::kMaxConsecutiveFailures, 5);
}

TEST(OutputDestinationSupervisorPolicy, HealthIsAcceptedProgressNotALaunch) {
  OutputDestinationSupervisorPolicy policy;
  policy.onGenerationStarted(0);

  // A destination that is up, connected and reporting "live" but has accepted
  // nothing is NOT healthy. This is Rule 7 at the exact point it was ignored.
  auto decision = policy.observe(activeObservation(100, 0, "live", "ok"));
  EXPECT_FALSE(decision.healthy);

  decision = policy.observe(activeObservation(200, 1, "live", "encoder-input-accepted"));
  EXPECT_TRUE(decision.healthy);

  // The SAME counter one second later is no longer fresh evidence.
  decision = policy.observe(activeObservation(1400, 1, "live", "encoder-input-accepted"));
  EXPECT_FALSE(decision.healthy);
  EXPECT_EQ(decision.action, SupervisorAction::None);  // decayed, but not yet acted on
}

TEST(OutputDestinationSupervisorPolicy, AStalledDestinationIsRestartedAtFiveSecondsNotAtOne) {
  OutputDestinationSupervisorPolicy policy;
  policy.onGenerationStarted(0);
  policy.observe(activeObservation(100, 5, "live", "encoder-input-accepted"));

  // Health decays at one second, which is what the truthful lifecycle reports.
  EXPECT_FALSE(policy.observe(activeObservation(2000, 5, "live", "encoder-input-accepted")).healthy);
  EXPECT_FALSE(policy.faultPending());

  // Acting on it waits for the restart budget — restarting an encoder over a
  // 1.2 s hiccup costs a reconnect and a keyframe for nothing.
  auto decision = policy.observe(activeObservation(6000, 5, "live", "encoder-input-accepted"));
  EXPECT_TRUE(policy.faultPending());
  EXPECT_EQ(decision.action, SupervisorAction::None);  // backoff not elapsed
  decision = policy.observe(activeObservation(6000 + 5000, 5, "live", "encoder-input-accepted"));
  EXPECT_EQ(decision.action, SupervisorAction::Restart);
}

TEST(OutputDestinationSupervisorPolicy, ADestinationThatNeverComesUpIsAFault) {
  OutputDestinationSupervisorPolicy policy;
  policy.onGenerationStarted(0);
  policy.observe(activeObservation(1000, 0, "starting", "waiting-for-frame"));
  EXPECT_FALSE(policy.faultPending());
  policy.observe(activeObservation(16000, 0, "starting", "waiting-for-frame"));
  EXPECT_TRUE(policy.faultPending());
}

TEST(OutputDestinationSupervisorPolicy, AHealthyInstantDoesNotResetTheBudgetButAHealthyRunDoes) {
  OutputDestinationSupervisorPolicy policy;
  policy.onGenerationStarted(0);
  policy.observe(activeObservation(10, 0, "failed", "ffmpeg-exited"));
  EXPECT_EQ(policy.consecutiveFailures(), 1);

  policy.observe(activeObservation(5010, 0, "starting", "recovered"));  // authorizes the restart
  policy.onGenerationStarted(5010);

  // Producing immediately is a healthy INSTANT: the streak survives it.
  policy.observe(activeObservation(5100, 1, "live", "encoder-input-accepted"));
  EXPECT_EQ(policy.consecutiveFailures(), 1);

  // Producing 30 s into the run is a healthy RUN: the budget comes back.
  policy.observe(activeObservation(5010 + 30000, 2, "live", "encoder-input-accepted"));
  EXPECT_EQ(policy.consecutiveFailures(), 0);
}

TEST(OutputDestinationSupervisorPolicy, ATerminalConfigurationBypassesTheLadderEntirely) {
  OutputDestinationSupervisorPolicy policy;
  policy.onGenerationStarted(0);
  DestinationObservation observation = activeObservation(100, 0, "warning", "source-name-invalid");
  observation.destinationHealth = "warning";
  observation.lastError = "NDI source name rejected";

  const auto decision = policy.observe(observation);
  EXPECT_EQ(decision.action, SupervisorAction::GiveUp);
  EXPECT_EQ(decision.failureClass, DestinationFailureClass::Terminal);
  EXPECT_TRUE(policy.gaveUp());
  // Not five retries at escalating cost for a configuration that will be
  // rejected five times: zero restarts, and the streak was never burned.
  EXPECT_EQ(policy.restarts(), 0);
  EXPECT_EQ(policy.consecutiveFailures(), 0);
  EXPECT_NE(policy.giveUpReason().find("rejected again"), std::string::npos);

  policy.reset(200);
  EXPECT_FALSE(policy.gaveUp());
}

TEST(OutputDestinationSupervisorPolicy, ARetryableFailureClimbsTheLadderAndGivesUpAfterFive) {
  OutputDestinationSupervisorPolicy policy;
  policy.onGenerationStarted(0);
  std::int64_t now = 0;
  int restarts = 0;
  for (int attempt = 0; attempt < 8 && !policy.gaveUp(); ++attempt) {
    now += 10;
    policy.observe(activeObservation(now, 0, "failed", "ffmpeg-exited"));
    now += 120000;  // past the tallest rung
    const auto decision = policy.observe(activeObservation(now, 0, "failed", "ffmpeg-exited"));
    if (decision.action == SupervisorAction::Restart) {
      ++restarts;
      policy.onGenerationStarted(now);
    }
  }
  EXPECT_TRUE(policy.gaveUp());
  EXPECT_EQ(restarts, OutputDestinationSupervisorPolicy::kMaxConsecutiveFailures);
  EXPECT_NE(policy.giveUpReason().find("Re-arm"), std::string::npos);
}

TEST(OutputDestinationSupervisorPolicy, AnUnknownFailureCodeIsRetryableNotTerminal) {
  // Guessing "terminal" would silently stop protecting a destination.
  EXPECT_FALSE(isTerminalResultCode("ffmpeg-exited"));
  EXPECT_FALSE(isTerminalResultCode("something-we-have-never-seen"));
  EXPECT_TRUE(isTerminalResultCode("endpoint-missing"));
  EXPECT_TRUE(isTerminalResultCode("rtmp-settings-invalid"));
  EXPECT_TRUE(isTerminalResultCode("source-name-invalid"));
  EXPECT_TRUE(isTerminalResultCode("runtime-missing"));
  EXPECT_TRUE(isTerminalResultCode("ndi-output-unavailable"));
  // The stream-start admission codes: the two that are settings the operator
  // must change are terminal; a GPU encoder start fault can be transient.
  EXPECT_TRUE(isTerminalResultCode("enhanced-rtmp-required"));
  EXPECT_TRUE(isTerminalResultCode("no-hardware-encoder"));
  EXPECT_FALSE(isTerminalResultCode("gpu-encoder-start-failed"));
  // 2026-09-20: a codec whose hardware encoder starts and runs but produces a
  // stream that is not deliverable (AV1's near-empty access units) is an
  // inadmissible CONFIGURATION, not a transient fault - retrying re-decides it
  // identically forever, so it must bypass the ladder like its two siblings.
  EXPECT_TRUE(isTerminalResultCode("codec-not-deliverable"));
}

// ===========================================================================
// The supervisor: generations, restarts, and fault isolation
// ===========================================================================

TEST(OutputDestinationSupervisor, ACrashedDestinationIsRestartedOnTheLadderAndTheGenerationAdvances) {
  SupervisorFixture fixture;
  ProgramFrame frame;
  frame.frameNumber = 1;
  fixture.sender->sync({"rtmp"}, &frame, 0);
  fixture.child->produce(10);
  fixture.now = 100;
  fixture.sender->pumpForTest();
  EXPECT_TRUE(fixture.reportFor("rtmp").healthy);
  EXPECT_EQ(fixture.reportFor("rtmp").generation, 1u);

  fixture.child->crash("ffmpeg-exited", "FFmpeg exited with code 1");
  fixture.now = 200;
  fixture.sender->pumpForTest();
  EXPECT_EQ(fixture.child->recovers(), 0);  // backoff, not an instant retry loop
  EXPECT_EQ(fixture.reportFor("rtmp").consecutiveFailures, 1);
  EXPECT_EQ(fixture.reportFor("rtmp").failureClass, "retryable");
  EXPECT_GT(fixture.reportFor("rtmp").nextAttemptInMs, 0);

  fixture.now = 200 + 5000;
  fixture.sender->pumpForTest();
  EXPECT_EQ(fixture.child->recovers(), 1);
  EXPECT_EQ(fixture.child->interrupts(), 1);  // interrupt-then-recover
  EXPECT_EQ(fixture.reportFor("rtmp").generation, 2u);
  EXPECT_EQ(fixture.reportFor("rtmp").restarts, 1);
}

TEST(OutputDestinationSupervisor, AnIpcDisconnectDecaysHealthAndIsRestarted) {
  // The destination never says anything is wrong: it keeps reporting "live"
  // while its counters stop. Only fresh evidence catches this.
  SupervisorFixture fixture;
  ProgramFrame frame;
  frame.frameNumber = 1;
  fixture.sender->sync({"rtmp"}, &frame, 0);
  fixture.child->produce(30);
  fixture.now = 100;
  fixture.sender->pumpForTest();
  ASSERT_TRUE(fixture.reportFor("rtmp").healthy);

  fixture.child->disconnectSilently();
  fixture.now = 1500;
  fixture.sender->pumpForTest();
  EXPECT_FALSE(fixture.reportFor("rtmp").healthy);  // decayed on evidence, not on a status string
  EXPECT_EQ(fixture.child->recovers(), 0);

  fixture.now = 7000;
  fixture.sender->pumpForTest();
  fixture.now = 7000 + 5000;
  fixture.sender->pumpForTest();
  EXPECT_EQ(fixture.child->recovers(), 1);
  EXPECT_EQ(fixture.reportFor("rtmp").generation, 2u);
}

TEST(OutputDestinationSupervisor, AMalformedReplyNeverEstablishesHealthAndIsCounted) {
  SupervisorFixture fixture;
  ProgramFrame frame;
  frame.frameNumber = 1;
  fixture.sender->sync({"rtmp"}, &frame, 0);
  fixture.child->answerMalformed();
  fixture.now = 100;
  fixture.sender->pumpForTest();

  const auto report = fixture.reportFor("rtmp");
  EXPECT_FALSE(report.healthy);
  EXPECT_EQ(report.acceptedUnits, 0);
  EXPECT_GE(report.malformedObservations, 1);
  EXPECT_EQ(report.lastProgressAgeMs, -1);  // nothing has ever been accepted
}

TEST(OutputDestinationSupervisor, AStaleCompletionFromARetiredChildIsRejectedByIdentity) {
  SupervisorFixture fixture;
  ProgramFrame frame;
  frame.frameNumber = 1;
  fixture.sender->sync({"rtmp"}, &frame, 0);

  const auto first = fixture.sender->currentGeneration("rtmp");
  ASSERT_TRUE(first.token != nullptr);
  DestinationEventStamp stamp{"rtmp", first.number, first.token};
  EXPECT_TRUE(fixture.sender->acceptEvent(stamp));

  // Two operator re-arms retire the run twice.
  fixture.sender->recover("rtmp", 10, "operator");
  fixture.sender->recover("rtmp", 20, "operator");
  const auto current = fixture.sender->currentGeneration("rtmp");
  EXPECT_NE(current.number, first.number);

  // Number-only guard: this stamp carries the CURRENT number but the RETIRED
  // object. It must still be rejected — the reason ShowEngineSupervisor checks
  // identity rather than a counter.
  DestinationEventStamp forged{"rtmp", current.number, first.token};
  EXPECT_FALSE(fixture.sender->acceptEvent(forged));

  EXPECT_FALSE(fixture.sender->acceptEvent(stamp));  // stale number AND stale object
  EXPECT_TRUE(fixture.sender->acceptEvent(DestinationEventStamp{"rtmp", current.number, current.token}));
  EXPECT_FALSE(fixture.sender->acceptEvent(DestinationEventStamp{"never-configured", 1, current.token}));
}

TEST(OutputDestinationSupervisor, ATerminalFailureIsPublishedLoudlyAndNeverRetried) {
  SupervisorFixture fixture("ndi");
  ProgramFrame frame;
  frame.frameNumber = 1;
  fixture.sender->sync({"ndi"}, &frame, 0);
  fixture.child->rejectConfiguration("source-name-invalid", "NDI source name rejected: \"a/b\"");
  fixture.now = 50;
  fixture.sender->pumpForTest();

  const auto report = fixture.reportFor("ndi");
  EXPECT_TRUE(report.gaveUp);
  EXPECT_EQ(report.failureClass, "terminal");
  EXPECT_EQ(report.restarts, 0);
  EXPECT_EQ(fixture.child->recovers(), 0);

  // Time passing must not turn a give-up into a retry loop.
  fixture.now = 10 * 60 * 1000;
  fixture.sender->pumpForTest();
  EXPECT_EQ(fixture.child->recovers(), 0);

  // And it must read as failed everywhere downstream, not as a private opinion.
  const auto session = fixture.sender->session();
  ASSERT_FALSE(session.senders.empty());
  EXPECT_EQ(session.senders.front().status, "failed");
  EXPECT_EQ(session.senders.front().lastResultCode, "supervisor-gave-up");
  ASSERT_TRUE(session.senders.front().supervisor.has_value());
  EXPECT_TRUE(session.senders.front().supervisor->gaveUp);
  EXPECT_FALSE(session.senders.front().supervisor->reason.empty());

  // The operator reset is always available.
  fixture.sender->recover("ndi", 100, "operator re-armed");
  EXPECT_FALSE(fixture.reportFor("ndi").gaveUp);
}

TEST(OutputDestinationSupervisor, PublishesWhichDestinationsAreActuallyIsolated) {
  // NDI's in-process risk is VISIBLE rather than an implementation detail, which
  // is the whole of what PR19 can honestly claim for it.
  EXPECT_FALSE(destinationIsolationTraits("rtmp").inProcess);
  EXPECT_TRUE(destinationIsolationTraits("rtmp").interruptible);
  EXPECT_FALSE(destinationIsolationTraits("srt").inProcess);
  EXPECT_TRUE(destinationIsolationTraits("ndi").interruptible == false);
  EXPECT_TRUE(destinationIsolationTraits("ndi").inProcess);
  // An unknown destination is assumed unprotected, never assumed contained.
  EXPECT_TRUE(destinationIsolationTraits("whatever-ships-next").inProcess);

  SupervisorFixture fixture("ndi");
  ProgramFrame frame;
  frame.frameNumber = 1;
  const auto session = fixture.sender->sync({"ndi"}, &frame, 0);
  ASSERT_FALSE(session.senders.empty());
  ASSERT_TRUE(session.senders.front().supervisor.has_value());
  EXPECT_TRUE(session.senders.front().supervisor->inProcessRisk);
  EXPECT_FALSE(session.senders.front().supervisor->interruptible);
}

TEST(OutputDestinationSupervisor, AHungDestinationBlocksNeitherTheSupervisorNorAHealthySibling) {
  // The hang is inside the child's transport call, behind the per-protocol
  // async writer — exactly the production arrangement.
  auto hanging = std::make_unique<HangingChild>();
  auto* hangingProbe = hanging.get();
  SupervisedOutputSender::Options options;
  options.startThread = false;
  SupervisedOutputSender hungDestination(std::make_unique<AsyncOutputSender>(std::move(hanging)), options);

  SupervisorFixture healthy("srt");

  ProgramFrame frame;
  frame.frameNumber = 1;
  hungDestination.sync({"rtmp"}, &frame, 0);
  ASSERT_TRUE(hangingProbe->awaitEntered());

  const auto start = std::chrono::steady_clock::now();
  for (int tick = 0; tick < 20; ++tick) {
    // The control path: a sync, a state read, and a supervisor evaluation, all
    // while the destination's writer is wedged in its transport call.
    hungDestination.sync({"rtmp"}, &frame, static_cast<double>(tick));
    hungDestination.session();
    hungDestination.pumpForTest();
    hungDestination.report();
    // The sibling keeps producing on its own writer and its own supervisor.
    healthy.sender->sync({"srt"}, &frame, static_cast<double>(tick));
    healthy.child->produce(1);
    healthy.now += 16;
    healthy.sender->pumpForTest();
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  EXPECT_LT(elapsed.count(), 2000) << "the wedged destination delayed the control path";
  EXPECT_TRUE(healthy.reportFor("srt").healthy);
  EXPECT_GE(healthy.reportFor("srt").acceptedUnits, 20);

  hungDestination.interrupt("rtmp");  // release the writer before teardown
}

TEST(OutputDestinationSupervisor, SupervisorActionsNeverRunOnTheCallersThread) {
  // The constraint: never spawn (or restart) on the render tick, and never under
  // coreMutex. A terminal failure is the fastest observable action, so it is the
  // one used to pin the thread.
  auto owned = std::make_unique<FakeDestinationChild>("rtmp");
  auto* child = owned.get();
  SupervisedOutputSender::Options options;
  options.tickInterval = std::chrono::milliseconds(5);
  options.startThread = true;
  SupervisedOutputSender sender(std::move(owned), options);

  ProgramFrame frame;
  frame.frameNumber = 1;
  sender.sync({"rtmp"}, &frame, 0);
  child->rejectConfiguration("endpoint-missing", "No RTMP endpoint is configured.");

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (child->interrupts() == 0 && std::chrono::steady_clock::now() < deadline) {
    sender.sync({"rtmp"}, &frame, 0);
    std::this_thread::yield();
  }
  ASSERT_GT(child->interrupts(), 0) << "the supervisor never acted";
  EXPECT_NE(child->actionThread(), std::this_thread::get_id());
}

// ---------------------------------------------------------------------------
// #597 — THE RESTART FLOOR
// ---------------------------------------------------------------------------
// Step 1 finding (2026-09-23), from the incident logs themselves: the eight
// encoder rebuilds between 21:09:10.48 and 21:09:28.49 were NOT the
// supervisor's. Only two `[outputSupervisor] restarting rtmp` lines exist in
// that window; the other six `[gpu-encode] started` lines have no supervisor
// decision behind them at all. They came from
// `RtmpOutputSenderAdapter::ensureFfmpegProcess`, which re-opens its own
// transport from the media tick under an adapter-local backoff whose first rung
// was ONE second (below the house ladder's five) and whose streak was cleared by
// the FIRST accepted frame. Overflow -> 1 s -> rebuild -> one accepted frame ->
// ~2 s -> overflow: the incident's 2.5-3.5 s cadence, measured off the log.
//
// The ladder was never "not applied" — it was never consulted.
//
// `kHealthyRunMs` (30 s) is ruled out explicitly and twice over: it cannot
// produce a 2.5-3.5 s interval at all, and the supervisor did not decide six of
// the eight rebuilds, so no supervisor budget could have been involved in them.

TEST(TransportRestartFloor, TheFloorIsTheHouseLadderAndTheFirstOpenIsNeverDelayed) {
  TransportRestartFloor floor;
  EXPECT_TRUE(floor.mayOpenAt(0)) << "a destination's FIRST open must never wait";

  floor.noteOpened(0);
  floor.noteFailure(1'000);
  EXPECT_FALSE(floor.mayOpenAt(1'000 + 4'999));
  EXPECT_TRUE(floor.mayOpenAt(1'000 + 5'000)) << "rung 1 is 5s, not the old 1s";

  floor.noteOpened(6'000);
  floor.noteFailure(7'000);
  EXPECT_FALSE(floor.mayOpenAt(7'000 + 9'999));
  EXPECT_TRUE(floor.mayOpenAt(7'000 + 10'000));

  floor.noteOpened(20'000);
  floor.noteFailure(21'000);
  EXPECT_TRUE(floor.mayOpenAt(21'000 + 20'000));
  floor.noteOpened(50'000);
  floor.noteFailure(51'000);
  EXPECT_TRUE(floor.mayOpenAt(51'000 + 40'000));
  floor.noteOpened(100'000);
  floor.noteFailure(101'000);
  EXPECT_TRUE(floor.mayOpenAt(101'000 + 60'000)) << "capped at 60s";
  EXPECT_FALSE(floor.mayOpenAt(101'000 + 59'999));
}

TEST(TransportRestartFloor, OneAcceptedFrameDoesNotReturnTheBudgetButAHealthyRunDoes) {
  TransportRestartFloor floor;
  floor.noteOpened(0);
  floor.noteFailure(1'000);
  EXPECT_EQ(floor.consecutiveFailures(), 1);

  // The incident's exact shape: the transport re-opens and accepts output, then
  // dies again well inside the healthy-run window.
  floor.noteOpened(6'000);
  floor.noteAccepted(6'100);
  floor.noteAccepted(8'000);
  EXPECT_EQ(floor.consecutiveFailures(), 1)
      << "a restart that produces one frame and dies must CLIMB the ladder";
  floor.noteFailure(8'500);
  EXPECT_FALSE(floor.mayOpenAt(8'500 + 9'999)) << "rung 2 is 10s";
  EXPECT_TRUE(floor.mayOpenAt(8'500 + 10'000));

  // A real healthy run returns the budget.
  floor.noteOpened(20'000);
  floor.noteAccepted(20'000 + OutputDestinationSupervisorPolicy::kHealthyRunMs - 1);
  EXPECT_EQ(floor.consecutiveFailures(), 2) << "one millisecond short is not a run";
  floor.noteAccepted(20'000 + OutputDestinationSupervisorPolicy::kHealthyRunMs);
  EXPECT_EQ(floor.consecutiveFailures(), 0);
  EXPECT_TRUE(floor.mayOpenAt(20'000 + OutputDestinationSupervisorPolicy::kHealthyRunMs));
}

TEST(TransportRestartFloor, AnOperatorOrSupervisorResetClearsTheFloor) {
  TransportRestartFloor floor;
  floor.noteOpened(0);
  floor.noteFailure(0);
  floor.noteFailure(0);
  EXPECT_FALSE(floor.mayOpenAt(1));
  floor.clear();
  EXPECT_TRUE(floor.mayOpenAt(1));
  EXPECT_EQ(floor.consecutiveFailures(), 0);
}

// THE WHOLE DECISION, NOT THE LEAF (CLAUDE.md #481 / #506).
//
// The three tests above are built against TransportRestartFloor alone, and a
// regression that put `clearFfmpegRetryBackoff()` back at the accepted-frame
// call site — which is exactly the defect — leaves every one of them green.
// This one drives a REAL RtmpOutputSender through its REAL `sync()` with the
// destination list RE-SYNCED on every tick, the way renderVideoOutputTick does,
// and measures the interval between successive transport opens.
//
// The destination is 127.0.0.1:1, so every FFmpeg launch dies at connect. The
// program frame is deliberately TINY (64x36 BGRA, 9216 bytes) so it fits inside
// the stdin pipe buffer and the first write of each generation SUCCEEDS: that
// accepted frame is the thing the old code reset its streak on, so without it
// this test could not tell the two defects apart.
//
// MEASURED, both ways, on this rig. Pre-fix: 6725 ms then 6691 ms - FLAT, the
// second one 3.3 s inside the rung it should have been serving, because that
// one accepted frame called clearFfmpegRetryBackoff() every single generation.
// Post-fix: 5742 ms then 10266 ms - rung 1 then rung 2, each carrying the ~0.7 s
// the FFmpeg child lived on top.
//
// NOTE what this harness does NOT reproduce: the incident's sub-5 s interval.
// Here a refused RTMP connect keeps FFmpeg alive ~6 s, so even the old 1 s rung
// produced a 6.7 s gap. What it DOES reproduce is the amplifier - a ladder that
// never climbs is an unbounded rebuild loop whatever its first rung. The rung
// VALUE (1 s vs 5 s) is pinned by TransportRestartFloor's own tests above.
//
// This one is SLOW on purpose (~23 s): the floor is a real wait an operator
// serves, and the clock it is keyed to is part of what is under test, so the
// test cannot fast-forward it without assuming the answer.
TEST(OutputDestinationSupervisor, RestartsAreNeverCloserThanTheCurrentLadderRung) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  std::error_code missing;
  if (!std::filesystem::exists(std::filesystem::path("C:\\ffmpeg\\bin") / "ffmpeg.exe", missing)) {
    std::fprintf(stderr,
                 "[  SKIPPED ] OutputDestinationSupervisor."
                 "RestartsAreNeverCloserThanTheCurrentLadderRung (ffmpeg absent at"
                 " C:\\ffmpeg\\bin) - this test did NOT run\n");
    return;
  }
  auto sender = createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  ProgramFrame frame{64, 36, 2, 7, "restart-floor", "d3d11"};
  frame.programFullBgra.width = 64;
  frame.programFullBgra.height = 36;
  frame.programFullBgra.bgra.assign(64u * 36u * 4u, 0x10);

  OutputDestinationSettings settings;
  settings.id = "rtmp";
  settings.label = "RTMP";
  settings.protocol = "rtmp";
  // Port 1: the connect is refused immediately, so every launched FFmpeg dies
  // without ever reaching the destination. No network, no listener, no waiting.
  settings.url = "rtmp://127.0.0.1:1/live";
  settings.streamKey = "restart-floor";
  settings.ffmpegBinDirectory = "C:\\ffmpeg\\bin";
  settings.videoCodec = "h264";

  // The sender's clock is the `elapsedMs` argument MediaCore feeds it, so the
  // test drives it from real wall time: the point of the assertion is the floor
  // an operator actually waits out, and a virtual clock would let a build whose
  // backoff is keyed to something else (the pre-fix adapter's own steady_clock)
  // look correct. Three opens is two intervals — 5 s then 10 s — which is the
  // smallest window in which "below the first rung" and "the ladder never
  // climbed" are both falsifiable.
  std::vector<double> opens;
  bool sawAcceptedFrame = false;
  double lastStartedAtMs = -1;
  const auto wallStart = std::chrono::steady_clock::now();
  const auto wallDeadline = wallStart + std::chrono::seconds(75);
  while (opens.size() < 3 && std::chrono::steady_clock::now() < wallDeadline) {
    const double elapsedMs = static_cast<double>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - wallStart)
            .count());
    const auto session = sender->sync({"rtmp"}, &frame, elapsedMs, {settings});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (session.senders.empty()) continue;
    const auto& record = session.senders.front();
    if (record.lastResultCode == "ffmpeg-missing" || record.lastResultCode == "runtime-missing") {
      std::fprintf(stderr,
                   "[  SKIPPED ] OutputDestinationSupervisor."
                   "RestartsAreNeverCloserThanTheCurrentLadderRung (%s) - this test did"
                   " NOT run\n", record.lastResultCode.c_str());
      return;
    }
    // A transport OPEN, not a status string: `startedAtMs` is stamped by
    // ensureFfmpegProcess the instant startFfmpegProcess() succeeds, and
    // lastResultCode is overwritten by the first accepted frame in the SAME
    // sync() call, so it cannot be used to count opens.
    if (record.startedAtMs != lastStartedAtMs) {
      lastStartedAtMs = record.startedAtMs;
      opens.push_back(record.startedAtMs);
    }
    if (record.lastResultCode == "encoder-input-accepted") sawAcceptedFrame = true;
  }

  ASSERT_GE(opens.size(), 3u)
      << "this test proves nothing unless the sender actually re-opened its"
         " transport several times (opens=" << opens.size() << ")";
  EXPECT_TRUE(sawAcceptedFrame)
      << "the accepted frame is the thing the pre-fix code reset its streak on;"
         " without one this test cannot distinguish the two defects";

  std::vector<double> intervals;
  for (std::size_t i = 1; i < opens.size(); ++i) intervals.push_back(opens[i] - opens[i - 1]);

  for (std::size_t i = 0; i < intervals.size(); ++i) {
    std::fprintf(stderr, "[restart-floor] interval %zu = %.0fms\n", i, intervals[i]);
  }
  // THE PROPERTY: interval i must be at least the rung the ladder has reached by
  // then. Each interval also carries the transport's own lifetime (the seconds
  // FFmpeg lived before its write failed), so the rung is a FLOOR, never an
  // equality - which is exactly how the assertion is stated.
  for (std::size_t i = 0; i < intervals.size(); ++i) {
    const auto rungMs = static_cast<double>(
        OutputDestinationSupervisorPolicy::backoffMsForFailureCount(static_cast<int>(i)));
    EXPECT_GE(intervals[i], rungMs)
        << "interval " << i << " (" << intervals[i] << "ms) is closer than the ladder's"
           " rung " << (i + 1) << " (" << rungMs << "ms) - either the floor is below the"
           " house ladder, or a restart that accepted one frame and died returned the"
           " budget. Both were true before #597 task 7; the measured pre-fix sequence"
           " was a FLAT 6735ms / 6703ms.";
    if (i > 0) {
      EXPECT_GE(intervals[i], intervals[i - 1])
          << "the ladder went BACKWARDS between interval " << (i - 1) << " and " << i
          << " - something is returning the budget without a healthy run";
    }
  }
#else
  EXPECT_TRUE(true) << "Needs the RTMP sender.";
#endif
}
