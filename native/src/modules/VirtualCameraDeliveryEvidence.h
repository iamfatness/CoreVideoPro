#pragma once

#include <array>
#include <cstdint>

namespace corevideo::modules {

// Reader-local facts, not proof of Program or receiver/display continuity.
// The V1 pixel header has a publication counter, not a Program ID or an epoch.
enum class VirtualCameraReadResult : unsigned {
  Fresh, Unchanged, Contended, Unavailable, Uninitialized, InvalidHeader, Count
};

struct VirtualCameraReadEvidence {
  std::array<std::uint64_t, static_cast<unsigned>(VirtualCameraReadResult::Count)> counts{};
  VirtualCameraReadResult lastResult = VirtualCameraReadResult::Unavailable;
  std::uint64_t lastPublication = 0;
  std::uint32_t lastSequence = 0;
  bool identityObserved = false;

  void record(VirtualCameraReadResult result) {
    lastResult = result;
    ++counts[static_cast<unsigned>(result)];
  }
  void recordFresh(std::uint64_t publication, std::uint32_t sequence) {
    record(VirtualCameraReadResult::Fresh);
    lastPublication = publication;
    lastSequence = sequence;
    identityObserved = true;
  }
  std::uint64_t count(VirtualCameraReadResult result) const {
    return counts[static_cast<unsigned>(result)];
  }
};

enum class VirtualCameraSampleContent { Fresh, Held, Slate };

// No allocation, clock, file I/O or atomics. MediaStream owns this under its
// existing lock. A sample becomes emitted only after MF accepts its event.
struct VirtualCameraDeliveryEvidence {
  std::uint64_t emitted = 0, fresh = 0, held = 0, slate = 0, failed = 0;
  std::uint64_t formatMismatches = 0, maximumIntervalHns = 0;
  std::int64_t lastEmissionHns = 0;
  bool emissionObserved = false;

  void recordEmission(bool accepted, VirtualCameraSampleContent content, std::int64_t nowHns) {
    if (!accepted) { ++failed; return; }
    ++emitted;
    switch (content) {
      case VirtualCameraSampleContent::Fresh: ++fresh; break;
      case VirtualCameraSampleContent::Held: ++held; break;
      case VirtualCameraSampleContent::Slate: ++slate; break;
    }
    if (emissionObserved && nowHns >= lastEmissionHns) {
      const auto interval = static_cast<std::uint64_t>(nowHns - lastEmissionHns);
      if (interval > maximumIntervalHns) maximumIntervalHns = interval;
    }
    lastEmissionHns = nowHns;
    emissionObserved = true;
  }
  // A stopped interval is not a missed live delivery deadline. Totals survive
  // Start/Stop; the log includes a run ID so comparisons can avoid that boundary.
  void restartCadence() { emissionObserved = false; }
};

}  // namespace corevideo::modules
