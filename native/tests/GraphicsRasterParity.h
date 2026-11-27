#pragma once

// Graphics raster parity standard (Metal graphics backend spec 7.3; G4 Task 6).
//
// Portable C++, no platform headers: the same code measures the Windows D2D
// raster (in the Windows session) and the Mac Core Text raster, so the two are
// judged by one definition of every metric.
//
// Images are premultiplied BGRA, row-major, tightly packed (what both rasters
// produce). Regions come from the RESOLVED layout (a JSON summary of the
// GraphicsDrawing), in raster pixels.
//
// Definitions the spec leaves open, decided in the plan:
// - Edge pixel: its 3x3 neighbourhood in the REFERENCE image has any channel
//   range above 2; otherwise it is interior.
// - Blur before SSIM: the 3x3 binomial kernel [1 2 1]/4, separable.
// - SSIM: 7x7 windows at stride 1, C1 = (0.01*255)^2, C2 = (0.03*255)^2, mean
//   over the region.
// - Line bands: the number of linePitch-high bands, from the region top, that
//   contain ink.
// - Text coverage: text can sit on an opaque plate, where alpha says nothing
//   about the glyphs. Each pixel's coverage is its premultiplied BGRA projected
//   onto the segment from the region's background (the most common colour on
//   the grown region's border, in that image) to the text colour
//   (premultiplied). On a transparent background this is the alpha.
//   "Alpha" in the text metrics below means coverage * 255.
//
// Tolerances (spec 7.3, verbatim):
// - Tier 1 geometry: interior channels within 2/255; edge band within 48/255;
//   summed alpha of the fixture within 0.5%; no pixel outside the reference
//   ink box grown by 1 px has alpha above 8/255. Pixels inside a text region
//   are judged by tier 2, not tier 1.
// - Tier 2 text: line bands equal; baseline within 1 px (never scaled: it is
//   computed, so a miss is a bug); and, times `toleranceScale` (2 for Segoe UI
//   substitution, the curated looks and the legacy fixtures), ink box edges
//   within 2 px, summed alpha within 8%, SSIM >= 0.90 (the deficit 0.10 is
//   scaled).
// - Layout: widths and crawl passWidth within the larger of 1.5 px and 1.5%; a
//   minFontSize fit within one bisection step (bounded by fontSize / 256).

#include <cstdint>
#include <string>
#include <vector>

#include "graphics/GraphicsTemplate.h"
#include "modules/Interfaces.h"
#include "rpc/Json.h"

namespace corevideo::graphics::parity {

struct ParityImage {
  int width = 0, height = 0;
  std::vector<uint8_t> bgraPremultiplied;
};

struct ParityRegion {
  std::string id;
  bool text = false;
  double x = 0, y = 0, width = 0, height = 0;  // raster px, the primitive's box
  double linePitch = 0;                        // text: fontSize * lineHeight, px
  int maxLines = 1;
  uint32_t colorArgb = 0xffffffffu;            // text colour (straight ARGB)
  double grow = 0;                             // px the measured area extends past the box
  // Text: the primitive's clip, raster px. Both rasters clip each primitive to
  // it (aliased), so no ink can fall outside it, and a text region's measured
  // area stops there. Grown past it, a table cell counted the next row's text
  // and took the neighbouring row's fill as its background.
  bool clipped = false;
  double clipX = 0, clipY = 0, clipWidth = 0, clipHeight = 0;
};

// {width, height, scale, primitives:[{id,type,box:[x,y,w,h],clip:[...],fontSize,lineHeight,maxLines,
//  color,crawlStart?}], crawl?:{passWidth}, slices?:[{x,y,width,height,first,count,source:[...]}]}.
// Boxes are in RASTER px. With `atlas`, primitives are placed in their atlas
// slice (planRuns + packAtlas, the code both rasters use), as the atlas
// texture holds them.
rpc::Json layoutSummary(const GraphicsDrawing& drawing, double scale, bool atlas);

std::vector<ParityRegion> regionsFromLayout(const rpc::Json& summary);

struct RegionMetrics {
  int inkX0 = 0, inkY0 = 0, inkX1 = 0, inkY1 = 0;  // inclusive-exclusive; empty when x1 <= x0
  double summedAlpha = 0;
  int lineBands = 0;
  int baselineRow = -1;  // lowest row with alpha > 128
};
// ink: alpha > 8. For a text region, alpha is coverage (see above).
RegionMetrics measureRegion(const ParityImage&, const ParityRegion&);

struct Verdict {
  bool pass = true;
  std::vector<std::string> failures;  // each names the region, metric, both values and the tolerance
};
Verdict judgeGeometry(const ParityImage& reference, const ParityImage& candidate, const std::vector<ParityRegion>&);
Verdict judgeText(const ParityImage& reference, const ParityImage& candidate, const std::vector<ParityRegion>&,
                  double toleranceScale);
Verdict judgeLayout(const rpc::Json& reference, const rpc::Json& candidate);
double alphaSsim(const ParityImage&, const ParityImage&, const ParityRegion&);

// ── PNG container ────────────────────────────────────────────────────────────
// Colour type 6, 8-bit. The samples ARE the premultiplied B,G,R,A bytes,
// reordered to R,G,B,A and stored verbatim: a viewer shows them slightly dark
// at partial alpha, which is expected. A reader must take the samples
// verbatim (CGImageSource -> CGDataProviderCopyData on Apple, a WIC
// CopyPixels in 32bppBGRA, the PNG codec's RGBA format, on Windows), never
// draw them through a context.
// Apple: GraphicsParityImageApple.mm. Windows: D3D11GraphicsReferenceTest.cpp.
bool writeParityPng(const std::string& path, const ParityImage& image, std::string& error);
bool readParityPng(const std::string& path, ParityImage& image, std::string& error);

// ── Fixture index (native/tests/fixtures/graphics-parity/index.json) ───────────
struct ParityFixture {
  std::string name, source;  // "manifest:<file>" | "curated:<templateId>" | "legacy:<lower-third|caption>"
  rpc::Json fields, brand;
  int width = 0, height = 0;  // 0: the template's bounds
  bool atlas = false;
  double textTolerance = 1;
  // An owner-recorded Windows/Mac difference (index "recordedDifference": its
  // reason). The fixture is still rendered, judged and printed on the sheet,
  // but its failures do not fail MacMatchesWindowsReference. Tolerances are
  // unchanged; every other fixture still blocks. Owner ruling 2026-11-27.
  std::string recordedDifference;
};
std::vector<ParityFixture> loadParityIndex(const std::string& indexPath, std::string& error);

// Builds the overlay a raster prepares for `fixture` (strict resources are the
// caller's), its raster size, and the resolved drawing (templates only).
struct ParityInput {
  corevideo::modules::CompositorOverlayContent overlay;
  int width = 0, height = 0;
  bool templated = false;
  GraphicsDrawing drawing;
  double scale = 1;
};
bool parityInput(const ParityFixture& fixture, const std::string& fixturesDir, ParityInput& out, std::string& error);

// The summary for a fixture: template layout, or the legacy tile layout.
rpc::Json fixtureLayout(const ParityFixture& fixture, const ParityInput& input);

}  // namespace corevideo::graphics::parity
