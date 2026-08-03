// CoreAudio capture-audio adapter — the macOS twin of
// WasapiAudioCaptureSourceAdapter: paired capture-device audio (microphones,
// interface inputs, virtual devices) into AudioFrames keyed
// `capture:<captureDeviceId>` (the ISO-3 pairing rule; `local-machine-audio`
// passes through verbatim), with truthful per-source metrics rows even for
// sources that failed to open.
//
// One AUHAL unit per source: input scope enabled on bus 1, output disabled,
// device pinned, input callback -> AudioUnitRender -> stereo float frames
// pushed into a per-source deque (shed past ~1s, counted as underruns), which
// pollAudioFrames() drains non-blocking on the audio worker's gather phase.
// The client format keeps the DEVICE's native sample rate so
// AudioFrame::sampleRate stays truthful (the core owns rate matching), the AU
// converts channels only.
//
// Loopback ("wasapi-loopback"-kind) sources: macOS has no WASAPI-loopback
// equivalent — system audio needs a virtual loopback device (BlackHole-class)
// selected EXPLICITLY. A loopback source with a default-render sentinel fails
// LOUDLY with that explanation in its metrics row; a loopback source with a
// real device UID captures it as an input (the kind string keeps its
// "loopback" substring so the monitor feedback guard still classifies it).

#include "modules/Interfaces.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_COREAUDIO

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "modules/AudioDsp.h"

namespace corevideo::modules {
namespace {

std::string caLowered(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string caCfString(CFStringRef value) {
  if (!value) {
    return {};
  }
  char buffer[512] = {0};
  if (CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
    return std::string(buffer);
  }
  return {};
}

std::string caDeviceString(AudioDeviceID device, AudioObjectPropertySelector selector) {
  AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  CFStringRef value = nullptr;
  UInt32 size = sizeof(value);
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &value) != noErr || !value) {
    return {};
  }
  std::string out = caCfString(value);
  CFRelease(value);
  return out;
}

std::vector<AudioDeviceID> caAllDevices() {
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

// Same keying as the WASAPI adapter and the stub: the VIDEO device id pairs
// capture audio to its ISO writer and routing-matrix row.
std::string participantIdForSource(const CaptureAudioSourceConfig& config) {
  if (config.captureDeviceId == "local-machine-audio") {
    return config.captureDeviceId;
  }
  return "capture:" + config.captureDeviceId;
}

bool isLoopbackKind(const std::string& kind) {
  return kind.find("loopback") != std::string::npos;
}

bool isInputKind(const std::string& kind) {
  return kind == "wasapi-input" || kind == "wasapi-capture" || kind == "virtual-device" ||
         kind == "coreaudio-input";
}

int64_t monotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

class CoreAudioCaptureSource final : public IAudioCaptureSource {
 public:
  ~CoreAudioCaptureSource() override { stopAll(); }

  void configure(const std::vector<CaptureAudioSourceConfig>& sources) override {
    stopAll();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& config : sources) {
      if (config.captureDeviceId.empty() || config.audioSourceKind == "none") {
        continue;
      }
      auto state = std::make_unique<SourceState>();
      state->config = config;
      state->participantId = participantIdForSource(config);
      if (!isLoopbackKind(config.audioSourceKind) && !isInputKind(config.audioSourceKind)) {
        state->lastError = "Audio source kind '" + config.audioSourceKind +
                           "' is not supported by the CoreAudio adapter.";
        state->warning = state->lastError;
        sources_.push_back(std::move(state));
        continue;
      }
      openSource(*state);
      sources_.push_back(std::move(state));
    }
  }

  std::vector<AudioFrame> pollAudioFrames(int64_t timestampMs) override {
    (void)timestampMs;  // frames are stamped on the capture callback
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AudioFrame> out;
    for (auto& source : sources_) {
      while (!source->pending.empty()) {
        out.push_back(std::move(source->pending.front()));
        source->pending.pop_front();
      }
      source->queuedFrames = 0;
    }
    return out;
  }

  std::vector<std::string> warnings() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    for (const auto& source : sources_) {
      if (!source->warning.empty()) {
        out.push_back(source->warning);
      }
    }
    return out;
  }

