#pragma once

// POSIX-only executable discovery shared by every adapter that launches an
// external `ffmpeg` (RtmpOutputSenderAdapter for RTMP + SRT output,
// SrtIngestCaptureAdapter for SRT/RTMP ingest). Windows keeps its own,
// unchanged resolution paths; this header is never included there.
//
// Why it exists: a macOS app launched from Finder/Dock inherits launchd's
// PATH (/usr/bin:/bin:/usr/sbin:/sbin), so a PATH-only search never finds a
// Homebrew or MacPorts ffmpeg even when one is installed — RTMP and SRT were
// silently unavailable for every normal launch. After PATH, the well-known
// package-manager bin directories are probed. The fallback list is a
// parameter so tests can inject a temp directory.

#if !defined(_WIN32)

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#include <cstdlib>
#endif

namespace corevideo::modules::posix_exec {

// Apple Silicon Homebrew, Intel Homebrew / manual installs, MacPorts.
inline std::vector<std::filesystem::path> defaultFfmpegFallbackDirectories() {
  return {"/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin"};
}

// A regular file (symlinks followed — Homebrew's bin entries are symlinks
// into the Cellar) that this process may execute.
inline bool isExecutableFile(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    return false;
  }
  return ::access(path.c_str(), X_OK) == 0;
}

// First executable `name` in each PATH entry (in order), then in each
// fallback directory (in order). Empty string when none qualifies. PATH wins
// whenever it holds a usable copy, so an operator's explicit PATH choice is
// never overridden by the fallback list.
inline std::string findOnPathThenFallbacks(std::string_view name, const char* pathEnv,
                                           const std::vector<std::filesystem::path>& fallbacks) {
  if (pathEnv != nullptr) {
    std::string_view rest(pathEnv);
    while (true) {
      const auto colon = rest.find(':');
      const std::string_view entry = rest.substr(0, colon);
      if (!entry.empty()) {
        const auto candidate = std::filesystem::path(std::string(entry)) / std::string(name);
        if (isExecutableFile(candidate)) {
          return candidate.string();
        }
      }
      if (colon == std::string_view::npos) {
        break;
      }
      rest.remove_prefix(colon + 1);
    }
  }
  for (const auto& directory : fallbacks) {
    const auto candidate = directory / std::string(name);
    if (isExecutableFile(candidate)) {
      return candidate.string();
    }
  }
  return {};
}

// Directory of the running executable (symlinks resolved), or empty.
inline std::filesystem::path currentExecutableDirectory() {
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (size == 0 || _NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  buffer.resize(std::char_traits<char>::length(buffer.c_str()));
  char resolved[PATH_MAX] = {};
  if (::realpath(buffer.c_str(), resolved) != nullptr) {
    return std::filesystem::path(resolved).parent_path();
  }
  return std::filesystem::path(buffer).parent_path();
#else
  std::error_code error;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
  return error ? std::filesystem::path{} : self.parent_path();
#endif
}

// ONE ffmpeg resolution for every POSIX adapter (the RTMP/SRT/HLS sender and
// SRT/RTMP ingest), mac streaming Phase 2 F1. Order:
//   1. the destination's configured bin directory
//   2. $COREVIDEO_FFMPEG_BIN_DIR, then $COREVIDEO_FFMPEG_DIR (ingest's old
//      name, kept as an alias), then $FFMPEG_BIN_DIR
//   3. the core's own directory: the macOS release bundles its LGPL ffmpeg in
//      Contents/Resources beside corevideo-native, so it beats any Homebrew copy
//   4. the current directory, PATH, then the Homebrew/MacPorts fallbacks.
// Every input is injectable so the order is testable without touching the
// real environment.
struct FfmpegSearch {
  std::string configuredDirectory;
  std::function<const char*(const char*)> getenv = [](const char* name) { return std::getenv(name); };
  std::filesystem::path executableDirectory = currentExecutableDirectory();
  std::filesystem::path currentDirectory = [] {
    std::error_code error;
    auto here = std::filesystem::current_path(error);
    return error ? std::filesystem::path{} : here;
  }();
  std::string pathEnvironment = [] {
    const char* path = std::getenv("PATH");
    return std::string(path ? path : "");
  }();
  std::vector<std::filesystem::path> fallbacks = defaultFfmpegFallbackDirectories();
};

inline std::string resolveFfmpeg(const FfmpegSearch& search) {
  constexpr const char* name = "ffmpeg";
  std::vector<std::filesystem::path> directories;
  if (!search.configuredDirectory.empty()) directories.emplace_back(search.configuredDirectory);
  for (const char* variable : {"COREVIDEO_FFMPEG_BIN_DIR", "COREVIDEO_FFMPEG_DIR", "FFMPEG_BIN_DIR"}) {
    const char* value = search.getenv ? search.getenv(variable) : nullptr;
    if (value != nullptr && *value != '\0') directories.emplace_back(value);
  }
  if (!search.executableDirectory.empty()) directories.push_back(search.executableDirectory);
  if (!search.currentDirectory.empty()) directories.push_back(search.currentDirectory);
  for (const auto& directory : directories) {
    const auto candidate = directory / name;
    if (isExecutableFile(candidate)) return candidate.string();
  }
  return findOnPathThenFallbacks(name, search.pathEnvironment.empty() ? nullptr : search.pathEnvironment.c_str(),
                                 search.fallbacks);
}

}  // namespace corevideo::modules::posix_exec

#endif  // !_WIN32
