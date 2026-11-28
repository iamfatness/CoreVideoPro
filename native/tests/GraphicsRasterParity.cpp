#include "GraphicsRasterParity.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

#include "graphics/GraphicsAssets.h"
#include "graphics/GraphicsCompatibility.h"
#include "graphics/GraphicsMotion.h"
#include "modules/OverlayTileRaster.h"

namespace corevideo::graphics::parity {
namespace {

using rpc::Json;

std::string format(const char* pattern, double a, double b, double tolerance) {
  char buffer[160];
  std::snprintf(buffer, sizeof(buffer), pattern, a, b, tolerance);
  return buffer;
}

Json box(const GraphicsBox& b, double scale, double dx = 0, double dy = 0) {
  return Json::Array{b.x * scale + dx, b.y * scale + dy, b.width * scale, b.height * scale};
}

GraphicsBox readBox(const Json* value) {
  GraphicsBox b;
  if (!value || !value->isArray() || value->asArray().size() != 4) return b;
  const auto& a = value->asArray();
  b.x = a[0].asNumber(); b.y = a[1].asNumber(); b.width = a[2].asNumber(); b.height = a[3].asNumber();
  return b;
}

// Premultiplied BGRA of a straight ARGB colour.
std::array<double, 4> premultipliedBgra(uint32_t argb) {
  const double a = double((argb >> 24) & 0xff);
  const auto channel = [&](int shift) { return double((argb >> shift) & 0xff) * a / 255.0; };
  return {channel(0), channel(8), channel(16), a};
}

const uint8_t* pixel(const ParityImage& image, int x, int y) {
  return image.bgraPremultiplied.data() + (size_t(y) * size_t(image.width) + size_t(x)) * 4;
}

struct Area { int x0, y0, x1, y1; };
Area measuredArea(const ParityImage& image, const ParityRegion& region) {
  Area a{int(std::floor(region.x - region.grow)), int(std::floor(region.y - region.grow)),
         int(std::ceil(region.x + region.width + region.grow)), int(std::ceil(region.y + region.height + region.grow))};
  if (region.text && region.clipped) {
    a.x0 = std::max(a.x0, int(std::floor(region.clipX)));
    a.y0 = std::max(a.y0, int(std::floor(region.clipY)));
    a.x1 = std::min(a.x1, int(std::ceil(region.clipX + region.clipWidth)));
    a.y1 = std::min(a.y1, int(std::ceil(region.clipY + region.clipHeight)));
  }
  a.x0 = std::clamp(a.x0, 0, image.width); a.x1 = std::clamp(a.x1, 0, image.width);
  a.y0 = std::clamp(a.y0, 0, image.height); a.y1 = std::clamp(a.y1, 0, image.height);
  return a;
}

// The most common colour on the area's border, in this image.
std::array<double, 4> background(const ParityImage& image, const Area& a) {
  std::map<uint32_t, int> counts;
  const auto add = [&](int x, int y) {
    const auto* p = pixel(image, x, y);
    ++counts[uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24];
  };
  for (int x = a.x0; x < a.x1; ++x) { add(x, a.y0); if (a.y1 - 1 > a.y0) add(x, a.y1 - 1); }
  for (int y = a.y0 + 1; y < a.y1 - 1; ++y) { add(a.x0, y); if (a.x1 - 1 > a.x0) add(a.x1 - 1, y); }
  uint32_t best = 0; int bestCount = -1;
  for (const auto& [value, count] : counts) if (count > bestCount) { best = value; bestCount = count; }
  return {double(best & 0xff), double((best >> 8) & 0xff), double((best >> 16) & 0xff), double(best >> 24)};
}

// Per-pixel coverage of the region's text colour over its background, 0..255.
std::vector<double> coverage(const ParityImage& image, const ParityRegion& region, Area& a) {
  a = measuredArea(image, region);
  std::vector<double> out(size_t(std::max(0, a.x1 - a.x0)) * size_t(std::max(0, a.y1 - a.y0)), 0);
  if (out.empty()) return out;
  const auto bg = background(image, a);
  const auto fg = premultipliedBgra(region.colorArgb);
  std::array<double, 4> d{fg[0] - bg[0], fg[1] - bg[1], fg[2] - bg[2], fg[3] - bg[3]};
  const double norm = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + d[3] * d[3];
  size_t i = 0;
  for (int y = a.y0; y < a.y1; ++y)
    for (int x = a.x0; x < a.x1; ++x, ++i) {
      const auto* p = pixel(image, x, y);
      if (norm <= 0) { out[i] = 0; continue; }
      double dot = 0;
      for (int c = 0; c < 4; ++c) dot += (double(p[c]) - bg[c]) * d[c];
      out[i] = std::clamp(dot / norm, 0.0, 1.0) * 255.0;
    }
  return out;
}

std::vector<double> blur3(const std::vector<double>& v, int w, int h) {
  std::vector<double> tmp(v.size()), out(v.size());
  const auto at = [&](const std::vector<double>& s, int x, int y) {
    return s[size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(std::clamp(x, 0, w - 1))];
  };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) tmp[size_t(y) * w + x] = (at(v, x - 1, y) + 2 * at(v, x, y) + at(v, x + 1, y)) / 4;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) out[size_t(y) * w + x] = (at(tmp, x, y - 1) + 2 * at(tmp, x, y) + at(tmp, x, y + 1)) / 4;
  return out;
}

