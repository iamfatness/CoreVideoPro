#pragma once

#include "graphics/GraphicsTimeline.h"
#include "rpc/Json.h"
#include <optional>

namespace corevideo::graphics {
struct GraphicsBox { double x = 0, y = 0, width = 0, height = 0; };
inline GraphicsBox intersect(GraphicsBox a, GraphicsBox b) {
  const auto right = (std::min)(a.x + a.width, b.x + b.width), bottom = (std::min)(a.y + a.height, b.y + b.height);
  a.x = (std::max)(a.x, b.x); a.y = (std::max)(a.y, b.y); a.width = (std::max)(0., right - a.x); a.height = (std::max)(0., bottom - a.y); return a;
}
enum class GraphicsMotionPhase { Settled, In, Out, Hidden };

struct GraphicsLayerMotion {
  std::string type = "cut", easing = "ease-out", direction = "left";
  double startMs = 0, durationMs = 0, distance = 24;
  bool distanceEdge = false;
  static GraphicsLayerMotion read(const rpc::Json& value);
  rpc::Json json() const;
  bool operator==(const GraphicsLayerMotion&) const = default;
};

struct GraphicsMotionUnit {
  std::string nodeId;
  int parent = -1;
  bool root = true;
  std::optional<GraphicsLayerMotion> in, out;
  GraphicsBox box, mask;
};

struct GraphicsMotionSummary {
  bool layered = false, anyFollows = true;
  size_t units = 0;
  double inMs = 0, outMs = 0;
};

struct GraphicsMotionState {
  GraphicsMotionPhase phase = GraphicsMotionPhase::Settled;
  double elapsedMs = 0;
  GraphicsTransition graphicIn, graphicOut;
  bool operator==(const GraphicsMotionState&) const = default;
};
struct GraphicsDrawing;
double motionEase(const std::string& easing, double t);
struct GraphicsAffine {
  double sx = 1, sy = 1, tx = 0, ty = 0;
  GraphicsBox apply(const GraphicsBox&) const;
  GraphicsAffine then(const GraphicsAffine& inner) const;
};
struct GraphicsMotionSample {
  GraphicsAffine transform;
  double opacity = 1;
  std::optional<GraphicsBox> clip;
};
GraphicsMotionSample sampleUnit(const GraphicsLayerMotion&, const GraphicsBox&,
    const std::optional<GraphicsBox>& mask, GraphicsMotionPhase, double elapsedMs);
GraphicsLayerMotion graphicMotion(const GraphicsTransition&, double width, double height);
GraphicsMotionSample sampleRun(const GraphicsDrawing&, int unit, const GraphicsMotionState&);
double motionTotal(const GraphicsMotionSummary&, const GraphicsTransition&, bool in);
struct GraphicsRun { int unit = -1; size_t first = 0, count = 0; GraphicsBox source; };
std::vector<GraphicsRun> planRuns(const GraphicsDrawing&);
struct GraphicsAtlasSlice { int x = 0, y = 0, width = 0, height = 0; };
bool packAtlas(const std::vector<GraphicsRun>&, double scale, std::vector<GraphicsAtlasSlice>&, int& width, int& height);
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
  double anchorMs_ = 0, anchorPos_ = 0, inMs_ = 0, outMs_ = 0;
  double position(double nowMs) const;
  void anchor(Mode mode, int direction, double nowMs, double position);
};
} // namespace corevideo::graphics
