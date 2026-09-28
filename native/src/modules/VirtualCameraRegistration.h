#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
#include <vector>

namespace corevideo::modules {

struct VirtualCameraRegistrationResult {
  bool ready = false;
  bool repaired = false;
  std::string warning;
};

// An old uninstaller can still remove our HKCU COM key after a new beta is
// installed. Repair only a missing key or one pointing to a DLL that no longer
// exists. A valid registration for another installed beta is left alone.
inline VirtualCameraRegistrationResult ensureVirtualCameraRegistration() {
  constexpr wchar_t kServer[] =
      L"Software\\Classes\\CLSID\\{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}\\InprocServer32";
  std::vector<wchar_t> modulePath(32768);
  const DWORD length = ::GetModuleFileNameW(nullptr, modulePath.data(),
                                             static_cast<DWORD>(modulePath.size()));
  if (length == 0 || length >= modulePath.size()) {
    return {false, false, "Could not locate this installation's virtual-camera DLL."};
  }
  std::wstring ownPath(modulePath.data(), length);
  const auto separator = ownPath.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    return {false, false, "Could not locate this installation's virtual-camera DLL."};
  }
  ownPath.resize(separator + 1);
  ownPath += L"corevideo-virtualcam.dll";
  if (::GetFileAttributesW(ownPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return {false, false, "This installation is missing corevideo-virtualcam.dll. Reinstall CoreVideo Pro."};
  }

  std::vector<wchar_t> registered(32768);
  DWORD bytes = static_cast<DWORD>(registered.size() * sizeof(wchar_t));
  if (::RegGetValueW(HKEY_CURRENT_USER, kServer, nullptr, RRF_RT_REG_SZ,
                     nullptr, registered.data(), &bytes) == ERROR_SUCCESS &&
      registered[0] != L'\0' &&
      ::GetFileAttributesW(registered.data()) != INVALID_FILE_ATTRIBUTES) {
    return {true, false, {}};
  }

  HMODULE dll = ::LoadLibraryW(ownPath.c_str());
  if (!dll) {
    return {false, false, "Windows could not load this installation's virtual-camera DLL. Reinstall CoreVideo Pro."};
  }
  using Register = HRESULT(STDAPICALLTYPE*)();
  const auto registerDll = reinterpret_cast<Register>(::GetProcAddress(dll, "DllRegisterServer"));
  const HRESULT result = registerDll ? registerDll() : E_NOINTERFACE;
  ::FreeLibrary(dll);
  if (FAILED(result)) {
    return {false, false, "Virtual-camera registration repair failed. Run Register-VirtualCamera.cmd from this installation."};
  }
  return {true, true, {}};
}

}  // namespace corevideo::modules
#endif
