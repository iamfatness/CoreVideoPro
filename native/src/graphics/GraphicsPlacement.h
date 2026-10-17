#pragma once
#include "rpc/Json.h"
#include <cmath>
#include <optional>
#include <string>

namespace corevideo::graphics {
// Independent of the legacy four-corner lower-third style. Coordinates are
// reference pixels at 1920x1080; anchors attach the same point on the content.
struct GraphicsPlacement {
  std::string anchor = "lower-left";
  double x = 96, y = -54, scale = 1;
  bool operator==(const GraphicsPlacement& b) const { return anchor == b.anchor && x == b.x && y == b.y && scale == b.scale; }
  rpc::Json json() const { return rpc::Json::Object{{"anchor",anchor},{"x",x},{"y",y},{"scale",scale}}; }
  static bool read(const rpc::Json& value, std::optional<GraphicsPlacement>& out) {
    if (value.isNull()) { out.reset(); return true; }
    if (!value.isObject() || value.asObject().size() != 4) return false;
    GraphicsPlacement p;
    for (const auto& entry : value.asObject()) {
      if (entry.first == "anchor") { if (!entry.second.isString()) return false; p.anchor = entry.second.asString(); }
      else {
        if (!entry.second.isNumber() || !std::isfinite(entry.second.asNumber())) return false;
        if (entry.first == "x") p.x=entry.second.asNumber();
        else if (entry.first == "y") p.y=entry.second.asNumber();
        else if (entry.first == "scale") p.scale=entry.second.asNumber();
        else return false;
      }
    }
    const bool anchor = p.anchor=="upper-left" || p.anchor=="upper-center" || p.anchor=="upper-right" ||
        p.anchor=="center-left" || p.anchor=="center" || p.anchor=="center-right" ||
        p.anchor=="lower-left" || p.anchor=="lower-center" || p.anchor=="lower-right";
    if (!anchor || std::abs(p.x)>3840 || std::abs(p.y)>2160 || p.scale<.1 || p.scale>4) return false;
    out=p; return true;
  }
};
}
