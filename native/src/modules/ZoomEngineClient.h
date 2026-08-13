#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace corevideo::modules {

struct ZoomEngineInitCommand {
  std::string jwt;
  std::string publicAppKey;
};

struct ZoomEngineJoinCommand {
  std::string meetingId;
  std::string passcode;
  std::string displayName = "CoreVideo Pro";
  std::string onBehalfToken;
  std::string userZak;
  std::string appPrivilegeToken;
};

struct ZoomEngineSubscribeCommand {
  std::string sourceUuid;
  std::uint32_t participantId = 0;
  // Request 1080p from Zoom (2 = ZoomSDKResolution_1080P). Zoom adaptively delivers
  // up to this based on the sender + conditions; the engine falls back to 720/360 if
  // 1080p subscribe fails. 720p (1) was capping every participant below 1080p.
  std::uint32_t resolution = 2;
  std::string mode;
  bool isolateAudio = false;
  bool audienceAudio = false;
};

struct ZoomEngineParticipant {
  std::uint32_t id = 0;
  std::string displayName;
  bool hasVideo = false;
  bool isTalking = false;
  bool isMuted = false;
  bool isSharingScreen = false;
};

enum class ZoomEngineEventKind {
  Unknown,
  Ping,
  Debug,
  Ready,
  AuthOk,
  AuthFail,
  Joined,
  Left,
  Participants,
  ActiveSpeaker,
  Frame,
  Audio,
  RawMediaStatus,
  Error,
};

struct ZoomEngineEvent {
  ZoomEngineEventKind kind = ZoomEngineEventKind::Unknown;
  std::string command;
  std::string sourceUuid;
  std::string stage;
  std::string message;
  std::uint32_t participantId = 0;
  std::uint32_t activeSpeakerId = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t byteLength = 0;
  std::size_t participantCount = 0;
  bool ok = false;
  // RawMediaStatus events only: engine-reported raw-media state ("active" field
  // of the raw_media_status event). True after StartRawRecording succeeds and
  // subscriptions re-arm; false after stop_raw_media (StopRawRecording +
  // unsubscribe_all) completes on the engine's command loop.
  bool rawMediaActive = false;
  std::vector<ZoomEngineParticipant> participants;
};

struct ZoomEngineRgbaFrame {
  std::string sourceUuid;
  std::string participantId;
  // Dimensions of the (downscaled) BGRA `rgba` thumbnail below.
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t frameId = 0;
  // Downscaled BGRA thumbnail (capped at the maxWidth/maxHeight passed to
  // readZoomEngineI420FrameSnapshot) for the WinUI base64 multiview path.
  std::vector<std::uint8_t> rgba;
  // Full-resolution I420 (YUV 4:2:0 planar) planes copied verbatim from shared
  // memory: Y (i420Width*i420Height) then U then V. The compositor uploads these
  // to GPU textures and converts to RGB in-shader, so the per-pixel CPU convert
  // is no longer paid at full resolution (only for the small thumbnail above).
  std::vector<std::uint8_t> i420;
  std::uint32_t i420Width = 0;
  std::uint32_t i420Height = 0;
};

std::string buildZoomEngineInitCommand(const ZoomEngineInitCommand& command);
std::string buildZoomEngineJoinCommand(const ZoomEngineJoinCommand& command);
std::string buildZoomEngineLeaveCommand();
std::string buildZoomEngineStartMediaCommand();
std::string buildZoomEngineStopMediaCommand();
std::string buildZoomEngineQuitCommand();
std::string buildZoomEngineSubscribeCommand(const ZoomEngineSubscribeCommand& command);
std::string buildZoomEngineSubscribeAudioCommand(const ZoomEngineSubscribeCommand& command);
std::string buildZoomEngineUnsubscribeCommand(const std::string& sourceUuid);

std::optional<ZoomEngineEvent> parseZoomEngineEvent(const std::string& line);

