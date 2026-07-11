#pragma once

// Shared-memory layout for the virtual camera (docs/virtual-camera-spec.md V2).
// The core (corevideo-native.exe) publishes the program frame as NV12 here; the
// virtual-camera COM DLL (loaded in frameserver.exe) reads the latest complete
// frame. Single latest-wins slot with a SEQLOCK: a camera only ever needs the
// newest frame, and the seqlock guarantees the reader never sees a torn frame
// (seq odd = writer mid-update, even = complete). Same discipline as the audio
// ring. This header is shared verbatim by the core and the DLL.

#include <cstddef>
#include <cstdint>
#include <cstdlib>  // std::getenv
#include <string>

namespace corevideo::modules {

inline constexpr std::uint32_t kVirtualCameraMagic = 0x43564643u;  // 'CVFC'
inline constexpr int kVirtualCameraMaxWidth = 1920;
inline constexpr int kVirtualCameraMaxHeight = 1080;

// NV12 max payload (1080p) = w*h*3/2.
inline constexpr std::size_t kVirtualCameraMaxPayload =
    static_cast<std::size_t>(kVirtualCameraMaxWidth) * kVirtualCameraMaxHeight * 3 / 2;

#pragma pack(push, 4)
struct VirtualCameraShmHeader {
  std::uint32_t magic;         // kVirtualCameraMagic once initialized
  volatile std::uint32_t seq;  // seqlock: odd = writing, even = complete
  std::int32_t width;          // current frame dimensions
  std::int32_t height;
  std::int32_t fps;
  std::uint32_t byteLen;       // NV12 bytes valid in the payload
  std::uint64_t frameNumber;   // monotonic, for the reader's drop/liveness telemetry
};
#pragma pack(pop)

inline std::size_t virtualCameraShmSize() {
  return sizeof(VirtualCameraShmHeader) + kVirtualCameraMaxPayload;
}

// Legacy session-scoped name. NO LONGER USED for serving: a Local\ mapping is
// invisible to the Windows Frame Server, which serves the camera from session 0
// while the core publishes from the user's interactive session. Kept only for
// reference. See virtualCameraShmFilePath() for the cross-session slot.
inline std::string virtualCameraShmName() {
  return "Local\\CoreVideoProVirtualCamera";
}

// Backing-file path for the slot. The slot MUST cross the session boundary: the
// core publishes from the user's interactive session (session 1) while the
// Windows Frame Server serves the camera from session 0. A Local\ mapping does
// not cross that boundary, and a Global\ object needs SeCreateGlobalPrivilege
// (denied to a normal, non-elevated app). So we back the slot with a real file
// on a machine-wide, world-readable path (%ProgramData%, identical in every
// session): both processes map the same file, which the OS keeps coherent across
// sessions, gated only by the file's ACL (set permissively at create time).
inline std::string virtualCameraShmDir() {
  const char* pd = std::getenv("ProgramData");
  std::string root = (pd != nullptr && *pd != '\0') ? std::string(pd) : std::string("C:\\ProgramData");
  return root + "\\CoreVideoPro";
}
inline std::string virtualCameraShmFilePath() {
  return virtualCameraShmDir() + "\\vcam-frame.shm";
}

}  // namespace corevideo::modules

#if defined(_WIN32)
#include <windows.h>
#include <sddl.h>  // ConvertStringSecurityDescriptorToSecurityDescriptorW

namespace corevideo::modules {

// Opens the backing file for the slot. writer=true creates it read+write with a
// permissive DACL so the session-0 Frame Server (a restricted service account /
// app container) can read what the core publishes; writer=false opens the
// existing file read-only. Returns INVALID_HANDLE_VALUE on failure. The returned
// handle must stay open for the lifetime of any view mapped from it.
inline HANDLE openVirtualCameraShmFile(bool writer) {
  const std::string path = virtualCameraShmFilePath();
  if (writer) {
    ::CreateDirectoryA(virtualCameraShmDir().c_str(), nullptr);  // ok if it exists
    ::DeleteFileA(path.c_str());  // start clean so the DACL below always applies
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    // Everyone (WD) read+write; ALL APPLICATION PACKAGES (AC) read. Everyone
    // covers the LocalService the Frame Server runs under; AC covers an
    // app-container'd frame server.
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;FRFW;;;WD)(A;;FR;;;AC)", SDDL_REVISION_1, &sd, nullptr) != FALSE) {
      sa.lpSecurityDescriptor = sd;
    }
    HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             sa.lpSecurityDescriptor != nullptr ? &sa : nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (sd != nullptr) ::LocalFree(sd);
    return h;
  }
  return ::CreateFileA(path.c_str(), GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
}

// Maps the whole slot over an open backing-file handle. Returns the view (or
// nullptr) and, on success, the mapping handle via outMapping. The file handle
// must outlive the view.
inline void* mapVirtualCameraShmView(HANDLE file, bool writer, HANDLE* outMapping) {
  if (outMapping != nullptr) *outMapping = nullptr;
  if (file == nullptr || file == INVALID_HANDLE_VALUE) return nullptr;
  HANDLE m = ::CreateFileMappingA(file, nullptr, writer ? PAGE_READWRITE : PAGE_READONLY, 0,
                                  static_cast<DWORD>(virtualCameraShmSize()), nullptr);
  if (m == nullptr) return nullptr;
  void* v = ::MapViewOfFile(m, writer ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0,
                            virtualCameraShmSize());
  if (v == nullptr) {
    ::CloseHandle(m);
    return nullptr;
  }
  if (outMapping != nullptr) *outMapping = m;
  return v;
}

}  // namespace corevideo::modules
#endif  // _WIN32
