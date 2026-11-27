// D2DCurveFlattening.h must reproduce D2D's own polygons, point for point.
//
// The Mac raster draws ellipses and rounded corners from these polygons so its
// curved edges match the Windows raster (G4 parity). This test asks D2D for
// the polygon it draws (ID2D1Geometry::Simplify with LINES at the same
// tolerance) and compares every vertex. Windows only: it needs d2d1.dll.

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d2d1.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "compositor/D2DCurveFlattening.h"

#pragma comment(lib, "d2d1.lib")

namespace {
namespace curves = corevideo::compositor::d2dcurves;

class CollectingSink final : public ID2D1SimplifiedGeometrySink {
 public:
  std::vector<curves::Point> points;
  int figures = 0;
  bool beziers = false;
  STDMETHOD(QueryInterface)(REFIID, void**) override { return E_NOINTERFACE; }
  STDMETHOD_(ULONG, AddRef)() override { return 1; }
  STDMETHOD_(ULONG, Release)() override { return 1; }
  STDMETHOD_(void, SetFillMode)(D2D1_FILL_MODE) override {}
  STDMETHOD_(void, SetSegmentFlags)(D2D1_PATH_SEGMENT) override {}
  STDMETHOD_(void, BeginFigure)(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN) override {
    ++figures;
    add(p);
  }
  STDMETHOD_(void, AddLines)(const D2D1_POINT_2F* p, UINT32 n) override {
    for (UINT32 i = 0; i < n; ++i) add(p[i]);
  }
  STDMETHOD_(void, AddBeziers)(const D2D1_BEZIER_SEGMENT*, UINT32) override { beziers = true; }
  STDMETHOD_(void, EndFigure)(D2D1_FIGURE_END) override {}
  STDMETHOD(Close)() override { return S_OK; }

 private:
  // D2D repeats the joint between segments; the polygon is the same without it.
  void add(D2D1_POINT_2F p) {
    if (!points.empty() && points.back().x == p.x && points.back().y == p.y) return;
    points.push_back({p.x, p.y});
  }
};

std::vector<curves::Point> d2dPolygon(ID2D1Geometry* geometry, float tolerance) {
  CollectingSink sink;
  EXPECT_TRUE(SUCCEEDED(geometry->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_LINES, nullptr, tolerance, &sink)));
  EXPECT_EQ(1, sink.figures);
  EXPECT_FALSE(sink.beziers);
  auto& p = sink.points;
  if (p.size() > 1 && p.front().x == p.back().x && p.front().y == p.back().y) p.pop_back();
  return p;
}

// D2D computes in float; the header in double. 1e-3 px is far below anything
// a rasterizer can see and far above float rounding at these magnitudes.
std::string firstDifference(const std::vector<curves::Point>& expected, const std::vector<curves::Point>& actual) {
  char text[200];
  if (expected.size() != actual.size()) {
    std::snprintf(text, sizeof text, "D2D has %zu points, the header %zu", expected.size(), actual.size());
    return text;
  }
  for (size_t i = 0; i < expected.size(); ++i) {
    const double scale = std::max({1.0, std::fabs(expected[i].x), std::fabs(expected[i].y)});
    if (std::fabs(expected[i].x - actual[i].x) > 1e-5 * scale + 1e-3 ||
        std::fabs(expected[i].y - actual[i].y) > 1e-5 * scale + 1e-3) {
      std::snprintf(text, sizeof text, "point %zu: D2D (%.4f, %.4f), header (%.4f, %.4f)", i, expected[i].x,
                    expected[i].y, actual[i].x, actual[i].y);
      return text;
    }
  }
  return {};
}

struct Factory {
  ID2D1Factory* d2d = nullptr;
  Factory() { D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d); }
  ~Factory() { if (d2d) d2d->Release(); }
};

}  // namespace

