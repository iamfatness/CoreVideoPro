// ScreenCaptureKit screen/window capture — the macOS twin of the WGC adapter.
// Same source shapes ("screen:<n>" / "window:<id>" ids via MacCaptureSupport,
// BGRA frames keyed `capture:<id>`, publish-latest-SHARED so a source never
// vanishes between deliveries), same teardown discipline (the delegate holds
// a shared_ptr state; stop() detaches it so an in-flight callback can never
// publish into torn-down state — the WGC frameMutex_ drain lesson, solved by
// ownership instead of a drain).
//
// SCShareableContent is async-only, so enumerate() (which must stay cheap —
// it runs on every snapshot) serves a CACHED list and kicks a throttled
// background refresh. Screen-recording TCC consent: a denied fetch leaves the
// cache empty and surfaces one loud stderr line; a failed stream start
// becomes a truthful "error" row.

#include "modules/Interfaces.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_SCK

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "modules/MacCaptureSupport.h"

namespace corevideo::modules {
namespace {

int64_t sckMonotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct SckFrameState {
  std::mutex mutex;
  std::vector<uint8_t> latestBgra;
  int width = 0;
  int height = 0;
  int64_t frameId = 0;
};

}  // namespace
}  // namespace corevideo::modules

@interface CvpSckStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
 @public
  std::shared_ptr<corevideo::modules::SckFrameState> state_;
}
@end

@implementation CvpSckStreamOutput
- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
  (void)stream;
  if (type != SCStreamOutputTypeScreen) {
    return;
  }
  auto state = state_;
  if (!state) {
    return;
  }
  CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sampleBuffer);
  if (!pixels) {
    return;
  }
  if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
    return;
  }
  const int width = static_cast<int>(CVPixelBufferGetWidth(pixels));
  const int height = static_cast<int>(CVPixelBufferGetHeight(pixels));
  const auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixels));
  const size_t stride = CVPixelBufferGetBytesPerRow(pixels);
  std::vector<uint8_t> bgra;
  if (base && width > 0 && height > 0) {
    corevideo::modules::maccapture::copyBgraTight(base, stride, width, height, bgra);
  }
  CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
  if (bgra.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  state->latestBgra = std::move(bgra);
  state->width = width;
  state->height = height;
  ++state->frameId;
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
  (void)stream;
  (void)error;  // surfaced through the row staying signalPresent=false
}
@end

namespace corevideo::modules {
namespace {

struct SckTarget {
  CaptureDeviceInfo info;
  SCDisplay* display = nil;
  SCWindow* window = nil;
};

// Targets cache shared with the async SCShareableContent completion handler:
// the block captures this by shared_ptr, so a handler landing after the
// adapter (or its owning MediaCore — every MediaCore TEST constructs one) has
// been destroyed writes into still-owned memory instead of a freed adapter.
struct SckTargetsState {
  std::mutex mutex;
  std::map<std::string, SckTarget> targets;
  int64_t lastRefreshKickMs = -10000;
};

struct SckSessionEntry {
  SCStream* stream = nil;
  CvpSckStreamOutput* output = nil;
  dispatch_queue_t queue = nil;
  std::shared_ptr<SckFrameState> state;
  int naturalWidth = 0;
  int naturalHeight = 0;
};

class SckScreenCaptureDevice final : public ICaptureDevice {
 public:
  ~SckScreenCaptureDevice() override {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, session] : sessions_) {
      stopSessionLocked(session);
    }
  }

