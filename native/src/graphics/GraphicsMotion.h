#pragma once

#include "graphics/GraphicsTimeline.h"
#include "rpc/Json.h"
#include <optional>

namespace corevideo::graphics {
struct GraphicsBox { double x = 0, y = 0, width = 0, height = 0; };
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
} // namespace corevideo::graphics
