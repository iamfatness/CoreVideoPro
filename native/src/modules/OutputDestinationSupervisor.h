#pragma once

// PR19 — the MECHANISM half of the output supervisor. The decision half is
// OutputDestinationSupervisorPolicy.h and is pure; this file owns the threads,
// the generations and the adapter calls.
//
// ---------------------------------------------------------------------------
// Shape
// ---------------------------------------------------------------------------
// `SupervisedOutputSender` is a decorator that sits OUTSIDE the per-protocol
// AsyncOutputSender:
//
//     CompositeOutputSender
//       └─ SupervisedOutputSender          <- this file (supervisor thread)
//            └─ AsyncOutputSender          <- bounded queue, drop-to-latest
//                 └─ RtmpOutputSenderAdapter / SrtOutputSenderAdapter /
//                    NdiOutputSenderAdapter
//
// Outside, not inside, and that is the whole isolation argument. Every call the
// supervisor makes into its child lands on AsyncOutputSender, whose sync/audio/
// fail/recover enqueue and return immediately and whose session() is a cached
// snapshot behind a small mutex. A wedged FFmpeg pipe or a blocked libNDI send
// therefore cannot block the supervisor, cannot block the caller, and cannot
// block a sibling destination — the sibling has its own writer thread already.
//
// Nothing here runs on the render tick and nothing here spawns under
// `coreMutex`. Restarts happen on the supervisor's own thread, the same reason
// BrowserSourceHostAdapter spawns on its own supervisor thread: a real show
// measured 60% of render-tick holds already over their 8 ms budget, and a
// destination's failure has no claim on Program's schedule. Program production,
// Program audio and Program playout never wait for anything in this file.
//
// ---------------------------------------------------------------------------
// Generations: a number AND an object identity
// ---------------------------------------------------------------------------
// ShowEngineSupervisor's stated reason for identity-checking rather than
// number-checking is that a counter-only guard reds nothing when the child
// object is swapped. A destination has exactly the same hazard: a retired
// FFmpeg child's last snapshot, or a sync issued microseconds before a restart,
// arrives AFTER the new generation began and would otherwise be counted as the
// new generation's progress — which is the one thing that establishes health.
// So every generation carries a `std::shared_ptr<const Run>` token owned solely
// by the supervisor; a stamp is admissible only if its weak_ptr still locks to
// the CURRENT token AND its number matches. A retired token is unlockable the
// instant the generation is replaced, so a stale completion is rejected even if
// a number were reused.
//
// ---------------------------------------------------------------------------
// Restart mechanism
// ---------------------------------------------------------------------------
// A restart is `interrupt(destination)` followed by `recover(destination, ...)`.
// interrupt() is the adapters' non-blocking transport cancellation — for
// RTMP/SRT it terminates the out-of-process FFmpeg child, which also releases a
// writer thread blocked in a pipe write. recover() re-probes the runtime, drops
// the adapter's own backoff and re-opens. Both already exist and are already
// used by Stop; the supervisor is not a second lifecycle, it is a caller.
//
// ---------------------------------------------------------------------------
// What is NOT protected (see destinationIsolationTraits below)
// ---------------------------------------------------------------------------
// RTMP and SRT are out-of-process FFmpeg children in a job object: a crash or a
// hang is a child's crash or hang, and the supervisor can end it. NDI is
// IN-PROCESS — NdiOutputSenderAdapter resolves Processing.NDI.Lib.x64.dll at
// runtime and calls send_send_video_v2 on the writer thread with clock_video
// set, so libNDI paces inside our address space — and it implements no
// interrupt(). A wedged libNDI send can therefore be detected, published and
// escalated by this supervisor, but it cannot be *released*: the writer thread
// is detached after AsyncOutputSender's 2 s grace and the leaked thread lives
// until exit. Moving NDI out of process is PR 20/28 and is explicitly out of
// scope here. What this file does is make that risk visible and bounded rather
// than silent: the destination is published with inProcessRisk=true and
// interruptible=false, its health decays on real evidence, and its failures
// climb the same ladder.

#include "modules/Interfaces.h"
#include "modules/OutputDestinationSupervisorPolicy.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace corevideo::modules {

// A destination's isolation properties, as a pure fact about the adapter that
// carries it. Declared in ONE place so a support bundle and the operator UI
// cannot disagree about which destinations are actually contained.
struct DestinationIsolationTraits {
  // The media path for this destination runs inside corevideo-native.exe.
  bool inProcess = false;
  // interrupt() can actually release a writer blocked in this destination's
  // transport call.
  bool interruptible = false;
};

[[nodiscard]] inline DestinationIsolationTraits destinationIsolationTraits(const std::string& destination) {
  if (destination == "ndi") {
    // In-process libNDI, no interrupt() override. PR 20/28.
    return DestinationIsolationTraits{true, false};
  }
  if (destination == "rtmp" || destination == "srt") {
    // Out-of-process FFmpeg child in a job object; interrupt() terminates it.
    return DestinationIsolationTraits{false, true};
  }
  // Unknown destinations are assumed unprotected. Claiming isolation we have
  // not established is exactly the failure mode this item exists to remove.
  return DestinationIsolationTraits{true, false};
}