// SHM region names for a source. instanceToken must match the token the engine
// was launched with (ZoomEngineProcessClient::instanceToken); empty reproduces
// the legacy fixed names.
std::string zoomEngineVideoSharedMemoryName(const std::string& sourceUuid,
                                            const std::string& instanceToken = {});
std::string zoomEngineAudioSharedMemoryName(const std::string& sourceUuid,
                                            const std::string& instanceToken = {});
std::size_t zoomEngineI420FrameByteSize(std::uint32_t width, std::uint32_t height);
std::size_t zoomEnginePcmAudioByteSize(std::uint32_t byteLength);
// Video-beacon fix: cheap header peek - returns the snapshot sequence (even =
// complete) so pollers skip the full-frame copy when nothing changed.
std::uint32_t readZoomEngineI420FrameSequence(const void* sharedMemory, std::size_t sharedMemorySize);

// buildThumbnail=false skips the (downscaled) RGBA convert entirely — the frame
// then carries only the full-res I420 planes for the compositor. Callers use it
// to pace thumbnail work/events without touching the real video path.
std::optional<ZoomEngineRgbaFrame> readZoomEngineI420FrameSnapshot(
    const void* sharedMemory,
    std::size_t sharedMemorySize,
    const std::string& sourceUuid,
    std::uint32_t participantId,
    std::uint32_t maxWidth,
    std::uint32_t maxHeight,
    bool buildThumbnail = true);

// One PCM chunk snapshot-read from an engine audio SHM region (single-slot
// seqlock, same tear protocol as the video frames: odd sequence = mid-write).
// The engine writes 16-bit signed little-endian interleaved PCM; the snapshot
// converts to interleaved float in [-1, 1] so it can flow straight into an
// AudioFrame's `pcm`.
struct ZoomEnginePcmAudioChunk {
  std::uint32_t sequence = 0;
  int sampleRate = 0;
  int channels = 0;
  std::vector<float> pcm;
};

std::optional<ZoomEnginePcmAudioChunk> readZoomEnginePcmAudioSnapshot(
    const void* sharedMemory, std::size_t sharedMemorySize);

// Ring reader (2026-07-05 Zoom-garble fix): drains ALL packets written since
// `nextReadCounter` (up to one ring of catch-up), appending decoded chunks to
// `out` in order and advancing the counter. Returns the number of packets
// LOST (overwritten before this reader caught up, or torn) - callers log it.
std::size_t readZoomEnginePcmAudioRing(const void* sharedMemory, std::size_t sharedMemorySize,
                                       std::uint64_t& nextReadCounter,
                                       std::vector<ZoomEnginePcmAudioChunk>& out);

// Fixed region size of the ring layout (both sides of the SHM agree via
// zoom-engine/shared/engine-ipc.h).
std::size_t zoomEngineAudioRingByteSize();

// Per-source pending-PCM accumulator for the runtime's audio ingest. Chunks
// append in arrival order; the mixer drains the whole buffer once per audio
// tick (one coalesced AudioFrame per source per poll, so multiple 10ms Zoom
// packets per ~20ms tick never overlap-sum). Bounded: when a slow consumer
// lets more than `maxSamples` per channel accumulate, the OLDEST samples are
// dropped (latest-wins, mirrors the video path) and counted.
struct ZoomEnginePendingAudio {
  int sampleRate = 0;
  int channels = 0;
  std::uint32_t lastSequence = 0;
  // Ring read cursor: packets consumed from the source ring so far.
  std::uint64_t nextReadCounter = 0;
  std::int64_t lostPackets = 0;
  std::int64_t droppedSamples = 0;
  std::int64_t ingestedChunks = 0;
  std::vector<float> pcm;
};

// Appends `chunk` to `pending`, resetting the buffer when the format changes
// and enforcing the per-channel `maxSamples` cap. Returns false (and leaves
// `pending` unchanged) for chunks with no samples or an invalid format.
bool appendZoomEnginePcmChunk(ZoomEnginePendingAudio& pending,
                              const ZoomEnginePcmAudioChunk& chunk,
                              std::size_t maxSamplesPerChannel);

}  // namespace corevideo::modules
