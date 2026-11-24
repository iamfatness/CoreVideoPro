#pragma once

// Library thumbnails on Metal (Metal graphics backend G5). Plain C++ so the
// shared GraphicsThumbnail.cpp can call it without Objective-C. Apple only;
// built with COREVIDEO_WITH_METAL.

#include <cstdint>
#include <string>
#include <vector>

#include "modules/Interfaces.h"

namespace corevideo::modules {

// Library worker only. Never touches a render thread, a cache or an MTLDevice.
// The CompositorOverlayRaster::thumbnailPng contract: width 1..320, height
// 1..180, strict resources, the shared process budget reserved and finished
// as for a raster, a crawl drawn with its items at their starts inside its
// clip (no atlas). Returns a straight-alpha PNG of at most 256 KiB, or empty
// with `warning` set to the reason.
std::vector<uint8_t> coreTextThumbnailPng(const CompositorOverlayContent& overlay, int width, int height,
                                          std::string& warning);

}  // namespace corevideo::modules
