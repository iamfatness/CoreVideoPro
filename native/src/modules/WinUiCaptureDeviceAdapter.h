#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Interfaces.h"

namespace corevideo::modules {

// Bridges capture-card frames that the WinUI shell reads (Game Capture / Elgato /
// any UVC device via Windows MediaCapture) INTO the native core compositor.
//
// The WinUI shell writes each device's BGRA frames into a named shared-memory
// buffer (a seqlock: header { sequence, width, height, reserved } followed by
// tightly packed BGRA), and announces the buffer name via the
// "register-capture-shm" command. This adapter maps each announced buffer and, on
// every render tick, copies the latest consistent frame out as a VideoFrame keyed
// by "capture:<deviceId>" — the same id a scene route's capture-input layer
// resolves to — so the core composites real capture pixels into the program frame
// (and therefore into recording / streaming output).
//
// All capture-device metadata calls (enumerate / selectInput / connect / ...) are
// delegated to the wrapped inner device so existing behavior is unchanged.
class WinUiCaptureDeviceAdapter final : public ICaptureDevice {
 public:
  explicit WinUiCaptureDeviceAdapter(std::unique_ptr<ICaptureDevice> inner);
  ~WinUiCaptureDeviceAdapter() override;

  std::vector<CaptureDeviceInfo> enumerate() const override { return inner_->enumerate(); }
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

  // Real capture frames from the WinUI shared-memory buffers, merged with whatever
  // the inner device produces (e.g. dev test patterns for hardware adapters).
  std::vector<VideoFrame> pollVideoFrames(int64_t timestampMs) override;

  // MUST forward: the shell bridge carries no audio, but the devices this wraps do
  // (SRT ingest carries its guest's audio embedded in the transport). The
  // ICaptureDevice default returns {} — inheriting it silently swallowed every
  // ingested audio frame while video flowed fine, the same shape as the 1-arg
  // connect() bug above. Any new ICaptureDevice method must be forwarded here.
  std::vector<AudioFrame> pollAudioFrames(int64_t timestampMs) override {
    return inner_->pollAudioFrames(timestampMs);
  }

  // Map (or re-map on size change) a WinUI capture buffer for a device.
  void registerCaptureBuffer(const std::string& deviceId, const std::string& shmName, int width, int height);
  // Release a device's buffer (device disconnected / capture stopped).
  void unregisterCaptureBuffer(const std::string& deviceId);

 private:
  struct Buffer {
    std::string shmName;
    void* mappingHandle = nullptr;  // HANDLE
    const uint8_t* view = nullptr;
    int width = 0;
    int height = 0;
    std::size_t mappedBytes = 0;
    uint32_t lastSequence = 0;
    int64_t frameId = 0;
    // Last consistent frame, held so the compositor always has capture pixels even on
    // render ticks with no new frame (capture ~30/60fps vs the render tick) — otherwise
    // those ticks composite the pink "no pixels" slate (flashing pink program/preview).
    std::shared_ptr<std::vector<uint8_t>> lastPixels;
    int lastWidth = 0;
    int lastHeight = 0;
  };

  void closeBufferLocked(Buffer& buffer);

  std::unique_ptr<ICaptureDevice> inner_;
  mutable std::mutex mutex_;
  std::map<std::string, Buffer> buffers_;
};

}  // namespace corevideo::modules
