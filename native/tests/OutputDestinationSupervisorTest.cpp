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
#include "modules/IsolatedOutputSender.h"
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
  // #597 round 2, item 2: the two are counted SEPARATELY. Round 1 shipped
  // restartForSupervisor() with this fake inheriting the default forward to
  // recover(), so `recovers()` counted both and no test in the tree could tell
  // them apart - reverting the supervisor to recover() left the suite green.
  OutputSenderSession restartForSupervisor(const std::string& destination, double elapsedMs,
                                           const std::string& reason) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++supervisedRestarts_;
    }
    return reopen(destination, elapsedMs, reason);
  }
  OutputSenderSession recover(const std::string& destination, double elapsedMs,
                              const std::string& reason) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++operatorRecovers_;
    }
    return reopen(destination, elapsedMs, reason);
  }
  OutputSenderSession reopen(const std::string&, double, const std::string&) {
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

  // Every re-open, however it was asked for.
  int recovers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return recovers_;
  }
  int supervisedRestarts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return supervisedRestarts_;
  }
  int operatorRecovers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return operatorRecovers_;
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
  int supervisedRestarts_ = 0;
  int operatorRecovers_ = 0;
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

// THE WHOLE DECISION, NOT THE LEAF (CLAUDE.md #481 / #506) — for the ADAPTER's
// restart authority. The SUPERVISOR's is the test below this one; round 0 named
// this test for the supervisor and drove none, which is how the supervisor-side
// defect survived the task (round 1 finding 3).
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
TEST(RtmpOutputSender, TransportRebuildsAreNeverCloserThanTheCurrentLadderRung) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  std::error_code missing;
  if (!std::filesystem::exists(std::filesystem::path("C:\\ffmpeg\\bin") / "ffmpeg.exe", missing)) {
    std::fprintf(stderr,
                 "[  SKIPPED ] RtmpOutputSender."
                 "TransportRebuildsAreNeverCloserThanTheCurrentLadderRung (ffmpeg absent at"
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
                   "[  SKIPPED ] RtmpOutputSender."
                   "TransportRebuildsAreNeverCloserThanTheCurrentLadderRung (%s) - this test did"
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
           " was a FLAT 6725ms / 6691ms.";
  }
  // NOTE (round 1, finding 4): there is deliberately no `intervals[i] >=
  // intervals[i-1]` line here. The pre-fix run SATISFIED it on a 1 ms margin
  // (6725 then 6691 failed only because 6691 < rung 2), so it falsified
  // nothing, and the per-rung assertion above already implies it - the rungs
  // are non-decreasing by construction.
#else
  EXPECT_TRUE(true) << "Needs the RTMP sender.";
#endif
}

