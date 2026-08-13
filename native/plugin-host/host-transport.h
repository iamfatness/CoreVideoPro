#pragma once

// Plugin-host audio transport (VST spec P2, docs/vst-host-spec.md §2).
//
// ONE shared-memory block + two named events per host instance:
//   core:  write pcm + frames/channels, ++seqIn, SetEvent(req), wait done (deadline)
//   host:  wait req, process pcm IN PLACE, seqOut = seqIn, SetEvent(done)
// The core treats a missed deadline as BYPASS for that tick (the block is
// abandoned until seqOut catches up; stale completions are discarded by the
// sequence check). Fixed-size single-slot: one exchange in flight by
// construction — the audio worker is the only writer and it is synchronous.
//
// This header is shared by the host executable and the media core; keep it
// dependency-free (no SDK, no engine headers).

#include <cstdint>
#include <string>

namespace corevideo::pluginhost {

// v2 (P2c): the block carries a plugin SELECTION (core -> host) and a status
// back-channel (host -> core). The magic is bumped so a mismatched pair fails
// loudly (bad-magic serve error -> core bypasses forever with deadlineMisses
// climbing) instead of silently ignoring the selection.
// v3 (round-2 A2/A3): parameter surface (host -> core publish + core -> host
// set-param ring), component STATE get/set (single-shot area, see below), and
// plugin latencySamples telemetry. Magic bumped CVP2 -> CVP3 so a stale host
// executable against a new core (or vice versa) fails LOUDLY at serve start
// (bad-magic error + exit 3) instead of silently ignoring the new fields.
inline constexpr uint32_t kHostBlockMagic = 0x43565033;  // "CVP3"
inline constexpr int32_t kHostBlockMaxSamples = 8192;    // interleaved floats (e.g. 960 frames stereo = 1920)
inline constexpr int32_t kHostPluginPathMax = 512;       // NUL-terminated bundle path
inline constexpr int32_t kHostPluginNameMax = 128;       // NUL-terminated class name
inline constexpr int32_t kHostErrorMax = 256;            // NUL-terminated error text
inline constexpr int32_t kHostEditorIdle = 0;
inline constexpr int32_t kHostEditorOpen = 1;
inline constexpr int32_t kHostEditorFailed = 2;

// ---- v3 parameter surface -------------------------------------------------
// Plugins can expose HUNDREDS of parameters (Waves shells). The published
// surface is capped at the first kHostParamPublishMax by controller index;
// paramTotalCount always carries the real total so the UI can say
// "64 of 511 shown". The cap keeps the block bounded and the generic-slider
// flyout usable; the plugin's own editor remains the full surface.
inline constexpr int32_t kHostParamPublishMax = 64;
inline constexpr int32_t kHostParamTitleMax = 64;    // UTF-8 (ASCII-folded) title
inline constexpr int32_t kHostParamUnitsMax = 16;    // unit label ("dB", "%")
inline constexpr int32_t kHostParamDisplayMax = 24;  // formatted value ("‑3.0")

// Core -> host set-param requests ride a tiny latest-wins ring: the writer
// (control plane, one slider at a time) appends {id, normalized} and bumps
// paramSetSeq; the host drains from its cursor to seq. Ring depth 16 far
// exceeds one UI gesture between host wakes; overflow safely drops the OLDEST
// entries (latest value wins — correct for absolute-valued sliders).
inline constexpr int32_t kHostParamSetRing = 16;

// ---- v3 component state ---------------------------------------------------
// getState/setState blobs move through a fixed single-shot area. 1 MiB covers
// real-world channel/mastering plugin states (typically KBs) by orders of
// magnitude; a state larger than the area FAILS LOUDLY with its size in the
// error (chunking is deliberately not implemented — a simple bounded contract
// over a brittle multi-round protocol, per the house robustness rule).
inline constexpr int32_t kHostStateMaxBytes = 1 * 1024 * 1024;
inline constexpr int32_t kHostStateRequestNone = 0;
inline constexpr int32_t kHostStateRequestGet = 1;
inline constexpr int32_t kHostStateRequestSet = 2;
inline constexpr int32_t kHostStateResultOk = 0;
inline constexpr int32_t kHostStateResultFailed = 1;

struct HostParamEntry {
  uint32_t id = 0;
  int32_t stepCount = 0;   // 0 = continuous, 1 = toggle, N = N+1 discrete steps
  int32_t flags = 0;       // ParameterInfo.flags (kCanAutomate etc.), advisory
  int32_t reserved = 0;
  double normalized = 0.0;  // current value, [0,1]
  char title[kHostParamTitleMax] = {};
  char units[kHostParamUnitsMax] = {};
  char display[kHostParamDisplayMax] = {};  // plugin-formatted value string
};

struct HostParamSetEntry {
  uint32_t id = 0;
  uint32_t reserved = 0;
  double normalized = 0.0;
};

// Host status codes (statusCode field).
inline constexpr int32_t kHostStatusTestProcessor = 0;  // legacy -6dB test gain
inline constexpr int32_t kHostStatusPluginActive = 1;   // real VST3 plugin processed the block
inline constexpr int32_t kHostStatusPluginFailed = 2;   // load/process failed -> block BYPASSED untouched

struct HostAudioBlock {
  uint32_t magic = kHostBlockMagic;
  volatile uint32_t seqIn = 0;   // bumped by the core before signaling req
  volatile uint32_t seqOut = 0;  // set to the captured seqIn by the host after processing
  int32_t sampleCount = 0;       // interleaved samples valid in pcm
  int32_t channels = 2;
  int32_t sampleRate = 48000;
  int32_t reserved = 0;
  // P2c selection, written by the core together with seqIn. Empty bundle =
  // the legacy -6dB test processor (plain "vst"/"host" insert names).
  char pluginBundle[kHostPluginPathMax] = {};  // .vst3 bundle path (scan id)
  char pluginClass[kHostPluginNameMax] = {};   // class name inside the bundle ("" = first audio class)
  // P2c status, written by the host together with seqOut. statusGeneration
  // bumps only when the status CONTENT changes so the core copies strings
  // rarely, never per tick.
  volatile uint32_t statusGeneration = 0;
  int32_t statusCode = kHostStatusTestProcessor;
  char activePlugin[kHostPluginNameMax] = {};
  char lastError[kHostErrorMax] = {};
  // UI -> host editor request. It uses a dedicated named event so opening a
  // GUI never races or waits on the realtime audio request/done exchange.
  volatile uint32_t editorRequestGeneration = 0;
  char editorPluginBundle[kHostPluginPathMax] = {};
  char editorPluginClass[kHostPluginNameMax] = {};
  // host -> core editor result telemetry.
  volatile uint32_t editorStatusGeneration = 0;
  int32_t editorStatusCode = kHostEditorIdle;
  char editorActivePlugin[kHostPluginNameMax] = {};
  char editorLastError[kHostErrorMax] = {};

