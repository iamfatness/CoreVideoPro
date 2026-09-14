#include "core/SourceFrameLease.h"
#include <algorithm>
#include <set>

namespace corevideo::core {
namespace { constexpr uint64_t maxSafe = 9007199254740991ULL; constexpr size_t maxBytes = 256 * 1024 * 1024; }
SourceFrameLease::SourceFrameLease(ExactRouteSourceRef id, modules::VideoFrame value, Format description, size_t bytes)
    : identity(std::move(id)), publicationSequence(value.exactSourceEvidence->publicationSequence),
      publicationFence(value.exactSourceEvidence->publicationFence), observedNs(value.exactSourceEvidence->observedNs),
      format(description), frame(std::move(value)), retainedBytes(bytes) {}
SourceFrameLease::Result SourceFrameLease::create(const modules::VideoFrame& frame, const CurrentSource& current,
    int64_t freshAfterNs, int64_t observedThroughNs, size_t byteLimit) {
  if (!validExactRouteSourceRef(current.identity) || current.publicationFence > maxSafe ||
      freshAfterNs < 0 || observedThroughNs < freshAfterNs || !byteLimit || byteLimit > maxBytes) return {Status::Invalid,{}};
  const auto& proof = frame.exactSourceEvidence;
  if (!proof || proof->identity.generation <= 0 || !proof->publicationSequence || proof->publicationSequence > maxSafe ||
      proof->publicationFence > maxSafe || proof->observedNs < 0 ||
      (proof->kind != modules::SourceFrameEvidence::Kind::Camera && proof->kind != modules::SourceFrameEvidence::Kind::Share)) return {Status::Invalid,{}};
  ExactRouteSourceRef id{proof->identity.sourceId, proof->identity.instanceId, proof->identity.processEpoch,
      proof->kind == modules::SourceFrameEvidence::Kind::Camera ? "camera" : "share", static_cast<uint64_t>(proof->identity.generation)};
  if (!validExactRouteSourceRef(id)) return {Status::Invalid,{}};
  if (!current.available || id != current.identity || proof->publicationFence != current.publicationFence ||
      proof->observedNs < freshAfterNs || proof->observedNs > observedThroughNs) return {Status::Stale,{}};
  // Exactly one representation: one producer proof cannot attest two allocations.
  if (bool(frame.i420) == bool(frame.pixels)) return {Status::Invalid,{}};
  const auto& payload = frame.i420 ? frame.i420 : frame.pixels;
  if (!payload || proof->payload != payload || proof->payload.owner_before(payload) || payload.owner_before(proof->payload)) return {Status::Invalid,{}};
  if (payload->capacity() > byteLimit) return {Status::Capacity,{}};
  Format format{frame.i420 ? Format::Encoding::I420 : Format::Encoding::Bgra,
      frame.i420 ? frame.i420Width : frame.pixelWidth, frame.i420 ? frame.i420Height : frame.pixelHeight,
      frame.i420 ? frame.i420Width : frame.pixelStride, frame.i420FullRange, frame.i420Bt601};
  if (format.width <= 0 || format.height <= 0 || format.width > 32768 || format.height > 32768 ||
      frame.participantId.size() > 512 || frame.width < 0 || frame.height < 0 || frame.naturalWidth < 0 || frame.naturalHeight < 0)
    return {Status::Invalid,{}};
  const size_t width = static_cast<size_t>(format.width), height = static_cast<size_t>(format.height);
  size_t required = 0;
  if (frame.i420) {
    if ((width & 1) || (height & 1)) return {Status::Invalid,{}};
    required = width * height + (width * height / 4) * 2;
  } else {
    if (format.stride < 0 || static_cast<size_t>(format.stride) < width * 4 ||
        static_cast<size_t>(format.stride) > payload->size() / height) return {Status::Invalid,{}};
    required = static_cast<size_t>(format.stride) * height;
  }
  if (required > payload->size()) return {Status::Invalid,{}};
  modules::VideoFrame compact = frame;
  compact.participantId = std::string(frame.participantId.data(), frame.participantId.size());
  // Never retain the caller's proof object: it may own huge spare string capacity.
  modules::SourceFrameEvidence evidence;
  evidence.identity = {std::string(id.sourceId.data(), id.sourceId.size()),
      std::string(id.instanceId.data(), id.instanceId.size()), std::string(id.processEpoch.data(), id.processEpoch.size()),
      proof->identity.generation};
  evidence.kind = proof->kind; evidence.publicationSequence = proof->publicationSequence;
  evidence.publicationFence = proof->publicationFence; evidence.observedNs = proof->observedNs;
  evidence.payload = payload;
  compact.exactSourceEvidence = std::make_shared<const modules::SourceFrameEvidence>(std::move(evidence));
  // Charge object storage, conservative control blocks, strings and the shared
  // allocation's full capacity. Shared payload aliases are never charged less.
  const auto& held = *compact.exactSourceEvidence;
  size_t bytes = sizeof(SourceFrameLease) + sizeof(modules::SourceFrameEvidence) +
      sizeof(std::vector<uint8_t>) + 192 + compact.participantId.capacity() + 1 +
      id.sourceId.capacity() + id.instanceId.capacity() + id.processEpoch.capacity() + id.kind.capacity() + 4 +
      held.identity.sourceId.capacity() + held.identity.instanceId.capacity() + held.identity.processEpoch.capacity() + 3;
  if (bytes > byteLimit || payload->capacity() > byteLimit - bytes) return {Status::Capacity,{}};
  bytes += payload->capacity();
  return {Status::Ready, Ptr(new SourceFrameLease(std::move(id), std::move(compact), format, bytes))};
}
SourceFrameLeaseSet::SourceFrameLeaseSet(std::string epoch, std::vector<SourceFrameLease::Ptr> values, size_t bytes)
    : processEpoch(std::move(epoch)), frames(std::move(values)), retainedBytes(bytes) {}
SourceFrameLeaseSet::Result SourceFrameLeaseSet::create(std::string epoch, std::vector<SourceFrameLease::Ptr> values, Limits limits) {
  using Status = SourceFrameLease::Status;
  if (!validExactRouteSourceRef({"set", "set", epoch, "camera", 1}) || !limits.sources || limits.sources > 8192 || !limits.bytes || limits.bytes > maxBytes)
    return {Status::Invalid,{}};
  if (values.size() > limits.sources) return {Status::Capacity,{}};
  std::set<std::string> identities;
  epoch = std::string(epoch.data(), epoch.size());
  size_t bytes = sizeof(SourceFrameLeaseSet) + 64 + epoch.capacity() + 1 + values.size() * sizeof(SourceFrameLease::Ptr);
  if (bytes > limits.bytes) return {Status::Capacity,{}};
  for (const auto& value : values) {
    if (!value || value->identity.processEpoch != epoch || !identities.insert(value->identity.sourceId).second) return {Status::Invalid,{}};
    // Conservative count per member, even if buffers alias. Never undercount owners.
    if (value->retainedBytes > limits.bytes - bytes) return {Status::Capacity,{}};
    bytes += value->retainedBytes;
  }
  std::sort(values.begin(), values.end(), [](const auto& a, const auto& b) { return a->identity.sourceId < b->identity.sourceId; });
  // Compact metadata capacity supplied by callers; pixel allocations remain shared.
  std::vector<SourceFrameLease::Ptr> compact(values.begin(), values.end());
  return {Status::Ready, Ptr(new SourceFrameLeaseSet(std::move(epoch), std::move(compact), bytes))};
}
} // namespace corevideo::core
