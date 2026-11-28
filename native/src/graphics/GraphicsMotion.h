#pragma once

#include "graphics/GraphicsTimeline.h"
#include "rpc/Json.h"
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace corevideo::graphics {
struct GraphicsBox { double x = 0, y = 0, width = 0, height = 0; };
inline GraphicsBox intersect(GraphicsBox a, GraphicsBox b) {
  const auto right = (std::min)(a.x + a.width, b.x + b.width), bottom = (std::min)(a.y + a.height, b.y + b.height);
  a.x = (std::max)(a.x, b.x); a.y = (std::max)(a.y, b.y); a.width = (std::max)(0., right - a.x); a.height = (std::max)(0., bottom - a.y); return a;
}
enum class GraphicsMotionPhase { Settled, In, Out, Hidden };

// Every layer motion type a template may name (the GraphicsTemplate validator's
// one list). The contract-parity test also accepts these where a shell's motion
// spec carries a "type" field, so it never reads one as a request.
inline constexpr std::array<std::string_view, 8> kGraphicsMotionTypes{
    "cut", "fade", "rise", "slide", "wipe", "grow", "pop", "reveal"};
inline bool isGraphicsMotionType(std::string_view type) {
  return std::find(kGraphicsMotionTypes.begin(), kGraphicsMotionTypes.end(), type) != kGraphicsMotionTypes.end();
}
// Layer motion easings (spec 6.3: expo-out reveals, quint-out text moves).
inline constexpr std::array<std::string_view, 6> kGraphicsMotionEasings{
    "linear", "ease-in", "ease-out", "ease-in-out", "expo-out", "quint-out"};
inline bool isGraphicsMotionEasing(std::string_view easing) {
  return std::find(kGraphicsMotionEasings.begin(), kGraphicsMotionEasings.end(), easing) != kGraphicsMotionEasings.end();
}

struct GraphicsLayerMotion {
  std::string type = "cut", easing = "ease-out", direction = "left";
  double startMs = 0, durationMs = 0, distance = 24;
  // reveal only: the soft leading edge in template px (0 = a hard edge, as wipe).
  double feather = 0;
  bool distanceEdge = false;
  static GraphicsLayerMotion read(const rpc::Json& value);
  rpc::Json json() const;
  bool operator==(const GraphicsLayerMotion&) const = default;
};

// P7 looping accent: a periodic per-frame transform or opacity of the layer's
// pre-rastered slice, clocked from the instance's entrance. Never a re-raster.
// - sweep: crosses its area once, in the first durationMs of every period
//   (direction names the side it enters from, as wipe).
// - pulse: opacity 1 -> 0.4 -> 1 over each period (cosine).
// - drift: a sine offset of up to distance px toward direction.
struct GraphicsLayerLoop {
  std::string type = "pulse", direction = "left";
  double periodMs = 2000, durationMs = 1000, distance = 6;
  static GraphicsLayerLoop read(const rpc::Json& value);
  rpc::Json json() const;
  bool operator==(const GraphicsLayerLoop&) const = default;
};
inline constexpr double kGraphicsLoopMinPeriodMs = 250, kGraphicsLoopMaxPeriodMs = 60000;
// P6 stagger: a text layer staggered by word splits into at most this many
// pieces (the last piece carries the remaining words); by line, maxLines.
inline constexpr int kGraphicsStaggerMaxWords = 12;

struct GraphicsMotionUnit {
  std::string nodeId;
  int parent = -1;
  bool root = true;
  std::optional<GraphicsLayerMotion> in, out;
  GraphicsBox box, mask;
  std::optional<GraphicsLayerLoop> loop;
  // Stagger bound from the template: the most pieces this layer can split
  // into at resolve, and the delay between consecutive pieces.
  int staggerPieces = 1;
  double staggerMs = 0;
};

struct GraphicsMotionSummary {
  bool layered = false, anyFollows = true;
  size_t units = 0;
  double inMs = 0, outMs = 0;
  bool inFollows = true, outFollows = true;
  // The template declares a crawl (visible or not): it prepares into an atlas
  // of one slice per item, like layered motion.
  bool crawl = false;
  // Units a resolved drawing can add by staggering text into pieces (the
  // admission bound counts them), and whether any layer loops.
  size_t staggerUnits = 0;
  bool loops = false;
};

struct GraphicsMotionState {
  GraphicsMotionPhase phase = GraphicsMotionPhase::Settled;
  double elapsedMs = 0;
  GraphicsTransition graphicIn, graphicOut;
  // Loop clock: ms since the instance began entering, in every phase.
  double loopMs = 0;
  bool operator==(const GraphicsMotionState&) const = default;
};
struct GraphicsDrawing;
double motionEase(const std::string& easing, double t);
struct GraphicsAffine {
  double sx = 1, sy = 1, tx = 0, ty = 0;
  GraphicsBox apply(const GraphicsBox&) const;
  GraphicsAffine then(const GraphicsAffine& inner) const;
};
// A reveal's soft leading edge in drawing units. Coverage ramps from 1 at
// (lead - width) to 0 at lead along the axis; sign +1 keeps what lies below
// lead (x or y), -1 what lies above it.
struct GraphicsRevealEdge {
  bool vertical = false;
  double sign = 1, lead = 0, width = 0;
  bool operator==(const GraphicsRevealEdge&) const = default;
};
struct GraphicsMotionSample {
  GraphicsAffine transform;
  double opacity = 1;
  std::optional<GraphicsBox> clip;
  // The innermost reveal's feather: a child's edge replaces its parent's.
  std::optional<GraphicsRevealEdge> edge;
};
GraphicsMotionSample sampleUnit(const GraphicsLayerMotion&, const GraphicsBox&,
    const std::optional<GraphicsBox>& mask, GraphicsMotionPhase, double elapsedMs);
GraphicsMotionSample sampleLoop(const GraphicsLayerLoop&, const GraphicsBox&,
    const std::optional<GraphicsBox>& mask, double loopMs);
GraphicsLayerMotion graphicMotion(const GraphicsTransition&, double width, double height);
GraphicsMotionSample sampleRun(const GraphicsDrawing&, int unit, const GraphicsMotionState&);
double motionTotal(const GraphicsMotionSummary&, const GraphicsTransition&, bool in);
struct GraphicsRun { int unit = -1; size_t first = 0, count = 0; GraphicsBox source; };
std::vector<GraphicsRun> planRuns(const GraphicsDrawing&);
struct GraphicsAtlasSlice { int x = 0, y = 0, width = 0, height = 0; };
struct GraphicsRunLayout {
  double width = 0, height = 0;
  std::vector<GraphicsRun> runs;
  std::vector<GraphicsAtlasSlice> slices;
  int atlasWidth = 0, atlasHeight = 0;
  std::shared_ptr<const GraphicsDrawing> drawing;
};
bool packAtlas(const std::vector<GraphicsRun>&, double scale, std::vector<GraphicsAtlasSlice>&, int& width, int& height);
// Where run `index` of an atlas draws this frame, in drawing units. Both
// compositors (D3D11, Metal) place runs through this one function, so motion,
// reveal, loop and crawl geometry cannot drift between platforms. Pure and
// allocation-free (render thread). False: the run draws nothing this frame.
struct GraphicsRunPlacement {
  GraphicsBox destination;
  std::optional<GraphicsBox> mask;
  double opacity = 1;
  std::optional<GraphicsRevealEdge> edge;
};
bool placeRun(const GraphicsRunLayout&, size_t index, const GraphicsMotionState&,
    std::optional<double> crawlDistance, GraphicsRunPlacement&);
// The overlay shader's reveal-edge constant for a placement drawn into the
// layer rect (normalized target units) of a targetWidth x targetHeight frame:
// {nx, ny, n.lead, 1/width} in target pixels, or all zero (no feather).
void revealEdgeConstants(const std::optional<GraphicsRevealEdge>&, double layoutWidth, double layoutHeight,
    double rectX, double rectY, double rectWidth, double rectHeight, int targetWidth, int targetHeight, float out[4]);
GraphicsMotionState previewMotion(const std::string& mode, double elapsedMs, double inMs, double outMs,
    const GraphicsTransition& graphicIn, const GraphicsTransition& graphicOut);

class GraphicsLayerClock {
 public:
  void configure(double inMs, double outMs);
  void show(double nowMs);
  void hide(double nowMs);
  void clear();
  std::string visibility(double nowMs) const;
  GraphicsMotionState state(double nowMs) const;
  bool settle(double nowMs);
 private:
  enum class Mode { Hidden, In, OnAir, Out };
  Mode mode_ = Mode::Hidden;
  int direction_ = 1;
  double anchorMs_ = 0, anchorPos_ = 0, inMs_ = 0, outMs_ = 0, loopAnchorMs_ = 0;
  double position(double nowMs) const;
  void anchor(Mode mode, int direction, double nowMs, double position);
};
} // namespace corevideo::graphics
