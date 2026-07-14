#include "modules/StillMediaFrameCache.h"

#include "modules/ImageResize.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincodec.h>
#endif

namespace corevideo::modules {
namespace {

std::string lowercaseAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

int64_t steadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

#if defined(_WIN32)
std::wstring widenUtf8(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  if (count <= 0) {
    return {};
  }
  std::wstring result(static_cast<size_t>(count - 1), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), count);
  return result;
}
#endif

// Portable stat half of the decoder (std::filesystem); the decode half is
// WIC and therefore Windows-only.
class PlatformStillImageDecoder final : public IStillImageDecoder {
 public:
  std::optional<uint64_t> fileVersion(const std::string& path) override {
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(path, ec);
    if (ec) {
      return std::nullopt;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
      return std::nullopt;
    }
    uint64_t version = static_cast<uint64_t>(writeTime.time_since_epoch().count());
    version ^= static_cast<uint64_t>(size) + 0x9e3779b97f4a7c15ull + (version << 6) + (version >> 2);
    return version;
  }

  bool decode(const std::string& path, StillImagePixels& out, std::string& error) override {
#if defined(_WIN32)
    // Runs on the cache's worker thread: pair every CoInitializeEx with a
    // CoUninitialize (S_FALSE = already initialized on this thread — still
    // needs the balancing call; RPC_E_CHANGED_MODE must not be balanced).
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool balanceCom = SUCCEEDED(co);
    bool ok = false;
    {
      IWICImagingFactory* factory = nullptr;
      IWICBitmapDecoder* decoder = nullptr;
      IWICBitmapFrameDecode* frame = nullptr;
      IWICFormatConverter* converter = nullptr;
      do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) {
          error = "WIC imaging factory unavailable";
          break;
        }
        if (FAILED(factory->CreateDecoderFromFilename(widenUtf8(path).c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &decoder))) {
          error = "WIC could not open/decode the file";
          break;
        }
        if (FAILED(decoder->GetFrame(0, &frame))) {
          error = "WIC could not read frame 0";
          break;
        }
        UINT width = 0;
        UINT height = 0;
        if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
          error = "WIC reported an empty image";
          break;
        }
        if (FAILED(factory->CreateFormatConverter(&converter))) {
          error = "WIC format converter unavailable";
          break;
        }
        // STRAIGHT alpha (32bppBGRA, not PBGRA): the compositor's video-layer
        // blend is SRC_ALPHA/INV_SRC_ALPHA — premultiplied content would
        // double-multiply and darken transparent logo edges.
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                         nullptr, 0.f, WICBitmapPaletteTypeCustom))) {
          error = "WIC could not convert to 32bpp BGRA";
          break;
        }
        const int stride = static_cast<int>(width) * 4;
        auto pixels = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(stride) *
                                                             static_cast<size_t>(height));
        if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                         static_cast<UINT>(pixels->size()), pixels->data()))) {
          error = "WIC CopyPixels failed";
          break;
        }
        out.width = static_cast<int>(width);
        out.height = static_cast<int>(height);
        out.bgra = std::move(pixels);
        ok = true;
      } while (false);
      if (converter) converter->Release();
      if (frame) frame->Release();
      if (decoder) decoder->Release();
      if (factory) factory->Release();
    }
    if (balanceCom) {
      CoUninitialize();
    }
    return ok;
#else
    (void)path;
    (void)out;
    error = "no still-image decoder on this platform (WIC is Windows-only)";
    return false;
#endif
  }
};

}  // namespace

std::unique_ptr<IStillImageDecoder> createPlatformStillImageDecoder() {
  return std::make_unique<PlatformStillImageDecoder>();
}

std::string normalizeMediaAssetPath(std::string path) {
  constexpr const char* prefix = "file:///";
  if (path.rfind(prefix, 0) == 0) {
    path = path.substr(std::strlen(prefix));
    std::replace(path.begin(), path.end(), '/', '\\');
  }
  return path;
}

bool isStillImagePath(const std::string& path) {
  const auto lower = lowercaseAscii(path);
  return lower.ends_with(".png") || lower.ends_with(".jpg") || lower.ends_with(".jpeg") ||
         lower.ends_with(".bmp") || lower.ends_with(".gif") || lower.ends_with(".tif") ||
         lower.ends_with(".tiff");
}

bool isStillImageMediaAsset(const std::string& mediaAssetKind, const std::string& mediaAssetPath) {
  if (lowercaseAscii(mediaAssetKind) == "image") {
    return true;
  }
  return isStillImagePath(normalizeMediaAssetPath(mediaAssetPath));
}

StillMediaFrameCache::StillMediaFrameCache(std::unique_ptr<IStillImageDecoder> decoder,
                                           size_t cacheBudgetBytes)
    : decoder_(std::move(decoder)), cacheBudgetBytes_(cacheBudgetBytes) {}

