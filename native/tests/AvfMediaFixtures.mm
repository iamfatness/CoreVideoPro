#include "AvfMediaFixtures.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace corevideo::testing {

namespace {

NSDictionary* bt709ColourProperties() {
  return @{
    AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
    AVVideoTransferFunctionKey : AVVideoTransferFunction_ITU_R_709_2,
    AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2,
  };
}

void tagBt709(CVPixelBufferRef buffer) {
  CVBufferSetAttachment(buffer, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2,
                        kCVAttachmentMode_ShouldPropagate);
  CVBufferSetAttachment(buffer, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2,
                        kCVAttachmentMode_ShouldPropagate);
  CVBufferSetAttachment(buffer, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                        kCVAttachmentMode_ShouldPropagate);
}

// Writes `frames` pixel buffers of `format`, filled by `fill(buffer, index)`.
std::string writeClip(const std::string& path, int width, int height, int fps, int frames, OSType format,
                      NSDictionary* outputSettings, AVFileType fileType,
                      const std::function<void(CVPixelBufferRef, int)>& fill) {
  @autoreleasepool {
    if (width <= 0 || height <= 0 || fps <= 0 || frames <= 0) return "invalid fixture dimensions";
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    NSError* error = nil;
    AVAssetWriter* writer = [[AVAssetWriter alloc] initWithURL:url fileType:fileType error:&error];
    if (!writer) return std::string("AVAssetWriter: ") + (error.localizedDescription.UTF8String ?: "unknown");
    AVAssetWriterInput* input = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo
                                                               outputSettings:outputSettings];
    input.expectsMediaDataInRealTime = NO;
    NSDictionary* bufferAttributes = @{
      (id)kCVPixelBufferPixelFormatTypeKey : @(format),
      (id)kCVPixelBufferWidthKey : @(width),
      (id)kCVPixelBufferHeightKey : @(height),
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    AVAssetWriterInputPixelBufferAdaptor* adaptor =
        [[AVAssetWriterInputPixelBufferAdaptor alloc] initWithAssetWriterInput:input
                                                   sourcePixelBufferAttributes:bufferAttributes];
    if (![writer canAddInput:input]) return "AVAssetWriter refused the video input";
    [writer addInput:input];
    if (![writer startWriting]) {
      return std::string("startWriting: ") + (writer.error.localizedDescription.UTF8String ?: "unknown");
    }
    [writer startSessionAtSourceTime:kCMTimeZero];
    for (int index = 0; index < frames; ++index) {
      CVPixelBufferRef buffer = nullptr;
      if (CVPixelBufferCreate(kCFAllocatorDefault, static_cast<size_t>(width), static_cast<size_t>(height), format,
                              (__bridge CFDictionaryRef)bufferAttributes, &buffer) != kCVReturnSuccess || !buffer) {
        return "CVPixelBufferCreate failed";
      }
      CVPixelBufferLockBaseAddress(buffer, 0);
      fill(buffer, index);
      CVPixelBufferUnlockBaseAddress(buffer, 0);
      tagBt709(buffer);
      for (int waited = 0; !input.readyForMoreMediaData && waited < 5000; ++waited) {
        [NSThread sleepForTimeInterval:0.001];
      }
      const BOOL appended = [adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(index, fps)];
      CVPixelBufferRelease(buffer);
      if (!appended) {
        return std::string("appendPixelBuffer: ") + (writer.error.localizedDescription.UTF8String ?: "unknown");
      }
    }
    [input markAsFinished];
    [writer endSessionAtSourceTime:CMTimeMake(frames, fps)];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
    if (writer.status != AVAssetWriterStatusCompleted) {
      return std::string("finishWriting: ") + (writer.error.localizedDescription.UTF8String ?: "not completed");
    }
    return "";
  }
}

}  // namespace

std::string writeFlatColourClip(const std::string& path, int width, int height, int fps,
                                const std::vector<FlatSegment>& segments, bool prores4444) {
  int frames = 0;
  for (const auto& segment : segments) frames += segment.frames;
  NSDictionary* settings = prores4444
      ? @{ AVVideoCodecKey : AVVideoCodecTypeAppleProRes4444, AVVideoWidthKey : @(width), AVVideoHeightKey : @(height) }
      : @{ AVVideoCodecKey : AVVideoCodecTypeH264, AVVideoWidthKey : @(width), AVVideoHeightKey : @(height),
           AVVideoColorPropertiesKey : bt709ColourProperties() };
  return writeClip(path, width, height, fps, frames, kCVPixelFormatType_32BGRA, settings,
                   prores4444 ? AVFileTypeQuickTimeMovie : AVFileTypeMPEG4,
                   [&](CVPixelBufferRef buffer, int index) {
    FlatSegment colour = segments.back();
    int first = 0;
    for (const auto& segment : segments) {
      if (index < first + segment.frames) { colour = segment; break; }
      first += segment.frames;
    }
    auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
    for (int y = 0; y < height; ++y) {
      uint8_t* row = base + static_cast<size_t>(y) * stride;
      for (int x = 0; x < width; ++x) {
        row[x * 4 + 0] = colour.b; row[x * 4 + 1] = colour.g; row[x * 4 + 2] = colour.r; row[x * 4 + 3] = colour.a;
      }
    }
  });
}

