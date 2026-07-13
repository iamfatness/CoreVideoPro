#pragma once

// Plugin-host scan helpers (VST host spec P1, docs/vst-host-spec.md).
//
// The out-of-process `corevideo-plugin-host --scan` prints one JSON line per
// discovered VST3 bundle. Parsing is a PURE function over that output so tests
// never spawn the executable; the runner shells out and captures stdout via a
// temp file (the scan is an operator-initiated, non-real-time action running
// on its own thread — never under coreMutex or the audio worker).

#include <cctype>
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
  // P2c: every audio class the probe found in the bundle (Waves shells carry
  // many). Insert names select against these.
  std::vector<std::string> classNames;
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

// P2b-2: which insert names route through the out-of-process host. Built-in
// names (gate/EQ/compressor/limiter) never match; "vst"-anything and the
// explicit host-test name do. Pure for tests.
inline bool isHostHandledInsertName(const std::string& name) {
  std::string lowered;
  lowered.reserve(name.size());
  for (const char character : name) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  return lowered.find("vst") != std::string::npos || lowered.find("host") != std::string::npos;
}

// ---------------------------------------------------------------------------
// P2c plugin selection. Insert-name convention:
//   "vst:<query>"          — select a REAL scanned plugin by class/plugin name
//   "vst:<bundle>/<class>" — disambiguate shell bundles (Waves) explicitly
//   "vst", "Host Test..."  — legacy: the host's -6dB test processor
// All matching is case-insensitive; pure functions for tests.
// ---------------------------------------------------------------------------
inline std::string pluginScanToLowerAscii(const std::string& value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const char character : value) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
  }
  return lowered;
}

inline std::string pluginScanTrim(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
  return value.substr(begin, end - begin);
}

// "" when the insert name is not a "vst:" selection (legacy names keep the
// test processor; built-ins never reach here).
inline std::string vstSelectionQueryFromInsertName(const std::string& insertName) {
  const std::string trimmed = pluginScanTrim(insertName);
  if (trimmed.size() < 5) {
    return {};
  }
  const std::string prefix = pluginScanToLowerAscii(trimmed.substr(0, 4));
  if (prefix != "vst:") {
    return {};
  }
  return pluginScanTrim(trimmed.substr(4));
}

struct VstInsertSelection {
  bool resolved = false;
  std::string bundleId;   // scan id (bundle path) to load in the host
  std::string className;  // class inside the bundle ("" = host picks first audio class)
  std::string error;      // human-readable when !resolved
};

// Resolves a "vst:" query against the scan results. Match order: explicit
// bundle/class split, exact class name, class-name substring, plugin-name
// match (first class wins). probe=="fail" plugins are never selectable.
inline VstInsertSelection resolveVstInsertSelection(const std::string& query,
                                                    const std::vector<PluginHostPluginInfo>& plugins) {
  VstInsertSelection selection;
  const std::string trimmed = pluginScanTrim(query);
  if (trimmed.empty()) {
    selection.error = "empty vst: selection";
    return selection;
  }
  if (plugins.empty()) {
    selection.error = "no VST3 scan results (run a plugin scan first)";
    return selection;
  }

  const auto selectable = [](const PluginHostPluginInfo& plugin) { return plugin.probe != "fail"; };
  const auto matches = [](const std::string& loweredHaystack, const std::string& loweredNeedle) {
    return loweredHaystack.find(loweredNeedle) != std::string::npos;
  };

  // Explicit "bundle/class" form.
  const auto slash = trimmed.find('/');
  if (slash != std::string::npos) {
    const std::string bundlePart = pluginScanToLowerAscii(pluginScanTrim(trimmed.substr(0, slash)));
    const std::string classPart = pluginScanToLowerAscii(pluginScanTrim(trimmed.substr(slash + 1)));
    for (const auto& plugin : plugins) {
      if (!selectable(plugin)) continue;
      if (!matches(pluginScanToLowerAscii(plugin.name), bundlePart) &&
          !matches(pluginScanToLowerAscii(plugin.id), bundlePart)) {
        continue;
      }
      for (const auto& className : plugin.classNames) {
        if (pluginScanToLowerAscii(className) == classPart ||
            matches(pluginScanToLowerAscii(className), classPart)) {
          selection.resolved = true;
          selection.bundleId = plugin.id;
          selection.className = className;
          return selection;
        }
      }
    }
    selection.error = "no scanned plugin matches '" + trimmed + "'";
    return selection;
  }

  const std::string wanted = pluginScanToLowerAscii(trimmed);
  // Pass 1: exact class-name match.
  for (const auto& plugin : plugins) {
    if (!selectable(plugin)) continue;
    for (const auto& className : plugin.classNames) {
      if (pluginScanToLowerAscii(className) == wanted) {
        selection.resolved = true;
        selection.bundleId = plugin.id;
        selection.className = className;
        return selection;
      }
    }
  }
  // Pass 2: class-name substring.
  for (const auto& plugin : plugins) {
    if (!selectable(plugin)) continue;
    for (const auto& className : plugin.classNames) {
      if (matches(pluginScanToLowerAscii(className), wanted)) {
        selection.resolved = true;
        selection.bundleId = plugin.id;
        selection.className = className;
        return selection;
      }
    }
  }
  // Pass 3: plugin (bundle) name — first known class, or let the host pick.
  for (const auto& plugin : plugins) {
    if (!selectable(plugin)) continue;
    if (pluginScanToLowerAscii(plugin.name) == wanted ||
        matches(pluginScanToLowerAscii(plugin.name), wanted)) {
      selection.resolved = true;
      selection.bundleId = plugin.id;
      selection.className = plugin.classNames.empty() ? std::string{} : plugin.classNames.front();
      return selection;
    }
  }
  selection.error = "no scanned plugin matches '" + trimmed + "'";
  return selection;
}

