// corevideo-vcam-helper-bench — measures the core -> sink-helper frame
// transport of the macOS virtual camera across a REAL process boundary (plan
// 2026-11-16-mac-vcam-provider-arrival-fix, Task 3 design B / Task 6).
//
//   corevideo-vcam-helper-bench --seconds N [--fps 60]
//
// The parent runs the production core side (HelperVirtualCameraPublisher over
// the POSIX launcher, shared memory and wake pipe) and feeds 1920x1080 NV12 on
// an absolute 60 Hz timeline. The child is this same binary running the
// production helper loop (runVcamSinkHelperFromDescriptors +
// SinkVirtualCameraPublisher). The one stand-in is the sink: PoolSink does the
// per-frame work CmioVirtualCameraSink does (pool-backed IOSurface
// CVPixelBuffer, plane copy, sample buffer) and then releases the sample
// instead of enqueueing it, because only a signed core in /Library may feed
// the real camera. It also runs the pre-change arrangement (the same sink
// in-process) on the same timeline, so the two are compared like for like.
//
// Never shipped; built on demand. Prints one JSON line per arrangement.

#include "core/MainRunLoopHost.h"
#include "modules/CmioVirtualCameraSink.h"
#include "modules/HelperVirtualCameraPublisher.h"
#include "modules/SinkVirtualCameraPublisher.h"
#include "modules/VcamSinkHelper.h"
#include "modules/VirtualCameraFrame.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace corevideo::modules;

namespace {

constexpr int kWidth = 1920, kHeight = 1080;

class PoolSink final : public IVirtualCameraSink {
 public:
  ~PoolSink() override { disconnect(); }
  std::string connect() override {
    NSDictionary* pixel = @{
      (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
      (id)kCVPixelBufferWidthKey : @(kWidth),
      (id)kCVPixelBufferHeightKey : @(kHeight),
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    NSDictionary* pool = @{(id)kCVPixelBufferPoolMinimumBufferCountKey : @6};
    return CVPixelBufferPoolCreate(kCFAllocatorDefault, (__bridge CFDictionaryRef)pool,
                                   (__bridge CFDictionaryRef)pixel, &pool_) == kCVReturnSuccess
               ? "" : "CAMERA_SINK_FAILED: pool";
  }
  SinkEnqueueResult enqueueNv12(const uint8_t* nv12, int width, int height, int64_t hostTimeNs) override {
    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool_, &buffer) != kCVReturnSuccess) return SinkEnqueueResult::QueueFull;
    CVPixelBufferLockBaseAddress(buffer, 0);
    copyPlane(buffer, 0, nv12, width, height);
    copyPlane(buffer, 1, nv12 + static_cast<size_t>(width) * height, width, height / 2);
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    if (format_ == nullptr) CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, buffer, &format_);
    CMSampleTimingInfo timing{CMTimeMake(1, 60), CMTimeMake(hostTimeNs, 1000000000), kCMTimeInvalid};
    CMSampleBufferRef sample = nullptr;
    CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, buffer, format_, &timing, &sample);
    CVPixelBufferRelease(buffer);
    if (sample == nullptr) return SinkEnqueueResult::Lost;
    CFRelease(sample);  // the real sink enqueues it here
    return SinkEnqueueResult::Ok;
  }
  void disconnect() override {
    if (pool_ != nullptr) { CVPixelBufferPoolRelease(pool_); pool_ = nullptr; }
    if (format_ != nullptr) { CFRelease(format_); format_ = nullptr; }
  }

 private:
  static void copyPlane(CVPixelBufferRef buffer, size_t plane, const uint8_t* source, int rowBytes, int rows) {
    auto* destination = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, plane));
    const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane);
    if (stride == static_cast<size_t>(rowBytes)) { std::memcpy(destination, source, static_cast<size_t>(rowBytes) * rows); return; }
    for (int row = 0; row < rows; ++row) std::memcpy(destination + stride * row, source + static_cast<size_t>(rowBytes) * row, rowBytes);
  }
  CVPixelBufferPoolRef pool_ = nullptr;
  CMVideoFormatDescriptionRef format_ = nullptr;
};

struct CallTimes {
  std::vector<double> ms;
  void add(double v) { ms.push_back(v); }
  double mean() const { double s = 0; for (double v : ms) s += v; return ms.empty() ? 0 : s / ms.size(); }
  double percentile(double p) const {
    if (ms.empty()) return 0;
    auto sorted = ms; std::sort(sorted.begin(), sorted.end());
    return sorted[std::min(sorted.size() - 1, static_cast<size_t>(p * sorted.size()))];
  }
  double max() const { return ms.empty() ? 0 : *std::max_element(ms.begin(), ms.end()); }
};

