#pragma once

// Shape drop shadow and glow (graphics styles P4), drawn the same way by both
// rasters. The Windows overlay raster draws on a software WIC target, which
// has no D2D Shadow effect, and Core Graphics' CGContextSetShadowWithColor is
// a true Gaussian that D2D cannot reproduce. So the shadow is computed HERE,
// in plain C++, and each raster only composites the finished premultiplied
// pixels 1:1 in device space:
//
//  1. Shape: the primitive's outline (rect, rounded rect, ellipse or polygon)
//     offset by (dx, dy) and grown by `spread`, as a polygon in template
//     units. Curves are D2D's chords (D2DCurveFlattening.h at 0.25 device px),
//     the polygon the D2D raster fills for the shape itself.
//  2. Coverage: exact signed-area coverage of that polygon in device pixels
//     (non-zero winding, clamped to 1), into a buffer covering the shadow
//     clip grown by the blur reach.
//  3. Blur: three box-blur passes per axis, each 2k+1 wide, integer
//     arithmetic (deterministic on every platform). k comes from `blur`:
//     a CSS-style blur radius b is a Gaussian with sigma b/2; three boxes of
//     width w have variance (w^2-1)/4, so w = sqrt(b^2+1) device px.
//  4. Colour: coverage x colour alpha x node opacity, premultiplied BGRA.
//
// Glow is a shadow at dx = dy = 0. Portable C++ (no platform headers).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "compositor/D2DCurveFlattening.h"

namespace corevideo::compositor::shapeshadow {

using d2dcurves::Point;

// Box half-width k for a blur radius in device pixels (step 3).
inline int boxRadius(double blurDevicePx) {
  if (!(blurDevicePx > 0)) return 0;
  const double width = std::sqrt(blurDevicePx * blurDevicePx + 1);
  return int(std::lround((width - 1) / 2));
}

// The outline a shadow is cast from, in template units (step 1). `points` is
// empty for a rect or ellipse; for a polygon it is the primitive's vertices.
struct Outline {
  enum class Kind { Rect, Ellipse, Polygon } kind = Kind::Rect;
  double x = 0, y = 0, width = 0, height = 0, cornerRadius = 0;
  std::vector<Point> points;
};

// A polygon grown outward by `spread` with mitred corners: every edge moves
// out along its normal and each vertex goes to where its two moved edges
// meet, with D2D's default miter limit (10 x half the width) bevelled to the
// limit. Orientation-independent.
inline std::vector<Point> growPolygon(const std::vector<Point>& in, double spread) {
  const size_t n = in.size();
  if (spread <= 0 || n < 3) return in;
  double area = 0;
  for (size_t i = 0; i < n; ++i) {
    const auto& a = in[i];
    const auto& b = in[(i + 1) % n];
    area += a.x * b.y - b.x * a.y;
  }
  // Outward normal of edge a->b: (dy, -dx) for a clockwise (y-down positive
  // area) polygon, the reverse otherwise.
  const double sign = area > 0 ? 1 : -1;
  std::vector<Point> out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    const auto& prev = in[(i + n - 1) % n];
    const auto& at = in[i];
    const auto& next = in[(i + 1) % n];
    auto normal = [&](const Point& a, const Point& b) {
      const double dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
      return length > 0 ? Point{sign * dy / length, -sign * dx / length} : Point{0, 0};
    };
    const Point n0 = normal(prev, at), n1 = normal(at, next);
    Point bisector{n0.x + n1.x, n0.y + n1.y};
    const double cosine = n0.x * n1.x + n0.y * n1.y;
    const double denominator = 1 + cosine;  // 2 cos^2(theta/2)
    const double miter = denominator > 1e-12 ? std::sqrt(2 / denominator) : 1e9;  // 1 / cos(theta/2)
    if (miter <= 10) {
      const double scale = spread / denominator;
      out.push_back({at.x + bisector.x * scale, at.y + bisector.y * scale});
    } else {  // beyond the miter limit: bevel
      out.push_back({at.x + n0.x * spread, at.y + n0.y * spread});
      out.push_back({at.x + n1.x * spread, at.y + n1.y * spread});
    }
  }
  return out;
}

// Step 1: the shadow's polygon in template units.
inline std::vector<Point> shadowPolygon(const Outline& o, double dx, double dy, double spread, double tolerance) {
  std::vector<Point> points;
  if (o.kind == Outline::Kind::Polygon) {
    points = growPolygon(o.points, spread);
  } else {
    const double x = o.x - spread, y = o.y - spread, w = o.width + 2 * spread, h = o.height + 2 * spread;
    if (w <= 0 || h <= 0) return {};
    if (o.kind == Outline::Kind::Ellipse) {
      points = d2dcurves::ellipse(x + w / 2, y + h / 2, w / 2, h / 2, tolerance);
    } else if (o.cornerRadius > 0) {
      // A rounded rect grown by s is the rounded rect with radius r + s.
      const double radius = std::min(o.cornerRadius + spread, std::min(w, h) / 2);
      points = d2dcurves::roundedRect(x, y, w, h, radius, tolerance);
    } else {
      points = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
    }
  }
  for (auto& p : points) { p.x += dx; p.y += dy; }
  return points;
}

