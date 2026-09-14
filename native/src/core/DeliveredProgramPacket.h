#pragma once
#include "core/RouteSourcePolicy.h"
#include <memory>
#include <optional>
#include <vector>
namespace corevideo::core {
// Value contract for Program handoff. Delivery here never proves presentation,
// encoding, or completion at any physical destination.
struct DeliveredProgramPacket {
  std::string showEpoch, sceneId, clockEpoch;
  uint64_t showRevision{0}, sceneRevision{0}, renderRevision{0}, clockGeneration{0};
  uint64_t packetSequence{0};
  int64_t productionSlot{-1}, producedAtNs{-1}, productionDeadlineNs{-1}, deliveryDeadlineNs{-1};
  std::vector<ExactRouteSourceRef> sources;
  int width{0}, height{0}, stride{0};
  std::shared_ptr<const std::vector<uint8_t>> pixels;
  // Owner supplied immutable GPU resource lease. gpuBytes is the provider's
  // committed allocation charge; readiness is acknowledged separately.
  std::shared_ptr<const void> gpuLease;
  size_t gpuBytes{0};
  struct Delivery { uint64_t sequence{0}; int64_t observedAtNs{-1}; };
  std::optional<Delivery> delivery;
};
// The nanosecond range is explicitly capped at the interoperable safe integer
// limit. A new clock epoch is required before this range is exhausted.
inline constexpr int64_t kProgramPacketMaxInteger = 9007199254740991LL;
struct ProgramPacketValidation { bool valid{false}; size_t retainedBytes{0}; };
ProgramPacketValidation validateDeliveredProgramPacket(const DeliveredProgramPacket&);
}