// ---------------------------------------------------------------------------
// THE SUPERVISOR'S OWN RESTART AUTHORITY (#597 task 7 round 1, finding 1)
// ---------------------------------------------------------------------------
// There are TWO things that rebuild a destination. The test above bounds the
// adapter's. This one bounds the supervisor's, and it exists because that one
// was NOT bounded: OutputDestinationSupervisorPolicy::observe() returned the
// budget on a healthy INSTANT of a run older than kHealthyRunMs -
// `failures_ = 0; nextAttemptAtMs_ = 0;` - WITHOUT clearing `faultPending_`, so
// the restart gate three lines below fired on the very next 250 ms tick against
// a 5,000 ms rung.
//
// The preconditions are the ORDINARY shape of a mid-show failure, which is why
// this was not exotic: `decision.healthy` is decided from acceptedUnits
// FRESHNESS (kProgressStaleMs, 1 s) and not from `status`, so a destination that
// fails while its last accepted unit is under a second old is `failed` and
// `healthy` in the same observation. It is the incident's 21:09:10.088 restart,
// 0.78 s after the 21:09:09.306 overflow whose reason string it carries.
//
// Driven the way the live path drives it: the destination list is RE-SYNCED on
// every tick (the part a naive test omits), the clock is injected, and the
// supervisor is pumped on this thread so nothing sleeps.
TEST(OutputDestinationSupervisor, RestartsAreNeverCloserThanTheCurrentLadderRung) {
  auto owned = std::make_unique<FakeDestinationChild>("rtmp");
  auto* child = owned.get();
  SupervisedOutputSender::Options options;
  std::int64_t now = 0;
  options.clock = [&now] { return now; };
  options.startThread = false;  // pumped below, so the ladder is walked in microseconds
  SupervisedOutputSender sender(std::move(owned), options);

  ProgramFrame frame;
  frame.frameNumber = 1;

  // A healthy RUN, comfortably past kHealthyRunMs, accepting output every tick.
  constexpr std::int64_t kTickMs = 250;
  const std::int64_t healthyUntil = OutputDestinationSupervisorPolicy::kHealthyRunMs * 2;
  for (; now < healthyUntil; now += kTickMs) {
    child->produce(1);
    sender.sync({"rtmp"}, &frame, 0);
    sender.pumpForTest();
  }
  ASSERT_EQ(child->recovers(), 0) << "a healthy destination must not be restarted at all";

  // Now it fails WHILE ITS LAST ACCEPTED UNIT IS STILL FRESH - the queue
  // overflow of the incident, reason string and all.
  child->crash("bitstream-queue-overflow",
               "Compressed-video queue overflow; the stream transport could not drain encoded"
               " video fast enough.");
  const std::int64_t faultAtMs = now;

  std::vector<std::int64_t> restarts;
  int seenRecovers = child->recovers();
  for (; now < faultAtMs + 300'000 && restarts.size() < 3; now += kTickMs) {
    sender.sync({"rtmp"}, &frame, 0);  // the live path re-syncs the list every tick
    sender.pumpForTest();
    if (child->recovers() > seenRecovers) {
      seenRecovers = child->recovers();
      restarts.push_back(now);
    }
  }

  ASSERT_GE(restarts.size(), 3u) << "the supervisor never restarted the destination";
  // #597 round 2, item 2. An AUTOMATIC restart must reach the child as
  // restartForSupervisor(), never as recover() - recover() is the OPERATOR's
  // re-arm and a child holding a restart floor drops it for that call. Round 1
  // made the split and nothing failed when it was undone; this is the line that
  // fails. It is asserted on the SUPERVISOR's own restarts, not on a direct
  // call, so it pins the decision site (OutputDestinationSupervisor.cpp's
  // Restart action) rather than the method's existence.
  EXPECT_EQ(child->supervisedRestarts(), static_cast<int>(restarts.size()));
  EXPECT_EQ(child->operatorRecovers(), 0)
      << "the supervisor used the OPERATOR path for its own automatic restart,"
         " which hands the child a key to the restart floor that bounds it";
  for (std::size_t i = 0; i < restarts.size(); ++i) {
    std::fprintf(stderr, "[supervisor-floor] restart %zu at +%lldms\n", i,
                 static_cast<long long>(restarts[i] - faultAtMs));
  }

  // THE PROPERTY, and the falsifier: the FIRST restart may not come sooner than
  // the ladder's first rung after the fault was observable. Pre-fix this was
  // measured at ONE TICK (250 ms) against a 5,000 ms rung.
  EXPECT_GE(restarts.front() - faultAtMs,
            OutputDestinationSupervisorPolicy::backoffMsForFailureCount(0))
      << "the supervisor restarted " << (restarts.front() - faultAtMs)
      << "ms after the fault, inside its own first rung - a healthy INSTANT is"
         " releasing a PENDING fault's rung";

  // And the ladder must climb. Each later generation costs kStartGraceMs before
  // it faults (nothing is produced after the first restart), so the interval is
  // grace + rung; the falsifier is the STEP between them, which is a whole rung
  // wide and cannot be satisfied by a one-millisecond margin the way round 0's
  // monotonic assertion was (round 1, finding 4).
  const auto firstGap = restarts[1] - restarts[0];
  const auto secondGap = restarts[2] - restarts[1];
  std::fprintf(stderr, "[supervisor-floor] gaps %lldms then %lldms\n",
               static_cast<long long>(firstGap), static_cast<long long>(secondGap));
  EXPECT_GE(secondGap - firstGap,
            OutputDestinationSupervisorPolicy::backoffMsForFailureCount(1) -
                OutputDestinationSupervisorPolicy::backoffMsForFailureCount(0))
      << "the ladder did not climb a full rung between restarts";
  EXPECT_FALSE(sender.report().empty());
}