double ssim(const std::vector<double>& a, const std::vector<double>& b, int w, int h) {
  if (w < 7 || h < 7) {
    // Too small for a 7x7 window: compare as one window.
    double ma = 0, mb = 0;
    for (size_t i = 0; i < a.size(); ++i) { ma += a[i]; mb += b[i]; }
    if (a.empty()) return 1;
    ma /= double(a.size()); mb /= double(b.size());
    double va = 0, vb = 0, cov = 0;
    for (size_t i = 0; i < a.size(); ++i) { va += (a[i] - ma) * (a[i] - ma); vb += (b[i] - mb) * (b[i] - mb); cov += (a[i] - ma) * (b[i] - mb); }
    va /= double(a.size()); vb /= double(a.size()); cov /= double(a.size());
    const double c1 = std::pow(0.01 * 255, 2), c2 = std::pow(0.03 * 255, 2);
    return ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
  }
  const double c1 = std::pow(0.01 * 255, 2), c2 = std::pow(0.03 * 255, 2);
  double total = 0; int windows = 0;
  for (int y = 0; y + 7 <= h; ++y)
    for (int x = 0; x + 7 <= w; ++x) {
      double ma = 0, mb = 0;
      for (int j = 0; j < 7; ++j) for (int i = 0; i < 7; ++i) { ma += a[size_t(y + j) * w + x + i]; mb += b[size_t(y + j) * w + x + i]; }
      ma /= 49; mb /= 49;
      double va = 0, vb = 0, cov = 0;
      for (int j = 0; j < 7; ++j)
        for (int i = 0; i < 7; ++i) {
          const double da = a[size_t(y + j) * w + x + i] - ma, db = b[size_t(y + j) * w + x + i] - mb;
          va += da * da; vb += db * db; cov += da * db;
        }
      va /= 48; vb /= 48; cov /= 48;
      total += ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
      ++windows;
    }
  return windows ? total / windows : 1;
}

bool insideAny(const std::vector<ParityRegion>& regions, bool text, int x, int y) {
  for (const auto& r : regions) {
    if (r.text != text) continue;
    if (x >= r.x - r.grow && x < r.x + r.width + r.grow && y >= r.y - r.grow && y < r.y + r.height + r.grow) return true;
  }
  return false;
}

std::string readFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), {});
}

}  // namespace

// ── Layout summary and regions ───────────────────────────────────────────────

