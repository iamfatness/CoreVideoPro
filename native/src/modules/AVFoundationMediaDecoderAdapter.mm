// AVFoundation media clip decoder — the macOS side of the reader seam declared
// in MediaClipDecoder.h. Every playback DECISION (cue poster, pause hold,
// resume, loop seam, end, frame ids, position) lives in the portable
// MediaClipDecoder; this file only opens a file, decodes samples on a private
// serial queue, copies them into tightly packed BGRA, hands them over one at a
// time and cancels cleanly.
//
// WebM (and anything else AVFoundation cannot open) is refused LOUDLY: there is
// no FFmpeg in this build (plan default D1; slice N5 adds it behind this same
// seam).

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AVF_MEDIA_DECODER

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/BoundedAsyncLog.h"
#include "core/FrameAllocation.h"
#include "modules/Interfaces.h"
#include "modules/MediaClipDecoder.h"
#include "modules/PixelBufferVideoFrame.h"
#include "modules/StillMediaFrameCache.h"

namespace corevideo::modules {

namespace {

constexpr const char* kWebmRefusal =
    "AVFoundation cannot open .webm files; WebM playback on Mac needs FFmpeg, which this build does not use yet.";

// How many decoded samples a reader holds beyond the one the owner is taking:
// "one sample beyond the owner's request". Each is a full BGRA frame (8.3 MB at
// 1080p), and the owner already queues up to three, so more here buys memory
// and nothing else.
constexpr size_t kSamplesDecodedAhead = 1;

// Every decoded frame copied into CPU BGRA, process-wide. The zero-copy path
// (N4) must leave this untouched; the test reads it, production never does.
std::atomic<uint64_t> g_cpuFrameCopies{0};

std::string lowercaseExtension(const std::string& path) {
  auto extension = std::filesystem::path(path).extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return extension;
}

int64_t to100ns(CMTime time) {
  if (!CMTIME_IS_NUMERIC(time)) return -1;
  return static_cast<int64_t>(CMTimeGetSeconds(time) * 10'000'000.0 + 0.5);
}

std::string describe(NSError* error) {
  const char* text = error ? error.localizedDescription.UTF8String : nullptr;
  return text && *text ? std::string(text) : std::string("unknown error");
}

double elapsedMs(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

// Everything a reader's queue blocks touch. Blocks hold this by shared_ptr and
// never the reader object, so a block still queued when the reader is destroyed
// touches valid memory, sees `cancelled`, and does nothing.
struct VideoReaderState {
  std::string path;
  std::function<void()> wake;  // called on the reader queue with NO lock held
  std::mutex mutex;
  // Guarded by `mutex`.
  std::deque<ClipVideoSample> ready;  // at most kSamplesDecodedAhead wait for take()
  bool endOfStream = false;
  bool failed = false;
  bool noVideoTrack = false;  // the file opened and has no video track (an audio bed)
  std::string error;
  AVAssetReader* reader = nil;  // also read by the destructor, to cancel
  AVURLAsset* asset = nil;      // likewise, to cancel a load in flight
  // Set before the reader starts, read on the reader queue only. ZERO-COPY
  // (N4): hand the decoder's IOSurface-backed buffer over retained, instead of
  // copying it into CPU BGRA.
  bool zeroCopy = false;
  // Reader-queue only. Always a plain track output; a rotated track is turned
  // upright per sample by `rotator` (review round 2026-11-22).
  AVAssetReaderOutput* output = nil;
  std::unique_ptr<struct SampleRotator> rotator;
  bool straightenAlpha = false;
  bool loggedZeroCopyRefusal = false;
  int64_t lastPts100ns = -1;
  int64_t lastDuration100ns = 0;
  double copyTotalMs = 0, copyMaxMs = 0;
  int copyCount = 0;
  // Any thread.
  std::atomic<bool> cancelled{false};
  std::atomic<int64_t> duration100ns{-1};
};

void wakeOwner(const std::shared_ptr<VideoReaderState>& state) {
  if (!state->cancelled.load() && state->wake) state->wake();
}

void failReader(const std::shared_ptr<VideoReaderState>& state, std::string error) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->failed = true;
    state->error = std::move(error);
  }
  wakeOwner(state);
}

// The async loaders, awaited on the reader's PRIVATE queue (never the caller's
// thread, never the deprecated synchronous accessors). The wait is sliced so a
// destroyed reader does not hold its queue for the length of a slow open.
bool waitUnlessCancelled(dispatch_semaphore_t done, const std::atomic<bool>& cancelled) {
  while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC)) != 0) {
    if (cancelled.load()) return false;
  }
  return true;
}

