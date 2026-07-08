#pragma once

// The virtual camera's per-tick "what do I publish?" decision (virtual-camera
// -spec V3), as a pure function so the publisher's core logic is unit-tested:
//   - valid program frame -> convert BGRA->NV12, mirror if requested
//   - no/invalid program frame -> the standby slate (law 2: never a black frame)
// The publisher calls this and writes the result into the SHM slot.

#include "modules/VirtualCameraFrame.h"
#include "modules/VirtualCameraSlate.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace corevideo::modules {

struct VirtualCameraComposeResult {
  int width = 0;
  int height = 0;
  bool isSlate = false;  // true when the standby slate was served
};

// Compose the NV12 frame to publish at the target (even) dimensions. `bgra`
// may be null / zero-size (no program yet) -> the slate is served. When the
// program dimensions differ from the target, this still converts at the
// program's own size (the DLL/Frame Server scales); pass matching sizes for a
// 1:1 result. Returns the chosen dimensions and whether the slate was used.
inline VirtualCameraComposeResult composeVirtualCameraNv12(const std::uint8_t* bgra, int bgraWidth,
                                                           int bgraHeight, int targetWidth,
                                                           int targetHeight, bool mirror,
                                                           std::vector<std::uint8_t>& out) {
  VirtualCameraComposeResult result;
  const bool haveProgram = bgra != nullptr && bgraWidth >= 2 && bgraHeight >= 2;
  if (haveProgram) {
    const int w = bgraWidth & ~1;
    const int h = bgraHeight & ~1;
    if (convertBgraToNv12(bgra, w, h, out)) {
      if (mirror) {
        mirrorNv12InPlace(out.data(), w, h);
      }
      result.width = w;
      result.height = h;
      result.isSlate = false;
      return result;
    }
  }
  // Fall through to the standby slate at the target dimensions.
  const int sw = targetWidth >= 2 ? (targetWidth & ~1) : 1280;
  const int sh = targetHeight >= 2 ? (targetHeight & ~1) : 720;
  fillVirtualCameraStandbySlate(sw, sh, out);
  result.width = sw;
  result.height = sh;
  result.isSlate = true;
  return result;
}

}  // namespace corevideo::modules