Json layoutSummary(const GraphicsDrawing& drawing, double scale, bool atlas) {
  Json::Array primitives;
  std::vector<GraphicsRun> runs;
  std::vector<GraphicsAtlasSlice> slices;
  int atlasWidth = 0, atlasHeight = 0;
  const bool packed = atlas && [&] {
    runs = planRuns(drawing);
    return packAtlas(runs, scale, slices, atlasWidth, atlasHeight);
  }();
  std::vector<std::pair<double, double>> offset(drawing.primitives.size(), {0.0, 0.0});
  std::vector<GraphicsBox> sliceClip(drawing.primitives.size());
  std::vector<bool> placed(drawing.primitives.size(), !packed);
  Json::Array sliceRows;
  if (packed) {
    for (size_t r = 0; r < runs.size(); ++r) {
      const auto& run = runs[r];
      const auto& slice = slices[r];
      sliceRows.push_back(Json::Object{{"x", slice.x}, {"y", slice.y}, {"width", slice.width}, {"height", slice.height},
          {"first", double(run.first)}, {"count", double(run.count)}, {"source", box(run.source, scale)}});
      for (size_t i = run.first; i < run.first + run.count && i < drawing.primitives.size(); ++i) {
        offset[i] = {slice.x - run.source.x * scale, slice.y - run.source.y * scale};
        sliceClip[i] = GraphicsBox{double(slice.x), double(slice.y), double(slice.width), double(slice.height)};
        placed[i] = true;
      }
    }
  }
  for (size_t i = 0; i < drawing.primitives.size(); ++i) {
    const auto& p = drawing.primitives[i];
    if (!placed[i]) continue;
    Json::Object row{{"id", p.id}, {"type", p.type},
        {"box", box(p.box, scale, offset[i].first, offset[i].second)},
        {"clip", packed ? box(sliceClip[i], 1.0) : box(p.clip, scale)},
        {"fontSize", p.fontSize * scale}, {"lineHeight", p.lineHeight}, {"maxLines", double(p.maxLines)},
        {"color", double(p.color)}, {"strokeWidth", p.strokeWidth * scale},
        {"effects", (p.outline ? p.outline->width : 0.0) * scale + (p.shadow ? (std::abs(p.shadow->x) + std::abs(p.shadow->y) + 2 * p.shadow->blur) : 0.0) * scale}};
    if (p.crawlStart) row["crawlStart"] = *p.crawlStart * scale;
    // A shape shadow (styles P4) paints its clip, outside the box: judged as
    // its own geometry region so the blur model is held to tier 1 too.
    if (p.shapeShadow) row["shadow"] = box(p.shadowClip, scale, offset[i].first, offset[i].second);
    primitives.push_back(std::move(row));
  }
  Json::Object summary{{"width", drawing.width * scale}, {"height", drawing.height * scale}, {"scale", scale},
                       {"primitives", std::move(primitives)}};
  if (drawing.crawl) summary["crawl"] = Json::Object{{"passWidth", drawing.crawl->passWidth * scale}};
  if (packed) summary["slices"] = std::move(sliceRows);
  return summary;
}

