#pragma once

#include <memory>

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

}  // namespace corevideo::modules
