// AVFoundation camera capture — the macOS twin of UvcCaptureDeviceAdapter.
// Same contract: enumerate cheap+throttled, connect() creates a per-device
// session that delivers I420 VideoFrames keyed `capture:<outputSourceId ?:
// id>` with truthful color hints, the no-first-frame watchdog fails LOUD and
// releases the device (reusing uvc::uvcNoFirstFrameTimedOut/Warning — the
// watchdog logic is generic and deliberately not forked), pollVideoFrames
// re-emits the held frame each tick, and every state transition lands in the
// CaptureDeviceInfo row (detected -> connected+waiting -> signalPresent, or
// error with a warning that names the device).
//
// Differences from the UVC/MF blueprint, all platform-shaped:
//  - AVCaptureSession owns the delivery thread (a dispatch queue), so there
//    is no readLoop; the watchdog is evaluated lazily in pollVideoFrames and
//    stops the session when it fires (frees a single-consumer device).
//  - Camera access needs TCC consent: a denied AVCaptureDeviceInput becomes a
//    truthful error row, never a silent placeholder.
//  - Ids: id = uvc::stableCaptureDeviceIdFromSymbolicLink(uniqueID) (the same
//    sha convention as Windows), nativeDeviceId = the raw uniqueID. The
//    two-arg connect override is kept: a shell that hashes differently still
//    keys frames its way.

#include "modules/Interfaces.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AVF_CAPTURE

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

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
#include "modules/UvcCaptureSupport.h"

@class CvpAvfCaptureDelegate;

namespace corevideo::modules {
namespace {

int64_t avfMonotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct AvfSessionState {
  std::mutex mutex;
  std::vector<uint8_t> latestI420;
  int width = 0;
  int height = 0;
  bool fullRange = false;
  bool bt601 = false;
  int64_t frameId = 0;
  int64_t startedAtMs = 0;
  std::string error;
};

}  // namespace
}  // namespace corevideo::modules

// The sample-buffer delegate: converts NV12 CVPixelBuffers to tight I420 and
// publishes under the session mutex. Runs on the session's dispatch queue —
// no app locks beyond the session's own state mutex.
@interface CvpAvfCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
 @public
  std::shared_ptr<corevideo::modules::AvfSessionState> state_;
}
@end

@implementation CvpAvfCaptureDelegate
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection {
  (void)output;
  (void)connection;
  auto state = state_;
  if (!state) {
    return;
  }
  CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sampleBuffer);
  if (!pixels || CVPixelBufferGetPlaneCount(pixels) < 2) {
    return;
  }
  if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
    return;
  }
  const int width = static_cast<int>(CVPixelBufferGetWidth(pixels)) & ~1;
  const int height = static_cast<int>(CVPixelBufferGetHeight(pixels)) & ~1;
  const auto* yBase = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 0));
  const auto* uvBase = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(pixels, 1));
  const size_t yStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 0);
  const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(pixels, 1);
  const uint32_t fourcc = CVPixelBufferGetPixelFormatType(pixels);
  std::string matrix;
  if (CFTypeRef attachment = CVBufferCopyAttachment(pixels, kCVImageBufferYCbCrMatrixKey, nullptr)) {
    char buffer[128] = {0};
    if (CFGetTypeID(attachment) == CFStringGetTypeID() &&
        CFStringGetCString(static_cast<CFStringRef>(attachment), buffer, sizeof(buffer),
                           kCFStringEncodingUTF8)) {
      matrix = buffer;
    }
    CFRelease(attachment);
  }
  std::vector<uint8_t> i420;
  const bool converted = corevideo::modules::maccapture::nv12ToI420Strided(
      yBase, yStride, uvBase, uvStride, width, height, i420);
  CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
  if (!converted) {
    return;
  }
  const auto hints =
      corevideo::modules::maccapture::deriveMacYuvColorHints(fourcc, matrix, height);
  std::lock_guard<std::mutex> lock(state->mutex);
  state->latestI420 = std::move(i420);
  state->width = width;
  state->height = height;
  state->fullRange = hints.fullRange;
  state->bt601 = hints.bt601;
  ++state->frameId;
}
@end

