#include "MediaSource.h"

#include <mfobjects.h>
#include <mfreadwrite.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace corevideo::virtualcam {

HRESULT MediaSource::BuildMediaType(IMFMediaType** outType) {
  ComPtr<IMFMediaType> type;
  HRESULT hr = MFCreateMediaType(&type);
  if (FAILED(hr)) return hr;
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width_, height_);
  MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fps_, 1);
  MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  type->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width_));
  type->SetUINT32(MF_MT_SAMPLE_SIZE, width_ * height_ * 3 / 2);
  *outType = type.Detach();
  return S_OK;
}

HRESULT MediaSource::RuntimeClassInitialize() {
  HRESULT hr = MFCreateEventQueue(&events_);
  if (FAILED(hr)) return hr;

  ComPtr<IMFMediaType> type;
  hr = BuildMediaType(&type);
  if (FAILED(hr)) return hr;

  ComPtr<IMFStreamDescriptor> descriptor;
  IMFMediaType* types[] = {type.Get()};
  hr = MFCreateStreamDescriptor(0, 1, types, &descriptor);
  if (FAILED(hr)) return hr;

  ComPtr<IMFMediaTypeHandler> handler;
  hr = descriptor->GetMediaTypeHandler(&handler);
  if (FAILED(hr)) return hr;
  handler->SetCurrentMediaType(type.Get());

  IMFStreamDescriptor* descriptors[] = {descriptor.Get()};
  hr = MFCreatePresentationDescriptor(1, descriptors, &presentation_);
  if (FAILED(hr)) return hr;
  presentation_->SelectStream(0);

  stream_ = Make<MediaStream>();
  if (stream_ == nullptr) return E_OUTOFMEMORY;
  hr = stream_->RuntimeClassInitialize(this, descriptor.Get(), width_, height_, fps_);
  if (FAILED(hr)) return hr;

  // Source/stream attribute stores (kept minimal; the Frame Server queries these
  // and a basic software colour source needs no special declarations here).
  hr = MFCreateAttributes(&sourceAttributes_, 1);
  if (FAILED(hr)) return hr;
  hr = MFCreateAttributes(&streamAttributes_, 1);
  if (FAILED(hr)) return hr;
  return S_OK;
}

// ---- IMFMediaEventGenerator ------------------------------------------------
IFACEMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
  std::lock_guard<std::mutex> lock(mutex_);
  HRESULT hr = CheckShutdown();
  return FAILED(hr) ? hr : events_->BeginGetEvent(callback, state);
}

IFACEMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
  std::lock_guard<std::mutex> lock(mutex_);
  HRESULT hr = CheckShutdown();
  return FAILED(hr) ? hr : events_->EndGetEvent(result, event);
}

IFACEMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    queue = events_;
  }
  return queue->GetEvent(flags, event);
}

IFACEMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                       const PROPVARIANT* value) {
  std::lock_guard<std::mutex> lock(mutex_);
  HRESULT hr = CheckShutdown();
  return FAILED(hr) ? hr : events_->QueueEventParamVar(type, extendedType, status, value);
}

// ---- IMFMediaSource --------------------------------------------------------
IFACEMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
  if (characteristics == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> lock(mutex_);
  HRESULT hr = CheckShutdown();
  if (FAILED(hr)) return hr;
  *characteristics = MFMEDIASOURCE_IS_LIVE;
  return S_OK;
}

IFACEMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
  if (descriptor == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> lock(mutex_);
  HRESULT hr = CheckShutdown();
  if (FAILED(hr)) return hr;
  return presentation_->Clone(descriptor);
}

IFACEMETHODIMP MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* /*timeFormat*/,
                                  const PROPVARIANT* startPosition) {
  std::lock_guard<std::mutex> lock(mutex_);
  HRESULT hr = CheckShutdown();
  if (FAILED(hr)) return hr;
  if (descriptor == nullptr) return E_INVALIDARG;

  // Announce the stream (new on first start, updated on restart).
  ComPtr<IUnknown> streamUnk;
  stream_.As(&streamUnk);
  PROPVARIANT var;
  PropVariantInit(&var);
  var.vt = VT_UNKNOWN;
  var.punkVal = streamUnk.Get();
  events_->QueueEventParamUnk(started_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                              streamUnk.Get());
  PropVariantClear(&var);

  hr = stream_->Start();
  if (FAILED(hr)) return hr;

  started_ = true;
  return events_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, startPosition);
}

IFACEMETHODIMP MediaSource::Stop() {
  std::lock_guard<std::mutex> lock(mutex_);
  HRESULT hr = CheckShutdown();
  if (FAILED(hr)) return hr;
  if (stream_) stream_->Stop();
  started_ = false;
  return events_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

IFACEMETHODIMP MediaSource::Pause() {
  std::lock_guard<std::mutex> lock(mutex_);
  HRESULT hr = CheckShutdown();
  if (FAILED(hr)) return hr;
  return MF_E_INVALID_STATE_TRANSITION;  // live source: no pause
}

IFACEMETHODIMP MediaSource::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  shutdown_ = true;
  if (stream_) {
    stream_->Shutdown();
    stream_.Reset();
  }
  if (events_) {
    events_->Shutdown();
    events_.Reset();
  }
  presentation_.Reset();
  sourceAttributes_.Reset();
  streamAttributes_.Reset();
  return S_OK;
}

// ---- IMFMediaSourceEx ------------------------------------------------------
IFACEMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
  if (attributes == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> lock(mutex_);
  HRESULT hr = CheckShutdown();
  if (FAILED(hr)) return hr;
  return sourceAttributes_.CopyTo(attributes);
}

IFACEMETHODIMP MediaSource::GetStreamAttributes(DWORD /*streamId*/, IMFAttributes** attributes) {
  if (attributes == nullptr) return E_POINTER;
  std::lock_guard<std::mutex> lock(mutex_);
  HRESULT hr = CheckShutdown();
  if (FAILED(hr)) return hr;
  return streamAttributes_.CopyTo(attributes);
}

IFACEMETHODIMP MediaSource::SetD3DManager(IUnknown* /*manager*/) {
  return S_OK;  // software source: ignore the D3D manager
}

// ---- IMFGetService ---------------------------------------------------------
IFACEMETHODIMP MediaSource::GetService(REFGUID /*service*/, REFIID riid, LPVOID* object) {
  if (object == nullptr) return E_POINTER;
  return QueryInterface(riid, object);
}

// ---- IKsControl (stubs) ----------------------------------------------------
IFACEMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* bytesReturned) {
  if (bytesReturned) *bytesReturned = 0;
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* bytesReturned) {
  if (bytesReturned) *bytesReturned = 0;
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* bytesReturned) {
  if (bytesReturned) *bytesReturned = 0;
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

}  // namespace corevideo::virtualcam
