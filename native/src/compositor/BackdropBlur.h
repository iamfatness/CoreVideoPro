#pragma once

// P9 true backdrop blur ("glass"): the portable half of the compositor pass.
//
// A glass plate (`backdrop: {blur, tint}` on a template rect) blurs the Program
// pixels already composited beneath it, tints them, and masks the result to the
// plate's rounded rectangle, all before the graphic's own raster is drawn over
// it. It is a GPU pass in each compositor (D3D11BackdropBlur on Windows,
// MetalBackdropBlur on the Mac), never in the overlay raster, which cannot see
// Program. Owner ruling 2026-11-27: true blur only, no frosted fallback.
//
// Everything both GPU passes must agree on lives here, so the two kernels are
// the same numbers by construction:
//   - the kernel plan (downsample factor, tap radius, normalized weights);
//   - the region of Program read, aligned to a FIXED grid so a moving plate
//     over a still picture does not shimmer;
//   - the per-frame geometry of a plate (motion, clip, opacity, scale);
//   - the constant-buffer layout both shaders read;
//   - a CPU reference of the whole pass. Backdrop parity is judged against it:
//     each GPU's output must match this reference within the tolerance in
//     kBackdropReferenceTolerance, on its own platform, so Windows and Mac
//     match each other through it. (The raster parity harness compares overlay
//     rasters, which never contain the backdrop, so it cannot judge this pass.)
//
// The pass (identical stages on both GPUs):
//   1. downsample: average each d x d block of the region into a half-float
//      texture (exact box filter, integer loads clamped to the frame);
//   2. horizontal Gaussian, 3. vertical Gaussian (2r+1 discrete taps, loads
//      clamped to the low-resolution region);
//   4. composite: bilinear upsample (manual, clamped), tint, rounded-rect
//      coverage, straight-alpha blend over Program inside the clip scissor.
//
// Nothing here allocates: a resolved draw is a POD, the plates were collected
// with the prepared raster (graphics::collectBackdrops).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "compositor/CompositorLayout.h"
#include "graphics/GraphicsMotion.h"
#include "graphics/GraphicsTemplate.h"

namespace corevideo::compositor {

// Low-resolution tap radius cap: 2r+1 = 25 taps per separable pass.
inline constexpr int kBackdropMaxRadius = 12;
// sigma in low-resolution pixels never exceeds this, so 3 sigma fits the cap.
inline constexpr double kBackdropMaxLowSigma = 4.0;
inline constexpr int kBackdropMinDownsample = 2;
inline constexpr int kBackdropMaxDownsample = 32;
// GPU vs CPU reference, per 8-bit channel: no channel may differ by more than
// this many levels. Half-float intermediates and GPU blend precision account
// for the difference; it is a new fixture tolerance, not a loosened one.
inline constexpr int kBackdropReferenceTolerance = 2;

struct BackdropKernel {
  int downsample = kBackdropMinDownsample;
  int radius = 1;
  float weights[16]{};  // weights[0..radius], symmetric, sum over -r..r == 1
};

// sigmaPx: Gaussian sigma in TARGET pixels. The downsample factor d is the
// smallest power of two (>= 2) that keeps sigma/d <= 4; the low-resolution
// sigma subtracts the variance the box downsample (1/12) and the bilinear
// upsample (1/6) already add, so the effective blur is close to sigmaPx.
inline BackdropKernel backdropKernel(double sigmaPx) {
  BackdropKernel kernel;
  sigmaPx = std::max(0.0, sigmaPx);
  int d = kBackdropMinDownsample;
  while (sigmaPx / d > kBackdropMaxLowSigma && d < kBackdropMaxDownsample) d *= 2;
  kernel.downsample = d;
  const double low = std::min(sigmaPx / d, kBackdropMaxLowSigma);
  const double sigma = std::sqrt(std::max(0.25, low * low - 0.25));
  kernel.radius = std::clamp(int(std::ceil(3.0 * sigma)), 1, kBackdropMaxRadius);
  double sum = 0;
  double raw[kBackdropMaxRadius + 1]{};
  for (int i = 0; i <= kBackdropMaxRadius; ++i) {
    raw[i] = i <= kernel.radius ? std::exp(-double(i) * i / (2.0 * sigma * sigma)) : 0.0;
    sum += i == 0 ? raw[i] : 2.0 * raw[i];
  }
  for (int i = 0; i <= kBackdropMaxRadius; ++i) kernel.weights[i] = float(raw[i] / sum);
  return kernel;
}

// Integer pixel rectangle, half-open [x0, x1) x [y0, y1).
struct BackdropPixelRect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
  int width() const { return x1 - x0; }
  int height() const { return y1 - y0; }
};

