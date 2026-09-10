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
