#include "modules/OutputDestinationSupervisor.h"

#include "core/BoundedAsyncLog.h"

#include <algorithm>
#include <utility>

namespace corevideo::modules {
namespace {

std::int64_t steadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

SupervisedOutputSender::SupervisedOutputSender(std::unique_ptr<IOutputSender> child)
    : SupervisedOutputSender(std::move(child), Options{}) {}

SupervisedOutputSender::SupervisedOutputSender(std::unique_ptr<IOutputSender> child, Options options)
    : options_(std::move(options)), child_(std::move(child)) {
  if (!options_.clock) {
    options_.clock = &steadyNowMs;
  }
  if (options_.startThread) {
    supervisor_ = std::thread(&SupervisedOutputSender::supervisorLoop, this);
  }
}

SupervisedOutputSender::~SupervisedOutputSender() {
  {
    std::lock_guard<std::mutex> lock(wakeMutex_);
    stopping_ = true;
  }
  wakeCv_.notify_all();
  if (supervisor_.joinable()) {
    // The supervisor thread only ever makes non-blocking child calls, so this
    // join is bounded by one tick. It is NEVER the thread that can wedge; the
    // one that can is the AsyncOutputSender writer the child owns, and that one
    // already has its own interrupt-then-grace-then-detach teardown.
    supervisor_.join();
  }
  child_.reset();
}

SupervisedOutputSender::Destination& SupervisedOutputSender::destinationLocked(const std::string& name) {
  auto it = destinations_.find(name);
  if (it != destinations_.end()) {
    return it->second;
  }
  Destination fresh;
  fresh.traits = destinationIsolationTraits(name);
  fresh.token = std::make_shared<const DestinationRun>(DestinationRun{1});
  fresh.generation = 1;
  fresh.policy.onGenerationStarted(options_.clock());
  return destinations_.emplace(name, std::move(fresh)).first->second;
}

void SupervisedOutputSender::noteDesired(const std::vector<std::string>& destinations, double elapsedMs) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [name, destination] : destinations_) {
    destination.desiredActive = false;
  }
  for (const auto& name : destinations) {
    if (name.empty()) continue;
    auto& destination = destinationLocked(name);
    destination.desiredActive = true;
    destination.lastElapsedMs = elapsedMs;
  }
}

// A record we will not reason about. A malformed reply must never be able to
// establish health (or a fault) — it is counted and discarded.
bool SupervisedOutputSender::recordIsWellFormed(const OutputSender& record) {
  if (record.destination.empty()) return false;
  if (record.framesSent < 0 || record.audioFramesSent < 0) return false;
  if (record.bytesSent < 0 || record.audioBytesSent < 0) return false;
  return true;
}

OutputSenderSession SupervisedOutputSender::sync(
    const std::vector<std::string>& destinations,
    const ProgramFrame* frame,
    double elapsedMs,
    const std::vector<OutputDestinationSettings>& destinationSettings,
    const std::vector<float>* programAudioPcm,
    int audioChannels,
    int audioSampleRate) {
  noteDesired(destinations, elapsedMs);
  // Non-blocking by construction: the child is an AsyncOutputSender, so this
  // enqueues and returns its cached snapshot. Program never waits here.
  auto session = child_ ? child_->sync(destinations, frame, elapsedMs, destinationSettings,
                                       programAudioPcm, audioChannels, audioSampleRate)
                        : OutputSenderSession{};
  applyReportsTo(session);
  return session;
}

void SupervisedOutputSender::submitAudio(const std::vector<float>& pcm, int channels, int sampleRate) {
  if (child_) child_->submitAudio(pcm, channels, sampleRate);
}

OutputSenderSession SupervisedOutputSender::fail(const std::string& destination, const std::string& message, double elapsedMs) {
  auto session = child_ ? child_->fail(destination, message, elapsedMs) : OutputSenderSession{};
  applyReportsTo(session);
  return session;
}

OutputSenderSession SupervisedOutputSender::recover(const std::string& destination, double elapsedMs, const std::string& reason) {
  // This is the OPERATOR reset every house supervisor is required to have: it
  // clears give-up, drops the failure streak and retires the current generation
  // so nothing in flight from the old run can be read as the new run's health.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = destinations_.find(destination);
    if (it != destinations_.end()) {
      auto& record = it->second;
      record.generation += 1;
      record.token = std::make_shared<const DestinationRun>(DestinationRun{record.generation});
      record.acceptedUnits = 0;
      record.policy.reset(options_.clock());
      record.reason.clear();
      record.failureClass = "none";
      record.healthy = false;
      record.lastProgressAgeMs = -1;
      record.lastElapsedMs = elapsedMs;
    }
  }
  auto session = child_ ? child_->recover(destination, elapsedMs, reason) : OutputSenderSession{};
  applyReportsTo(session);
  return session;
}

