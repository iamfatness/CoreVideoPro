#include <gtest/gtest.h>

#if defined(_WIN32) && COREVIDEO_WITH_VIRTUALCAM
#include <windows.h>

#include "modules/VirtualCameraPublisher.h"

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