std::vector<ParityRegion> regionsFromLayout(const Json& summary) {
  std::vector<ParityRegion> out;
  const auto* primitives = summary.get("primitives");
  if (!primitives || !primitives->isArray()) return out;
  for (const auto& row : primitives->asArray()) {
    ParityRegion r;
    r.id = row.getString("id");
    const auto type = row.getString("type");
    r.text = type == "text";
    auto b = readBox(row.get("box"));
    const auto clip = readBox(row.get("clip"));
    // Judge only what is drawn: the box inside its clip.
    const double x0 = std::max(b.x, clip.x), y0 = std::max(b.y, clip.y);
    const double x1 = std::min(b.x + b.width, clip.x + clip.width), y1 = std::min(b.y + b.height, clip.y + clip.height);
    if (x1 <= x0 || y1 <= y0) continue;
    r.x = x0; r.y = y0; r.width = x1 - x0; r.height = y1 - y0;
    r.colorArgb = uint32_t(row.getNumber("color"));
    if (r.text) {
      const double size = row.getNumber("fontSize");
      r.linePitch = size * row.getNumber("lineHeight", 1.2);
      r.maxLines = int(row.getNumber("maxLines", 1));
      r.grow = std::ceil(0.35 * size + row.getNumber("effects"));
      if (row.get("clip")) {
        r.clipped = true;
        r.clipX = clip.x; r.clipY = clip.y; r.clipWidth = clip.width; r.clipHeight = clip.height;
      }
    } else {
      r.grow = 2 + std::ceil(row.getNumber("strokeWidth"));
    }
    if (const auto* shadowBox = row.get("shadow")) {
      ParityRegion shadow;
      shadow.id = r.id + ".shadow";
      const auto s = readBox(shadowBox);
      shadow.x = s.x; shadow.y = s.y; shadow.width = s.width; shadow.height = s.height;
      shadow.colorArgb = r.colorArgb;
      out.push_back(std::move(shadow));
    }
    out.push_back(std::move(r));
  }
  return out;
}

// ── Metrics ──────────────────────────────────────────────────────────────────

RegionMetrics measureRegion(const ParityImage& image, const ParityRegion& region) {
  RegionMetrics m;
  m.inkX0 = image.width; m.inkY0 = image.height; m.inkX1 = 0; m.inkY1 = 0;
  Area a{};
  std::vector<double> alpha;
  if (region.text) alpha = coverage(image, region, a);
  else {
    a = measuredArea(image, region);
    for (int y = a.y0; y < a.y1; ++y)
      for (int x = a.x0; x < a.x1; ++x) alpha.push_back(pixel(image, x, y)[3]);
  }
  const int w = a.x1 - a.x0;
  std::set<int> bands;
  size_t i = 0;
  for (int y = a.y0; y < a.y1; ++y)
    for (int x = a.x0; x < a.x1; ++x, ++i) {
      const double v = alpha[i];
      m.summedAlpha += v;
      if (v > 8) {
        m.inkX0 = std::min(m.inkX0, x); m.inkY0 = std::min(m.inkY0, y);
        m.inkX1 = std::max(m.inkX1, x + 1); m.inkY1 = std::max(m.inkY1, y + 1);
        if (region.linePitch > 0 && y >= region.y) bands.insert(int((y - region.y) / region.linePitch));
      }
      if (v > 128) m.baselineRow = std::max(m.baselineRow, y);
    }
  (void)w;
  if (m.inkX1 <= m.inkX0) { m.inkX0 = m.inkY0 = m.inkX1 = m.inkY1 = 0; }
  m.lineBands = int(bands.size());
  return m;
}

double alphaSsim(const ParityImage& reference, const ParityImage& candidate, const ParityRegion& region) {
  Area ar{}, ac{};
  const auto a = coverage(reference, region, ar);
  const auto b = coverage(candidate, region, ac);
  const int w = ar.x1 - ar.x0, h = ar.y1 - ar.y0;
  if (a.size() != b.size() || w <= 0 || h <= 0) return 0;
  return ssim(blur3(a, w, h), blur3(b, w, h), w, h);
}

// ── Judges ───────────────────────────────────────────────────────────────────

