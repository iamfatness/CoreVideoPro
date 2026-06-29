#pragma once

#include "modules/Interfaces.h"
#include "modules/ZoomEngineProcess.h"
#include "modules/ZoomEngineState.h"
#include "rpc/Json.h"
#include <chrono>
#include <map>
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
  // Returns the latest decoded BGRA frame per participant, carrying real pixels,
  // WITHOUT consuming the pending stdout/event queue (drainFrameEvents) that
  // feeds the WinUI multiview tiles. Used by the compositor tick to ingest real
  // pixels into RealZoomCaptureSource.
  [[nodiscard]] std::vector<VideoFrame> latestDecodedVideoFrames(int64_t timestampMs);
  [[nodiscard]] std::vector<VideoFrame> pollCompositorVideoFrames(int64_t timestampMs);
  [[nodiscard]] std::vector<AudioFrame> pollCompositorAudioFrames(int64_t timestampMs);

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
  void applyJoinCredentialsFromPayload(const rpc::Json& payload);
  [[nodiscard]] double runtimeElapsedMs() const;

  Config config_;
  std::unique_ptr<ZoomEngineProcessClient> process_;
  ZoomEngineRuntimeState state_;
  mutable std::mutex mutex_;
  std::thread reader_;
  bool readerRunning_ = false;
  bool initialized_ = false;
  bool mediaStarted_ = false;
  // Operator opted in to raw capture (Studio "Engine On"). Raw recording /
  // recording-rights request only starts once this is set, so it no longer
  // fires automatically on meeting join.
  bool captureRequested_ = false;
  // Last subscription set sent to the engine (sourceUuid -> resolution), so the
  // per-tick spine sync only sends a subscribe/unsubscribe command when the set
  // actually CHANGES. Previously it re-sent every subscription on every spine
  // tick, flooding the engine command pipe (process_->sendLine blocks once the
  // pipe buffer fills) and timing out the 4s spine round-trip — which jammed the
  // whole bridge (roster + multiview layout couldn't get through).
  std::map<std::string, int> sentSubscriptions_;
  int fallbackTick_ = 0;
  std::chrono::steady_clock::time_point startedAt_;
  std::vector<rpc::Json> pendingFrameEvents_;

  struct DecodedFrame {
    // Full-resolution I420 planes (Y + U + V tightly packed). The compositor
    // uploads these to the GPU and converts to RGB in-shader.
    std::shared_ptr<const std::vector<std::uint8_t>> i420;
    int width = 0;   // luma width
    int height = 0;  // luma height
    std::int64_t frameId = 0;
  };
  // Latest decoded I420 frame per participantId, tapped alongside the stdout
  // event queue so the compositor can read real pixels without draining the
  // multiview event path.
  std::map<std::string, DecodedFrame> latestDecodedFrames_;
};

}  // namespace corevideo::modules