// P2a: one probe verdict per plugin, parsed from the host's probe-result line.
struct PluginProbeResult {
  bool pass = false;
  std::string vendor;
  std::string className;
  std::vector<std::string> classNames;  // P2c: all audio classes in the bundle
  std::string reason;  // first failure reason, "" when passing
  bool parsed = false;
};

inline PluginProbeResult parsePluginProbeResult(const std::string& output) {
  PluginProbeResult result;
  std::istringstream stream(output);
  std::string line;
  while (std::getline(stream, line)) {
    if (line.empty() || line.front() != '{') {
      continue;
    }
    const auto parsed = rpc::Json::parse(line);
    if (!parsed.has_value() || parsed->getString("cmd") != "probe-result") {
      continue;
    }
    result.parsed = true;
    result.pass = parsed->get("pass") != nullptr && parsed->get("pass")->asBool();
    result.vendor = parsed->getString("vendor");
    result.className = parsed->getString("className");
    if (const rpc::Json* classNames = parsed->get("classNames");
        classNames != nullptr && classNames->isArray()) {
      for (const auto& entry : classNames->asArray()) {
        if (!entry.asString().empty()) {
          result.classNames.push_back(entry.asString());
        }
      }
    }
    if (const rpc::Json* reasons = parsed->get("reasons");
        reasons != nullptr && reasons->isArray() && !reasons->asArray().empty()) {
      result.reason = reasons->asArray().front().asString();
    }
    return result;
  }
  // No probe-result line: the host process died mid-probe (plugin crashed on
  // load). That IS the verdict — the isolation caught it.
  result.reason = "plugin crashed the probe process";
  return result;
}

// Runs `<exePath> --scan`, returning raw stdout ("" on launch failure).
// SECURITY: exePath derives from an environment variable, so this must NEVER
// go through a shell (std::system/popen would be command injection — CodeQL
// cpp/command-line-injection). The process is spawned directly with stdout on
// an anonymous pipe: the path is an argv element, not shell input, and no
// temp files are involved. Callers run this OFF the lock domains.
#ifdef _WIN32
inline std::string runPluginHostProcess(const std::string& exePath, const std::vector<std::string>& args) {
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
  std::string commandLine = "\"" + exePath + "\"";
  for (const auto& arg : args) {
    commandLine += " \"" + arg + "\"";
  }
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
                       : (output.find("\"cmd\":\"") != std::string::npos ? output : std::string{});
}

inline std::string runPluginHostScan(const std::string& exePath) {
  return runPluginHostProcess(exePath, {"--scan"});
}
#else
inline std::string runPluginHostProcess(const std::string& exePath, const std::vector<std::string>& args) {
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
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>("corevideo-plugin-host"));
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(exePath.c_str(), argv.data());
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
                   : (output.find("\"cmd\":\"") != std::string::npos ? output : std::string{});
}

inline std::string runPluginHostScan(const std::string& exePath) {
  return runPluginHostProcess(exePath, {"--scan"});
}
#endif

}  // namespace corevideo::core
