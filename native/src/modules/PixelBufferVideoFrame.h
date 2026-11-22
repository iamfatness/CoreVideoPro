#pragma once
// A decoded picture that stays where the decoder put it (macOS media
// zero-copy, plan 2026-11-21 N4): an IOSurface-backed 32BGRA CVPixelBuffer,
// RETAINED for as long as any VideoFrame (or a Metal pass, see
// compositor/MetalPixelBufferTextures.h) still refers to it.
//
// WHY RETAINING IS THE WHOLE CONTRACT. The buffer comes from the decoder's
// pool. The pool reuses a buffer the moment its retain count drops to zero, and
// the decoder then writes the NEXT picture into the same IOSurface. Anything
// still sampling it at that point shows a recycled (or half-written, torn)
// frame. So a holder never keeps a raw CVPixelBufferRef: it keeps this object
// through a shared_ptr, and the buffer goes back to the pool only when the
// last holder lets go.
//
// Apple only; included from Objective-C++ and C++ translation units.
#if defined(__APPLE__)

#include <CoreVideo/CoreVideo.h>

#include <memory>

#include "modules/GpuVideoFrame.h"

namespace corevideo::modules {

class PixelBufferVideoFrame final : public GpuVideoFrame {
 public:
  explicit PixelBufferVideoFrame(CVPixelBufferRef buffer) : buffer_(CVPixelBufferRetain(buffer)) {
    backend = Backend::AppleIOSurfaceBgra;
    width = static_cast<int>(CVPixelBufferGetWidth(buffer));
    height = static_cast<int>(CVPixelBufferGetHeight(buffer));
  }
  ~PixelBufferVideoFrame() override { CVPixelBufferRelease(buffer_); }
  PixelBufferVideoFrame(const PixelBufferVideoFrame&) = delete;
  PixelBufferVideoFrame& operator=(const PixelBufferVideoFrame&) = delete;

  // Borrowed: valid while this object lives. Never stored raw by a caller.
  [[nodiscard]] CVPixelBufferRef pixelBuffer() const { return buffer_; }

 private:
  CVPixelBufferRef buffer_;
};

// The only way in: refuses (nullptr) anything that is not an IOSurface-backed
// 32BGRA buffer, so a consumer that sees backend AppleIOSurfaceBgra can wrap
// the surface as a BGRA texture without checking again.
inline std::shared_ptr<const GpuVideoFrame> makePixelBufferVideoFrame(CVPixelBufferRef buffer) {
  if (!buffer || CVPixelBufferGetIOSurface(buffer) == nullptr ||
      CVPixelBufferGetPixelFormatType(buffer) != kCVPixelFormatType_32BGRA ||
      CVPixelBufferGetWidth(buffer) == 0 || CVPixelBufferGetHeight(buffer) == 0) {
    return nullptr;
  }
  return std::make_shared<const PixelBufferVideoFrame>(buffer);
}

}  // namespace corevideo::modules

#endif  // __APPLE__
