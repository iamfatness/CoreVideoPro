#pragma once

#if defined(_WIN32)
#include <windows.h>
#include <sddl.h>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace corevideo::test {

// Test-only security. The publisher's backing-file policy is deliberately not
// changed: deletion is authorized by the private containing directory instead.
struct VcamTestDirectorySecurity {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  DWORD error = ERROR_SUCCESS;

  explicit VcamTestDirectorySecurity(const wchar_t* rights = L"FA") {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) { error = GetLastError(); return; }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER || size == 0) {
      if (error == ERROR_SUCCESS || size == 0) error = ERROR_INVALID_DATA;
      CloseHandle(token); return;
    }
    std::vector<BYTE> bytes(size);
    if (!GetTokenInformation(token, TokenUser, bytes.data(), size, &size)) {
      error = GetLastError(); CloseHandle(token); return;
    }
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) {
      error = GetLastError(); CloseHandle(token); return;
    }
    const std::wstring sddl = std::wstring(L"D:P(A;OICI;") + rights + L";;;" + sid + L")";
    LocalFree(sid); CloseHandle(token);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
      error = GetLastError(); return;
    }
    error = ERROR_SUCCESS;
  }
  ~VcamTestDirectorySecurity() { if (descriptor) LocalFree(descriptor); }
  VcamTestDirectorySecurity(const VcamTestDirectorySecurity&) = delete;
  VcamTestDirectorySecurity& operator=(const VcamTestDirectorySecurity&) = delete;
};

struct VcamTestCleanupResult {
  bool ok;
  DWORD error;
  std::string path;
};

class VirtualCameraTestDirectory {
public:
  VirtualCameraTestDirectory() {
    char temporary[MAX_PATH]{};
    const DWORD length = GetTempPathA(MAX_PATH, temporary);
    if (length == 0 || length >= MAX_PATH) {
      error_ = length == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE;
      stage_ = "temporary-directory";
      return;
    }
    create(std::string(temporary, length));
  }
  explicit VirtualCameraTestDirectory(const std::string& parent) { create(parent); }
  VirtualCameraTestDirectory(const VirtualCameraTestDirectory&) = delete;
  VirtualCameraTestDirectory& operator=(const VirtualCameraTestDirectory&) = delete;

  ~VirtualCameraTestDirectory() {
    if (!owned_) return;
    for (const auto* name : names_) {
      const auto result = remove(name);
      if (!result.ok) report(result);
    }
    if (!RemoveDirectoryA(path_.c_str()))
      std::fprintf(stderr, "[vcam-test-cleanup] remove-directory win32=%lu path=%s\n",
          static_cast<unsigned long>(GetLastError()), path_.c_str());
  }

  bool ready() const { return owned_ && error_ == ERROR_SUCCESS; }
  const std::string& path() const { return path_; }
  DWORD error() const { return error_; }
  const char* stage() const { return stage_; }

  VcamTestCleanupResult remove(std::string_view name, decltype(&::DeleteFileA) unlink = &::DeleteFileA) const {
    bool known = false;
    for (const auto* allowed : names_) if (name == allowed) known = true;
    if (!owned_ || !known) return {false, ERROR_INVALID_NAME, path_};
    const DWORD attributes = GetFileAttributesA(path_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return {false, GetLastError(), path_};
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
      return {false, ERROR_ACCESS_DENIED, path_};
    const std::string target = path_ + "\\" + std::string(name);
    if (unlink(target.c_str())) return {true, ERROR_SUCCESS, target};
    const DWORD error = GetLastError();
    return {error == ERROR_FILE_NOT_FOUND, error, target};
  }

  static void report(const VcamTestCleanupResult& result) {
    std::fprintf(stderr, "[vcam-test-cleanup] delete-file win32=%lu path=%s\n",
        static_cast<unsigned long>(result.error), result.path.c_str());
  }

private:
  void create(std::string parent) {
    stage_ = "create-private-directory";
    if (parent.empty()) { error_ = ERROR_PATH_NOT_FOUND; return; }
    if (parent.back() != '\\' && parent.back() != '/') parent.push_back('\\');
    VcamTestDirectorySecurity security;
    if (!security.descriptor) { error_ = security.error; stage_ = "directory-security"; return; }
    SECURITY_ATTRIBUTES attributes{static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)), security.descriptor, FALSE};
    const std::string prefix = parent + "cvp-vcam-shm-test-" + std::to_string(GetCurrentProcessId()) +
        "-" + std::to_string(GetTickCount64()) + "-";
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
      path_ = prefix + std::to_string(attempt);
      if (CreateDirectoryA(path_.c_str(), &attributes)) { owned_ = true; error_ = ERROR_SUCCESS; return; }
      error_ = GetLastError();
      // Never reuse a stale directory or a possible junction from a prior PID.
      if (error_ != ERROR_ALREADY_EXISTS) return;
    }
  }

  inline static constexpr std::array<const char*, 4> names_{
      "vcam-frame.shm", "vcam-serve.log", "vcam-correlation-v1.shm", "vcam-history-v1.shm"};
  std::string path_;
  DWORD error_ = ERROR_SUCCESS;
  const char* stage_ = "create-private-directory";
  bool owned_ = false;
};

} // namespace corevideo::test
#endif