  std::vector<CaptureDeviceInfo> enumerate() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshLocked(false);
    return infosLocked();
  }

  std::vector<CaptureDeviceInfo> selectInput(const std::string&, const std::string&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return infosLocked();
  }

  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string&, int) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return infosLocked();
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    std::lock_guard<std::mutex> lock(mutex_);
    refreshLocked(true);
    SckTarget target;
    bool found = false;
    {
      std::lock_guard<std::mutex> targetsLock(targets_->mutex);
      auto it = targets_->targets.find(deviceId);
      if (it != targets_->targets.end()) {
        target = it->second;
        found = true;
      }
    }
    // (Early-return must happen OUTSIDE the scoped lock: infosLocked() takes
    // targets_->mutex itself, and it is not recursive.)
    if (!found) {
      return infosLocked();
    }
    auto existing = sessions_.find(deviceId);
    if (existing != sessions_.end()) {
      stopSessionLocked(existing->second);
      sessions_.erase(existing);
    }
    SckSessionEntry session;
    if (startSessionLocked(target, session)) {
      sessions_.emplace(deviceId, std::move(session));
    } else {
      failedConnects_[deviceId] =
          "Screen capture could not start (screen-recording permission?).";
      std::fprintf(stderr, "[sck-capture] start failed for %s\n", deviceId.c_str());
    }
    return infosLocked();
  }

  std::vector<CaptureDeviceInfo> disconnect(const std::string& deviceId) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(deviceId);
    if (it != sessions_.end()) {
      stopSessionLocked(it->second);
      sessions_.erase(it);
    }
    failedConnects_.erase(deviceId);
    return infosLocked();
  }

  std::vector<VideoFrame> pollVideoFrames(int64_t timestampMs) override {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VideoFrame> frames;
    for (auto& [id, session] : sessions_) {
      if (!session.state) {
        continue;
      }
      std::lock_guard<std::mutex> stateLock(session.state->mutex);
      if (session.state->frameId == 0 || session.state->latestBgra.empty()) {
        continue;
      }
      VideoFrame frame;
      frame.participantId = "capture:" + id;
      frame.width = session.state->width;
      frame.height = session.state->height;
      frame.pixels = std::make_shared<std::vector<uint8_t>>(session.state->latestBgra);
      frame.pixelWidth = session.state->width;
      frame.pixelHeight = session.state->height;
      frame.pixelStride = session.state->width * 4;
      // The stream is configured at native size, so natural == delivered; if
      // a future config downscales, these must stay the SOURCE dims.
      frame.naturalWidth = session.naturalWidth > 0 ? session.naturalWidth : frame.pixelWidth;
      frame.naturalHeight = session.naturalHeight > 0 ? session.naturalHeight : frame.pixelHeight;
      frame.frameId = session.state->frameId;
      frame.timestampMs = timestampMs;
      frames.push_back(std::move(frame));  // held frame re-emitted every tick
    }
    return frames;
  }

 private:
  void refreshLocked(bool force) const {
    // PROCESS-WIDE throttle, not per-instance: every SCShareableContent call
    // consults TCC/replayd over XPC, and a process that constructs many
    // MediaCores (the test suite constructs hundreds) would otherwise storm
    // the daemon — measured as multi-second per-call stalls that read as a
    // hang. One fetch per process serves everyone; a slow 30s re-poll (or an
    // explicit connect) keeps the list fresh for the live app.
    static std::atomic<int64_t> processLastKickMs{-1000000};
    const int64_t nowMs = sckMonotonicMs();
    int64_t last = processLastKickMs.load(std::memory_order_relaxed);
    const int64_t interval = force ? 2000 : 30000;
    if (nowMs - last < interval ||
        !processLastKickMs.compare_exchange_strong(last, nowMs, std::memory_order_relaxed)) {
      return;
    }
    std::shared_ptr<SckTargetsState> shared = targets_;
    [SCShareableContent getShareableContentWithCompletionHandler:^(
                            SCShareableContent* content, NSError* error) {
      if (!content) {
        static bool warned = false;
        if (!warned) {
          warned = true;
          std::fprintf(stderr,
                       "[sck-capture] shareable content unavailable: %s (screen-recording "
                       "permission?)\n",
                       error ? error.localizedDescription.UTF8String : "unknown");
        }
        return;
      }
      std::map<std::string, SckTarget> next;
      int screenIndex = 0;
      for (SCDisplay* display in content.displays) {
        SckTarget target;
        const int width = static_cast<int>(display.width);
        const int height = static_cast<int>(display.height);
        target.info.id = maccapture::sckScreenId(screenIndex);
        target.info.name = maccapture::sckScreenName(screenIndex, width, height);
        target.info.kind = "screen";
        target.info.vendor = "ScreenCaptureKit";
        target.info.width = width;
        target.info.height = height;
        target.info.frameRate = 60;
        target.info.connectionState = "detected";
        target.display = display;
        next.emplace(target.info.id, std::move(target));
        ++screenIndex;
      }
      for (SCWindow* window in content.windows) {
        if (!window.onScreen || window.frame.size.width < 64 || window.frame.size.height < 64 ||
            !window.title || window.title.length == 0) {
          continue;
        }
        SckTarget target;
        target.info.id = maccapture::sckWindowId(window.windowID);
        target.info.name = std::string(window.title.UTF8String) + " — " +
                           (window.owningApplication.applicationName.UTF8String
                                ? window.owningApplication.applicationName.UTF8String
                                : "");
        target.info.kind = "window";
        target.info.vendor = "ScreenCaptureKit";
        target.info.width = static_cast<int>(window.frame.size.width);
        target.info.height = static_cast<int>(window.frame.size.height);
        target.info.frameRate = 60;
        target.info.connectionState = "detected";
        target.window = window;
        next.emplace(target.info.id, std::move(target));
      }
      std::lock_guard<std::mutex> lock(shared->mutex);
      shared->targets = std::move(next);
    }];
  }

  bool startSessionLocked(const SckTarget& target, SckSessionEntry& session) {
    SCContentFilter* filter = nil;
    int width = target.info.width;
    int height = target.info.height;
    if (target.display) {
      filter = [[SCContentFilter alloc] initWithDisplay:target.display excludingWindows:@[]];
    } else if (target.window) {
      filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target.window];
    }
    if (!filter) {
      return false;
    }
    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width = static_cast<size_t>(std::max(2, width));
    config.height = static_cast<size_t>(std::max(2, height));
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    config.minimumFrameInterval = CMTimeMake(1, 60);
    config.queueDepth = 5;
    config.showsCursor = YES;
    session.state = std::make_shared<SckFrameState>();
    session.output = [[CvpSckStreamOutput alloc] init];
    session.output->state_ = session.state;
    session.queue =
        dispatch_queue_create("us.iamfatness.corevideopro.sck-capture", DISPATCH_QUEUE_SERIAL);
    session.stream = [[SCStream alloc] initWithFilter:filter
                                        configuration:config
                                             delegate:session.output];
    NSError* error = nil;
    if (![session.stream addStreamOutput:session.output
                                    type:SCStreamOutputTypeScreen
                      sampleHandlerQueue:session.queue
                                   error:&error]) {
      return false;
    }
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block BOOL started = NO;
    [session.stream startCaptureWithCompletionHandler:^(NSError* startError) {
      started = startError == nil;
      dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    if (!started) {
      session.output->state_ = nullptr;
      session.stream = nil;
      session.output = nil;
      return false;
    }
    session.naturalWidth = width;
    session.naturalHeight = height;
    return true;
  }

  void stopSessionLocked(SckSessionEntry& session) {
    if (session.output) {
      session.output->state_ = nullptr;  // in-flight callbacks publish nowhere
    }
    if (session.stream) {
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      [session.stream stopCaptureWithCompletionHandler:^(NSError*) {
        dispatch_semaphore_signal(done);
      }];
      dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
    }
    session.stream = nil;
    session.output = nil;
    session.queue = nil;
    session.state.reset();
  }

  std::vector<CaptureDeviceInfo> infosLocked() const {
    std::vector<CaptureDeviceInfo> out;
    std::lock_guard<std::mutex> targetsLock(targets_->mutex);
    out.reserve(targets_->targets.size());
    for (const auto& [id, target] : targets_->targets) {
      CaptureDeviceInfo info = target.info;
      const auto session = sessions_.find(id);
      const bool live = session != sessions_.end();
      bool hasFrame = false;
      if (live && session->second.state) {
        std::lock_guard<std::mutex> stateLock(session->second.state->mutex);
        hasFrame = session->second.state->frameId > 0;
      }
      info.connectionState = live ? "connected" : "detected";
      // Stronger than WGC's "a session exists": signalPresent means a frame
      // actually arrived (the UVC semantics).
      info.signalPresent = hasFrame;
      const auto failed = failedConnects_.find(id);
      if (failed != failedConnects_.end()) {
        info.connectionState = "error";
        info.warning = failed->second;
      }
      out.push_back(std::move(info));
    }
    return out;
  }

  mutable std::mutex mutex_;
  mutable std::shared_ptr<SckTargetsState> targets_ = std::make_shared<SckTargetsState>();
  std::map<std::string, SckSessionEntry> sessions_;
  std::map<std::string, std::string> failedConnects_;
};

}  // namespace

std::unique_ptr<ICaptureDevice> createSckScreenCaptureDevice() {
  return std::make_unique<SckScreenCaptureDevice>();
}

}  // namespace corevideo::modules

#else  // gate

#include <memory>

namespace corevideo::modules {

std::unique_ptr<ICaptureDevice> createSckScreenCaptureDevice() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_SCK
