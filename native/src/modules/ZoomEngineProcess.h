#pragma once

#include "modules/ZoomEngineClient.h"

#include <optional>
#include <string>

namespace corevideo::modules {

struct ZoomEngineProcessOptions {
  std::string executablePath;
  int connectTimeoutMs = 30000;
};

class ZoomEngineProcessClient {
 public:
  ZoomEngineProcessClient() = default;
  ~ZoomEngineProcessClient();

  ZoomEngineProcessClient(const ZoomEngineProcessClient&) = delete;
  ZoomEngineProcessClient& operator=(const ZoomEngineProcessClient&) = delete;

  // REQUIRES DEV MACHINE: launches the vendored Zoom Meeting SDK helper and
  // connects to its pipe/socket IPC. This is not used by the default stub path.
  bool start(const ZoomEngineProcessOptions& options);
  void stop();
  [[nodiscard]] bool running() const;
  [[nodiscard]] const std::string& lastError() const;

  bool sendLine(const std::string& line);
  [[nodiscard]] std::optional<std::string> readLine();
  [[nodiscard]] std::optional<ZoomEngineEvent> readEvent();

 private:
  bool connectIpc(int timeoutMs);
  void closeIpc();
  void setError(std::string message);

#if defined(_WIN32)
  void* processHandle_ = nullptr;
  void* threadHandle_ = nullptr;
  void* parentToEngine_ = nullptr;
  void* engineToParent_ = nullptr;
#else
  int processId_ = -1;
  int parentToEngine_ = -1;
  int engineToParent_ = -1;
#endif
  std::string lastError_;
};

}  // namespace corevideo::modules