Verdict judgeGeometry(const ParityImage& reference, const ParityImage& candidate, const std::vector<ParityRegion>& regions) {
  Verdict v;
  const auto fail = [&](std::string message) { v.pass = false; v.failures.push_back(std::move(message)); };
  if (reference.width != candidate.width || reference.height != candidate.height ||
      reference.bgraPremultiplied.size() != candidate.bgraPremultiplied.size()) {
    fail("size: reference " + std::to_string(reference.width) + "x" + std::to_string(reference.height) +
         " vs candidate " + std::to_string(candidate.width) + "x" + std::to_string(candidate.height));
    return v;
  }
  const int W = reference.width, H = reference.height;
  // Reference ink box over the whole image (alpha > 8), grown by 1 px.
  int x0 = W, y0 = H, x1 = 0, y1 = 0;
  double refAlpha = 0, candAlpha = 0;
  // Text regions (grown) are tier 2's: excluded from the fixture's summed
  // alpha, the reference ink box and the stray-ink check, or a text-only
  // fixture would be held to 0.5% and 1 px instead of tier 2's 8% and 2 px.
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      if (insideAny(regions, true, x, y)) continue;
      const auto ra = pixel(reference, x, y)[3];
      refAlpha += ra; candAlpha += pixel(candidate, x, y)[3];
      if (ra > 8) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x + 1); y1 = std::max(y1, y + 1); }
    }
  int stray = 0, firstX = -1, firstY = -1, strayAlpha = 0;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      if (insideAny(regions, true, x, y)) continue;
      if (x >= x0 - 1 && x < x1 + 1 && y >= y0 - 1 && y < y1 + 1) continue;
      const int ca = pixel(candidate, x, y)[3];
      if (ca > 8) { if (!stray++) { firstX = x; firstY = y; strayAlpha = ca; } }
    }
  if (stray)
    fail("stray ink: " + std::to_string(stray) + " px outside the reference ink box grown by 1 px, first at " +
         std::to_string(firstX) + "," + std::to_string(firstY) + " alpha " + std::to_string(strayAlpha) + " (tolerance 8)");
  if (refAlpha > 0 && std::abs(candAlpha - refAlpha) > 0.005 * refAlpha)
    fail(format("summed alpha: reference %.0f vs candidate %.0f (tolerance %.1f%%)", refAlpha, candAlpha, 0.5));
  for (const auto& region : regions) {
    if (region.text) continue;
    const auto a = measuredArea(reference, region);
    int interiorBad = 0, edgeBad = 0, worstInterior = 0, worstEdge = 0;
    for (int y = a.y0; y < a.y1; ++y)
      for (int x = a.x0; x < a.x1; ++x) {
        if (insideAny(regions, true, x, y)) continue;  // text pixels: tier 2
        int range = 0;
        for (int c = 0; c < 4; ++c) {
          int lo = 255, hi = 0;
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              const int nx = std::clamp(x + dx, 0, W - 1), ny = std::clamp(y + dy, 0, H - 1);
              const int s = pixel(reference, nx, ny)[c];
              lo = std::min(lo, s); hi = std::max(hi, s);
            }
          range = std::max(range, hi - lo);
        }
        const bool edge = range > 2;
        int diff = 0;
        for (int c = 0; c < 4; ++c) diff = std::max(diff, std::abs(int(pixel(reference, x, y)[c]) - int(pixel(candidate, x, y)[c])));
        if (edge) { if (diff > 48) ++edgeBad; worstEdge = std::max(worstEdge, diff); }
        else { if (diff > 2) ++interiorBad; worstInterior = std::max(worstInterior, diff); }
      }
    if (interiorBad)
      fail(region.id + " interior: " + std::to_string(interiorBad) + " px off by more than 2/255 (worst " +
           std::to_string(worstInterior) + ")");
    if (edgeBad)
      fail(region.id + " edge band: " + std::to_string(edgeBad) + " px off by more than 48/255 (worst " +
           std::to_string(worstEdge) + ")");
  }
  return v;
}

