#pragma once
#include "core/RouteSourcePolicy.h"
#include "modules/Interfaces.h"
#include <memory>
#include <vector>

namespace corevideo::core {
// Historical, immutable ownership; not proof of future availability or delivery.
class SourceFrameLease final {
 public:
  struct CurrentSource { ExactRouteSourceRef identity; uint64_t publicationFence{0}; bool available{false}; };
  struct Format {
    enum class Encoding { I420, Bgra };
    Encoding encoding; int width, height, stride; bool fullRange, bt601;
    bool operator==(const Format&) const = default;
  };
  enum class Status { Ready, Invalid, Stale, Capacity };
  using Ptr = std::shared_ptr<const SourceFrameLease>;
  struct Result { Status status; Ptr lease; };
  static Result create(const modules::VideoFrame&, const CurrentSource&, int64_t freshAfterNs,
      int64_t observedThroughNs, size_t maxRetainedBytes = 64 * 1024 * 1024);
  const ExactRouteSourceRef identity;
  const uint64_t publicationSequence, publicationFence;
  const int64_t observedNs;
  const Format format;
  const modules::VideoFrame frame;
  const size_t retainedBytes;
 private:
  SourceFrameLease(ExactRouteSourceRef, modules::VideoFrame, Format, size_t);
};
class SourceFrameLeaseSet final {
 public:
  struct Limits { size_t sources{32}, bytes{64 * 1024 * 1024}; };
  using Ptr = std::shared_ptr<const SourceFrameLeaseSet>;
  struct Result { SourceFrameLease::Status status; Ptr leases; };
  static Result create(std::string processEpoch, std::vector<SourceFrameLease::Ptr>, Limits);
  const std::string processEpoch;
  // Canonical sourceId order, camera/share have distinct provider source IDs.
  const std::vector<SourceFrameLease::Ptr> frames;
  const size_t retainedBytes;
 private:
  SourceFrameLeaseSet(std::string, std::vector<SourceFrameLease::Ptr>, size_t);
};
}
