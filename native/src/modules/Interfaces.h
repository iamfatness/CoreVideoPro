#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace corevideo::modules {

struct VideoFrame {
  std::string participantId;
  int width = 0;
  int height = 0;
  int64_t timestampMs = 0;
};

struct AudioFrame {
  std::string participantId;
  int sampleRate = 48000;
  int channels = 1;
  int64_t timestampMs = 0;
};

struct ProgramFrame {
  int width = 1920;
  int height = 1080;
  int layerCount = 0;
  int64_t frameNumber = 0;
};

struct OutputSession {
  bool active = false;
  std::vector<std::string> destinations;
  std::vector<std::string> isoParticipantIds;
  int64_t encodedFrameCount = 0;
};

struct CaptureDeviceInfo {
  std::string id;
  std::string name;
  std::string kind;
};

class IZoomCaptureSource {
 public:
  virtual ~IZoomCaptureSource() = default;
  virtual std::vector<VideoFrame> pollVideoFrames() = 0;
  virtual std::vector<AudioFrame> pollAudioFrames() = 0;
};

class ICompositor {
 public:
  virtual ~ICompositor() = default;
  virtual ProgramFrame render(const std::vector<VideoFrame>& frames, int layerCount) = 0;
};

class IAudioMixer {
 public:
  virtual ~IAudioMixer() = default;
  virtual int64_t mix(const std::vector<AudioFrame>& frames) = 0;
};

class IEncoderSink {
 public:
  virtual ~IEncoderSink() = default;
  virtual OutputSession start(const std::vector<std::string>& destinations, const std::vector<std::string>& isoParticipantIds) = 0;
  virtual void submit(const ProgramFrame& frame) = 0;
  virtual OutputSession session() const = 0;
};

class ICaptureDevice {
 public:
  virtual ~ICaptureDevice() = default;
  virtual std::vector<CaptureDeviceInfo> enumerate() const = 0;
};

struct ModuleSet {
  std::unique_ptr<IZoomCaptureSource> zoom;
  std::unique_ptr<ICompositor> compositor;
  std::unique_ptr<IAudioMixer> mixer;
  std::unique_ptr<IEncoderSink> encoder;
  std::unique_ptr<ICaptureDevice> captureDevice;
};

ModuleSet createStubModules();

}  // namespace corevideo::modules
