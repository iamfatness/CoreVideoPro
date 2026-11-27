#pragma once

// The polygons D2D draws in place of curves, for rasters that must match it.
//
// D2D flattens every curve to line segments before it rasterizes: an
// ellipse or rounded-rectangle corner is drawn as chords, which lie inside
// the true convex curve. At D2D's default tolerance (0.25 device px) that
// lowers antialiased edge coverage by about 0.07 against exact coverage, so a
// raster that fills true curves (Core Graphics) reads heavier on every
// curved edge (G4 parity, 2026-11-27). This file reproduces D2D's polygons
// exactly: the same four-cubic geometry and traversal order as
// ID2D1EllipseGeometry / ID2D1RoundedRectangleGeometry, flattened with the
// hybrid forward differencing algorithm of WPF's CBezierFlattener (dotnet/wpf,
// src/Microsoft.DotNet.Wpf/src/WpfGfx/core/geometry/bezierflattener.cpp,
// MIT licence, (c) .NET Foundation and Contributors), from which D2D descends.
// D2DCurveFlatteningTest.cpp checks every point against ID2D1Geometry::Simplify.
//
// Plain C++ (no platform headers): the Mac raster builds its paths from it and
// the Windows test proves it. Coordinates are D2D's: y down.

#include <algorithm>
#include <cmath>
#include <vector>

namespace corevideo::compositor::d2dcurves {

struct Point {
  double x = 0, y = 0;
};

// D2D1_DEFAULT_FLATTENING_TOLERANCE, in device pixels. D2D divides it by the
// world transform's scale, so pass 0.25 / scale for user-space geometry.
constexpr double kDefaultTolerance = 0.25;
// The cubic D2D uses for a quarter ellipse: control points at kappa of the radius.
constexpr double kKappa = 0.55228474983079356;

// Appends the flattened points of one cubic after p0 (p0 itself is not
// appended; p3 always is). CBezierFlattener::Flatten without tangents.
inline void flattenCubic(Point p0, Point p1, Point p2, Point p3, double tolerance, std::vector<Point>& out) {
  constexpr double kTwiceMinStep = 1e-3;  // TWICE_MIN_BEZIER_STEP_SIZE
  const double limit = std::max(0.0, tolerance) * 6;  // |e2|, |e3| are 6x the error
  const double quarter = limit * 0.25;
  const auto norm = [](Point p) { return std::max(std::fabs(p.x), std::fabs(p.y)); };  // ApproxNorm
  // Hybrid forward differencing basis.
  Point e0 = p0, e1{p3.x - p0.x, p3.y - p0.y};
  Point e2{(p1.x - 2 * p2.x + p3.x) * 6, (p1.y - 2 * p2.y + p3.y) * 6};
  Point e3{(p0.x - 2 * p1.x + p2.x) * 6, (p0.y - 2 * p1.y + p2.y) * 6};
  long long steps = 1;
  double stepSize = 1;
  const auto halve = [&] {
    e2 = {(e2.x + e3.x) * 0.125, (e2.y + e3.y) * 0.125};
    e1 = {(e1.x - e2.x) * 0.5, (e1.y - e2.y) * 0.5};
    e3 = {e3.x * 0.25, e3.y * 0.25};
    steps *= 2;
    stepSize *= 0.5;
  };
  while ((norm(e2) > limit || norm(e3) > limit) && stepSize > kTwiceMinStep) halve();
  while (steps > 1) {
    // Step: e0 += e1, e1 += e2, e2 = 2e2 - e3, e3 = old e2.
    const Point old2 = e2;
    e0 = {e0.x + e1.x, e0.y + e1.y};
    e1 = {e1.x + old2.x, e1.y + old2.y};
    e2 = {2 * old2.x - e3.x, 2 * old2.y - e3.y};
    e3 = old2;
    out.push_back(e0);
    --steps;
    if (norm(e2) > limit && stepSize > kTwiceMinStep) {
      halve();
    } else {
      while ((steps & 1) == 0) {
        const Point doubled{e2.x * 2 - e3.x, e2.y * 2 - e3.y};
        if (!(norm(e3) <= quarter && norm(doubled) <= quarter)) break;
        e1 = {e1.x * 2 + e2.x, e1.y * 2 + e2.y};
        e3 = {e3.x * 4, e3.y * 4};
        e2 = {doubled.x * 4, doubled.y * 4};
        steps /= 2;
        stepSize *= 2;
      }
    }
  }
  out.push_back(p3);
}

// ID2D1EllipseGeometry: starts at the left point and runs clockwise on screen
// (left, top, right, bottom). Closed; the start point is not repeated.
inline std::vector<Point> ellipse(double cx, double cy, double rx, double ry, double tolerance) {
  std::vector<Point> out{{cx - rx, cy}};
  const double kx = kKappa * rx, ky = kKappa * ry;
  flattenCubic({cx - rx, cy}, {cx - rx, cy - ky}, {cx - kx, cy - ry}, {cx, cy - ry}, tolerance, out);
  flattenCubic({cx, cy - ry}, {cx + kx, cy - ry}, {cx + rx, cy - ky}, {cx + rx, cy}, tolerance, out);
  flattenCubic({cx + rx, cy}, {cx + rx, cy + ky}, {cx + kx, cy + ry}, {cx, cy + ry}, tolerance, out);
  flattenCubic({cx, cy + ry}, {cx - kx, cy + ry}, {cx - rx, cy + ky}, {cx - rx, cy}, tolerance, out);
  out.pop_back();
  return out;
}

// ID2D1RoundedRectangleGeometry with equal radii: starts at the top of the
// left edge and runs clockwise on screen. The radius is clamped to half the
// shorter side; 0 is the plain rectangle. Closed; the start is not repeated.
inline std::vector<Point> roundedRect(double x, double y, double w, double h, double radius, double tolerance) {
  const double r = std::clamp(radius, 0.0, std::min(w, h) * 0.5);
  if (r <= 0) return {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
  const double k = kKappa * r, x1 = x + w, y1 = y + h;
  std::vector<Point> out{{x, y + r}};
  // A side shorter than the two radii has no straight part: no extra point.
  const auto lineTo = [&out](Point p) {
    if (out.back().x != p.x || out.back().y != p.y) out.push_back(p);
  };
  flattenCubic({x, y + r}, {x, y + r - k}, {x + r - k, y}, {x + r, y}, tolerance, out);
  lineTo({x1 - r, y});
  flattenCubic({x1 - r, y}, {x1 - r + k, y}, {x1, y + r - k}, {x1, y + r}, tolerance, out);
  lineTo({x1, y1 - r});
  flattenCubic({x1, y1 - r}, {x1, y1 - r + k}, {x1 - r + k, y1}, {x1 - r, y1}, tolerance, out);
  lineTo({x + r, y1});
  flattenCubic({x + r, y1}, {x + r - k, y1}, {x, y1 - r + k}, {x, y1 - r}, tolerance, out);
  if (out.size() > 1 && out.back().x == out.front().x && out.back().y == out.front().y) out.pop_back();
  return out;
}

}  // namespace corevideo::compositor::d2dcurves