Verdict judgeText(const ParityImage& reference, const ParityImage& candidate, const std::vector<ParityRegion>& regions,
                  double s) {
  Verdict v;
  const auto fail = [&](std::string message) { v.pass = false; v.failures.push_back(std::move(message)); };
  if (reference.width != candidate.width || reference.height != candidate.height) {
    fail("size differs");
    return v;
  }
  for (const auto& region : regions) {
    if (!region.text) continue;
    const auto r = measureRegion(reference, region), c = measureRegion(candidate, region);
    const auto& id = region.id;
    if (r.lineBands != c.lineBands)
      fail(id + " line bands: reference " + std::to_string(r.lineBands) + " vs candidate " + std::to_string(c.lineBands) + " (must be equal)");
    const double edge = 2 * s;
    const std::array<std::pair<const char*, std::pair<int, int>>, 4> edges{{
        {"ink left", {r.inkX0, c.inkX0}}, {"ink top", {r.inkY0, c.inkY0}},
        {"ink right", {r.inkX1, c.inkX1}}, {"ink bottom", {r.inkY1, c.inkY1}}}};
    for (const auto& [name, values] : edges)
      if (std::abs(values.first - values.second) > edge)
        fail(id + " " + name + format(": reference %.0f vs candidate %.0f (tolerance %.0f px)", values.first, values.second, edge));
    // The baseline is computed, never scaled: a miss is a bug, not a font difference (spec 7.3).
    if (std::abs(r.baselineRow - c.baselineRow) > 1)
      fail(id + format(" baseline: reference %.0f vs candidate %.0f (tolerance %.0f px)", r.baselineRow, c.baselineRow, 1));
    if (r.summedAlpha > 0 && std::abs(c.summedAlpha - r.summedAlpha) > 0.08 * s * r.summedAlpha)
      fail(id + format(" summed alpha: reference %.0f vs candidate %.0f (tolerance %.0f%%)", r.summedAlpha, c.summedAlpha, 8 * s));
    const double structure = alphaSsim(reference, candidate, region);
    const double floor = 1 - 0.10 * s;
    if (structure < floor) fail(id + format(" SSIM: %.3f vs floor %.3f (blurred coverage)%.0f", structure, floor, 0));
  }
  return v;
}

Verdict judgeLayout(const Json& reference, const Json& candidate) {
  Verdict v;
  const auto fail = [&](std::string message) { v.pass = false; v.failures.push_back(std::move(message)); };
  const auto within = [](double a, double b) { return std::abs(a - b) <= std::max(1.5, 0.015 * std::abs(a)); };
  const auto* rp = reference.get("primitives");
  const auto* cp = candidate.get("primitives");
  if (!rp || !cp || !rp->isArray() || !cp->isArray() || rp->asArray().size() != cp->asArray().size()) {
    fail("primitive count differs");
    return v;
  }
  for (size_t i = 0; i < rp->asArray().size(); ++i) {
    const auto& a = rp->asArray()[i];
    const auto& b = cp->asArray()[i];
    const auto id = a.getString("id") + "#" + std::to_string(i);
    if (a.getString("id") != b.getString("id") || a.getString("type") != b.getString("type")) {
      fail(id + ": primitive differs (" + b.getString("id") + ")");
      continue;
    }
    const auto ab = readBox(a.get("box")), bb = readBox(b.get("box"));
    if (!within(ab.width, bb.width))
      fail(id + format(" width: reference %.2f vs candidate %.2f (tolerance %.2f px)", ab.width, bb.width, std::max(1.5, 0.015 * ab.width)));
    if (!within(ab.x, bb.x))
      fail(id + format(" x: reference %.2f vs candidate %.2f (tolerance %.2f px)", ab.x, bb.x, std::max(1.5, 0.015 * std::abs(ab.x))));
    const double fa = a.getNumber("fontSize"), fb = b.getNumber("fontSize");
    if (std::abs(fa - fb) > fa / 256 + 1e-9)
      fail(id + format(" fit font size: reference %.3f vs candidate %.3f (tolerance %.3f)", fa, fb, fa / 256));
  }
  const auto* rc = reference.get("crawl");
  const auto* cc = candidate.get("crawl");
  if (bool(rc) != bool(cc)) fail("crawl presence differs");
  else if (rc) {
    const double a = rc->getNumber("passWidth"), b = cc->getNumber("passWidth");
    if (std::abs(a - b) > std::max(1.5, 0.015 * a))
      fail(format("crawl passWidth: reference %.2f vs candidate %.2f (tolerance %.2f px)", a, b, std::max(1.5, 0.015 * a)));
  }
  return v;
}