OutputSenderSession SupervisedOutputSender::session() const {
  auto session = child_ ? child_->session() : OutputSenderSession{};
  applyReportsTo(session);
  return session;
}

void SupervisedOutputSender::interrupt(const std::string& destination) {
  if (child_) child_->interrupt(destination);
}

DestinationGeneration SupervisedOutputSender::currentGeneration(const std::string& destination) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = destinations_.find(destination);
  if (it == destinations_.end()) return {};
  return DestinationGeneration{it->second.generation, it->second.token};
}

bool SupervisedOutputSender::acceptEvent(const DestinationEventStamp& stamp) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = destinations_.find(stamp.destination);
  if (it == destinations_.end()) return false;
  // Identity FIRST, then the number. The identity check is the one that still
  // works when a child object is swapped without the counter moving — the
  // reason ShowEngineSupervisor guards this way.
  const auto locked = stamp.token.lock();
  if (!locked || locked != it->second.token) return false;
  return stamp.number == it->second.generation;
}

std::vector<DestinationSupervisorReport> SupervisedOutputSender::report() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<DestinationSupervisorReport> out;
  out.reserve(destinations_.size());
  for (const auto& [name, record] : destinations_) {
    DestinationSupervisorReport entry;
    entry.destination = name;
    entry.generation = record.generation;
    entry.healthy = record.healthy;
    entry.gaveUp = record.policy.gaveUp();
    entry.consecutiveFailures = record.policy.consecutiveFailures();
    entry.restarts = record.policy.restarts();
    entry.nextAttemptInMs = record.nextAttemptInMs;
    entry.lastProgressAgeMs = record.lastProgressAgeMs;
    entry.acceptedUnits = record.acceptedUnits;
    entry.staleEventsRejected = record.staleEventsRejected;
    entry.malformedObservations = record.malformedObservations;
    entry.failureClass = record.failureClass;
    entry.reason = record.reason;
    entry.inProcessRisk = record.traits.inProcess;
    entry.interruptible = record.traits.interruptible;
    out.push_back(std::move(entry));
  }
  return out;
}

void SupervisedOutputSender::applyReportsTo(OutputSenderSession& session) const {
  const auto reports = report();
  if (reports.empty()) return;
  bool anyGaveUp = false;
  for (auto& sender : session.senders) {
    const auto match = std::find_if(reports.begin(), reports.end(), [&](const DestinationSupervisorReport& r) {
      return r.destination == sender.destination;
    });
    if (match == reports.end()) continue;
    OutputSupervisorState state;
    state.generation = match->generation;
    state.healthy = match->healthy;
    state.gaveUp = match->gaveUp;
    state.consecutiveFailures = match->consecutiveFailures;
    state.restarts = match->restarts;
    state.nextAttemptInMs = match->nextAttemptInMs;
    state.lastProgressAgeMs = match->lastProgressAgeMs;
    state.acceptedUnits = match->acceptedUnits;
    state.staleEventsRejected = match->staleEventsRejected;
    state.malformedObservations = match->malformedObservations;
    state.failureClass = match->failureClass;
    state.reason = match->reason;
    state.inProcessRisk = match->inProcessRisk;
    state.interruptible = match->interruptible;
    sender.supervisor = state;
    if (match->gaveUp) {
      // Loud, not silent. The lifecycle machine (core::SenderLifecyclePolicy)
      // reads `status`/`destinationHealth`, so publishing the give-up here is
      // what makes the destination read as `failed` everywhere downstream
      // instead of the supervisor keeping a private opinion.
      anyGaveUp = true;
      sender.status = "failed";
      sender.destinationHealth = "failed";
      sender.lastResultCode = "supervisor-gave-up";
      sender.warning = match->reason;
      sender.lastError = match->reason;
    }
  }
  if (anyGaveUp) {
    session.status = "failed";
  }
}

