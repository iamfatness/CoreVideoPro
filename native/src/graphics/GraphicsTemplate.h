#pragma once

#include "rpc/Json.h"
#include "graphics/GraphicsMotion.h"
#include "graphics/GraphicsAssets.h"
#include "graphics/GraphicsPlacement.h"
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <set>

namespace corevideo::graphics {
class GraphicsTemplateDraft;
struct GraphicsTextOutline { uint32_t color; double width; };
struct GraphicsTextShadow { uint32_t color; double x, y, blur; };
// A rect/ellipse fill gradient (graphics styles P2). Linear: CSS angle in
// degrees (90 = left to right, 180 = top to bottom), the gradient line through
// the box centre long enough that the corners project to 0 and 1. Radial: an
// ellipse with radii half the box, centred at (centerX, centerY) in box units.
// 2..8 stops, offsets non-decreasing in 0..1, straight ARGB colours.
struct GraphicsGradientStop { double offset; uint32_t color; };
// Graphics styles P3: a polygon plate's vertices, template units.
struct GraphicsPoint { double x, y; };
// Graphics styles P4: a shape's drop shadow (template units). Glow is a
// shadow at dx = dy = 0. Drawn by compositor/ShapeShadowRaster.h.
struct GraphicsShapeShadow { uint32_t color; double dx, dy, blur, spread; };
struct GraphicsGradient {
  bool radial = false;
  double angle = 90, centerX = 0.5, centerY = 0.5;
  std::vector<GraphicsGradientStop> stops;
};
// P9 true backdrop blur ("glass", spec 6.1): the Program pixels behind a rect
// plate are Gaussian-blurred and tinted by a GPU pass in the COMPOSITOR before
// the graphic composites over them. The overlay raster draws the plate's fill
// as usual and never sees the backdrop. `blur` is the Gaussian radius in px at
// 1080p (sigma = blur / 2), bounded; `tint` is ARGB like every primitive colour.
inline constexpr double kGraphicsBackdropBlurMin = 1, kGraphicsBackdropBlurMax = 64;
inline constexpr size_t kGraphicsBackdropMaxPlates = 4;  // per template (frame budget)
struct GraphicsBackdrop { double blur = 0; uint32_t tint = 0; };
struct GraphicsCrawlGeometry {
  GraphicsBox window, clip;
  double speed = 120, passWidth = 0;
  bool rightward = false, loop = true;
};
struct GraphicsPrimitive {
  std::optional<double> crawlStart;
  std::string id, type, text, font, align, overflow;
  GraphicsBox box, clip;
  uint32_t color = 0xffffffff;  // a gradient's first stop
  double opacity = 1, cornerRadius = 0, fontSize = 28, lineHeight = 1.2;
  uint32_t strokeColor = 0;
  double strokeWidth = 0;
  bool lineRises = false;
  std::optional<GraphicsGradient> gradient;
  // A rect with shape "polygon" or a skewX: its outline (3..16 points,
  // absolute template units, inside the box). Empty: the box itself.
  std::vector<GraphicsPoint> polygon;
  // Rect/ellipse shadow, and the clip it composites inside: the parent clip
  // around the shifted, grown shape (the shape's own clip is its box).
  std::optional<GraphicsShapeShadow> shapeShadow;
  GraphicsBox shadowClip;
  int unit = -1;
  int fontWeight = 400, maxLines = 1;
  // Text style (graphics styles P1). letterSpacing is extra advance after
  // every character in template px (the document's em value times the
  // resolved font size); italic selects the family's italic face. A
  // textTransform is applied to `text` by the shared layout, before
  // measurement, so both rasters draw the same characters.
  double letterSpacing = 0;
  bool italic = false;
  std::optional<GraphicsTextOutline> outline;
  std::optional<GraphicsTextShadow> shadow;
  std::optional<GraphicsBackdrop> backdrop;  // rect only
  std::shared_ptr<const GraphicsAsset> image;
  std::string fit = "contain", imageAlign = "center", imageVerticalAlign = "center";
};
// One glass plate as the compositor needs it, in template units. Collected
// once on the raster owner with the prepared raster; nothing is allocated per
// frame. `unit` is the plate's motion unit (sampled from `motion` when the
// raster is a layered-motion atlas; a whole-graphic raster moves as one).
struct GraphicsBackdropPlate {
  GraphicsBox box, clip;
  double cornerRadius = 0, opacity = 1, blur = 0;
  uint32_t tint = 0;
  int unit = -1;
};
struct GraphicsDrawing;
struct GraphicsBackdropSet {
  double width = 0, height = 0;  // template bounds (the raster's coordinate space)
  std::vector<GraphicsBackdropPlate> plates;
  std::shared_ptr<const GraphicsDrawing> motion;  // set only for a runs (atlas) raster
};
struct GraphicsNodeGeometry { std::string id,parent,type; GraphicsBox box,clip; bool visible=true; };
struct GraphicsDrawing {
  double width = 0, height = 0;
  std::vector<GraphicsPrimitive> primitives;
  std::optional<GraphicsCrawlGeometry> crawl;
  std::vector<std::string> warnings;
  std::set<std::string> usedFields;
  std::vector<GraphicsNodeGeometry> nodes;
  std::vector<GraphicsMotionUnit> units;
};

// Immutable, bounded declarative definitions. Parsing/installation never occurs
// on a render caller; field/brand resolution and layout run on the raster owner.
class GraphicsTemplate {
 public:
  static std::shared_ptr<const GraphicsTemplate> load(const std::string& manifest, std::string& error, GraphicsAssets assets = {});
  static std::shared_ptr<const GraphicsTemplate> corporateLowerThird();
  static const std::vector<std::shared_ptr<const GraphicsTemplate>>& curated();
  const rpc::Json& document() const { return document_; }
  const std::array<uint8_t, 32>& hash() const { return hash_; }
  const GraphicsAssets& assets() const { return assets_; }
  std::string id() const { return document_.getString("templateId"); }
  int version() const { return int(document_.getNumber("templateVersion")); }
  double width() const { return document_.get("bounds")->getNumber("width"); }
  double height() const { return document_.get("bounds")->getNumber("height"); }
  const GraphicsMotionSummary& motionSummary() const { return motion_; }
  rpc::Json defaultFields() const;
  std::optional<GraphicsPlacement> defaultPlacement() const { std::optional<GraphicsPlacement> value; if (const auto* p = document_.get("defaultPlacement")) GraphicsPlacement::read(*p,value); return value; }
  bool validateFields(const rpc::Json& fields, bool requireValues, std::string& error) const;
  bool resolve(const rpc::Json& fields, const rpc::Json& brand, GraphicsDrawing& drawing, std::string& error, std::shared_ptr<const GraphicsAsset> logo = {}) const;
 private:
  GraphicsMotionSummary motion_;
  rpc::Json document_;
  std::array<uint8_t, 32> hash_{};
  GraphicsAssets assets_;
};

struct GraphicsTemplateInput {
  std::shared_ptr<const GraphicsTemplate> definition;
  rpc::Json fields, brand;
  std::shared_ptr<const GraphicsAsset> logo;
  std::optional<GraphicsPlacement> placement;
  std::shared_ptr<GraphicsTemplateDraft> draft;
  std::string draftKey;
  bool wholeGraphicMotion = false;
  mutable std::shared_ptr<const GraphicsTemplateInput> resolvedDraft;
};
// Upper bound, in bytes, that one raster preparation of `input` at
// width x height px is admitted with by the preparation cache. A plain
// template (or a legacy overlay, input == nullptr) prepares one raster:
// width x height x 8. Layered motion (not whole-graphic) and crawls prepare an
// atlas of one slice per run, so the bound is the atlas's: at most
// 2 * units + 1 runs of the raster width, plus one run per crawl item (64 at
// most) and one split of the surrounding run, each item at most 8192 template
// units wide. Capped at the 256 MiB cache budget. The worker charges the
// actual atlas against the process budget after layout.
// The drawing's glass plates, or null when it has none (the common case costs
// nothing). The caller attaches `motion` once the drawing is retained by a run layout.
std::shared_ptr<GraphicsBackdropSet> collectBackdrops(const GraphicsDrawing& drawing);
uint64_t overlayAdmissionBytes(const GraphicsTemplateInput* input, int width, int height);
// The start and end points {x0, y0, x1, y1} of a linear gradient across `box`
// (graphics styles P2), shared by both rasters. 90 and 180 degrees are the V1
// horizontal and vertical forms, from the box's top-left corner exactly as V1
// drew them; any other angle runs through the box centre, long enough that
// the far corners project to 0 and 1 (CSS linear-gradient geometry).
std::array<double, 4> linearGradientLine(const GraphicsGradient& gradient, const GraphicsBox& box);
// A text node's textTransform ("uppercase" | "lowercase"; anything else returns
// `text` unchanged). Simple one-to-one case mapping, identical on every
// platform; the layout applies it before measuring (graphics styles P1).
std::string transformGraphicsText(const std::string& text, const std::string& transform);
} // namespace corevideo::graphics
