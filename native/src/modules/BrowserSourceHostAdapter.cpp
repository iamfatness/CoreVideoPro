#include "modules/BrowserSourceHostAdapter.h"

#include "modules/BrowserSourceShm.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace corevideo::modules {
namespace {

constexpr int64_t kFrozenFrameGraceMs = 2000;   // serve the last frame this long after host death
constexpr int64_t kMapRetryIntervalMs = 500;    // SHM open attempts while the host boots
constexpr int64_t kFpsWindowMs = 2000;

bool hasScheme(const std::string& url, const char* scheme) {
  const std::size_t length = std::strlen(scheme);
  if (url.size() < length) {
    return false;
  }
  for (std::size_t i = 0; i < length; ++i) {
    const char a = url[i] >= 'A' && url[i] <= 'Z' ? static_cast<char>(url[i] - 'A' + 'a') : url[i];
    if (a != scheme[i]) {
      return false;
    }
  }
  return true;
}

std::string resolveDefaultHostPath() {
  if (const char* fromEnv = std::getenv("COREVIDEO_BROWSER_HOST_PATH");
      fromEnv != nullptr && fromEnv[0] != '\0') {
    return fromEnv;
  }
#ifdef _WIN32
  char moduleName[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(nullptr, moduleName, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    std::filesystem::path path(moduleName);
    return (path.parent_path() / "corevideo-browser-host.exe").string();
  }
#endif
  return {};
}

unsigned long currentPid() {
#ifdef _WIN32
  return static_cast<unsigned long>(GetCurrentProcessId());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

}  // namespace

BrowserSourceHostAdapter::BrowserSourceHostAdapter(std::string hostExecutablePath)
    : hostExecutablePath_(hostExecutablePath.empty() ? resolveDefaultHostPath()
                                                     : std::move(hostExecutablePath)) {}

BrowserSourceHostAdapter::~BrowserSourceHostAdapter() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    for (auto& [id, source] : sources_) {
      closeProcessLocked(source);
      closeMappingLocked(source);
    }
    sources_.clear();
  }
  supervisorCv_.notify_all();
  if (supervisor_.joinable()) {
    supervisor_.join();
  }
}

bool BrowserSourceHostAdapter::validateAddRequest(const std::string& url, int width, int height,
                                                  int fps, std::string& error) {
  if (url.empty()) {
    error = "browser.add requires a url.";
    return false;
  }
  if (!hasScheme(url, "http://") && !hasScheme(url, "https://") && !hasScheme(url, "file://") &&
      !hasScheme(url, "data:")) {
    error = "browser.add url must be http(s)://, file:// or data: (got '" + url.substr(0, 64) + "').";
    return false;
  }
  if (url.size() > 8000) {
    error = "browser.add url is too long (max 8000 characters).";
    return false;
  }
  for (const char c : url) {
    // The url rides a CreateProcess command line: forbid anything that could
    // escape the quoted argument or smuggle control bytes. Spaces must be
    // %20-encoded (they are in any valid URL).
    if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x21) {
      error = "browser.add url must not contain whitespace, quotes, backslashes or control characters (percent-encode them).";
      return false;
    }
  }
  if (width < 16 || width > 4096 || height < 16 || height > 4096) {
    error = "browser.add width/height must be within 16..4096.";
    return false;
  }
  if (fps < 1 || fps > 60) {
    error = "browser.add fps must be within 1..60.";
    return false;
  }
  return true;
}

std::string BrowserSourceHostAdapter::addSource(const std::string& url, int width, int height,
                                                int fps, std::string& error) {
  if (!validateAddRequest(url, width, height, fps, error)) {
    std::fprintf(stderr, "[browser] REJECTED add: %s\n", error.c_str());
    return {};
  }

  std::string id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const int ordinal = nextOrdinal_++;
    id = "browser:" + std::to_string(ordinal);
    Source source;
    source.id = id;
    source.url = url;
    source.width = width;
    source.height = height;
    source.fps = fps;
    source.shmName = browsershm::shmName(currentPid(), ordinal);
    sources_.emplace(id, std::move(source));
  }
  std::fprintf(stderr, "[browser] added %s %dx%d@%d url='%s'\n", id.c_str(), width, height, fps,
               url.c_str());
  ensureSupervisorStarted();
  supervisorCv_.notify_all();
  return id;
}

