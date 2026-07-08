#pragma once

// Shared-memory layout for the virtual camera (docs/virtual-camera-spec.md V2).
// The core (corevideo-native.exe) publishes the program frame as NV12 here; the
// virtual-camera COM DLL (loaded in frameserver.exe) reads the latest complete
// frame. Single latest-wins slot with a SEQLOCK: a camera only ever needs the
// newest frame, and the seqlock guarantees the reader never sees a torn frame
// (seq odd = writer mid-update, even = complete). Same discipline as the audio
// ring. This header is shared verbatim by the core and the DLL.

#include <cstddef>
#include <cstdint>
#include <string>

namespace corevideo::modules {

inline constexpr std::uint32_t kVirtualCameraMagic = 0x43564643u;  // 'CVFC'
inline constexpr int kVirtualCameraMaxWidth = 1920;
inline constexpr int kVirtualCameraMaxHeight = 1080;

// NV12 max payload (1080p) = w*h*3/2.
inline constexpr std::size_t kVirtualCameraMaxPayload =
    static_cast<std::size_t>(kVirtualCameraMaxWidth) * kVirtualCameraMaxHeight * 3 / 2;

#pragma pack(push, 4)
struct VirtualCameraShmHeader {
  std::uint32_t magic;         // kVirtualCameraMagic once initialized
  volatile std::uint32_t seq;  // seqlock: odd = writing, even = complete
  std::int32_t width;          // current frame dimensions
  std::int32_t height;
  std::int32_t fps;
  std::uint32_t byteLen;       // NV12 bytes valid in the payload
  std::uint64_t frameNumber;   // monotonic, for the reader's drop/liveness telemetry
};
#pragma pack(pop)

inline std::size_t virtualCameraShmSize() {
  return sizeof(VirtualCameraShmHeader) + kVirtualCameraMaxPayload;
}

// Fixed name for the region (single virtual camera per machine session). The
// core creates it; the DLL opens it read-only.
inline std::string virtualCameraShmName() {
  return "Local\\CoreVideoProVirtualCamera";
}

}  // namespace corevideo::modules
