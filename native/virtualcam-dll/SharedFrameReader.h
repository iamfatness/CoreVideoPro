#pragma once

// Reads the core's NV12 program frame out of the shared-memory slot
// (docs/virtual-camera-spec.md V2b). This runs INSIDE frameserver.exe (the DLL
// is loaded there), opening the region the core created read-only and doing a
// seqlock read so it never returns a torn frame. If the core is not publishing
// (region absent, or no complete frame yet) it reports "no frame" and the media
// source serves its standby slate instead (law 2: never a black frame).

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "modules/VirtualCameraShm.h"
#include "VcamLog.h"

namespace corevideo::virtualcam {

using corevideo::modules::VirtualCameraShmHeader;
using corevideo::modules::kVirtualCameraMagic;
using corevideo::modules::openVirtualCameraShmFile;
using corevideo::modules::mapVirtualCameraShmView;
using corevideo::modules::virtualCameraShmFilePath;
using corevideo::modules::virtualCameraShmSize;

class SharedFrameReader {
 public:
  ~SharedFrameReader() { close(); }

  // Opens the region read-only. Safe to call repeatedly; returns true once mapped.
  bool ensureOpen() {
    if (view_ != nullptr) {
      return true;
    }
    // File-backed slot on %ProgramData% (cross-session; see VirtualCameraShm.h).
    // The core publishes from the user's session; we run in the session-0 Frame
    // Server, so a Local\ named mapping would be invisible - we open the shared
    // backing file instead.
    const std::string path = virtualCameraShmFilePath();
    file_ = openVirtualCameraShmFile(/*writer=*/false);
    if (file_ == INVALID_HANDLE_VALUE) {
      char b[192];
      _snprintf_s(b, sizeof(b), _TRUNCATE, "SHM CreateFile('%s') FAILED err=%lu",
                  path.c_str(), GetLastError());
      VcamServeLog(b);
      return false;  // core not publishing yet (file absent)
    }
    view_ = mapVirtualCameraShmView(file_, /*writer=*/false, &mapping_);
    if (view_ == nullptr) {
      char b[192];
      _snprintf_s(b, sizeof(b), _TRUNCATE, "SHM map view FAILED err=%lu", GetLastError());
      VcamServeLog(b);
      CloseHandle(file_);
      file_ = INVALID_HANDLE_VALUE;
      return false;
    }
    VcamServeLog("SHM opened OK (file-backed)");
    header_ = static_cast<const VirtualCameraShmHeader*>(view_);
    return true;
  }

  void close() {
    if (view_ != nullptr) {
      UnmapViewOfFile(view_);
      view_ = nullptr;
    }
    if (mapping_ != nullptr) {
      CloseHandle(mapping_);
      mapping_ = nullptr;
    }
    if (file_ != INVALID_HANDLE_VALUE) {
      CloseHandle(file_);
      file_ = INVALID_HANDLE_VALUE;
    }
    header_ = nullptr;
  }

  // Seqlock read of the latest complete frame into `out`. Returns true and sets
  // width/height on success; false if no complete frame is available (torn every
  // retry, or the core stopped publishing).
  bool readLatest(std::vector<std::uint8_t>& out, int& width, int& height) {
    if (!ensureOpen() || header_ == nullptr) {
      return false;
    }
    if (header_->magic != kVirtualCameraMagic) {
      return false;  // region not initialized by the core
    }
    const auto* payload =
        static_cast<const std::uint8_t*>(view_) + sizeof(VirtualCameraShmHeader);
    for (int attempt = 0; attempt < 8; ++attempt) {
      const std::uint32_t seq1 = header_->seq;
      if ((seq1 & 1u) != 0u) {
        continue;  // writer mid-update
      }
      const std::int32_t w = header_->width;
      const std::int32_t h = header_->height;
      const std::uint32_t bytes = header_->byteLen;
      if (w <= 0 || h <= 0 || bytes == 0 ||
          bytes > corevideo::modules::kVirtualCameraMaxPayload) {
        return false;
      }
      out.resize(bytes);
      std::memcpy(out.data(), payload, bytes);
      const std::uint32_t seq2 = header_->seq;
      if (seq1 == seq2) {  // stable across the copy -> not torn
        width = w;
        height = h;
        return true;
      }
    }
    return false;
  }

 private:
  HANDLE file_ = INVALID_HANDLE_VALUE;
  HANDLE mapping_ = nullptr;
  const void* view_ = nullptr;
  const VirtualCameraShmHeader* header_ = nullptr;
};

}  // namespace corevideo::virtualcam
