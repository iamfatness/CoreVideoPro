#pragma once

#include "modules/ZoomEngineProcess.h"
#include "modules/ZoomEngineState.h"
#include "rpc/Json.h"

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace corevideo::modules {

class ZoomEngineRuntime {
 public:
  ZoomEngineRuntime();
  ~ZoomEngineRuntime();

  ZoomEngineRuntime(const ZoomEngineRuntime&) = delete;
  ZoomEngineRuntime& operator=(const ZoomEngineRuntime&) = delete;

  [[nodiscard]] bool configured() const;
  [[nodiscard]] rpc::Json join(const rpc::Json& payload);
  [[nodiscard]] rpc::Json leave();
  [[nodiscard]] rpc::Json snapshot();
  [[nodiscard]] rpc::Json syncSpine(const rpc::Json& payload, double elapsedMs);
  [[nodiscard]] std::vector<rpc::Json> drainFrameEvents();

 private:
  struct Config {
    std::string executablePath;
    std::string sdkJwt;
    std::string publicAppKey;
    std::string passcode;
    std::string onBehalfToken;
    std::string userZak;
    std::string appPrivilegeToken;
    int connectTimeoutMs = 30000;
    int joinWaitMs = 7000;
  };

  [[nodiscard]] static Config loadConfig();
  [[nodiscard]] bool ensureStartedLocked();
  void startReaderLocked();
  void readerLoop();
  void applyEvent(const ZoomEngineEvent& event);
  void stopReader();
  [[nodiscard]] rpc::Json rawCaptureSnapshotLocked();
  [[nodiscard]] rpc::Json spineSnapshotLocked(const rpc::Json& payload, double elapsedMs);
  void enqueueFrameEventLocked(const ZoomEngineEvent& event);
  bool ensureMediaStartedLocked();

  Config config_;
  std::unique_ptr<ZoomEngineProcessClient> process_;
  ZoomEngineRuntimeState state_;
  mutable std::mutex mutex_;
  std::thread reader_;
  bool readerRunning_ = false;
  bool initialized_ = false;
  bool mediaStarted_ = false;
  int fallbackTick_ = 0;
  std::vector<rpc::Json> pendingFrameEvents_;
};

}  // namespace corevideo::modules
