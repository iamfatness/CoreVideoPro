#pragma once
#include "modules/MonitorFrameAdmission.h"
#include <map>

namespace corevideo::modules {
// Owned exclusively by the monitor worker. One immutable last-good image per
// demanded source; removal, replacement and destruction happen on that owner.
// Logical payload charge is not total GPU residency or process working set.
class MonitorInputCache {
 public:
  static constexpr size_t Capacity = MonitorRenderDiagnostics::InputCapacity;
  static constexpr uint64_t ByteBudget = MonitorRenderDiagnostics::InputByteBudget;
  void prepare(MonitorRenderRequest& request, MonitorRenderResult& observation) {
    for (auto it = retained_.begin(); it != retained_.end();) {
      if (!monitorRequestNeedsFrame(request, it->second)) {
        bytes_ -= charge(it->second);
        it = retained_.erase(it);
      } else ++it;
    }
    // Missing arrivals for a still demanded source hold its actual old epoch,
    // frame identity and capture observation, never the new job's stamp.
    for (const auto& [id, frame] : retained_) {
      if (std::none_of(request.frames.begin(), request.frames.end(),
          [&](const auto& current) { return current.participantId == id; })) {
        request.frames.push_back(frame);
        request.frames.back().sourceEpoch = 0; // desired epoch unknown without an arrival
        request.unavailableInputs.push_back(id);
      }
    }
    for (auto& frame : request.frames) {
      auto prior = retained_.find(frame.participantId);
      const bool missing = std::find(request.unavailableInputs.begin(), request.unavailableInputs.end(),
          frame.participantId) != request.unavailableInputs.end();
      const auto requestedEpoch = frame.sourceEpoch;
      if (missing) {
        if (prior != retained_.end()) { frame = prior->second; ++observation.heldInputs; }
        else ++observation.unavailableInputs;
        observation.inputs.push_back({frame.participantId, prior != retained_.end() ? "held" : "unavailable",
            "input-unavailable", prior != retained_.end() ? frame.sourceEpoch : 0, requestedEpoch,
            prior != retained_.end() ? frame.frameId : 0, prior != retained_.end() ? frame.captureTimestamp100ns : 0});
        continue;
      }
      if (!frame.hasContent()) continue; // intentional legacy slate, not an arrival
      ++observation.readyInputs;
      observation.inputs.push_back({frame.participantId, "ready", "", frame.sourceEpoch, requestedEpoch,
          frame.frameId, frame.captureTimestamp100ns});
      const auto oldBytes = prior == retained_.end() ? 0 : charge(prior->second);
      const auto newBytes = charge(frame);
      if (frame.participantId.empty() || newBytes > ByteBudget ||
          bytes_ - oldBytes > ByteBudget - newBytes ||
          (prior == retained_.end() && retained_.size() >= Capacity)) {
        ++refusals_; // still render the ready input; only refuse retention
        continue;
      }
      retained_[frame.participantId] = frame;
      bytes_ = bytes_ - oldBytes + newBytes;
    }
    observation.retainedInputs = retained_.size();
    observation.retainedInputBytes = bytes_;
    observation.retentionRefusals = refusals_;
  }
 private:
  static uint64_t charge(const VideoFrame& frame) {
    uint64_t bytes = 0;
    const auto add = [&](uint64_t size) {
      bytes = size > ByteBudget || bytes > ByteBudget - size ? ByteBudget + 1 : bytes + size;
    };
    if (frame.pixels) add(frame.pixels->size());
    if (frame.i420) add(frame.i420->size());
    if (frame.hasGpuPixels()) {
      const auto pixels = uint64_t(frame.gpuPixels->width) * uint64_t(frame.gpuPixels->height);
      add(pixels > ByteBudget / 4 ? ByteBudget + 1 : pixels * 4);
    }
    return bytes;
  }
  std::map<std::string, VideoFrame> retained_;
  uint64_t bytes_ = 0, refusals_ = 0;
};
}