NSArray<AVAssetTrack*>* loadTracks(AVURLAsset* asset, AVMediaType type, const std::atomic<bool>& cancelled,
                                   NSError* __autoreleasing* error) {
  __block NSArray<AVAssetTrack*>* result = nil;
  __block NSError* loadError = nil;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  [asset loadTracksWithMediaType:type
               completionHandler:^(NSArray<AVAssetTrack*>* tracks, NSError* failure) {
                 result = tracks;
                 loadError = failure;
                 dispatch_semaphore_signal(done);
               }];
  if (!waitUnlessCancelled(done, cancelled)) return nil;
  if (error) *error = loadError;
  return result;
}

template <typename ReaderState>
void loadDuration(AVURLAsset* asset, const std::shared_ptr<ReaderState>& state) {
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  [asset loadValuesAsynchronouslyForKeys:@[ @"duration" ] completionHandler:^{ dispatch_semaphore_signal(done); }];
  if (!waitUnlessCancelled(done, state->cancelled)) return;
  NSError* error = nil;
  if ([asset statusOfValueForKey:@"duration" error:&error] == AVKeyValueStatusLoaded) {
    const auto duration = to100ns(asset.duration);
    if (duration > 0) state->duration100ns.store(duration);
  }
}

bool trackContainsAlpha(AVAssetTrack* track) {
  for (id description in track.formatDescriptions) {
    const auto format = (__bridge CMFormatDescriptionRef)description;
    const auto value = CMFormatDescriptionGetExtension(format, kCMFormatDescriptionExtension_ContainsAlphaChannel);
    if (value && CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)value)) return true;
  }
  return false;
}

struct TrackTransform {
  bool rotated = false;
  CGAffineTransform transform = CGAffineTransformIdentity;
};

// The track property a rotation needs, loaded asynchronously on the reader
// queue like every other asset property here.
TrackTransform loadTrackTransform(AVAssetTrack* track, const std::atomic<bool>& cancelled) {
  TrackTransform result;
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  NSArray<NSString*>* keys = @[ @"preferredTransform" ];
  [track loadValuesAsynchronouslyForKeys:keys completionHandler:^{ dispatch_semaphore_signal(done); }];
  if (!waitUnlessCancelled(done, cancelled)) return result;
  NSError* error = nil;
  if ([track statusOfValueForKey:@"preferredTransform" error:&error] != AVKeyValueStatusLoaded) return result;
  result.transform = track.preferredTransform;
  result.rotated = !CGAffineTransformIsIdentity(result.transform);
  return result;
}

// A ROTATED TRACK (a phone held upright, plan 2026-11-21 Task 2), turned upright
// ONE DECODED SAMPLE AT A TIME on the GPU (Core Image on the system Metal device).
// Review round 2026-11-22 replaced AVAssetReaderVideoCompositionOutput here
// because that output (1) forces alpha to 255, so a rotated ProRes 4444
// lower-third covered the scene with its fill colour, and (2) re-times the clip
// onto a composition frameDuration, so a variable-frame-rate phone clip lost or
// gained frames (measured: 15 frames out for 16 samples). Here every source
// sample becomes exactly one upright sample at its own timestamp, and alpha is
// carried through untouched: colour management is off end to end and the
// pixels are never premultiplied or unpremultiplied, so the owner's STRAIGHT
// alpha contract holds (AvfMediaDecoder.ARotatedAlphaClipStaysUprightAndKeepsItsStraightAlpha).
// The output is an IOSurface-backed, Metal-compatible pool buffer, so the
// zero-copy path wraps it exactly like a decoder buffer, and the pool cannot
// recycle a buffer a frame (or a Metal pass) still retains.
struct SampleRotator {
  CIContext* context = nil;
  CGAffineTransform storageToDisplay = CGAffineTransformIdentity;  // y-down, as preferredTransform
  size_t storageWidth = 0, storageHeight = 0;
  size_t displayWidth = 0, displayHeight = 0;
  CGAffineTransform ciTransform = CGAffineTransformIdentity;  // y-up Core Image space
  bool exactQuarterTurn = false;  // a multiple of 90 degrees and/or a flip: sample nearest, bit-exact
  CVPixelBufferPoolRef pool = nullptr;
  ~SampleRotator() { if (pool) CVPixelBufferPoolRelease(pool); }

