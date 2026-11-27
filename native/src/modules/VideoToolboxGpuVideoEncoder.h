#pragma once

// The macOS GPU-direct hardware encoder (macOS parity Phase 4, Task 2 / O4a):
// VideoToolbox behind the platform-neutral GpuVideoEncoder seam, fed the
// compositor's encoder IOSurface (ProgramFrame::encoderSharedTexture.iosurfaceId)
// instead of a CPU readback piped to ffmpeg.
//
// Behaviour (the plan's contract, mirrored from MediaFoundationGpuVideoEncoder
// where the two must agree):
//   - start: H.264 or HEVC only, hardware required, real time, NO frame
//     reordering (D2: FLV refuses reordered raw HEVC; GpuVideoEncoderConfig has
//     no B-frames field, so Windows GPU-direct is B-frame-free too), BT.709
//     colour, CBR via kVTCompressionPropertyKey_ConstantBitRate (Task 0 row
//     E-2: accepted for hardware H.264 and HEVC at 1080p60 on Apple Silicon).
//   - submit never blocks: the frame is handed to a serial queue that waits up
//     to 34 ms for the compositor's publishedFrameNumber (the Windows keyed-mutex
//     consumer timeout), looks the IOSurface up, and encodes. At most two
//     frames are in flight; a third, a missing surface or a wait timeout is a
//     counted SKIP, never a failure.
//   - PTS = frameNumber * 10'000'000 / fps (100 ns), the same absolute epoch as
//     MediaFoundationGpuVideoEncoder.cpp:685.
//   - Output is Annex B; a keyframe carries its parameter sets first.
//   - healthy() goes false on kVTInvalidSessionErr / kVTVideoEncoderMalfunctionErr
//     or 30 consecutive encode errors; submit then returns false.
//   - stop drains, completes, invalidates; idempotent; never touches coreMutex.

#include "modules/GpuVideoEncoder.h"

#include <cstdint>
#include <memory>

namespace corevideo::modules {

class VideoToolboxGpuVideoEncoder final : public GpuVideoEncoder {
 public:
  VideoToolboxGpuVideoEncoder();
  ~VideoToolboxGpuVideoEncoder() override;

  [[nodiscard]] bool start(const GpuVideoEncoderConfig& config, GpuEncodedChunkSink sink) override;
  [[nodiscard]] bool submit(const GpuVideoEncoderFrame& frame) override;
  [[nodiscard]] bool requestKeyframe() override;
  [[nodiscard]] int programTickRate(const GpuVideoEncoderConfig&) const override { return kProgramTickRate; }
  void stop() override;
  [[nodiscard]] bool healthy() const override;
  [[nodiscard]] std::string lastFailure() const override;

  // Frames dropped without encoding (missing surface, publish-wait timeout,
  // in-flight limit). Diagnostics and tests.
  [[nodiscard]] int64_t framesSkipped() const;
  // Frames handed to VideoToolbox (or queued for it) whose output has not come back.
  [[nodiscard]] int framesInFlight() const;

  struct Impl;

 private:
  std::shared_ptr<Impl> impl_;
};

// nullptr on any platform without the VideoToolbox encoder build.
[[nodiscard]] std::unique_ptr<GpuVideoEncoder> createVideoToolboxGpuVideoEncoder();

}  // namespace corevideo::modules
