#pragma once

// Plugin-host scan helpers (VST host spec P1, docs/vst-host-spec.md).
//
// The out-of-process `corevideo-plugin-host --scan` prints one JSON line per
// discovered VST3 bundle. Parsing is a PURE function over that output so tests
// never spawn the executable; the runner shells out and captures stdout via a
// temp file (the scan is an operator-initiated, non-real-time action running
// on its own thread — never under coreMutex or the audio worker).

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "rpc/Json.h"

namespace corevideo::core {

struct PluginHostPluginInfo {
  std::string id;      // bundle path — stable identity for probe/load phases
  std::string name;
  std::string vendor;
  std::string probe = "pending";  // pending|pass|fail (probe lands in P2)
};

inline std::vector<PluginHostPluginInfo> parsePluginScanOutput(const std::string& output) {
  std::vector<PluginHostPluginInfo> plugins;
  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty() || line.front() != '{') {
      continue;  // stray build/tool noise around the JSON lines
    }
    const auto parsed = rpc::Json::parse(line);
    if (!parsed.has_value() || parsed->getString("cmd") != "plugin") {
      continue;
    }
    PluginHostPluginInfo plugin;
    plugin.id = parsed->getString("id");
    plugin.name = parsed->getString("name");
    plugin.vendor = parsed->getString("vendor");
    const auto probe = parsed->getString("probe");
    if (!probe.empty()) {
      plugin.probe = probe;
    }
    if (!plugin.id.empty()) {
      plugins.push_back(std::move(plugin));
    }
  }
  return plugins;
}

// Runs `<exePath> --scan`, returning raw stdout ("" on launch failure).
// SECURITY: exePath derives from an environment variable, so this must NEVER
// go through a shell (std::system/popen would be command injection — CodeQL
// cpp/command-line-injection). The process is spawned directly with stdout on
// an anonymous pipe: the path is an argv element, not shell input, and no
// temp files are involved. Callers run this OFF the lock domains.
#ifdef _WIN32
inline std::string runPluginHostScan(const std::string& exePath) {
  if (exePath.empty()) {
    return {};
  }

  SECURITY_ATTRIBUTES pipeAttributes{};
  pipeAttributes.nLength = sizeof(pipeAttributes);
  pipeAttributes.bInheritHandle = TRUE;
  HANDLE readEnd = nullptr;
  HANDLE writeEnd = nullptr;
  if (!::CreatePipe(&readEnd, &writeEnd, &pipeAttributes, 0)) {
    return {};
  }
  ::SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = writeEnd;
  startup.hStdError = writeEnd;

  // lpApplicationName carries the exact executable path (no shell, no PATH
  // search); the command line only supplies argv for the child.
  std::string commandLine = "\"" + exePath + "\" --scan";
  std::vector<char> mutableCommandLine(commandLine.begin(), commandLine.end());
  mutableCommandLine.push_back('\0');
  PROCESS_INFORMATION process{};
  const BOOL launched = ::CreateProcessA(exePath.c_str(), mutableCommandLine.data(), nullptr, nullptr,
                                         TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  ::CloseHandle(writeEnd);
  if (!launched) {
    ::CloseHandle(readEnd);
    return {};
  }

  std::string output;
  char buffer[4096];
  DWORD bytesRead = 0;
  while (::ReadFile(readEnd, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
    output.append(buffer, bytesRead);
  }
  ::CloseHandle(readEnd);

  ::WaitForSingleObject(process.hProcess, 15000);
  DWORD exitCode = 1;
  ::GetExitCodeProcess(process.hProcess, &exitCode);
  ::CloseHandle(process.hProcess);
  ::CloseHandle(process.hThread);
  return exitCode == 0 ? output
                       : (output.find("\"cmd\":\"plugin\"") != std::string::npos ? output : std::string{});
}
#else
inline std::string runPluginHostScan(const std::string& exePath) {
  if (exePath.empty()) {
    return {};
  }

  int fds[2];
  if (::pipe(fds) != 0) {
    return {};
  }
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return {};
  }
  if (child == 0) {
    ::dup2(fds[1], STDOUT_FILENO);
    ::dup2(fds[1], STDERR_FILENO);
    ::close(fds[0]);
    ::close(fds[1]);
    ::execl(exePath.c_str(), "corevideo-plugin-host", "--scan", static_cast<char*>(nullptr));
    _exit(127);  // exec failed — no shell fallback, by design
  }

  ::close(fds[1]);
  std::string output;
  char buffer[4096];
  ssize_t bytesRead = 0;
  while ((bytesRead = ::read(fds[0], buffer, sizeof(buffer))) > 0) {
    output.append(buffer, static_cast<size_t>(bytesRead));
  }
  ::close(fds[0]);

  int status = 0;
  ::waitpid(child, &status, 0);
  const bool cleanExit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
  return cleanExit ? output
                   : (output.find("\"cmd\":\"plugin\"") != std::string::npos ? output : std::string{});
}
#endif

}  // namespace corevideo::core
