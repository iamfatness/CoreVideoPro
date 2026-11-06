#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace corevideo::modules {
inline int64_t monitorEvidenceNow100ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
}
enum class MonitorDeliveryPhase : uint8_t { Unknown, Stopped, Priming, Playing, Suppressed, Starved };
inline const char* monitorDeliveryPhaseName(MonitorDeliveryPhase phase) {
  switch (phase) {
    case MonitorDeliveryPhase::Stopped: return "stopped";
    case MonitorDeliveryPhase::Priming: return "priming";
    case MonitorDeliveryPhase::Playing: return "playing";
    case MonitorDeliveryPhase::Suppressed: return "suppressed";
    case MonitorDeliveryPhase::Starved: return "starved";
    default: return "unknown";
  }
}
struct MonitorDeliveryEvidence {
  bool supported = false, coherent = true, suppressionApplied = false;
  uint64_t backendEpoch = 0, lifecycleEpoch = 0, lastEventLifecycleEpoch = 0;
  int64_t rawEvents = 0, rawMissingFrames = 0;
  int64_t suppressedEvents = 0, unexpectedEvents = 0;
  int64_t primingSilenceFrames = 0, suppressedSilenceFrames = 0;
  int64_t observedAt100ns = 0, lastEventAt100ns = 0;
  MonitorDeliveryPhase phase = MonitorDeliveryPhase::Unknown;
  MonitorDeliveryPhase lastEvent = MonitorDeliveryPhase::Unknown;
  std::string endpointId;
};

// One device callback writer. Lifecycle writes happen before it starts or
// after it joins. Suppression is an atomic intent applied by the device owner.
// No allocation, locks, spin-until-success or device operation in this recorder.
class MonitorDeliveryRecorder {
 public:
  MonitorDeliveryRecorder() : backendEpoch_(nextEpoch_.fetch_add(1)) {}
  void setSuppressed(bool value) { suppressed_.store(value); }
  void beginEndpoint() {
    sequence_.fetch_add(1);
    lifecycleEpoch_.fetch_add(1);
    phase_.store(MonitorDeliveryPhase::Priming);
    sequence_.fetch_add(1);
  }
  void endEndpoint() {
    sequence_.fetch_add(1);
    phase_.store(MonitorDeliveryPhase::Stopped);
    sequence_.fetch_add(1);
  }
  void observe(uint64_t requested, uint64_t real, bool primed, int64_t at100ns) {
    const auto missing = static_cast<int64_t>(requested > real ? requested - real : 0);
    const bool suppressed = suppressed_.load();
    sequence_.fetch_add(1);
    if (suppressed) suppressedSilenceFrames_.fetch_add(missing);
    else if (!primed) primingSilenceFrames_.fetch_add(missing);
    auto phase = suppressed ? MonitorDeliveryPhase::Suppressed :
        !primed ? MonitorDeliveryPhase::Priming : MonitorDeliveryPhase::Playing;
    if (primed && missing > 0) {
      rawEvents_.fetch_add(1); rawMissingFrames_.fetch_add(missing);
      if (suppressed) suppressedEvents_.fetch_add(1);
      else { unexpectedEvents_.fetch_add(1); phase = MonitorDeliveryPhase::Starved; }
      lastEvent_.store(suppressed ? MonitorDeliveryPhase::Suppressed : MonitorDeliveryPhase::Starved);
      lastEventAt100ns_.store(at100ns);
      lastEventLifecycleEpoch_.store(lifecycleEpoch_.load());
    }
    phase_.store(phase);
    sequence_.fetch_add(1);
  }
  int64_t rawEvents() const { return rawEvents_.load(); }
  MonitorDeliveryEvidence snapshot(int64_t at100ns) const {
    for (int attempt = 0; attempt < 3; ++attempt) {
      const auto before = sequence_.load();
      if (before & 1) continue;
      MonitorDeliveryEvidence value;
      value.supported = true; value.backendEpoch = backendEpoch_;
      value.lifecycleEpoch = lifecycleEpoch_.load();
      value.lastEventLifecycleEpoch = lastEventLifecycleEpoch_.load();
      value.rawEvents = rawEvents_.load(); value.rawMissingFrames = rawMissingFrames_.load();
      value.suppressedEvents = suppressedEvents_.load(); value.unexpectedEvents = unexpectedEvents_.load();
      value.primingSilenceFrames = primingSilenceFrames_.load();
      value.suppressedSilenceFrames = suppressedSilenceFrames_.load();
      value.lastEventAt100ns = lastEventAt100ns_.load();
      value.phase = phase_.load(); value.lastEvent = lastEvent_.load();
      value.suppressionApplied = suppressed_.load();
      if (sequence_.load() != before) continue;
      value.observedAt100ns = at100ns;
      return value;
    }
    MonitorDeliveryEvidence unavailable;
    unavailable.supported = true; unavailable.coherent = false;
    return unavailable; // owner retains its previous valid observation/time
  }
 private:
  inline static std::atomic<uint64_t> nextEpoch_{1};
  const uint64_t backendEpoch_;
  std::atomic<uint64_t> sequence_{0}, lifecycleEpoch_{0}, lastEventLifecycleEpoch_{0};
  std::atomic<int64_t> rawEvents_{0}, rawMissingFrames_{0}, suppressedEvents_{0}, unexpectedEvents_{0};
  std::atomic<int64_t> primingSilenceFrames_{0}, suppressedSilenceFrames_{0}, lastEventAt100ns_{0};
  std::atomic<bool> suppressed_{false};
  std::atomic<MonitorDeliveryPhase> phase_{MonitorDeliveryPhase::Stopped}, lastEvent_{MonitorDeliveryPhase::Unknown};
};
}
