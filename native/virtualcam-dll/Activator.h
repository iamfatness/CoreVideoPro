#pragma once

// The activation object the Frame Server CoCreates from our CLSID
// (docs/virtual-camera-spec.md V2b). MFCreateVirtualCamera / the Frame Server
// expect the registered CLSID to be an IMFActivate, NOT the media source
// itself: it CoCreates this, then calls ActivateObject(IID_IMFMediaSource) to
// build the real source. IMFActivate derives from IMFAttributes, so this backs
// itself with a real attribute store and forwards every IMFAttributes call.

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include "MediaSource.h"
#include "VcamLog.h"

namespace corevideo::virtualcam {

class Activator
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFActivate, IMFAttributes>> {
 public:
  HRESULT RuntimeClassInitialize() { return MFCreateAttributes(&attributes_, 1); }

  // IMFActivate
  IFACEMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
    VcamServeLog("Activator::ActivateObject (building the media source)");
    if (ppv == nullptr) return E_POINTER;
    auto source = Microsoft::WRL::Make<MediaSource>();
    if (source == nullptr) return E_OUTOFMEMORY;
    HRESULT hr = source->RuntimeClassInitialize();
    if (FAILED(hr)) { VcamServeLog("  MediaSource init FAILED"); return hr; }
    return source.CopyTo(riid, ppv);
  }
  IFACEMETHODIMP ShutdownObject() override { return S_OK; }
  IFACEMETHODIMP DetachObject() override { return S_OK; }

  // IMFAttributes - forward to the backing store.
  IFACEMETHODIMP GetItem(REFGUID k, PROPVARIANT* v) override { return attributes_->GetItem(k, v); }
  IFACEMETHODIMP GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) override { return attributes_->GetItemType(k, t); }
  IFACEMETHODIMP CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) override { return attributes_->CompareItem(k, v, r); }
  IFACEMETHODIMP Compare(IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) override { return attributes_->Compare(a, m, r); }
  IFACEMETHODIMP GetUINT32(REFGUID k, UINT32* v) override { return attributes_->GetUINT32(k, v); }
  IFACEMETHODIMP GetUINT64(REFGUID k, UINT64* v) override { return attributes_->GetUINT64(k, v); }
  IFACEMETHODIMP GetDouble(REFGUID k, double* v) override { return attributes_->GetDouble(k, v); }
  IFACEMETHODIMP GetGUID(REFGUID k, GUID* v) override { return attributes_->GetGUID(k, v); }
  IFACEMETHODIMP GetStringLength(REFGUID k, UINT32* l) override { return attributes_->GetStringLength(k, l); }
  IFACEMETHODIMP GetString(REFGUID k, LPWSTR v, UINT32 n, UINT32* l) override { return attributes_->GetString(k, v, n, l); }
  IFACEMETHODIMP GetAllocatedString(REFGUID k, LPWSTR* v, UINT32* l) override { return attributes_->GetAllocatedString(k, v, l); }
  IFACEMETHODIMP GetBlobSize(REFGUID k, UINT32* s) override { return attributes_->GetBlobSize(k, s); }
  IFACEMETHODIMP GetBlob(REFGUID k, UINT8* b, UINT32 n, UINT32* s) override { return attributes_->GetBlob(k, b, n, s); }
  IFACEMETHODIMP GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* s) override { return attributes_->GetAllocatedBlob(k, b, s); }
  IFACEMETHODIMP GetUnknown(REFGUID k, REFIID riid, LPVOID* v) override { return attributes_->GetUnknown(k, riid, v); }
  IFACEMETHODIMP SetItem(REFGUID k, REFPROPVARIANT v) override { return attributes_->SetItem(k, v); }
  IFACEMETHODIMP DeleteItem(REFGUID k) override { return attributes_->DeleteItem(k); }
  IFACEMETHODIMP DeleteAllItems() override { return attributes_->DeleteAllItems(); }
  IFACEMETHODIMP SetUINT32(REFGUID k, UINT32 v) override { return attributes_->SetUINT32(k, v); }
  IFACEMETHODIMP SetUINT64(REFGUID k, UINT64 v) override { return attributes_->SetUINT64(k, v); }
  IFACEMETHODIMP SetDouble(REFGUID k, double v) override { return attributes_->SetDouble(k, v); }
  IFACEMETHODIMP SetGUID(REFGUID k, REFGUID v) override { return attributes_->SetGUID(k, v); }
  IFACEMETHODIMP SetString(REFGUID k, LPCWSTR v) override { return attributes_->SetString(k, v); }
  IFACEMETHODIMP SetBlob(REFGUID k, const UINT8* b, UINT32 n) override { return attributes_->SetBlob(k, b, n); }
  IFACEMETHODIMP SetUnknown(REFGUID k, IUnknown* u) override { return attributes_->SetUnknown(k, u); }
  IFACEMETHODIMP LockStore() override { return attributes_->LockStore(); }
  IFACEMETHODIMP UnlockStore() override { return attributes_->UnlockStore(); }
  IFACEMETHODIMP GetCount(UINT32* c) override { return attributes_->GetCount(c); }
  IFACEMETHODIMP GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) override { return attributes_->GetItemByIndex(i, k, v); }
  IFACEMETHODIMP CopyAllItems(IMFAttributes* d) override { return attributes_->CopyAllItems(d); }

 private:
  Microsoft::WRL::ComPtr<IMFAttributes> attributes_;
};

}  // namespace corevideo::virtualcam