// ── Fixture index ────────────────────────────────────────────────────────────

std::vector<ParityFixture> loadParityIndex(const std::string& indexPath, std::string& error) {
  std::vector<ParityFixture> out;
  const auto parsed = Json::parse(readFile(indexPath));
  if (!parsed || !parsed->get("fixtures") || !parsed->get("fixtures")->isArray()) {
    error = "unreadable index: " + indexPath;
    return out;
  }
  for (const auto& row : parsed->get("fixtures")->asArray()) {
    ParityFixture f;
    f.name = row.getString("name");
    f.source = row.getString("source");
    f.fields = row.get("fields") ? *row.get("fields") : Json(Json::Object{});
    f.brand = row.get("brand") ? *row.get("brand") : Json(Json::Object{});
    f.width = int(row.getNumber("width", 0));
    f.height = int(row.getNumber("height", 0));
    f.atlas = row.get("atlas") && row.get("atlas")->isBool() && row.get("atlas")->asBool();
    f.textTolerance = row.getNumber("textTolerance", 1);
    f.recordedDifference = row.getString("recordedDifference");
    out.push_back(std::move(f));
  }
  error.clear();
  return out;
}

namespace {
// The brand the library thumbnails use for the default look (GraphicsThumbnail.cpp).
Json defaultBrand() {
  const auto a = defaultAppearance("compact-solid");
  return Json::Object{{"brand.primary", a.backgroundColor}, {"brand.onPrimary", a.nameColor},
      {"brand.onPrimaryMuted", a.titleColor}, {"brand.accent", a.accentColor}, {"brand.fontBody", a.fontFamily},
      {"brand.fontHeading", a.fontFamily}, {"brand.secondary", a.titleColor}, {"brand.background", a.backgroundColor},
      {"brand.foreground", a.nameColor}, {"brand.logo", "logo"}};
}
// Arial, not Segoe UI: parity fixtures use fonts both platforms ship so they
// measure the rasterizers (spec 7.3). With Segoe UI the Mac drew Selawik and
// the title read 8.6% heavy from the font alone (Selawik through D2D: +12.9%).
// Segoe UI to Selawik has its own fixture, segoe-substitution. Owner ruling
// 2026-11-27. The shared GraphicsMotionFixture.h motionBrand() is unchanged.
Json motionBrand() {
  return Json::Object{{"brand.primary", "#203040"}, {"brand.onPrimary", "#FFFFFF"},
      {"brand.accent", "#44C1A1"}, {"brand.fontBody", "Arial"}};
}
}  // namespace

