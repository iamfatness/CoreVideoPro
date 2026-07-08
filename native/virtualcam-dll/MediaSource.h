#pragma once

// The virtual-camera media source (docs/virtual-camera-spec.md V2b). A software
// IMFMediaSource with one NV12 stream, CoCreated by the Windows Frame Server
// when the core calls MFCreateVirtualCamera with our CLSID. Implements the
// interfaces the Frame Server expects of a software camera source: IMFMediaSourceEx,
// IMFGetService, IKsControl (control stubs). Modeled on smourier/VCamSample.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <ks.h>
#include <ksproxy.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <mutex>

#include "MediaStream.h"

namespace corevideo::virtualcam {

class MediaSource
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IMFMediaSourceEx, IMFGetService, IKsControl> {
 public:
  MediaSource() = default;

  HRESULT RuntimeClassInitialize();

  // IMFMediaEventGenerator
  IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
  IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
  IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
  IFACEMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                            const PROPVARIANT* value) override;

  // IMFMediaSource
  IFACEMETHODIMP GetCharacteristics(DWORD* characteristics) override;
  IFACEMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
  IFACEMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                       const PROPVARIANT* startPosition) override;
  IFACEMETHODIMP Stop() override;
  IFACEMETHODIMP Pause() override;
  IFACEMETHODIMP Shutdown() override;

  // IMFMediaSourceEx
  IFACEMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
  IFACEMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
  IFACEMETHODIMP SetD3DManager(IUnknown* manager) override;

  // IMFGetService
  IFACEMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;

  // IKsControl (control-plane stubs; a program-output camera exposes no KS props)
  IFACEMETHODIMP KsProperty(PKSPROPERTY property, ULONG length, LPVOID data, ULONG dataLength,
                            ULONG* bytesReturned) override;
  IFACEMETHODIMP KsMethod(PKSMETHOD method, ULONG length, LPVOID data, ULONG dataLength,
                          ULONG* bytesReturned) override;
  IFACEMETHODIMP KsEvent(PKSEVENT event, ULONG length, LPVOID data, ULONG dataLength,
                         ULONG* bytesReturned) override;

 private:
  HRESULT CheckShutdown() const { return shutdown_ ? MF_E_SHUTDOWN : S_OK; }
  HRESULT BuildMediaType(IMFMediaType** type);

  mutable std::mutex mutex_;
  Microsoft::WRL::ComPtr<IMFMediaEventQueue> events_;
  Microsoft::WRL::ComPtr<IMFPresentationDescriptor> presentation_;
  Microsoft::WRL::ComPtr<MediaStream> stream_;
  Microsoft::WRL::ComPtr<IMFAttributes> sourceAttributes_;
  Microsoft::WRL::ComPtr<IMFAttributes> streamAttributes_;
  UINT32 width_ = 1280;
  UINT32 height_ = 720;
  UINT32 fps_ = 30;
  bool shutdown_ = false;
  bool started_ = false;
};

}  // namespace corevideo::virtualcam
