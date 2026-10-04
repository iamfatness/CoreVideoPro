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

inline std::wstring cameraRegistrationString(HKEY root, const wchar_t* key,
                                             const wchar_t* value = nullptr) {
  std::vector<wchar_t> buffer(32768);
  DWORD bytes = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
  if (::RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                    nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) return {};
  return buffer.data();
}

// Startup may repair the user's alias to an installed machine runtime. It must
// never load/register a development DLL or replace an unrelated user key.
inline VirtualCameraRegistrationResult ensureVirtualCameraRegistration() {
  constexpr wchar_t kServer[] =
      L"Software\\Classes\\CLSID\\{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}\\InprocServer32";
  constexpr wchar_t kRole[] = L"CoreVideoPro.VirtualCamera.v1";
  std::vector<wchar_t> modulePath(32768);
  const DWORD length = ::GetModuleFileNameW(nullptr, modulePath.data(),
                                           static_cast<DWORD>(modulePath.size()));
  if (length == 0 || length >= modulePath.size()) {
    return {false, false, "CAMERA_APP_PATH_UNAVAILABLE: Could not locate this installation."};
  }
  std::wstring app(modulePath.data(), length);
  const auto separator = app.find_last_of(L"\\/");
  if (separator == std::wstring::npos) return {false, false, "CAMERA_APP_PATH_UNAVAILABLE"};
  app.resize(separator);
  const auto machine = cameraRegistrationString(HKEY_LOCAL_MACHINE, kServer);
  if (machine.empty()) {
    return {false, false, "CAMERA_MACHINE_REGISTRATION_MISSING: Run Register-VirtualCamera.cmd with administrator approval."};
  }
  const auto role = cameraRegistrationString(HKEY_LOCAL_MACHINE, kServer, L"CoreVideoOwnerRole");
  const auto owner = cameraRegistrationString(HKEY_LOCAL_MACHINE, kServer, L"CoreVideoOwnerAppDirectory");
  const auto hash = cameraRegistrationString(HKEY_LOCAL_MACHINE, kServer, L"CoreVideoSha256");
  const auto common = cameraRegistrationString(HKEY_LOCAL_MACHINE,
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion", L"CommonFilesDir");
  const auto expected = common + L"\\CoreVideoProCamera\\" + hash + L"\\corevideo-virtualcam.dll";
  if (role != kRole || _wcsicmp(owner.c_str(), app.c_str()) != 0 ||
      hash.size() != 64 || hash.find_first_not_of(L"0123456789abcdef") != std::wstring::npos ||
      common.empty() || _wcsicmp(machine.c_str(), expected.c_str()) != 0) {
    return {false, false, "CAMERA_MACHINE_REGISTRATION_CONFLICT: Camera ownership/path differs from this installation. Run Register-VirtualCamera.cmd; unknown registrations require inspection."};
  }
  const auto attributes = ::GetFileAttributesW(machine.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return {false, false, "CAMERA_RUNTIME_MISSING: Reinstall the virtual camera from this installation."};
  }
  const auto user = cameraRegistrationString(HKEY_CURRENT_USER, kServer);
  if (!user.empty()) {
    const auto userRole = cameraRegistrationString(HKEY_CURRENT_USER, kServer, L"CoreVideoOwnerRole");
    const auto userOwner = cameraRegistrationString(HKEY_CURRENT_USER, kServer, L"CoreVideoOwnerAppDirectory");
    if (userRole != kRole || _wcsicmp(userOwner.c_str(), app.c_str()) != 0) {
      return {false, false, "CAMERA_USER_REGISTRATION_CONFLICT: A per-user camera key has different ownership. Run Register-VirtualCamera.cmd to inspect/migrate it."};
    }
    if (_wcsicmp(user.c_str(), machine.c_str()) == 0) return {true, false, {}};
    if (::GetFileAttributesW(user.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return {false, false, "CAMERA_USER_REGISTRATION_CONFLICT: The user alias points at a different existing DLL."};
    }
  }
  HKEY key = nullptr;
  LONG result = ::RegCreateKeyExW(HKEY_CURRENT_USER, kServer, 0, nullptr,
      REG_OPTION_NON_VOLATILE, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
  if (result == ERROR_SUCCESS) {
    const auto put = [&](const wchar_t* name, const std::wstring& value) {
      if (result == ERROR_SUCCESS) result = ::RegSetValueExW(key, name, 0, REG_SZ,
          reinterpret_cast<const BYTE*>(value.c_str()),
          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    };
    put(nullptr, machine); put(L"ThreadingModel", L"Both");
    put(L"CoreVideoOwnerRole", kRole); put(L"CoreVideoOwnerAppDirectory", owner);
    put(L"CoreVideoSha256", hash);
    ::RegCloseKey(key);
  }
  if (result != ERROR_SUCCESS) {
    return {false, false, "CAMERA_USER_REGISTRATION_REPAIR_FAILED: Run Register-VirtualCamera.cmd from this installation."};
  }
  return {true, true, {}};
}

}  // namespace corevideo::modules
#endif
