#pragma once
// Per-camera state shared between the AVFoundation delegate queue (publish) and
// the render tick (sample). Plain C++ so the arrival contract is testable
// without a camera.
#include "modules/I420CaptureArrival.h"
#include "modules/Interfaces.h"
#include "modules/MacCaptureLiveness.h"
#include "modules/UvcIsoArrivalQueue.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace corevideo::modules {

struct AvfSessionState {
  explicit AvfSessionState(std::string sourceId = {})
      : arrival(std::move(sourceId), nullptr) {}

  std::mutex mutex;
  int64_t frameId = 0;      // advances once per delivered camera frame
  int64_t startedAtMs = 0;
  std::string error;
  UvcIsoArrivalQueue isoArrivals;

  // One camera delivery. Called once per sample buffer from the capture queue.
  void publish(std::shared_ptr<std::vector<uint8_t>> i420, int width, int height,
               bool fullRange, bool bt601) {
    VideoFrame published = arrival.publishMutable(std::move(i420), width, height, fullRange, bt601);
    if (!published.hasContent()) return;
    {
      std::lock_guard<std::mutex> lock(mutex);
      liveness.sample(MacCaptureLiveness::steadyNowNs());
      published.captureLiveness = liveness.value();
    }
    isoArrivals.offer(published);  // outside the state lock, as before
    std::lock_guard<std::mutex> lock(mutex);
    latest = std::move(published);
    ++frameId;
  }

  // The device went away or the session failed: the held picture reports the loss, so
  // the core's dropout policy holds it for 2 s and then fails it (CaptureLivenessPolicy.h).
  void markLost(CaptureLossReason reason) {
    std::lock_guard<std::mutex> lock(mutex);
    liveness.lost(reason, MacCaptureLiveness::steadyNowNs());
  }

  // The held picture for latest-wins consumers (Program, monitors). Returns the
  // same arrival identity every time until the camera delivers again.
  bool sampleLatest(VideoFrame& out, int64_t& frameIdOut) {
    std::lock_guard<std::mutex> lock(mutex);
    frameIdOut = frameId;
    if (frameId <= 0 || !latest.hasContent()) return false;
    out = latest;
    // The current liveness, whose sample time is the last DELIVERY: a held picture ages.
    out.captureLiveness = liveness.value();
    return true;
  }

 private:
  I420CaptureArrival arrival;
  VideoFrame latest;
  MacCaptureLiveness liveness{/*cadenceTrusted=*/true};  // a camera delivers continuously
};

}  // namespace corevideo::modules