// The full-resolution region read for one plate. x0/y0 are multiples of the
// downsample factor (a grid fixed to the frame, never to the plate), so the
// low-resolution samples of a still picture do not change as a plate moves.
struct BackdropRegion {
  int x0 = 0, y0 = 0;        // target px, multiples of d
  int lowWidth = 0, lowHeight = 0;
  int copyWidth = 0, copyHeight = 0;  // region clamped to the target (for a copy)
};

inline int backdropFloorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

inline BackdropRegion backdropRegion(const BackdropPixelRect& bounds, const BackdropKernel& kernel,
                                     int targetWidth, int targetHeight) {
  const int d = kernel.downsample;
  // Kernel support plus one low-resolution texel for the bilinear upsample.
  const int pad = (kernel.radius + 2) * d;
  const int maxX = ((targetWidth + d - 1) / d) * d, maxY = ((targetHeight + d - 1) / d) * d;
  BackdropRegion region;
  region.x0 = std::max(0, backdropFloorDiv(bounds.x0 - pad, d) * d);
  region.y0 = std::max(0, backdropFloorDiv(bounds.y0 - pad, d) * d);
  const int x1 = std::min(maxX, (backdropFloorDiv(bounds.x1 + pad + d - 1, d)) * d);
  const int y1 = std::min(maxY, (backdropFloorDiv(bounds.y1 + pad + d - 1, d)) * d);
  region.lowWidth = std::max(0, (x1 - region.x0) / d);
  region.lowHeight = std::max(0, (y1 - region.y0) / d);
  region.copyWidth = std::max(0, std::min(x1, targetWidth) - region.x0);
  region.copyHeight = std::max(0, std::min(y1, targetHeight) - region.y0);
  return region;
}

// One plate's work for one frame, resolved from the prepared plate list.
struct BackdropDraw {
  float plate[4]{};     // x0, y0, x1, y1 in target px (fractional)
  float radiusPx = 0;   // rounded-corner radius in target px
  float opacity = 1;    // layer x key x motion x node opacity
  float tint[4]{};      // straight RGBA, 0..1
  BackdropPixelRect bounds;  // composite scissor: plate ∩ clip ∩ target
  BackdropKernel kernel;
  BackdropRegion region;
};

inline void backdropTintRgba(uint32_t argb, float out[4]) {
  out[0] = float((argb >> 16) & 255) / 255.f;
  out[1] = float((argb >> 8) & 255) / 255.f;
  out[2] = float(argb & 255) / 255.f;
  out[3] = float((argb >> 24) & 255) / 255.f;
}

// Resolves `plate` for this frame. `animated` is the graphic's on-screen rect
// (normalized, after the key transform), `clip` the layer's normalized clip,
// `alpha` the layer x key opacity: the same inputs the overlay draw uses, so
// the glass moves, fades and clips exactly with the plate's raster. Returns
// false when nothing would be drawn (fully clipped, transparent, off screen).
inline bool resolveBackdropDraw(const graphics::GraphicsBackdropPlate& plate,
                                const graphics::GraphicsBackdropSet& set,
                                const graphics::GraphicsMotionState& state,
                                const LayerRect& animated, const LayerRect& clip, float alpha,
                                int targetWidth, int targetHeight, BackdropDraw& out) {
  if (targetWidth <= 0 || targetHeight <= 0 || set.width <= 0 || set.height <= 0) return false;
  graphics::GraphicsMotionSample sample;
  if (set.motion) sample = graphics::sampleRun(*set.motion, plate.unit, state);
  const double opacity = double(alpha) * sample.opacity * plate.opacity;
  if (!(opacity > 0.0)) return false;
  // Template units -> target pixels.
  const double ox = double(animated.x) * targetWidth, oy = double(animated.y) * targetHeight;
  const double kx = double(animated.width) * targetWidth / set.width;
  const double ky = double(animated.height) * targetHeight / set.height;
  if (!(kx > 0.0) || !(ky > 0.0)) return false;
  const auto box = sample.transform.apply(plate.box);
  auto area = sample.transform.apply(plate.clip);
  if (sample.clip) area = graphics::intersect(area, *sample.clip);
  const double px0 = ox + box.x * kx, py0 = oy + box.y * ky;
  const double px1 = px0 + box.width * kx, py1 = py0 + box.height * ky;
  double cx0 = std::max({px0, ox + area.x * kx, double(clip.x) * targetWidth, 0.0});
  double cy0 = std::max({py0, oy + area.y * ky, double(clip.y) * targetHeight, 0.0});
  double cx1 = std::min({px1, ox + (area.x + area.width) * kx, double(clip.x + clip.width) * targetWidth, double(targetWidth)});
  double cy1 = std::min({py1, oy + (area.y + area.height) * ky, double(clip.y + clip.height) * targetHeight, double(targetHeight)});
  if (!(cx1 > cx0) || !(cy1 > cy0)) return false;
  out.bounds = {int(std::floor(cx0)), int(std::floor(cy0)), int(std::ceil(cx1)), int(std::ceil(cy1))};
  out.bounds.x0 = std::clamp(out.bounds.x0, 0, targetWidth); out.bounds.x1 = std::clamp(out.bounds.x1, 0, targetWidth);
  out.bounds.y0 = std::clamp(out.bounds.y0, 0, targetHeight); out.bounds.y1 = std::clamp(out.bounds.y1, 0, targetHeight);
  if (out.bounds.empty()) return false;
  out.plate[0] = float(px0); out.plate[1] = float(py0); out.plate[2] = float(px1); out.plate[3] = float(py1);
  const double scale = ky * std::min(sample.transform.sx, sample.transform.sy);
  out.radiusPx = float(std::clamp(plate.cornerRadius * scale, 0.0, std::min(px1 - px0, py1 - py0) / 2));
  out.opacity = float(std::min(1.0, opacity));
  backdropTintRgba(plate.tint, out.tint);
  // blur is the Gaussian radius in template px (1080p at scale 1); sigma = blur / 2.
  out.kernel = backdropKernel(plate.blur * 0.5 * ky * sample.transform.sy);
  out.region = backdropRegion(out.bounds, out.kernel, targetWidth, targetHeight);
  return out.region.lowWidth > 0 && out.region.lowHeight > 0;
}