bool BrowserSourceHostAdapter::removeSource(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = sources_.find(id);
  if (it == sources_.end()) {
    return false;
  }
  closeProcessLocked(it->second);
  closeMappingLocked(it->second);
  sources_.erase(it);
  std::fprintf(stderr, "[browser] removed %s\n", id.c_str());
  return true;
}

bool BrowserSourceHostAdapter::reloadSource(const std::string& id, std::string& error) {
  bool needsRespawn = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sources_.find(id);
    if (it == sources_.end()) {
      error = "Unknown browser source '" + id + "'.";
      return false;
    }
    Source& source = it->second;
    bool reloadedInPlace = false;
#ifdef _WIN32
    if (source.processHandle != nullptr && source.stdinWrite != nullptr) {
      // Live host: reload the page in place via the stdin command pipe (7 bytes
      // into an empty 4K pipe buffer — never blocks in practice).
      const char command[] = "reload\n";
      DWORD written = 0;
      if (WriteFile(static_cast<HANDLE>(source.stdinWrite), command, sizeof(command) - 1, &written,
                    nullptr) &&
          written == sizeof(command) - 1) {
        reloadedInPlace = true;
      }
    }
#endif
    if (!reloadedInPlace) {
      // Dead / gave-up / unpipeable host: give it a fresh restart cycle.
      closeProcessLocked(source);
      source.policy.reset();
      source.lastError.clear();
      needsRespawn = true;
    }
    std::fprintf(stderr, "[browser] reload %s (%s)\n", id.c_str(),
                 reloadedInPlace ? "in-place" : "respawn");
  }
  if (needsRespawn) {
    ensureSupervisorStarted();
    supervisorCv_.notify_all();
  }
  return true;
}

std::vector<CaptureDeviceInfo> BrowserSourceHostAdapter::enumerate() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const int64_t now = nowMs();
  std::vector<CaptureDeviceInfo> infos;
  infos.reserve(sources_.size());
  for (const auto& [id, source] : sources_) {
    CaptureDeviceInfo info;
    info.id = id;
    info.name = "Browser: " + (source.url.size() > 60 ? source.url.substr(0, 57) + "..." : source.url);
    info.kind = "browser";
    info.vendor = "browser";
    info.inputIds = {"browser"};
    info.inputLabels = {"Web page (WebView2)"};
    info.inputHasEmbeddedAudio = {false};
    info.selectedInputId = "browser";
    info.width = source.width;
    info.height = source.height;
    info.frameRate = source.fps;
    const bool running = source.processHandle != nullptr;
    const bool freshFrame = source.lastFrameAtMs != 0 && now - source.lastFrameAtMs < kFrozenFrameGraceMs;
    info.connectionState = running                      ? "connected"
                           : source.policy.gaveUp()     ? "failed"
                           : !source.lastError.empty()  ? "failed"
                                                        : "connecting";
    info.signalPresent = freshFrame;
    info.warning = source.lastError;
    infos.push_back(std::move(info));
  }
  return infos;
}