std::string writeBt709BarsClip(const std::string& path, int frames, int fps) {
  constexpr int kSize = 64;
  struct Ycc { uint8_t y, cb, cr; };
  // [row][column], row 0 = top.
  static constexpr Ycc kQuadrants[2][2] = {{{16, 128, 128}, {235, 128, 128}}, {{173, 42, 26}, {63, 102, 240}}};
  NSDictionary* settings = @{ AVVideoCodecKey : AVVideoCodecTypeH264, AVVideoWidthKey : @(kSize),
                              AVVideoHeightKey : @(kSize), AVVideoColorPropertiesKey : bt709ColourProperties() };
  return writeClip(path, kSize, kSize, fps, frames, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, settings,
                   AVFileTypeMPEG4, [&](CVPixelBufferRef buffer, int) {
    auto* luma = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
    const size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
    for (int y = 0; y < kSize; ++y) {
      for (int x = 0; x < kSize; ++x) luma[static_cast<size_t>(y) * lumaStride + x] = kQuadrants[y / 32][x / 32].y;
    }
    auto* chroma = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
    const size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
    for (int y = 0; y < kSize / 2; ++y) {
      for (int x = 0; x < kSize / 2; ++x) {
        const auto& quadrant = kQuadrants[y / 16][x / 16];
        chroma[static_cast<size_t>(y) * chromaStride + x * 2 + 0] = quadrant.cb;
        chroma[static_cast<size_t>(y) * chromaStride + x * 2 + 1] = quadrant.cr;
      }
    }
  });
}

