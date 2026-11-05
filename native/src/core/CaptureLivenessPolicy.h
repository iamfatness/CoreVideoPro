#pragma once

#include "core/SourceBus.h"

#include <algorithm>

namespace corevideo::core {

inline constexpr int64_t kCaptureStallNs = kOnAirStallNs;
inline constexpr int64_t kCaptureLostHoldNs = 2'000'000'000;
inline constexpr int64_t kCaptureSilentFailNs = 5'000'000'000;

enum class CaptureOnAirPhase : uint8_t { Unreported, Live, Stalled, LostHolding, Failed };

struct CaptureOnAirDecision {
  CaptureOnAirPhase phase;
  SourceHealth health;
  int64_t failInNs;
  bool policyApplies;
};

inline CaptureOnAirDecision decideCaptureOnAir(const modules::CaptureLiveness& liveness, int64_t nowNs) {
  using modules::CaptureLivenessState;
  if (liveness.state == CaptureLivenessState::Unreported) {
    return {CaptureOnAirPhase::Unreported, SourceHealth::Producing, 0, false};
  }

  const auto sampleAgeNs = (std::max)(int64_t{0}, nowNs - liveness.lastSampleNs);
  const auto silenceRemainingNs = kCaptureSilentFailNs - sampleAgeNs;
  const bool lost = liveness.state == CaptureLivenessState::Lost ||
      (liveness.lostAtNs != 0 && liveness.lastSampleNs <= liveness.lostAtNs);
  if (lost) {
    const auto lossAgeNs = (std::max)(int64_t{0}, nowNs - liveness.lostAtNs);
    const auto holdRemainingNs = kCaptureLostHoldNs - lossAgeNs;
    const auto remainingNs = liveness.cadenceTrusted
        ? (std::min)(holdRemainingNs, silenceRemainingNs) : holdRemainingNs;
    if (remainingNs <= 0) {
      return {CaptureOnAirPhase::Failed, SourceHealth::Failed, 0, true};
    }
    return {CaptureOnAirPhase::LostHolding, SourceHealth::Stalled, remainingNs, true};
  }

  if (liveness.cadenceTrusted && sampleAgeNs >= kCaptureStallNs) {
    if (silenceRemainingNs <= 0) {
      return {CaptureOnAirPhase::Failed, SourceHealth::Failed, 0, true};
    }
    return {CaptureOnAirPhase::Stalled, SourceHealth::Stalled, silenceRemainingNs, true};
  }
  return {CaptureOnAirPhase::Live, SourceHealth::Producing, 0, true};
}

inline const char* captureLivenessName(CaptureOnAirPhase phase) {
  switch (phase) {
    case CaptureOnAirPhase::Unreported: return "unreported";
    case CaptureOnAirPhase::Live: return "live";
    case CaptureOnAirPhase::Stalled: return "stalled";
    case CaptureOnAirPhase::LostHolding:
    case CaptureOnAirPhase::Failed: return "lost";
  }
  return "unreported";
}

inline const char* captureLossReasonName(modules::CaptureLossReason reason) {
  using modules::CaptureLossReason;
  switch (reason) {
    case CaptureLossReason::None: return "";
    case CaptureLossReason::DeviceRemoved: return "device-removed";
    case CaptureLossReason::DeviceInvalidated: return "device-invalidated";
    case CaptureLossReason::StreamError: return "stream-error";
    case CaptureLossReason::EndOfStream: return "end-of-stream";
    case CaptureLossReason::ReaderFailed: return "reader-failed";
    case CaptureLossReason::ShellReaderLost: return "shell-reader-lost";
    case CaptureLossReason::ShellStopped: return "shell-stopped";
  }
  return "";
}

}  // namespace corevideo::core
