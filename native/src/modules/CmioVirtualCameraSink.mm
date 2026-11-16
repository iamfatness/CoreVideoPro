// CoreMediaIO sink transport for the macOS virtual camera (roadmap M9.2,
// docs/virtual-camera-macos-spec.md §2). Compiled only on Apple with
// COREVIDEO_WITH_MAC_VIRTUALCAM (CMake). Built with -fobjc-arc; the CF objects
// below are not ARC-managed and are released explicitly.
//
// The only per-frame CPU work is the bulk Y/UV plane copy into a pool-backed
// IOSurface CVPixelBuffer (one of the at-most-3 copies the spec allows). Each
// frame is its own buffer and is never written after the enqueue, so a torn
// frame cannot happen by construction (spec §4 law 1).
//
// Runtime proof (extension installed, real consumers) is Task 7's e2e step; CI
// and the unit suite only compile this file.

#include "modules/CmioVirtualCameraSink.h"

#include "core/BoundedAsyncLog.h"
#include "modules/OwnVirtualCamera.h"
#include "modules/SinkVirtualCameraPublisher.h"

#import <CoreMediaIO/CMIOHardware.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <dispatch/dispatch.h>
#include <mach/mach_time.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <strings.h>
#include <vector>

namespace corevideo::modules {
namespace {

// Fixed IDs (plan Global Constraints). The device UID is matched against BOTH
// the UUID string and the legacy ID: which one CMIO reports depends on how the
// extension publishes the device.
constexpr const char* kSinkStreamName = "CoreVideo Pro Camera Sink";
constexpr int kSinkWidth = 1920;
constexpr int kSinkHeight = 1080;
constexpr int kPoolMinimumBuffers = 6;

// Device arrivals (the extension starting, updating or restarting after this
// process first touched CMIO) are applied on the MAIN run loop, which the core
// services (core/MainRunLoopHost.h, src/main.cpp). kCMIOHardwarePropertyRunLoop
// ('rnlp') was set to NULL here once; the HAL accepts it and ignores it for
// device arrivals (measured on macOS 26), so it is not used.

CMIOObjectPropertyAddress globalAddress(CMIOObjectPropertySelector selector) {
  return CMIOObjectPropertyAddress{selector, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain};
}

std::string toStdString(CFStringRef value) {
  if (value == nullptr) return {};
  char buffer[512] = {};
  if (CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8)) return buffer;
  return {};
}

std::string copyStringProperty(CMIOObjectID object, CMIOObjectPropertySelector selector) {
  auto address = globalAddress(selector);
  CFStringRef value = nullptr;
  UInt32 used = 0;
  if (CMIOObjectGetPropertyData(object, &address, 0, nullptr, sizeof(value), &used, &value) != noErr) return {};
  std::string result = toStdString(value);
  if (value != nullptr) CFRelease(value);
  return result;
}

std::vector<CMIOObjectID> copyObjectList(CMIOObjectID object, CMIOObjectPropertySelector selector) {
  auto address = globalAddress(selector);
  UInt32 size = 0;
  if (CMIOObjectGetPropertyDataSize(object, &address, 0, nullptr, &size) != noErr || size == 0) return {};
  std::vector<CMIOObjectID> ids(size / sizeof(CMIOObjectID));
  UInt32 used = 0;
  if (CMIOObjectGetPropertyData(object, &address, 0, nullptr, size, &used, ids.data()) != noErr) return {};
  ids.resize(used / sizeof(CMIOObjectID));
  return ids;
}

bool isOurDevice(const std::string& uid) { return isOwnVirtualCameraUid(uid); }

CMIODeviceID findDevice() {
  for (CMIOObjectID device : copyObjectList(kCMIOObjectSystemObject, kCMIOHardwarePropertyDevices))
    if (isOurDevice(copyStringProperty(device, kCMIODevicePropertyDeviceUID))) return device;
  return kCMIOObjectUnknown;
}

void queueAltered(CMIOStreamID, void*, void*) {
  // Nothing to do: the producer never waits on the queue (a full queue drops).
}

void setNumber(CFMutableDictionaryRef dict, CFStringRef key, int32_t value) {
  CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
  CFDictionarySetValue(dict, key, number);
  CFRelease(number);
}

std::string statusText(const char* operation, OSStatus status) {
  return std::string(operation) + " failed (OSStatus " + std::to_string(static_cast<long>(status)) + ")";
}

class CmioVirtualCameraSink final : public IVirtualCameraSink {
 public:
  ~CmioVirtualCameraSink() override { disconnect(); }

