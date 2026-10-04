#include <gtest/gtest.h>

#if defined(_WIN32) && COREVIDEO_WITH_VIRTUALCAM
#include <windows.h>

#include "modules/VirtualCameraPublisher.h"
#include "modules/VirtualCameraRegistration.h"
#include <filesystem>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

constexpr wchar_t kServer[] =
    L"Software\\Classes\\CLSID\\{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}\\InprocServer32";

LONG writeRegistration(HKEY root, const wchar_t* path) {
  HKEY server = nullptr;
  LONG result = ::RegCreateKeyExW(root, kServer, 0, nullptr, REG_OPTION_VOLATILE,
                                  KEY_ALL_ACCESS, nullptr, &server, nullptr);
  if (result != ERROR_SUCCESS) return result;
  result = ::RegSetValueExW(server, nullptr, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(path),
                           static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
  ::RegCloseKey(server);
  return result;
}

std::wstring readRegistration(HKEY root) {
  wchar_t path[MAX_PATH]{};
  DWORD bytes = sizeof(path);
  if (::RegGetValueW(root, kServer, nullptr, RRF_RT_REG_SZ, nullptr, path, &bytes) != ERROR_SUCCESS) {
    return {};
  }
  return path;
}

TEST(VirtualCamRegistration, OldUninstallerPreservesNewerDllButOwnUninstallerRemovesItsKey) {
  using Unregister = HRESULT(STDAPICALLTYPE*)();
  const HMODULE dll = ::LoadLibraryA(COREVIDEO_VCAM_DLL_PATH);
  ASSERT_NE(dll, nullptr) << "virtual-camera DLL could not load: " << ::GetLastError();
  const auto unregister = reinterpret_cast<Unregister>(::GetProcAddress(dll, "DllUnregisterServer"));
  ASSERT_NE(unregister, nullptr);

  wchar_t ownPath[MAX_PATH]{};
  ASSERT_GT(::GetModuleFileNameW(dll, ownPath, MAX_PATH), 0u);
  const std::wstring sandboxPath = L"Software\\CoreVideoProTests\\VirtualCamRegistration-" +
                                   std::to_wstring(::GetCurrentProcessId());
  HKEY sandbox = nullptr;
  ASSERT_EQ(::RegCreateKeyExW(HKEY_CURRENT_USER, sandboxPath.c_str(), 0, nullptr,
                             REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &sandbox, nullptr),
            ERROR_SUCCESS);
  const std::wstring realRegistrationBefore = readRegistration(HKEY_CURRENT_USER);
  const LONG overrideResult = ::RegOverridePredefKey(HKEY_CURRENT_USER, sandbox);
  EXPECT_EQ(overrideResult, ERROR_SUCCESS);
  if (overrideResult == ERROR_SUCCESS) {
    constexpr wchar_t newerDll[] = L"C:\\newer-beta\\corevideo-virtualcam.dll";
    EXPECT_EQ(writeRegistration(sandbox, newerDll), ERROR_SUCCESS);
    EXPECT_EQ(unregister(), S_OK);
    EXPECT_EQ(readRegistration(sandbox), newerDll)
        << "uninstalling an old beta must not remove a newer camera registration";

    EXPECT_EQ(writeRegistration(sandbox, ownPath), ERROR_SUCCESS);
    EXPECT_EQ(unregister(), S_OK);
    EXPECT_TRUE(readRegistration(sandbox).empty())
        << "uninstalling the owning build must remove its camera registration";
    EXPECT_EQ(::RegOverridePredefKey(HKEY_CURRENT_USER, nullptr), ERROR_SUCCESS);
  }
  ::RegCloseKey(sandbox);
  ::RegDeleteTreeW(HKEY_CURRENT_USER, sandboxPath.c_str());
  EXPECT_EQ(readRegistration(HKEY_CURRENT_USER), realRegistrationBefore)
      << "the isolated registry test changed the user's installed camera";
  ::FreeLibrary(dll);
}

TEST(VirtualCamRegistration, StartupRepairsOnlyAliasOfOwnedMachineRuntime) {
  namespace fs = std::filesystem;
  wchar_t exe[MAX_PATH]{};
  ASSERT_GT(::GetModuleFileNameW(nullptr, exe, MAX_PATH), 0u);
  const auto app = fs::path(exe).parent_path().wstring();
  const auto temporary = fs::temp_directory_path() / (L"CoreVideoCameraOwnership-" + std::to_wstring(::GetCurrentProcessId()));
  const std::wstring hash(64, L'a');
  const auto runtime = temporary / L"CoreVideoProCamera" / hash / L"corevideo-virtualcam.dll";
  fs::create_directories(runtime.parent_path());
  fs::copy_file(fs::path(COREVIDEO_VCAM_DLL_PATH), runtime, fs::copy_options::overwrite_existing);
  const std::wstring sandboxPath = L"Software\\CoreVideoProCameraPolicyTests-" + std::to_wstring(::GetCurrentProcessId());
  HKEY sandbox = nullptr, machine = nullptr, user = nullptr;
  ASSERT_EQ(::RegCreateKeyExW(HKEY_CURRENT_USER, sandboxPath.c_str(), 0, nullptr,
      REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &sandbox, nullptr), ERROR_SUCCESS);
  ASSERT_EQ(::RegCreateKeyExW(sandbox, L"machine", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &machine, nullptr), ERROR_SUCCESS);
  ASSERT_EQ(::RegCreateKeyExW(sandbox, L"user", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr, &user, nullptr), ERROR_SUCCESS);
  struct Cleanup {
    HKEY sandbox, machine, user; std::wstring key; fs::path directory;
    ~Cleanup() {
      ::RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
      ::RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
      ::RegCloseKey(user); ::RegCloseKey(machine); ::RegCloseKey(sandbox);
      ::RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
      if (directory.parent_path() == fs::temp_directory_path() &&
          directory.filename() == (L"CoreVideoCameraOwnership-" + std::to_wstring(::GetCurrentProcessId()))) fs::remove_all(directory);
    }
  } cleanup{sandbox, machine, user, sandboxPath, temporary};
  const auto set = [](HKEY root, const wchar_t* path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    LONG result = ::RegCreateKeyExW(root, path, 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &key, nullptr);
    if (result == ERROR_SUCCESS) {
      result = ::RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size()+1)*sizeof(wchar_t)));
      ::RegCloseKey(key);
    }
    return result;
  };
  ASSERT_EQ(writeRegistration(machine, runtime.c_str()), ERROR_SUCCESS);
  ASSERT_EQ(set(machine, kServer, L"CoreVideoOwnerRole", L"CoreVideoPro.VirtualCamera.v1"), ERROR_SUCCESS);
  ASSERT_EQ(set(machine, kServer, L"CoreVideoOwnerAppDirectory", app), ERROR_SUCCESS);
  ASSERT_EQ(set(machine, kServer, L"CoreVideoSha256", hash), ERROR_SUCCESS);
  ASSERT_EQ(set(machine, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion", L"CommonFilesDir", temporary.wstring()), ERROR_SUCCESS);
  ASSERT_EQ(::RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine), ERROR_SUCCESS);
  ASSERT_EQ(::RegOverridePredefKey(HKEY_CURRENT_USER, user), ERROR_SUCCESS);
  auto result = corevideo::modules::ensureVirtualCameraRegistration();
  EXPECT_TRUE(result.ready) << result.warning;
  EXPECT_TRUE(result.repaired);
  EXPECT_EQ(readRegistration(user), runtime.wstring());
  EXPECT_FALSE(corevideo::modules::ensureVirtualCameraRegistration().repaired);
  ASSERT_EQ(set(user, kServer, L"CoreVideoOwnerRole", L"foreign"), ERROR_SUCCESS);
  result = corevideo::modules::ensureVirtualCameraRegistration();
  EXPECT_FALSE(result.ready);
  EXPECT_EQ(readRegistration(user), runtime.wstring());
  EXPECT_NE(result.warning.find("CAMERA_USER_REGISTRATION_CONFLICT"), std::string::npos);
  ASSERT_EQ(::RegDeleteTreeW(machine, kServer), ERROR_SUCCESS);
  result = corevideo::modules::ensureVirtualCameraRegistration();
  EXPECT_FALSE(result.ready);
  EXPECT_NE(result.warning.find("CAMERA_MACHINE_REGISTRATION_MISSING"), std::string::npos);
}
// Run explicitly with COREVIDEO_REQUIRE_VCAM_START=1 against each intended
// registration path. The default native suite has no camera/OS prerequisite;
// an omitted hardware gate is missing evidence, not proof of startup.
TEST(VirtualCamRegistration, RegisteredCameraStartsOnThisMachine) {
  char required[8]{};
  if (::GetEnvironmentVariableA("COREVIDEO_REQUIRE_VCAM_START", required, sizeof(required)) == 0) {
    std::fprintf(stderr, "[  MISSING_EVIDENCE ] VirtualCamRegistration.RegisteredCameraStartsOnThisMachine\n");
    return;
  }
  auto publisher = corevideo::modules::createVirtualCameraPublisher();
  ASSERT_NE(publisher, nullptr);
  ASSERT_TRUE(publisher->start(1280, 720, 30));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  auto state = publisher->status();
  while (state.state == "starting" && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    state = publisher->status();
  }
  EXPECT_EQ(state.state, "live") << state.warning;
  publisher->stop();
}

}  // namespace
#endif
