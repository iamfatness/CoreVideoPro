// COM entry points + self-registration for corevideo-virtualcam.dll
// (docs/virtual-camera-spec.md V2b). A user-mode in-proc COM server exposing the
// virtual-camera media source under CLSID_CoreVideoVirtualCameraSource. NO kernel
// driver, NO signing: DllRegisterServer writes a per-machine InprocServer32 key
// (the installer runs regsvr32; a per-user variant can target HKCU). Modeled on
// smourier/VCamSample.

#include <windows.h>
#include <mfapi.h>
#include <new>
#include <cstdio>
#include <string>

#include <wrl/client.h>
#include <wrl/implements.h>

#include "Guids.h"
#include "MediaSource.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace {

HMODULE g_module = nullptr;
LONG g_lockCount = 0;

class ClassFactory
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IClassFactory> {
 public:
  IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
    if (object == nullptr) return E_POINTER;
    *object = nullptr;
    if (outer != nullptr) return CLASS_E_NOAGGREGATION;

    auto source = Make<corevideo::virtualcam::MediaSource>();
    if (source == nullptr) return E_OUTOFMEMORY;
    HRESULT hr = source->RuntimeClassInitialize();
    if (FAILED(hr)) return hr;
    return source.CopyTo(riid, object);
  }

  IFACEMETHODIMP LockServer(BOOL lock) override {
    if (lock) {
      InterlockedIncrement(&g_lockCount);
    } else {
      InterlockedDecrement(&g_lockCount);
    }
    return S_OK;
  }
};

std::wstring ModulePath() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(g_module, path, MAX_PATH);
  return path;
}

LONG SetKeyValue(HKEY root, const wchar_t* subkey, const wchar_t* name, const wchar_t* value) {
  HKEY key = nullptr;
  LONG rc = RegCreateKeyExW(root, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                            &key, nullptr);
  if (rc != ERROR_SUCCESS) return rc;
  rc = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value),
                      static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
  RegCloseKey(key);
  return rc;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = instance;
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** object) {
  if (object == nullptr) return E_POINTER;
  *object = nullptr;
  if (clsid != CLSID_CoreVideoVirtualCameraSource) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  auto factory = Make<ClassFactory>();
  if (factory == nullptr) return E_OUTOFMEMORY;
  return factory.CopyTo(riid, object);
}

STDAPI DllCanUnloadNow() {
  return g_lockCount == 0 ? S_OK : S_FALSE;
}

// Registers PER-USER (HKCU\Software\Classes) so the installer needs NO admin /
// elevation - matches MFVirtualCameraAccess_CurrentUser. COM merges HKCU\Software
// \Classes into HKEY_CLASSES_ROOT for the current user, so CoCreate still finds it.
STDAPI DllRegisterServer() {
  wchar_t clsidText[64] = {};
  StringFromGUID2(CLSID_CoreVideoVirtualCameraSource, clsidText, 64);

  std::wstring inproc = L"Software\\Classes\\CLSID\\";
  inproc += clsidText;
  const std::wstring server = inproc + L"\\InprocServer32";
  const std::wstring path = ModulePath();

  LONG rc = SetKeyValue(HKEY_CURRENT_USER, inproc.c_str(), nullptr, L"CoreVideo Pro Camera Source");
  if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
  rc = SetKeyValue(HKEY_CURRENT_USER, server.c_str(), nullptr, path.c_str());
  if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
  rc = SetKeyValue(HKEY_CURRENT_USER, server.c_str(), L"ThreadingModel", L"Both");
  if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
  return S_OK;
}

STDAPI DllUnregisterServer() {
  wchar_t clsidText[64] = {};
  StringFromGUID2(CLSID_CoreVideoVirtualCameraSource, clsidText, 64);
  std::wstring inproc = L"Software\\Classes\\CLSID\\";
  inproc += clsidText;
  const std::wstring server = inproc + L"\\InprocServer32";
  RegDeleteKeyW(HKEY_CURRENT_USER, server.c_str());
  RegDeleteKeyW(HKEY_CURRENT_USER, inproc.c_str());
  return S_OK;
}