// Step 2: exact area coverage of a closed polygon (device px, y down) over
// the w x h buffer whose top-left device pixel is (x0, y0), as 0..65535.
// Signed-area accumulation: each edge deposits, per cell it crosses, the
// signed area to its right; a prefix sum along each row gives the winding
// coverage, clamped to 1 (non-zero for shapes that do not overlap themselves).
// Memory: 4 bytes per pixel of accumulation, then 4 of coverage.
inline std::vector<uint32_t> polygonCoverage(const std::vector<Point>& devicePoints, int x0, int y0, int w, int h) {
  std::vector<uint32_t> coverage;
  if (w <= 0 || h <= 0) return coverage;
  if (devicePoints.size() < 3) { coverage.assign(size_t(w) * size_t(h), 0); return coverage; }
  std::vector<float> accumulation(size_t(w + 2) * size_t(h), 0.f);
  const auto deposit = [&](double ax, double ay, double bx, double by) {
    // Relative to the buffer; one row strip at a time.
    ax -= x0; bx -= x0; ay -= y0; by -= y0;
    if (ay == by) return;
    const double direction = ay < by ? 1 : -1;
    if (ay > by) { std::swap(ax, bx); std::swap(ay, by); }
    const double top = std::max(ay, 0.0), bottom = std::min(by, double(h));
    if (top >= bottom) return;
    const double slope = (bx - ax) / (by - ay);
    for (int row = int(std::floor(top)); row < h && row < bottom; ++row) {
      const double sy0 = std::max(top, double(row)), sy1 = std::min(bottom, double(row + 1));
      if (sy1 <= sy0) continue;
      const double dyRow = (sy1 - sy0) * direction;
      const double xa = ax + (sy0 - ay) * slope, xb = ax + (sy1 - ay) * slope;
      float* line = accumulation.data() + size_t(row) * size_t(w + 2);
      const double left = std::min(xa, xb), right = std::max(xa, xb);
      // A piece of the segment inside cell c deposits the area right of it
      // within that cell into c and the rest into c + 1. Left of the buffer
      // everything lands in column 0; right of it nothing is visible (the
      // prefix sum runs left to right), so it is dropped.
      const auto piece = [&](double from, double to, double dyPiece) {
        if (to <= 0) { line[0] += float(dyPiece); return; }
        if (from >= w) return;
        const int c = int(std::floor((from + to) / 2));
        const double inside = (c + 1) - (from + to) / 2;
        line[c] += float(dyPiece * inside);
        line[c + 1] += float(dyPiece * (1 - inside));
      };
      if (right - left < 1e-12 || std::floor(left) == std::floor(right)) {
        piece(left, right, dyRow);
        continue;
      }
      // Spans cells: split at the cell lines (and the buffer's edges), each
      // piece taking its share of the row's height (y is linear in x).
      const double span = right - left;
      double from = left;
      while (from < right) {
        double to = from < 0 ? std::min(right, 0.0) : from >= w ? right : std::min(right, std::floor(from) + 1);
        if (to <= from) to = std::min(right, from + 1);  // from is an exact cell line
        piece(from, to, dyRow * (to - from) / span);
        from = to;
      }
    }
  };
  for (size_t i = 0; i < devicePoints.size(); ++i) {
    const auto& a = devicePoints[i];
    const auto& b = devicePoints[(i + 1) % devicePoints.size()];
    deposit(a.x, a.y, b.x, b.y);
  }
  coverage.resize(size_t(w) * size_t(h));
  for (int row = 0; row < h; ++row) {
    double sum = 0;
    const float* line = accumulation.data() + size_t(row) * size_t(w + 2);
    uint32_t* out = coverage.data() + size_t(row) * size_t(w);
    for (int column = 0; column < w; ++column) {
      sum += line[column];
      out[column] = uint32_t(std::lround(std::min(1.0, std::fabs(sum)) * 65535.0));
    }
  }
  return coverage;
}

// Step 3 helper: one box pass of half-width k along a line of `count` values
// spaced `stride` apart; outside the line is 0. Values are 0..65535, rounded.
inline void boxPass(uint32_t* values, int count, size_t stride, int k, std::vector<uint32_t>& scratch) {
  if (k <= 0 || count <= 0) return;
  scratch.assign(size_t(count), 0);
  const uint32_t width = uint32_t(2 * k + 1);
  uint64_t sum = 0;
  for (int i = 0; i < std::min(k, count); ++i) sum += values[size_t(i) * stride];
  for (int i = 0; i < count; ++i) {
    if (i + k < count) sum += values[size_t(i + k) * stride];
    if (i - k - 1 >= 0) sum -= values[size_t(i - k - 1) * stride];
    scratch[size_t(i)] = uint32_t((sum + width / 2) / width);
  }
  for (int i = 0; i < count; ++i) values[size_t(i) * stride] = scratch[size_t(i)];
}

