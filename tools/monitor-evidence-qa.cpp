#include "modules/AsyncAudioMonitorOutput.h"
#include "rpc/Json.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace corevideo::modules;
using corevideo::rpc::Json;
using Clock = std::chrono::steady_clock;

// Explicit local rehearsal, EXCLUDE_FROM_ALL and never packaged. Submits real
// silent PCM to a shared-mode endpoint; it cannot prove audible continuity.
int main(int argc, char** argv) {
  if (argc < 2 || argc > 3 || std::filesystem::exists(argv[1])) return 2;
  const std::string device = argc == 3 ? argv[2] : "";
  Json::Array samples, failures;
  const auto check = [&](bool value, const char* message) { if (!value) failures.emplace_back(message); };
  try {
    auto backend = createWasapiMonitorOutput();
    if (!backend || !backend->hardwareOutput()) throw std::runtime_error("Real WASAPI backend unavailable");
    AsyncAudioMonitorOutput monitor(std::move(backend));
    const auto sample = [&](const char* label) {
      const auto e = monitor.deliveryEvidence();
      const double age = e.observedAt100ns > 0 ? (monitorEvidenceNow100ns() - e.observedAt100ns) / 10000.0 : -1;
      samples.emplace_back(Json::Object{{"phase", label}, {"classificationSupported", e.supported},
          {"backendEpoch", static_cast<double>(e.backendEpoch)}, {"lifecycleEpoch", static_cast<double>(e.lifecycleEpoch)},
          {"endpointId", e.endpointId}, {"devicePhase", monitorDeliveryPhaseName(e.phase)},
          {"rawEvents", static_cast<double>(e.rawEvents)}, {"rawMissingFrames", static_cast<double>(e.rawMissingFrames)},
          {"suppressedEvents", static_cast<double>(e.suppressedEvents)}, {"unexpectedEvents", static_cast<double>(e.unexpectedEvents)},
          {"primingSilenceFrames", static_cast<double>(e.primingSilenceFrames)},
          {"suppressionApplied", e.suppressionApplied}, {"observationAgeMs", age},
          {"lastEventAt100ns", static_cast<double>(e.lastEventAt100ns)},
          {"lastEventLifecycleEpoch", static_cast<double>(e.lastEventLifecycleEpoch)},
          {"lastEvent", monitorDeliveryPhaseName(e.lastEvent)}});
      check(e.supported && e.coherent, "Missing coherent WASAPI event classification");
      check(age >= 0 && age < 500, "Device evidence did not remain fresh within the rehearsal tolerance");
      check(e.rawEvents == e.suppressedEvents + e.unexpectedEvents, "Raw/category totals disagree");
      return e;
    };
    const auto wait = [&](auto predicate, const char* failure) {
      const auto deadline = Clock::now() + std::chrono::seconds(5);
      while (!predicate() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (!predicate()) throw std::runtime_error(failure);
    };
    const auto feed = [&] {
      const float pcm[960]{}; // 480 actual sample frames, silence by construction
      auto next = Clock::now();
      for (int i = 0; i < 100; ++i) {
        check(monitor.render(pcm, 480, 2, 1.0), "Silent PCM was not admitted by the async owner");
        next += std::chrono::milliseconds(10); std::this_thread::sleep_until(next);
      }
    };
    monitor.start(device, 48000, 2);
    wait([&] { return monitor.active() && monitor.deliveryEvidence().supported; }, "Endpoint did not activate");
    const auto initial = sample("initial-priming");
    feed();
    const auto primed = sample("playing-before-deliberate-starvation");
    // Deliberately withhold PCM to prove unexpected-starvation classification.
    wait([&] { return monitor.deliveryEvidence().unexpectedEvents > primed.unexpectedEvents; }, "Withheld PCM did not produce a measured starvation event");
    const auto starved = sample("deliberate-starvation");
    check(starved.lastEvent == MonitorDeliveryPhase::Starved, "Starvation was misclassified");

    // Re-prime while annotation is applied; then stop sending PCM. The flag
    // observed by the device, not later desired intent, classifies the gap.
    monitor.setIntentionalSilence(true);
    feed();
    wait([&] { return monitor.deliveryEvidence().suppressionApplied; }, "Suppression intent was not applied");
    const auto beforeMuteDrain = sample("before-suppressed-drain");
    wait([&] { return monitor.deliveryEvidence().suppressedEvents > beforeMuteDrain.suppressedEvents; }, "Suppressed drain was not observed while idle");
    const auto muted = sample("suppressed-running");
    check(muted.unexpectedEvents >= starved.unexpectedEvents, "Suppression erased an earlier starvation event");
    check(muted.rawEvents > starved.rawEvents, "Suppressed event disappeared from the raw count");

    monitor.stop();
    wait([&] { return !monitor.preparing(); }, "Endpoint did not stop");
    const auto stopped = sample("disabled");
    // The callback may observe another gap between the sampled value and stop.
    check(stopped.rawEvents >= muted.rawEvents, "Disable reset raw history");
    check(stopped.phase == MonitorDeliveryPhase::Stopped, "Disabled endpoint was not observed stopped");
    const auto stoppedAt = stopped.observedAt100ns;
    wait([&] { return monitor.deliveryEvidence().observedAt100ns > stoppedAt; }, "Stopped endpoint evidence did not refresh");
    check(monitor.deliveryEvidence().rawEvents == stopped.rawEvents, "Stopped endpoint invented a raw event");
    check(stopped.endpointId == muted.endpointId, "Disabled evidence lost its endpoint context");
    monitor.setIntentionalSilence(false);
    monitor.start(device, 48000, 2);
    wait([&] { return monitor.active() && monitor.deliveryEvidence().lifecycleEpoch > stopped.lifecycleEpoch; }, "Endpoint did not reenable in a new lifecycle");
    const auto reopened = sample("reenabled-priming");
    check(reopened.backendEpoch == initial.backendEpoch, "Same backend unexpectedly changed counter epoch");
    check(reopened.rawEvents == stopped.rawEvents, "Reenable resurfaced history as a new event");
    check(reopened.lastEventLifecycleEpoch == muted.lifecycleEpoch, "Event lifecycle was rewritten after restart");
    check(monitor.acceptedFrameCount() > 0, "No actual PCM acceptance observed");
    check(monitor.droppedFrameCount() == 0, "Async PCM queue dropped sample frames");
    monitor.stop();
    wait([&] { return !monitor.preparing(); }, "Final endpoint stop did not complete");
    sample("final-stopped");
  } catch (const std::exception& error) { failures.emplace_back(error.what()); }
  const bool passed = failures.empty();
  Json result = Json::Object{{"passed", passed}, {"samples", std::move(samples)}, {"failures", std::move(failures)},
      {"scope", "Actual WASAPI endpoint and async owner; deliberately induced starvation and suppression; silent PCM, no audible-continuity claim; no system default or volume changes"}};
  std::ofstream output(argv[1]); output << result.stringify();
  if (!output) return 2;
  std::cout << (passed ? "Monitor device evidence rehearsal passed\n" : "Monitor device evidence rehearsal failed; evidence retained\n");
  return passed ? 0 : 1;
}
