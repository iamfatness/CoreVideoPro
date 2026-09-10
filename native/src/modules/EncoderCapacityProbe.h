#pragma once

// PROBED encoder capacity, replacing the hard-coded literal that used to be
// handed to planIsoEncoders (`8 / 1 / hardware:true / software:true`).
//
// WHY THIS EXISTS
// ---------------
// Beta means 5-20 operators running real shows on machines we cannot see. The
// literal told every one of those machines it had eight hardware encode
// sessions. A laptop with an older integrated GPU and two — or zero, for the
// requested resolution — was told the same thing, and the overflow spilled
// silently onto the software MFT: CPU pegged, frames dropped, no explanation.
//
// WHAT THIS PROBE HONESTLY KNOWS, AND WHAT IT DOES NOT
// ----------------------------------------------------
// `production-realtime-architecture.md:136` is explicit: **hardware-session
// creation is not proof of sustainable capacity.** This probe establishes
// creation, and nothing more. It:
//
//   * identifies the DXGI adapter (description, vendor/device, LUID) so a
//     support bundle from a machine nobody watched names the actual GPU;
//   * enumerates the hardware video-encoder MFTs for the codec;
//   * for the ACTUAL width/height/fps of this recording, tries to configure a
//     hardware encoder and take it to NOTIFY_BEGIN_STREAMING — which is where
//     NVENC's driver-side session limit is enforced — repeatedly, counting how
//     many independent sessions can be *created* concurrently;
//   * asks whether an OS software H.264 MFT exists at all.
//
// The concurrent count is therefore a CEILING, flagged as such
// (`ceilingIsCreationProofOnly`), never a sustained-throughput guarantee. A
// machine that can create six sessions may still fall over encoding six 1080p60
// streams. Everything downstream must treat the number as an upper bound.
//
// It is also a ceiling AT A MOMENT IN TIME. The count is whatever the driver
// would still hand out while the probe ran, so anything else already holding
// encoder sessions (an egress ffmpeg, another application, a previous show)
// lowers it. Probing is suppressed while a recording is live for exactly that
// reason, but nothing outside this process can be suppressed. Measured on the
// dev rig: an idle RTX 4090 reports 8 (the probe's own cap) at 1080p30, and the
// same GPU reported 5 while the test suite held its own encoders open.
//
// The resolution/fps term is real and cheap: the sessions are configured at the
// recording's own frame size and rate, so a GPU whose encoder cannot take
// 4K60 reports a ceiling of zero for 4K60 while still reporting a healthy
// ceiling for 1080p30. That is the one extra term worth having here; the full
// PR-25 admission manager (GPU transfer, memory, storage throughput) is not.
//
// THREADING LAW
// -------------
// Probing costs COM activation, driver calls and GPU session churn — hundreds
// of milliseconds in the bad cases. It runs on a DETACHED BACKGROUND THREAD and
// NEVER under `coreMutex`, on the render thread, or on the audio worker
// (mirrors `startPluginHostScan` / `StillMediaFrameCache`). The recording-arm
// path only ever calls `lookup()`, which takes this cache's own leaf mutex for
// a map read and returns immediately — a miss is reported as "pending", not
// waited on.
//
// FAILURE IS NEVER FATAL
// ----------------------
// A probe that cannot run (no DXGI, no MF, disabled by env, still warming)
// yields `probed == false`. The caller then uses the SAME assumed capacity the
// product shipped with, so a tester whose GPU we cannot interrogate still runs
// their show — and the admission decision refuses to refuse anything on the
// strength of an assumption (see IsoEncoderAdmission.h).

#include "modules/IsoEncoderPlacement.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace corevideo::modules {

// The exact workload a capacity answer is valid for. Capacity is not a property
// of the machine alone — a GPU that sustains eight 1080p30 sessions may manage
// one at 4K60 — so the cache is keyed by the whole tuple.
struct EncoderProbeKey {
  std::string codec = "h264";  // canonical name ("h264" / "hevc")
  int width = 1920;
  int height = 1080;
  int fps = 30;

  bool operator<(const EncoderProbeKey& other) const {
    if (codec != other.codec) return codec < other.codec;
    if (width != other.width) return width < other.width;
    if (height != other.height) return height < other.height;
    return fps < other.fps;
  }

  std::string describe() const;
};

enum class EncoderProbeStatus {
  // Never asked for, or the background probe has not finished yet. Callers fall
  // back to the assumed capacity.
  Pending,
  // The probe ran and its numbers are usable.
  Ready,
  // The probe ran and could not determine capacity (no DXGI adapter, MF
  // enumeration failed). Callers fall back to the assumed capacity.
  Failed,
  // Turned off deliberately (COREVIDEO_ENCODER_PROBE=0), or this build/platform
  // has no probe. Callers fall back to the assumed capacity.
  Disabled,
};

const char* encoderProbeStatusId(EncoderProbeStatus status);

struct ProbedEncoderCapacity {
  EncoderProbeStatus status = EncoderProbeStatus::Pending;

  // True only when `status == Ready`. Gates every decision that is allowed to
  // REFUSE a workload: we never refuse a show on an assumption.
  bool probed = false;

