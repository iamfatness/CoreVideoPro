#include "modules/RealZoomCaptureSource.h"

#include "modules/ProgramFramePreview.h"

#include <utility>

namespace corevideo::modules {

RealZoomCaptureSource::RealZoomCaptureSource(std::unique_ptr<IZoomCaptureSource> fallback)
    : fallback_(std::move(fallback)) {}

void RealZoomCaptureSource::ingestFrame(const std::string& participantId,
                                        const uint8_t* bgra,
                                        int width,
                                        int height,
                                        int64_t frameId,
                                        int64_t timestampMs) {
  if (participantId.empty() || !bgra || width <= 0 || height <= 0) {
    return;
  }
  const int stride = width * 4;
  auto buffer = std::make_shared<std::vector<uint8_t>>(
      bgra, bgra + static_cast<size_t>(stride) * static_cast<size_t>(height));

  std::lock_guard<std::mutex> lock(mutex_);
  StoredFrame& stored = frames_[participantId];
  stored.pixels = std::move(buffer);
  stored.i420 = nullptr;  // latest representation wins
  stored.width = width;
  stored.height = height;
  stored.stride = stride;
  stored.frameId = frameId;
  stored.timestampMs = timestampMs;
}

void RealZoomCaptureSource::ingestI420Frame(const std::string& participantId,
                                            const uint8_t* i420,
                                            int width,
                                            int height,
                                            int64_t frameId,
                                            int64_t timestampMs) {
  if (participantId.empty() || !i420 || width <= 0 || height <= 0) {
    return;
  }
  if ((width & 1) != 0 || (height & 1) != 0) {
    return;  // I420 requires even dimensions (2x2 chroma subsampling)
  }
  const size_t yLength = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t total = yLength + (yLength / 4) * 2;
  auto buffer = std::make_shared<std::vector<uint8_t>>(i420, i420 + total);

  std::lock_guard<std::mutex> lock(mutex_);
  StoredFrame& stored = frames_[participantId];
  stored.i420 = std::move(buffer);
  stored.pixels = nullptr;  // latest representation wins
  stored.width = width;
  stored.height = height;
  stored.stride = 0;
  stored.frameId = frameId;
  stored.timestampMs = timestampMs;
}

void RealZoomCaptureSource::ingestFrameEvents(const std::vector<rpc::Json>& events) {
  for (const auto& event : events) {
    if (event.getString("type") != "zoom-video-frame") {
      continue;
    }
    const rpc::Json* frame = event.get("frame");
    if (!frame || !frame->isObject()) {
      continue;
    }
    const auto participantId = frame->getString("participantId");
    const int width = static_cast<int>(frame->get("width") ? frame->get("width")->asNumber() : 0.0);
    const int height = static_cast<int>(frame->get("height") ? frame->get("height")->asNumber() : 0.0);
    const int64_t frameId = static_cast<int64_t>(frame->get("frameId") ? frame->get("frameId")->asNumber() : 0.0);
    const int64_t timestampMs = static_cast<int64_t>(frame->get("observedAtMs") ? frame->get("observedAtMs")->asNumber() : 0.0);
    const auto bgraBase64 = frame->getString("bgraBase64");
    if (participantId.empty() || width <= 0 || height <= 0 || bgraBase64.empty()) {
      continue;
    }
    const auto decoded = base64Decode(bgraBase64);
    if (decoded.size() < static_cast<size_t>(width) * static_cast<size_t>(height) * 4u) {
      continue;
    }
    ingestFrame(participantId, decoded.data(), width, height, frameId, timestampMs);
  }
}

std::vector<VideoFrame> RealZoomCaptureSource::pollVideoFrames() {
  std::vector<VideoFrame> result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result.reserve(frames_.size());
    for (const auto& [participantId, stored] : frames_) {
      VideoFrame frame;
      frame.participantId = participantId;
      frame.width = stored.width;
      frame.height = stored.height;
      frame.naturalWidth = stored.width;
      frame.naturalHeight = stored.height;
      frame.timestampMs = stored.timestampMs;
      frame.frameId = stored.frameId;
      if (stored.i420) {
        frame.i420 = stored.i420;
        frame.i420Width = stored.width;
        frame.i420Height = stored.height;
      } else {
        frame.pixels = stored.pixels;
        frame.pixelWidth = stored.width;
        frame.pixelHeight = stored.height;
        frame.pixelStride = stored.stride;
      }
      result.push_back(std::move(frame));
    }
  }
  if (result.empty() && fallback_) {
    return fallback_->pollVideoFrames();
  }
  return result;
}

std::vector<AudioFrame> RealZoomCaptureSource::pollAudioFrames() {
  if (fallback_) {
    return fallback_->pollAudioFrames();
  }
  return {};
}

size_t RealZoomCaptureSource::participantCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_.size();
}

}  // namespace corevideo::modules