// The one constant block every stage of both shaders reads (160 bytes, every
// member 16-byte aligned so HLSL cbuffer and MSL constant layouts agree).
struct BackdropShaderConstants {
  int32_t source[4]{};   // origin.xy (first full-res texel of the region in the
                         // texture being read), max.xy (last valid texel, inclusive)
  int32_t low[4]{};      // lowWidth, lowHeight, downsample, radius
  int32_t step[4]{};     // blur direction (1,0) / (0,1); unused elsewhere
  float weights[16]{};
  float plate[4]{};      // target px
  float tint[4]{};       // straight RGBA
  float place[4]{};      // region x0, y0 in target px; radiusPx; opacity
};
static_assert(sizeof(BackdropShaderConstants) == 160, "HLSL/MSL constant layout");

// sourceOriginX/Y and sourceMaxX/Y describe where the region's first texel
// sits in the texture the downsample reads: Metal reads Program directly
// (origin = region.x0/y0, max = target - 1); D3D11 reads a copy of the region
// (origin = 0, max = copy - 1). Both address the same pixels.
inline BackdropShaderConstants backdropConstants(const BackdropDraw& draw, int sourceOriginX, int sourceOriginY,
                                                 int sourceMaxX, int sourceMaxY, int stepX = 0, int stepY = 0) {
  BackdropShaderConstants c;
  c.source[0] = sourceOriginX; c.source[1] = sourceOriginY; c.source[2] = sourceMaxX; c.source[3] = sourceMaxY;
  c.low[0] = draw.region.lowWidth; c.low[1] = draw.region.lowHeight;
  c.low[2] = draw.kernel.downsample; c.low[3] = draw.kernel.radius;
  c.step[0] = stepX; c.step[1] = stepY;
  std::memcpy(c.weights, draw.kernel.weights, sizeof(c.weights));
  std::memcpy(c.plate, draw.plate, sizeof(c.plate));
  std::memcpy(c.tint, draw.tint, sizeof(c.tint));
  c.place[0] = float(draw.region.x0); c.place[1] = float(draw.region.y0);
  c.place[2] = draw.radiusPx; c.place[3] = draw.opacity;
  return c;
}

// Rounded-rectangle coverage at a pixel centre: the same expression as both
// shaders' backdropCoverage().
inline float backdropCoverage(float x, float y, const float plate[4], float radius) {
  const float cx = (plate[0] + plate[2]) * 0.5f, cy = (plate[1] + plate[3]) * 0.5f;
  const float hx = (plate[2] - plate[0]) * 0.5f, hy = (plate[3] - plate[1]) * 0.5f;
  const float qx = std::fabs(x - cx) - hx + radius, qy = std::fabs(y - cy) - hy + radius;
  const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
  const float sdf = std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - radius;
  return std::clamp(0.5f - sdf, 0.f, 1.f);
}