  // --- adapter identity (also what a support bundle needs to name the GPU) ---
  std::string adapterDescription;  // e.g. "NVIDIA GeForce RTX 4090"
  std::uint64_t adapterLuid = 0;
  unsigned vendorId = 0;
  unsigned deviceId = 0;

  // --- capability for THIS key ---
  bool hardwareAvailable = false;
  bool softwareAvailable = false;
  std::string hardwareEncoderName;  // the MFT's friendly name, when we got one

  // How many independent hardware encoder sessions could be CREATED and taken
  // to begin-streaming concurrently at this key's size/rate. A ceiling.
  int hardwareSessionCeiling = 0;
  // Always true today. Kept explicit so the day someone adds a real sustained
  // throughput measurement, every consumer of the number can tell the
  // difference instead of inheriting a silent promotion.
  bool ceilingIsCreationProofOnly = true;
  // The cap the probe stopped counting at. If the ceiling equals this, the true
  // ceiling may be higher — we simply stopped asking.
  int probeSessionCap = 0;

  // Logical processors seen (cheap, no probing). Bounds how far an Auto spill
  // onto the software MFT may reasonably go; see softwareSessionBudget().
  unsigned logicalProcessors = 0;

  EncoderProbeKey key;
  std::int64_t probeDurationMs = 0;
  // Human-readable account of what happened, verbatim into logs/manifest. This
  // is the field that makes a failure on a machine nobody watched diagnosable.
  std::string detail;

  // A one-line summary for logs, the manifest and operator messages.
  std::string describe() const;
};

// The capacity the product assumed before probing existed. Kept as a NAMED
// fallback rather than a literal at the call site so "we are guessing" is
// visible in the code and in the manifest.
IsoEncoderCapacity assumedIsoEncoderCapacity(int reservedForProgram);

// Translate a probe result into the placement budget. `reservedForProgram` is
// the sessions Program itself owns (1 today). The software budget is derived
// from the CPU and the pixel rate — a CPU that can plausibly carry two 1080p30
// software encodes cannot carry six, and pretending otherwise is exactly the
// silent degradation this work removes.
IsoEncoderCapacity toIsoEncoderCapacity(const ProbedEncoderCapacity& probe, int reservedForProgram);

// Concurrent software ISO encodes this machine is admitted to attempt at the
// probed key. Returns -1 ("unbounded / unknown") when the probe did not run,
// so an assumption never causes a refusal. Pure; unit-tested.
int softwareSessionBudget(unsigned logicalProcessors, int width, int height, int fps);

// Process-wide cache. One entry per (codec,w,h,fps); invalidated wholesale when
// the DXGI adapter LUID changes underneath us (eGPU swap, driver reinstall,
// switchable-graphics flip).
class EncoderCapacityCache {
 public:
  static EncoderCapacityCache& instance();

  // NON-BLOCKING. Safe at the recording-arm call site (which runs under
  // coreMutex): a leaf-mutex map read, no COM, no driver calls, no waiting.
  // A miss returns status Pending and ALSO kicks the background probe, so the
  // next arm is warm.
  ProbedEncoderCapacity lookup(const EncoderProbeKey& key);

  // Start (or refresh) the background probe for `key`. Returns immediately.
  // Call it as early as the workload is known — sink construction for the
  // default profile, configureRecording for the real one — so the first arm of
  // a session finds a warm cache.
  void prewarm(const EncoderProbeKey& key);

  // Drop everything (device loss, adapter change, tests).
  void invalidate();

  // Suppress probing while a recording is live: the probe transiently occupies
  // hardware encoder sessions, which must never compete with a running show.
  void setRecordingActive(bool active);

  // Test seam: replace the platform probe. Mirrors StillMediaFrameCache's
  // injectable decoder so the cache and the decision logic are testable with no
  // GPU present.
  using ProbeFn = std::function<ProbedEncoderCapacity(const EncoderProbeKey&)>;
  void setProbeFunctionForTesting(ProbeFn fn);

  // Test seam: pin the answer SYNCHRONOUSLY for every workload, so a test that
  // arms a recording is not racing a background thread (or depending on the
  // GPU in the machine running CI). Structural, like
  // MediaCore::setStillImageDecoderForTest: no env var, command or wire field
  // reaches it. Pass nullptr to clear.
  void setForcedCapacityForTesting(const ProbedEncoderCapacity* capacity);

 private:
  EncoderCapacityCache() = default;

  void startProbeLocked(const EncoderProbeKey& key);

  std::mutex mutex_;
  std::map<EncoderProbeKey, ProbedEncoderCapacity> cache_;
  std::map<EncoderProbeKey, bool> inFlight_;
  std::uint64_t observedAdapterLuid_ = 0;
  bool haveObservedAdapter_ = false;
  bool recordingActive_ = false;
  bool forcedCapacityActive_ = false;
  ProbedEncoderCapacity forcedCapacity_;
  ProbeFn probeFn_;
};

// The platform probe itself. Windows + COREVIDEO_WITH_MF_ENCODER does the real
// DXGI/MFT work; every other configuration returns status Disabled. Exposed so
// a diagnostic command could call it directly — but NEVER from a hot path.
ProbedEncoderCapacity probeEncoderCapacity(const EncoderProbeKey& key);

}  // namespace corevideo::modules
