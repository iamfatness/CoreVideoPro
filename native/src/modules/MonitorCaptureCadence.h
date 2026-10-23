#pragma once
#include "modules/CapturePresentationWindow.h"
#include "core/ProgramPlayoutTimeline.h"
#ifdef _WIN32
#include "modules/D3DDeadlineWait.h"
#endif
#include <map>
#include <thread>

namespace corevideo::modules {
// Monitor-owner source selection. Each consumer owns its clock while capture
// owns the existing three-arrival history. No production GPU demand or lease
// crosses this boundary; optional rendering still owns its separate uploads.
class MonitorCaptureCadence {
 public:
  void prepare(MonitorRenderRequest& request) {
    if (request.captureSlot < 0 || request.captureAnchorNs <= 0) return;
    for (auto it = clocks_.begin(); it != clocks_.end();) {
      if (std::none_of(request.frames.begin(), request.frames.end(), [&](const auto& frame) {
            return frame.participantId == it->first && frame.capturePresentation;
          })) it = clocks_.erase(it);
      else ++it;
    }
    const core::ProgramPlayoutTimeline timeline(2, request.captureAnchorNs);
    const auto cutoff = timeline.deadlineNs(request.captureSlot - 1) - 8'333'334;
    for (auto& frame : request.frames) {
      auto owner = frame.capturePresentation;
      if (!owner) continue;
      if (!clocks_.count(frame.participantId) && clocks_.size() >= MonitorRenderDiagnostics::InputCapacity) {
        request.unavailableInputs.push_back(frame.participantId);
        frame.capturePresentation.reset(); continue;
      }
      auto& state = clocks_[frame.participantId];
      if (state.owner.lock() != owner) state = {owner, {}};
      const auto choice = owner->chooseForConsumer(frame, request.captureSlot, request.captureAnchorNs, state.clock);
      auto resolved = choice ? owner->resolveForConsumer(*choice) : std::optional<VideoFrame>{};
      while (choice && !resolved && !request.cancelled()) {
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now >= cutoff) break;
#ifdef _WIN32
        wait_.pause(cutoff - now);
#else
        std::this_thread::yield();
#endif
        resolved = owner->resolveForConsumer(*choice);
      }
      if (resolved && resolved->participantId == frame.participantId && resolved->sourceEpoch == frame.sourceEpoch) {
        const auto generation = frame.sourceGeneration;
        frame = std::move(*resolved); frame.sourceGeneration = generation;
        frame.preparedGpu.reset(); frame.gpuPixels.reset(); frame.monitorGpuPixels.reset();
      } else {
        request.unavailableInputs.push_back(frame.participantId);
        state.clock.initialized = false;
      }
      frame.capturePresentation.reset(); frame.readyCapture.reset();
    }
  }
 private:
  struct Entry { std::weak_ptr<CapturePresentationWindow> owner; CapturePresentationWindow::Clock clock; };
  std::map<std::string, Entry> clocks_;
#ifdef _WIN32
  D3DDeadlineWait wait_;
#endif
};
}