  std::string connect() override {
    disconnect();
    device_ = findDevice();
    if (device_ == kCMIOObjectUnknown)
      return "CAMERA_EXTENSION_NOT_FOUND: CoreVideo Pro Camera is not installed, not approved, or restarting.";

    for (CMIOObjectID stream : copyObjectList(device_, kCMIODevicePropertyStreams)) {
      if (copyStringProperty(stream, kCMIOObjectPropertyName) == kSinkStreamName) { stream_ = stream; break; }
    }
    if (stream_ == kCMIOObjectUnknown) return fail("CAMERA_SINK_FAILED: the camera has no sink stream.");

    OSStatus status = CMIOStreamCopyBufferQueue(stream_, &queueAltered, this, &queue_);
    if (status != noErr || queue_ == nullptr)
      return fail("CAMERA_SINK_FAILED: " + statusText("CMIOStreamCopyBufferQueue", status));
    status = CMIODeviceStartStream(device_, stream_);
    if (status != noErr) return fail(sinkStartStreamWarning(static_cast<std::int32_t>(status)));
    streaming_ = true;

    if (!createPool()) return fail("CAMERA_SINK_FAILED: CVPixelBufferPoolCreate failed.");

    devicesChanged_ = std::make_shared<std::atomic<bool>>(false);
    listenerQueue_ = dispatch_queue_create("us.iamfatness.corevideopro.vcam.devices", DISPATCH_QUEUE_SERIAL);
    auto flag = devicesChanged_;
    listener_ = ^(UInt32, const CMIOObjectPropertyAddress*) { flag->store(true, std::memory_order_release); };
    auto devicesAddress = globalAddress(kCMIOHardwarePropertyDevices);
    if (CMIOObjectAddPropertyListenerBlock(kCMIOObjectSystemObject, &devicesAddress, listenerQueue_, listener_) != noErr) {
      listener_ = nil;  // Loss is then caught by the enqueue error path alone.
    }
    return "";
  }