// Three distinct frames, rotated, so no copy is helped by an unchanged source.
std::vector<std::shared_ptr<const std::vector<uint8_t>>> frames() {
  std::vector<std::shared_ptr<const std::vector<uint8_t>>> out;
  for (int f = 0; f < 3; ++f) {
    auto bytes = std::make_shared<std::vector<uint8_t>>(nv12FrameSize(kWidth, kHeight));
    for (size_t i = 0; i < bytes->size(); ++i) (*bytes)[i] = static_cast<uint8_t>((i * (f + 3)) >> 3);
    out.push_back(std::move(bytes));
  }
  return out;
}

template <typename Publish>
CallTimes feed(double seconds, int fps, Publish&& publish) {
  CallTimes calls;
  const auto source = frames();
  const auto period = std::chrono::nanoseconds(1'000'000'000 / fps);
  const auto start = std::chrono::steady_clock::now();
  const int64_t total = static_cast<int64_t>(seconds * fps);
  for (int64_t n = 0; n < total; ++n) {
    std::this_thread::sleep_until(start + period * n);
    const auto t0 = std::chrono::steady_clock::now();
    publish(source[n % source.size()], n + 1);
    calls.add(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  return calls;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--bench-child") {
    return corevideo::core::runWithMainRunLoop([] {
      return runVcamSinkHelperFromDescriptors(kVcamHelperRegionFd, kVcamHelperWakeFd, std::make_unique<PoolSink>(),
                                              machHostClockNs);
    });
  }
  double seconds = 30;
  int fps = 60;
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--seconds") seconds = std::atof(argv[i + 1]);
    else if (std::string(argv[i]) == "--fps") fps = std::atoi(argv[i + 1]);
  }
  const int64_t offered = static_cast<int64_t>(seconds * fps);

  {  // Before: the sink in the feeding process.
    SinkVirtualCameraPublisher camera(std::make_unique<PoolSink>(), machHostClockNs);
    camera.start(kWidth, kHeight, fps);
    const auto calls = feed(seconds, fps, [&](auto frame, int64_t sequence) {
      camera.publishNv12IdentifiedOnWorker(std::move(frame), kWidth, kHeight, sequence, 0);
    });
    const auto status = camera.status();
    std::printf("{\"arrangement\":\"in-process\",\"seconds\":%.0f,\"fps\":%d,\"offered\":%lld,\"published\":%llu,"
                "\"sequenceGaps\":%llu,\"feederCallMs\":{\"mean\":%.3f,\"p99\":%.3f,\"max\":%.3f}}\n",
                seconds, fps, static_cast<long long>(offered), static_cast<unsigned long long>(status.framesPublished),
                static_cast<unsigned long long>(camera.sequenceGaps()), calls.mean(), calls.percentile(0.99), calls.max());
    camera.stop();
  }
  {  // After: the production core side, the helper in another process.
    HelperVirtualCameraPublisher camera(createPosixVcamHelperLauncher(currentExecutablePath(), {"--bench-child"}),
                                        defaultHelperVirtualCameraClocks(machHostClockNs));
    const auto spawnStart = std::chrono::steady_clock::now();
    if (!camera.start(kWidth, kHeight, fps)) {
      std::fprintf(stderr, "helper did not connect: %s\n", camera.status().warning.c_str());
      return 1;
    }
    const double connectMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - spawnStart).count();
    const auto calls = feed(seconds, fps, [&](auto frame, int64_t sequence) {
      camera.publishNv12IdentifiedOnWorker(std::move(frame), kWidth, kHeight, sequence, 0);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let the last hand-off land
    const auto status = camera.status();
    std::printf("{\"arrangement\":\"helper-process\",\"seconds\":%.0f,\"fps\":%d,\"offered\":%lld,\"published\":%llu,"
                "\"transportDrops\":%llu,\"sequenceGaps\":%llu,\"queueFullDrops\":%llu,\"helperConnectMs\":%.1f,"
                "\"feederCallMs\":{\"mean\":%.3f,\"p99\":%.3f,\"max\":%.3f},"
                "\"handOffToPickupMs\":{\"mean\":%.3f,\"max\":%.3f},\"helperPublishMs\":{\"mean\":%.3f,\"max\":%.3f}}\n",
                seconds, fps, static_cast<long long>(offered), static_cast<unsigned long long>(status.framesPublished),
                static_cast<unsigned long long>(status.transportDrops), static_cast<unsigned long long>(status.sinkSequenceGaps),
                static_cast<unsigned long long>(status.sinkQueueFullDrops), connectMs, calls.mean(), calls.percentile(0.99),
                calls.max(), status.transportMeanMs, status.transportMaxMs, status.sinkPublishMeanMs, status.sinkPublishMaxMs);
    camera.stop();
    return status.framesPublished == static_cast<unsigned long long>(offered) && status.transportDrops == 0 ? 0 : 1;
  }
}
