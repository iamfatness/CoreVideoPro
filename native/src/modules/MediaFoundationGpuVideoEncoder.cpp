#include "modules/MediaFoundationGpuVideoEncoder.h"

#include <algorithm>

#include "modules/EncoderCapacityProbe.h"

namespace corevideo::modules {

bool mediaFoundationHardwareEncoderAvailable(int width, int height, int fps) {
#if defined(_WIN32)
  if (width <= 0 || height <= 0 || fps <= 0) {
    return false;
  }
  // Reuse the ONE hardware-encoder probe (it enumerates the H.264 MFTs and takes
  // one to begin-streaming to prove a real session). lookup() is a leaf-mutex map
  // read that returns immediately; a miss kicks a background probe and reports
  // pending, so availability is false until the probe lands (the sender prewarms
  // it, matching the recording sink's prewarm). Never a false positive.
  const auto capacity = EncoderCapacityCache::instance().lookup(
      EncoderProbeKey{"h264", width, height, fps});
  return capacity.probed && capacity.hardwareAvailable && capacity.hardwareSessionCeiling > 0;
#else
  (void)width;
  (void)height;
  (void)fps;
  return false;
#endif
}

std::unique_ptr<GpuVideoEncoder> createMediaFoundationGpuVideoEncoder() {
  // Increment 3a: the seam, availability probe, path policy (T1), and bitstream
  // muxer (T2) are in place; the async-MFT + D3D11 encode core is the next
  // increment. Returning nullptr keeps the sender on the proven CPU-pipe path by
  // construction until the encode core lands, so the tree is always shippable.
  return nullptr;
}

}  // namespace corevideo::modules