// IEEE binary16 storage rounding (round to nearest even), for the reference's
// half-float intermediates. Values are colour channels in [0, 1].
inline float backdropRoundHalf(float v) {
  if (v == 0.f || !std::isfinite(v)) return v;
  if (std::fabs(v) < 6.103515625e-05f) return std::nearbyint(v * 16777216.f) / 16777216.f;
  int e = 0;
  const float m = std::frexp(v, &e);
  return std::ldexp(std::nearbyint(m * 2048.f) / 2048.f, e);
}

// CPU reference of the whole pass over a tightly packed BGRA8 image, in place.
// Bounded test/qualification code: allocates, never runs on a render thread.
inline void backdropReference(std::vector<uint8_t>& bgra, int width, int height, const BackdropDraw& draw) {
  const auto& r = draw.region;
  const int d = draw.kernel.downsample, radius = draw.kernel.radius;
  const int lw = r.lowWidth, lh = r.lowHeight;
  if (lw <= 0 || lh <= 0 || draw.bounds.empty()) return;
  std::vector<float> a(size_t(lw) * lh * 3), b(a.size());
  const auto at = [&](int x, int y) { return (size_t(y) * width + x) * 4; };
  for (int j = 0; j < lh; ++j) {
    for (int i = 0; i < lw; ++i) {
      float sum[3]{};
      for (int v = 0; v < d; ++v) {
        for (int u = 0; u < d; ++u) {
          const int x = std::min(r.x0 + i * d + u, width - 1), y = std::min(r.y0 + j * d + v, height - 1);
          const auto o = at(x, y);
          sum[0] += bgra[o + 2] / 255.f; sum[1] += bgra[o + 1] / 255.f; sum[2] += bgra[o] / 255.f;
        }
      }
      for (int c = 0; c < 3; ++c) a[(size_t(j) * lw + i) * 3 + c] = backdropRoundHalf(sum[c] / float(d * d));
    }
  }
  const auto blur = [&](const std::vector<float>& from, std::vector<float>& to, int sx, int sy) {
    for (int j = 0; j < lh; ++j) {
      for (int i = 0; i < lw; ++i) {
        float sum[3]{};
        for (int k = -radius; k <= radius; ++k) {
          const float w = draw.kernel.weights[k < 0 ? -k : k];
          const int x = std::clamp(i + k * sx, 0, lw - 1), y = std::clamp(j + k * sy, 0, lh - 1);
          for (int c = 0; c < 3; ++c) sum[c] += w * from[(size_t(y) * lw + x) * 3 + c];
        }
        for (int c = 0; c < 3; ++c) to[(size_t(j) * lw + i) * 3 + c] = backdropRoundHalf(sum[c]);
      }
    }
  };
  blur(a, b, 1, 0);
  blur(b, a, 0, 1);
  for (int y = draw.bounds.y0; y < draw.bounds.y1; ++y) {
    for (int x = draw.bounds.x0; x < draw.bounds.x1; ++x) {
      const float px = float(x) + 0.5f, py = float(y) + 0.5f;
      const float coverage = backdropCoverage(px, py, draw.plate, draw.radiusPx);
      const float alpha = coverage * draw.opacity;
      if (alpha <= 0.f) continue;
      const float qx = (px - float(r.x0)) / float(d) - 0.5f, qy = (py - float(r.y0)) / float(d) - 0.5f;
      const float fx0 = std::floor(qx), fy0 = std::floor(qy);
      const float fx = qx - fx0, fy = qy - fy0;
      const int x0 = std::clamp(int(fx0), 0, lw - 1), x1 = std::clamp(int(fx0) + 1, 0, lw - 1);
      const int y0 = std::clamp(int(fy0), 0, lh - 1), y1 = std::clamp(int(fy0) + 1, 0, lh - 1);
      const auto o = at(x, y);
      for (int c = 0; c < 3; ++c) {
        const auto t = [&](int ix, int iy) { return a[(size_t(iy) * lw + ix) * 3 + c]; };
        const float top = t(x0, y0) + (t(x1, y0) - t(x0, y0)) * fx;
        const float bottom = t(x0, y1) + (t(x1, y1) - t(x0, y1)) * fx;
        float value = top + (bottom - top) * fy;
        value = value * (1.f - draw.tint[3]) + draw.tint[c] * draw.tint[3];
        const int channel = c == 0 ? 2 : c == 1 ? 1 : 0;
        const float dst = bgra[o + channel] / 255.f;
        const float blended = value * alpha + dst * (1.f - alpha);
        bgra[o + channel] = uint8_t(std::clamp(int(std::lround(blended * 255.f)), 0, 255));
      }
    }
  }
}

}  // namespace corevideo::compositor
