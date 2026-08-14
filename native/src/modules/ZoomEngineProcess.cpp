#include "modules/ZoomEngineProcess.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine-ipc.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace corevideo::modules {
namespace {

#if defined(_WIN32)
IpcFd asIpcFd(void* value) {
  return static_cast<IpcFd>(value);
}

bool isConnected(void* value) {
  return value && value != INVALID_HANDLE_VALUE;
}

void closeHandle(void*& value) {
  if (isConnected(value)) {
    CloseHandle(static_cast<HANDLE>(value));
  }
  value = nullptr;
}

std::string windowsError(const char* prefix) {
  return std::string(prefix) + " failed with Win32 error " + std::to_string(GetLastError());
}
#else
int connectUnixSocket(const char* path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

void closeFd(int& value) {
  if (value >= 0) {
    close(value);
  }
  value = -1;
}

bool isConnected(int value) {
  return value >= 0;
}
#endif

}  // namespace

std::string makeZoomEngineInstanceToken() {
  static std::atomic<std::uint32_t> spawnCounter{0};
  const std::uint32_t n = spawnCounter.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
  const unsigned long pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
  const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
  return std::to_string(pid) + "-" + std::to_string(n);
}

ZoomEngineProcessClient::~ZoomEngineProcessClient() {
  stop();
}

bool ZoomEngineProcessClient::start(const ZoomEngineProcessOptions& options) {
  stop();
  lastError_.clear();
  if (options.executablePath.empty()) {
    setError("Zoom engine executable path is empty.");
    return false;
  }

  // Establish the per-instance IPC token before spawning: the engine must be
  // told it on its command line, and our own pipe/SHM names derive from it.
  instanceToken_ = options.instanceToken.empty() ? makeZoomEngineInstanceToken()
                                                  : options.instanceToken;

#if defined(_WIN32)
  STARTUPINFOA startupInfo{};
  startupInfo.cb = sizeof(startupInfo);
  PROCESS_INFORMATION processInfo{};
  std::string commandLine = "\"" + options.executablePath + "\"";
  if (!instanceToken_.empty()) {
    commandLine += " --ipc-token " + instanceToken_;
  }
  const std::string workingDirectory = std::filesystem::path(options.executablePath).parent_path().string();
  const BOOL created = CreateProcessA(
      options.executablePath.c_str(),
      commandLine.data(),
      nullptr,
      nullptr,
      FALSE,
      CREATE_NO_WINDOW | CREATE_SUSPENDED,
      nullptr,
      workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
      &startupInfo,
      &processInfo);
  if (!created) {
    setError(windowsError("CreateProcessA"));
    return false;
  }

  // Put the helper in a private kill-on-close job before it executes any SDK
  // code. If corevideo-native is force-killed, Windows closes this last job
  // handle and terminates the helper immediately; otherwise the orphan keeps
  // Zoom's callback/runtime ownership for several seconds and the replacement
  // helper fails InitSDK with code 14.
  HANDLE job = CreateJobObjectA(nullptr, nullptr);
  if (!job) {
    const auto error = windowsError("CreateJobObjectA");
    TerminateProcess(processInfo.hProcess, 1);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    setError(error);
    return false;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    const auto error = windowsError("SetInformationJobObject");
    TerminateProcess(processInfo.hProcess, 1);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    CloseHandle(job);
    setError(error);
    return false;
  }
  if (!AssignProcessToJobObject(job, processInfo.hProcess)) {
    const auto error = windowsError("AssignProcessToJobObject");
    TerminateProcess(processInfo.hProcess, 1);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    CloseHandle(job);
    setError(error);
    return false;
  }

  processHandle_ = processInfo.hProcess;
  threadHandle_ = processInfo.hThread;
  jobHandle_ = job;
  if (ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1)) {
    setError(windowsError("ResumeThread"));
    stop();
    return false;
  }
#else
  const pid_t pid = fork();
  if (pid < 0) {
    setError(std::string("fork failed: ") + std::strerror(errno));
    return false;
  }
  if (pid == 0) {
    if (!instanceToken_.empty()) {
      execl(options.executablePath.c_str(), options.executablePath.c_str(), "--ipc-token",
            instanceToken_.c_str(), static_cast<char*>(nullptr));
    } else {
      execl(options.executablePath.c_str(), options.executablePath.c_str(), static_cast<char*>(nullptr));
    }
    _exit(127);
  }
  processId_ = static_cast<int>(pid);
#endif

  if (!connectIpc(options.connectTimeoutMs)) {
    stop();
    return false;
  }
  return true;
}