// The same defect stated as a pure property of the policy, so a reader can see
// what the mechanism test above is actually pinning. This one fails the moment
// the `!faultPending_` guard is removed, with no supervisor and no child.
TEST(OutputDestinationSupervisorPolicyLadder, AHealthyInstantNeverReleasesAPendingFaultsRung) {
  OutputDestinationSupervisorPolicy policy;
  std::int64_t now = 0;
  policy.onGenerationStarted(now);
  // A healthy run: accepted units advancing every tick, well past kHealthyRunMs.
  std::int64_t units = 0;
  for (; now < OutputDestinationSupervisorPolicy::kHealthyRunMs * 2; now += 250) {
    (void)policy.observe(activeObservation(now, ++units, "live", "encoder-input-accepted"));
  }
  // It fails while the last accepted unit is still under kProgressStaleMs old,
  // so the policy calls it `healthy` and `failed` in the same observation.
  const auto armed = policy.observe(activeObservation(now, units, "failed", "bitstream-queue-overflow"));
  ASSERT_EQ(armed.action, SupervisorAction::None);
  ASSERT_TRUE(policy.faultPending());
  const std::int64_t faultAtMs = now;

  for (now += 250; now < faultAtMs + OutputDestinationSupervisorPolicy::kBaseBackoffMs; now += 250) {
    const auto decision = policy.observe(activeObservation(now, units, "failed", "bitstream-queue-overflow"));
    ASSERT_NE(decision.action, SupervisorAction::Restart)
        << "restarted at +" << (now - faultAtMs) << "ms, inside the 5s first rung";
  }
  const auto restart = policy.observe(activeObservation(now, units, "failed", "bitstream-queue-overflow"));
  EXPECT_EQ(restart.action, SupervisorAction::Restart) << "and it must still restart once the rung is served";
}


// ---------------------------------------------------------------------------
// THE WRAPPER LAW, PINNED (#597 round 2, items 2 and 3)
// ---------------------------------------------------------------------------
// restartForSupervisor() is a non-pure virtual whose DEFAULT forwards to
// recover(). That is what makes it safe to add - and exactly what makes an
// un-forwarded wrapper silent: the call still works, it just arrives as the
// wrong event one layer up. This repo has paid for that shape three times (the
// 1-arg connect() pink tiles, SRT's swallowed pollAudioFrames, and five
// unforwarded test-only virtuals earlier in this same plan), so the forwarding
// is pinned rather than reasoned about.
namespace {

// Records WHICH of the two calls arrived, and nothing else.
class RecoverCauseRecorder final : public IOutputSender {
 public:
  explicit RecoverCauseRecorder(std::string destination) {
    record_.destination = std::move(destination);
    record_.senderId = record_.destination + ":program";
    record_.status = "starting";
    record_.destinationHealth = "starting";
    record_.lastResultCode = "waiting-for-frame";
  }
  OutputSenderSession sync(const std::vector<std::string>&, const ProgramFrame*, double,
                           const std::vector<OutputDestinationSettings>&, const std::vector<float>*, int,
                           int) override {
    return session();
  }
  void submitAudio(const std::vector<float>&, int, int) override {}
  OutputSenderSession fail(const std::string&, const std::string&, double) override { return session(); }
  OutputSenderSession recover(const std::string&, double, const std::string&) override {
    operatorRecovers_.fetch_add(1);
    return session();
  }
  OutputSenderSession restartForSupervisor(const std::string&, double, const std::string&) override {
    supervisedRestarts_.fetch_add(1);
    return session();
  }
  OutputSenderSession session() const override {
    OutputSenderSession session;
    session.senders.push_back(record_);
    session.activeSenderCount = 1;
    session.status = record_.status;
    return session;
  }
  int operatorRecovers() const { return operatorRecovers_.load(); }
  int supervisedRestarts() const { return supervisedRestarts_.load(); }

