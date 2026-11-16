#pragma once
// Shared by SourceGradeIdentityTest.cpp (G0 wire cases) and MetalSourceGradePixelTest.cpp
// (G1 pixel proof). Grading plan 2026-11-16, Tasks 2 and 3.
#include "core/SourceBus.h"
#include "modules/Interfaces.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace corevideo::gradetest {

// One solid-colour BGRA bus source with a settable epoch and presence.
// kind MUST be "test": a "capture"-kind bus source would be removed by
// syncCaptureSources (core/CaptureBusRoster.h), because no capture adapter emits it.
class EpochSource final : public core::ISource {
 public:
  EpochSource(std::string sourceId, uint8_t r, uint8_t g, uint8_t b, uint64_t epoch,
              int width = 64, int height = 36)
      : epoch_(epoch) {
    descriptor_.sourceId = std::move(sourceId);
    descriptor_.kind = "test";
    descriptor_.width = width;
    descriptor_.height = height;
    descriptor_.fpsNumerator = 60;
    descriptor_.fpsDenominator = 1;
    descriptor_.pixelFormat = "bgra";
    descriptor_.hasVideo = true;
    auto pixels = std::make_shared<std::vector<uint8_t>>(size_t(width) * size_t(height) * 4);
    for (size_t i = 0; i + 3 < pixels->size(); i += 4) {
      (*pixels)[i] = b; (*pixels)[i + 1] = g; (*pixels)[i + 2] = r; (*pixels)[i + 3] = 0xff;
    }
    pixels_ = std::move(pixels);
  }
  void setEpoch(uint64_t epoch) { epoch_.store(epoch); }
  void setPresent(bool present) { present_.store(present); }
  const core::SourceDescriptor& descriptor() const override { return descriptor_; }
  core::SourceTick poll(int64_t programTime100ns) override {
    core::SourceTick tick;
    if (!present_.load()) { tick.health = core::SourceHealth::Idle; return tick; }
    modules::VideoFrame frame;
    frame.participantId = descriptor_.sourceId;
    frame.width = frame.pixelWidth = frame.naturalWidth = descriptor_.width;
    frame.height = frame.pixelHeight = frame.naturalHeight = descriptor_.height;
    frame.pixelStride = descriptor_.width * 4;
    frame.pixels = pixels_;
    frame.frameId = ++frameId_;
    frame.timestampMs = programTime100ns / 10000;
    frame.sourceEpoch = epoch_.load();
    counters_.framesIngested = static_cast<uint64_t>(frameId_);
    counters_.lastFrameId = frameId_;
    tick.video.push_back(std::move(frame));
    tick.health = core::SourceHealth::Producing;
    return tick;
  }
  core::SourceIngestCounters counters() const override { return counters_; }

 private:
  core::SourceDescriptor descriptor_;
  std::shared_ptr<const std::vector<uint8_t>> pixels_;
  std::atomic<uint64_t> epoch_;
  std::atomic<bool> present_{true};
  int64_t frameId_ = 0;
  core::SourceIngestCounters counters_;
};

// Wraps a real compositor, reports a non-"software" renderer so set-source-grade
// is admitted (MediaCore::applySourceGrade), and keeps the last Program plan it was handed.
class GradeTestCompositor final : public modules::ICompositor {
 public:
  explicit GradeTestCompositor(std::unique_ptr<modules::ICompositor> inner) : inner_(std::move(inner)) {}
  std::string rendererName() const override { return "grade-test"; }
  modules::ProgramFrame render(const modules::CompositorRenderPlan& plan,
                               const std::vector<modules::VideoFrame>& frames) override {
    lastPlan = plan;
    return inner_->render(plan, frames);
  }
  modules::CompositorRenderPlan lastPlan;

 private:
  std::unique_ptr<modules::ICompositor> inner_;
};

}  // namespace corevideo::gradetest
