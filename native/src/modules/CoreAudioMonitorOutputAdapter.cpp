// CoreAudio monitor (MON bus) output — the macOS twin of
// WasapiMonitorOutputAdapter, same pull-model contract
// (docs/audio-pull-monitor-spec.md): the audio worker PUSHES into the shared
// SpscRing via render(); the device PULLS through the AUHAL render callback.
// AUHAL is the pull model natively, so the WASAPI adapter's event-driven
// render thread maps to the callback 1:1, with three deliberate differences:
//  - No producer-side resample: the AU's client stream format is set to the
//    SOURCE rate (float32 interleaved stereo) and the unit's built-in
//    converter handles rate/format conversion to the device — our code never
//    touches sample data (spec §4.1), which also retires the WASAPI ppm trim.
//  - No MMCSS analogue: the CoreAudio IO thread is already time-constrained;
//    the callback obeys "no locks / no allocation / no I/O" instead (it
//    touches only the lock-free ring and atomics).
//  - stop() tears down with AudioOutputUnitStop + Uninitialize, which
//    guarantees no further callbacks — the analogue of joining the WASAPI
//    render thread first.

#include "modules/Interfaces.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_COREAUDIO

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "modules/SpscRing.h"

namespace corevideo::modules {
namespace {

std::string lowered(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string cfStringToStd(CFStringRef value) {
  if (!value) {
    return {};
  }
  char buffer[512] = {0};
  if (CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
    return std::string(buffer);
  }
  return {};
}

std::string deviceStringProperty(AudioDeviceID device, AudioObjectPropertySelector selector) {
  AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  CFStringRef value = nullptr;
  UInt32 size = sizeof(value);
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value) != noErr || !value) {
    return {};
  }
  std::string out = cfStringToStd(value);
  CFRelease(value);
  return out;
}

std::vector<AudioDeviceID> allDevices() {
  AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) !=
      noErr) {
    return {};
  }
  std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size,
                                 devices.data()) != noErr) {
    return {};
  }
  devices.resize(size / sizeof(AudioDeviceID));
  return devices;
}

bool isDefaultSentinel(const std::string& id) {
  return id.empty() || id == "default" || id == "default-render" || id == "default-render-output";
}

class CoreAudioMonitorOutput final : public IAudioMonitorOutput {
 public:
  ~CoreAudioMonitorOutput() override { stop(); }