std::vector<VideoFrame> BrowserSourceHostAdapter::pollVideoFrames(int64_t timestampMs) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sources_.empty()) {
    return {};
  }
  const int64_t now = nowMs();
  std::vector<VideoFrame> frames;
  for (auto& [id, source] : sources_) {
    if (source.view == nullptr && source.processHandle != nullptr) {
      tryMapLocked(source, now);
    }
    if (source.view != nullptr) {
      // Same discipline (and same per-frame copy cost) as the shipped WinUI
      // capture-shm ingest: one tightly-packed BGRA copy on the render tick.
      auto read = browsershm::readNewFrame(source.view, source.mappedBytes, source.lastSequence);
      if (read.gotNewFrame) {
        if (source.frameId == 0) {
          std::fprintf(stderr, "[browser] first frame %s %dx%d (compositing real page pixels)\n",
                       id.c_str(), read.width, read.height);
        }
        ++source.frameId;
        source.lastPixels = std::move(read.pixels);
        source.lastWidth = read.width;
        source.lastHeight = read.height;
        source.lastFrameAtMs = now;
        source.policy.onHealthy();
        if (source.fpsWindowStartMs == 0) {
          source.fpsWindowStartMs = now;
        }
        ++source.fpsWindowFrames;
        if (now - source.fpsWindowStartMs >= kFpsWindowMs) {
          source.measuredFps = source.fpsWindowFrames * 1000.0 /
                               static_cast<double>(now - source.fpsWindowStartMs);
          source.fpsWindowStartMs = now;
          source.fpsWindowFrames = 0;
        }
      }
    }

    if (!source.lastPixels || source.lastWidth <= 0 || source.lastHeight <= 0) {
      continue;  // no frame ever received (compositor shows its slate)
    }
    const bool running = source.processHandle != nullptr;
    if (!running && now - source.lastFrameAtMs > kFrozenFrameGraceMs) {
      continue;  // host died and the freeze grace elapsed -> slate
    }

    VideoFrame frame;
    frame.participantId = "capture:" + id;
    frame.width = source.lastWidth;
    frame.height = source.lastHeight;
    frame.naturalWidth = source.lastWidth;
    frame.naturalHeight = source.lastHeight;
    frame.pixels = source.lastPixels;
    frame.pixelWidth = source.lastWidth;
    frame.pixelHeight = source.lastHeight;
    frame.pixelStride = source.lastWidth * 4;
    frame.frameId = source.frameId;
    frame.timestampMs = timestampMs;
    frames.push_back(std::move(frame));
  }
  return frames;
}

std::vector<BrowserSourceTelemetry> BrowserSourceHostAdapter::telemetry() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const int64_t now = nowMs();
  std::vector<BrowserSourceTelemetry> result;
  result.reserve(sources_.size());
  for (const auto& [id, source] : sources_) {
    BrowserSourceTelemetry item;
    item.id = id;
    item.url = source.url;
    item.width = source.width;
    item.height = source.height;
    item.fps = source.fps;
    item.measuredFps = source.measuredFps;
    item.running = source.processHandle != nullptr;
    item.gaveUp = source.policy.gaveUp();
    item.restartCount = source.restartCount;
    item.consecutiveFailures = source.policy.consecutiveFailures();
    item.lastError = source.lastError;
    item.framesReceived = source.frameId;
    const bool freshFrame = source.lastFrameAtMs != 0 && now - source.lastFrameAtMs < kFrozenFrameGraceMs;
    item.health = item.running && freshFrame ? "live"
                  : item.gaveUp              ? "failed"
                  : item.running             ? "starting"
                  : item.lastError.empty()   ? "starting"
                                             : "failed";
    result.push_back(std::move(item));
  }
  return result;
}

bool BrowserSourceHostAdapter::empty() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sources_.empty();
}

void BrowserSourceHostAdapter::ensureSupervisorStarted() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (supervisorStarted_ || stopping_) {
    return;
  }
  supervisorStarted_ = true;
  supervisor_ = std::thread([this] { supervisorLoop(); });
}

void BrowserSourceHostAdapter::supervisorLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    supervisorCv_.wait_for(lock, std::chrono::milliseconds(250));
    if (stopping_) {
      break;
    }
    const int64_t now = nowMs();

#ifdef _WIN32
    // 1) Liveness: reap dead hosts LOUDLY and schedule their backoff restart.
    for (auto& [id, source] : sources_) {
      if (source.processHandle == nullptr) {
        continue;
      }
      const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(source.processHandle), 0);
      if (wait != WAIT_OBJECT_0) {
        continue;
      }
      DWORD exitCode = 0;
      GetExitCodeProcess(static_cast<HANDLE>(source.processHandle), &exitCode);
      closeProcessLocked(source);
      closeMappingLocked(source);
      source.policy.onFailure(now);
      source.lastError = "browser host exited (code " + std::to_string(exitCode) + ")";
      std::fprintf(stderr,
                   "[browser] WARNING %s host DIED (exit=%lu, failure %d/%d) — %s\n",
                   id.c_str(), static_cast<unsigned long>(exitCode),
                   source.policy.consecutiveFailures(),
                   BrowserHostRestartPolicy::kMaxConsecutiveFailures,
                   source.policy.gaveUp()
                       ? "GIVING UP; reload the source to retry"
                       : "restarting with backoff");
      }