  // Lazily sized from the first decoded buffer, so the geometry is the buffer's,
  // never a container's naturalSize that can disagree with it.
  bool prepare(size_t width, size_t height, std::string& error) {
    if (pool && width == storageWidth && height == storageHeight) return true;
    if (pool) { CVPixelBufferPoolRelease(pool); pool = nullptr; }
    storageWidth = width; storageHeight = height;
    const auto w = static_cast<CGFloat>(width), h = static_cast<CGFloat>(height);
    const CGRect display = CGRectApplyAffineTransform(CGRectMake(0, 0, w, h), storageToDisplay);
    displayWidth = static_cast<size_t>(std::lround(std::abs(display.size.width)));
    displayHeight = static_cast<size_t>(std::lround(std::abs(display.size.height)));
    if (displayWidth == 0 || displayHeight == 0) { error = "the track's transform has no area"; return false; }
    // preferredTransform maps storage to display in y-DOWN pixels; Core Image is
    // y-UP. Flip into storage y-down, apply the transform moved back into
    // positive space, flip out of display y-down.
    const CGAffineTransform flipIn = CGAffineTransformMake(1, 0, 0, -1, 0, h);
    const CGAffineTransform toOrigin = CGAffineTransformMakeTranslation(-display.origin.x, -display.origin.y);
    const CGAffineTransform flipOut = CGAffineTransformMake(1, 0, 0, -1, 0, static_cast<CGFloat>(displayHeight));
    ciTransform = CGAffineTransformConcat(
        CGAffineTransformConcat(CGAffineTransformConcat(flipIn, storageToDisplay), toOrigin), flipOut);
    const auto unit = [](CGFloat v) { return std::abs(v) < 1e-6 || std::abs(std::abs(v) - 1) < 1e-6; };
    const auto whole = [](CGFloat v) { return std::abs(v - std::round(v)) < 1e-6; };
    const auto& t = ciTransform;
    exactQuarterTurn = unit(t.a) && unit(t.b) && unit(t.c) && unit(t.d) && whole(t.tx) && whole(t.ty);
    NSDictionary* attributes = @{
      (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
      (id)kCVPixelBufferWidthKey : @(displayWidth),
      (id)kCVPixelBufferHeightKey : @(displayHeight),
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
      (id)kCVPixelBufferMetalCompatibilityKey : @YES,
    };
    if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attributes, &pool) !=
            kCVReturnSuccess || !pool) {
      pool = nullptr;
      error = "the rotation buffer pool could not be created";
      return false;
    }
    if (!context) {
      context = [CIContext contextWithOptions:@{
        kCIContextWorkingColorSpace : [NSNull null],
        kCIContextOutputColorSpace : [NSNull null],
        kCIContextUseSoftwareRenderer : @NO,
        kCIContextCacheIntermediates : @NO,
      }];
      if (!context) { error = "Core Image is unavailable"; return false; }
    }
    return true;
  }

  // +1 retained upright buffer, or nullptr with `error` set.
  CVPixelBufferRef rotate(CVImageBufferRef source, std::string& error) {
    if (!prepare(CVPixelBufferGetWidth(source), CVPixelBufferGetHeight(source), error)) return nullptr;
    CVPixelBufferRef out = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &out) != kCVReturnSuccess || !out) {
      error = "no rotation buffer was available";
      return nullptr;
    }
    CIImage* image = [CIImage imageWithCVPixelBuffer:source options:@{ kCIImageColorSpace : [NSNull null] }];
    if (exactQuarterTurn) image = [image imageBySamplingNearest];
    image = [image imageByApplyingTransform:ciTransform];
    CIRenderDestination* destination = [[CIRenderDestination alloc] initWithPixelBuffer:out];
    destination.colorSpace = nil;
    // Core Image reads a BGRA buffer as STRAIGHT alpha and premultiplies it for
    // its working space; writing Unpremultiplied hands the owner straight alpha back.
    destination.alphaMode = CIRenderDestinationAlphaUnpremultiplied;
    NSError* failure = nil;
    CIRenderTask* task = [context startTaskToRender:image
                                           fromRect:CGRectMake(0, 0, displayWidth, displayHeight)
                                      toDestination:destination
                                            atPoint:CGPointZero
                                              error:&failure];
    if (!task || ![task waitUntilCompletedAndReturnError:&failure]) {
      CVPixelBufferRelease(out);
      error = std::string("Core Image could not rotate a frame (") +
              (failure.localizedDescription.UTF8String ?: "unknown") + ")";
      return nullptr;
    }
    CVBufferPropagateAttachments(source, out);
    return out;
  }
};