std::string writeToneClip(const std::string& path, int seconds, double toneHz) {
  @autoreleasepool {
    constexpr int kSize = 64, kFps = 30, kRate = 48000;
    if (seconds <= 0) return "invalid fixture duration";
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    NSError* error = nil;
    AVAssetWriter* writer = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&error];
    if (!writer) return std::string("AVAssetWriter: ") + (error.localizedDescription.UTF8String ?: "unknown");
    AVAssetWriterInput* video = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeVideo outputSettings:@{
      AVVideoCodecKey : AVVideoCodecTypeH264, AVVideoWidthKey : @(kSize), AVVideoHeightKey : @(kSize),
      AVVideoColorPropertiesKey : bt709ColourProperties() }];
    NSDictionary* bufferAttributes = @{ (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
      (id)kCVPixelBufferWidthKey : @(kSize), (id)kCVPixelBufferHeightKey : @(kSize),
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{} };
    AVAssetWriterInputPixelBufferAdaptor* adaptor =
        [[AVAssetWriterInputPixelBufferAdaptor alloc] initWithAssetWriterInput:video
                                                   sourcePixelBufferAttributes:bufferAttributes];
    AudioChannelLayout layout = {};
    layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
    AVAssetWriterInput* audio = [[AVAssetWriterInput alloc] initWithMediaType:AVMediaTypeAudio outputSettings:@{
      AVFormatIDKey : @(kAudioFormatMPEG4AAC), AVSampleRateKey : @(kRate), AVNumberOfChannelsKey : @2,
      AVEncoderBitRateKey : @192000,
      AVChannelLayoutKey : [NSData dataWithBytes:&layout length:sizeof(layout)] }];
    video.expectsMediaDataInRealTime = NO;
    audio.expectsMediaDataInRealTime = NO;
    if (![writer canAddInput:video] || ![writer canAddInput:audio]) return "AVAssetWriter refused an input";
    [writer addInput:video];
    [writer addInput:audio];
    if (![writer startWriting]) {
      return std::string("startWriting: ") + (writer.error.localizedDescription.UTF8String ?: "unknown");
    }
    [writer startSessionAtSourceTime:kCMTimeZero];
    AudioStreamBasicDescription format = {};
    format.mSampleRate = kRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = 2;
    format.mBitsPerChannel = 32;
    format.mBytesPerFrame = 8;
    format.mFramesPerPacket = 1;
    format.mBytesPerPacket = 8;
    CMAudioFormatDescriptionRef description = nullptr;
    if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &format, sizeof(layout), &layout, 0, nullptr, nullptr,
                                       &description) != noErr) {
      return "CMAudioFormatDescriptionCreate failed";
    }
    std::string failure;
    // Feed whichever input is ready. AVAssetWriter interleaves the two tracks itself and stops
    // accepting one while the other is behind, so a fixed "video then audio" order can wait forever.
    constexpr int kAudioChunk = 1024;
    const int videoFrames = seconds * kFps;
    const int64_t audioFrames = static_cast<int64_t>(seconds) * kRate;
    int videoIndex = 0;
    int64_t audioIndex = 0;
    const auto giveUpAt = [NSDate dateWithTimeIntervalSinceNow:30.0];
    while ((videoIndex < videoFrames || audioIndex < audioFrames) && failure.empty()) {
      bool progressed = false;
      if (videoIndex < videoFrames && video.readyForMoreMediaData) {
        CVPixelBufferRef buffer = nullptr;
        if (CVPixelBufferCreate(kCFAllocatorDefault, kSize, kSize, kCVPixelFormatType_32BGRA,
                                (__bridge CFDictionaryRef)bufferAttributes, &buffer) != kCVReturnSuccess || !buffer) {
          failure = "CVPixelBufferCreate failed";
          break;
        }
        CVPixelBufferLockBaseAddress(buffer, 0);
        std::memset(CVPixelBufferGetBaseAddress(buffer), 0x80, CVPixelBufferGetBytesPerRow(buffer) * kSize);
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        tagBt709(buffer);
        if (![adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(videoIndex, kFps)]) {
          failure = "the video frame could not be appended";
        }
        CVPixelBufferRelease(buffer);
        ++videoIndex;
        progressed = true;
        if (videoIndex == videoFrames) [video markAsFinished];
      }
      if (failure.empty() && audioIndex < audioFrames && audio.readyForMoreMediaData) {
        const int count = static_cast<int>((std::min)(static_cast<int64_t>(kAudioChunk), audioFrames - audioIndex));
        std::vector<float> pcm(static_cast<size_t>(count) * 2u);
        for (int i = 0; i < count; ++i) {
          const float value = 0.5f * static_cast<float>(
              std::sin(2.0 * M_PI * toneHz * static_cast<double>(audioIndex + i) / kRate));
          pcm[static_cast<size_t>(i) * 2u] = value;
          pcm[static_cast<size_t>(i) * 2u + 1u] = value;
        }
        CMBlockBufferRef block = nullptr;
        const size_t bytes = pcm.size() * sizeof(float);
        if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, bytes, kCFAllocatorDefault, nullptr, 0,
                                               bytes, kCMBlockBufferAssureMemoryNowFlag, &block) != kCMBlockBufferNoErr ||
            CMBlockBufferReplaceDataBytes(pcm.data(), block, 0, bytes) != kCMBlockBufferNoErr) {
          if (block) CFRelease(block);
          failure = "CMBlockBuffer failed";
          break;
        }
        CMSampleBufferRef sample = nullptr;
        const OSStatus created = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
            kCFAllocatorDefault, block, description, count, CMTimeMake(audioIndex, kRate), nullptr, &sample);
        CFRelease(block);
        if (created != noErr || !sample) { failure = "the audio sample buffer could not be created"; break; }
        if (![audio appendSampleBuffer:sample]) failure = "the audio chunk could not be appended";
        CFRelease(sample);
        audioIndex += count;
        progressed = true;
        if (audioIndex == audioFrames) [audio markAsFinished];
      }
      if (!progressed) {
        if ([giveUpAt timeIntervalSinceNow] < 0) { failure = "AVAssetWriter never became ready for more media"; break; }
        [NSThread sleepForTimeInterval:0.001];
      }
    }
    CFRelease(description);
    if (!failure.empty()) {
      [writer cancelWriting];
      return failure + (writer.error ? std::string(": ") + (writer.error.localizedDescription.UTF8String ?: "") : "");
    }
    [writer endSessionAtSourceTime:CMTimeMake(seconds, 1)];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
    dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
    if (writer.status != AVAssetWriterStatusCompleted) {
      return std::string("finishWriting: ") + (writer.error.localizedDescription.UTF8String ?: "not completed");
    }
    return "";
  }
}

std::string writeFlatColourPng(const std::string& path, int width, int height,
                               uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
  @autoreleasepool {
    // CoreGraphics bitmaps are premultiplied; the fixture is described straight.
    const auto premultiply = [a](uint8_t channel) { return static_cast<uint8_t>((channel * a + 127) / 255); };
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    for (size_t offset = 0; offset < pixels.size(); offset += 4) {
      pixels[offset] = premultiply(b); pixels[offset + 1] = premultiply(g);
      pixels[offset + 2] = premultiply(r); pixels[offset + 3] = a;
    }
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(pixels.data(), static_cast<size_t>(width), static_cast<size_t>(height), 8,
        static_cast<size_t>(width) * 4u, space,
        static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Little));
    CGColorSpaceRelease(space);
    if (!context) return "CGBitmapContextCreate failed";
    CGImageRef image = CGBitmapContextCreateImage(context);
    CGContextRelease(context);
    if (!image) return "CGBitmapContextCreateImage failed";
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithURL((__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    bool ok = false;
    if (destination) {
      CGImageDestinationAddImage(destination, image, nullptr);
      ok = CGImageDestinationFinalize(destination);
      CFRelease(destination);
    }
    CGImageRelease(image);
    return ok ? "" : "the PNG could not be written";
  }
}

}  // namespace corevideo::testing
