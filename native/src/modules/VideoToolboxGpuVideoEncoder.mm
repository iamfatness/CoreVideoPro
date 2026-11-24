// VideoToolbox GPU-direct encoder (macOS parity Phase 4, Task 2). Contract in
// VideoToolboxGpuVideoEncoder.h.

#include "modules/VideoToolboxGpuVideoEncoder.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace corevideo::modules {

namespace {

constexpr int kMaxInFlight = 2;
constexpr auto kPublishWait = std::chrono::milliseconds(34);  // Windows keyed-mutex consumer timeout
constexpr int kMaxConsecutiveErrors = 30;

int64_t to100ns(CMTime t) {
  if (!CMTIME_IS_NUMERIC(t)) return 0;
  return static_cast<int64_t>(CMTimeGetSeconds(CMTimeConvertScale(t, 10'000'000, kCMTimeRoundingMethod_RoundHalfAwayFromZero)) * 10'000'000.0 + 0.5);
}

// Exact 100 ns conversion without going through double for integer timescales.
int64_t exact100ns(CMTime t) {
  if (!CMTIME_IS_NUMERIC(t) || t.timescale == 0) return to100ns(t);
  return static_cast<int64_t>((static_cast<__int128>(t.value) * 10'000'000) / t.timescale);
}

}  // namespace

struct VideoToolboxGpuVideoEncoder::Impl : std::enable_shared_from_this<VideoToolboxGpuVideoEncoder::Impl> {
  GpuVideoEncoderConfig config;
  GpuEncodedChunkSink sink;
  VTCompressionSessionRef session = nullptr;
  dispatch_queue_t queue = nullptr;
  std::atomic<bool> running{false};
  std::atomic<bool> healthy{true};
  std::atomic<bool> keyframeRequested{false};
  std::atomic<int> inFlight{0};
  std::atomic<int64_t> skipped{0};
  std::atomic<int> consecutiveErrors{0};
  mutable std::mutex failureMutex;
  std::string failure;
  std::mutex stopMutex;
  // Held across submit's running check, in-flight increment and dispatch_async,
  // and by stop() while it flips `running`. Once stop() has flipped it, no new
  // block can be enqueued, so the drain that follows is final and `queue` /
  // `session` are never read concurrently with stop() tearing them down.
  std::mutex submitMutex;

  void setFailure(std::string value) {
    std::lock_guard<std::mutex> lock(failureMutex);
    failure = std::move(value);
  }

  // One frame's resources, owned until VideoToolbox hands the frame back.
  struct FrameContext {
    std::shared_ptr<Impl> owner;
    IOSurfaceRef surface = nullptr;
    int64_t frameNumber = 0;
  };

  static void outputCallback(void* /*outputRefcon*/, void* sourceFrameRefcon, OSStatus status,
                             VTEncodeInfoFlags infoFlags, CMSampleBufferRef sample) {
    auto* context = static_cast<FrameContext*>(sourceFrameRefcon);
    if (context == nullptr) return;
    std::shared_ptr<Impl> self = std::move(context->owner);
    const int64_t frameNumber = context->frameNumber;
    if (context->surface) {
      IOSurfaceDecrementUseCount(context->surface);
      CFRelease(context->surface);
    }
    delete context;
    if (!self) return;
    self->inFlight.fetch_sub(1);
    if (status != noErr) {
      self->noteEncodeError(status);
      return;
    }
    if ((infoFlags & kVTEncodeInfo_FrameDropped) != 0 || sample == nullptr) {
      self->skipped.fetch_add(1);
      return;
    }
    self->consecutiveErrors.store(0);
    self->emit(sample, frameNumber);
  }

  // VideoToolbox returned a frame we could not read: counted, never silent.
  void noteOutputLoss() {
    setFailure("vt-output-malformed");
    skipped.fetch_add(1);
  }

  void noteEncodeError(OSStatus status) {
    setFailure("vt-encode:" + std::to_string(static_cast<int>(status)));
    if (status == kVTInvalidSessionErr || status == kVTVideoEncoderMalfunctionErr ||
        consecutiveErrors.fetch_add(1) + 1 >= kMaxConsecutiveErrors) {
      healthy.store(false);
    }
  }

