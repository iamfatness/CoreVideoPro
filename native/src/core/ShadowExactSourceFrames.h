#pragma once
#include "core/RouteSourcePolicy.h"
#include "modules/Interfaces.h"
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <stdexcept>

namespace corevideo::core {
// Shadow-only storage. It has no compositor, output, subscription or RPC access.
class ShadowExactSourceFrames final {
 public:
  struct CurrentSource {
    ExactRouteSourceRef identity;
    uint64_t publicationFence{0};
    bool available{false};
    bool operator==(const CurrentSource&) const = default;
  };
  struct Limits { size_t sources{8192}, frames{32}, bytes{64*1024*1024}, epochs{128}; };
  enum class Status { Applied, Unchanged, Invalid, Stale, Capacity, Missing, Expired };
  struct Result { Status status; std::shared_ptr<const modules::VideoFrame> frame; };
  ShadowExactSourceFrames() = default;
  explicit ShadowExactSourceFrames(Limits limits) : limits_(limits) {
    if (!limits.sources || limits.sources > 8192 || !limits.frames || limits.frames > 8192 ||
        !limits.bytes || limits.bytes > 256*1024*1024 || !limits.epochs || limits.epochs > 4096)
      throw std::invalid_argument("Invalid exact frame store bounds");
  }
  // Complete owner observation; current eligibility never comes from an old lease.
  Status reconcile(std::string epoch, uint64_t sequence, std::vector<CurrentSource> sources) {
    if (epoch.empty() || epoch.size() > 512 || !sequence || sequence > maximum) return invalidate(Status::Invalid);
    if (sources.size() > limits_.sources) return invalidate(Status::Capacity);
    std::map<std::string, CurrentSource> next;
    for (auto& source : sources) {
      if (!validExactRouteSourceRef(source.identity) || source.identity.processEpoch != epoch ||
          source.publicationFence > maximum || !next.emplace(source.identity.sourceId, source).second) return invalidate(Status::Invalid);
    }
    std::map<Key, Entry> retired;
    std::lock_guard lock(mutex_);
    if (retiredEpochs_.contains(epoch) || (epoch == epoch_ && sequence < sequence_)) return Status::Stale;
    if (epoch == epoch_ && sequence == sequence_) {
      if (valid_ && next == current_) return Status::Unchanged;
      valid_ = false; return Status::Invalid;
    }
    auto marks = epoch == epoch_ ? marks_ : std::map<std::string, CurrentSource>{};
    for (const auto& [id, source] : next) {
      const auto prior = marks.find(id);
      if (prior != marks.end()) {
        const auto& old = prior->second;
        if (source.identity.generation < old.identity.generation ||
            (source.identity.generation == old.identity.generation &&
             (source.identity != old.identity || !current_.contains(id) || (source.publicationFence < old.publicationFence ||
              (source.available != old.available && source.publicationFence == old.publicationFence))))) {
          valid_ = false; return Status::Stale;
        }
      } else if (marks.size() == limits_.sources) { valid_ = false; return Status::Capacity; }
      marks[id] = source;
    }
    if (!epoch_.empty() && epoch != epoch_) {
      if (retiredEpochs_.size() >= limits_.epochs) { valid_ = false; return Status::Capacity; }
      retiredEpochs_.insert(epoch_);
    }
    for (auto it = frames_.begin(); it != frames_.end();) {
      const auto current = next.find(it->first.identity.sourceId);
      if (current == next.end() || !current->second.available ||
          current->second.identity != it->first.identity || current->second.publicationFence != it->first.fence) {
        bytes_ -= it->second.bytes;
        retired.insert(frames_.extract(it++));
      } else ++it;
    }
    epoch_ = std::move(epoch); sequence_ = sequence; current_.swap(next); marks_.swap(marks); valid_ = true;
    return Status::Applied;
  }
  Status publish(const modules::VideoFrame& frame) {
    const auto evidence = frame.exactSourceEvidence;
    if (!evidence || frame.participantId.size() > 512 || !evidence->publicationSequence ||
        evidence->publicationSequence > maximum || evidence->publicationFence > maximum || evidence->observedNs < 0 ||
        (evidence->kind != modules::SourceFrameEvidence::Kind::Camera && evidence->kind != modules::SourceFrameEvidence::Kind::Share)) return Status::Invalid;
    const auto& id = evidence->identity;
    Key key{{id.sourceId, id.instanceId, id.processEpoch,
             evidence->kind == modules::SourceFrameEvidence::Kind::Camera ? "camera" : "share",
             static_cast<uint64_t>(id.generation)}, evidence->publicationFence};
    if (!validExactRouteSourceRef(key.identity) || (!frame.hasI420() && !frame.hasPixels()) ||
        evidence->payload != (frame.hasI420() ? frame.i420 : frame.pixels)) return Status::Invalid;
    size_t bytes = sizeof(modules::VideoFrame) + sizeof(modules::SourceFrameEvidence) + 4096;
    const auto add = [&](const auto& buffer) {
      if (!buffer) return true;
      if (buffer->capacity() > limits_.bytes || bytes > limits_.bytes - buffer->capacity()) return false;
      bytes += buffer->capacity(); return true;
    };
    if (!add(frame.i420) || (frame.pixels != frame.i420 && !add(frame.pixels)) || bytes > limits_.bytes) return Status::Capacity;
    auto frozen = std::make_shared<const modules::VideoFrame>(frame);
    std::shared_ptr<const modules::VideoFrame> retired;
    std::lock_guard lock(mutex_);
    const auto current = current_.find(key.identity.sourceId);
    if (!valid_ || current == current_.end() || !current->second.available || current->second.identity != key.identity ||
        current->second.publicationFence != key.fence) return Status::Stale;
    auto prior = frames_.find(key);
    if (prior != frames_.end()) {
      const auto& old = *prior->second.frame->exactSourceEvidence;
      if (evidence->publicationSequence < old.publicationSequence || evidence->observedNs < old.observedNs) return Status::Stale;
      if (evidence->publicationSequence == old.publicationSequence)
        return evidence == prior->second.frame->exactSourceEvidence ? Status::Unchanged : Status::Stale;
    }
    const size_t oldBytes = prior == frames_.end() ? 0 : prior->second.bytes;
    if ((prior == frames_.end() && frames_.size() == limits_.frames) || bytes > limits_.bytes - (bytes_ - oldBytes)) return Status::Capacity;
    if (prior == frames_.end()) frames_.emplace(std::move(key), Entry{std::move(frozen), bytes});
    else { retired = std::move(prior->second.frame); prior->second = {std::move(frozen), bytes}; }
    bytes_ = bytes_ - oldBytes + bytes;
    return Status::Applied;
  }
  Result resolve(const ExactRouteSourceRef& ref, int64_t freshAfterNs) const {
    if (!validExactRouteSourceRef(ref) || freshAfterNs < 0) return {Status::Invalid, {}};
    std::lock_guard lock(mutex_);
    const auto current = current_.find(ref.sourceId);
    if (!valid_ || ref.processEpoch != epoch_ || current == current_.end() || !current->second.available || current->second.identity != ref)
      return {Status::Missing, {}};
    const auto found = frames_.find({ref, current->second.publicationFence});
    if (found == frames_.end()) return {Status::Missing, {}};
    if (found->second.frame->exactSourceEvidence->observedNs < freshAfterNs) return {Status::Expired, {}};
    return {Status::Unchanged, found->second.frame};
  }
 private:
  Status invalidate(Status status) { std::lock_guard lock(mutex_); valid_ = false; return status; }
  struct Key {
    ExactRouteSourceRef identity; uint64_t fence;
    bool operator<(const Key& r) const {
      return std::tie(identity.sourceId,identity.instanceId,identity.processEpoch,identity.kind,identity.generation,fence) <
             std::tie(r.identity.sourceId,r.identity.instanceId,r.identity.processEpoch,r.identity.kind,r.identity.generation,r.fence);
    }
  };
  struct Entry { std::shared_ptr<const modules::VideoFrame> frame; size_t bytes; };
  static constexpr uint64_t maximum = 9007199254740991ULL;
  Limits limits_;
  mutable std::mutex mutex_;
  std::string epoch_; uint64_t sequence_{0}; size_t bytes_{0};
  bool valid_{false};
  std::map<std::string, CurrentSource> current_;
  std::map<std::string, CurrentSource> marks_; // Bounded departure/generation watermarks.
  std::map<Key, Entry> frames_;
  std::set<std::string> retiredEpochs_;
};
} // namespace corevideo::core
