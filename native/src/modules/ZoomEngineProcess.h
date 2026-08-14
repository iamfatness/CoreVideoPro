#pragma once

#include "modules/ZoomEngineClient.h"

#include <optional>
#include <string>

namespace corevideo::modules {

struct ZoomEngineProcessOptions {
  std::string executablePath;
  int connectTimeoutMs = 30000;
  // Per-instance IPC token. Spliced into the engine's pipe/socket/SHM names and
  // passed to the engine on its command line so this app instance never collides
  // with another process on the shared "ZoomObsPlugin_" base name (chiefly the
  // OBS zoom plugin). Empty => the client generates one in start().
  std::string instanceToken;
};

// Builds a process-unique IPC token (host pid + a monotonic spawn counter) so
// repeated engine spawns within one app, separate app instances, and the OBS
// zoom plugin all get distinct pipe/socket/SHM names.
std::string makeZoomEngineInstanceToken();

// The pipe/subprocess client for the Zoom engine helper. The I/O entry points are
// virtual so ZoomEngineRuntime's outbound sender thread (phase 2 increment 3) can be
// exercised in unit tests with a fake client — no real subprocess, controllable
// blocking/failing sendLine.
class ZoomEngineProcessClient {
 public:
  ZoomEngineProcessClient() = default;
  virtual ~ZoomEngineProcessClient();

  ZoomEngineProcessClient(const ZoomEngineProcessClient&) = delete;
  ZoomEngineProcessClient& operator=(const ZoomEngineProcessClient&) = delete;

  // REQUIRES DEV MACHINE: launches the vendored Zoom Meeting SDK helper and
  // connects to its pipe/socket IPC. This is not used by the default stub path.
  bool start(const ZoomEngineProcessOptions& options);
  virtual void stop();
  [[nodiscard]] virtual bool running() const;
  [[nodiscard]] virtual std::string lastError() const;

  virtual bool sendLine(const std::string& line);
  [[nodiscard]] std::optional<std::string> readLine();
  [[nodiscard]] virtual std::optional<ZoomEngineEvent> readEvent();

  // The instance token this client launched the engine with (see
  // ZoomEngineProcessOptions::instanceToken). Read back by ZoomEngineRuntime to
  // derive matching SHM region names. Empty until start() succeeds.
  [[nodiscard]] const std::string& instanceToken() const { return instanceToken_; }

 private:
  bool connectIpc(int timeoutMs);
  void closeIpc();
  void setError(std::string message);

#if defined(_WIN32)
  void* processHandle_ = nullptr;
  void* threadHandle_ = nullptr;
  // Kill-on-close job ownership guarantees the Meeting SDK helper cannot
  // outlive a crashed/force-terminated media core and retain Zoom callbacks.
  void* jobHandle_ = nullptr;
  void* parentToEngine_ = nullptr;
  void* engineToParent_ = nullptr;
#else
  int processId_ = -1;
  int parentToEngine_ = -1;
  int engineToParent_ = -1;
#endif
  std::string instanceToken_;
  std::string lastError_;
};

}  // namespace corevideo::modules
