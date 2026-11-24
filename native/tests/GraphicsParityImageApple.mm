// Verbatim-sample PNG container for the graphics parity standard (G4 Task 6):
// see GraphicsRasterParity.h. Apple only (ImageIO).

#if defined(__APPLE__)

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include "GraphicsRasterParity.h"

namespace corevideo::graphics::parity {

bool writeParityPng(const std::string& path, const ParityImage& image, std::string& error) {
  if (image.width <= 0 || image.height <= 0 ||
      image.bgraPremultiplied.size() != size_t(image.width) * size_t(image.height) * 4) {
    error = "invalid image";
    return false;
  }
  std::vector<uint8_t> rgba(image.bgraPremultiplied.size());
  for (size_t i = 0; i < rgba.size(); i += 4) {
    rgba[i] = image.bgraPremultiplied[i + 2];
    rgba[i + 1] = image.bgraPremultiplied[i + 1];
    rgba[i + 2] = image.bgraPremultiplied[i];
    rgba[i + 3] = image.bgraPremultiplied[i + 3];
  }
  // Declared STRAIGHT alpha (kCGImageAlphaLast), so ImageIO stores the samples
  // as given rather than un-premultiplying them.
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, rgba.data(), CFIndex(rgba.size()));
  CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
  CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
  CGImageRef cg = CGImageCreate(size_t(image.width), size_t(image.height), 8, 32, size_t(image.width) * 4, space,
                                CGBitmapInfo(kCGImageAlphaLast) | kCGBitmapByteOrderDefault, provider, nullptr, false,
                                kCGRenderingIntentDefault);
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(path.c_str()), CFIndex(path.size()), false);
  CGImageDestinationRef destination = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr) : nullptr;
  bool ok = false;
  if (destination && cg) {
    CGImageDestinationAddImage(destination, cg, nullptr);
    ok = CGImageDestinationFinalize(destination);
  }
  if (destination) CFRelease(destination);
  if (url) CFRelease(url);
  if (cg) CGImageRelease(cg);
  CGColorSpaceRelease(space);
  CGDataProviderRelease(provider);
  CFRelease(data);
  if (!ok) error = "PNG write failed: " + path;
  return ok;
}

bool readParityPng(const std::string& path, ParityImage& image, std::string& error) {
  image = ParityImage{};
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(path.c_str()), CFIndex(path.size()), false);
  CGImageSourceRef source = url ? CGImageSourceCreateWithURL(url, nullptr) : nullptr;
  CGImageRef cg = source ? CGImageSourceCreateImageAtIndex(source, 0, nullptr) : nullptr;
  bool ok = false;
  if (!cg) error = "PNG read failed: " + path;
  else if (CGImageGetBitsPerComponent(cg) != 8 || CGImageGetBitsPerPixel(cg) != 32 ||
           CGImageGetAlphaInfo(cg) != kCGImageAlphaLast) {
    error = "not an 8-bit straight-RGBA PNG (verbatim samples required): " + path;
  } else {
    CFDataRef pixels = CGDataProviderCopyData(CGImageGetDataProvider(cg));
    if (!pixels) error = "PNG samples unavailable: " + path;
    else {
      image.width = int(CGImageGetWidth(cg));
      image.height = int(CGImageGetHeight(cg));
      const size_t row = CGImageGetBytesPerRow(cg);
      const auto* bytes = CFDataGetBytePtr(pixels);
      image.bgraPremultiplied.resize(size_t(image.width) * size_t(image.height) * 4);
      for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
          const auto* s = bytes + size_t(y) * row + size_t(x) * 4;
          auto* d = image.bgraPremultiplied.data() + (size_t(y) * size_t(image.width) + size_t(x)) * 4;
          d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
        }
      CFRelease(pixels);
      ok = true;
    }
  }
  if (cg) CGImageRelease(cg);
  if (source) CFRelease(source);
  if (url) CFRelease(url);
  return ok;
}

}  // namespace corevideo::graphics::parity

#endif  // __APPLE__