  // Length-prefixed (AVCC/HVCC) NAL units -> Annex B, parameter sets first on a keyframe.
  void emit(CMSampleBufferRef sample, int64_t frameNumber) {
    bool keyframe = true;
    if (CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        attachments && CFArrayGetCount(attachments) > 0) {
      auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
      keyframe = !CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync);
    }
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    const bool hevc = CMFormatDescriptionGetMediaSubType(format) == kCMVideoCodecType_HEVC;
    static const uint8_t kStart[4] = {0, 0, 0, 1};
    std::vector<uint8_t> out;
    int nalLengthSize = 4;
    {
      size_t count = 0;
      int headerLength = 4;
      const OSStatus st = hevc
          ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, nullptr, nullptr, &count, &headerLength)
          : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, nullptr, nullptr, &count, &headerLength);
      if (st == noErr && headerLength > 0) nalLengthSize = headerLength;
      if (keyframe && st == noErr) {
        for (size_t i = 0; i < count; ++i) {
          const uint8_t* ps = nullptr;
          size_t psSize = 0;
          const OSStatus pst = hevc
              ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, i, &ps, &psSize, nullptr, nullptr)
              : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &ps, &psSize, nullptr, nullptr);
          if (pst != noErr || ps == nullptr) continue;
          out.insert(out.end(), kStart, kStart + 4);
          out.insert(out.end(), ps, ps + psSize);
        }
      }
    }
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (block == nullptr) {
      noteOutputLoss();
      return;
    }
    const size_t total = CMBlockBufferGetDataLength(block);
    std::vector<uint8_t> data(total);
    if (CMBlockBufferCopyDataBytes(block, 0, total, data.data()) != kCMBlockBufferNoErr) {
      noteOutputLoss();
      return;
    }
    size_t offset = 0;
    while (offset + static_cast<size_t>(nalLengthSize) <= total) {
      uint32_t length = 0;
      for (int i = 0; i < nalLengthSize; ++i) length = (length << 8) | data[offset + i];
      offset += nalLengthSize;
      if (length == 0 || offset + length > total) {
        noteOutputLoss();  // a truncated access unit is never sent half-formed
        return;
      }
      out.insert(out.end(), kStart, kStart + 4);
      out.insert(out.end(), data.begin() + offset, data.begin() + offset + length);
      offset += length;
    }
    GpuEncodedChunk chunk;
    chunk.data = out.data();
    chunk.size = out.size();
    chunk.keyframe = keyframe;
    chunk.frameNumber = frameNumber;
    const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
    CMTime dts = CMSampleBufferGetDecodeTimeStamp(sample);
    if (!CMTIME_IS_NUMERIC(dts)) dts = pts;  // no reordering: decode order is presentation order
    chunk.pts100ns = exact100ns(pts);
    chunk.dts100ns = exact100ns(dts);
    chunk.duration100ns = exact100ns(CMSampleBufferGetDuration(sample));
    chunk.timingValid = CMTIME_IS_NUMERIC(pts);
    if (sink) sink(chunk);
  }

  // Runs on the serial queue.
  // `running` only gates NEW submits; frames already queued are encoded while
  // stop() drains, so a stop never silently loses an accepted frame.
  void encodeOne(uint32_t iosurfaceId, int64_t frameNumber, std::shared_ptr<std::atomic<int64_t>> published) {
    if (session == nullptr) {
      inFlight.fetch_sub(1);
      return;
    }
    if (published) {
      const auto deadline = std::chrono::steady_clock::now() + kPublishWait;
      while (published->load(std::memory_order_acquire) < frameNumber) {
        if (std::chrono::steady_clock::now() >= deadline) {
          skipped.fetch_add(1);
          inFlight.fetch_sub(1);
          return;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
      }
      if (published->load(std::memory_order_acquire) != frameNumber) {
        skipped.fetch_add(1);  // the slot was already rewritten with a newer picture
        inFlight.fetch_sub(1);
        return;
      }
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    IOSurfaceRef surface = IOSurfaceLookup(iosurfaceId);  // Task 0 row E-1: non-global surfaces resolve in-process
#pragma clang diagnostic pop
    if (surface == nullptr) {
      setFailure("iosurface-lookup-failed");
      skipped.fetch_add(1);
      inFlight.fetch_sub(1);
      return;
    }
    IOSurfaceIncrementUseCount(surface);
    // Re-check under our use count (Task 3 review): a slot the compositor began
    // rewriting between the check above and the increment no longer carries this
    // frame. Once the count is held the compositor will not choose the slot.
    if (published && published->load(std::memory_order_acquire) != frameNumber) {
      IOSurfaceDecrementUseCount(surface);
      CFRelease(surface);
      skipped.fetch_add(1);
      inFlight.fetch_sub(1);
      return;
    }
    CVPixelBufferRef pixels = nullptr;
    const CVReturn cv = CVPixelBufferCreateWithIOSurface(kCFAllocatorDefault, surface, nullptr, &pixels);
    if (cv != kCVReturnSuccess || pixels == nullptr) {
      IOSurfaceDecrementUseCount(surface);
      CFRelease(surface);
      setFailure("iosurface-lookup-failed");
      skipped.fetch_add(1);
      inFlight.fetch_sub(1);
      return;
    }
    // The compositor's BGRA is BT.709 full range; say so on the source so the
    // encoder's colour conversion and the stream's VUI agree.
    CVBufferSetAttachment(pixels, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixels, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(pixels, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    const int fps = std::max(1, config.fps);
    const CMTime pts = CMTimeMake(frameNumber * 10'000'000LL / fps, 10'000'000);
    const CMTime duration = CMTimeMake(10'000'000LL / fps, 10'000'000);
    CFDictionaryRef properties = nullptr;
    const bool forceKeyframe = keyframeRequested.exchange(false);
    if (forceKeyframe) {
      const void* keys[] = {kVTEncodeFrameOptionKey_ForceKeyFrame};
      const void* values[] = {kCFBooleanTrue};
      properties = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
    }
    auto* context = new FrameContext{shared_from_this(), surface, frameNumber};
    const OSStatus st = VTCompressionSessionEncodeFrame(session, pixels, pts, duration, properties, context, nullptr);
    if (properties) CFRelease(properties);
    CVPixelBufferRelease(pixels);
    if (st != noErr) {
      // The callback is not invoked for a frame the session refused.
      IOSurfaceDecrementUseCount(surface);
      CFRelease(surface);
      delete context;
      inFlight.fetch_sub(1);
      if (forceKeyframe) keyframeRequested.store(true);  // the IDR request survives a refused frame
      noteEncodeError(st);
    }
  }
};

VideoToolboxGpuVideoEncoder::VideoToolboxGpuVideoEncoder() : impl_(std::make_shared<Impl>()) {}

VideoToolboxGpuVideoEncoder::~VideoToolboxGpuVideoEncoder() { stop(); }

bool VideoToolboxGpuVideoEncoder::start(const GpuVideoEncoderConfig& config, GpuEncodedChunkSink sink) {
  stop();
  Impl& s = *impl_;
  s.config = config;
  s.sink = std::move(sink);
  s.healthy.store(true);
  s.consecutiveErrors.store(0);
  s.skipped.store(0);
  s.keyframeRequested.store(false);
  CMVideoCodecType codecType = 0;
  if (config.codec == "h264") {
    codecType = kCMVideoCodecType_H264;
  } else if (config.codec == "hevc" || config.codec == "h265") {
    codecType = kCMVideoCodecType_HEVC;
  } else {
    s.setFailure("vt-codec-unsupported:" + config.codec);
    return false;
  }
  if (config.width <= 0 || config.height <= 0) {
    s.setFailure("vt-session-create:" + std::to_string(static_cast<int>(kVTParameterErr)));
    return false;
  }
  NSDictionary* spec = @{(id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES};
  VTCompressionSessionRef session = nullptr;
  OSStatus st = VTCompressionSessionCreate(kCFAllocatorDefault, config.width, config.height, codecType,
                                           (__bridge CFDictionaryRef)spec, nullptr, kCFAllocatorDefault,
                                           &Impl::outputCallback, nullptr, &session);
  if (st != noErr || session == nullptr) {
    s.setFailure("vt-session-create:" + std::to_string(static_cast<int>(st)));
    return false;
  }
  const int fps = std::max(1, config.fps);
  const double keyframeSeconds = std::clamp(config.keyframeIntervalSeconds, 0.5, 10.0);
  const int bitsPerSecond = std::max(1, config.bitrateKbps) * 1000;
  auto set = [&](CFStringRef key, CFTypeRef value, const char* name) -> bool {
    const OSStatus status = VTSessionSetProperty(session, key, value);
    if (status != noErr) {
      s.setFailure(std::string("vt-property:") + name + ":" + std::to_string(static_cast<int>(status)));
      return false;
    }
    return true;
  };
  CFStringRef profile = nullptr;
  if (codecType == kCMVideoCodecType_HEVC) {
    profile = kVTProfileLevel_HEVC_Main_AutoLevel;
  } else if (config.h264Profile == "main") {
    profile = kVTProfileLevel_H264_Main_AutoLevel;
  } else if (config.h264Profile == "baseline") {
    profile = kVTProfileLevel_H264_Baseline_AutoLevel;
  } else {
    profile = kVTProfileLevel_H264_High_AutoLevel;  // "high" | "auto"
  }
  bool ok = set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue, "RealTime") &&
            set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse, "AllowFrameReordering") &&
            set(kVTCompressionPropertyKey_ProfileLevel, profile, "ProfileLevel") &&
            set(kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFTypeRef)@(fps), "ExpectedFrameRate") &&
            set(kVTCompressionPropertyKey_MaxKeyFrameInterval,
                (__bridge CFTypeRef)@(configuredGopFrames(fps, keyframeSeconds)), "MaxKeyFrameInterval") &&
            set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
                (__bridge CFTypeRef)@(keyframeSeconds), "MaxKeyFrameIntervalDuration") &&
            set(kVTCompressionPropertyKey_ColorPrimaries, kCVImageBufferColorPrimaries_ITU_R_709_2, "ColorPrimaries") &&
            set(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2,
                "TransferFunction") &&
            set(kVTCompressionPropertyKey_YCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2, "YCbCrMatrix");
  if (ok) {
    // AverageBitRate always; CBR adds ConstantBitRate (Task 0 row E-2: accepted
    // by the hardware H.264 and HEVC encoders); VBR gets a one-second cap at
    // 1.5x for headroom. Measured 2026-11-23 (Task 2): CBR holds 4 Mbps to
    // within 0.2% on structured 720p30 content that changes every frame.
    ok = set(kVTCompressionPropertyKey_AverageBitRate, (__bridge CFTypeRef)@(bitsPerSecond), "AverageBitRate");
    if (ok && config.rateControl == "vbr") {
      NSArray* limits = @[@(static_cast<long long>(bitsPerSecond) * 3 / 16), @1.0];
      ok = set(kVTCompressionPropertyKey_DataRateLimits, (__bridge CFTypeRef)limits, "DataRateLimits");
    } else if (ok) {
      ok = set(kVTCompressionPropertyKey_ConstantBitRate, (__bridge CFTypeRef)@(bitsPerSecond), "ConstantBitRate");
    }
  }
  if (ok) {
    st = VTCompressionSessionPrepareToEncodeFrames(session);
    if (st != noErr) {
      s.setFailure("vt-prepare:" + std::to_string(static_cast<int>(st)));
      ok = false;
    }
  }
  if (!ok) {
    VTCompressionSessionInvalidate(session);
    CFRelease(session);
    return false;
  }
  s.session = session;
  s.queue = dispatch_queue_create("us.iamfatness.corevideo.vt-encode", DISPATCH_QUEUE_SERIAL);
  s.setFailure("");
  std::lock_guard<std::mutex> gate(s.submitMutex);
  s.running.store(true);
  return true;
}

