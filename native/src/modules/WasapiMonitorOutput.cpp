#include "modules/Interfaces.h"

// The audio monitor output is a dev-machine adapter. COREVIDEO_STUB builds must
// stay portable and headless, so this factory resolves to nullptr unless all
// three gates are explicit: non-stub, dev adapters, and the WASAPI monitor flag.
// When nullptr, MediaCore falls back to the deterministic Beep() pulse so the
// stub build and contract tests run without a playback device.
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_WASAPI_MONITOR

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace corevideo::modules {

namespace {

// Minimal scoped COM pointer (the codebase avoids pulling in WRL/ComPtr here).
template <typename T>
class ComPtrLite {
 public:
  ComPtrLite() = default;
  ~ComPtrLite() { reset(); }
  ComPtrLite(const ComPtrLite&) = delete;
  ComPtrLite& operator=(const ComPtrLite&) = delete;
  ComPtrLite(ComPtrLite&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }

  T** put() {
    reset();
    return &ptr_;
  }
  T* get() const { return ptr_; }
  T* operator->() const { return ptr_; }
  explicit operator bool() const { return ptr_ != nullptr; }
  void reset() {
    if (ptr_) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }

 private:
  T* ptr_ = nullptr;
};

}  // namespace

// Real WASAPI shared-mode monitor output. Renders the interleaved-stereo MON bus
// to the default render endpoint, resampling-free (it requests the device mix
// format and converts the engine's float PCM into it sample-by-sample). On any
// failure it returns false so the caller falls back to the Beep pulse.
class WasapiMonitorOutput final : public IAudioMonitorOutput {
 public:
  WasapiMonitorOutput() { comReady_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
  ~WasapiMonitorOutput() override {
    stop();
    if (comReady_) {
      CoUninitialize();
    }
  }

  bool play(const float* pcm, int frames, int channels, int sampleRate, double volume) override {
    if (!pcm || frames <= 0 || channels <= 0) {
      return false;
    }
    if (!ensureClient(sampleRate, channels)) {
      return false;
    }

    UINT32 padding = 0;
    if (FAILED(client_->GetCurrentPadding(&padding))) {
      return false;
    }
    const UINT32 available = bufferFrames_ > padding ? bufferFrames_ - padding : 0;
    const UINT32 writeFrames = std::min<UINT32>(available, static_cast<UINT32>(frames));
    if (writeFrames == 0) {
      return true;  // device buffer full this tick; treated as played.
    }

    BYTE* data = nullptr;
    if (FAILED(render_->GetBuffer(writeFrames, &data))) {
      return false;
    }
    auto* out = reinterpret_cast<float*>(data);
    const double gain = std::clamp(volume, 0.0, 1.0);
    const int outChannels = mixChannels_;
    for (UINT32 frame = 0; frame < writeFrames; ++frame) {
      for (int channel = 0; channel < outChannels; ++channel) {
        const int srcChannel = std::min(channel, channels - 1);
        const float sample = pcm[static_cast<size_t>(frame) * channels + srcChannel];
        out[static_cast<size_t>(frame) * outChannels + channel] = static_cast<float>(sample * gain);
      }
    }
    if (FAILED(render_->ReleaseBuffer(writeFrames, 0))) {
      return false;
    }
    if (!started_) {
      client_->Start();
      started_ = true;
    }
    return true;
  }

 private:
  bool ensureClient(int sampleRate, int channels) {
    if (client_ && mixSampleRate_ == sampleRate) {
      return true;
    }
    stop();

    ComPtrLite<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(enumerator.put())))) {
      return false;
    }
    ComPtrLite<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))) {
      return false;
    }
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client_.put())))) {
      return false;
    }
    WAVEFORMATEX* mixFormat = nullptr;
    if (FAILED(client_->GetMixFormat(&mixFormat)) || !mixFormat) {
      return false;
    }
    mixChannels_ = mixFormat->nChannels;
    mixSampleRate_ = static_cast<int>(mixFormat->nSamplesPerSec);
    const REFERENCE_TIME bufferDuration = 10 * 10000;  // 10 ms
    const HRESULT init = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, mixFormat, nullptr);
    CoTaskMemFree(mixFormat);
    if (FAILED(init)) {
      return false;
    }
    if (FAILED(client_->GetBufferSize(&bufferFrames_))) {
      return false;
    }
    if (FAILED(client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(render_.put())))) {
      return false;
    }
    (void)channels;
    return true;
  }

  void stop() {
    if (client_ && started_) {
      client_->Stop();
    }
    started_ = false;
    render_.reset();
    client_.reset();
  }

  bool comReady_ = false;
  bool started_ = false;
  ComPtrLite<IAudioClient> client_;
  ComPtrLite<IAudioRenderClient> render_;
  UINT32 bufferFrames_ = 0;
  int mixChannels_ = 2;
  int mixSampleRate_ = 0;
};

std::unique_ptr<IAudioMonitorOutput> createWasapiMonitorOutput() {
  auto output = std::make_unique<WasapiMonitorOutput>();
  return output;
}

}  // namespace corevideo::modules

#else

namespace corevideo::modules {

std::unique_ptr<IAudioMonitorOutput> createWasapiMonitorOutput() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif
