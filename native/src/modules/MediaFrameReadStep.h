#pragma once

// One step of a raw-frame pipe reader: read exactly one frame into a fresh buffer.
//
// #728 (live 2026-10-01). The FFmpeg media decoder allocated one 1920x1080 BGRA
// buffer (about 8.3 MB) per frame with a throwing allocator on its own thread.
// Windows ran low on virtual memory, the allocation threw std::bad_alloc, nothing
// on that thread caught it, and the core terminated: Program, the recording, the
// streams and the Zoom session, three times in 2.5 minutes, for one frame of a
// looping background clip. The process itself held about 1 GB.
//
// A frame that cannot be allocated is DROPPED. Its bytes are still read off the
// pipe, into a small stack buffer, because the pipe carries no framing: skipping
// the read would shift every later frame by a partial picture. The caller keeps
// showing the frame it already has.
//
// Pure and header-only (the FfmpegSenderDiagnostics shape) so the failure is
// testable without a process or a low-memory machine.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace corevideo::modules {

using MediaFrameBuffer = std::shared_ptr<std::vector<std::uint8_t>>;

enum class MediaFrameReadResult {
  Frame,    // `out` holds one whole frame
  Dropped,  // the frame was consumed from the pipe but could not be allocated
  Ended,    // the pipe closed before a whole frame arrived
};

// The production allocator: a null buffer instead of an exception.
inline MediaFrameBuffer tryAllocateMediaFrame(std::size_t bytes) noexcept {
  try {
    return std::make_shared<std::vector<std::uint8_t>>(bytes);
  } catch (const std::bad_alloc&) {
    return nullptr;
  } catch (const std::length_error&) {
    return nullptr;
  }
}

// Reuses a frame buffer once nobody else holds it.
//
// The clip reader used to allocate a fresh frame for EVERY frame: 8.3 MB sixty times a
// second, about 500 MB/s of allocate-and-free, each one a chance to fail on a machine that
// is short of memory (#728). A published frame is shared with the compositor, which may hold
// it for a tick or for as long as the clip is paused, so a buffer is only taken back when
// this recycler is its sole owner. Not thread-safe: it lives on the reader thread.
class MediaFrameRecycler {
 public:
  // The frame that was just replaced as "latest". It is reused later if every other holder
  // has let go of it by then.
  void offer(MediaFrameBuffer retired) noexcept { retired_ = std::move(retired); }

  // A buffer of exactly `bytes`: the retired one when this is its only owner, else a new one,
  // else null.
  MediaFrameBuffer take(std::size_t bytes) noexcept {
    if (retired_ && retired_.use_count() == 1 && retired_->size() == bytes) {
      ++reused_;
      return std::move(retired_);
    }
    return tryAllocateMediaFrame(bytes);
  }

  std::uint64_t reused() const noexcept { return reused_; }

 private:
  MediaFrameBuffer retired_;
  std::uint64_t reused_ = 0;
};

// `read(destination, wanted)` returns the bytes read, 0 when the pipe is closed.
// `allocate(bytes)` returns a buffer of that size, or null.
template <typename Read, typename Allocate>
MediaFrameReadResult readMediaFrame(std::size_t frameBytes, Read&& read, Allocate&& allocate,
                                    MediaFrameBuffer& out) {
  constexpr std::size_t kMaxChunk = static_cast<std::size_t>(1) << 20;
  out.reset();
  auto frame = allocate(frameBytes);
  if (frame) {
    std::size_t offset = 0;
    while (offset < frameBytes) {
      const std::size_t got = read(frame->data() + offset, (std::min)(frameBytes - offset, kMaxChunk));
      if (got == 0) return MediaFrameReadResult::Ended;
      offset += got;
    }
    out = std::move(frame);
    return MediaFrameReadResult::Frame;
  }

  std::array<std::uint8_t, 64 * 1024> scratch;
  std::size_t remaining = frameBytes;
  while (remaining > 0) {
    const std::size_t got = read(scratch.data(), (std::min)(remaining, scratch.size()));
    if (got == 0) return MediaFrameReadResult::Ended;
    remaining -= got;
  }
  return MediaFrameReadResult::Dropped;
}

}  // namespace corevideo::modules