// Copies one decoded pixel buffer into a tightly packed BGRA vector
// (stride = width * 4). nullptr = the allocation failed; the sample is dropped
// and the clip holds its last frame (the MF adapter's behaviour, MF:1171-1178).
std::shared_ptr<const std::vector<uint8_t>> copyBgra(CVImageBufferRef image, int& width, int& height,
                                                    bool straightenAlpha) {
  static core::FrameAllocationFailures allocationFailures("media-decoder-avf");
  g_cpuFrameCopies.fetch_add(1, std::memory_order_relaxed);
  width = static_cast<int>(CVPixelBufferGetWidth(image));
  height = static_cast<int>(CVPixelBufferGetHeight(image));
  if (width <= 0 || height <= 0) return nullptr;
  const size_t rowBytes = static_cast<size_t>(width) * 4u;
  const size_t frameBytes = rowBytes * static_cast<size_t>(height);
  auto pixels = core::tryMakeFrameBuffer(frameBytes);
  if (!pixels) {
    allocationFailures.note(frameBytes);
    return nullptr;
  }
  if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return nullptr;
  const auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));
  const size_t stride = CVPixelBufferGetBytesPerRow(image);
  if (base && stride >= rowBytes) {
    for (int y = 0; y < height; ++y) {
      std::memcpy(pixels->data() + static_cast<size_t>(y) * rowBytes, base + static_cast<size_t>(y) * stride, rowBytes);
    }
  }
  CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
  if (!base || stride < rowBytes) return nullptr;
  if (straightenAlpha) {
    // Alpha codecs only. The owner's contract is STRAIGHT alpha.
    auto* data = pixels->data();
    for (size_t offset = 0; offset < frameBytes; offset += 4) {
      const unsigned alpha = data[offset + 3];
      if (alpha == 0 || alpha == 255) continue;
      for (int channel = 0; channel < 3; ++channel) {
        data[offset + channel] = static_cast<uint8_t>((std::min)(255u, (data[offset + channel] * 255u + alpha / 2) / alpha));
      }
    }
  }
  return pixels;
}

// Runs on the reader queue. Decodes until kSamplesDecodedAhead samples are
// ready (or the reader ends or fails), then stops: take() schedules the next.
void decodeNext(const std::shared_ptr<VideoReaderState>& state) {
  for (;;) {
    @autoreleasepool {
      if (state->cancelled.load() || !state->output) return;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->ready.size() >= kSamplesDecodedAhead || state->endOfStream || state->failed) return;
      }
      CMSampleBufferRef sampleBuffer = [state->output copyNextSampleBuffer];
      if (!sampleBuffer) {
        AVAssetReader* reader = nil;
        { std::lock_guard<std::mutex> lock(state->mutex); reader = state->reader; }
        if (state->cancelled.load() || reader.status == AVAssetReaderStatusCancelled) return;
        if (reader.status == AVAssetReaderStatusCompleted) {
          { std::lock_guard<std::mutex> lock(state->mutex); state->endOfStream = true; }
          wakeOwner(state);
        } else {
          failReader(state, "AVFoundation stopped decoding (" + describe(reader.error) + ")");
        }
        return;
      }
      CVImageBufferRef image = CMSampleBufferGetImageBuffer(sampleBuffer);
      if (!image) { CFRelease(sampleBuffer); continue; }  // a marker sample with no picture
      const auto copyStart = std::chrono::steady_clock::now();
      CVPixelBufferRef upright = nullptr;  // +1 when the track is rotated; released below
      if (state->rotator) {
        std::string rotateError;
        upright = state->rotator->rotate(image, rotateError);
        if (!upright) {
          CFRelease(sampleBuffer);
          failReader(state, "AVFoundation cannot play this rotated clip (" + rotateError + ")");
          return;
        }
        image = upright;
      }
      ClipVideoSample decoded;
      if (state->zeroCopy) {
        // ZERO-COPY (N4): the buffer itself, retained by PixelBufferVideoFrame
        // until the last frame AND the last Metal pass sampling it let go, so
        // the decoder's pool can never recycle it under a reader.
        decoded.gpu = makePixelBufferVideoFrame(image);
        if (decoded.gpu) {
          decoded.width = decoded.gpu->width;
          decoded.height = decoded.gpu->height;
        } else if (!state->loggedZeroCopyRefusal) {
          state->loggedZeroCopyRefusal = true;
          ::corevideo::core::nativeLogf("[media-decoder] %s: decoded buffer is not an IOSurface-backed BGRA buffer; "
                                        "copying to CPU BGRA instead of zero-copy\n", state->path.c_str());
        }
      }
      if (!decoded.gpu) decoded.bgra = copyBgra(image, decoded.width, decoded.height, state->straightenAlpha);
      const auto copyMs = elapsedMs(copyStart);
      decoded.pts100ns = (std::max)(int64_t{0}, to100ns(CMSampleBufferGetPresentationTimeStamp(sampleBuffer)));
      auto duration = to100ns(CMSampleBufferGetDuration(sampleBuffer));
      if (duration <= 0) {
        duration = state->lastPts100ns >= 0 && decoded.pts100ns > state->lastPts100ns
            ? decoded.pts100ns - state->lastPts100ns : state->lastDuration100ns;
      }
      decoded.duration100ns = duration;
      state->lastPts100ns = decoded.pts100ns;
      state->lastDuration100ns = duration;
      if (upright) CVPixelBufferRelease(upright);  // a zero-copy frame holds its own retain
      CFRelease(sampleBuffer);
      if (!decoded.bgra && !decoded.gpu) continue;  // allocation failed: drop this sample, keep reading
      // The measurement the load drill reads: what the CPU copy costs per frame.
      state->copyTotalMs += copyMs;
      state->copyMaxMs = (std::max)(state->copyMaxMs, copyMs);
      if (++state->copyCount >= 600) {
        ::corevideo::core::nativeVerboseLogf("[media-decoder] copy avg %.2f ms max %.2f ms (%d frames, %dx%d)\n",
            state->copyTotalMs / state->copyCount, state->copyMaxMs, state->copyCount, decoded.width, decoded.height);
        state->copyTotalMs = 0; state->copyMaxMs = 0; state->copyCount = 0;
      }
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->ready.push_back(std::move(decoded));
      }
      wakeOwner(state);
    }
  }
}

