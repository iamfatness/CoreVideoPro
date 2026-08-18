#include "MediaStream.h"

#include <mfobjects.h>

#include "MediaSource.h"
#include "VcamLog.h"
#include "modules/VirtualCameraSlate.h"

using Microsoft::WRL::ComPtr;

namespace corevideo::virtualcam {

HRESULT MediaStream::RuntimeClassInitialize(MediaSource* source, IMFStreamDescriptor* descriptor,
                                            UINT32 width, UINT32 height, UINT32 fps) {
  if (source == nullptr || descriptor == nullptr) {
    return E_INVALIDARG;
  }
  HRESULT hr = source->QueryInterface(IID_PPV_ARGS(&source_));
  if (FAILED(hr)) {
    return hr;
  }
  descriptor_ = descriptor;
  width_ = width;
  height_ = height;
  fps_ = fps == 0 ? 30 : fps;
  frameDuration_ = 10000000LL / static_cast<LONGLONG>(fps_);
  return MFCreateEventQueue(&events_);
}

// ---- IMFMediaEventGenerator (forward to the queue) -------------------------
IFACEMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  return events_->BeginGetEvent(callback, state);
}

IFACEMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  return events_->EndGetEvent(result, event);
}

IFACEMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    queue = events_;
  }
  return queue->GetEvent(flags, event);
}

IFACEMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                       const PROPVARIANT* value) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  return events_->QueueEventParamVar(type, extendedType, status, value);
}

// ---- IMFMediaStream --------------------------------------------------------
IFACEMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
  if (source == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  return source_.CopyTo(source);
}

IFACEMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
  if (descriptor == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  return descriptor_.CopyTo(descriptor);
}

