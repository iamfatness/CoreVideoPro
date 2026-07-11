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
    lastGood_.assign(scratch_.begin(), scratch_.end());  // cache for miss recovery
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
  if (events_) {
    events_->Shutdown();
    events_.Reset();
  }
  source_.Reset();
  descriptor_.Reset();
}

}  // namespace corevideo::virtualcam
