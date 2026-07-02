#pragma once

#include "modules/Interfaces.h"

#include <cstdint>
#include <vector>

namespace corevideo::modules {

// Windows DirectWrite/D2D + WIC overlay tile raster (Item 9).
//
// Produces the same straight-alpha BGRA content tile as
// rasterizeOverlayTileBgra (OverlayTileRaster.h) — identical geometry via
// computeOverlayTileLayout — but with real DirectWrite text (brand font
// family, antialiased) and a real WIC image decode for imageUri instead of the
// bitmap font and checker placeholder.
//
// Returns true only when the full platform raster succeeded; on any failure
// (non-Windows build, factory/COM failure, undecodable image) it returns false
// and the caller falls back to the portable CPU tile. Compiled to a stub that
// returns false unless the D3D11 dev-adapter gates are on.
bool rasterizeOverlayTileDirectWrite(
    const CompositorOverlayContent& overlay,
    int widthPx,
    int heightPx,
    std::vector<uint8_t>& outBgra);

}  // namespace corevideo::modules