#endif

    // 2) Spawn candidates: sources with no live host whose backoff window opened.
    struct SpawnJob {
      Source snapshot;
    };
    std::vector<SpawnJob> jobs;
    for (auto& [id, source] : sources_) {
      if (source.processHandle != nullptr || source.spawnInFlight) {
        continue;
      }
      if (!source.policy.shouldAttemptRestart(now)) {
        continue;
      }
      source.spawnInFlight = true;
      jobs.push_back(SpawnJob{source});
    }
    if (jobs.empty()) {
      continue;
    }

    // 3) Spawn UNLOCKED (CreateProcess can take tens of ms cold; the render
    //    thread polls frames under this mutex and must never wait on it).
    lock.unlock();
    struct SpawnOutcome {
      std::string id;
      uint64_t generation = 0;
      bool ok = false;
      void* processHandle = nullptr;
      void* stdinWrite = nullptr;
      std::string error;
    };
    std::vector<SpawnOutcome> outcomes;
    outcomes.reserve(jobs.size());
    for (const auto& job : jobs) {
      SpawnOutcome outcome;
      outcome.id = job.snapshot.id;
      outcome.generation = job.snapshot.generation;
      outcome.ok = spawnHost(job.snapshot, outcome.processHandle, outcome.stdinWrite, outcome.error);
      outcomes.push_back(std::move(outcome));
    }
    lock.lock();

    // 4) Commit under the lock, guarding against remove()/reload() races.
    const int64_t commitNow = nowMs();
    for (auto& outcome : outcomes) {
      auto it = sources_.find(outcome.id);
      if (it == sources_.end() || it->second.generation != outcome.generation || stopping_) {
        // Source vanished (or was recycled) while we were spawning: kill the orphan.
#ifdef _WIN32
        if (outcome.processHandle != nullptr) {
          TerminateProcess(static_cast<HANDLE>(outcome.processHandle), 0);
          CloseHandle(static_cast<HANDLE>(outcome.processHandle));
        }
        if (outcome.stdinWrite != nullptr) {
          CloseHandle(static_cast<HANDLE>(outcome.stdinWrite));
        }
#endif
        continue;
      }
      Source& source = it->second;
      source.spawnInFlight = false;
      if (outcome.ok) {
        source.processHandle = outcome.processHandle;
        source.stdinWrite = outcome.stdinWrite;
        ++source.generation;
        ++source.restartCount;
        source.lastSequence = 0;
        source.lastMapAttemptMs = 0;
        std::fprintf(stderr, "[browser] %s host spawned (attempt %d)\n", outcome.id.c_str(),
                     source.restartCount);
      } else {
        source.policy.onFailure(commitNow);
        source.lastError = outcome.error;
        std::fprintf(stderr,
                     "[browser] WARNING %s host SPAWN FAILED (failure %d/%d): %s — %s\n",
                     outcome.id.c_str(), source.policy.consecutiveFailures(),
                     BrowserHostRestartPolicy::kMaxConsecutiveFailures, outcome.error.c_str(),
                     source.policy.gaveUp() ? "GIVING UP; reload the source to retry"
                                            : "retrying with backoff");
      }
    }
  }
}

bool BrowserSourceHostAdapter::spawnHost(const Source& snapshot, void*& processHandle,
                                         void*& stdinWrite, std::string& error) const {
  processHandle = nullptr;
  stdinWrite = nullptr;
#ifndef _WIN32
  // POSIX host spawn. The host is a separate process by design (page content
  // must never run in the core), and it is handed its arguments as an ARGV
  // VECTOR — never a shell string. The url is operator- and remote-supplied,
  // and this repo already shipped a shell-injection hole in the SRT ingest path
  // by interpolating a url into `/bin/sh -c`.
  if (hostExecutablePath_.empty() || !std::filesystem::exists(hostExecutablePath_)) {
    error = "corevideo-browser-host not found at '" + hostExecutablePath_ +
            "' (set COREVIDEO_BROWSER_HOST_PATH).";
    return false;
  }
  int controlPipe[2] = {-1, -1};
  if (::pipe(controlPipe) != 0) {
    error = "could not create the browser host control pipe.";
    return false;
  }
  const std::string width = std::to_string(snapshot.width);
  const std::string height = std::to_string(snapshot.height);
  const std::string fps = std::to_string(snapshot.fps);
  std::vector<std::string> args{
      hostExecutablePath_, "--url", snapshot.url, "--width", width,
      "--height", height, "--fps", fps, "--shm", snapshot.shmName,
  };
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& argument : args) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(controlPipe[0]);
    ::close(controlPipe[1]);
    error = "fork failed for the browser host.";
    return false;
  }
  if (pid == 0) {
    // Child: the read end becomes stdin so closing our write end (or dying)
    // gives the host EOF and it exits — no orphaned hosts.
    ::dup2(controlPipe[0], STDIN_FILENO);
    ::close(controlPipe[0]);
    ::close(controlPipe[1]);
    ::execv(hostExecutablePath_.c_str(), argv.data());
    ::_exit(127);
  }
  ::close(controlPipe[0]);
  processHandle = reinterpret_cast<void*>(static_cast<intptr_t>(pid));
  stdinWrite = reinterpret_cast<void*>(static_cast<intptr_t>(controlPipe[1]));
  return true;
