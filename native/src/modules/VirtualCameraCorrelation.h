#pragma once
#include <cstdint>

namespace corevideo::modules {
// Separate from the shipping 32-byte pixel header. No COM pointers, names,
// credentials or pixels. All timestamps are host-monotonic 100ns ticks.
struct VirtualCameraCorrelationRecord {
  uint32_t magic = 0, version = 1;
  volatile uint32_t sequence = 0;
  uint32_t pixelSequence = 0;
  uint64_t epochHigh = 0, epochLow = 0, publication = 0;
  int64_t programSequence = 0, deliveredAt100ns = 0, publishedAt100ns = 0;
  uint64_t pixelFileIdentity = 0;
  uint32_t pixelVolumeIdentity = 0, recordBytes = 80;
};
static_assert(sizeof(VirtualCameraCorrelationRecord) == 80);
inline constexpr uint32_t kVirtualCameraCorrelationMagic = 0x43565431;

inline bool correlatesCameraPixels(const VirtualCameraCorrelationRecord& before,
    const VirtualCameraCorrelationRecord& after, uint32_t pixelSequence, uint64_t publication,
    uint64_t pixelFileIdentity = 0, uint32_t pixelVolumeIdentity = 0) {
  return before.magic == kVirtualCameraCorrelationMagic && after.magic == before.magic &&
      before.version == 1 && after.version == 1 && before.recordBytes == sizeof(before) &&
      after.recordBytes == sizeof(after) && (before.sequence & 1u) == 0 &&
      before.sequence == after.sequence && before.pixelSequence == pixelSequence &&
      after.pixelSequence == pixelSequence && before.publication == publication &&
      after.publication == publication && before.programSequence > 0 &&
      before.programSequence == after.programSequence && (before.epochHigh || before.epochLow) &&
      before.epochHigh == after.epochHigh && before.epochLow == after.epochLow &&
      before.pixelFileIdentity == pixelFileIdentity && after.pixelFileIdentity == pixelFileIdentity &&
      before.pixelVolumeIdentity == pixelVolumeIdentity && after.pixelVolumeIdentity == pixelVolumeIdentity &&
      before.deliveredAt100ns == after.deliveredAt100ns && before.publishedAt100ns == after.publishedAt100ns;
}
}
