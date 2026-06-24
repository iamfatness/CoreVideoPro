// Windows WASAPI endpoint capture adapter.
//
// Dev-gated implementation of IAudioCaptureSource. It captures either a render
// endpoint in loopback mode (system audio) or a capture endpoint (mic/device)
// and returns interleaved float PCM frames to MediaCore's mixer each tick.

#include "modules/Interfaces.h"

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
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>

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
  std::string out(static_cast<size_t>(needed - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
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

class WasapiAudioCaptureSource final : public IAudioCaptureSource {
 public:
  ~WasapiAudioCaptureSource() override {
    stopAll();
    if (comInitialized_) {
      ::CoUninitialize();
      comInitialized_ = false;
    }
  }

  void configure(const std::vector<CaptureAudioSourceConfig>& sources) override {
    stopAll();
    warnings_.clear();
    if (!ensureCom()) {
      return;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) {
      warn("Could not create WASAPI device enumerator (hr=" + hexHrCapture(hr) + ").");
      return;
    }

    for (const auto& source : sources) {
      if (source.captureDeviceId.empty() || source.audioSourceKind == "none") {
        continue;
      }
      if (!isWasapiCaptureSource(source)) {
        warn("Audio source " + participantIdForSource(source) + " uses " + source.audioSourceKind +
             "; WASAPI capture does not own that source type yet.");
        continue;
      }
      openSource(enumerator, source);
    }

    safeReleaseCapture(enumerator);
  }

  std::vector<AudioFrame> pollAudioFrames(int64_t timestampMs) override {
    std::vector<AudioFrame> frames;
    for (auto& source : sources_) {
      pollSource(source, timestampMs, frames);
    }
    return frames;
  }

  std::vector<std::string> warnings() const override { return warnings_; }

  std::vector<CaptureAudioSourceMetrics> metrics() const override {
    std::vector<CaptureAudioSourceMetrics> metrics;
    metrics.reserve(sources_.size());
    for (const auto& source : sources_) {
      metrics.push_back(CaptureAudioSourceMetrics{
          source.config.captureDeviceId,
          source.participantId,
          source.config.audioSourceKind,
          source.started,
          source.framesReceived,
          source.sampleRate,
          source.channels,
          source.warning});
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
    int64_t emptyPacketPolls = 0;
    std::string endpointId;
    std::string endpointName;
    std::string warning;
  };

  bool ensureCom() {
    if (comInitialized_) {
      return true;
    }
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
      return true;
    }
    if (FAILED(hr)) {
      warn("Could not initialize COM for WASAPI capture (hr=" + hexHrCapture(hr) + ").");
      return false;
    }
    comInitialized_ = true;
    return true;
  }

  void openSource(IMMDeviceEnumerator* enumerator, const CaptureAudioSourceConfig& config) {
    SourceState state;
    state.config = config;
    state.participantId = participantIdForSource(config);
    state.loopback = isLoopbackSource(config);

    IMMDevice* device = resolveDevice(enumerator, config, state.loopback);
    if (device == nullptr) {
      warn("WASAPI capture source " + state.participantId + " has no matching endpoint.");
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
      warn("Could not activate WASAPI capture client for " + state.participantId + " (hr=" + hexHrCapture(hr) + ").");
      safeReleaseCapture(device);
      cleanup(state);
      return;
    }
    safeReleaseCapture(device);

    hr = state.client->GetMixFormat(&state.mixFormat);
    if (FAILED(hr) || state.mixFormat == nullptr || !describeFormat(state)) {
      warn("Unsupported WASAPI capture format for " + state.participantId + ".");
      cleanup(state);
      return;
    }

    constexpr REFERENCE_TIME kBufferDuration = 2'000'000;
    const DWORD flags = state.loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
    hr = state.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, state.mixFormat, nullptr);
    if (FAILED(hr)) {
      warn("Could not initialize WASAPI capture stream for " + state.participantId + " (hr=" + hexHrCapture(hr) + ").");
      cleanup(state);
      return;
    }

    hr = state.client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&state.captureClient));
    if (FAILED(hr) || state.captureClient == nullptr) {
      warn("Could not obtain WASAPI capture service for " + state.participantId + " (hr=" + hexHrCapture(hr) + ").");
      cleanup(state);
      return;
    }

    hr = state.client->Start();
    if (FAILED(hr)) {
      warn("Could not start WASAPI capture for " + state.participantId + " (hr=" + hexHrCapture(hr) + ").");
      cleanup(state);
      return;
    }

    state.started = true;
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
    if (FAILED(source.captureClient->GetNextPacketSize(&packetFrames))) {
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
      if (FAILED(source.captureClient->GetBuffer(&data, &frameCount, &flags, nullptr, nullptr))) {
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
        source.framesReceived += frameCount;
        source.warning.clear();
        frames.push_back(std::move(frame));
      }

      source.captureClient->ReleaseBuffer(frameCount);
      if (FAILED(source.captureClient->GetNextPacketSize(&packetFrames))) {
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
    for (auto& source : sources_) {
      cleanup(source);
    }
    sources_.clear();
  }

  void cleanup(SourceState& source) {
    if (source.client != nullptr && source.started) {
      source.client->Stop();
    }
    source.started = false;
    safeReleaseCapture(source.captureClient);
    if (source.mixFormat != nullptr) {
      ::CoTaskMemFree(source.mixFormat);
      source.mixFormat = nullptr;
    }
    safeReleaseCapture(source.client);
  }

  void warn(std::string warning) { warnings_.push_back(std::move(warning)); }

  bool comInitialized_ = false;
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
