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

#include <cstdio>
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
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propidl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <thread>

#include "modules/SpscRing.h"

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
    // Record the RESOLVED endpoint id (an empty request resolves to the OS
    // default) so the feedback guard can compare it against loopback capture
    // endpoints (spec R6).
    resolvedEndpointId_.clear();
    {
      LPWSTR rawId = nullptr;
      if (SUCCEEDED(device->GetId(&rawId)) && rawId != nullptr) {
        resolvedEndpointId_ = wideToUtf8(rawId);
        ::CoTaskMemFree(rawId);
      }
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
    if (FAILED(hr) || client_ == nullptr) {
      warn("Could not activate the WASAPI audio client (hr=" + hexHr(hr) + ").");
      safeRelease(device);
      cleanup();
      return false;
    }

    // Endpoint-contention guard (zoom-audio-spec O4): if ANOTHER process has
    // an active render session on this endpoint (e.g. the Zoom client playing
    // the meeting while the operator monitors it here), the operator hears a
    // superposition our PCM taps can never see. Detect it and say so — the
    // 2026-07-05 hunt burned hours on exactly this invisible second copy.
    warnAboutForeignRenderSessions(device);
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

    // Pull model (docs/audio-pull-monitor-spec.md): the DEVICE paces delivery
    // via the event; a dedicated render thread pulls from the SPSC ring. The
    // engine's resampler (RATEADJUST) absorbs clock drift via the slow
    // ring-depth trim — our code never time-warps samples.
    constexpr REFERENCE_TIME kBufferDuration = 2'000'000;  // 200ms endpoint buffer
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                             AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_RATEADJUST,
                             kBufferDuration, 0, mixFormat_, nullptr);
    if (FAILED(hr)) {
      warn("Could not initialize the shared-mode render stream (hr=" + hexHr(hr) + ").");
      cleanup();
      return false;
    }

    renderEvent_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (renderEvent_ == nullptr || FAILED(client_->SetEventHandle(renderEvent_))) {
      warn("Could not arm the event-driven render callback.");
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

    // Drift correction v3: the ENGINE resamples (SetSampleRate a few ppm on a
    // slow loop) - we never touch sample data. Absence is non-fatal: without
    // the service the cushion drains over ~27min and re-primes (one soft
    // hiccup), which beats any hand-rolled warp.
    if (FAILED(client_->GetService(__uuidof(IAudioClockAdjustment), reinterpret_cast<void**>(&clockAdjust_)))) {
      clockAdjust_ = nullptr;
    }

    hr = client_->Start();
    if (FAILED(hr)) {
      warn("Could not start the WASAPI render stream (hr=" + hexHr(hr) + ").");
      cleanup();
      return false;
    }

    ring_.clear();
    ringDryFrames_ = 0;
    ringDropFrames_ = 0;
    renderRun_.store(true, std::memory_order_release);
    renderThread_ = std::thread([this] { renderLoop(); });

    active_ = true;
    openedDeviceId_ = deviceId;
    return true;
  }

  void stop() override {
    // Join the render thread FIRST (it takes no app locks, so joining here —
    // under audioOutputMutex_ — cannot deadlock; see the engine-off teardown
    // rules). Only then stop the client and release COM objects it used.
    renderRun_.store(false, std::memory_order_release);
    if (renderEvent_ != nullptr) {
      ::SetEvent(renderEvent_);
    }
    if (renderThread_.joinable()) {
      renderThread_.join();
    }
    if (client_ != nullptr && active_) {
      client_->Stop();
    }
    cleanup();
    active_ = false;
    openedDeviceId_.clear();
    resolvedEndpointId_.clear();
  }

  // PULL MODEL (docs/audio-pull-monitor-spec.md): render() is now a pure,
  // non-blocking PUSH into the SPSC ring from the audio worker. The device-
  // paced render thread (renderLoop) is the only writer to WASAPI. This
  // removes the padding race that generated every transport artifact class of
  // the 2026-07-05 click hunt (grain, drift, servo splice/flutter).
  bool render(const float* interleaved, int frameCount, int channels, double volume) override {
    if (!active_ || interleaved == nullptr || frameCount <= 0 || channels <= 0) {
      return false;
    }

    // Convert to device-rate stereo in the worker (lerp only when rates
    // genuinely differ) and apply volume at push.
    const double ratio = static_cast<double>(deviceSampleRate_) / static_cast<double>(sourceSampleRate_);
    const int outFrames =
        deviceSampleRate_ == sourceSampleRate_ ? frameCount : std::max(1, static_cast<int>(std::llround(frameCount * ratio)));
    pushScratch_.resize(static_cast<size_t>(outFrames) * 2);
    const auto gain = static_cast<float>(std::clamp(volume, 0.0, 1.0));
    for (int outIndex = 0; outIndex < outFrames; ++outIndex) {
      float left = 0.f;
      float right = 0.f;
      sampleSource(outIndex, outFrames, interleaved, frameCount, channels, left, right);
      pushScratch_[static_cast<size_t>(outIndex) * 2] = left * gain;
      pushScratch_[static_cast<size_t>(outIndex) * 2 + 1] = right * gain;
    }

    const size_t accepted = ring_.push(pushScratch_.data(), static_cast<size_t>(outFrames));
    if (accepted < static_cast<size_t>(outFrames)) {
      ringDropFrames_ += static_cast<int64_t>(outFrames) - static_cast<int64_t>(accepted);
      // Bounded-latency policy: with the depth trim active this should never
      // fire in steady state; if it does, it is telemetry, not a click (the
      // ring seam stays continuous — only the newest tail is lost).
    }
    framesRendered_ += static_cast<int64_t>(accepted);
    return accepted > 0;
  }

  bool active() const override { return active_; }
  bool hardwareOutput() const override { return true; }
  std::string deviceName() const override { return deviceName_; }
  std::vector<std::string> warnings() const override { return warnings_; }
  // Telemetry continuity: "underruns" now means ring-dry EVENTS (the device
  // asked for audio the worker had not produced yet).
  std::int64_t underrunCount() const override { return ringDryEvents_.load(std::memory_order_relaxed); }
  std::string resolvedEndpointId() const override { return resolvedEndpointId_; }

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

  // Fetch the source stereo sample for an output frame. LINEAR interpolation
  // (not nearest-index): with the servo stretching blocks by ±1 frame, nearest
  // rounding concentrated the whole correction into one audible step — lerp
  // spreads it smoothly across the block. The mapping branch also engages when
  // the servo changed the block length at equal rates.
  void sampleSource(int outIndex, int outFrames, const float* interleaved, int sourceFrames, int sourceChannels,
                    float& left, float& right) const {
    const auto fetch = [&](int frame, float& outL, float& outR) {
      const int clamped = std::clamp(frame, 0, sourceFrames - 1);
      const float l = interleaved[static_cast<size_t>(clamped) * sourceChannels];
      outL = l;
      outR = sourceChannels == 1 ? l : interleaved[static_cast<size_t>(clamped) * sourceChannels + 1];
    };
    if (deviceSampleRate_ != sourceSampleRate_ && outFrames > 1) {
      const double position = static_cast<double>(outIndex) * (sourceFrames - 1) / static_cast<double>(outFrames - 1);
      const int base = static_cast<int>(position);
      const float frac = static_cast<float>(position - base);
      float l0 = 0.f, r0 = 0.f, l1 = 0.f, r1 = 0.f;
      fetch(base, l0, r0);
      fetch(base + 1, l1, r1);
      left = l0 + (l1 - l0) * frac;
      right = r0 + (r1 - r0) * frac;
    } else {
      fetch(std::min(outIndex, sourceFrames - 1), left, right);
    }
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

  // Device-paced consumer: waits for the endpoint's event, then fills ALL the
  // space the device exposes from the ring (silence on dry, counted). Takes
  // NO application locks and does no file I/O — real-time discipline per the
  // pull-model spec §4.
  void renderLoop() {
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    double depthAccum = 0.0;
    int depthSamples = 0;
    while (renderRun_.load(std::memory_order_acquire)) {
      if (::WaitForSingleObject(renderEvent_, 500) != WAIT_OBJECT_0) {
        continue;  // periodic timeout keeps shutdown responsive
      }
      if (!renderRun_.load(std::memory_order_acquire)) {
        break;
      }
      UINT32 padding = 0;
      if (FAILED(client_->GetCurrentPadding(&padding))) {
        continue;
      }
      UINT32 available = bufferFrameCount_ > padding ? bufferFrameCount_ - padding : 0;
      while (available > 0) {
        const UINT32 chunk = std::min<UINT32>(available, 2048);
        BYTE* buffer = nullptr;
        if (FAILED(renderClient_->GetBuffer(chunk, &buffer)) || buffer == nullptr) {
          break;
        }
        pullScratch_.resize(static_cast<size_t>(chunk) * 2);
        const size_t real = ring_.pop(pullScratch_.data(), chunk);
        if (real < chunk) {
          ringDryFrames_ += static_cast<int64_t>(chunk - real);
          const auto events = ringDryEvents_.fetch_add(1, std::memory_order_relaxed) + 1;
          if (events == 1 || events % 200 == 0) {
            std::fprintf(stderr, "[monitor] ring dry #%lld (%u frames silence; device '%s')\n",
                         static_cast<long long>(events), chunk - static_cast<UINT32>(real), deviceName_.c_str());
          }
        }
        writeDeviceBlock(buffer, chunk, pullScratch_.data());
        renderClient_->ReleaseBuffer(chunk, 0);
        available -= chunk;
      }

      // Clock-drift trim (spec §3): ring depth is a phase-stable signal
      // (unlike endpoint padding). Nudge the engine's resampler a few ppm
      // every ~5s so the standing depth holds at ~3 worker ticks forever.
      depthAccum += static_cast<double>(ring_.depthFrames());
      ++depthSamples;
      if (clockAdjust_ != nullptr && depthSamples >= 500) {
        const double averageDepth = depthAccum / depthSamples;
        depthAccum = 0.0;
        depthSamples = 0;
        const double target = deviceSampleRate_ * 0.060;  // 60ms standing depth
        const double correction =
            std::clamp((averageDepth - target) / (5.0 * deviceSampleRate_), -0.0005, 0.0005);
        // Ring too deep → engine consumes too slowly → RAISE its idea of our
        // rate so it drains faster (and vice versa).
        const double newRate = static_cast<double>(deviceSampleRate_) * (1.0 + correction);
        (void)clockAdjust_->SetSampleRate(static_cast<float>(newRate));
      }
    }
  }

  // Convert ring floats (device-rate stereo, volume pre-applied) to the
  // device's sample format.
  void writeDeviceBlock(BYTE* buffer, UINT32 frames, const float* stereo) const {
    for (UINT32 frame = 0; frame < frames; ++frame) {
      const float left = stereo[static_cast<size_t>(frame) * 2];
      const float right = stereo[static_cast<size_t>(frame) * 2 + 1];
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

  // Z4/O4: enumerate ACTIVE render sessions other processes hold on the
  // monitor endpoint and warn with the offender's name — the operator hears
  // their output superimposed on ours, and no PCM tap can show it.
  void warnAboutForeignRenderSessions(IMMDevice* device) {
    if (device == nullptr) {
      return;
    }
    IAudioSessionManager2* manager = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&manager))) ||
        manager == nullptr) {
      return;  // best-effort guard; never fail the monitor over it
    }
    IAudioSessionEnumerator* sessions = nullptr;
    if (FAILED(manager->GetSessionEnumerator(&sessions)) || sessions == nullptr) {
      safeRelease(manager);
      return;
    }
    int count = 0;
    (void)sessions->GetCount(&count);
    const DWORD ourPid = ::GetCurrentProcessId();
    std::string offenders;
    for (int index = 0; index < count; ++index) {
      IAudioSessionControl* control = nullptr;
      if (FAILED(sessions->GetSession(index, &control)) || control == nullptr) {
        continue;
      }
      AudioSessionState sessionState = AudioSessionStateInactive;
      IAudioSessionControl2* control2 = nullptr;
      if (SUCCEEDED(control->GetState(&sessionState)) && sessionState == AudioSessionStateActive &&
          SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                            reinterpret_cast<void**>(&control2))) &&
          control2 != nullptr) {
        DWORD pid = 0;
        if (SUCCEEDED(control2->GetProcessId(&pid)) && pid != 0 && pid != ourPid) {
          std::string name = "pid " + std::to_string(pid);
          if (HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            char path[MAX_PATH];
            DWORD size = MAX_PATH;
            if (::QueryFullProcessImageNameA(process, 0, path, &size)) {
              const std::string full(path, size);
              const auto slash = full.find_last_of("\\/");
              name = slash == std::string::npos ? full : full.substr(slash + 1);
            }
            ::CloseHandle(process);
          }
          if (offenders.find(name) == std::string::npos) {
            offenders += offenders.empty() ? name : (", " + name);
          }
        }
      }
      safeRelease(control2);
      safeRelease(control);
    }
    safeRelease(sessions);
    safeRelease(manager);
    if (!offenders.empty()) {
      warn("Another app is playing audio on the monitor device (" + offenders +
           ") — you may hear it doubled over the mix. Mute it or monitor on a different device.");
    }
  }

  void cleanup() {
    safeRelease(clockAdjust_);
    safeRelease(renderClient_);
    if (renderEvent_ != nullptr) {
      ::CloseHandle(renderEvent_);
      renderEvent_ = nullptr;
    }
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
  // Pull-model state (spec §3): SPSC ring between the audio worker (producer)
  // and the device-paced render thread (consumer).
  SpscRing ring_{16384};  // ~340ms capacity @48k; standing target 60ms
  HANDLE renderEvent_ = nullptr;
  std::thread renderThread_;
  std::atomic<bool> renderRun_{false};
  std::vector<float> pushScratch_;
  std::vector<float> pullScratch_;
  std::atomic<int64_t> ringDryEvents_{0};
  int64_t ringDryFrames_ = 0;
  int64_t ringDropFrames_ = 0;
  IAudioClockAdjustment* clockAdjust_ = nullptr;
  std::string resolvedEndpointId_;
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
