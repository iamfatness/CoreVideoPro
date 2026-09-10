#pragma once

// ADMIT OR REFUSE an ISO recording workload, given a placement plan and the
// provenance of the capacity it was planned against.
//
// `IsoEncoderPlacement.h` decides WHERE each ISO track encodes. It is pure and
// stays pure. This file decides whether the resulting plan is one we are
// willing to run, and — when it is not what the operator asked for — what to
// say about it. The split matters: placement is arithmetic, admission is
// policy, and only the policy needs to know whether the numbers it was handed
// were measured or guessed.
//
// THE RULE THIS ENFORCES
// ----------------------
// `docs/production-realtime-execution-plan.md`: an unsupported workload is
// rejected before the show, not silently degraded during it. Today an
// over-subscribed machine spills auto placement onto the software MFT and the
// operator finds out afterwards, from a `fallbackReason` in a manifest they
// will never open. So:
//
//   * tracks that could not be placed at all  -> REFUSE ISO (program records)
//   * a spill larger than the machine's software budget -> REFUSE ISO, and say
//     how many ISO sources it CAN take
//   * a spill within budget -> ADMIT, but loudly: `recording.warning`
//
// AND THE RULE THAT PROTECTS TESTERS
// ----------------------------------
// **We never refuse a show on the strength of an assumption.** If the capacity
// came from the shipped fallback rather than a probe (`IsoCapacitySource::
// probed == false` — probe pending, probe failed, probe disabled, no GPU we can
// interrogate), the verdict may warn but may NOT refuse. Degrading to today's
// behaviour and saying so is the honest outcome; blocking someone's show
// because we could not read their driver is not.
//
// Precedent for the shape of the refusal: MediaFoundationEncoderAdapter already
// disables ISO while letting Program proceed when the target folder is not
// writable. Same channel (`recording.warning`), same priority-1 treatment of
// Program.

#include "modules/IsoEncoderPlacement.h"

#include <algorithm>
#include <string>
#include <vector>

namespace corevideo::modules {

// Where the IsoEncoderCapacity came from. Carried separately from the capacity
// itself so planIsoEncoders stays free of provenance.
struct IsoCapacitySource {
  // True ONLY when a probe actually measured this machine. Gates refusal.
  bool probed = false;
  // "ready" | "pending" | "failed" | "disabled" — verbatim into the manifest.
  std::string status = "pending";
  // e.g. "NVIDIA GeForce RTX 4090"; empty when unknown.
  std::string adapterDescription;
  // True while the session count is creation-proof only, i.e. an upper bound
  // rather than a sustained-throughput guarantee. Shapes the wording so an
  // operator is never told a ceiling is a promise.
  bool ceilingIsCreationProofOnly = true;
  // Human-readable workload label, e.g. "h264 1920x1080@30".
  std::string workload;
};

enum class IsoAdmissionDecision {
  // Every ISO track lands where it should. Arm silently.
  Admit,
  // ISO arms, but not the way the operator would assume. Loud warning first.
  AdmitWithWarning,
  // ISO does not arm at all. Program still records.
  RefuseIso,
};

struct IsoAdmissionVerdict {
  IsoAdmissionDecision decision = IsoAdmissionDecision::Admit;
  int hardwareCount = 0;
  int softwareCount = 0;
  int unplacedCount = 0;
  // How many ISO sources this machine is admitted to run at this workload.
  // -1 when unknown (unprobed). Present so the refusal is ACTIONABLE: the
  // operator is told the number to come down to, not merely that they failed.
  int admissibleIsoCount = -1;
  // Empty for Admit; operator-facing otherwise. Goes to `recording.warning`.
  std::string message;
  // Stable, greppable classifier for logs and support bundles.
  std::string code;

  bool armIso() const { return decision != IsoAdmissionDecision::RefuseIso; }
  bool shouldWarn() const { return decision != IsoAdmissionDecision::Admit; }
};

namespace detail {

inline std::string isoCountLabel(int count) {
  return std::to_string(count) + (count == 1 ? " ISO source" : " ISO sources");
}

inline std::string onThisMachine(const IsoCapacitySource& source) {
  if (source.adapterDescription.empty()) {
    return "this machine";
  }
  return source.adapterDescription;
}

}  // namespace detail

// How many ISO sources the machine is admitted to run: the hardware budget
// left after Program's reservation, plus the software spill budget. Returns -1
// when the software budget is unbounded/unknown (the unprobed case), which the
// verdict reports as "unknown" rather than inventing a number.
inline int admissibleIsoSourceCount(const IsoEncoderCapacity& capacity, IsoEncoderMode mode) {
  const int hardwareBudget =
      capacity.hardwareAvailable
          ? (std::max)(0, capacity.hardwareSessionLimit - (std::max)(0, capacity.reservedHardwareSessions))
          : 0;
  if (mode == IsoEncoderMode::Hardware) {
    return hardwareBudget;
  }
  if (!capacity.softwareAvailable) {
    return hardwareBudget;
  }
  if (capacity.softwareSessionLimit < 0) {
    return -1;  // unbounded / unknown
  }
  if (mode == IsoEncoderMode::Software) {
    // Operator intent is software: hardware sessions are not on the table, so
    // the number they need to hear is the software budget alone.
    return capacity.softwareSessionLimit;
  }
  return hardwareBudget + capacity.softwareSessionLimit;
}

// The decision. Pure — no GPU, no clock, no I/O — so the whole policy is unit
// testable, in the shape the repo already uses for CaptureReaderStallPolicy,
// NativeUvcCapturePolicy, PresentationAttempt, DeviceLossPolicy and
// OutputLifecyclePolicy.
inline IsoAdmissionVerdict evaluateIsoAdmission(const std::vector<IsoEncoderAssignment>& plan,
                                                IsoEncoderMode mode,
                                                const IsoEncoderCapacity& capacity,
                                                const IsoCapacitySource& source) {
  IsoAdmissionVerdict verdict;
  for (const auto& assignment : plan) {
    switch (assignment.path) {
      case IsoEncoderPath::Hardware:
        ++verdict.hardwareCount;
        break;
      case IsoEncoderPath::Software:
        ++verdict.softwareCount;
        break;
      case IsoEncoderPath::Unavailable:
        ++verdict.unplacedCount;
        break;
    }
  }
  verdict.admissibleIsoCount = admissibleIsoSourceCount(capacity, mode);

  if (plan.empty()) {
    return verdict;  // Nothing selected; nothing to admit.
  }

  const std::string workload = source.workload.empty() ? std::string("this recording") : source.workload;

  // 1. Tracks with nowhere to go. This is already a hard failure today (they
  //    would write nothing); make it a refusal with a reason instead of N
  //    silently dead stems.
  if (verdict.unplacedCount > 0) {
    verdict.decision = IsoAdmissionDecision::RefuseIso;
    verdict.code = "iso-encoder-unplaceable";
    verdict.message = "ISO recording disabled: " + detail::isoCountLabel(verdict.unplacedCount) +
                      " could not be given an encoder for " + workload + " on " +
                      detail::onThisMachine(source) +
                      ". Program is still recording. Reduce ISO sources, lower the recording "
                      "resolution or frame rate, or switch ISO encoding to Auto.";
    return verdict;
  }

  if (verdict.softwareCount == 0) {
    return verdict;  // Everything on hardware as intended.
  }

  // 2. A spill happened. Whether it is tolerable depends on whether we MEASURED
  //    this machine. An assumption may warn; it may never refuse.
  const bool overBudget = source.probed && capacity.softwareSessionLimit >= 0 &&
                          verdict.softwareCount > capacity.softwareSessionLimit;

  if (overBudget) {
    verdict.decision = IsoAdmissionDecision::RefuseIso;
    verdict.code = "iso-encoder-oversubscribed";
    verdict.message =
        "ISO recording disabled: " + std::to_string(verdict.hardwareCount + verdict.softwareCount) +
        " ISO sources at " + workload + " need " + detail::isoCountLabel(verdict.softwareCount) +
        " on the CPU software encoder, which " + detail::onThisMachine(source) +
        " cannot sustain. Program is still recording. This machine is good for about " +
        std::to_string((std::max)(0, verdict.admissibleIsoCount)) + " ISO sources at " + workload +
        " — reduce the selection, or lower the recording resolution or frame rate.";
    return verdict;
  }

  // 3. Tolerable spill — but never silent. This is the case that used to be
  //    discoverable only afterwards, in the manifest.
  verdict.decision = IsoAdmissionDecision::AdmitWithWarning;
  verdict.code = source.probed ? "iso-encoder-software-spill" : "iso-encoder-software-spill-unprobed";
  verdict.message = detail::isoCountLabel(verdict.softwareCount) +
                    " will record on the CPU software encoder, not the GPU";
  if (verdict.hardwareCount > 0) {
    verdict.message += " (" + std::to_string(verdict.hardwareCount) + " on hardware)";
  }
  verdict.message += ": ";
  if (!capacity.hardwareAvailable) {
    verdict.message += "no hardware encoder is available for " + workload + " on " +
                       detail::onThisMachine(source) + ".";
  } else {
    verdict.message += detail::onThisMachine(source) + " has " +
                       std::to_string((std::max)(0, capacity.hardwareSessionLimit)) +
                       " hardware encode session(s) for " + workload + " and Program owns " +
                       std::to_string((std::max)(0, capacity.reservedHardwareSessions)) + ".";
  }
  if (!source.probed) {
    verdict.message += " Encoder capacity could not be measured on this machine (probe " +
                       (source.status.empty() ? std::string("pending") : source.status) +
                       "), so this uses the assumed default — expect higher CPU load than the "
                       "GPU path and watch for dropped frames.";
  } else if (source.ceilingIsCreationProofOnly) {
    verdict.message +=
        " The hardware session count is what this machine could CREATE, not a guarantee it can "
        "sustain them — watch for dropped frames.";
  }
  return verdict;
}

}  // namespace corevideo::modules