  std::vector<CaptureAudioSourceMetrics> metrics() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CaptureAudioSourceMetrics> out;
    out.reserve(sources_.size());
    for (const auto& source : sources_) {
      CaptureAudioSourceMetrics metric;
      metric.captureDeviceId = source->config.captureDeviceId;
      metric.sourceId = source->participantId;
      metric.audioSourceKind = source->config.audioSourceKind;
      metric.streaming = source->started;
      metric.framesReceived = source->framesReceived;
      metric.emptyPacketPolls = source->emptyPacketPolls;
      metric.sampleRate = source->sampleRate;
      metric.channels = 2;
      metric.endpointId = source->endpointId;
      metric.endpointName = source->endpointName;
      metric.lastError = source->lastError;
      metric.warning = source->warning;
      metric.peakDbfs = source->peakDbfs;
      metric.rmsDbfs = source->rmsDbfs;
      metric.signalPresent = source->signalPresent;
      metric.queuedFrames = source->queuedFrames;
      metric.underrunCount = source->underrunCount;
      metric.startedAtMs = source->startedAtMs;
      metric.lastFrameAtMs = source->lastFrameAtMs;
      out.push_back(std::move(metric));
    }
    return out;
  }

 private:
  struct SourceState {
    CaptureAudioSourceConfig config;
    std::string participantId;
    AudioUnit unit = nullptr;
    std::deque<AudioFrame> pending;
    std::string endpointId;
    std::string endpointName;
    std::string lastError;
    std::string warning;
    int sampleRate = 0;
    bool started = false;
    int64_t framesReceived = 0;
    int64_t emptyPacketPolls = 0;
    int64_t queuedFrames = 0;
    int64_t underrunCount = 0;
    int64_t startedAtMs = 0;
    int64_t lastFrameAtMs = 0;
    double peakDbfs = -120.0;
    double rmsDbfs = -120.0;
    bool signalPresent = false;
    CoreAudioCaptureSource* owner = nullptr;
  };

  bool resolveDevice(SourceState& state, AudioDeviceID& out) {
    const std::string requested = !state.config.nativeAudioDeviceId.empty()
                                      ? state.config.nativeAudioDeviceId
                                      : state.config.audioDeviceId;
    const bool loopback = isLoopbackKind(state.config.audioSourceKind);
    const bool defaultSentinel = requested.empty() || requested == "default" ||
                                 requested == "default-capture" || requested == "default-render";
    if (loopback && defaultSentinel) {
      state.lastError =
          "System-audio loopback on macOS needs a virtual loopback device (e.g. BlackHole) "
          "selected explicitly; there is no default loopback endpoint.";
      return false;
    }
    if (defaultSentinel) {
      AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultInputDevice,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
      UInt32 size = sizeof(out);
      if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &out) !=
              noErr ||
          out == kAudioObjectUnknown) {
        state.lastError = "No default CoreAudio input device is available.";
        return false;
      }
      return true;
    }
    const std::string wanted = caLowered(requested);
    for (AudioDeviceID device : caAllDevices()) {
      if (caLowered(caDeviceString(device, kAudioDevicePropertyDeviceUID)) == wanted ||
          caLowered(caDeviceString(device, kAudioObjectPropertyName)) == wanted) {
        out = device;
        return true;
      }
    }
    state.lastError = "Audio device '" + requested + "' was not found.";
    return false;
  }

  void openSource(SourceState& state) {
    AudioDeviceID device = kAudioObjectUnknown;
    if (!resolveDevice(state, device)) {
      state.warning = state.lastError;
      return;
    }
    state.endpointId = caDeviceString(device, kAudioDevicePropertyDeviceUID);
    state.endpointName = caDeviceString(device, kAudioObjectPropertyName);

    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_HALOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component || AudioComponentInstanceNew(component, &state.unit) != noErr) {
      fail(state, "CoreAudio input unit could not be created.");
      return;
    }
    UInt32 enable = 1;
    UInt32 disable = 0;
    OSStatus status = AudioUnitSetProperty(state.unit, kAudioOutputUnitProperty_EnableIO,
                                           kAudioUnitScope_Input, 1, &enable, sizeof(enable));
    if (status == noErr) {
      status = AudioUnitSetProperty(state.unit, kAudioOutputUnitProperty_EnableIO,
                                    kAudioUnitScope_Output, 0, &disable, sizeof(disable));
    }
    if (status == noErr) {
      status = AudioUnitSetProperty(state.unit, kAudioOutputUnitProperty_CurrentDevice,
                                    kAudioUnitScope_Global, 0, &device, sizeof(device));
    }
    if (status != noErr) {
      fail(state, "CoreAudio input device could not be selected (" + std::to_string(status) + ").");
      return;
    }
    // Keep the DEVICE's native rate so AudioFrame::sampleRate is truthful; the
    // AU converts channel count to interleaved stereo only.
    Float64 nominalRate = 48000.0;
    AudioObjectPropertyAddress rateAddress{kAudioDevicePropertyNominalSampleRate,
                                           kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
    UInt32 rateSize = sizeof(nominalRate);
    AudioObjectGetPropertyData(device, &rateAddress, 0, nullptr, &rateSize, &nominalRate);
    state.sampleRate = static_cast<int>(nominalRate);

    AudioStreamBasicDescription format{};
    format.mSampleRate = nominalRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    format.mBytesPerFrame = 8;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = 8;
    status = AudioUnitSetProperty(state.unit, kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output, 1, &format, sizeof(format));
    if (status != noErr) {
      fail(state, "CoreAudio input format was rejected (" + std::to_string(status) + ").");
      return;
    }
    state.owner = this;
    AURenderCallbackStruct callback{&CoreAudioCaptureSource::inputCallback, &state};
    status = AudioUnitSetProperty(state.unit, kAudioOutputUnitProperty_SetInputCallback,
                                  kAudioUnitScope_Global, 0, &callback, sizeof(callback));
    if (status == noErr) {
      status = AudioUnitInitialize(state.unit);
    }
    if (status == noErr) {
      status = AudioOutputUnitStart(state.unit);
    }
    if (status != noErr) {
      fail(state, "CoreAudio input could not start (" + std::to_string(status) + ").");
      return;
    }
    state.started = true;
    state.startedAtMs = monotonicMs();
  }

  void fail(SourceState& state, const std::string& why) {
    state.lastError = why;
    state.warning = why;
    if (state.unit) {
      AudioComponentInstanceDispose(state.unit);
      state.unit = nullptr;
    }
    std::fprintf(stderr, "[coreaudio-capture] %s (%s)\n", why.c_str(),
                 state.config.captureDeviceId.c_str());
  }

  static OSStatus inputCallback(void* refCon, AudioUnitRenderActionFlags* flags,
                                const AudioTimeStamp* timestamp, UInt32 bus, UInt32 frames,
                                AudioBufferList* /*unused*/) {
    auto* state = static_cast<SourceState*>(refCon);
    auto* self = state->owner;
    if (!self || frames == 0) {
      return noErr;
    }
    std::vector<float> pcm(static_cast<size_t>(frames) * 2);
    AudioBufferList buffers{};
    buffers.mNumberBuffers = 1;
    buffers.mBuffers[0].mNumberChannels = 2;
    buffers.mBuffers[0].mDataByteSize = static_cast<UInt32>(pcm.size() * sizeof(float));
    buffers.mBuffers[0].mData = pcm.data();
    if (AudioUnitRender(state->unit, flags, timestamp, bus, frames, &buffers) != noErr) {
      return noErr;  // dropped packet; the staleness warning path surfaces it
    }
    AudioFrame frame;
    frame.participantId = state->participantId;
    frame.sampleRate = state->sampleRate;
    frame.channels = 2;
    frame.sampleCount = static_cast<int>(frames);
    frame.timestampMs = monotonicMs() + state->config.audioSyncOffsetMs;
    frame.pcm = std::move(pcm);
    const double peak = computeSamplePeakDbfs(frame.pcm.data(), frame.pcm.size());
    const double rms = computeRmsDbfs(frame.pcm.data(), frame.pcm.size());
    frame.peakLevel = peak;
    frame.rmsLevel = rms;
    std::lock_guard<std::mutex> lock(self->mutex_);
    state->framesReceived += frames;
    state->queuedFrames += frames;
    state->lastFrameAtMs = frame.timestampMs;
    state->peakDbfs = peak;
    state->rmsDbfs = rms;
    state->signalPresent = peak > -60.0;
    state->pending.push_back(std::move(frame));
    // Shed past ~1s so an un-drained queue never grows unbounded (counted as
    // underruns, mirroring the WASAPI adapter).
    while (state->queuedFrames > 48000 && !state->pending.empty()) {
      state->queuedFrames -= state->pending.front().sampleCount;
      state->pending.pop_front();
      ++state->underrunCount;
    }
    return noErr;
  }

  void stopAll() {
    std::vector<std::unique_ptr<SourceState>> retired;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      retired.swap(sources_);
    }
    // Stop units OUTSIDE the mutex: AudioOutputUnitStop synchronizes with the
    // input callback, which takes the mutex.
    for (auto& source : retired) {
      if (source->unit) {
        AudioOutputUnitStop(source->unit);
        AudioUnitUninitialize(source->unit);
        AudioComponentInstanceDispose(source->unit);
        source->unit = nullptr;
      }
    }
  }

  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<SourceState>> sources_;
};

}  // namespace

std::unique_ptr<IAudioCaptureSource> createCoreAudioCaptureSource() {
  return std::make_unique<CoreAudioCaptureSource>();
}

}  // namespace corevideo::modules

#else  // gate

#include <memory>

namespace corevideo::modules {

std::unique_ptr<IAudioCaptureSource> createCoreAudioCaptureSource() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_COREAUDIO