StillMediaFrameCache::~StillMediaFrameCache() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  workerCv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void StillMediaFrameCache::setDesired(std::vector<StillRequest> desired) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    std::map<std::string, DesiredEntry> next;
    for (auto& request : desired) {
      if (request.sourceKey.empty() || request.path.empty()) {
        continue;
      }
      auto existing = desired_.find(request.sourceKey);
      if (existing != desired_.end() && existing->second.path == request.path) {
        // Same key + path: keep the bound frame/failure. Re-stat on the worker
        // pass below (cheap; catches an edited file) — never a blind re-decode.
        DesiredEntry entry = std::move(existing->second);
        entry.checked = false;
        next.emplace(request.sourceKey, std::move(entry));
      } else {
        DesiredEntry entry;
        entry.path = std::move(request.path);
        next.emplace(std::move(request.sourceKey), std::move(entry));
      }
    }
    desired_ = std::move(next);
    ++pendingGeneration_;
    if (!desired_.empty()) {
      ensureWorkerLocked();
    } else {
      // Nothing to resolve: an empty set is trivially processed.
      processedGeneration_ = pendingGeneration_;
      idleCv_.notify_all();
    }
  }
  workerCv_.notify_all();
}

std::vector<VideoFrame> StillMediaFrameCache::collectFrames(int64_t timestampMs) {
  std::vector<VideoFrame> frames;
  std::lock_guard<std::mutex> lock(mutex_);
  ++useTick_;
  for (auto& [sourceKey, entry] : desired_) {
    if (!entry.bound || !entry.bound->image.bgra) {
      continue;
    }
    const auto& image = entry.bound->image;
    std::const_pointer_cast<CachedImage>(entry.bound)->lastUseTick = useTick_;
    VideoFrame frame;
    frame.participantId = sourceKey;
    frame.width = image.width;
    frame.height = image.height;
    frame.naturalWidth = image.width;
    frame.naturalHeight = image.height;
    frame.timestampMs = timestampMs;
    frame.pixels = image.bgra;  // shared, immutable — zero pixel copies per tick
    frame.pixelWidth = image.width;
    frame.pixelHeight = image.height;
    frame.pixelStride = image.width * 4;
    frame.frameId = entry.bound->frameId;  // stable => texture caches dedup uploads
    frames.push_back(std::move(frame));
  }
  return frames;
}

std::vector<std::string> StillMediaFrameCache::warnings() const {
  std::vector<std::string> result;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [sourceKey, entry] : desired_) {
    if (!entry.failure.empty()) {
      result.push_back("Still media " + sourceKey + " (" + entry.path + "): " + entry.failure);
    }
  }
  return result;
}

bool StillMediaFrameCache::waitForIdle(int timeoutMs) const {
  std::unique_lock<std::mutex> lock(mutex_);
  return idleCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
    return processedGeneration_ == pendingGeneration_;
  });
}

void StillMediaFrameCache::ensureWorkerLocked() {
  if (workerStarted_) {
    return;
  }
  workerStarted_ = true;
  worker_ = std::thread([this] { workerLoop(); });
}

void StillMediaFrameCache::warnRateLimitedLocked(const std::string& key, const std::string& message) {
  const int64_t now = steadyNowMs();
  int64_t& last = lastWarnMs_[key];
  if (last != 0 && now - last < 5000) {
    return;
  }
  last = now;
  std::fprintf(stderr, "[still-media] %s\n", message.c_str());
}

void StillMediaFrameCache::enforceBudgetLocked() {
  if (cacheBytes_ <= cacheBudgetBytes_) {
    return;
  }
  std::set<std::string> desiredPaths;
  for (const auto& [sourceKey, entry] : desired_) {
    desiredPaths.insert(entry.path);
  }
  while (cacheBytes_ > cacheBudgetBytes_) {
    auto victim = cache_.end();
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
      if (desiredPaths.count(it->first) != 0) {
        continue;  // never evict a still an active scene route references
      }
      if (victim == cache_.end() || it->second->lastUseTick < victim->second->lastUseTick) {
        victim = it;
      }
    }
    if (victim == cache_.end()) {
      warnRateLimitedLocked("cache-over-budget",
                            "still cache over budget (" + std::to_string(cacheBytes_) + " > " +
                                std::to_string(cacheBudgetBytes_) +
                                " bytes) but every entry is scene-referenced; keeping all");
      return;
    }
    cacheBytes_ -= std::min(cacheBytes_, victim->second->bytes);
    std::fprintf(stderr,
                 "[still-media] evicting cached still '%s' (%zu bytes, LRU) — cache over %zu-byte budget\n",
                 victim->first.c_str(), victim->second->bytes, cacheBudgetBytes_);
    cache_.erase(victim);
  }
}