// Runs on the reader queue.
void openVideo(const std::shared_ptr<VideoReaderState>& state) {
  @autoreleasepool {
    if (state->cancelled.load()) return;
    if (lowercaseExtension(state->path) == ".webm") { failReader(state, kWebmRefusal); return; }
    std::error_code exists;
    if (!std::filesystem::exists(std::filesystem::path(state->path), exists)) {
      failReader(state, "the file does not exist");
      return;
    }
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:state->path.c_str()]];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
    { std::lock_guard<std::mutex> lock(state->mutex); state->asset = asset; }
    NSError* error = nil;
    NSArray<AVAssetTrack*>* tracks = loadTracks(asset, AVMediaTypeVideo, state->cancelled, &error);
    if (state->cancelled.load()) return;
    if (!tracks || error) {
      failReader(state, "AVFoundation cannot open this file (" + describe(error) + ")");
      return;
    }
    if (tracks.count == 0) {
      // An audio bed, not a failure (N3): MediaClipDecoder gives it a
      // transparent picture and plays its audio.
      { std::lock_guard<std::mutex> lock(state->mutex); state->noVideoTrack = true; }
      wakeOwner(state);
      return;
    }
    loadDuration(asset, state);
    if (state->cancelled.load()) return;
    AVAssetTrack* track = tracks.firstObject;
    // An opaque track is opaque: no per-pixel alpha pass (measured: VideoToolbox
    // delivers 0xFF, AvfMediaDecoder.ACuePosterIsTheFirstFrameAndOpaque).
    const bool hasAlpha = trackContainsAlpha(track);
    const auto transform = loadTrackTransform(track, state->cancelled);
    if (state->cancelled.load()) return;
    AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    if (!reader) {
      failReader(state, "AVFoundation cannot open this file (" + describe(error) + ")");
      return;
    }
    NSDictionary* colour = @{
      AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
      AVVideoTransferFunctionKey : AVVideoTransferFunction_ITU_R_709_2,
      AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2,
    };
    NSMutableDictionary* settings = [@{
      (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
      AVVideoColorPropertiesKey : colour,
    } mutableCopy];
    if (state->zeroCopy) {
      // Buffers Metal can wrap as a texture in place (N4).
      settings[(id)kCVPixelBufferIOSurfacePropertiesKey] = @{};
      settings[(id)kCVPixelBufferMetalCompatibilityKey] = @YES;
    }
    if (transform.rotated) {
      // Plan default P3, kept: only a non-identity transform pays for a
      // rotation, so every unrotated clip keeps the plain output byte for byte.
      state->rotator = std::make_unique<SampleRotator>();
      state->rotator->storageToDisplay = transform.transform;
    }
    AVAssetReaderOutput* output = nil;
    @try {
      AVAssetReaderTrackOutput* plain = [[AVAssetReaderTrackOutput alloc] initWithTrack:track outputSettings:settings];
      plain.alwaysCopiesSampleData = NO;
      output = plain;
    } @catch (NSException* exception) {
      failReader(state, std::string("AVFoundation cannot open this file (") +
                            (exception.reason.UTF8String ?: "invalid reader settings") + ")");
      return;
    }
    if (![reader canAddOutput:output]) {
      failReader(state, "AVFoundation cannot open this file (the video track cannot be decoded to BGRA)");
      return;
    }
    [reader addOutput:output];
    if (![reader startReading]) {
      failReader(state, "AVFoundation cannot open this file (" + describe(reader.error) + ")");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->reader = reader;
    }
    state->output = output;
    // Measured on macOS 26.3 (AvfMediaDecoder.AProRes4444AlphaClipReachesTheOwnerAsStraightAlpha):
    // VideoToolbox hands a ProRes 4444 frame over as STRAIGHT alpha already, so
    // no conversion runs. `straightenAlpha` stays as the one switch to flip if
    // a codec is ever measured premultiplied.
    state->straightenAlpha = false;
    (void)hasAlpha;
  }
  decodeNext(state);
}

