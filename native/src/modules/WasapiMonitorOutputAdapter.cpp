// Windows WASAPI monitor (MON bus) output adapter.
//
// Dev-gated real implementation of IAudioMonitorOutput: it opens a shared-mode
// WASAPI render endpoint and plays the mixer's monitor bus to a real device,
// replacing the placeholder Beep() that the monitor path used to call. Built
// only when COREVIDEO_WITH_WASAPI_MONITOR is set (which requires
// COREVIDEO_ENABLE_DEV_ADAPTERS); in every other build the factory below
// returns nullptr so MediaCore keeps its safe in-memory stub monitor output.
//
// Scope: shared-mode, timer-driven push with overflow-drop (a monitor is
// real-time; dropping a few frames under back-pressure is preferable to
// blocking the media tick). It converts the source stereo float bus to the
// device mix format (float32 or 16/32-bit PCM) and linearly resamples when the
// device runs at a rate other than the source rate. Exclusive mode and ASIO are
// out of scope here (see the F2 completion plan).

#include "modules/Interfaces.h"

#include <memory>
#include <string>
#include <vector>

namespace corevideo::modules {

#if defined(COREVIDEO_WITH_WASAPI_MONITOR) && COREVIDEO_WITH_WASAPI_MONITOR

}  // namespace corevideo::modules

// Keep the heavy Windows headers out of the namespace block.
#ifndef NOMINMAX
#define NOMINMAX  // keep std::min/std::max usable; windows.h defines min/max macros otherwise
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