bool parityInput(const ParityFixture& fixture, const std::string& fixturesDir, ParityInput& out, std::string& error) {
  out = ParityInput{};
  const auto colon = fixture.source.find(':');
  const auto kind = fixture.source.substr(0, colon), value = fixture.source.substr(colon + 1);
  if (kind == "legacy") {
    auto& o = out.overlay;
    o.keyPhase = "on-air";
    o.keyProgress = 1;
    if (value == "caption") {
      o.isCaption = true;
      o.text = fixture.fields.getString("text");
      o.speaker = fixture.fields.getString("speaker");
    } else {
      o.text = fixture.fields.getString("name");
      o.title = fixture.fields.getString("title");
    }
    o.appearance = defaultAppearance("compact-solid");
    o.fontFamily = o.appearance.fontFamily;
    o.brandColor = "#44c1a1";
    out.width = fixture.width > 0 ? fixture.width : 1152;
    out.height = fixture.height > 0 ? fixture.height : 162;
    return true;
  }
  std::shared_ptr<const GraphicsTemplate> definition;
  if (kind == "curated") {
    for (const auto& candidate : GraphicsTemplate::curated())
      if (candidate->id() == value) definition = candidate;
    if (!definition) { error = "unknown curated template " + value; return false; }
  } else if (kind == "manifest") {
    const auto path = fixturesDir + "/" + value;
    const auto manifest = readFile(path);
    const auto parsed = Json::parse(manifest);
    if (!parsed) { error = "unreadable manifest " + path; return false; }
    GraphicsAssets assets;
    const auto slash = path.rfind('/');
    if (!loadGraphicsAssets(*parsed, path.substr(0, slash), assets, error)) return false;
    definition = GraphicsTemplate::load(manifest, error, std::move(assets));
    if (!definition) return false;
  } else {
    error = "unknown fixture source " + fixture.source;
    return false;
  }
  auto input = std::make_shared<GraphicsTemplateInput>();
  input->definition = definition;
  auto fields = definition->defaultFields().asObject();
  if (fixture.fields.isObject())
    for (const auto& [key, field] : fixture.fields.asObject()) fields[key] = field;
  input->fields = Json(std::move(fields));
  input->brand = fixture.brand.isString() && fixture.brand.asString() == "motion" ? motionBrand()
               : (fixture.brand.isObject() && !fixture.brand.asObject().empty()) ? fixture.brand : defaultBrand();
  if (!definition->validateFields(input->fields, false, error)) return false;
  if (!definition->resolve(input->fields, input->brand, out.drawing, error)) return false;
  out.templated = true;
  out.width = fixture.width > 0 ? fixture.width : int(std::lround(definition->width()));
  out.height = fixture.height > 0 ? fixture.height : int(std::lround(definition->height()));
  out.scale = out.drawing.height > 0 ? double(out.height) / out.drawing.height : 1;
  out.overlay.appearance.enabled = true;
  out.overlay.graphicsTemplate = std::move(input);
  out.overlay.graphicsContentHash = definition->hash();
  out.overlay.hasGraphicsContentHash = true;
  return true;
}

Json fixtureLayout(const ParityFixture& fixture, const ParityInput& input) {
  if (input.templated) return layoutSummary(input.drawing, input.scale, fixture.atlas);
  // Legacy: the shared tile layout (OverlayTileRaster), text lines and the accent bar.
  const auto layout = modules::computeOverlayTileLayout(input.overlay, input.width, input.height);
  Json::Array primitives;
  for (size_t i = 0; i < layout.textLines.size(); ++i) {
    const auto& line = layout.textLines[i];
    primitives.push_back(Json::Object{{"id", "line-" + std::to_string(i)}, {"type", "text"},
        {"box", Json::Array{line.rect.x, line.rect.y, line.rect.width, line.rect.height}},
        {"clip", Json::Array{0.0, 0.0, double(input.width), double(input.height)}},
        // The caption layout reports no font size; a line's box is one line high.
        {"fontSize", line.fontSize > 0 ? double(line.fontSize) : double(line.rect.height) / 1.2},
        {"lineHeight", 1.2}, {"maxLines", 1.0}, {"color", double(line.colorArgb)},
        {"strokeWidth", 0.0}, {"effects", 0.0}});
  }
  if (layout.accentBar.width > 0)
    primitives.push_back(Json::Object{{"id", "accent"}, {"type", "rect"},
        {"box", Json::Array{layout.accentBar.x, layout.accentBar.y, layout.accentBar.width, layout.accentBar.height}},
        {"clip", Json::Array{0.0, 0.0, double(input.width), double(input.height)}},
        {"fontSize", 0.0}, {"lineHeight", 1.0}, {"maxLines", 1.0}, {"color", double(layout.accentArgb)},
        {"strokeWidth", 0.0}, {"effects", 0.0}});
  return Json::Object{{"width", double(input.width)}, {"height", double(input.height)}, {"scale", 1.0},
                      {"primitives", std::move(primitives)}};
}

}  // namespace corevideo::graphics::parity
