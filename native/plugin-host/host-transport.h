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

inline constexpr uint32_t kHostBlockMagic = 0x43565048;  // "CVPH"
inline constexpr int32_t kHostBlockMaxSamples = 8192;    // interleaved floats (e.g. 960 frames stereo = 1920)

struct HostAudioBlock {
  uint32_t magic = kHostBlockMagic;
  volatile uint32_t seqIn = 0;   // bumped by the core before signaling req
  volatile uint32_t seqOut = 0;  // set to seqIn by the host after processing
  int32_t sampleCount = 0;       // interleaved samples valid in pcm
  int32_t channels = 2;
  int32_t sampleRate = 48000;
  int32_t reserved = 0;
  float pcm[kHostBlockMaxSamples];
};

// Kernel object names derived from the operator-invisible instance name the
// core mints (per-process unique, mirroring the zoom-engine token pattern).
inline std::string hostShmName(const std::string& instance) { return "corevideo-vsthost-shm-" + instance; }
inline std::string hostReqEventName(const std::string& instance) { return "corevideo-vsthost-req-" + instance; }
inline std::string hostDoneEventName(const std::string& instance) { return "corevideo-vsthost-done-" + instance; }

}  // namespace corevideo::pluginhost