// Convert a COM wide string to UTF-8 for the transport-neutral string fields.
std::string wideToUtf8(const wchar_t* wide) {
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

std::string asciiLowerMonitor(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string normalizeWasapiEndpointId(std::string value) {
  value = asciiLowerMonitor(std::move(value));
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
void safeRelease(T*& ptr) {
  if (ptr != nullptr) {
    ptr->Release();
    ptr = nullptr;
  }
}

class WasapiMonitorOutput final : public IAudioMonitorOutput {
 public:
  ~WasapiMonitorOutput() override {
    stop();
    if (comInitialized_) {
      ::CoUninitialize();
      comInitialized_ = false;
    }
  }

  bool start(const std::string& deviceId, int sampleRate, int channels) override {
    sourceSampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    sourceChannels_ = channels > 0 ? channels : 2;
    if (active_ && deviceId == openedDeviceId_) {
      return true;  // already streaming the requested endpoint
    }
    stop();
    warnings_.clear();

    if (!ensureCom()) {
      return false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr) || enumerator == nullptr) {
      warn("Could not create the WASAPI device enumerator (hr=" + hexHr(hr) + ").");
      return false;
    }

    IMMDevice* device = resolveDevice(enumerator, deviceId);
    safeRelease(enumerator);
    if (device == nullptr) {
      warn("No active WASAPI render endpoint matched '" + deviceId + "'.");
      return false;
    }

    deviceName_ = readFriendlyName(device, deviceId);

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
    if (FAILED(hr) || client_ == nullptr) {
      warn("Could not activate the WASAPI audio client (hr=" + hexHr(hr) + ").");
      safeRelease(device);
      cleanup();
      return false;
    }
    safeRelease(device);

    hr = client_->GetMixFormat(&mixFormat_);
    if (FAILED(hr) || mixFormat_ == nullptr) {
      warn("Could not read the device mix format (hr=" + hexHr(hr) + ").");
      cleanup();
      return false;
    }
    if (!describeFormat(*mixFormat_)) {
      warn("Unsupported device sample format; only float32 and 16/32-bit PCM are handled.");
      cleanup();
      return false;
    }

    // 200 ms shared-mode endpoint buffer (in 100 ns units).
    constexpr REFERENCE_TIME kBufferDuration = 2'000'000;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0, mixFormat_, nullptr);
    if (FAILED(hr)) {
      warn("Could not initialize the shared-mode render stream (hr=" + hexHr(hr) + ").");
      cleanup();
      return false;
    }

    hr = client_->GetBufferSize(&bufferFrameCount_);
    if (FAILED(hr) || bufferFrameCount_ == 0) {
      warn("Could not query the endpoint buffer size (hr=" + hexHr(hr) + ").");
      cleanup();
      return false;
    }

    hr = client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&renderClient_));
    if (FAILED(hr) || renderClient_ == nullptr) {
      warn("Could not obtain the WASAPI render client (hr=" + hexHr(hr) + ").");
      cleanup();
      return false;
    }

    hr = client_->Start();
    if (FAILED(hr)) {
      warn("Could not start the WASAPI render stream (hr=" + hexHr(hr) + ").");
      cleanup();
      return false;
    }

    active_ = true;
    openedDeviceId_ = deviceId;
    return true;
  }

  void stop() override {
    if (client_ != nullptr && active_) {
      client_->Stop();
    }
    cleanup();
    active_ = false;
    openedDeviceId_.clear();
  }

  bool render(const float* interleaved, int frameCount, int channels, double volume) override {
    if (!active_ || renderClient_ == nullptr || client_ == nullptr || interleaved == nullptr ||
        frameCount <= 0 || channels <= 0) {
      return false;
    }

    // Resample the source bus to the device rate (linear, mono/stereo-aware).
    const double ratio = static_cast<double>(deviceSampleRate_) / static_cast<double>(sourceSampleRate_);
    const int outFrames =
        deviceSampleRate_ == sourceSampleRate_ ? frameCount : std::max(1, static_cast<int>(std::llround(frameCount * ratio)));

    int written = 0;
    while (written < outFrames) {
      UINT32 padding = 0;
      if (FAILED(client_->GetCurrentPadding(&padding))) {
        break;
      }
      const UINT32 available = bufferFrameCount_ > padding ? bufferFrameCount_ - padding : 0;
      if (available == 0) {
        break;  // endpoint full; drop the rest this tick rather than block
      }
      const UINT32 chunk = std::min<UINT32>(available, static_cast<UINT32>(outFrames - written));
      BYTE* buffer = nullptr;
      if (FAILED(renderClient_->GetBuffer(chunk, &buffer)) || buffer == nullptr) {
        break;
      }
      writeChunk(buffer, chunk, written, outFrames, interleaved, frameCount, channels, volume);
      renderClient_->ReleaseBuffer(chunk, 0);
      written += static_cast<int>(chunk);
    }

    framesRendered_ += written;
    return written > 0;
  }

  bool active() const override { return active_; }
  bool hardwareOutput() const override { return true; }
  std::string deviceName() const override { return deviceName_; }
  std::vector<std::string> warnings() const override { return warnings_; }

 private:
  bool ensureCom() {
    if (comInitialized_) {
      return true;
    }
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) {
      return true;  // COM already initialized in another mode on this thread
    }
    if (FAILED(hr)) {
      warn("Could not initialize COM for WASAPI (hr=" + hexHr(hr) + ").");
      return false;
    }
    comInitialized_ = true;
    return true;
  }

  IMMDevice* resolveDevice(IMMDeviceEnumerator* enumerator, const std::string& deviceId) {
    if (deviceId.empty()) {
      IMMDevice* device = nullptr;
      enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
      return device;
    }

    IMMDeviceCollection* collection = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)) || collection == nullptr) {
      IMMDevice* device = nullptr;
      enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
      return device;
    }

    IMMDevice* match = nullptr;
    UINT count = 0;
    collection->GetCount(&count);
    const auto requestedEndpointId = normalizeWasapiEndpointId(deviceId);
    for (UINT index = 0; index < count && match == nullptr; ++index) {
      IMMDevice* candidate = nullptr;
      if (FAILED(collection->Item(index, &candidate)) || candidate == nullptr) {
        continue;
      }
      LPWSTR rawId = nullptr;
      if (SUCCEEDED(candidate->GetId(&rawId)) && rawId != nullptr) {
        const std::string id = normalizeWasapiEndpointId(wideToUtf8(rawId));
        ::CoTaskMemFree(rawId);
        if (id == requestedEndpointId || readFriendlyName(candidate, std::string{}) == deviceId) {
          match = candidate;  // keep reference, do not release
          continue;
        }
      }
      safeRelease(candidate);
    }
    safeRelease(collection);

    if (match == nullptr) {
      // Selected endpoint not found; fall back to the default and note it.
      warn("Render endpoint '" + deviceId + "' not found; using the system default.");
      enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &match);
    }
    return match;
  }

  std::string readFriendlyName(IMMDevice* device, const std::string& fallback) {
    IPropertyStore* store = nullptr;
    std::string name = fallback;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr) {
      PROPVARIANT value;
      ::PropVariantInit(&value);
      if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = wideToUtf8(value.pwszVal);
      }
      ::PropVariantClear(&value);
      safeRelease(store);
    }
    return name;
  }

  bool describeFormat(const WAVEFORMATEX& format) {
    deviceSampleRate_ = static_cast<int>(format.nSamplesPerSec);
    deviceChannels_ = format.nChannels;
    deviceBytesPerSample_ = format.wBitsPerSample / 8;
    deviceIsFloat_ = false;
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
      deviceIsFloat_ = true;
    } else if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
      const auto& ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
      deviceIsFloat_ = ext.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
      if (!deviceIsFloat_ && ext.SubFormat != KSDATAFORMAT_SUBTYPE_PCM) {
        return false;
      }
    } else if (format.wFormatTag != WAVE_FORMAT_PCM) {
      return false;
    }
    if (deviceChannels_ <= 0 || (deviceBytesPerSample_ != 2 && deviceBytesPerSample_ != 4)) {
      return false;
    }
    return true;
  }

  // Fetch the source stereo sample for an output frame, resampling by index.
  void sampleSource(int outIndex, int outFrames, const float* interleaved, int sourceFrames, int sourceChannels,
                    float& left, float& right) const {
    int sourceIndex = outIndex;
    if (deviceSampleRate_ != sourceSampleRate_ && outFrames > 1) {
      const double position = static_cast<double>(outIndex) * (sourceFrames - 1) / static_cast<double>(outFrames - 1);
      sourceIndex = std::clamp(static_cast<int>(std::llround(position)), 0, sourceFrames - 1);
    } else {
      sourceIndex = std::min(outIndex, sourceFrames - 1);
    }
    const float l = interleaved[static_cast<size_t>(sourceIndex) * sourceChannels];
    left = l;
    right = sourceChannels == 1 ? l : interleaved[static_cast<size_t>(sourceIndex) * sourceChannels + 1];
  }

  void writeChunk(BYTE* buffer, UINT32 chunk, int writtenSoFar, int outFrames, const float* interleaved,
                  int sourceFrames, int sourceChannels, double volume) const {
    const auto gain = static_cast<float>(std::clamp(volume, 0.0, 1.0));
    for (UINT32 frame = 0; frame < chunk; ++frame) {
      float left = 0.f;
      float right = 0.f;
      sampleSource(writtenSoFar + static_cast<int>(frame), outFrames, interleaved, sourceFrames, sourceChannels, left,
                   right);
      left *= gain;
      right = right * gain;
      for (int channel = 0; channel < deviceChannels_; ++channel) {
        const float value = channel == 0 ? left : (channel == 1 ? right : 0.f);
        BYTE* slot = buffer + (static_cast<size_t>(frame) * deviceChannels_ + channel) * deviceBytesPerSample_;
        if (deviceIsFloat_) {
          const float clamped = std::clamp(value, -1.f, 1.f);
          std::memcpy(slot, &clamped, sizeof(float));
        } else if (deviceBytesPerSample_ == 2) {
          const float clamped = std::clamp(value, -1.f, 1.f);
          const auto sample = static_cast<int16_t>(std::llround(clamped * 32767.0f));
          std::memcpy(slot, &sample, sizeof(int16_t));
        } else {  // 32-bit PCM
          const float clamped = std::clamp(value, -1.f, 1.f);
          const auto sample = static_cast<int32_t>(std::llround(static_cast<double>(clamped) * 2147483647.0));
          std::memcpy(slot, &sample, sizeof(int32_t));
        }
      }
    }
  }

  void cleanup() {
    safeRelease(renderClient_);
    if (mixFormat_ != nullptr) {
      ::CoTaskMemFree(mixFormat_);
      mixFormat_ = nullptr;
    }
    safeRelease(client_);
    bufferFrameCount_ = 0;
  }

  void warn(std::string message) { warnings_.push_back(std::move(message)); }

  static std::string hexHr(HRESULT hr) {
    static const char* kDigits = "0123456789ABCDEF";
    std::string out = "0x";
    const auto value = static_cast<uint32_t>(hr);
    for (int shift = 28; shift >= 0; shift -= 4) {
      out.push_back(kDigits[(value >> shift) & 0xF]);
    }
    return out;
  }

  bool comInitialized_ = false;
  bool active_ = false;
  IAudioClient* client_ = nullptr;
  IAudioRenderClient* renderClient_ = nullptr;
  WAVEFORMATEX* mixFormat_ = nullptr;
  UINT32 bufferFrameCount_ = 0;
  int sourceSampleRate_ = 48000;
  int sourceChannels_ = 2;
  int deviceSampleRate_ = 48000;
  int deviceChannels_ = 2;
  int deviceBytesPerSample_ = 4;
  bool deviceIsFloat_ = true;
  int64_t framesRendered_ = 0;
  std::string openedDeviceId_;
  std::string deviceName_;
  std::vector<std::string> warnings_;
};

}  // namespace

std::unique_ptr<IAudioMonitorOutput> createWasapiMonitorOutput() {
  return std::make_unique<WasapiMonitorOutput>();
}

#else  // COREVIDEO_WITH_WASAPI_MONITOR not enabled

std::unique_ptr<IAudioMonitorOutput> createWasapiMonitorOutput() {
  return nullptr;  // default build keeps the in-memory stub monitor output
}

#endif

}  // namespace corevideo::modules