class AvfClipVideoReader final : public IClipVideoReader {
 public:
  AvfClipVideoReader(const std::string& path, std::function<void()> wake, bool zeroCopy)
      : state_(std::make_shared<VideoReaderState>()),
        queue_(dispatch_queue_create("com.corevideo.media-decoder.video",
                                     dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                                                             QOS_CLASS_USER_INITIATED, 0))) {
    state_->path = path;
    state_->wake = std::move(wake);
    state_->zeroCopy = zeroCopy;
    const auto state = state_;
    dispatch_async(queue_, ^{ openVideo(state); });
  }

  // Cancels, then DRAINS the queue: nothing this reader scheduled can run (and
  // no wake can fire) after the destructor returns.
  //
  // cancelReading runs ON THE READER QUEUE, after any decode in flight. Calling
  // it from this thread while decodeNext was inside copyNextSampleBuffer
  // crashed (EXC_BAD_ACCESS in FigSimpleMutexUnlock): AVAssetReader does not
  // support those two concurrently. cancelLoading stays here, because it is
  // what unblocks an open that is still waiting on the asset.
  ~AvfClipVideoReader() override {
    state_->cancelled.store(true);
    AVURLAsset* asset = nil;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      asset = state_->asset;
    }
    [asset cancelLoading];
    const auto state = state_;
    dispatch_sync(queue_, ^{
      AVAssetReader* reader = nil;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        reader = state->reader;
      }
      [reader cancelReading];
    });
  }

  ClipReadStatus take(ClipVideoSample& out, std::string& error) override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->ready.empty()) {
      out = std::move(state_->ready.front());
      state_->ready.pop_front();
      // Refill: keep kSamplesDecodedAhead decoded ahead of the owner's request.
      const auto state = state_;
      dispatch_async(queue_, ^{ decodeNext(state); });
      return ClipReadStatus::Ready;
    }
    if (state_->failed) { error = state_->error; return ClipReadStatus::Failed; }
    if (state_->noVideoTrack) return ClipReadStatus::NoVideoTrack;
    if (state_->endOfStream) return ClipReadStatus::EndOfStream;
    return ClipReadStatus::Pending;
  }

  int64_t duration100ns() const override { return state_->duration100ns.load(); }

 private:
  std::shared_ptr<VideoReaderState> state_;
  dispatch_queue_t queue_;
};

// ---- Audio: the second reader. Same queue, one-ahead, wake and teardown rules
// as video. AVFoundation resamples to 48 kHz and up/down-mixes to stereo, so
// every sample handed over is interleaved float32 48000 Hz stereo.

struct AudioReaderState {
  std::string path;
  int64_t start100ns = 0;
  std::function<void()> wake;
  std::mutex mutex;
  // Guarded by `mutex`.
  std::deque<ClipAudioSample> ready;
  bool endOfStream = false;
  bool failed = false;
  std::string error;
  AVAssetReader* reader = nil;
  AVURLAsset* asset = nil;
  // Reader-queue only.
  AVAssetReaderOutput* output = nil;
  // Any thread.
  std::atomic<bool> cancelled{false};
  std::atomic<int64_t> duration100ns{-1};
};

void wakeOwner(const std::shared_ptr<AudioReaderState>& state) {
  if (!state->cancelled.load() && state->wake) state->wake();
}

void failReader(const std::shared_ptr<AudioReaderState>& state, std::string error) {
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->failed = true;
    state->error = std::move(error);
  }
  wakeOwner(state);
}

void endReader(const std::shared_ptr<AudioReaderState>& state) {
  { std::lock_guard<std::mutex> lock(state->mutex); state->endOfStream = true; }
  wakeOwner(state);
}

