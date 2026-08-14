// Windows WASAPI endpoint capture adapter.
//
// Dev-gated implementation of IAudioCaptureSource. It captures either a render
// endpoint in loopback mode (system audio) or a capture endpoint (mic/device)
// and returns interleaved float PCM frames to MediaCore's mixer each tick.

#include "modules/Interfaces.h"
#include "modules/AudioDsp.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace corevideo::modules {

#if defined(COREVIDEO_WITH_WASAPI_CAPTURE) && COREVIDEO_WITH_WASAPI_CAPTURE

}  // namespace corevideo::modules

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace corevideo::modules {
namespace {

std::string wideToUtf8Capture(const wchar_t* wide) {
  if (wide == nullptr) {
    return {};
  }
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) {
    return {};
  }
  std::string out(static_cast<size_t>(needed), '\0');
  const int written = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
  if (written <= 0) return {};
  out.resize(static_cast<size_t>(written));
  if (!out.empty() && out.back() == '\0') out.pop_back();
  return out;
}

std::string asciiLowerCapture(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string normalizeWasapiEndpointId(std::string value) {
  value = asciiLowerCapture(std::move(value));
  constexpr const char* kWinRtMmdevapiMarker = "mmdevapi#";
  const auto marker = value.find(kWinRtMmdevapiMarker);
  if (marker == std::string::npos) {
    return value;
  }

  const auto idStart = marker + std::strlen(kWinRtMmdevapiMarker);
  const auto idEnd = value.find('#', idStart);
  return value.substr(idStart, idEnd == std::string::npos ? std::string::npos : idEnd - idStart);
}

template <typename T>
void safeReleaseCapture(T*& ptr) {
  if (ptr != nullptr) {
    ptr->Release();
    ptr = nullptr;
  }
}

std::string hexHrCapture(HRESULT hr) {
  static const char* kDigits = "0123456789ABCDEF";
  std::string out = "0x";
  const auto value = static_cast<uint32_t>(hr);
  for (int shift = 28; shift >= 0; shift -= 4) {
    out.push_back(kDigits[(value >> shift) & 0xF]);
  }
  return out;
}

std::string participantIdForSource(const CaptureAudioSourceConfig& source) {
  return source.captureDeviceId == "local-machine-audio" ? source.captureDeviceId : "capture:" + source.captureDeviceId;
}

bool isLoopbackSource(const CaptureAudioSourceConfig& source) {
  const auto kind = source.audioSourceKind;
  return kind == "loopback" ||
         kind == "wasapi-loopback" ||
         kind == "wasapi-render-loopback" ||
         kind == "system-loopback";
}

bool isWasapiCaptureSource(const CaptureAudioSourceConfig& source) {
  return isLoopbackSource(source) ||
         source.audioSourceKind == "wasapi-input" ||
         source.audioSourceKind == "wasapi-capture" ||
         source.audioSourceKind == "virtual-device";
}

int64_t monotonicCaptureMs() {
  return static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

class WasapiAudioCaptureSource final : public IAudioCaptureSource {
 public:
  ~WasapiAudioCaptureSource() override {
    stopAll();
  }

  void configure(const std::vector<CaptureAudioSourceConfig>& sources) override {
    stopAll();
    std::vector<CaptureAudioSourceConfig> captureThreadSources;
    {
      std::lock_guard<std::mutex> lock(sourcesMutex_);
      warnings_.clear();
      for (const auto& source : sources) {
        if (source.captureDeviceId.empty() || source.audioSourceKind == "none") {
          continue;
        }
        if (!isWasapiCaptureSource(source)) {
          warn("Audio source " + participantIdForSource(source) + " uses " + source.audioSourceKind +
               "; WASAPI capture does not own that source type yet.");
          continue;
        }
        captureThreadSources.push_back(source);
      }
    }

    startCaptureThread(std::move(captureThreadSources));
  }

  std::vector<AudioFrame> pollAudioFrames(int64_t timestampMs) override {
    (void)timestampMs;
    std::vector<AudioFrame> frames;
    std::lock_guard<std::mutex> lock(sourcesMutex_);
    for (auto& source : sources_) {
      while (!source.pendingFrames.empty()) {
        frames.push_back(std::move(source.pendingFrames.front()));
        source.pendingFrames.pop_front();
      }
      source.queuedFrames = 0;
    }
    return frames;
  }

  std::vector<std::string> warnings() const override {
    std::lock_guard<std::mutex> lock(sourcesMutex_);
    return warnings_;
  }

  std::vector<CaptureAudioSourceMetrics> metrics() const override {
    std::lock_guard<std::mutex> lock(sourcesMutex_);
    std::vector<CaptureAudioSourceMetrics> metrics;
    metrics.reserve(sources_.size());
    for (const auto& source : sources_) {
      metrics.push_back(CaptureAudioSourceMetrics{
          source.config.captureDeviceId,
          source.participantId,
          source.config.audioSourceKind,
          source.started,
          source.framesReceived,
          source.emptyPacketPolls,
          source.sampleRate,
          source.channels,
          source.endpointId,
          source.endpointName,
          source.lastError,
          source.warning,
          source.lastPeakDbfs,
          source.lastRmsDbfs,
          source.signalPresent,
          source.framesRendered,
          source.queuedFrames,
          source.underrunCount,
          source.startedAtMs,
          source.lastFrameAtMs,
          source.stoppedAtMs});
    }
    return metrics;
  }

 private:
  struct SourceState {
    CaptureAudioSourceConfig config;
    std::string participantId;
    bool loopback = false;
    bool started = false;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    int sampleRate = 48000;
    int channels = 2;
    int bytesPerSample = 4;
    bool isFloat = true;
    int64_t framesReceived = 0;
    int64_t framesRendered = 0;
    int64_t queuedFrames = 0;
    int64_t underrunCount = 0;
    int64_t emptyPacketPolls = 0;
    int64_t startedAtMs = 0;
    int64_t lastFrameAtMs = 0;
    int64_t stoppedAtMs = 0;
    std::deque<AudioFrame> pendingFrames;
    double lastPeakDbfs = -120.0;
    double lastRmsDbfs = -120.0;
    bool signalPresent = false;
    std::string endpointId;
    std::string endpointName;
    std::string lastError;
    std::string warning;
  };

  void openSource(IMMDeviceEnumerator* enumerator, const CaptureAudioSourceConfig& config) {
    SourceState state;
    state.config = config;
    state.participantId = participantIdForSource(config);
    state.loopback = isLoopbackSource(config);

    IMMDevice* device = resolveDevice(enumerator, config, state.loopback);
    if (device == nullptr) {
      failOpenSource(state, "WASAPI capture source " + state.participantId + " has no matching endpoint.");
      return;
    }
    state.endpointName = friendlyName(device);
    LPWSTR rawId = nullptr;
    if (SUCCEEDED(device->GetId(&rawId)) && rawId != nullptr) {
      state.endpointId = wideToUtf8Capture(rawId);
      ::CoTaskMemFree(rawId);
    }

    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&state.client));
    if (FAILED(hr) || state.client == nullptr) {
      failOpenSource(state, "Could not activate WASAPI capture client for " + state.participantId + " (hr=" + hexHrCapture(hr) + ").", hr);
      safeReleaseCapture(device);
      return;
    }
    safeReleaseCapture(device);

    hr = state.client->GetMixFormat(&state.mixFormat);
    if (FAILED(hr) || state.mixFormat == nullptr || !describeFormat(state)) {
      failOpenSource(state, "Unsupported WASAPI capture format for " + state.participantId + ".", hr);
      return;
    }

    // Bypass endpoint audio effects when the driver supports raw capture. Some
    // multi-endpoint USB interfaces apply communications processing when a
    // conventional capture client opens, which can disturb their render paths.
    // Raw mode is still shared-mode WASAPI and is deliberately best-effort.
    IAudioClient2* client2 = nullptr;
    if (SUCCEEDED(state.client->QueryInterface(__uuidof(IAudioClient2),
                                               reinterpret_cast<void**>(&client2))) &&
        client2 != nullptr) {
      AudioClientProperties properties{};
      properties.cbSize = sizeof(properties);
      properties.bIsOffload = FALSE;
      properties.eCategory = AudioCategory_Other;
      properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
      const HRESULT propertiesHr = client2->SetClientProperties(&properties);
      if (FAILED(propertiesHr)) {
        warn("WASAPI raw capture is unavailable for " + state.participantId +
             "; continuing in standard shared mode (hr=" + hexHrCapture(propertiesHr) + ").");
      }
      safeReleaseCapture(client2);
    }

    constexpr REFERENCE_TIME kBufferDuration = 2'000'000;
    const DWORD flags = state.loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = state.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, state.mixFormat, nullptr);
    if (FAILED(hr)) {
      failOpenSource(state, "Could not initialize WASAPI capture stream for " + state.participantId + " (hr=" + hexHrCapture(hr) + ").", hr);
      return;
    }

    hr = state.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&state.captureClient));
    if (FAILED(hr) || state.captureClient == nullptr) {
      failOpenSource(state, "Could not obtain WASAPI capture service for " + state.participantId + " (hr=" + hexHrCapture(hr) + ").", hr);
      return;
    }

    hr = state.client->Start();
    if (FAILED(hr)) {
      failOpenSource(state, "Could not start WASAPI capture for " + state.participantId + " (hr=" + hexHrCapture(hr) + ").", hr);
      return;
    }

    state.started = true;
    state.startedAtMs = monotonicCaptureMs();
    state.stoppedAtMs = 0;
    sources_.push_back(std::move(state));
  }

  void failOpenSource(SourceState& state, std::string message, HRESULT hr = S_OK) {
    if (FAILED(hr)) {
      state.lastError = "WASAPI open hr=" + hexHrCapture(hr);
    }
    state.warning = std::move(message);
    warn(state.warning);
    cleanup(state);
    sources_.push_back(std::move(state));
  }

  IMMDevice* resolveDevice(IMMDeviceEnumerator* enumerator, const CaptureAudioSourceConfig& config, bool loopback) {
    const EDataFlow flow = loopback ? eRender : eCapture;
    const std::string requested = !config.nativeAudioDeviceId.empty() ? config.nativeAudioDeviceId : config.audioDeviceId;
    if (requested.empty() || requested == "default" || requested == "default-render" || requested == "default-capture") {
      IMMDevice* device = nullptr;
      enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
      return device;
    }

    IMMDeviceCollection* collection = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection)) || collection == nullptr) {
      IMMDevice* device = nullptr;
      enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
      return device;
    }

    IMMDevice* match = nullptr;
    UINT count = 0;
    collection->GetCount(&count);
    const auto requestedEndpointId = normalizeWasapiEndpointId(requested);
    for (UINT index = 0; index < count && match == nullptr; ++index) {
      IMMDevice* candidate = nullptr;
      if (FAILED(collection->Item(index, &candidate)) || candidate == nullptr) {
        continue;
      }
      LPWSTR rawId = nullptr;
      const bool idMatches = SUCCEEDED(candidate->GetId(&rawId)) &&
                             rawId != nullptr &&
                             normalizeWasapiEndpointId(wideToUtf8Capture(rawId)) == requestedEndpointId;
      if (rawId != nullptr) {
        ::CoTaskMemFree(rawId);
      }
      const bool nameMatches = !config.audioDeviceName.empty() &&
                               asciiLowerCapture(friendlyName(candidate)) == asciiLowerCapture(config.audioDeviceName);
      if (idMatches || nameMatches) {
        match = candidate;
        continue;
      }
      safeReleaseCapture(candidate);
    }
    safeReleaseCapture(collection);

    if (match == nullptr) {
      warn("WASAPI endpoint '" + requested + "' was not found; using the default endpoint for " +
           (loopback ? "loopback." : "capture."));
      enumerator->GetDefaultAudioEndpoint(flow, eConsole, &match);
    }
    return match;
  }

  std::string friendlyName(IMMDevice* device) {
    IPropertyStore* store = nullptr;
    std::string name;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr) {
      PROPVARIANT value;
      ::PropVariantInit(&value);
      if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = wideToUtf8Capture(value.pwszVal);
      }
      ::PropVariantClear(&value);
      safeReleaseCapture(store);
    }
    return name;
  }

  bool describeFormat(SourceState& state) {
    const auto& format = *state.mixFormat;
    state.sampleRate = static_cast<int>(format.nSamplesPerSec);
    state.channels = format.nChannels;
    state.bytesPerSample = format.wBitsPerSample / 8;
    state.isFloat = false;
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
      state.isFloat = true;
    } else if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
      const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
      state.isFloat = ext.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
      if (!state.isFloat && ext.SubFormat != KSDATAFORMAT_SUBTYPE_PCM) {
        return false;
      }
    } else if (format.wFormatTag != WAVE_FORMAT_PCM) {
      return false;
    }
    return state.sampleRate > 0 && state.channels > 0 && (state.bytesPerSample == 2 || state.bytesPerSample == 4);
  }

  void pollSource(SourceState& source, int64_t timestampMs, std::vector<AudioFrame>& frames) {
    if (!source.started || source.captureClient == nullptr) {
      return;
    }

    UINT32 packetFrames = 0;
    HRESULT hr = source.captureClient->GetNextPacketSize(&packetFrames);
    if (FAILED(hr)) {
      ++source.underrunCount;
      source.lastError = "GetNextPacketSize hr=" + hexHrCapture(hr);
      source.warning = "WASAPI capture could not query packet size for " + diagnosticEndpointLabel(source) + ".";
      return;
    }
    if (packetFrames == 0 && source.framesReceived == 0) {
      ++source.emptyPacketPolls;
      source.warning = "WASAPI capture is open on " + diagnosticEndpointLabel(source) +
                       " but the endpoint has not produced loopback packets.";
    }

    while (packetFrames > 0) {
      BYTE* data = nullptr;
      UINT32 frameCount = 0;
      DWORD flags = 0;
      hr = source.captureClient->GetBuffer(&data, &frameCount, &flags, nullptr, nullptr);
      if (FAILED(hr)) {
        ++source.underrunCount;
        source.lastError = "GetBuffer hr=" + hexHrCapture(hr);
        source.warning = "WASAPI capture could not read a packet from " + diagnosticEndpointLabel(source) + ".";
        return;
      }

      if (frameCount > 0) {
        AudioFrame frame;
        frame.participantId = source.participantId;
        frame.sampleRate = source.sampleRate;
        frame.channels = 2;
        frame.timestampMs = timestampMs + source.config.audioSyncOffsetMs;
        frame.sampleCount = static_cast<int>(frameCount);
        frame.pcm.resize(static_cast<size_t>(frameCount) * 2u);
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
          std::fill(frame.pcm.begin(), frame.pcm.end(), 0.f);
        } else {
          convertPacket(source, data, frameCount, frame.pcm);
        }
        source.lastPeakDbfs = computeSamplePeakDbfs(frame.pcm.data(), frame.pcm.size());
        source.lastRmsDbfs = computeRmsDbfs(frame.pcm.data(), frame.pcm.size());
        source.signalPresent = source.lastPeakDbfs > -60.0;
        source.framesReceived += frameCount;
        source.framesRendered += frameCount;
        source.lastFrameAtMs = timestampMs;
        source.queuedFrames = 0;
        source.lastError.clear();
        source.warning.clear();
        frames.push_back(std::move(frame));
      }

      source.captureClient->ReleaseBuffer(frameCount);
      hr = source.captureClient->GetNextPacketSize(&packetFrames);
      if (FAILED(hr)) {
        ++source.underrunCount;
        source.lastError = "GetNextPacketSize hr=" + hexHrCapture(hr);
        source.warning = "WASAPI capture could not query the next packet for " + diagnosticEndpointLabel(source) + ".";
        return;
      }
    }
  }

  std::string diagnosticEndpointLabel(const SourceState& source) const {
    const std::string name = source.endpointName.empty() ? source.config.audioDeviceName : source.endpointName;
    const std::string id = source.endpointId.empty() ? source.config.nativeAudioDeviceId : source.endpointId;
    if (!name.empty() && !id.empty()) {
      return "'" + name + "' (" + id + ")";
    }
    if (!name.empty()) {
      return "'" + name + "'";
    }
    if (!id.empty()) {
      return id;
    }
    return source.participantId;
  }

  void convertPacket(const SourceState& source, const BYTE* data, UINT32 frameCount, std::vector<float>& out) const {
    for (UINT32 frame = 0; frame < frameCount; ++frame) {
      const float left = sampleAt(source, data, frame, 0);
      const float right = source.channels == 1 ? left : sampleAt(source, data, frame, 1);
      out[static_cast<size_t>(frame) * 2u] = left;
      out[static_cast<size_t>(frame) * 2u + 1u] = right;
    }
  }

  float sampleAt(const SourceState& source, const BYTE* data, UINT32 frame, int channel) const {
    const BYTE* slot = data + (static_cast<size_t>(frame) * source.channels + channel) * source.bytesPerSample;
    if (source.isFloat) {
      float value = 0.f;
      std::memcpy(&value, slot, sizeof(float));
      return std::clamp(value, -1.f, 1.f);
    }
    if (source.bytesPerSample == 2) {
      int16_t value = 0;
      std::memcpy(&value, slot, sizeof(int16_t));
      return static_cast<float>(value) / 32768.f;
    }
    int32_t value = 0;
    std::memcpy(&value, slot, sizeof(int32_t));
    return static_cast<float>(static_cast<double>(value) / 2147483648.0);
  }

  void stopAll() {
    stopping_.store(true);
    if (captureThread_.joinable()) {
      captureThread_.join();
    }
    stopping_.store(false);
    std::lock_guard<std::mutex> lock(sourcesMutex_);
    sources_.clear();
  }

  void cleanup(SourceState& source) {
    if (source.client != nullptr && source.started) {
      source.client->Stop();
    }
    source.started = false;
    if (source.startedAtMs > 0) {
      source.stoppedAtMs = monotonicCaptureMs();
    }
    safeReleaseCapture(source.captureClient);
    if (source.mixFormat != nullptr) {
      ::CoTaskMemFree(source.mixFormat);
      source.mixFormat = nullptr;
    }
    safeReleaseCapture(source.client);
  }

  void warn(std::string warning) { warnings_.push_back(std::move(warning)); }

  void startCaptureThread(std::vector<CaptureAudioSourceConfig> sources) {
    if (sources.empty()) {
      return;
    }
    stopping_.store(false);
    captureThread_ = std::thread([this, sources = std::move(sources)]() mutable { captureLoop(std::move(sources)); });
  }

  void captureLoop(std::vector<CaptureAudioSourceConfig> configs) {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool didInitializeCom = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
      std::lock_guard<std::mutex> lock(sourcesMutex_);
      warn("Could not initialize COM on WASAPI capture thread (hr=" + hexHrCapture(hr) + ").");
      return;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    const HRESULT enumeratorHr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                                    __uuidof(IMMDeviceEnumerator),
                                                    reinterpret_cast<void**>(&enumerator));
    if (FAILED(enumeratorHr) || enumerator == nullptr) {
      std::lock_guard<std::mutex> lock(sourcesMutex_);
      warn("Could not create WASAPI device enumerator on capture thread (hr=" + hexHrCapture(enumeratorHr) + ").");
      if (didInitializeCom) {
        ::CoUninitialize();
      }
      return;
    }

    {
      std::lock_guard<std::mutex> lock(sourcesMutex_);
      for (const auto& config : configs) {
        openSource(enumerator, config);
      }
    }
    safeReleaseCapture(enumerator);

    while (!stopping_.load()) {
      {
        std::lock_guard<std::mutex> lock(sourcesMutex_);
        const auto timestampMs = monotonicCaptureMs();
        for (auto& source : sources_) {
          std::vector<AudioFrame> frames;
          pollSource(source, timestampMs, frames);
          for (auto& frame : frames) {
            source.pendingFrames.push_back(std::move(frame));
          }
          constexpr int64_t kMaxQueuedFramesPerSource = 48000;
          int64_t queued = 0;
          for (const auto& frame : source.pendingFrames) {
            queued += frame.sampleCount;
          }
          while (queued > kMaxQueuedFramesPerSource && !source.pendingFrames.empty()) {
            queued -= source.pendingFrames.front().sampleCount;
            source.pendingFrames.pop_front();
            ++source.underrunCount;
          }
          source.queuedFrames = queued;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    {
      std::lock_guard<std::mutex> lock(sourcesMutex_);
      for (auto& source : sources_) {
        cleanup(source);
      }
    }

    if (didInitializeCom) {
      ::CoUninitialize();
    }
  }

  std::atomic<bool> stopping_{false};
  mutable std::mutex sourcesMutex_;
  std::thread captureThread_;
  std::vector<SourceState> sources_;
  std::vector<std::string> warnings_;
};

}  // namespace

std::unique_ptr<IAudioCaptureSource> createWasapiAudioCaptureSource() {
  return std::make_unique<WasapiAudioCaptureSource>();
}

#else

std::unique_ptr<IAudioCaptureSource> createWasapiAudioCaptureSource() {
  return nullptr;
}

#endif

}  // namespace corevideo::modules
