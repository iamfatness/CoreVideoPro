#pragma once
#include "modules/Interfaces.h"
#include "modules/CpuSourceGpuView.h"
#include "modules/CapturePresentationWindow.h"
#include "modules/ReadyCapturePublication.h"
#include "core/ProgramPlayoutTimeline.h"
#include <optional>
#include <algorithm>

namespace corevideo::modules {
// One reservation in the existing Program pool owns these immutable choices.
// CPU and optional-monitor payloads are not retained across composition ticks.
// Preparation may complete later; neither its identity nor observation changes.
struct PreparedProgramComposition {
  static constexpr size_t kMaxSources = 64;
  CompositorRenderPlan plan;
  std::vector<VideoFrame> frames;
  int64_t sequence = 0, productionSlot = -1, anchorNs = 0;
  struct CaptureChoice {
    size_t index;
    std::shared_ptr<CapturePresentationWindow> owner;
    std::optional<CapturePresentationWindow::Choice> choice;
    std::shared_ptr<ReadyCapturePublication> ready;
  };
  std::vector<CaptureChoice> captureChoices;

  bool deferred(size_t index) const {
    return std::any_of(captureChoices.begin(), captureChoices.end(),
        [index](const auto& choice) { return choice.index == index; });
  }
  // Reserve half an output period for composition/submission/GPU completion.
  // Readiness may use only time remaining before this original-slot boundary;
  // it never changes the output deadline or creates another reservation.
  int64_t readinessDeadlineNs(int bufferFrames) const {
    const core::ProgramPlayoutTimeline timeline(bufferFrames, anchorNs);
    return timeline.deadlineNs(productionSlot) - 8'333'334;
  }
  bool preparedSourcesReady() const {
    for (const auto& frame : frames)
      if (frame.preparedGpu && !frame.gpuPixels && !frame.preparedGpu->acquire(true)) return false;
    return true;
  }
  bool resolveCaptures(bool final = true) {
    bool complete = true;
    for (const auto& pending : captureChoices) {
      auto& frame = frames[pending.index];
      auto resolved = pending.ready ? pending.ready->acquire(frame) :
          pending.choice ? pending.owner->resolve(*pending.choice, final) : std::optional<VideoFrame>{};
      if (!resolved || resolved->participantId != frame.participantId || resolved->sourceEpoch != frame.sourceEpoch) {
        complete = false;
        if (!final) continue;
        // Never stamp a held image with the scheduled future identity.
        // Keep the weak producer token for lifecycle/error attribution. Its
        // identity cannot match the missing observation, so admission cannot
        // acquire its older pixels as the scheduled frame.
        frame.gpuPixels.reset(); frame.captureTimestamp100ns = 0;
        if (pending.choice) frame.frameId = pending.choice->frameId;
      } else {
        const auto sourceGeneration = frame.sourceGeneration;
        frame = std::move(*resolved); frame.sourceGeneration = sourceGeneration;
        frame.pixels.reset(); frame.i420.reset(); frame.monitorGpuPixels.reset();
      }
      frame.capturePresentation.reset();
      frame.readyCapture.reset();
    }
    return complete;
  }

  static std::optional<PreparedProgramComposition> capture(CompositorRenderPlan plan,
      std::vector<VideoFrame> selected, int64_t sequence, int64_t slot, int64_t anchor) {
    if (selected.size() > kMaxSources || sequence <= 0 || slot < 0 || anchor <= 0) return {};
    std::vector<CaptureChoice> choices;
    for (size_t index = 0; index < selected.size(); ++index) {
      auto& frame = selected[index];
      const int width = frame.hasI420() ? frame.i420Width : frame.hasPixels() ? frame.pixelWidth : frame.width;
      const int height = frame.hasI420() ? frame.i420Height : frame.hasPixels() ? frame.pixelHeight : frame.height;
      const auto& token = frame.preparedGpu;
      if (frame.capturePresentation) {
        choices.push_back({index, frame.capturePresentation, frame.capturePresentation->choose(frame, slot, anchor)});
      } else if (frame.readyCapture && frame.gpuPixels) {
        choices.push_back({index, {}, {}, frame.readyCapture});
        frame.gpuPixels.reset(); // do not pin an obsolete capture throughout CPU preparation
      } else if (token && token->sourceId == frame.participantId && token->sourceEpoch == frame.sourceEpoch &&
          token->frameId == frame.frameId && token->captureTimestamp100ns == frame.captureTimestamp100ns &&
          token->width == width && token->height == height)
        (void)token->acquire(true); // demand the chosen identity, never a future arrival
      frame.width = width; frame.height = height;
      frame.pixels.reset(); frame.i420.reset(); frame.monitorGpuPixels.reset();
      frame.capturePresentation.reset();
      frame.readyCapture.reset();
    }
    return PreparedProgramComposition{std::move(plan), std::move(selected), sequence, slot, anchor, std::move(choices)};
  }
};
}
