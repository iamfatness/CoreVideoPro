#include "AvfMediaFixtures.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

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
