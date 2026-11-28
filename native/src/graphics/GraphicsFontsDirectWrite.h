#pragma once

// DirectWrite font resolution for native graphics on Windows (graphics styles
// P8, slice 7). ONE place decides which collection and family a requested
// font becomes, so GraphicsTemplate's measurement and CompositorOverlayRaster's
// drawing cannot disagree. The Core Text twin is GraphicsTextCoreText.
//
// Order:
//   1. the family in the system font collection (unchanged from before packs,
//      so every existing graphic resolves, measures and draws exactly as it did);
//   2. the family in the private pack collection: every present font file that
//      a pack under graphicsPacksDirectory() declares, loaded through an
//      IDWriteFactory5 font set (IDWriteFontSetBuilder1::AddFontFile). Nothing
//      is installed system-wide;
//   3. a pack-declared family whose file is missing or did not load: its
//      declared fallback from the system collection plus a warning, never a
//      failure on air. In strict pack-font mode (graphicsPackFontsStrict) it is
//      unavailable with graphicsPackFontMissingError;
//   4. otherwise unavailable, as before (strict graphics fail, legacy warns).

#if defined(_WIN32) && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwrite.h>

#include <cstddef>
#include <string>

#include "compositor/ComPtrLite.h"

namespace corevideo::graphics {

struct DirectWriteGraphicsFont {
  modules::ComPtrLite<IDWriteFontCollection> collection;  // null: unavailable
  std::wstring family;   // the family to pass to CreateTextFormat with `collection`
  std::string warning;   // a non-fatal substitution warning for the operator
  std::string error;     // why it is unavailable, when a pack decided it
  bool available() const { return bool(collection); }
};

// Thread-safe; called on raster workers and during template measurement.
DirectWriteGraphicsFont resolveDirectWriteGraphicsFont(IDWriteFactory* factory, const std::string& family);

// Faces in the private pack collection (rebuilt when the pack registry's
// generation changes). Zero when no pack ships a font or IDWriteFactory5 is
// unavailable.
size_t directWritePackFontFaces();

}  // namespace corevideo::graphics

#endif  // _WIN32 && D3D11 dev adapters