// Identity of one destination run. Held only by the supervisor; a weak_ptr to it
// is what makes an inbound event admissible.
struct DestinationRun {
  std::uint64_t generation = 0;
};

struct DestinationGeneration {
  std::uint64_t number = 0;
  std::shared_ptr<const DestinationRun> token;
};

// Stamp carried by anything that reports back about a destination: a snapshot
// harvested after a sync, an FFmpeg child's exit, a restart completion.
struct DestinationEventStamp {
  std::string destination;
  std::uint64_t number = 0;
  std::weak_ptr<const DestinationRun> token;
};

// Published per destination into `sessionState` (and therefore into /snapshot
// and every support bundle) alongside the existing sender record.
struct DestinationSupervisorReport {
  std::string destination;
  std::uint64_t generation = 0;
  bool healthy = false;
  bool gaveUp = false;
  int consecutiveFailures = 0;
  int restarts = 0;
  std::int64_t nextAttemptInMs = 0;
  std::int64_t lastProgressAgeMs = -1;  // -1 = never accepted anything
  std::int64_t acceptedUnits = 0;
  std::int64_t staleEventsRejected = 0;
  std::int64_t malformedObservations = 0;
  std::string failureClass = "none";
  std::string reason;
  bool inProcessRisk = false;
  bool interruptible = false;
};

class SupervisedOutputSender final : public IOutputSender {
 public:
  struct Options {
    // Injected monotonic clock, so the ladder is testable without sleeping.
    std::function<std::int64_t()> clock;
    // Supervisor evaluation cadence. Independent of the media tick by design.
    std::chrono::milliseconds tickInterval{250};
    // Tests drive evaluation explicitly via pumpForTest().
    bool startThread = true;
  };

  explicit SupervisedOutputSender(std::unique_ptr<IOutputSender> child);
  SupervisedOutputSender(std::unique_ptr<IOutputSender> child, Options options);
  ~SupervisedOutputSender() override;

  OutputSenderSession sync(
      const std::vector<std::string>& destinations,
      const ProgramFrame* frame,
      double elapsedMs,
      const std::vector<OutputDestinationSettings>& destinationSettings = {},
      const std::vector<float>* programAudioPcm = nullptr,
      int audioChannels = 0,
      int audioSampleRate = 0) override;
  void submitAudio(const std::vector<float>& pcm, int channels, int sampleRate) override;
  OutputSenderSession fail(const std::string& destination, const std::string& message, double elapsedMs) override;
  OutputSenderSession recover(const std::string& destination, double elapsedMs, const std::string& reason) override;
  OutputSenderSession session() const override;
  void interrupt(const std::string& destination) override;

  // --- supervisor surface -------------------------------------------------

  // The generation an event must match to be admissible.
  [[nodiscard]] DestinationGeneration currentGeneration(const std::string& destination) const;

  // Identity-and-number check. A retired child's completion is rejected here.
  [[nodiscard]] bool acceptEvent(const DestinationEventStamp& stamp) const;

  [[nodiscard]] std::vector<DestinationSupervisorReport> report() const;

  // Run exactly one supervisor evaluation on the CALLING thread. Deterministic
  // tests use this with Options::startThread=false; production uses the thread.
  void pumpForTest();

 private:
  struct Destination {
    OutputDestinationSupervisorPolicy policy;
    std::shared_ptr<const DestinationRun> token;
    std::uint64_t generation = 0;
    bool desiredActive = false;
    std::int64_t acceptedUnits = 0;
    std::int64_t staleEventsRejected = 0;
    std::int64_t malformedObservations = 0;
    bool healthy = false;
    std::string failureClass = "none";
    std::string reason;
    std::int64_t lastProgressAgeMs = -1;
    std::int64_t nextAttemptInMs = 0;
    double lastElapsedMs = 0;
    DestinationIsolationTraits traits;
    // Adapter fields observed this cycle (never trusted as health on their own).
    std::string status;
    std::string destinationHealth;
    std::string lastResultCode;
    std::string lastError;
    bool observedThisCycle = false;
  };

  Destination& destinationLocked(const std::string& name);
  void noteDesired(const std::vector<std::string>& destinations, double elapsedMs);
  // Harvests the child's published records. Every record is stamped with the
  // generation current at harvest time and admitted only if that stamp is still
  // current when it is applied.
  void harvest();
  void evaluate();
  void applyReportsTo(OutputSenderSession& session) const;
  static bool recordIsWellFormed(const OutputSender& record);
  void supervisorLoop();

  Options options_;
  std::unique_ptr<IOutputSender> child_;
  mutable std::mutex mutex_;
  std::map<std::string, Destination> destinations_;

  std::mutex wakeMutex_;
  std::condition_variable wakeCv_;
  bool stopping_ = false;
  std::thread supervisor_;
};

}  // namespace corevideo::modules