bool VideoToolboxGpuVideoEncoder::submit(const GpuVideoEncoderFrame& frame) {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> gate(s.submitMutex);
  if (!s.running.load() || !s.healthy.load()) return false;
  if (frame.iosurfaceId == 0) {
    s.skipped.fetch_add(1);
    return true;
  }
  if (s.inFlight.fetch_add(1) >= kMaxInFlight) {
    s.inFlight.fetch_sub(1);
    s.skipped.fetch_add(1);
    return true;
  }
  std::shared_ptr<Impl> owner = impl_;
  const uint32_t id = frame.iosurfaceId;
  const int64_t number = frame.frameNumber;
  auto published = frame.publishedFrameNumber;
  dispatch_async(s.queue, ^{
    owner->encodeOne(id, number, published);
  });
  return true;
}

bool VideoToolboxGpuVideoEncoder::requestKeyframe() {
  impl_->keyframeRequested.store(true);
  return true;
}

void VideoToolboxGpuVideoEncoder::stop() {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> lock(s.stopMutex);
  bool wasRunning = false;
  {
    std::lock_guard<std::mutex> gate(s.submitMutex);
    wasRunning = s.running.exchange(false);
  }
  if (s.queue) {
    dispatch_sync(s.queue, ^{});  // drain: queued frames see running=false and release
  }
  if (s.session) {
    if (wasRunning) VTCompressionSessionCompleteFrames(s.session, kCMTimeInvalid);
    VTCompressionSessionInvalidate(s.session);
    CFRelease(s.session);
    s.session = nullptr;
  }
  s.queue = nullptr;  // ARC releases the dispatch queue
}

bool VideoToolboxGpuVideoEncoder::healthy() const { return impl_->healthy.load(); }

std::string VideoToolboxGpuVideoEncoder::lastFailure() const {
  std::lock_guard<std::mutex> lock(impl_->failureMutex);
  return impl_->failure;
}

int64_t VideoToolboxGpuVideoEncoder::framesSkipped() const { return impl_->skipped.load(); }

int VideoToolboxGpuVideoEncoder::framesInFlight() const { return impl_->inFlight.load(); }

std::unique_ptr<GpuVideoEncoder> createVideoToolboxGpuVideoEncoder() {
  return std::make_unique<VideoToolboxGpuVideoEncoder>();
}

}  // namespace corevideo::modules
