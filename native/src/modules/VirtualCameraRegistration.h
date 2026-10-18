#pragma once

#include <string>
#include "core/BoundedAsyncLog.h"

#ifdef _WIN32
#include <windows.h>
#include <vector>

namespace corevideo::modules {

struct VirtualCameraRegistrationResult {
  bool ready = false;
  bool repaired = false;
  std::string warning;
  bool predefinedUserMismatch = false;
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
inline VirtualCameraRegistrationResult ensureVirtualCameraRegistrationForRoots(
    HKEY machineRoot, HKEY userRoot) {
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
  const auto machine = cameraRegistrationString(machineRoot, kServer);
  if (machine.empty()) {
    return {false, false, "CAMERA_MACHINE_REGISTRATION_MISSING: Run Register-VirtualCamera.cmd with administrator approval."};
  }
  const auto role = cameraRegistrationString(machineRoot, kServer, L"CoreVideoOwnerRole");
  const auto owner = cameraRegistrationString(machineRoot, kServer, L"CoreVideoOwnerAppDirectory");
  const auto hash = cameraRegistrationString(machineRoot, kServer, L"CoreVideoSha256");
  const auto common = cameraRegistrationString(machineRoot,
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
  const auto readUser = [&](const wchar_t* name, LONG& status, DWORD& type) {
    std::vector<wchar_t> data(32768); DWORD bytes = static_cast<DWORD>(data.size()*sizeof(wchar_t));
    status = ::RegGetValueW(userRoot,kServer,name,RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,&type,data.data(),&bytes);
    return status==ERROR_SUCCESS ? std::wstring(data.data()) : std::wstring{};
  };
  LONG userStatus=0, roleStatus=0, ownerStatus=0; DWORD userType=0,roleType=0,ownerType=0;
  const auto user = readUser(nullptr,userStatus,userType);
  if (!user.empty()) {
    const auto userRole = readUser(L"CoreVideoOwnerRole",roleStatus,roleType);
    const auto userOwner = readUser(L"CoreVideoOwnerAppDirectory",ownerStatus,ownerType);
    if (userRole != kRole || _wcsicmp(userOwner.c_str(), app.c_str()) != 0) {
      ::corevideo::core::nativeLogf("[camera-registration-read] pid=%lu thread=%lu app=[%ls] expected_role=[%ls] user=[%ls] user_status=%ld user_type=%lu role=[%ls] role_status=%ld role_type=%lu owner=[%ls] owner_status=%ld owner_type=%lu\n",
          ::GetCurrentProcessId(),::GetCurrentThreadId(),app.c_str(),kRole,user.c_str(),userStatus,userType,userRole.c_str(),roleStatus,roleType,userOwner.c_str(),ownerStatus,ownerType);
      return {false, false, "CAMERA_USER_REGISTRATION_CONFLICT: A per-user camera key has different ownership. Run Register-VirtualCamera.cmd to inspect/migrate it."};
    }
    if (_wcsicmp(user.c_str(), machine.c_str()) == 0) return {true, false, {}};
    if (::GetFileAttributesW(user.c_str()) != INVALID_FILE_ATTRIBUTES) {
      return {false, false, "CAMERA_USER_REGISTRATION_CONFLICT: The user alias points at a different existing DLL."};
    }
  }
  HKEY key = nullptr;
  LONG result = ::RegCreateKeyExW(userRoot, kServer, 0, nullptr,
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

inline VirtualCameraRegistrationResult ensureVirtualCameraRegistration() {
  // HKCU is a process-cached predefined mapping, potentially established by
  // another Windows/COM caller. Re-open the effective user's hive for each
  // attempt, and use that same owned handle for both inspection and repair.
  // Do not invalidate the predefined cache globally or relax ownership checks.
  HKEY user = nullptr;
  const auto error = ::RegOpenCurrentUser(KEY_READ | KEY_WRITE, &user);
  if (error != ERROR_SUCCESS) {
    return {false, false, "CAMERA_USER_HIVE_UNAVAILABLE: Cannot open the user's camera registry (error " +
        std::to_string(error) + ")."};
  }
  struct Close { HKEY key; ~Close() { ::RegCloseKey(key); } } close{user};
  auto result = ensureVirtualCameraRegistrationForRoots(HKEY_LOCAL_MACHINE, user);
  constexpr auto server = L"Software\\Classes\\CLSID\\{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}\\InprocServer32";
  for (const auto value : {static_cast<const wchar_t*>(nullptr),
                          L"CoreVideoOwnerRole", L"CoreVideoOwnerAppDirectory"}) {
    if (cameraRegistrationString(HKEY_CURRENT_USER, server, value) !=
        cameraRegistrationString(user, server, value)) result.predefinedUserMismatch = true;
  }
  return result;
}

}  // namespace corevideo::modules
#endif