namespace corevideo::modules {
namespace {

struct AvfDeviceEntry {
  // Connect bailed waiting on TCC; retried when consent lands.
  bool pendingPermission = false;
  CaptureDeviceInfo info;
  std::string outputSourceId;
  AVCaptureSession* session = nil;
  CvpAvfCaptureDelegate* delegate = nil;
  dispatch_queue_t queue = nil;
  std::shared_ptr<AvfSessionState> state;
};

// Devices map shared with the async discovery block: enumerate() must stay
// CHEAP (it runs on every snapshot at tick rate), so AVCaptureDevice
// discovery happens on a background queue and publishes here. The block owns
// the state jointly, so a result landing after the adapter (or its owning
// MediaCore — every MediaCore test constructs one) is gone writes into
// still-owned memory. Same pattern as the SCK targets cache.
struct AvfDevicesState {
  std::mutex mutex;
  std::map<std::string, AvfDeviceEntry> devices;
  int64_t lastRefreshKickMs = -10000;
  // Set by the TCC completion handler (arbitrary queue) — consumed on the
  // next poll tick under the mutex.
  std::atomic<bool> retryAfterPermission{false};
};

class AvfCaptureDevice final : public ICaptureDevice {
 public:
  ~AvfCaptureDevice() override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    for (auto& [id, entry] : shared_->devices) {
      stopSessionLocked(entry);
    }
  }

  std::vector<CaptureDeviceInfo> enumerate() const override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    refreshLocked(false);
    return snapshotLocked();
  }

