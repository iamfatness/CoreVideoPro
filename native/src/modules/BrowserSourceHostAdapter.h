#pragma once

// Browser sources (docs/capture-sources-spec.md Phase BR, BR-1 render-only).
//
// Each browser source is a dedicated corevideo-browser-host.exe subprocess (the
// zoom-engine isolation pattern: untrusted page content NEVER runs in the core).
// The host renders the URL offscreen via WebView2 and publishes BGRA frames into a
// named shared-memory seqlock buffer (BrowserSourceShm.h — the SAME layout the WinUI
// capture bridge uses), which this adapter polls each render tick and emits as
// VideoFrames keyed "capture:browser:<n>" — so browser sources ride the existing
// capture ingest seam: compositor, routing, multiview and scenes need zero changes.
//
// Supervision (loud failures, never silent): a lazily-started supervisor thread
// watches every host process. Host death -> the source goes "failed" with a LOUD
// stderr warning, the last frame is re-served for 2 s (then the compositor's slate
// takes over), and the host is respawned with capped exponential backoff
// (BrowserHostRestartPolicy: 5->10->20->40->60 s, give up after 5 consecutive
// failures until an operator reload). Spawns run OFF the adapter mutex so the
// render thread's pollVideoFrames can never block behind CreateProcess.
//
// House rule: processes are spawned with direct CreateProcessA (argument vector
// built + validated here) — never std::system / shell strings.

#include "modules/BrowserHostRestartPolicy.h"
#include "modules/Interfaces.h"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace corevideo::modules {

// Plain-data health/telemetry snapshot for one browser source (MediaCore turns
// this into the `browserSources` snapshot node).
struct BrowserSourceTelemetry {
  std::string id;      // "browser:<n>"
  std::string url;
  int width = 0;
  int height = 0;
  int fps = 0;              // commanded
  double measuredFps = 0;   // frames observed by the poll over the last window
  bool running = false;     // host process alive
  bool gaveUp = false;      // restart policy exhausted (operator reload required)
  int restartCount = 0;
  int consecutiveFailures = 0;
  std::string health;       // "starting" | "live" | "failed"
  std::string lastError;
  int64_t framesReceived = 0;
};

class BrowserSourceHostAdapter {
 public:
  // `hostExecutablePath` empty => resolve next to the running executable
  // (corevideo-browser-host.exe), overridable via COREVIDEO_BROWSER_HOST_PATH.
  explicit BrowserSourceHostAdapter(std::string hostExecutablePath = std::string());
  ~BrowserSourceHostAdapter();

  BrowserSourceHostAdapter(const BrowserSourceHostAdapter&) = delete;
  BrowserSourceHostAdapter& operator=(const BrowserSourceHostAdapter&) = delete;

  // Adds a browser source and schedules its host spawn on the supervisor thread
  // (sub-ms: never blocks on process creation). Returns the new source id
  // ("browser:<n>"), or an empty string with `error` set when the request is
  // invalid (unsupported scheme, embedded quotes/whitespace, bad dimensions).
  std::string addSource(const std::string& url, int width, int height, int fps, std::string& error);

  // Stops + removes a source. Returns false when the id is unknown.
  bool removeSource(const std::string& id);

  // Reloads the page: asks a live host to reload via its stdin command pipe; a
  // dead/gave-up host gets a fresh restart cycle (policy reset). Returns false
  // with `error` set when the id is unknown.
  bool reloadSource(const std::string& id, std::string& error);

  // Browser sources presented as capture devices (kind "browser") so they appear
  // in the Sources pickers exactly like screens do.
  [[nodiscard]] std::vector<CaptureDeviceInfo> enumerate() const;

  // Latest BGRA frame per source, keyed "capture:browser:<n>". Re-serves the last
  // held frame on ticks with no fresh SHM publish (the WinUi-bridge discipline);
  // stops emitting ~2 s after a host dies so the slate becomes visible.
  [[nodiscard]] std::vector<VideoFrame> pollVideoFrames(int64_t timestampMs);

  [[nodiscard]] std::vector<BrowserSourceTelemetry> telemetry() const;

  [[nodiscard]] bool empty() const;

  // URL/dimension validation shared with tests. Accepts http(s)/file/data URLs
  // with no whitespace, control characters, double quotes, or backslashes.
  static bool validateAddRequest(const std::string& url, int width, int height, int fps,
                                 std::string& error);

 private:
  struct Source {
    std::string id;
    std::string url;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    std::string shmName;
    uint64_t generation = 0;  // bumps on every (re)spawn commit; guards spawn races

    // Process (Win32 HANDLEs as void* so this header stays windows.h-free).
    void* processHandle = nullptr;
    void* stdinWrite = nullptr;

    // Shared-memory mapping (reader side).
    void* mappingHandle = nullptr;
    const uint8_t* view = nullptr;
    std::size_t mappedBytes = 0;
    uint32_t lastSequence = 0;
    int64_t lastMapAttemptMs = 0;

    // Latest held frame (re-served between SHM publishes).
    std::shared_ptr<std::vector<uint8_t>> lastPixels;
    int lastWidth = 0;
    int lastHeight = 0;
    int64_t frameId = 0;
    int64_t lastFrameAtMs = 0;

    // Supervision.
    BrowserHostRestartPolicy policy;
    int restartCount = 0;
    std::string lastError;
    bool spawnInFlight = false;

    // Measured-fps window.
    int64_t fpsWindowStartMs = 0;
    int64_t fpsWindowFrames = 0;
    double measuredFps = 0;
  };

  void ensureSupervisorStarted();
  void supervisorLoop();
  // Runs UNLOCKED. Returns true and fills the handles on success.
  bool spawnHost(const Source& snapshot, void*& processHandle, void*& stdinWrite,
                 std::string& error) const;
  static void closeProcessLocked(Source& source);
  static void closeMappingLocked(Source& source);
  void tryMapLocked(Source& source, int64_t nowMs);
  [[nodiscard]] static int64_t nowMs();

  std::string hostExecutablePath_;
  mutable std::mutex mutex_;  // leaf lock; never held across spawn/blocking work
  std::map<std::string, Source> sources_;
  int nextOrdinal_ = 1;

  std::thread supervisor_;
  std::condition_variable supervisorCv_;
  bool supervisorStarted_ = false;
  bool stopping_ = false;
};

std::unique_ptr<BrowserSourceHostAdapter> createBrowserSourceHostAdapter();

}  // namespace corevideo::modules
