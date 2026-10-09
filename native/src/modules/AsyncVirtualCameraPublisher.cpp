#include "modules/AsyncVirtualCameraPublisher.h"
#include "modules/GpuWebcamFramer.h"
#include "modules/VirtualCameraCompose.h"
#include "modules/ImageResize.h"
#include <exception>
#include <future>
#include <chrono>
#if defined(_WIN32)
#include <objbase.h>
#endif
namespace corevideo::modules {
AsyncVirtualCameraPublisher::AsyncVirtualCameraPublisher(std::unique_ptr<IVirtualCameraPublisher> backend)
    : backend_(std::move(backend)), worker_([this] { run(); }) {}
AsyncVirtualCameraPublisher::~AsyncVirtualCameraPublisher() {
  { std::lock_guard<std::mutex> lock(mutex_); shutdown_ = true; }
  wake_.notify_one();
  worker_.join();
}
bool AsyncVirtualCameraPublisher::start(int w, int h, int fps) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (desiredOn_) return true;
  desiredOn_ = true; width_ = w; height_ = h; fps_ = fps; ++revision_;
  cached_.enabled = true; cached_.state = "starting"; cached_.warning.clear();
  cached_.width = w; cached_.height = h; cached_.fps = fps;
  wake_.notify_one();
  return true; // accepted; completion/failure is observed through status()
}
void AsyncVirtualCameraPublisher::stop() {
  std::optional<ProgramFrame> retiredFrame;
  std::shared_ptr<const std::vector<uint8_t>> retiredNv12;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!desiredOn_) return;
    desiredOn_ = false; ++revision_;
    frame_.swap(retiredFrame); retiredNv12 = std::move(nv12_);
    cached_.enabled = false; cached_.state = "stopping";
    wake_.notify_one();
  }
}
void AsyncVirtualCameraPublisher::setMirror(bool mirror) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (mirror_ == mirror) return;
  mirror_ = mirror; ++revision_; wake_.notify_one();
}
void AsyncVirtualCameraPublisher::setFramerEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (framerEnabled_ == enabled) return;
  framerEnabled_ = enabled; ++revision_; wake_.notify_one();
}
void AsyncVirtualCameraPublisher::setDeviceName(const std::string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (name.empty() || name_ == name) return;
  name_ = name; cached_.deviceName = name; ++revision_; wake_.notify_one();
}
VirtualCameraStatus AsyncVirtualCameraPublisher::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = cached_;
  result.framesAccepted = framesAccepted_;
  result.pendingFramesReplaced = pendingFramesReplaced_;
  result.publicationExceptions = publicationExceptions_;
  result.framerEnabled = framerEnabled_;
  if (!framerEnabled_) { result.framerState = "off"; result.framerWarning.clear(); }
  else if (!desiredOn_) { result.framerState = "waiting"; result.framerWarning.clear(); }
  return result;
}
void AsyncVirtualCameraPublisher::publish(const ProgramFrame& frame) {
  // The fallback frame may own a full-resolution CPU buffer. Copy and retire
  // it outside the state lock so snapshot/control calls cannot wait on allocation.
  std::optional<ProgramFrame> pending(frame);
  std::shared_ptr<const std::vector<uint8_t>> retired;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!desiredOn_) return;
    ++framesAccepted_;
    if (frame_ || nv12_) ++pendingFramesReplaced_;
    frame_.swap(pending); retired = std::move(nv12_); wake_.notify_one();
  }
}
void AsyncVirtualCameraPublisher::publishNv12(const uint8_t* bytes, int w, int h) {
  if (!bytes || w <= 0 || h <= 0 || w > 7680 || h > 4320) return;
  publishNv12Shared(std::make_shared<const std::vector<uint8_t>>(bytes, bytes + static_cast<size_t>(w) * h * 3 / 2), w, h);
}
void AsyncVirtualCameraPublisher::publishNv12Shared(std::shared_ptr<const std::vector<uint8_t>> bytes, int w, int h) {
  publishNv12Identified(std::move(bytes), w, h, 0, 0);
}
void AsyncVirtualCameraPublisher::publishNv12Identified(std::shared_ptr<const std::vector<uint8_t>> bytes,
    int w, int h, int64_t programSequence, int64_t deliveredAt100ns) {
  std::optional<ProgramFrame> retiredFrame;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!desiredOn_ || !bytes) return;
  ++framesAccepted_;
  if (frame_ || nv12_) ++pendingFramesReplaced_;
  nv12_.swap(bytes); frame_.swap(retiredFrame); frameWidth_ = w; frameHeight_ = h;
  pendingProgramSequence_ = programSequence; pendingDeliveredAt100ns_ = deliveredAt100ns;
  wake_.notify_one();
}
void AsyncVirtualCameraPublisher::run() {
#if defined(_WIN32)
  const auto comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
#endif
  uint64_t appliedRevision = 0;
  bool appliedOn = false, framerHasProcessedFrame = false;
  std::unique_ptr<GpuWebcamFramer> framer;
  std::future<std::unique_ptr<GpuWebcamFramer>> preparing;
  uint64_t framerFrames = 0, framerFailures = 0;
  for (;;) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto ready = [&] {
      return shutdown_ || revision_ != appliedRevision || frame_ || nv12_ ||
        (preparing.valid() && preparing.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
    };
    // Poll only during preparation so a completed job is retired even when
    // the camera was stopped and there are no more incoming frames.
    if (preparing.valid()) wake_.wait_for(lock, std::chrono::milliseconds(16), ready);
    else wake_.wait(lock, ready);
    if (shutdown_) break;
    const auto revision = revision_;
    const bool on = desiredOn_, mirror = mirror_;
    const bool framerEnabled = framerEnabled_;
    const auto name = name_;
    const int w = width_, h = height_, fps = fps_;
    int fw = frameWidth_, fh = frameHeight_;
    int64_t programSequence = pendingProgramSequence_, deliveredAt100ns = pendingDeliveredAt100ns_;
    auto frame = std::move(frame_); frame_.reset();
    auto nv12 = std::move(nv12_);
    lock.unlock();
    VirtualCameraStatus observed;
    const bool attemptedPublication = on && (nv12 || frame);
    bool publicationFailed = false;
    try {
      backend_->setMirror(mirror);
      backend_->setDeviceName(name);
      if (on && !appliedOn) { backend_->stop(); backend_->start(w, h, fps); }
      else if (!on && appliedOn) backend_->stop();
      appliedOn = on;
      const bool cameraReady = on && backend_->status().enabled;
      if (!cameraReady || !framerEnabled) { framer.reset(); framerHasProcessedFrame = false; }
      // One bounded preparation job. Never wait for PNG decode/shader compile
      // while publishing: clean video continues until the stage is ready.
      if (preparing.valid() && preparing.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto prepared = preparing.get();
        if (cameraReady && framerEnabled) { framer = std::move(prepared); framerHasProcessedFrame = false; }
      }
      if (cameraReady && framerEnabled && !framer && !preparing.valid()) {
        preparing = std::async(std::launch::async, [w,h] {
#if defined(_WIN32)
          const auto hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
          struct ComScope { HRESULT hr; ~ComScope() { if(SUCCEEDED(hr)) CoUninitialize(); } } com{hr};
#endif
          auto prepared = std::make_unique<GpuWebcamFramer>();
          prepared->prepare(w,h);
          return prepared;
        });
      }
      // The fallback already converts on this worker. Fork only the webcam's
      // pixels; retain the clean Program buffer and its delivery attribution.
      if (on && framerEnabled && framer && frame && !nv12) {
        const auto& px = !frame->programFullBgra.bgra.empty() ? frame->programFullBgra : frame->preview;
        auto converted = std::make_shared<std::vector<uint8_t>>();
        std::vector<uint8_t> resized;
        const uint8_t* bgra = px.bgra.empty() ? nullptr : px.bgra.data();
        int bw = px.width, bh = px.height;
        // Preserve the backend's existing fixed-media-type scaling behavior.
        if (bgra && (bw != w || bh != h)) {
          if (resizeBgraBilinear(bgra, bw, bh, w, h, resized)) {
            bgra = resized.data(); bw = w; bh = h;
          } else bgra = nullptr;
        }
        const auto composed = composeVirtualCameraNv12(bgra, bw, bh, w, h, false, *converted);
        nv12 = std::move(converted); fw = composed.width; fh = composed.height;
        programSequence = composed.isSlate ? 0 : frame->frameNumber;
        deliveredAt100ns = composed.isSlate ? 0 : frame->deliveredAt100ns;
        frame.reset();
      }
      if (on && framerEnabled && framer && nv12) {
        if (auto decorated = framer->apply(nv12, fw, fh, mirror)) {
          nv12 = std::move(decorated); ++framerFrames; framerHasProcessedFrame = true;
        } else ++framerFailures;
      }
      if (on && nv12) backend_->publishNv12IdentifiedOnWorker(std::move(nv12), fw, fh, programSequence, deliveredAt100ns);
      else if (on && frame) backend_->publish(*frame);
      observed = backend_->status();
      observed.framerEnabled = framerEnabled;
      observed.framerFrames = framerFrames; observed.framerFailures = framerFailures;
      observed.framerState = !framerEnabled ? "off" : !framer ? "waiting" :
        !framer->warning().empty() ? "unavailable" :
        framerHasProcessedFrame && observed.enabled ? "active" : "waiting";
      if (framer) observed.framerWarning = framer->warning();
    } catch (const std::exception& e) {
      publicationFailed = attemptedPublication;
      observed.state = "failed"; observed.warning = e.what(); appliedOn = on;
    } catch (...) {
      publicationFailed = attemptedPublication;
      observed.state = "failed"; observed.warning = "Virtual camera operation failed."; appliedOn = on;
    }
    lock.lock();
    if (publicationFailed) ++publicationExceptions_;
    appliedRevision = revision;
    // A slow start completing after Stop must never resurrect a live status.
    if (revision_ == revision) cached_ = std::move(observed);
  }
  try { backend_->stop(); } catch (...) {}
  backend_.reset();
  framer.reset();
  if (preparing.valid()) { try { preparing.get().reset(); } catch (...) {} }
#if defined(_WIN32)
  if (SUCCEEDED(comHr)) CoUninitialize();
#endif
}
}
