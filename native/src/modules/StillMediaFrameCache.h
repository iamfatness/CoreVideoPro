#pragma once

// Still-image media routes (logos/bugs — the POS-2 overlay case): scene routes
// that reference an image asset used to composite the colorFromParticipantId
// placeholder forever, because no consumer ever published a VideoFrame keyed
// "media:<assetId>". This cache closes that gap for STILL images:
//
//   scene sync (program OR preview) --> setDesired({"media:<assetId>", path})
//   background worker               --> stat + decode ONCE (WIC on Windows),
//                                       cached by (path, file version)
//   render gather (under coreMutex) --> collectFrames(): persistent BGRA
//                                       VideoFrames, zero per-tick pixel work
//
// THREADING LAWS honored by construction:
//   - setDesired/collectFrames/warnings only take the cache's own leaf mutex_
//     briefly (no file I/O, no pixel work) — safe at the sanctioned coreMutex
//     call sites.
//   - The worker thread NEVER takes coreMutex (mirrors startPluginHostScan);
//     all file I/O + decode + downscale happens there with mutex_ RELEASED.
//   - Published frames share one immutable pixel buffer (shared_ptr) with a
//     stable frameId, so the GPU compositor's per-source texture cache uploads
//     the still exactly once and every later tick is a cache hit.
//
// Failure honesty: a missing file / failed decode keeps the placeholder and
// emits a rate-limited (5s/key) stderr warning naming the path; the current
// failure set is also surfaced via warnings() so the render plan / snapshot
// diagnostics carry it. Missing files are re-checked every ~5s so an asset
// that appears later self-heals without a scene re-sync.
//
// Alpha: decoded as STRAIGHT (non-premultiplied) 32bpp BGRA. Both compositors
// blend video layers with straight source alpha (D3D11 blend state
// SRC_ALPHA/INV_SRC_ALPHA + textured shader emitting sampled.a * layer.a; the
// CPU preview blendPixelBgra uses the source alpha too), so transparent PNG
// logos composite correctly on program, preview and multiview.

#include "modules/Interfaces.h"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace corevideo::modules {

struct StillImagePixels {
  int width = 0;
  int height = 0;
  // Tightly packed straight-alpha BGRA, stride = width * 4.
  std::shared_ptr<const std::vector<uint8_t>> bgra;
};

// Injectable decode seam so the cache is testable everywhere (WIC is
// Windows-only): tests inject a fake; production uses the platform decoder.
class IStillImageDecoder {
 public:
  virtual ~IStillImageDecoder() = default;
  // Content-version token for the file (mtime + size folded together), or
  // nullopt when the file is missing/unreadable. Cheap stat — no decode.
  virtual std::optional<uint64_t> fileVersion(const std::string& path) = 0;
  // Decodes the file to straight-alpha BGRA. Returns false + `error` on failure.
  virtual bool decode(const std::string& path, StillImagePixels& out, std::string& error) = 0;
};

// WIC-backed decoder on Windows (CreateDecoderFromFilename -> 32bppBGRA ->
// CopyPixels); on other platforms fileVersion still works but decode fails
// with a clear "unsupported platform" error (loud, never silent).
std::unique_ptr<IStillImageDecoder> createPlatformStillImageDecoder();

// Strips a file:/// URI prefix into a plain Windows path (mirrors the media
// frame adapter's normalization so both consumers key the same file).
std::string normalizeMediaAssetPath(std::string path);

// Extension-based still test: .png/.jpg/.jpeg/.bmp/.gif/.tif/.tiff.
bool isStillImagePath(const std::string& path);

// A media asset is a still when its kind says "image" OR its path carries a
// still-image extension regardless of kind — the shell's media bin classifies
// PNG logos under "lower-third" groups, so kind alone is not trustworthy.
bool isStillImageMediaAsset(const std::string& mediaAssetKind, const std::string& mediaAssetPath);

class StillMediaFrameCache {
 public:
  // Decoded stills larger than program max are downscaled during decode (a
  // 100MP photo must not become a 400MB persistent frame).
  static constexpr int kMaxStillWidth = 3840;
  static constexpr int kMaxStillHeight = 2160;
  static constexpr size_t kDefaultCacheBudgetBytes = 64ull * 1024ull * 1024ull;

  explicit StillMediaFrameCache(std::unique_ptr<IStillImageDecoder> decoder = createPlatformStillImageDecoder(),
                                size_t cacheBudgetBytes = kDefaultCacheBudgetBytes);
  ~StillMediaFrameCache();

  StillMediaFrameCache(const StillMediaFrameCache&) = delete;
  StillMediaFrameCache& operator=(const StillMediaFrameCache&) = delete;

  struct StillRequest {
    std::string sourceKey;  // the frame key layers match, e.g. "media:<assetId>"
    std::string path;       // normalized filesystem path
  };

  // Replaces the desired still set (call on scene sync — program AND preview
  // route changes). Cheap: leaf mutex + worker notify; no file I/O here.
  void setDesired(std::vector<StillRequest> desired);

  // Returns one persistent VideoFrame per desired still whose decode has
  // completed. Cheap: shared_ptr copies of the cached buffers — ZERO per-tick
  // pixel copies; stable frameId so texture caches dedup uploads.
  [[nodiscard]] std::vector<VideoFrame> collectFrames(int64_t timestampMs);

  // Current decode/stat failures, one human-readable line per failing key
  // (mirrors IMediaFrameSource::warnings() so the render plan can carry them).
  [[nodiscard]] std::vector<std::string> warnings() const;

  // Blocks until the worker has processed every setDesired issued so far (or
  // the timeout elapses). Test/diagnostic helper; returns true when idle.
  bool waitForIdle(int timeoutMs) const;

 private:
  struct CachedImage {
    StillImagePixels image;
    uint64_t version = 0;
    int64_t frameId = 0;
    size_t bytes = 0;
    uint64_t lastUseTick = 0;
  };
  struct DesiredEntry {
    std::string path;
    bool checked = false;  // worker concluded for the current path (bound or failed)
    bool missingFile = false;  // stat failed — re-check periodically (self-heal)
    std::shared_ptr<const CachedImage> bound;
    std::string failure;  // non-empty => this key currently has no frame, and why
  };

  void ensureWorkerLocked();
  void workerLoop();
  // Rate-limited (5s/key) stderr warning. Caller holds mutex_.
  void warnRateLimitedLocked(const std::string& key, const std::string& message);
  // Evicts least-recently-used cache entries not currently desired until the
  // byte budget holds. Caller holds mutex_.
  void enforceBudgetLocked();

  std::unique_ptr<IStillImageDecoder> decoder_;
  const size_t cacheBudgetBytes_;

  mutable std::mutex mutex_;
  mutable std::condition_variable workerCv_;
  mutable std::condition_variable idleCv_;
  bool stop_ = false;
  bool workerStarted_ = false;
  std::thread worker_;
  uint64_t pendingGeneration_ = 0;
  uint64_t processedGeneration_ = 0;
  uint64_t useTick_ = 0;
  int64_t nextFrameId_ = 1;
  size_t cacheBytes_ = 0;
  std::map<std::string, DesiredEntry> desired_;                 // key = sourceKey
  std::map<std::string, std::shared_ptr<CachedImage>> cache_;   // key = path
  std::map<std::string, int64_t> lastWarnMs_;
};

}  // namespace corevideo::modules