// Runs on the reader queue.
void decodeNextAudio(const std::shared_ptr<AudioReaderState>& state) {
  for (;;) {
    @autoreleasepool {
      if (state->cancelled.load() || !state->output) return;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->ready.size() >= kSamplesDecodedAhead || state->endOfStream || state->failed) return;
      }
      CMSampleBufferRef sampleBuffer = [state->output copyNextSampleBuffer];
      if (!sampleBuffer) {
        AVAssetReader* reader = nil;
        { std::lock_guard<std::mutex> lock(state->mutex); reader = state->reader; }
        if (state->cancelled.load() || reader.status == AVAssetReaderStatusCancelled) return;
        if (reader.status == AVAssetReaderStatusCompleted) endReader(state);
        else failReader(state, "AVFoundation stopped decoding audio (" + describe(reader.error) + ")");
        return;
      }
      ClipAudioSample decoded;
      decoded.pts100ns = (std::max)(int64_t{0}, to100ns(CMSampleBufferGetPresentationTimeStamp(sampleBuffer)));
      CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sampleBuffer);
      const size_t bytes = block ? CMBlockBufferGetDataLength(block) : 0;
      // Whole stereo frames only (2 x float32).
      const size_t floats = (bytes / (2 * sizeof(float))) * 2;
      if (floats > 0) {
        decoded.pcm.resize(floats);
        if (CMBlockBufferCopyDataBytes(block, 0, floats * sizeof(float), decoded.pcm.data()) != kCMBlockBufferNoErr) {
          decoded.pcm.clear();
        }
      }
      CFRelease(sampleBuffer);
      if (decoded.pcm.empty()) continue;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->ready.push_back(std::move(decoded));
      }
      wakeOwner(state);
    }
  }
}

// Runs on the reader queue.
void openAudio(const std::shared_ptr<AudioReaderState>& state) {
  @autoreleasepool {
    if (state->cancelled.load()) return;
    if (lowercaseExtension(state->path) == ".webm") { failReader(state, kWebmRefusal); return; }
    std::error_code exists;
    if (!std::filesystem::exists(std::filesystem::path(state->path), exists)) {
      failReader(state, "the file does not exist");
      return;
    }
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:state->path.c_str()]];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
    { std::lock_guard<std::mutex> lock(state->mutex); state->asset = asset; }
    NSError* error = nil;
    NSArray<AVAssetTrack*>* tracks = loadTracks(asset, AVMediaTypeAudio, state->cancelled, &error);
    if (state->cancelled.load()) return;
    if (!tracks || error) {
      failReader(state, "AVFoundation cannot open this file (" + describe(error) + ")");
      return;
    }
    // No audio track is NOT a failure: the reader ends without one sample and
    // MediaClipDecoder treats the clip as silent, unwarned.
    if (tracks.count == 0) { endReader(state); return; }
    loadDuration(asset, state);  // an audio bed's only duration (N3)
    if (state->cancelled.load()) return;
    AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    if (!reader) {
      failReader(state, "AVFoundation cannot open this file (" + describe(error) + ")");
      return;
    }
    if (state->start100ns > 0) {
      reader.timeRange = CMTimeRangeMake(CMTimeMake(state->start100ns, 10'000'000), kCMTimePositiveInfinity);
    }
    NSDictionary* settings = @{
      AVFormatIDKey : @(kAudioFormatLinearPCM),
      AVSampleRateKey : @48000,
      AVNumberOfChannelsKey : @2,
      AVLinearPCMBitDepthKey : @32,
      AVLinearPCMIsFloatKey : @YES,
      AVLinearPCMIsBigEndianKey : @NO,
      AVLinearPCMIsNonInterleaved : @NO,
    };
    AVAssetReaderAudioMixOutput* output = nil;
    @try {
      output = [AVAssetReaderAudioMixOutput assetReaderAudioMixOutputWithAudioTracks:@[ tracks.firstObject ]
                                                                      audioSettings:settings];
    } @catch (NSException* exception) {
      failReader(state, std::string("AVFoundation cannot open this file (") +
                            (exception.reason.UTF8String ?: "invalid audio reader settings") + ")");
      return;
    }
    output.alwaysCopiesSampleData = NO;
    if (![reader canAddOutput:output]) {
      failReader(state, "AVFoundation cannot open this file (the audio track cannot be decoded to PCM)");
      return;
    }
    [reader addOutput:output];
    if (![reader startReading]) {
      failReader(state, "AVFoundation cannot open this file (" + describe(reader.error) + ")");
      return;
    }
    { std::lock_guard<std::mutex> lock(state->mutex); state->reader = reader; }
    state->output = output;
  }
  decodeNextAudio(state);
}

