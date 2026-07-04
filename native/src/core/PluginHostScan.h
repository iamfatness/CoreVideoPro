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
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

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

// Runs `<exePath> --scan`, returning raw stdout ("" on launch failure). Uses a
// temp-file redirect for portability; callers run this OFF the lock domains.
inline std::string runPluginHostScan(const std::string& exePath) {
  if (exePath.empty()) {
    return {};
  }
  std::string tempPath;
#ifdef _WIN32
  const char* tempDir = std::getenv("TEMP");
  tempPath = std::string(tempDir != nullptr ? tempDir : ".") + "\\corevideo-vst-scan.jsonl";
#else
  tempPath = "/tmp/corevideo-vst-scan.jsonl";
#endif
  const std::string command = "\"" + exePath + "\" --scan > \"" + tempPath + "\" 2>&1";
#ifdef _WIN32
  // std::system routes through cmd.exe, which needs the WHOLE command quoted
  // again when the exe path itself is quoted.
  const int exitCode = std::system(("\"" + command + "\"").c_str());
#else
  const int exitCode = std::system(command.c_str());
#endif
  std::ifstream file(tempPath, std::ios::binary);
  std::string output;
  if (file) {
    std::ostringstream buffer;
    buffer << file.rdbuf();
    output = buffer.str();
  }
  std::remove(tempPath.c_str());
  return exitCode == 0 ? output : (output.find("\"cmd\":\"plugin\"") != std::string::npos ? output : std::string{});
}

}  // namespace corevideo::core
