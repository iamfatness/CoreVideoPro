#pragma once

#include <memory>
#include <string_view>

#include "modules/GpuVideoEncoder.h"

// Windows GPU-direct encoder (#521 slice 1): the Media Foundation hardware H.264
// MFT fed the compositor's D3D11 keyed-mutex program texture via an
// IMFDXGIDeviceManager, emitting an H.264 elementary-stream bitstream. Runs its
// own D3D11 device + thread (the vcam-tap pattern) — never coreMutex, never the
// render thread. On non-Windows this compiles to a stub whose factory returns
// nullptr, so the seam links everywhere and VideoToolbox (slice 4) drops in.

namespace corevideo::modules {

// Is a hardware H.264 encoder MFT available for this workload, with a free
// session? Feeds GpuEncodePathInputs (hardwareEncoderAvailable && sessionAvailable).
// Reuses EncoderCapacityProbe; never throws. Always false on non-Windows.
[[nodiscard]] bool mediaFoundationHardwareEncoderAvailable(int width, int height, int fps);

// Create the Windows GPU encoder. Returns nullptr on non-Windows (the caller then
// uses the CPU-pipe fallback). A non-null instance still reports failure through
// start() returning false if init fails at stream time.
[[nodiscard]] std::unique_ptr<GpuVideoEncoder> createMediaFoundationGpuVideoEncoder();

#if defined(_WIN32)
// #601: the product's rate-control vocabulary ("cbr" | "vbr" - the only two
// spellings normalizeRateControl() produces) mapped onto the Media Foundation
// codec-API rate-control mode. Exposed (rather than kept private to the .cpp)
// so the mapping is testable without a hardware encoder; the return value is an
// eAVEncCommonRateControlMode, widened to UINT32 so this header stays free of
// codecapi.h.
[[nodiscard]] unsigned int mediaFoundationRateControlMode(std::string_view rateControl);

// The peak bitrate to declare alongside the mean, in BITS per second. Mirrors
// the CPU path's rule in RtmpFfmpegArgs (maxrate = 1.5x the target under vbr,
// = the target under cbr) so both paths mean the same thing by "vbr".
[[nodiscard]] unsigned int mediaFoundationPeakBitrateBps(std::string_view rateControl,
                                                         int bitrateKbps);
#endif

}  // namespace corevideo::modules