void StillMediaFrameCache::workerLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    const bool haveMissing = std::any_of(desired_.begin(), desired_.end(), [](const auto& pair) {
      return pair.second.missingFile;
    });
    const bool haveWork = pendingGeneration_ != processedGeneration_ ||
                          std::any_of(desired_.begin(), desired_.end(), [](const auto& pair) {
                            return !pair.second.checked;
                          });
    if (!haveWork) {
      if (haveMissing) {
        // A referenced file may appear later (asset copied in after the scene
        // was built): re-stat every ~5s so it self-heals without a re-sync.
        workerCv_.wait_for(lock, std::chrono::seconds(5));
        for (auto& [sourceKey, entry] : desired_) {
          if (entry.missingFile) {
            entry.checked = false;
          }
        }
      } else {
        workerCv_.wait(lock);
      }
      continue;
    }

    const uint64_t generation = pendingGeneration_;
    // Snapshot one unresolved entry at a time so a mid-pass setDesired can
    // retarget without waiting for a long decode batch.
    std::string workKey;
    std::string workPath;
    for (auto& [sourceKey, entry] : desired_) {
      if (!entry.checked) {
        workKey = sourceKey;
        workPath = entry.path;
        break;
      }
    }
    if (workKey.empty()) {
      processedGeneration_ = std::max(processedGeneration_, generation);
      idleCv_.notify_all();
      continue;
    }

    // ---- File I/O + decode with mutex_ RELEASED (never under any lock) ----
    lock.unlock();
    std::optional<uint64_t> version = decoder_ ? decoder_->fileVersion(workPath) : std::nullopt;
    StillImagePixels decoded;
    std::string decodeError;
    bool decodeOk = false;
    bool cacheHit = false;
    if (version) {
      {
        std::lock_guard<std::mutex> peek(mutex_);
        auto cached = cache_.find(workPath);
        if (cached != cache_.end() && cached->second->version == *version) {
          cacheHit = true;  // decode-once: scene re-syncs bind the cached image
        }
      }
      if (!cacheHit) {
        decodeOk = decoder_ && decoder_->decode(workPath, decoded, decodeError);
        if (decodeOk && decoded.bgra &&
            (decoded.width > kMaxStillWidth || decoded.height > kMaxStillHeight)) {
          // Size guard: keep the still within the program max (aspect preserved).
          const double scale = std::min(static_cast<double>(kMaxStillWidth) / decoded.width,
                                        static_cast<double>(kMaxStillHeight) / decoded.height);
          const int scaledWidth = std::max(1, static_cast<int>(decoded.width * scale));
          const int scaledHeight = std::max(1, static_cast<int>(decoded.height * scale));
          auto scaled = std::make_shared<std::vector<uint8_t>>();
          if (resizeBgraBilinear(decoded.bgra->data(), decoded.width, decoded.height, scaledWidth,
                                 scaledHeight, *scaled)) {
            std::fprintf(stderr,
                         "[still-media] downscaled oversized still '%s' %dx%d -> %dx%d\n",
                         workPath.c_str(), decoded.width, decoded.height, scaledWidth, scaledHeight);
            decoded.width = scaledWidth;
            decoded.height = scaledHeight;
            decoded.bgra = std::move(scaled);
          }
        }
      }
    }
    lock.lock();
    // ---- Publish under mutex_ (cheap map/pointer writes only) ----
    auto entryIt = desired_.find(workKey);
    if (entryIt == desired_.end() || entryIt->second.path != workPath) {
      continue;  // retargeted while we worked; the new entry is still unchecked
    }
    DesiredEntry& entry = entryIt->second;
    entry.checked = true;
    if (!version) {
      entry.bound.reset();
      entry.missingFile = true;
      entry.failure = "file missing or unreadable — keeping placeholder";
      warnRateLimitedLocked(workKey, "still media " + workKey + " file missing or unreadable: '" +
                                         workPath + "' (placeholder stays)");
      continue;
    }
    entry.missingFile = false;
    auto cached = cache_.find(workPath);
    if (cached != cache_.end() && cached->second->version == *version) {
      entry.bound = cached->second;
      entry.failure.clear();
      cached->second->lastUseTick = ++useTick_;
      continue;
    }
    if (!decodeOk || !decoded.bgra) {
      entry.bound.reset();
      entry.failure = decodeError.empty() ? "decode failed — keeping placeholder" : decodeError;
      warnRateLimitedLocked(workKey, "still media " + workKey + " decode FAILED for '" + workPath +
                                         "': " + entry.failure);
      continue;
    }
    auto image = std::make_shared<CachedImage>();
    image->image = std::move(decoded);
    image->version = *version;
    image->frameId = nextFrameId_++;
    image->bytes = image->image.bgra->size();
    image->lastUseTick = ++useTick_;
    if (cached != cache_.end()) {
      cacheBytes_ -= std::min(cacheBytes_, cached->second->bytes);
      cached->second = image;
    } else {
      cache_.emplace(workPath, image);
    }
    cacheBytes_ += image->bytes;
    enforceBudgetLocked();
    entry.bound = std::move(image);
    entry.failure.clear();
  }
}

}  // namespace corevideo::modules