 private:
  OutputSender record_;
  std::atomic<int> operatorRecovers_{0};
  std::atomic<int> supervisedRestarts_{0};
};

}  // namespace

// Item 3, directly: AsyncOutputSender carries the cause across its QUEUE, which
// is the one hop where it could be lost without any signature changing -
// deleting the `else if (item.supervisedRestart)` arm in the writer loop demotes
// every supervisor restart to an operator reset and nothing else moves.
TEST(AsyncOutputSender, TheRecoverCauseSurvivesTheQueue) {
  auto owned = std::make_unique<RecoverCauseRecorder>("rtmp");
  auto* inner = owned.get();
  AsyncOutputSender sender(std::move(owned));

  sender.restartForSupervisor("rtmp", 0, "supervisor restart");
  ASSERT_TRUE(sender.drainForTest(std::chrono::seconds(2)));
  EXPECT_EQ(inner->supervisedRestarts(), 1);
  EXPECT_EQ(inner->operatorRecovers(), 0) << "the queue demoted a supervisor restart to an operator reset";

  sender.recover("rtmp", 0, "operator re-arm");
  ASSERT_TRUE(sender.drainForTest(std::chrono::seconds(2)));
  EXPECT_EQ(inner->operatorRecovers(), 1);
  EXPECT_EQ(inner->supervisedRestarts(), 1) << "an operator re-arm was promoted to a supervisor restart";
}

