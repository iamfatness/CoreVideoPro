#pragma once
#include "modules/VirtualCameraCorrelation.h"
#include "modules/VirtualCameraShm.h"
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <objbase.h>
namespace corevideo::modules {
class VirtualCameraCorrelationMapping {
 public:
  VirtualCameraCorrelationMapping() = default;
  VirtualCameraCorrelationMapping(const VirtualCameraCorrelationMapping&) = delete;
  VirtualCameraCorrelationMapping& operator=(const VirtualCameraCorrelationMapping&) = delete;
  ~VirtualCameraCorrelationMapping() { close(); }
  bool open(bool writer) {
    if (view_) return true;
    file_ = openVirtualCameraShmFile(writer, virtualCameraShmDir() + "\\vcam-correlation-v1.shm");
    if (file_ == INVALID_HANDLE_VALUE) return false;
    view_ = static_cast<VirtualCameraCorrelationRecord*>(
        mapVirtualCameraShmView(file_, writer, &mapping_, sizeof(VirtualCameraCorrelationRecord)));
    if (!view_) { close(); return false; }
    return true;
  }
  void close() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    view_ = nullptr; mapping_ = nullptr; file_ = INVALID_HANDLE_VALUE;
  }
  bool read(VirtualCameraCorrelationRecord& result) const {
    if (!view_) return false;
    const auto first = view_->sequence;
    if (first & 1u) return false;
    MemoryBarrier();
    result = *view_;
    MemoryBarrier();
    return first == view_->sequence && result.sequence == first;
  }
  // Always invalidate a previous run, even with tracing disabled, so a stale
  // sidecar cannot accidentally match a reset pixel publication count.
  void start(bool enabled, HANDLE pixelFile = INVALID_HANDLE_VALUE) {
    if (!open(true)) return;
    enabled_ = enabled;
    view_->sequence = (view_->sequence | 1u);
    MemoryBarrier();
    GUID epoch{};
    if (enabled_ && FAILED(CoCreateGuid(&epoch))) enabled_ = false;
    BY_HANDLE_FILE_INFORMATION fileInfo{};
    if (enabled_ && !GetFileInformationByHandle(pixelFile, &fileInfo)) enabled_ = false;
    uint64_t words[2]{};
    std::memcpy(words, &epoch, sizeof(epoch));
    view_->magic = enabled_ ? kVirtualCameraCorrelationMagic : 0;
    view_->version = 1; view_->pixelSequence = 0; view_->publication = 0;
    view_->recordBytes = sizeof(VirtualCameraCorrelationRecord);
    view_->pixelFileIdentity = (uint64_t(fileInfo.nFileIndexHigh) << 32) | fileInfo.nFileIndexLow;
    view_->pixelVolumeIdentity = fileInfo.dwVolumeSerialNumber;
    view_->epochHigh = words[0]; view_->epochLow = words[1];
    view_->programSequence = 0; view_->deliveredAt100ns = 0; view_->publishedAt100ns = 0;
    MemoryBarrier();
    view_->sequence = (view_->sequence | 1u) + 1u;
  }
  void begin() {
    if (!enabled_ || !view_) return;
    view_->sequence = (view_->sequence | 1u);
    MemoryBarrier();
  }
  void finish(uint32_t pixelSequence, uint64_t publication, int64_t programSequence,
              int64_t deliveredAt100ns) {
    if (!enabled_ || !view_) return;
    view_->pixelSequence = pixelSequence;
    view_->publication = publication;
    view_->programSequence = programSequence;
    view_->deliveredAt100ns = deliveredAt100ns;
    view_->publishedAt100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    MemoryBarrier();
    view_->sequence = (view_->sequence | 1u) + 1u;
  }
 private:
  HANDLE file_ = INVALID_HANDLE_VALUE, mapping_ = nullptr;
  VirtualCameraCorrelationRecord* view_ = nullptr;
  bool enabled_ = false;
};
}
#endif