IFACEMETHODIMP MediaStream::RequestSample(IUnknown* token) {
  // PACE DELIVERY TO THE FRAME RATE. The pipeline requests the next sample the
  // moment the previous one completes, so completing immediately free-runs the
  // whole serve chain at CPU speed - measured ~2000 samples/s (vs 60 declared),
  // i.e. ~6GB/s of 3MB copies through the Frame Server and every consumer. That
  // load is what starved system audio and strobed the picture whenever an app
  // consumed the camera. A camera source controls delivery cadence exactly like
  // hardware delivering at sensor rate: wait until the next frame is DUE.
  LONGLONG waitHns = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (!running_) return MF_E_INVALIDREQUEST;
    const LONGLONG now = MFGetSystemTime();
    if (nextDueHns_ == 0 || now >= nextDueHns_ + frameDuration_) {
      nextDueHns_ = now;  // first sample, or we fell behind: re-anchor, no catch-up burst
    }
    waitHns = nextDueHns_ - now;
    nextDueHns_ += frameDuration_;
    // Lazy high-resolution timer: the process timer resolution is often 15.6ms,
    // which would quantize a plain Sleep and cap 60fps pacing at ~40fps.
    if (waitHns > 0 && pacerTimer_ == nullptr) {
      pacerTimer_ = ::CreateWaitableTimerExW(nullptr, nullptr,
                                             CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_ALL_ACCESS);
    }
  }
  if (waitHns > 0) {
    bool waited = false;
    if (pacerTimer_ != nullptr) {
      LARGE_INTEGER due;
      due.QuadPart = -waitHns;  // relative
      if (::SetWaitableTimer(pacerTimer_, &due, 0, nullptr, nullptr, FALSE)) {
        ::WaitForSingleObject(pacerTimer_, static_cast<DWORD>(waitHns / 10000 + 20));
        waited = true;
      }
    }
    if (!waited) {
      ::Sleep(static_cast<DWORD>(waitHns / 10000));
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  if (!running_) return MF_E_INVALIDREQUEST;

  ComPtr<IMFSample> sample;
  HRESULT hr = CreateSample(token, &sample);
  if (FAILED(hr)) {
    return hr;
  }
  return events_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

HRESULT MediaStream::CreateSample(IUnknown* token, IMFSample** outSample) {
  const DWORD nv12Len = width_ * height_ * 3 / 2;

  ComPtr<IMFSample> sample;
  HRESULT hr = MFCreateSample(&sample);
  if (FAILED(hr)) return hr;

  ComPtr<IMFMediaBuffer> buffer;
  hr = MFCreateMemoryBuffer(nv12Len, &buffer);
  if (FAILED(hr)) return hr;

  BYTE* dst = nullptr;
  DWORD maxLen = 0;
  hr = buffer->Lock(&dst, &maxLen, nullptr);
  if (FAILED(hr)) return hr;
  hr = FillFromSharedMemoryOrSlate(dst, nv12Len);
  buffer->Unlock();
  if (FAILED(hr)) return hr;

  hr = buffer->SetCurrentLength(nv12Len);
  if (FAILED(hr)) return hr;
  hr = sample->AddBuffer(buffer.Get());
  if (FAILED(hr)) return hr;

  // Live source: stamp each sample with the ACTUAL current system time (100ns),
  // not an accumulating counter. A counter that advances by frameDuration_ per
  // sample drifts from the wall clock whenever the serve rate != the declared
  // fps, and the consumer buffers to reconcile the mismatch -> growing latency.
  // The real capture time keeps the pipeline's latency minimal.
  sample->SetSampleTime(MFGetSystemTime());
  sample->SetSampleDuration(frameDuration_);
  if (token != nullptr) {
    sample->SetUnknown(MFSampleExtension_Token, token);
  }
  *outSample = sample.Detach();
  return S_OK;
}

HRESULT MediaStream::FillFromSharedMemoryOrSlate(BYTE* dst, DWORD dstLen) {
  int w = 0;
  int h = 0;
  const bool got = reader_.readLatest(scratch_, w, h);
  static int fillCount = 0;
  if ((fillCount++ % 60) == 0) {
    char b[128];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "Fill #%d: readLatest=%d dims=%dx%d want=%ux%u len=%zu/%lu",
                fillCount, got ? 1 : 0, w, h, width_, height_, scratch_.size(),
                static_cast<unsigned long>(dstLen));
    VcamServeLog(b);
  }
  if (got && static_cast<UINT32>(w) == width_ && static_cast<UINT32>(h) == height_ &&
      scratch_.size() == dstLen) {
    memcpy(dst, scratch_.data(), dstLen);
    // Keep the current frame for miss recovery without a third 3 MB memcpy.
    // scratch_ receives the previous reusable allocation for the next read.
    lastGood_.swap(scratch_);
    missStreak_ = 0;
    return S_OK;
  }
  // Transient miss (seqlock collision, or a tick the core didn't publish): hold
  // the last good frame instead of flashing the slate. A single dropped read is
  // then invisible - the camera just repeats the previous frame. Only after a
  // sustained absence (~0.5s at 60fps) does the core count as gone -> slate.
  constexpr int kHoldFramesBeforeSlate = 30;
  ++missStreak_;
  if (!lastGood_.empty() && lastGood_.size() == dstLen && missStreak_ <= kHoldFramesBeforeSlate) {
    memcpy(dst, lastGood_.data(), dstLen);
    return S_OK;
  }
  // Core absent (or never started) -> standby slate (law 2: never a black frame).
  std::vector<std::uint8_t> slate;
  corevideo::modules::fillVirtualCameraStandbySlate(static_cast<int>(width_),
                                                    static_cast<int>(height_), slate);
  if (slate.size() == dstLen) {
    memcpy(dst, slate.data(), dstLen);
  } else {
    memset(dst, 16, dstLen);  // safe dark fallback
  }
  return S_OK;
}

HRESULT MediaStream::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  running_ = true;
  nextPts_ = 0;
  nextDueHns_ = 0;  // re-anchor delivery pacing per run
  VcamServeLog("MediaStream::Start - serving begins");
  return events_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}

HRESULT MediaStream::Stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return MF_E_SHUTDOWN;
  running_ = false;
  reader_.close();
  return events_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void MediaStream::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  shutdown_ = true;
  running_ = false;
  reader_.close();
  if (pacerTimer_ != nullptr) {
    ::CloseHandle(pacerTimer_);
    pacerTimer_ = nullptr;
  }
  if (events_) {
    events_->Shutdown();
    events_.Reset();
  }
  source_.Reset();
  descriptor_.Reset();
}

}  // namespace corevideo::virtualcam