// Items 2 and 3 together, through the PRODUCTION composition:
// CompositeOutputSender -> SupervisedOutputSender -> AsyncOutputSender -> adapter,
// which is exactly what createIsolatedOutputSender builds for the live core. Any
// ONE of those three inheriting the default collapses the distinction, and this
// is the only test that would notice.
TEST(IsolatedOutputSender, TheRecoverCauseSurvivesEveryWrapper) {
  auto owned = std::make_unique<RecoverCauseRecorder>("rtmp");
  auto* inner = owned.get();
  std::vector<std::unique_ptr<IOutputSender>> children;
  children.push_back(std::move(owned));
  auto sender = createIsolatedOutputSender(std::move(children), {"rtmp"});
  ASSERT_NE(sender, nullptr);

  sender->restartForSupervisor("rtmp", 0, "supervisor restart");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (inner->supervisedRestarts() + inner->operatorRecovers() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_EQ(inner->supervisedRestarts(), 1)
      << "one of Composite / Supervised / Async inherited the default and turned"
         " the supervisor's automatic restart into an operator reset";
  EXPECT_EQ(inner->operatorRecovers(), 0);
}

// ---------------------------------------------------------------------------
// THE ARMED FLOOR, AT THE ADAPTER (#597 round 2, items 1 and 2)
// ---------------------------------------------------------------------------
// Three properties of the gate this round changed, all on ONE armed floor so the
// ~6 s it costs to arm is paid once:
//
//   * a NON-settings change is refused while the floor is armed (round 1's
//     finding-2 rule, which until now nothing pinned),
//   * a SUPERVISOR restart does NOT clear the floor (finding 1's second half -
//     reverting restartForSupervisor() to recover() at the adapter fails HERE,
//     and the supervisor test above fails at the decision site),
//   * an OPERATOR recover() DOES clear it, because the house rule is that an
//     operator action always clears give-up.
TEST(RtmpOutputSender, AnArmedFloorYieldsToAnOperatorAndToNobodyElse) {
#if COREVIDEO_WITH_RTMP_OUTPUT
  std::error_code missing;
  if (!std::filesystem::exists(std::filesystem::path("C:\\ffmpeg\\bin") / "ffmpeg.exe", missing)) {
    std::fprintf(stderr,
                 "[  SKIPPED ] RtmpOutputSender.AnArmedFloorYieldsToAnOperatorAndToNobodyElse"
                 " (ffmpeg absent at C:\\ffmpeg\\bin) - this test did NOT run\n");
    return;
  }
  auto sender = createRtmpOutputSender();
  ASSERT_NE(sender, nullptr);

  ProgramFrame frame{64, 36, 2, 7, "armed-floor", "d3d11"};
  frame.programFullBgra.width = 64;
  frame.programFullBgra.height = 36;
  frame.programFullBgra.bgra.assign(64u * 36u * 4u, 0x10);

  OutputDestinationSettings settings;
  settings.id = "rtmp";
  settings.label = "RTMP";
  settings.protocol = "rtmp";
  settings.url = "rtmp://127.0.0.1:1/live";
  settings.streamKey = "armed-floor";
  settings.ffmpegBinDirectory = "C:\\ffmpeg\\bin";
  settings.videoCodec = "h264";

  const auto wallStart = std::chrono::steady_clock::now();
  auto nowMs = [&wallStart] {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - wallStart)
                                   .count());
  };
  auto tick = [&](const ProgramFrame& f) {
    const auto session = sender->sync({"rtmp"}, &f, nowMs(), {settings});
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return session.senders.empty() ? OutputSender{} : session.senders.front();
  };

  // Arm the floor: open, then let the refused connect break the pipe.
  OutputSender record;
  const auto armDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  do {
    record = tick(frame);
    if (record.lastResultCode == "ffmpeg-missing" || record.lastResultCode == "runtime-missing") {
      std::fprintf(stderr,
                   "[  SKIPPED ] RtmpOutputSender.AnArmedFloorYieldsToAnOperatorAndToNobodyElse"
                   " (%s) - this test did NOT run\n", record.lastResultCode.c_str());
      return;
    }
  } while (record.lastResultCode != "ffmpeg-retry-backoff" &&
           std::chrono::steady_clock::now() < armDeadline);
  ASSERT_EQ(record.lastResultCode, "ffmpeg-retry-backoff") << "the floor never armed";

  // A NON-settings change - a different program size - is not operator intent
  // and must not buy a rebuild. Round 0's gate would have let this through the
  // moment the transport happened to be up.
  ProgramFrame resized{96, 54, 3, 7, "armed-floor", "d3d11"};
  resized.programFullBgra.width = 96;
  resized.programFullBgra.height = 54;
  resized.programFullBgra.bgra.assign(96u * 54u * 4u, 0x10);
  EXPECT_EQ(tick(resized).lastResultCode, "ffmpeg-retry-backoff")
      << "a program-size change rebuilt the transport inside its rung";

  // The SUPERVISOR's own automatic restart keeps the floor.
  sender->restartForSupervisor("rtmp", nowMs(), "Output supervisor restarted this destination.");
  EXPECT_EQ(tick(frame).lastResultCode, "ffmpeg-retry-backoff")
      << "a supervisor restart cleared the floor that exists to bound it";

  // The OPERATOR's re-arm clears it, and the very next tick may open.
  sender->recover("rtmp", nowMs(), "Operator re-armed the destination.");
  const auto afterOperator = tick(frame);
  EXPECT_NE(afterOperator.lastResultCode, "ffmpeg-retry-backoff")
      << "an operator re-arm was made to wait out a ladder it had just overridden";
#else
  EXPECT_TRUE(true) << "Needs the RTMP sender.";
#endif
}