class AvfClipAudioReader final : public IClipAudioReader {
 public:
  AvfClipAudioReader(const std::string& path, int64_t start100ns, std::function<void()> wake)
      : state_(std::make_shared<AudioReaderState>()),
        queue_(dispatch_queue_create("com.corevideo.media-decoder.audio",
                                     dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL,
                                                                             QOS_CLASS_USER_INITIATED, 0))) {
    state_->path = path;
    state_->start100ns = (std::max)(int64_t{0}, start100ns);
    state_->wake = std::move(wake);
    const auto state = state_;
    dispatch_async(queue_, ^{ openAudio(state); });
  }

  ~AvfClipAudioReader() override {
    state_->cancelled.store(true);
    AVURLAsset* asset = nil;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      asset = state_->asset;
    }
    [asset cancelLoading];
    const auto state = state_;
    dispatch_sync(queue_, ^{
      AVAssetReader* queued = nil;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        queued = state->reader;
      }
      [queued cancelReading];  // same rule as the video reader's destructor
    });
  }

  ClipReadStatus take(ClipAudioSample& out, std::string& error) override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->ready.empty()) {
      out = std::move(state_->ready.front());
      state_->ready.pop_front();
      const auto state = state_;
      dispatch_async(queue_, ^{ decodeNextAudio(state); });
      return ClipReadStatus::Ready;
    }
    if (state_->failed) { error = state_->error; return ClipReadStatus::Failed; }
    if (state_->endOfStream) return ClipReadStatus::EndOfStream;
    return ClipReadStatus::Pending;
  }

  int64_t duration100ns() const override { return state_->duration100ns.load(); }

 private:
  std::shared_ptr<AudioReaderState> state_;
  dispatch_queue_t queue_;
};

class AvfClipReaderFactory final : public IClipReaderFactory {
 public:
  explicit AvfClipReaderFactory(bool zeroCopy) : zeroCopy_(zeroCopy) {}
  std::unique_ptr<IClipVideoReader> openVideo(const std::string& path, std::function<void()> wake,
                                              std::string& error) override {
    error.clear();
    return std::make_unique<AvfClipVideoReader>(path, std::move(wake), zeroCopy_);
  }

  // Returns a reader at once; "no audio track" is learned on the reader's queue
  // and reported as an end of stream with no samples.
  std::unique_ptr<IClipAudioReader> openAudio(const std::string& path, int64_t start100ns,
                                              std::function<void()> wake, std::string& error) override {
    error.clear();
    return std::make_unique<AvfClipAudioReader>(path, start100ns, std::move(wake));
  }

  // ImageIO on Apple (StillMediaFrameCache.cpp), straight-alpha BGRA.
  bool decodeStill(const std::string& path, VideoFrame& frame, std::string& error) override {
    const auto decoder = createPlatformStillImageDecoder();
    StillImagePixels pixels;
    if (!decoder || !decoder->decode(path, pixels, error) || !pixels.bgra || pixels.width <= 0 || pixels.height <= 0) {
      if (error.empty()) error = "the image could not be decoded";
      return false;
    }
    frame.width = frame.pixelWidth = frame.naturalWidth = pixels.width;
    frame.height = frame.pixelHeight = frame.naturalHeight = pixels.height;
    frame.pixelStride = pixels.width * 4;
    frame.pixels = std::move(pixels.bgra);
    return true;
  }

 private:
  bool zeroCopy_ = false;
};

}  // namespace

std::shared_ptr<IClipReaderFactory> createAvFoundationClipReaderFactory() {
  return std::make_shared<AvfClipReaderFactory>(/*zeroCopy=*/false);
}

std::shared_ptr<IClipReaderFactory> createAvFoundationZeroCopyClipReaderFactory() {
  return std::make_shared<AvfClipReaderFactory>(/*zeroCopy=*/true);
}

uint64_t avfMediaDecoderCpuFrameCopiesForTest() { return g_cpuFrameCopies.load(); }

std::function<std::unique_ptr<IMediaDecoder>()> createAvFoundationMediaDecoderFactory() {
  return [] { return std::unique_ptr<IMediaDecoder>(new MediaClipDecoder(createAvFoundationClipReaderFactory())); };
}

std::function<std::unique_ptr<IMediaDecoder>()> createAvFoundationZeroCopyMediaDecoderFactory() {
  return [] {
    return std::unique_ptr<IMediaDecoder>(new MediaClipDecoder(createAvFoundationZeroCopyClipReaderFactory()));
  };
}

}  // namespace corevideo::modules

#else  // gate

#include <functional>
#include <memory>

#include "modules/Interfaces.h"

namespace corevideo::modules {

std::function<std::unique_ptr<IMediaDecoder>()> createAvFoundationMediaDecoderFactory() {
  return {};
}

std::function<std::unique_ptr<IMediaDecoder>()> createAvFoundationZeroCopyMediaDecoderFactory() {
  return {};
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_AVF_MEDIA_DECODER