#else
  if (hostExecutablePath_.empty() || !std::filesystem::exists(hostExecutablePath_)) {
    error = "corevideo-browser-host.exe not found at '" + hostExecutablePath_ +
            "' (set COREVIDEO_BROWSER_HOST_PATH).";
    return false;
  }

  // stdin command pipe: the host reloads on "reload\n" and exits on EOF, so a
  // dead core can never leave orphan hosts behind.
  SECURITY_ATTRIBUTES pipeAttributes{};
  pipeAttributes.nLength = sizeof(pipeAttributes);
  pipeAttributes.bInheritHandle = TRUE;
  HANDLE stdinRead = nullptr;
  HANDLE stdinWriteLocal = nullptr;
  if (!CreatePipe(&stdinRead, &stdinWriteLocal, &pipeAttributes, 0)) {
    error = "CreatePipe failed (err=" + std::to_string(GetLastError()) + ").";
    return false;
  }
  // Only the child's read end may be inherited.
  SetHandleInformation(stdinWriteLocal, HANDLE_FLAG_INHERIT, 0);

  // Route the host's stdout AND stderr to OUR stderr (media-core.log): the
  // core's own stdout is the RPC channel and must never carry host output.
  HANDLE inheritableErr = nullptr;
  const HANDLE ourErr = GetStdHandle(STD_ERROR_HANDLE);
  if (ourErr != nullptr && ourErr != INVALID_HANDLE_VALUE) {
    DuplicateHandle(GetCurrentProcess(), ourErr, GetCurrentProcess(), &inheritableErr, 0, TRUE,
                    DUPLICATE_SAME_ACCESS);
  }

  std::string commandLine = "\"" + hostExecutablePath_ + "\"";
  commandLine += " --url \"" + snapshot.url + "\"";
  commandLine += " --width " + std::to_string(snapshot.width);
  commandLine += " --height " + std::to_string(snapshot.height);
  commandLine += " --fps " + std::to_string(snapshot.fps);
  commandLine += " --shm " + snapshot.shmName;

  STARTUPINFOA startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  startupInfo.dwFlags = STARTF_USESTDHANDLES;
  startupInfo.hStdInput = stdinRead;
  startupInfo.hStdOutput = inheritableErr;
  startupInfo.hStdError = inheritableErr;
  PROCESS_INFORMATION processInfo{};
  const std::string workingDirectory =
      std::filesystem::path(hostExecutablePath_).parent_path().string();
  const BOOL created = CreateProcessA(
      hostExecutablePath_.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
      nullptr, workingDirectory.empty() ? nullptr : workingDirectory.c_str(), &startupInfo,
      &processInfo);
  const DWORD createError = GetLastError();
  CloseHandle(stdinRead);
  if (inheritableErr != nullptr) {
    CloseHandle(inheritableErr);
  }
  if (!created) {
    CloseHandle(stdinWriteLocal);
    error = "CreateProcessA('" + hostExecutablePath_ + "') failed (err=" +
            std::to_string(createError) + ").";
    return false;
  }
  CloseHandle(processInfo.hThread);
  processHandle = processInfo.hProcess;
  stdinWrite = stdinWriteLocal;
  return true;
#endif
}

