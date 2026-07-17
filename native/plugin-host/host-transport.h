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
inline constexpr uint32_t kHostBlockMagic = 0x43565032;  // "CVP2"
inline constexpr int32_t kHostBlockMaxSamples = 8192;    // interleaved floats (e.g. 960 frames stereo = 1920)
inline constexpr int32_t kHostPluginPathMax = 512;       // NUL-terminated bundle path
inline constexpr int32_t kHostPluginNameMax = 128;       // NUL-terminated class name
inline constexpr int32_t kHostErrorMax = 256;            // NUL-terminated error text
inline constexpr int32_t kHostEditorIdle = 0;
inline constexpr int32_t kHostEditorOpen = 1;
inline constexpr int32_t kHostEditorFailed = 2;

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

}  // namespace corevideo::pluginhost
