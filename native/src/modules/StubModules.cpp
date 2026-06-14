#include "modules/Interfaces.h"

#include <utility>

namespace corevideo::modules {
namespace {

class SyntheticZoomCaptureSource final : public IZoomCaptureSource {
 public:
  std::vector<VideoFrame> pollVideoFrames() override {
    ++frameNumber_;
    return {
        {"synthetic-speaker-1", 1280, 720, frameNumber_ * 16},
        {"synthetic-speaker-2", 1280, 720, frameNumber_ * 16},
    };
  }

  std::vector<AudioFrame> pollAudioFrames() override {
    return {
        {"synthetic-speaker-1", 48000, 1, frameNumber_ * 16},
        {"synthetic-speaker-2", 48000, 1, frameNumber_ * 16},
    };
  }

 private:
  int64_t frameNumber_ = 0;
};

class CpuNoopCompositor final : public ICompositor {
 public:
  ProgramFrame render(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) override {
    ++frameNumber_;
    const int layerCount = renderPlan.layers.empty() ? static_cast<int>(frames.size()) : static_cast<int>(renderPlan.layers.size());
    return {renderPlan.width, renderPlan.height, layerCount, frameNumber_, renderPlan.renderPlanId, "software"};
  }

 private:
  int64_t frameNumber_ = 0;
};

class PassthroughAudioMixer final : public IAudioMixer {
 public:
  int64_t mix(const std::vector<AudioFrame>& frames) override {
    mixedFrameCount_ += static_cast<int64_t>(frames.size());
    return mixedFrameCount_;
  }

 private:
  int64_t mixedFrameCount_ = 0;
};

class CountingEncoderSink final : public IEncoderSink {
 public:
  OutputSession start(const std::vector<std::string>& destinations, const std::vector<std::string>& isoParticipantIds) override {
    session_.active = true;
    session_.destinations = destinations;
    session_.isoParticipantIds = isoParticipantIds;
    return session_;
  }

  void submit(const ProgramFrame&) override {
    if (session_.active) {
      ++session_.encodedFrameCount;
    }
  }

  OutputSession session() const override { return session_; }

 private:
  OutputSession session_;
};

class FakeCaptureDevice final : public ICaptureDevice {
 public:
  std::vector<CaptureDeviceInfo> enumerate() const override {
    return {
        {"fake-camera-1", "CoreVideo Synthetic Camera", "video"},
        {"fake-microphone-1", "CoreVideo Synthetic Microphone", "audio"},
    };
  }
};

}  // namespace

ModuleSet createStubModules() {
  ModuleSet modules;
  modules.zoom = std::make_unique<SyntheticZoomCaptureSource>();
  modules.compositor = std::make_unique<CpuNoopCompositor>();
  modules.mixer = std::make_unique<PassthroughAudioMixer>();
  modules.encoder = std::make_unique<CountingEncoderSink>();
  modules.captureDevice = std::make_unique<FakeCaptureDevice>();
  return modules;
}

}  // namespace corevideo::modules