void SupervisedOutputSender::evaluate() {
  const std::int64_t now = options_.clock();

  // 1. Capture the generation stamp for every known destination BEFORE reading
  //    the child, so a restart that lands during the read invalidates the read.
  std::vector<DestinationEventStamp> stamps;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stamps.reserve(destinations_.size());
    for (const auto& [name, record] : destinations_) {
      stamps.push_back(DestinationEventStamp{name, record.generation, record.token});
    }
  }

  // 2. Read the child WITHOUT the supervisor lock. AsyncOutputSender::session()
  //    is a cached snapshot; a wedged writer cannot make this block.
  OutputSenderSession snapshot = child_ ? child_->session() : OutputSenderSession{};

  struct PendingAction {
    std::string destination;
    SupervisorAction action = SupervisorAction::None;
    std::string reason;
    double elapsedMs = 0;
  };
  std::vector<PendingAction> actions;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [name, record] : destinations_) {
      record.observedThisCycle = false;
    }
    for (const auto& sender : snapshot.senders) {
      if (!recordIsWellFormed(sender)) {
        // Attribute the malformed reply to the destination it claims when it
        // claims one; otherwise it is unattributable and simply discarded.
        if (!sender.destination.empty()) {
          destinationLocked(sender.destination).malformedObservations += 1;
        }
        continue;
      }
      auto it = destinations_.find(sender.destination);
      if (it == destinations_.end()) continue;  // never seen as desired: not ours to supervise
      auto& record = it->second;
      const auto stamp = std::find_if(stamps.begin(), stamps.end(), [&](const DestinationEventStamp& s) {
        return s.destination == sender.destination;
      });
      const bool admissible = stamp != stamps.end() && stamp->number == record.generation &&
                              stamp->token.lock() == record.token;
      if (!admissible) {
        // A retired generation's snapshot. Counting it would let a dead child's
        // frame counters vouch for the live one.
        record.staleEventsRejected += 1;
        continue;
      }
      record.observedThisCycle = true;
      record.status = sender.status;
      record.destinationHealth = sender.destinationHealth;
      record.lastResultCode = sender.lastResultCode;
      record.lastError = sender.lastError;
      record.acceptedUnits = sender.framesSent + sender.audioFramesSent;
    }

    for (auto& [name, record] : destinations_) {
      DestinationObservation observation;
      observation.desiredActive = record.desiredActive;
      observation.observed = record.observedThisCycle;
      observation.acceptedUnits = record.acceptedUnits;
      observation.status = record.status;
      observation.destinationHealth = record.destinationHealth;
      observation.lastResultCode = record.lastResultCode;
      observation.lastError = record.lastError;
      observation.nowMs = now;

      const auto decision = record.policy.observe(observation);
      record.healthy = decision.healthy;
      record.failureClass = toString(decision.failureClass);
      if (!decision.reason.empty()) record.reason = decision.reason;
      record.lastProgressAgeMs = record.policy.progressAgeMs(now);
      record.nextAttemptInMs =
          record.policy.faultPending() ? (std::max)(std::int64_t{0}, record.policy.nextAttemptAtMs() - now)
                                       : 0;
      if (decision.action == SupervisorAction::Restart) {
        // Retire the old run under the lock, so anything still in flight from it
        // is rejected by the identity check above from this instant onward.
        record.generation += 1;
        record.token = std::make_shared<const DestinationRun>(DestinationRun{record.generation});
        record.acceptedUnits = 0;
        record.policy.onGenerationStarted(now);
        actions.push_back(PendingAction{name, decision.action, decision.reason, record.lastElapsedMs});
      } else if (decision.action == SupervisorAction::GiveUp) {
        record.reason = record.policy.giveUpReason();
        actions.push_back(PendingAction{name, decision.action, record.reason, record.lastElapsedMs});
      }
    }
  }

  // 3. Adapter calls happen with NO supervisor lock held and on the supervisor's
  //    own thread — never the render tick, never under coreMutex, never a spawn
  //    on a caller's stack.
  for (const auto& action : actions) {
    if (action.action == SupervisorAction::Restart) {
      ::corevideo::core::nativeLogf("[outputSupervisor] restarting %s: %s\n", action.destination.c_str(),
                                    action.reason.c_str());
      if (child_) {
        // interrupt() first: for RTMP/SRT this ends the FFmpeg child, which also
        // releases a writer blocked in a pipe write so recover() can be applied.
        child_->interrupt(action.destination);
        child_->recover(action.destination, action.elapsedMs,
                        "Output supervisor restarted this destination: " + action.reason);
      }
    } else if (action.action == SupervisorAction::GiveUp) {
      ::corevideo::core::nativeLogf("[outputSupervisor] giving up on %s: %s\n", action.destination.c_str(),
                                    action.reason.c_str());
      if (child_) {
        // Stop the transport but leave the destination published as failed with
        // its reason. Silence here would be the papercut, not the protection.
        child_->interrupt(action.destination);
      }
    }
  }
}

void SupervisedOutputSender::pumpForTest() { evaluate(); }

void SupervisedOutputSender::supervisorLoop() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(wakeMutex_);
      if (wakeCv_.wait_for(lock, options_.tickInterval, [&] { return stopping_; })) {
        return;
      }
    }
    evaluate();
  }
}

}  // namespace corevideo::modules