  SinkEnqueueResult enqueueNv12(const uint8_t* nv12, int width, int height, int64_t hostTimeNs) override {
    if (queue_ == nullptr || pool_ == nullptr || nv12 == nullptr) return SinkEnqueueResult::Lost;
    if (devicesChanged_ && devicesChanged_->exchange(false, std::memory_order_acq_rel) &&
        findDevice() != device_)
      return SinkEnqueueResult::Lost;
    if (width != kSinkWidth || height != kSinkHeight) return SinkEnqueueResult::QueueFull;
    if (CMSimpleQueueGetCount(queue_) >= CMSimpleQueueGetCapacity(queue_)) return SinkEnqueueResult::QueueFull;

    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool_, &buffer) != kCVReturnSuccess || buffer == nullptr)
      return SinkEnqueueResult::QueueFull;  // pool exhausted: consumers still hold every buffer
    if (CVPixelBufferLockBaseAddress(buffer, 0) != kCVReturnSuccess) {
      CVPixelBufferRelease(buffer);
      return SinkEnqueueResult::QueueFull;
    }
    const size_t lumaBytes = static_cast<size_t>(width) * height;
    copyPlane(buffer, 0, nv12, width, height);
    copyPlane(buffer, 1, nv12 + lumaBytes, width, height / 2);
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_601_4,
                          kCVAttachmentMode_ShouldPropagate);

    if (format_ == nullptr || !CMVideoFormatDescriptionMatchesImageBuffer(format_, buffer)) {
      if (format_ != nullptr) { CFRelease(format_); format_ = nullptr; }
      if (CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, buffer, &format_) != noErr) {
        CVPixelBufferRelease(buffer);
        return SinkEnqueueResult::Lost;
      }
    }
    CMSampleTimingInfo timing{CMTimeMake(1, 60), CMTimeMake(hostTimeNs, 1000000000), kCMTimeInvalid};
    CMSampleBufferRef sample = nullptr;
    const OSStatus created = CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, buffer, format_, &timing, &sample);
    CVPixelBufferRelease(buffer);  // the sample buffer holds its own reference
    if (created != noErr || sample == nullptr) return SinkEnqueueResult::Lost;
    // The queue takes our +1 reference; the extension's consumer releases it (OBS pattern).
    if (CMSimpleQueueEnqueue(queue_, sample) != noErr) {
      CFRelease(sample);
      return SinkEnqueueResult::Lost;
    }
    return SinkEnqueueResult::Ok;
  }

  void disconnect() override {
    if (listener_ != nil) {
      auto devicesAddress = globalAddress(kCMIOHardwarePropertyDevices);
      CMIOObjectRemovePropertyListenerBlock(kCMIOObjectSystemObject, &devicesAddress, listenerQueue_, listener_);
      listener_ = nil;
    }
    listenerQueue_ = nil;
    devicesChanged_.reset();
    if (streaming_) {
      CMIODeviceStopStream(device_, stream_);
      streaming_ = false;
    }
    if (stream_ != kCMIOObjectUnknown) {
      CMSimpleQueueRef unregistered = nullptr;  // drop the queue-altered proc (it names `this`)
      CMIOStreamCopyBufferQueue(stream_, nullptr, nullptr, &unregistered);
      if (unregistered != nullptr) CFRelease(unregistered);
    }
    if (queue_ != nullptr) { CFRelease(queue_); queue_ = nullptr; }
    if (pool_ != nullptr) { CVPixelBufferPoolRelease(pool_); pool_ = nullptr; }
    if (format_ != nullptr) { CFRelease(format_); format_ = nullptr; }
    stream_ = kCMIOObjectUnknown;
    device_ = kCMIOObjectUnknown;
  }

 private:
  std::string fail(std::string warning) {
    disconnect();
    return warning;
  }

  bool createPool() {
    CFMutableDictionaryRef pixel = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    setNumber(pixel, kCVPixelBufferPixelFormatTypeKey,
              static_cast<int32_t>(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange));
    setNumber(pixel, kCVPixelBufferWidthKey, kSinkWidth);
    setNumber(pixel, kCVPixelBufferHeightKey, kSinkHeight);
    CFDictionaryRef surface = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
                                                 &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(pixel, kCVPixelBufferIOSurfacePropertiesKey, surface);
    CFRelease(surface);
    CFMutableDictionaryRef poolAttributes = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    setNumber(poolAttributes, kCVPixelBufferPoolMinimumBufferCountKey, kPoolMinimumBuffers);
    const CVReturn result = CVPixelBufferPoolCreate(kCFAllocatorDefault, poolAttributes, pixel, &pool_);
    CFRelease(poolAttributes);
    CFRelease(pixel);
    return result == kCVReturnSuccess && pool_ != nullptr;
  }

  static void copyPlane(CVPixelBufferRef buffer, size_t plane, const uint8_t* source, int rowBytes, int rows) {
    auto* destination = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, plane));
    const size_t stride = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane);
    if (destination == nullptr) return;
    if (stride == static_cast<size_t>(rowBytes)) {
      std::memcpy(destination, source, static_cast<size_t>(rowBytes) * rows);
      return;
    }
    for (int row = 0; row < rows; ++row)
      std::memcpy(destination + stride * row, source + static_cast<size_t>(rowBytes) * row, rowBytes);
  }

  CMIODeviceID device_ = kCMIOObjectUnknown;
  CMIOStreamID stream_ = kCMIOObjectUnknown;
  CMSimpleQueueRef queue_ = nullptr;
  bool streaming_ = false;
  CVPixelBufferPoolRef pool_ = nullptr;
  CMVideoFormatDescriptionRef format_ = nullptr;
  std::shared_ptr<std::atomic<bool>> devicesChanged_;
  dispatch_queue_t listenerQueue_ = nil;
  CMIOObjectPropertyListenerBlock listener_ = nil;
};

}  // namespace

std::unique_ptr<IVirtualCameraSink> createCmioVirtualCameraSink() {
  return std::make_unique<CmioVirtualCameraSink>();
}

int64_t machHostClockNs() {
  static const mach_timebase_info_data_t timebase = [] {
    mach_timebase_info_data_t info{};
    mach_timebase_info(&info);
    return info;
  }();
  const unsigned __int128 ticks = mach_absolute_time();
  return static_cast<int64_t>(ticks * timebase.numer / timebase.denom);
}

}  // namespace corevideo::modules
