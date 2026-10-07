#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Interfaces.h"
#include "ShmCapturePreparation.h"

namespace corevideo::modules {

// Bridges capture-card frames that the WinUI shell reads (Game Capture / Elgato /
// any UVC device via Windows MediaCapture) INTO the native core compositor.
//
// The WinUI shell writes each device's BGRA frames into a named shared-memory
// buffer (a seqlock: header { sequence, width, height, reserved } followed by
// tightly packed BGRA), and announces the buffer name via the
// "register-capture-shm" command. This adapter maps each announced buffer and, on
// a dedicated preparation owner copies consistent frames; render ticks consume
// completed VideoFrame descriptors keyed
// by "capture:<deviceId>" — the same id a scene route's capture-input layer
// resolves to — so the core composites real capture pixels into the program frame
// (and therefore into recording / streaming output).
//
// All capture-device metadata calls (enumerate / selectInput / connect / ...) are
// delegated to the wrapped inner device so existing behavior is unchanged.
class WinUiCaptureDeviceAdapter final : public ICaptureDevice {
 public:
  explicit WinUiCaptureDeviceAdapter(std::unique_ptr<ICaptureDevice> inner,
      std::function<void()> beforeShmCopy = {});
  ~WinUiCaptureDeviceAdapter() override;

  std::vector<CaptureDeviceInfo> enumerate() const override { return inner_->enumerate(); }
  void setVideoConsumerDemand(const std::vector<SourceVideoDemand>& demands) override {
    inner_->setVideoConsumerDemand(demands);
  }
  std::vector<VideoFrame> takeCpuVideoFrames() override { return inner_->takeCpuVideoFrames(); }
  std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId, const std::string& inputId) override {
    return inner_->selectInput(deviceId, inputId);
  }
  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId, int offsetMs) override {
    return inner_->setAudioSyncOffset(deviceId, offsetMs);
  }
  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    return inner_->connect(deviceId);
  }
  // Forward the shell's routing id (outputSourceId) to the inner device. Without this
  // override the ICaptureDevice default drops outputSourceId and calls the 1-arg connect,
  // so native-UVC frames stay keyed by the core's own MF id instead of the shell's
  // captureDeviceId -> the multiview layer lookup misses -> pink tiles. (2026-07-10)
  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId,
                                         const std::string& outputSourceId) override {
    return inner_->connect(deviceId, outputSourceId);
  }

  std::vector<CaptureDeviceInfo> disconnect(const std::string& deviceId) override {
    return inner_->disconnect(deviceId);
  }
  std::vector<CaptureDeviceInfo> configureSrtIngestSources(const std::vector<SrtIngestSourceConfig>& sources) override {
    return inner_->configureSrtIngestSources(sources);
  }
  std::vector<CaptureDeviceInfo> configureRtmpIngestSources(const std::vector<RtmpIngestSourceConfig>& sources) override {
    return inner_->configureRtmpIngestSources(sources);
  }

  // Real capture frames from the WinUI shared-memory buffers, merged with whatever
  // the inner device produces (e.g. dev test patterns for hardware adapters).
  void captureVideoTick(int64_t timestampMs) override;
  CapturePreparationDiagnostics shmCapturePreparationDiagnostics() const override;

  // MUST forward: the shell bridge carries no audio, but the devices this wraps do
  // (SRT ingest carries its guest's audio embedded in the transport). The
  // ICaptureDevice default returns {} — inheriting it silently swallowed every
  // ingested audio frame while video flowed fine, the same shape as the 1-arg
  // connect() bug above. Any new ICaptureDevice method must be forwarded here.
  void captureAudioTick(int64_t timestampMs) override {
    struct Collect final : ICaptureAudioConsumer {
      WinUiCaptureDeviceAdapter* self = nullptr;
      void publish(AudioFrame frame) override { self->postAudio(std::move(frame)); }
    } collect;
    collect.self = this;
    inner_->deliverAudio(collect, timestampMs);
  }
  std::vector<std::string> audioSourceIds() const override { return inner_->audioSourceIds(); }

  // Map (or re-map on size change) a WinUI capture buffer for a device.
  void registerCaptureBuffer(const std::string& deviceId, const std::string& shmName, int width, int height) override;
  // Release a device's buffer (device disconnected / capture stopped).
  void unregisterCaptureBuffer(const std::string& deviceId) override;

 private:
  std::unique_ptr<ICaptureDevice> inner_;
  ShmCapturePreparation preparation_;
};

}  // namespace corevideo::modules
