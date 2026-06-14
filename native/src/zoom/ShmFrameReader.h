#pragma once

#include <cstdint>
#include <vector>

namespace corevideo::zoom {

// A single decoded planar I420 frame copied out of the engine's shared memory.
// `i420` holds Y (width*height) followed by U and V (each width*height/4).
struct DecodedI420Frame {
  uint32_t sequence = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> i420;
};

enum class FrameReadStatus {
  Ok,          // a complete, consistent frame was decoded
  Empty,       // header present but no frame yet (zero dimensions)
  InProgress,  // writer mid-update (odd sequence or torn read) — retry
  Malformed    // buffer too small or internally inconsistent
};

// Header layout written by the engine (see zoom-engine/shared/engine-ipc.h
// `ShmFrameHeader`): four little-endian uint32 — sequence, width, height, y_len.
constexpr std::size_t kShmFrameHeaderBytes = 16;

// Decode one frame from a shared-memory snapshot laid out as the header above
// followed by tightly packed Y + U + V planes. Implements the engine's
// lock-free protocol: a frame is only valid when `sequence` is even and
// unchanged across the copy. `base`/`size` describe the mapped region; the
// function never reads past `size`.
FrameReadStatus tryReadFrame(const uint8_t* base, std::size_t size, DecodedI420Frame& out);

}  // namespace corevideo::zoom