// Step 3: blurs `values` (w x h, 0..65535) in place with half-width k.
inline void blurValues(std::vector<uint32_t>& values, int w, int h, int k) {
  if (w <= 0 || h <= 0 || k <= 0 || values.size() != size_t(w) * size_t(h)) return;
  std::vector<uint32_t> scratch;
  for (int pass = 0; pass < 3; ++pass)
    for (int row = 0; row < h; ++row) boxPass(values.data() + size_t(row) * size_t(w), w, 1, k, scratch);
  for (int pass = 0; pass < 3; ++pass)
    for (int column = 0; column < w; ++column) boxPass(values.data() + column, h, size_t(w), k, scratch);
}

// Step 4: premultiplied BGRA of `argb` at `opacity` for one blurred value.
inline void shadowPixel(uint32_t value, uint32_t argb, double opacity, uint8_t* px) {
  const double alpha = double((argb >> 24) & 0xff) * std::clamp(opacity, 0.0, 1.0);
  const uint32_t a = uint32_t(std::lround(double(value) * alpha / 65535.0));
  const uint32_t r = (argb >> 16) & 0xff, g = (argb >> 8) & 0xff, b = argb & 0xff;
  px[0] = uint8_t((b * a + 127) / 255);
  px[1] = uint8_t((g * a + 127) / 255);
  px[2] = uint8_t((r * a + 127) / 255);
  px[3] = uint8_t(a);
}

// The largest coverage buffer one shadow may use (4096 x 4096 px, 64 MiB at
// 4 bytes each); a larger one is refused (rasterize reports `refused`).
constexpr uint64_t kMaxBufferPixels = 4096ull * 4096ull;

// Everything a raster needs to composite one shadow: premultiplied BGRA
// pixels for the device-pixel rectangle (x, y, width, height), top row first.
struct ShadowRaster {
  int x = 0, y = 0, width = 0, height = 0;
  std::vector<uint8_t> bgra;
  bool refused = false;  // the buffer would exceed kMaxBufferPixels: nothing drawn
};

// The full pipeline. `outline` and the shadow parameters are in template
// units; device = template * scale + (offsetX, offsetY) (the drawTemplate
// transform). `clip` (template units) bounds the composite; the coverage
// buffer is the clip grown by the blur reach, so blur near the clip edge sees
// the shape beyond it. Pixels outside [0, targetWidth) x [0, targetHeight)
// are never produced.
inline ShadowRaster rasterize(const Outline& outline, uint32_t argb, double dx, double dy, double blur,
                              double spread, double opacity, double clipX, double clipY, double clipWidth,
                              double clipHeight, double scale, double offsetX, double offsetY, int targetWidth,
                              int targetHeight) {
  ShadowRaster out;
  if (!(scale > 0) || clipWidth <= 0 || clipHeight <= 0 || ((argb >> 24) & 0xff) == 0 || opacity <= 0) return out;
  const int k = boxRadius(blur * scale);
  const int reach = 3 * k + 1;
  // Composite rectangle: the clip in device pixels, rounded out, on the target.
  const int cx0 = std::max(0, int(std::floor(clipX * scale + offsetX)));
  const int cy0 = std::max(0, int(std::floor(clipY * scale + offsetY)));
  const int cx1 = std::min(targetWidth, int(std::ceil((clipX + clipWidth) * scale + offsetX)));
  const int cy1 = std::min(targetHeight, int(std::ceil((clipY + clipHeight) * scale + offsetY)));
  if (cx1 <= cx0 || cy1 <= cy0) return out;
  const int bx0 = cx0 - reach, by0 = cy0 - reach, bw = cx1 - cx0 + 2 * reach, bh = cy1 - cy0 + 2 * reach;
  if (uint64_t(bw) * uint64_t(bh) > kMaxBufferPixels) {
    out.refused = true;
    return out;
  }
  auto polygon = shadowPolygon(outline, dx, dy, spread, d2dcurves::kDefaultTolerance / scale);
  for (auto& p : polygon) { p.x = p.x * scale + offsetX; p.y = p.y * scale + offsetY; }
  auto values = polygonCoverage(polygon, bx0, by0, bw, bh);
  blurValues(values, bw, bh, k);
  out.x = cx0; out.y = cy0; out.width = cx1 - cx0; out.height = cy1 - cy0;
  out.bgra.resize(size_t(out.width) * size_t(out.height) * 4);
  for (int row = 0; row < out.height; ++row)
    for (int column = 0; column < out.width; ++column)
      shadowPixel(values[size_t(row + reach) * size_t(bw) + size_t(column + reach)], argb, opacity,
                  out.bgra.data() + (size_t(row) * size_t(out.width) + size_t(column)) * 4);
  return out;
}

}  // namespace corevideo::compositor::shapeshadow