  bool start(const std::string& deviceId, int sampleRate, int channels) override {
    (void)channels;  // the bus is stereo by the time it reaches the monitor
    if (active_ && deviceId == requestedId_ && sampleRate == sourceRate_) {
      return true;  // idempotent re-target of the identical device
    }
    stop();
    requestedId_ = deviceId;
    sourceRate_ = sampleRate > 0 ? sampleRate : 48000;

    AudioDeviceID device = kAudioObjectUnknown;
    if (!resolveDevice(deviceId, device)) {
      return false;
    }
    deviceName_ = deviceStringProperty(device, kAudioObjectPropertyName);
    resolvedEndpointId_ = deviceStringProperty(device, kAudioDevicePropertyDeviceUID);

    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component || AudioComponentInstanceNew(component, &unit_) != noErr) {
      warn("CoreAudio output unit could not be created.");
      return false;
    }
    OSStatus status = AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice,
                                           kAudioUnitScope_Global, 0, &device, sizeof(device));
    if (status != noErr) {
      warn("CoreAudio output device could not be selected (" + std::to_string(status) + ").");
      teardownUnit();
      return false;
    }
    // Client format = SOURCE rate, float32 interleaved stereo; the AU's
    // converter owns the device-rate/format hop (spec §4.1: never hand-warp).
    AudioStreamBasicDescription format{};
    format.mSampleRate = sourceRate_;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    format.mBytesPerFrame = 8;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = 8;
    status = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                  &format, sizeof(format));
    if (status != noErr) {
      warn("CoreAudio stream format was rejected (" + std::to_string(status) + ").");
      teardownUnit();
      return false;
    }
    AURenderCallbackStruct callback{&CoreAudioMonitorOutput::renderCallback, this};
    status = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0, &callback, sizeof(callback));
    if (status == noErr) {
      status = AudioUnitInitialize(unit_);
    }
    if (status == noErr) {
      status = AudioOutputUnitStart(unit_);
    }
    if (status != noErr) {
      warn("CoreAudio output could not start (" + std::to_string(status) + ").");
      teardownUnit();
      return false;
    }
    ring_.clear();
    active_ = true;
    return true;
  }

  void stop() override {
    if (unit_) {
      // Stop + uninitialize guarantee no further render callbacks — the
      // analogue of joining the WASAPI render thread before teardown. The
      // callback takes no app locks, so this cannot deadlock under
      // audioOutputMutex_.
      AudioOutputUnitStop(unit_);
      teardownUnit();
    }
    active_ = false;
  }

  bool render(const float* interleaved, int frameCount, int channels, double volume) override {
    if (!active_ || frameCount <= 0 || channels <= 0) {
      return false;
    }
    const float gain = static_cast<float>(std::clamp(volume, 0.0, 1.0));
    stereoScratch_.resize(static_cast<size_t>(frameCount) * 2);
    for (int i = 0; i < frameCount; ++i) {
      const size_t base = static_cast<size_t>(i) * channels;
      const float left = interleaved[base] * gain;
      const float right = channels >= 2 ? interleaved[base + 1] * gain : left;
      stereoScratch_[static_cast<size_t>(i) * 2] = left;
      stereoScratch_[static_cast<size_t>(i) * 2 + 1] = right;
    }
    const size_t accepted = ring_.push(stereoScratch_.data(), static_cast<size_t>(frameCount));
    if (accepted < static_cast<size_t>(frameCount)) {
      dropFrames_ += static_cast<int64_t>(frameCount) - static_cast<int64_t>(accepted);
    }
    return accepted > 0;
  }

  bool active() const override { return active_; }
  bool hardwareOutput() const override { return true; }
  std::string deviceName() const override { return deviceName_; }
  std::vector<std::string> warnings() const override { return warnings_; }
  std::int64_t underrunCount() const override {
    return dryEvents_.load(std::memory_order_relaxed);
  }
  std::string resolvedEndpointId() const override { return resolvedEndpointId_; }

 private:
  static OSStatus renderCallback(void* refCon, AudioUnitRenderActionFlags* /*flags*/,
                                 const AudioTimeStamp* /*timestamp*/, UInt32 /*bus*/,
                                 UInt32 frames, AudioBufferList* ioData) {
    auto* self = static_cast<CoreAudioMonitorOutput*>(refCon);
    auto* out = static_cast<float*>(ioData->mBuffers[0].mData);
    const size_t real = self->ring_.pop(out, frames);  // dry tail = silence
    if (real < frames) {
      self->dryFrames_.fetch_add(static_cast<int64_t>(frames - real), std::memory_order_relaxed);
      self->dryEvents_.fetch_add(1, std::memory_order_relaxed);
    }
    return noErr;
  }

  bool resolveDevice(const std::string& requestedId, AudioDeviceID& out) {
    if (isDefaultSentinel(requestedId)) {
      AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
      UInt32 size = sizeof(out);
      if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &out) !=
              noErr ||
          out == kAudioObjectUnknown) {
        warn("No default CoreAudio output device is available.");
        return false;
      }
      return true;
    }
    const std::string wanted = lowered(requestedId);
    for (AudioDeviceID device : allDevices()) {
      if (lowered(deviceStringProperty(device, kAudioDevicePropertyDeviceUID)) == wanted ||
          lowered(deviceStringProperty(device, kAudioObjectPropertyName)) == wanted) {
        out = device;
        return true;
      }
    }
    warn("Monitor device '" + requestedId + "' was not found; using the system default output.");
    return resolveDevice("", out);
  }

  void teardownUnit() {
    if (unit_) {
      AudioUnitUninitialize(unit_);
      AudioComponentInstanceDispose(unit_);
      unit_ = nullptr;
    }
  }

  void warn(const std::string& message) {
    warnings_.push_back(message);
    if (warnings_.size() > 8) {
      warnings_.erase(warnings_.begin());
    }
    std::fprintf(stderr, "[coreaudio-monitor] %s\n", message.c_str());
  }

  AudioUnit unit_ = nullptr;
  SpscRing ring_{16384};  // ≈340ms at 48k, same envelope as the WASAPI adapter
  std::vector<float> stereoScratch_;
  std::string requestedId_;
  std::string deviceName_;
  std::string resolvedEndpointId_;
  std::vector<std::string> warnings_;
  int sourceRate_ = 48000;
  bool active_ = false;
  int64_t dropFrames_ = 0;
  std::atomic<int64_t> dryFrames_{0};
  std::atomic<int64_t> dryEvents_{0};
};

}  // namespace

std::unique_ptr<IAudioMonitorOutput> createCoreAudioMonitorOutput() {
  return std::make_unique<CoreAudioMonitorOutput>();
}

}  // namespace corevideo::modules

#else  // gate

#include <memory>

namespace corevideo::modules {

std::unique_ptr<IAudioMonitorOutput> createCoreAudioMonitorOutput() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_COREAUDIO
