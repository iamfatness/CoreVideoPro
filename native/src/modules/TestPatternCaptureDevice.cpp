#include "modules/Interfaces.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace corevideo::modules {
namespace {

// Number of BGRA buffers kept warm for the test pattern. The source publishes
// one frame per poll and the compositor consumes (copies) it synchronously, so
// a small ring is enough; it still exercises the bounded/ref-counted pool and
// surfaces back-pressure if a downstream consumer ever holds frames.
constexpr size_t kPoolCapacity = 4;

// Eight-bar SMPTE-style palette in 0xAARRGGBB. Deterministic so tests can
// assert exact sampled colors.
constexpr std::array<uint32_t, 8> kBarColors = {
    0xffc0c0c0u,  // 75% white
    0xffc0c000u,  // yellow
    0xff00c0c0u,  // cyan
    0xff00c000u,  // green
    0xffc000c0u,  // magenta
    0xffc00000u,  // red
    0xff0000c0u,  // blue
    0xff101010u,  // near-black
};

// A deterministic test-pattern capture device. It is part of the stub build and
// always produces real BGRA pixels through pollVideoFrames(), so the F1 frame
// path (source -> render plan -> compositor -> ProgramFrame) can be exercised
// end to end with no hardware. Frames are drawn into recycled buffers from a
// bounded FramePool; exhaustion is observable as droppedFrames in the snapshot.
class TestPatternCaptureDevice final : public ICaptureDevice {
 public:
  TestPatternCaptureDevice(std::string id, int width, int height, int frameRate)
      : id_(std::move(id)),
        width_(width > 0 ? width : 1280),
        height_(height > 0 ? height : 720),
        frameRate_(frameRate > 0 ? frameRate : 30),
        pool_(FramePool::create(
            kPoolCapacity, static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4u)) {}

  std::vector<CaptureDeviceInfo> enumerate() const override { return {info()}; }

  std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId, const std::string& inputId) override {
    if (deviceId == id_ && inputId == "pattern-1") {
      // Single fixed input; selection is a no-op acknowledgement.
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId, int offsetMs) override {
    if (deviceId == id_) {
      audioSyncOffsetMs_ = std::max(-500, std::min(500, offsetMs));
    }
    return enumerate();
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    if (deviceId == id_) {
      connected_ = true;
    }
    return enumerate();
  }

  std::vector<VideoFrame> pollVideoFrames(int64_t timestampMs) override {
    const size_t bytes = static_cast<size_t>(width_) * static_cast<size_t>(height_) * 4u;
    auto buffer = pool_->acquire(bytes);
    if (!buffer) {
      // Pool exhausted: a downstream consumer is still holding every buffer.
      // The pool already incremented its dropped counter; report no frame.
      return {};
    }

    ++sequence_;
    drawPattern(*buffer, sequence_);
    ++framesIngested_;

    VideoFrame frame;
    frame.participantId = "capture:" + id_;
    frame.width = width_;
    frame.height = height_;
    frame.timestampMs = timestampMs;
    frame.pixels = std::shared_ptr<const std::vector<uint8_t>>(std::move(buffer));
    frame.pixelWidth = width_;
    frame.pixelHeight = height_;
    frame.pixelStride = width_ * 4;
    frame.frameId = sequence_;
    std::vector<VideoFrame> result;
    result.push_back(std::move(frame));
    return result;
  }

 private:
  CaptureDeviceInfo info() const {
    CaptureDeviceInfo device;
    device.id = id_;
    device.name = "CoreVideo Test Pattern";
    device.kind = "video";
    device.vendor = "corevideo";
    device.inputIds = {"pattern-1"};
    device.inputLabels = {"SMPTE Bars"};
    device.inputHasEmbeddedAudio = {false};
    device.selectedInputId = "pattern-1";
    device.width = width_;
    device.height = height_;
    device.frameRate = frameRate_;
    device.connectionState = connected_ ? "connected" : "detected";
    // Signal is present only once the device has actually produced a frame, so
    // the snapshot never over-reports a source that has not delivered pixels.
    device.signalPresent = framesIngested_ > 0;
    device.droppedFrames = pool_->droppedFrames();
    device.framesIngested = framesIngested_;
    device.audioSyncOffsetMs = audioSyncOffsetMs_;
    return device;
  }

  // Draws eight vertical color bars with a 1px-per-frame horizontal scroll so
  // successive frames differ (frameId/timestamp aside) and motion is visible.
  void drawPattern(std::vector<uint8_t>& buffer, int64_t sequence) const {
    const int stride = width_ * 4;
    const int barWidth = std::max(1, width_ / static_cast<int>(kBarColors.size()));
    const int scroll = static_cast<int>(sequence % static_cast<int64_t>(width_ <= 0 ? 1 : width_));
    for (int y = 0; y < height_; ++y) {
      uint8_t* row = buffer.data() + static_cast<size_t>(y) * static_cast<size_t>(stride);
      for (int x = 0; x < width_; ++x) {
        const int shifted = (x + scroll) % width_;
        const size_t barIndex = std::min(kBarColors.size() - 1,
                                          static_cast<size_t>(shifted / barWidth));
        const uint32_t argb = kBarColors[barIndex];
        uint8_t* pixel = row + static_cast<size_t>(x) * 4u;
        pixel[0] = static_cast<uint8_t>(argb & 0xffu);          // B
        pixel[1] = static_cast<uint8_t>((argb >> 8) & 0xffu);   // G
        pixel[2] = static_cast<uint8_t>((argb >> 16) & 0xffu);  // R
        pixel[3] = static_cast<uint8_t>((argb >> 24) & 0xffu);  // A
      }
    }
  }

  std::string id_;
  int width_;
  int height_;
  int frameRate_;
  std::shared_ptr<FramePool> pool_;
  bool connected_ = false;
  int audioSyncOffsetMs_ = 0;
  int64_t sequence_ = 0;
  int64_t framesIngested_ = 0;
};

}  // namespace

std::unique_ptr<ICaptureDevice> createTestPatternCaptureDevice(
    std::string id, int width, int height, int frameRate) {
  return std::make_unique<TestPatternCaptureDevice>(std::move(id), width, height, frameRate);
}

// Decoder-stage seam factory. The default in-container build ships no codec, so
// this resolves to nullptr; a real FFmpeg libavcodec decoder is wired in behind
// COREVIDEO_ENABLE_DEV_ADAPTERS + COREVIDEO_WITH_FFMPEG_DECODE later (Phase B).
std::unique_ptr<IVideoDecoder> createVideoDecoder(const std::string& codec) {
#if COREVIDEO_ENABLE_DEV_ADAPTERS && defined(COREVIDEO_WITH_FFMPEG_DECODE) && COREVIDEO_WITH_FFMPEG_DECODE
  return createFfmpegVideoDecoder(codec);
#else
  (void)codec;
  return nullptr;
#endif
}

}  // namespace corevideo::modules
