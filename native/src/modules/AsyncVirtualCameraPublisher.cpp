#include "modules/AsyncVirtualCameraPublisher.h"
#include <exception>
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
  bool appliedOn = false;
  for (;;) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [&] { return shutdown_ || revision_ != appliedRevision || frame_ || nv12_; });
    if (shutdown_) break;
    const auto revision = revision_;
    const bool on = desiredOn_, mirror = mirror_;
    const auto name = name_;
    const int w = width_, h = height_, fps = fps_, fw = frameWidth_, fh = frameHeight_;
    const int64_t programSequence = pendingProgramSequence_, deliveredAt100ns = pendingDeliveredAt100ns_;
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
      if (on && nv12) backend_->publishNv12IdentifiedOnWorker(std::move(nv12), fw, fh, programSequence, deliveredAt100ns);
      else if (on && frame) backend_->publish(*frame);
      observed = backend_->status();
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
#if defined(_WIN32)
  if (SUCCEEDED(comHr)) CoUninitialize();
#endif
}
}