TEST(D2DCurveFlattening, EllipsesMatchD2DPointForPoint) {
  Factory factory;
  ASSERT_TRUE(factory.d2d != nullptr);
  if (!factory.d2d) return;
  int checked = 0, failed = 0;
  std::string first;
  for (const float tolerance : {0.25f, 0.125f, 0.1f, 1.0f})
    for (int rx = 1; rx <= 600; rx += rx < 40 ? 1 : rx < 160 ? 3 : 17)
      for (const int ry : {rx, rx / 2 + 1, rx * 3 / 2 + 1}) {
        const float cx = 400.5f, cy = 110.25f;
        ID2D1EllipseGeometry* geometry = nullptr;
        if (FAILED(factory.d2d->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx, cy), float(rx), float(ry)), &geometry)))
          continue;
        const auto difference = firstDifference(d2dPolygon(geometry, tolerance), curves::ellipse(cx, cy, rx, ry, tolerance));
        geometry->Release();
        ++checked;
        if (!difference.empty() && ++failed == 1)
          first = "ellipse r " + std::to_string(rx) + "x" + std::to_string(ry) + " tolerance " + std::to_string(tolerance) + ": " + difference;
      }
  EXPECT_GT(checked, 1000);
  EXPECT_EQ(0, failed) << first;
}

TEST(D2DCurveFlattening, RoundedRectanglesMatchD2DPointForPoint) {
  Factory factory;
  ASSERT_TRUE(factory.d2d != nullptr);
  if (!factory.d2d) return;
  int checked = 0, failed = 0;
  std::string first;
  struct Box { float x, y, w, h; };
  for (const Box box : {Box{10, 10, 240, 200}, Box{0, 0, 1152, 162}, Box{13, 13, 234, 194}, Box{2.5f, 7.25f, 61, 33}})
    for (const float tolerance : {0.25f, 0.125f})
      for (float radius = 1; radius <= 120; radius += radius < 30 ? 1 : 7) {
        ID2D1RoundedRectangleGeometry* geometry = nullptr;
        if (FAILED(factory.d2d->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(D2D1::RectF(box.x, box.y, box.x + box.w, box.y + box.h), radius, radius), &geometry)))
          continue;
        // Radii over half a side are clamped by both (spec 5.7); stop where D2D's clamp starts.
        if (radius > std::min(box.w, box.h) * 0.5f) { geometry->Release(); continue; }
        const auto difference = firstDifference(d2dPolygon(geometry, tolerance),
                                                curves::roundedRect(box.x, box.y, box.w, box.h, radius, tolerance));
        geometry->Release();
        ++checked;
        if (!difference.empty() && ++failed == 1)
          first = "rounded rect " + std::to_string(box.w) + "x" + std::to_string(box.h) + " r " + std::to_string(radius) +
                  " tolerance " + std::to_string(tolerance) + ": " + difference;
      }
  EXPECT_GT(checked, 100);
  EXPECT_EQ(0, failed) << first;
}

// The reason this exists: D2D's chords lie inside the true curve, so its
// antialiased edge holds less coverage than an exact ellipse. Pin the size of
// the effect the Mac raster now reproduces (area of polygon < area of ellipse).
TEST(D2DCurveFlattening, ChordsLieInsideTheCurve) {
  const auto polygon = curves::ellipse(400, 110, 120, 100, curves::kDefaultTolerance);
  double area = 0;
  for (size_t i = 0; i < polygon.size(); ++i) {
    const auto& a = polygon[i];
    const auto& b = polygon[(i + 1) % polygon.size()];
    area += a.x * b.y - b.x * a.y;
  }
  area = std::fabs(area) / 2;
  const double exact = 3.14159265358979 * 120 * 100;
  const double perimeter = 2 * 3.14159265358979 * std::sqrt((120.0 * 120 + 100.0 * 100) / 2);
  const double meanInset = (exact - area) / perimeter;  // px the edge sits inside, on average
  EXPECT_GT(meanInset, 0.05);
  EXPECT_LT(meanInset, 0.25);
}

#endif  // _WIN32
