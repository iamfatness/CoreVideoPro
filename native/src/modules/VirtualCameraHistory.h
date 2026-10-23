#pragma once
#include "modules/VirtualCameraShm.h"
#if defined(_WIN32)
#include <objbase.h>
#include <chrono>
#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>
#include "modules/VirtualCameraDeliveryEvidence.h"

namespace corevideo::modules {
// Optional successor transport. The shipping latest-slot layout stays intact
// for older DLLs. A bounded retained publication is never a catch-up queue:
// each camera request consumes at most one picture at its original deadline.
inline constexpr uint32_t kCameraHistoryMagic = 0x43564831;
inline constexpr uint32_t kCameraHistorySlots = 4;
struct CameraHistoryHeader {
  uint32_t magic = 0, version = 1;
  volatile uint32_t sequence = 0;
  uint32_t slots = kCameraHistorySlots;
  uint64_t epochHigh = 0, epochLow = 0, pixelFileIdentity = 0;
  uint32_t pixelVolumeIdentity = 0, payloadCapacity = kVirtualCameraMaxPayload;
  uint64_t latest = 0, legacyPublication = 0;
  uint32_t legacySequence = 0, headerBytes = sizeof(CameraHistoryHeader);
};
struct CameraHistorySlot {
  volatile uint32_t sequence = 0;
  int32_t width = 0, height = 0;
  uint32_t byteLen = 0;
  uint64_t publication = 0, legacyPublication = 0;
  int64_t programSequence = 0, deliveredAt100ns = 0, publishedAt100ns = 0;
  uint32_t legacySequence = 0, reserved = 0;
};
static_assert(sizeof(CameraHistoryHeader) == 72 && sizeof(CameraHistorySlot) == 64);
inline std::string cameraHistoryPath() { return virtualCameraShmDir() + "\\vcam-history-v1.shm"; }
inline constexpr size_t cameraHistorySize() {
  return sizeof(CameraHistoryHeader) + kCameraHistorySlots * (sizeof(CameraHistorySlot) + kVirtualCameraMaxPayload);
}
inline CameraHistorySlot* cameraHistorySlot(void* view, uint64_t publication) {
  return reinterpret_cast<CameraHistorySlot*>(static_cast<uint8_t*>(view) + sizeof(CameraHistoryHeader) +
      ((publication - 1) % kCameraHistorySlots) * (sizeof(CameraHistorySlot) + kVirtualCameraMaxPayload));
}
class CameraHistoryMapping {
 public:
  CameraHistoryMapping() = default;
  CameraHistoryMapping(const CameraHistoryMapping&) = delete;
  ~CameraHistoryMapping() { close(); }
  bool open(bool writer) {
    if (view) return true;
    file = openVirtualCameraShmFile(writer, cameraHistoryPath());
    if (file != INVALID_HANDLE_VALUE) view = mapVirtualCameraShmView(file, writer, &mapping, cameraHistorySize());
    if (!view) { close(); return false; }
    return true;
  }
  void close() {
    if (view) UnmapViewOfFile(view);
    if (mapping) CloseHandle(mapping);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    view = nullptr; mapping = nullptr; file = INVALID_HANDLE_VALUE;
  }
  void* view = nullptr;
 private:
  HANDLE file = INVALID_HANDLE_VALUE, mapping = nullptr;
};
class VirtualCameraHistoryWriter {
 public:
  ~VirtualCameraHistoryWriter() { stop(); }
  bool start(HANDLE pixelFile) {
    stop();
    BY_HANDLE_FILE_INFORMATION identity{};
    GUID epoch{};
    if (!GetFileInformationByHandle(pixelFile, &identity) || FAILED(CoCreateGuid(&epoch)) || !mapping_.open(true)) return false;
    auto* header = static_cast<CameraHistoryHeader*>(mapping_.view);
    header->sequence |= 1u; MemoryBarrier();
    uint64_t words[2]{}; std::memcpy(words, &epoch, sizeof(epoch));
    header->magic = kCameraHistoryMagic; header->version = 1;
    header->slots = kCameraHistorySlots; header->payloadCapacity = kVirtualCameraMaxPayload;
    header->headerBytes = sizeof(CameraHistoryHeader);
    header->epochHigh = words[0]; header->epochLow = words[1];
    header->pixelFileIdentity = (uint64_t(identity.nFileIndexHigh) << 32) | identity.nFileIndexLow;
    header->pixelVolumeIdentity = identity.dwVolumeSerialNumber;
    header->latest = header->legacyPublication = 0; header->legacySequence = 0;
    MemoryBarrier(); header->sequence = (header->sequence | 1u) + 1u;
    return true;
  }
  void stop() {
    if (mapping_.view) {
      auto* header = static_cast<CameraHistoryHeader*>(mapping_.view);
      header->sequence |= 1u; MemoryBarrier(); header->magic = 0;
      MemoryBarrier(); header->sequence = (header->sequence | 1u) + 1u;
    }
    mapping_.close();
  }
  void publish(const uint8_t* pixels, int width, int height, uint64_t legacyPublication,
      uint32_t legacySequence, int64_t programSequence, int64_t deliveredAt100ns) {
    if (!mapping_.view) return;
    const auto bytes = static_cast<size_t>(width) * height * 3 / 2;
    if (!pixels || width <= 0 || height <= 0 || bytes > kVirtualCameraMaxPayload) return;
    auto* header = static_cast<CameraHistoryHeader*>(mapping_.view);
    const auto next = header->latest + 1;
    auto* slot = cameraHistorySlot(mapping_.view, next);
    slot->sequence |= 1u; MemoryBarrier();
    slot->width = width; slot->height = height; slot->byteLen = static_cast<uint32_t>(bytes);
    slot->publication = next; slot->legacyPublication = legacyPublication;
    slot->legacySequence = legacySequence; slot->programSequence = programSequence;
    slot->deliveredAt100ns = deliveredAt100ns;
    slot->publishedAt100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 100;
    std::memcpy(slot + 1, pixels, bytes);
    MemoryBarrier(); slot->sequence = (slot->sequence | 1u) + 1u;
    // Older retained slots remain readable while this new slot is written.
    header->sequence |= 1u; MemoryBarrier();
    header->latest = next; header->legacyPublication = legacyPublication;
    header->legacySequence = legacySequence;
    MemoryBarrier(); header->sequence = (header->sequence | 1u) + 1u;
  }
 private:
  CameraHistoryMapping mapping_;
};
class VirtualCameraHistoryReader {
 public:
  void close() { mapping_.close(); resetCursor(); retryOpenAt_ = 0; }
  // nullopt means an older publisher: use the shipping latest-slot protocol.
  // A recognized active transport returns its actual miss, never hides an
  // overwrite/collision by falling through to a newer legacy picture.
  std::optional<VirtualCameraReadResult> read(const VirtualCameraShmHeader* legacy,
      uint64_t fileIdentity, uint32_t volumeIdentity, std::vector<uint8_t>& pixels,
      int& width, int& height, unsigned* copyBudget, VirtualCameraReadEvidence& evidence) {
    using Result = VirtualCameraReadResult;
    const auto miss = [&](Result result) { evidence.record(result); return result; };
    if (!mapping_.view) {
      const auto now = GetTickCount64();
      if (now < retryOpenAt_) return {};
      retryOpenAt_ = now + 1000;
      if (!mapping_.open(false)) return {};
    }
    CameraHistoryHeader before{};
    if (!snapshot(before)) {
      // A current history writer keeps the legacy slot odd throughout its
      // transaction. An even legacy slot with an abandoned odd sidecar must
      // still be usable by an older publisher after a crash.
      if (legacy->seq & 1u) return miss(Result::Contended);
      return {};
    }
    if (!valid(before, fileIdentity, volumeIdentity)) { resetCursor(); return {}; }
    auto legacySequence = legacy->seq; MemoryBarrier();
    if (legacySequence & 1u) return miss(Result::Contended);
    auto legacyPublication = legacy->frameNumber; MemoryBarrier();
    if (legacySequence != legacy->seq) return miss(Result::Contended);
    if (before.legacySequence != legacySequence || before.legacyPublication != legacyPublication) {
      // A new publication may have committed after the first header snapshot.
      // Refresh metadata before deciding this is an older publisher; otherwise
      // that race would itself fall through to latest-only and skip a picture.
      if (!snapshot(before)) return miss(Result::Contended);
      if (!valid(before, fileIdentity, volumeIdentity)) { resetCursor(); return {}; }
      legacySequence = legacy->seq; MemoryBarrier();
      legacyPublication = legacy->frameNumber; MemoryBarrier();
      if ((legacySequence & 1u) || legacySequence != legacy->seq) return miss(Result::Contended);
      if (before.legacySequence != legacySequence || before.legacyPublication != legacyPublication) {
        resetCursor(); return {}; // Old publisher resumed on the same backing file.
      }
    }
    if (!before.latest) return miss(Result::Uninitialized);
    const bool sameEpoch = epochHigh_ == before.epochHigh && epochLow_ == before.epochLow;
    auto wanted = sameEpoch && last_ ? last_ + 1 : before.latest;
    if (wanted > before.latest) return miss(Result::Unchanged);
    uint64_t skipped = 0;
    if (before.latest - wanted >= kCameraHistorySlots) {
      skipped = before.latest - wanted; wanted = before.latest;
    }
    auto* slot = cameraHistorySlot(mapping_.view, wanted);
    const auto sequence = slot->sequence;
    if (sequence & 1u) return miss(Result::Contended);
    MemoryBarrier(); const CameraHistorySlot record = *slot;
    if (record.publication != wanted) return miss(Result::Contended);
    if (record.width <= 0 || record.height <= 0 || record.width > kVirtualCameraMaxWidth ||
        record.height > kVirtualCameraMaxHeight || ((record.width | record.height) & 1) ||
        record.byteLen != static_cast<size_t>(record.width) * record.height * 3 / 2 ||
        record.byteLen > kVirtualCameraMaxPayload) return miss(Result::InvalidHeader);
    if (copyBudget && !*copyBudget) { ++evidence.copyBudgetExhaustions; return miss(Result::Contended); }
    if (copyBudget) --*copyBudget;
    pixels.resize(record.byteLen); std::memcpy(pixels.data(), slot + 1, record.byteLen);
    MemoryBarrier();
    CameraHistoryHeader after{};
    if (slot->sequence != sequence || !snapshot(after) || !valid(after, fileIdentity, volumeIdentity) ||
        before.epochHigh != after.epochHigh || before.epochLow != after.epochLow ||
        after.latest < wanted || after.latest - wanted >= kCameraHistorySlots) {
      ++evidence.changedDuringCopy; return miss(Result::Contended);
    }
    const auto finalLegacySequence = legacy->seq; MemoryBarrier();
    if ((finalLegacySequence & 1u) || after.legacySequence != finalLegacySequence ||
        after.legacyPublication != legacy->frameNumber || legacy->seq != finalLegacySequence)
      return miss(Result::Contended);
    width = record.width; height = record.height;
    epochHigh_ = after.epochHigh; epochLow_ = after.epochLow; last_ = wanted;
    evidence.recordFresh(record.legacyPublication, record.legacySequence);
    evidence.recordCorrelation(record.programSequence > 0, after.epochHigh, after.epochLow, record.programSequence);
    ++evidence.historyReads;
    if (wanted < after.latest) ++evidence.historyRetainedReads;
    if (skipped) { ++evidence.historyOverflows; evidence.historySkippedPublications += skipped; }
    evidence.historyMaximumLag = (std::max)(evidence.historyMaximumLag, after.latest - wanted);
    return Result::Fresh;
  }
 private:
  bool snapshot(CameraHistoryHeader& result) const {
    const auto* header = static_cast<const CameraHistoryHeader*>(mapping_.view);
    const auto first = header->sequence;
    if (first & 1u) return false;
    MemoryBarrier(); result = *header; MemoryBarrier();
    return first == header->sequence && result.sequence == first;
  }
  static bool valid(const CameraHistoryHeader& header, uint64_t fileIdentity, uint32_t volumeIdentity) {
    return header.magic == kCameraHistoryMagic && header.version == 1 &&
        header.headerBytes == sizeof(CameraHistoryHeader) && header.slots == kCameraHistorySlots &&
        header.payloadCapacity == kVirtualCameraMaxPayload && (header.epochHigh || header.epochLow) &&
        header.pixelFileIdentity == fileIdentity && header.pixelVolumeIdentity == volumeIdentity;
  }
  void resetCursor() { epochHigh_ = epochLow_ = last_ = 0; }
  CameraHistoryMapping mapping_;
  uint64_t epochHigh_ = 0, epochLow_ = 0, last_ = 0, retryOpenAt_ = 0;
};
}
#endif
