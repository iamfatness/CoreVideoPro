// macOS OH Framer PNG decode (roadmap M9.2 Task 9) — the ImageIO analogue of
// the WIC decode in GpuWebcamFramer.cpp. Runs once, in GpuWebcamFramer::prepare
// on the preparation thread, never per frame.
#include "modules/WebcamFramerAppleDecode.h"
#if defined(__APPLE__) && COREVIDEO_WITH_MAC_VIRTUALCAM
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <cstring>
#include "WebcamFramerAsset.h"

namespace corevideo::modules {
std::string decodeWebcamFramerRgba(std::vector<uint8_t>& rgba, int& width, int& height) {
  CFDataRef encoded = CFDataCreateWithBytesNoCopy(nullptr, webcamFramerPng, sizeof(webcamFramerPng),
                                                  kCFAllocatorNull);
  if (!encoded) return "embedded PNG stream";
  CGImageSourceRef source = CGImageSourceCreateWithData(encoded, nullptr);
  CFRelease(encoded);
  if (!source) return "PNG decode";
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
  CFRelease(source);
  if (!image) return "PNG frame";
  std::string failed;
  const size_t w = CGImageGetWidth(image), h = CGImageGetHeight(image);
  const CGImageAlphaInfo alpha = CGImageGetAlphaInfo(image);
  const CGBitmapInfo order = CGImageGetBitmapInfo(image) & kCGBitmapByteOrderMask;
  // The asset is an 8-bit RGBA PNG; ImageIO hands its samples back as decoded:
  // straight alpha, R G B A in memory. Anything else would need a converting
  // draw, and CoreGraphics bitmap contexts only draw PREMULTIPLIED alpha, which
  // would change every semi-transparent pixel. Refuse instead of guessing.
  if (CGImageGetBitsPerComponent(image) != 8 || CGImageGetBitsPerPixel(image) != 32 ||
      alpha != kCGImageAlphaLast ||
      (order != kCGBitmapByteOrderDefault && order != kCGBitmapByteOrder32Big)) {
    failed = "straight-alpha decode";
  } else if (CFDataRef pixels = CGDataProviderCopyData(CGImageGetDataProvider(image))) {
    const size_t stride = CGImageGetBytesPerRow(image);
    if (static_cast<size_t>(CFDataGetLength(pixels)) < stride * (h - 1) + w * 4) {
      failed = "PNG pixels";
    } else {
      rgba.resize(w * h * 4);
      const UInt8* base = CFDataGetBytePtr(pixels);
      for (size_t y = 0; y < h; ++y) std::memcpy(rgba.data() + y * w * 4, base + y * stride, w * 4);
      width = static_cast<int>(w);
      height = static_cast<int>(h);
    }
    CFRelease(pixels);
  } else {
    failed = "PNG pixels";
  }
  CGImageRelease(image);
  return failed;
}
}
#endif