  std::vector<CaptureDeviceInfo> selectInput(const std::string& deviceId,
                                             const std::string& inputId) override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    auto it = shared_->devices.find(deviceId);
    if (it != shared_->devices.end() && !inputId.empty()) {
      it->second.info.selectedInputId = inputId;
    }
    return snapshotLocked();
  }

  std::vector<CaptureDeviceInfo> setAudioSyncOffset(const std::string& deviceId,
                                                    int offsetMs) override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    auto it = shared_->devices.find(deviceId);
    if (it != shared_->devices.end()) {
      it->second.info.audioSyncOffsetMs = std::clamp(offsetMs, -500, 500);
    }
    return snapshotLocked();
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId) override {
    return connect(deviceId, std::string());
  }

  std::vector<CaptureDeviceInfo> connect(const std::string& deviceId,
                                         const std::string& outputSourceId) override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    refreshLocked(true);
    auto it = findLocked(deviceId);
    if (it == shared_->devices.end()) {
      return snapshotLocked();
    }
    auto& entry = it->second;
    stopSessionLocked(entry);  // fresh attempt clears a prior error / re-plug
    entry.outputSourceId = outputSourceId;
    startSessionLocked(entry);
    return snapshotLocked();
  }

  std::vector<CaptureDeviceInfo> disconnect(const std::string& deviceId) override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    auto it = findLocked(deviceId);
    if (it != shared_->devices.end()) {
      stopSessionLocked(it->second);
      it->second.outputSourceId.clear();
      it->second.info.connectionState = "detected";
      it->second.info.signalPresent = false;
      it->second.info.warning.clear();
    }
    return snapshotLocked();
  }

  std::vector<VideoFrame> pollVideoFrames(int64_t timestampMs) override {
    std::lock_guard<std::mutex> lock(shared_->mutex);
    // Consent arrived after a connect bailed — start those sessions now.
    if (shared_->retryAfterPermission.exchange(false)) {
      for (auto& [retryId, retryEntry] : shared_->devices) {
        if (retryEntry.pendingPermission) {
          retryEntry.pendingPermission = false;
          std::fprintf(stderr,
                       "[avf-capture] permission granted — reconnecting '%s'\n",
                       retryEntry.info.name.c_str());
          startSessionLocked(retryEntry);
        }
      }
    }
    std::vector<VideoFrame> frames;
    for (auto& [id, entry] : shared_->devices) {
      if (!entry.state) {
        continue;
      }
      std::string error;
      int64_t frameId = 0;
      int64_t startedAtMs = 0;
      VideoFrame frame;
      {
        std::lock_guard<std::mutex> stateLock(entry.state->mutex);
        error = entry.state->error;
        frameId = entry.state->frameId;
        startedAtMs = entry.state->startedAtMs;
        if (frameId > 0 && !entry.state->latestI420.empty()) {
          frame.i420 = std::make_shared<std::vector<uint8_t>>(entry.state->latestI420);
          frame.i420Width = entry.state->width;
          frame.i420Height = entry.state->height;
          frame.i420FullRange = entry.state->fullRange;
          frame.i420Bt601 = entry.state->bt601;
          frame.width = entry.state->width;
          frame.height = entry.state->height;
          frame.frameId = frameId;
        }
      }
      // The no-first-frame watchdog, evaluated lazily (the session's dispatch
      // queue replaces the UVC readLoop): negotiated-but-silent devices fail
      // LOUD and release the camera.
      if (error.empty() && frameId == 0 && startedAtMs > 0 &&
          uvc::uvcNoFirstFrameTimedOut(frameId, avfMonotonicMs() - startedAtMs)) {
        error = uvc::uvcNoFirstFrameWarning(entry.info.name, uvc::kUvcNoFirstFrameTimeoutMs);
        {
          std::lock_guard<std::mutex> stateLock(entry.state->mutex);
          entry.state->error = error;
        }
        stopSessionRuntimeLocked(entry);
        std::fprintf(stderr, "[avf-capture] %s\n", error.c_str());
      }
      if (!error.empty()) {
        entry.info.connectionState = "error";
        entry.info.signalPresent = false;
        entry.info.warning = error;
        continue;
      }
      if (frameId > 0) {
        entry.info.signalPresent = true;
        entry.info.warning.clear();
        entry.info.width = frame.i420Width;
        entry.info.height = frame.i420Height;
        const std::string key =
            entry.outputSourceId.empty() ? entry.info.id : entry.outputSourceId;
        frame.participantId = "capture:" + key;
        frame.naturalWidth = frame.i420Width;
        frame.naturalHeight = frame.i420Height;
        frame.timestampMs = timestampMs + entry.info.audioSyncOffsetMs;
        frames.push_back(std::move(frame));  // re-emit held frame every tick
      }
    }
    return frames;
  }

 private:
  std::map<std::string, AvfDeviceEntry>::iterator findLocked(const std::string& deviceId) {
    auto it = shared_->devices.find(deviceId);
    if (it != shared_->devices.end()) {
      return it;
    }
    for (it = shared_->devices.begin(); it != shared_->devices.end(); ++it) {
      if (it->second.info.nativeDeviceId == deviceId) {
        return it;
      }
    }
    return shared_->devices.end();
  }

  void refreshLocked(bool force) const {
    const int64_t nowMs = avfMonotonicMs();
    if (!force && nowMs - shared_->lastRefreshKickMs < 2000) {
      return;
    }
    shared_->lastRefreshKickMs = nowMs;
    std::shared_ptr<AvfDevicesState> shared = shared_;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
      NSArray<AVCaptureDeviceType>* types = @[
        AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal,
        AVCaptureDeviceTypeContinuityCamera
      ];
      AVCaptureDeviceDiscoverySession* discovery = [AVCaptureDeviceDiscoverySession
          discoverySessionWithDeviceTypes:types
                                mediaType:AVMediaTypeVideo
                                 position:AVCaptureDevicePositionUnspecified];
      std::map<std::string, std::pair<std::string, std::string>> present;  // id -> {name, uid}
      for (AVCaptureDevice* device in discovery.devices) {
        const std::string uniqueId = device.uniqueID.UTF8String ? device.uniqueID.UTF8String : "";
        if (uniqueId.empty()) {
          continue;
        }
        const std::string name =
            device.localizedName.UTF8String ? device.localizedName.UTF8String : uniqueId;
        present.emplace(uvc::stableCaptureDeviceIdFromSymbolicLink(uniqueId),
                        std::make_pair(name, uniqueId));
      }
      std::lock_guard<std::mutex> lock(shared->mutex);
      for (const auto& [id, nameAndUid] : present) {
        auto it = shared->devices.find(id);
        if (it == shared->devices.end()) {
          AvfDeviceEntry entry;
          entry.info.id = id;
          entry.info.name = nameAndUid.first;
          entry.info.kind = "video";
          entry.info.vendor = "avfoundation";
          entry.info.inputIds = {"camera"};
          entry.info.inputLabels = {"Camera"};
          entry.info.inputHasEmbeddedAudio = {false};
          entry.info.selectedInputId = "camera";
          entry.info.connectionState = "detected";
          entry.info.nativeDeviceId = nameAndUid.second;
          shared->devices.emplace(id, std::move(entry));
        }
      }
      // Streaming-but-vanished devices stay listed as an error (unplugged);
      // idle vanished are dropped — the UVC merge policy.
      for (auto it = shared->devices.begin(); it != shared->devices.end();) {
        if (present.count(it->first)) {
          ++it;
          continue;
        }
        if (it->second.session) {
          it->second.info.connectionState = "error";
          it->second.info.signalPresent = false;
          it->second.info.warning = "Camera is no longer present (unplugged?)";
          ++it;
        } else {
          it = shared->devices.erase(it);
        }
      }
    });
  }

  void startSessionLocked(AvfDeviceEntry& entry) {
    // TCC consent is REQUIRED before a session will deliver frames. Without
    // this the session starts, reports "connected", and then silently never
    // produces a sample — which is exactly how "web cams aren't working"
    // presented: connected rows, no pixels, no error anywhere. Ask, and make
    // every outcome a truthful row instead of silence.
    const AVAuthorizationStatus camAuth =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (camAuth == AVAuthorizationStatusDenied ||
        camAuth == AVAuthorizationStatusRestricted) {
      entry.info.connectionState = "error";
      entry.info.warning =
          "Camera access is denied for CoreVideo Pro. Grant it in System "
          "Settings > Privacy & Security > Camera, then reconnect.";
      std::fprintf(stderr, "[avf-capture] camera permission DENIED for '%s'\n",
                   entry.info.name.c_str());
      return;
    }
    if (camAuth == AVAuthorizationStatusNotDetermined) {
      // Prompt once, without blocking the caller (this runs under the core
      // lock; a modal wait here froze the render thread for seconds). The
      // grant lands asynchronously AFTER this connect has already bailed, so
      // flag it and let the poll tick retry — an operator who approves the
      // prompt must not have to hunt for a Connect button again.
      auto shared = shared_;
      [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                               completionHandler:^(BOOL granted) {
        std::fprintf(stderr, "[avf-capture] camera permission %s\n",
                     granted ? "granted" : "denied");
        if (granted) {
          shared->retryAfterPermission.store(true);
        }
      }];
      entry.pendingPermission = true;
      entry.info.connectionState = "error";
      entry.info.warning =
          "Waiting for camera permission — approve the macOS prompt.";
      std::fprintf(stderr, "[avf-capture] camera permission requested for '%s'\n",
                   entry.info.name.c_str());
      return;
    }
    entry.pendingPermission = false;

    NSString* uniqueId = [NSString stringWithUTF8String:entry.info.nativeDeviceId.c_str()];
    AVCaptureDevice* device = [AVCaptureDevice deviceWithUniqueID:uniqueId];
    if (!device) {
      entry.info.connectionState = "error";
      entry.info.warning = "Camera device could not be opened.";
      return;
    }
    NSError* error = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
    if (!input) {
      entry.info.connectionState = "error";
      entry.info.warning = std::string("Camera access failed: ") +
                           (error ? error.localizedDescription.UTF8String
                                  : "no diagnostics (camera permission denied?)");
      std::fprintf(stderr, "[avf-capture] %s (%s)\n", entry.info.warning.c_str(),
                   entry.info.name.c_str());
      return;
    }
    entry.state = std::make_shared<AvfSessionState>();
    entry.state->startedAtMs = avfMonotonicMs();
    entry.session = [[AVCaptureSession alloc] init];
    [entry.session beginConfiguration];
    if (![entry.session canAddInput:input]) {
      entry.info.connectionState = "error";
      entry.info.warning = "Camera input was rejected by the capture session.";
      entry.session = nil;
      return;
    }
    [entry.session addInput:input];
    AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
    output.videoSettings = @{
      (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey :
          @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
    };
    output.alwaysDiscardsLateVideoFrames = YES;
    entry.delegate = [[CvpAvfCaptureDelegate alloc] init];
    entry.delegate->state_ = entry.state;
    entry.queue = dispatch_queue_create("us.iamfatness.corevideopro.avf-capture",
                                        DISPATCH_QUEUE_SERIAL);
    [output setSampleBufferDelegate:entry.delegate queue:entry.queue];
    if (![entry.session canAddOutput:output]) {
      entry.info.connectionState = "error";
      entry.info.warning = "Camera output was rejected by the capture session.";
      entry.session = nil;
      entry.delegate = nil;
      return;
    }
    [entry.session addOutput:output];
    [entry.session commitConfiguration];
    [entry.session startRunning];
    entry.info.connectionState = "connected";
    entry.info.signalPresent = false;
    entry.info.warning = "Waiting for the first camera frame.";
    std::fprintf(stderr, "[avf-capture] connected '%s' frameKey=capture:%s\n",
                 entry.info.name.c_str(),
                 (entry.outputSourceId.empty() ? entry.info.id : entry.outputSourceId).c_str());
  }

  // Stops the runtime pieces but leaves the info row alone (the caller sets
  // the state it wants to report).
  void stopSessionRuntimeLocked(AvfDeviceEntry& entry) {
    if (entry.session) {
      // Detach the delegate before stopping so an in-flight callback cannot
      // publish into a torn-down state (the WGC teardown-drain lesson, solved
      // here by the shared_ptr: the callback holds its own reference).
      entry.delegate->state_ = nullptr;
      [entry.session stopRunning];
      entry.session = nil;
      entry.delegate = nil;
      entry.queue = nil;
    }
  }

  void stopSessionLocked(AvfDeviceEntry& entry) {
    stopSessionRuntimeLocked(entry);
    entry.state.reset();
  }

  std::vector<CaptureDeviceInfo> snapshotLocked() const {
    std::vector<CaptureDeviceInfo> out;
    out.reserve(shared_->devices.size());
    for (const auto& [id, entry] : shared_->devices) {
      out.push_back(entry.info);
    }
    return out;
  }

  mutable std::shared_ptr<AvfDevicesState> shared_ = std::make_shared<AvfDevicesState>();
};

}  // namespace

std::unique_ptr<ICaptureDevice> createAvfCaptureDevice() {
  return std::make_unique<AvfCaptureDevice>();
}

}  // namespace corevideo::modules

#else  // gate

#include <memory>

namespace corevideo::modules {

std::unique_ptr<ICaptureDevice> createAvfCaptureDevice() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AVF_CAPTURE
