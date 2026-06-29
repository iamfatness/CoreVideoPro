#include "WinUiCaptureDeviceAdapter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace corevideo::modules {

namespace {
// Shared-memory layout written by the WinUI shell:
//   [0] uint32 sequence   (seqlock: even = stable, odd = writer mid-update)
//   [4] uint32 width
//   [8] uint32 height
//   [12] uint32 reserved
//   [16..] tightly packed BGRA (width * height * 4)
constexpr std::size_t kHeaderBytes = 16;

inline uint32_t loadSeq(const uint8_t* p) {
  return *reinterpret_cast<const volatile uint32_t*>(p);
}

inline uint32_t loadU32(const uint8_t* p, std::size_t off) {
  uint32_t v = 0;
  std::memcpy(&v, p + off, sizeof(v));
  return v;
}
}  // namespace

WinUiCaptureDeviceAdapter::WinUiCaptureDeviceAdapter(std::unique_ptr<ICaptureDevice> inner)
    : inner_(std::move(inner)) {}

WinUiCaptureDeviceAdapter::~WinUiCaptureDeviceAdapter() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : buffers_) {
    closeBufferLocked(entry.second);
  }
  buffers_.clear();
}

void WinUiCaptureDeviceAdapter::closeBufferLocked(Buffer& buffer) {
#ifdef _WIN32
  if (buffer.view) {
    UnmapViewOfFile(buffer.view);
  }
  if (buffer.mappingHandle) {
    CloseHandle(static_cast<HANDLE>(buffer.mappingHandle));
  }
#endif
  buffer.view = nullptr;
  buffer.mappingHandle = nullptr;
}

void WinUiCaptureDeviceAdapter::registerCaptureBuffer(const std::string& deviceId,
                                                      const std::string& shmName,
                                                      int width,
                                                      int height) {
  if (deviceId.empty() || shmName.empty() || width <= 0 || height <= 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto& buffer = buffers_[deviceId];
  if (buffer.view && buffer.shmName == shmName && buffer.width == width && buffer.height == height) {
    return;  // already mapped at this size
  }

  closeBufferLocked(buffer);
  buffer = Buffer{};
  buffer.shmName = shmName;
  buffer.width = width;
  buffer.height = height;
  buffer.mappedBytes = kHeaderBytes + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;

#ifdef _WIN32
  const std::wstring wname(shmName.begin(), shmName.end());
  HANDLE handle = OpenFileMappingW(FILE_MAP_READ, FALSE, wname.c_str());
  if (!handle) {
    buffers_.erase(deviceId);
    return;
  }
  const void* view = MapViewOfFile(handle, FILE_MAP_READ, 0, 0, buffer.mappedBytes);
  if (!view) {
    CloseHandle(handle);
    buffers_.erase(deviceId);
    return;
  }
  buffer.mappingHandle = handle;
  buffer.view = static_cast<const uint8_t*>(view);
#endif
  std::fprintf(stderr, "[capture-shm] registered '%s' %dx%d shm='%s' mapped=%d\n",
               deviceId.c_str(), width, height, shmName.c_str(), buffer.view ? 1 : 0);
}

void WinUiCaptureDeviceAdapter::unregisterCaptureBuffer(const std::string& deviceId) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = buffers_.find(deviceId);
  if (it != buffers_.end()) {
    closeBufferLocked(it->second);
    buffers_.erase(it);
  }
}

std::vector<VideoFrame> WinUiCaptureDeviceAdapter::pollVideoFrames(int64_t timestampMs) {
  // Keep whatever the inner device produces (e.g. dev hardware test patterns).
  std::vector<VideoFrame> frames = inner_->pollVideoFrames(timestampMs);

  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& entry : buffers_) {
    const std::string& deviceId = entry.first;
    Buffer& buffer = entry.second;
    if (!buffer.view) {
      continue;
    }

    const uint8_t* base = buffer.view;
    const uint32_t s1 = loadSeq(base);
    // Try to pick up a NEW frame; if there isn't one this tick we fall through and
    // re-emit the last held frame (below) so the compositor never starves.
    if ((s1 & 1u) == 0 && s1 != buffer.lastSequence) {
      const uint32_t w = loadU32(base, 4);
      const uint32_t h = loadU32(base, 8);
      const std::size_t bgraBytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4;
      if (w != 0 && h != 0 && kHeaderBytes + bgraBytes <= buffer.mappedBytes) {
        auto pixels = std::make_shared<std::vector<uint8_t>>(base + kHeaderBytes, base + kHeaderBytes + bgraBytes);
        const uint32_t s2 = loadSeq(base);
        if (s2 == s1) {  // not torn
          buffer.lastSequence = s1;
          if (buffer.frameId == 0) {
            std::fprintf(stderr, "[capture-shm] first frame '%s' %ux%u (core now compositing real capture pixels)\n",
                         deviceId.c_str(), w, h);
          }
          ++buffer.frameId;
          buffer.lastPixels = std::move(pixels);
          buffer.lastWidth = static_cast<int>(w);
          buffer.lastHeight = static_cast<int>(h);
        }
      }
    }

    // Emit the latest frame we have — read this tick OR the last one held — so a render
    // tick with no fresh capture frame still composites real pixels instead of the pink
    // "no pixels" slate (that gap was the flashing-pink program/preview).
    if (!buffer.lastPixels || buffer.lastWidth == 0 || buffer.lastHeight == 0) {
      continue;  // no frame ever received for this device yet
    }

    VideoFrame frame;
    frame.participantId = "capture:" + deviceId;
    frame.width = buffer.lastWidth;
    frame.height = buffer.lastHeight;
    frame.naturalWidth = buffer.lastWidth;
    frame.naturalHeight = buffer.lastHeight;
    frame.pixels = buffer.lastPixels;
    frame.pixelWidth = buffer.lastWidth;
    frame.pixelHeight = buffer.lastHeight;
    frame.pixelStride = buffer.lastWidth * 4;
    frame.frameId = buffer.frameId;
    frame.timestampMs = timestampMs;

    // A real capture frame supersedes any inner (test-pattern) frame for the same id.
    frames.erase(std::remove_if(frames.begin(), frames.end(),
                                [&frame](const VideoFrame& f) { return f.participantId == frame.participantId; }),
                 frames.end());
    frames.push_back(std::move(frame));
  }

  return frames;
}

}  // namespace corevideo::modules