void BrowserSourceHostAdapter::closeProcessLocked(Source& source) {
#ifdef _WIN32
  if (source.stdinWrite != nullptr) {
    // Closing stdin is the graceful quit signal (host exits on EOF)...
    CloseHandle(static_cast<HANDLE>(source.stdinWrite));
    source.stdinWrite = nullptr;
  }
  if (source.processHandle != nullptr) {
    // ...and the terminate right behind it keeps teardown deterministic without
    // ever waiting under the adapter mutex (the host has no state to flush).
    TerminateProcess(static_cast<HANDLE>(source.processHandle), 0);
    CloseHandle(static_cast<HANDLE>(source.processHandle));
    source.processHandle = nullptr;
  }
#else
  source.stdinWrite = nullptr;
  source.processHandle = nullptr;
#endif
}

void BrowserSourceHostAdapter::closeMappingLocked(Source& source) {
#ifdef _WIN32
  if (source.view != nullptr) {
    UnmapViewOfFile(source.view);
  }
  if (source.mappingHandle != nullptr) {
    CloseHandle(static_cast<HANDLE>(source.mappingHandle));
  }
#else
  if (source.view != nullptr && source.mappedBytes > 0) {
    ::munmap(const_cast<uint8_t*>(source.view), source.mappedBytes);
  }
  // mappingHandle carries the fd as an integer on POSIX. -1/0 means unset.
  if (source.mappingHandle != nullptr) {
    ::close(static_cast<int>(reinterpret_cast<intptr_t>(source.mappingHandle)));
  }
#endif
  source.view = nullptr;
  source.mappingHandle = nullptr;
  source.mappedBytes = 0;
  source.lastSequence = 0;
}

void BrowserSourceHostAdapter::tryMapLocked(Source& source, int64_t nowMsValue) {
  if (nowMsValue - source.lastMapAttemptMs < kMapRetryIntervalMs) {
    return;
  }
  source.lastMapAttemptMs = nowMsValue;
#ifdef _WIN32
  const std::wstring wname(source.shmName.begin(), source.shmName.end());
  HANDLE handle = OpenFileMappingW(FILE_MAP_READ, FALSE, wname.c_str());
  if (handle == nullptr) {
    return;  // host still booting; retried on the next interval
  }
  const std::size_t bytes = browsershm::mappingBytes(source.width, source.height);
  const void* view = MapViewOfFile(handle, FILE_MAP_READ, 0, 0, bytes);
  if (view == nullptr) {
    CloseHandle(handle);
    std::fprintf(stderr, "[browser] WARNING %s MapViewOfFile('%s') failed (err=%lu)\n",
                 source.id.c_str(), source.shmName.c_str(),
                 static_cast<unsigned long>(GetLastError()));
    return;
  }
  source.mappingHandle = handle;
  source.view = static_cast<const uint8_t*>(view);
  source.mappedBytes = bytes;
  std::fprintf(stderr, "[browser] mapped %s shm='%s' (%dx%d)\n", source.id.c_str(),
               source.shmName.c_str(), source.width, source.height);
#else
  // Read-only shared mapping published by the host process. Retried on the
  // same interval as Windows: the host is still booting for the first frames,
  // and a missing segment is normal until it publishes.
  const int fd = ::shm_open(source.shmName.c_str(), O_RDONLY, 0);
  if (fd < 0) {
    return;  // host still booting; retried on the next interval
  }
  const std::size_t bytes = browsershm::mappingBytes(source.width, source.height);
  void* view = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd, 0);
  if (view == MAP_FAILED) {
    ::close(fd);
    std::fprintf(stderr, "[browser] WARNING %s mmap('%s', %zu) failed: %s\n",
                 source.id.c_str(), source.shmName.c_str(), bytes,
                 std::strerror(errno));
    return;
  }
  source.mappingHandle = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
  source.view = static_cast<const uint8_t*>(view);
  source.mappedBytes = bytes;
  std::fprintf(stderr, "[browser] mapped %s shm='%s' (%dx%d)\n", source.id.c_str(),
               source.shmName.c_str(), source.width, source.height);
#endif
}

int64_t BrowserSourceHostAdapter::nowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::unique_ptr<BrowserSourceHostAdapter> createBrowserSourceHostAdapter() {
  return std::make_unique<BrowserSourceHostAdapter>();
}

}  // namespace corevideo::modules