  // ---- v3: latency telemetry (host -> core). Reported by the ACTIVE audio
  // selection's processor (getLatencySamples); 0 for the test processor and
  // while nothing loaded. Changes bump statusGeneration (it rides the status
  // publish) so the core's rare-copy harvest picks it up without polling.
  volatile uint32_t latencySamples = 0;

  // ---- v3: parameter surface (host -> core). The host publishes the ACTIVE
  // selection's params (last audio selection processed; the editor selection
  // when no audio flows). paramListGeneration bumps on plugin/list changes
  // (titles, count); paramValuesGeneration bumps whenever any published value
  // or display string changed (editor knob moves land here). The core copies
  // only on generation change — never per tick.
  volatile uint32_t paramListGeneration = 0;
  volatile uint32_t paramValuesGeneration = 0;
  int32_t paramTotalCount = 0;      // real controller count (may exceed the cap)
  int32_t paramPublishedCount = 0;  // entries valid in params[]
  char paramPluginClass[kHostPluginNameMax] = {};  // whose params these are
  HostParamEntry params[kHostParamPublishMax] = {};

  // ---- v3: core -> host set-param ring (dedicated event, never the audio
  // req). paramSetPluginBundle/Class select the target slot; entries are
  // applied in order. The host is the value authority: applied values come
  // back through the published params above.
  volatile uint32_t paramSetSeq = 0;
  char paramSetPluginBundle[kHostPluginPathMax] = {};
  char paramSetPluginClass[kHostPluginNameMax] = {};
  HostParamSetEntry paramSet[kHostParamSetRing] = {};

  // ---- v3: component state get/set (dedicated request/done event pair; a
  // CONTROL-PLANE round trip — the audio worker never touches it). Single
  // request in flight by construction (the core serializes under a mutex).
  volatile uint32_t stateRequestGeneration = 0;
  int32_t stateRequestKind = kHostStateRequestNone;
  char statePluginBundle[kHostPluginPathMax] = {};
  char statePluginClass[kHostPluginNameMax] = {};
  volatile uint32_t stateResponseGeneration = 0;
  int32_t stateResultCode = kHostStateResultOk;
  char stateError[kHostErrorMax] = {};
  int32_t stateDataBytes = 0;  // set: payload in; get: payload out
  uint8_t stateData[kHostStateMaxBytes] = {};

  float pcm[kHostBlockMaxSamples];
};

// Bounded NUL-terminated copy into a fixed block field (both sides use this;
// no strncpy so the remainder is not zero-filled per tick).
template <int32_t Capacity>
inline void copyBlockString(char (&field)[Capacity], const char* text) {
  int32_t index = 0;
  if (text != nullptr) {
    for (; index < Capacity - 1 && text[index] != '\0'; ++index) {
      field[index] = text[index];
    }
  }
  field[index] = '\0';
}

// Kernel object names derived from the operator-invisible instance name the
// core mints (per-process unique, mirroring the zoom-engine token pattern).
inline std::string hostShmName(const std::string& instance) { return "corevideo-vsthost-shm-" + instance; }
inline std::string hostReqEventName(const std::string& instance) { return "corevideo-vsthost-req-" + instance; }
inline std::string hostDoneEventName(const std::string& instance) { return "corevideo-vsthost-done-" + instance; }
inline std::string hostEditorEventName(const std::string& instance) { return "corevideo-vsthost-editor-" + instance; }
// v3: set-param nudge (core -> host) and the state request/done pair. All
// separate from req/done so param/state traffic can NEVER stall the realtime
// audio exchange.
inline std::string hostParamEventName(const std::string& instance) { return "corevideo-vsthost-param-" + instance; }
inline std::string hostStateReqEventName(const std::string& instance) { return "corevideo-vsthost-statereq-" + instance; }
inline std::string hostStateDoneEventName(const std::string& instance) { return "corevideo-vsthost-statedone-" + instance; }

}  // namespace corevideo::pluginhost