void ZoomEngineProcessClient::stop() {
  if (isConnected(parentToEngine_)) {
    (void)sendLine(buildZoomEngineQuitCommand());
  }
  closeIpc();

#if defined(_WIN32)
  if (processHandle_) {
    const DWORD waitResult = WaitForSingleObject(static_cast<HANDLE>(processHandle_), 1000);
    if (waitResult == WAIT_TIMEOUT) {
      TerminateProcess(static_cast<HANDLE>(processHandle_), 1);
      WaitForSingleObject(static_cast<HANDLE>(processHandle_), 1000);
    }
  }
  closeHandle(threadHandle_);
  closeHandle(processHandle_);
  closeHandle(jobHandle_);
#else
  if (processId_ > 0) {
    int status = 0;
    const pid_t waited = waitpid(static_cast<pid_t>(processId_), &status, WNOHANG);
    if (waited == 0) {
      kill(static_cast<pid_t>(processId_), SIGTERM);
      for (int i = 0; i < 10; ++i) {
        if (waitpid(static_cast<pid_t>(processId_), &status, WNOHANG) == processId_) {
          processId_ = -1;
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      kill(static_cast<pid_t>(processId_), SIGKILL);
      waitpid(static_cast<pid_t>(processId_), &status, 0);
    }
    processId_ = -1;
  }
#endif
}

bool ZoomEngineProcessClient::running() const {
#if defined(_WIN32)
  if (!processHandle_) {
    return false;
  }
  DWORD exitCode = 0;
  return GetExitCodeProcess(static_cast<HANDLE>(processHandle_), &exitCode) && exitCode == STILL_ACTIVE;
#else
  return processId_ > 0;
#endif
}

std::string ZoomEngineProcessClient::lastError() const {
  return lastError_;
}

bool ZoomEngineProcessClient::sendLine(const std::string& line) {
  if (!isConnected(parentToEngine_)) {
    setError("Zoom engine parent-to-engine IPC is not connected.");
    return false;
  }
  if (!ipc_write_line(parentToEngine_, line)) {
    setError("Failed to write to Zoom engine IPC.");
    return false;
  }
  return true;
}

std::optional<std::string> ZoomEngineProcessClient::readLine() {
  if (!isConnected(engineToParent_)) {
    setError("Zoom engine engine-to-parent IPC is not connected.");
    return std::nullopt;
  }
  std::string line;
  if (!ipc_read_line(engineToParent_, line)) {
    setError("Failed to read from Zoom engine IPC.");
    return std::nullopt;
  }
  return line;
}

std::optional<ZoomEngineEvent> ZoomEngineProcessClient::readEvent() {
  auto line = readLine();
  if (!line) {
    return std::nullopt;
  }
  auto event = parseZoomEngineEvent(*line);
  if (!event) {
    setError("Zoom engine emitted an unparseable event.");
  }
  return event;
}

bool ZoomEngineProcessClient::connectIpc(int timeoutMs) {
#if defined(_WIN32)
  const std::string pipeP2E = ipc_pipe_p2e(instanceToken_);
  const std::string pipeE2P = ipc_pipe_e2p(instanceToken_);
#else
  const std::string sockP2E = ipc_sock_p2e(instanceToken_);
  const std::string sockE2P = ipc_sock_e2p(instanceToken_);
#endif
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
#if defined(_WIN32)
    if (!parentToEngine_) {
      if (WaitNamedPipeA(pipeP2E.c_str(), 100)) {
        parentToEngine_ = CreateFileA(pipeP2E.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (parentToEngine_ == INVALID_HANDLE_VALUE) {
          parentToEngine_ = nullptr;
        }
      }
    }
    if (!engineToParent_) {
      if (WaitNamedPipeA(pipeE2P.c_str(), 100)) {
        engineToParent_ = CreateFileA(pipeE2P.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (engineToParent_ == INVALID_HANDLE_VALUE) {
          engineToParent_ = nullptr;
        }
      }
    }
#else
    if (parentToEngine_ < 0) {
      parentToEngine_ = connectUnixSocket(sockP2E.c_str());
    }
    if (engineToParent_ < 0) {
      engineToParent_ = connectUnixSocket(sockE2P.c_str());
    }
#endif
    if (isConnected(parentToEngine_) && isConnected(engineToParent_)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  setError("Timed out connecting to Zoom engine IPC.");
  closeIpc();
  return false;
}

void ZoomEngineProcessClient::closeIpc() {
#if defined(_WIN32)
  closeHandle(parentToEngine_);
  closeHandle(engineToParent_);
#else
  closeFd(parentToEngine_);
  closeFd(engineToParent_);
#endif
}

void ZoomEngineProcessClient::setError(std::string message) {
  lastError_ = std::move(message);
}

}  // namespace corevideo::modules
