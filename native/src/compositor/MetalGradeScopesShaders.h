#pragma once
// Metal Shading Language port of the three D3D grade-scope programs (D3DGradeScopes.h:9-72),
// 1:1 in bin layout (328,708 uints), analysis size (256x144), output (1536x512), modes and view
// semantics, so a Mac scope image and its published sampleWidth/sampleHeight/sampleCount/roiPixel*
// fields mean what they mean on Windows. HLSL -> MSL: InterlockedAdd/Max -> atomic_fetch_add/max
// on device atomic_uint (MSL 2.0+, every Apple-silicon GPU; spec R5 needs no fallback),
// frac -> fract, Load -> read. The fullscreen-triangle vertex is the compositor's (uv (0,0) is
// top-left), so `y = 1 - uv.y` reads exactly as in the HLSL pixel shader.
// Compiled at runtime in its own library (like the vcam kernel), so a failure here can only take
// the scopes down, never the preview.

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL
namespace corevideo::modules {
inline constexpr char kMetalGradeScopeSource[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

// kGradeScopeCompute (D3DGradeScopes.h:9-26). modes = (histogram, waveform, view, circle).
kernel void gradeScopeBins(texture2d<float, access::read> source [[texture(0)]],
                           device atomic_uint* bins [[buffer(0)]],
                           constant float4& modes [[buffer(1)]],
                           uint2 p [[thread_position_in_grid]]) {
  if (p.x >= 256 || p.y >= 144) return;
  float2 region = (float2(p) + .5) / float2(256, 144) * 2 - 1;
  if (modes.w > .5 && dot(region, region) > 1) return;
  float3 rgb = saturate(source.read(p).rgb);
  float y = dot(rgb, float3(.2126, .7152, .0722));
  uint4 h = uint4(float4(rgb, y) * 255);
  atomic_fetch_add_explicit(&bins[h.r], 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(&bins[256 + h.g], 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(&bins[512 + h.b], 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(&bins[768 + h.a], 1u, memory_order_relaxed);
  uint x = p.x;
  atomic_fetch_add_explicit(&bins[1024 + x + 256 * h.r], 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(&bins[66560 + x + 256 * h.g], 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(&bins[132096 + x + 256 * h.b], 1u, memory_order_relaxed);
  atomic_fetch_add_explicit(&bins[197632 + x + 256 * h.a], 1u, memory_order_relaxed);
  float2 c = saturate(float2((rgb.b - y) / 1.8556, (rgb.r - y) / 1.5748) + .5);
  uint2 bin = uint2(c * 255);
  atomic_fetch_add_explicit(&bins[263168 + bin.x + 256 * bin.y], 1u, memory_order_relaxed);
}

// kGradeScopePeak (D3DGradeScopes.h:27-32): one thread per histogram bin, four channel maxima.
kernel void gradeScopePeak(device atomic_uint* bins [[buffer(0)]], uint p [[thread_position_in_grid]]) {
  if (p >= 256) return;
  for (uint channel = 0; channel < 4; ++channel) {
    uint value = atomic_load_explicit(&bins[channel * 256 + p], memory_order_relaxed);
    atomic_fetch_max_explicit(&bins[328704 + channel], value, memory_order_relaxed);
  }
}

struct ScopeVSOut { float4 pos [[position]]; float2 uv; };
vertex ScopeVSOut gradeScopeVertex(uint vid [[vertex_id]]) {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  ScopeVSOut out; out.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); out.uv = uv;
  return out;
}

static inline float b2f(bool v) { return v ? 1.0 : 0.0; }

// kGradeScopePixel (D3DGradeScopes.h:33-72): histogram | waveform | vectorscope.
fragment float4 gradeScopeImage(ScopeVSOut in [[stage_in]], device const uint* bins [[buffer(0)]],
                                constant float4& modes [[buffer(1)]]) {
  float2 uv = in.uv;
  uint panel = min(uint(uv.x * 3), 2u); float x = fract(uv.x * 3), y = 1 - uv.y;
  if (modes.z > .5) { panel = uint(modes.z) - 1; x = uv.x; if (panel == 2) { x = (uv.x - .5) * 3 + .5; if (x < 0 || x > 1) return float4(.012, .012, .012, 1); } }
  float3 rgb = float3(.012, .012, .012);
  if (panel == 0) {
    uint bin = min(uint(x * 256), 255u);
    float4 peak = max(float4(1), float4(bins[328704], bins[328705], bins[328706], bins[328707]));
    peak.rgb = max(peak.r, max(peak.g, peak.b));
    float4 h = float4(bins[bin], bins[256 + bin], bins[512 + bin], bins[768 + bin]) / peak;
    if (modes.x < .5) rgb += y < h.a ? .8 : 0;
    else rgb += float3(b2f(y < h.r), b2f(y < h.g), b2f(y < h.b)) * .75;
  } else if (panel == 1) {
    uint bx = min(uint(x * 256), 255u), by = min(uint(y * 256), 255u);
    if (modes.y < .5) rgb += saturate(log2(1 + float(bins[197632 + bx + 256 * by])) / 5);
    else if (modes.y < 1.5) rgb += saturate(log2(1 + float3(bins[1024 + bx + 256 * by], bins[66560 + bx + 256 * by], bins[132096 + bx + 256 * by])) / 5);
    else {
      uint c = min(uint(x * 3), 2u); bx = min(uint(fract(x * 3) * 256), 255u);
      float v = saturate(log2(1 + float(bins[1024 + c * 65536 + bx + 256 * by])) / 5);
      rgb += float3(b2f(c == 0), b2f(c == 1), b2f(c == 2)) * v;
    }
  } else {
    uint bx = min(uint(x * 256), 255u), by = min(uint(y * 256), 255u);
    rgb += float3(.35, 1, .65) * saturate(log2(1 + float(bins[263168 + bx + 256 * by])) / 6);
    float2 c = float2(x - .5, y - .5); float r = length(c);
    if (abs(r - .375) < .003 || abs(r - .25) < .002 || abs(c.x) < .002 || abs(c.y) < .002) rgb = max(rgb, float3(.12));
    if (c.x < 0 && abs(c.y + c.x * 1.54) < .003) rgb = max(rgb, float3(.4, .25, .17));
    for (uint t = 0; t < 6; ++t) {
      float3 patch = t == 0 ? float3(.75, 0, 0) : t == 1 ? float3(.75, .75, 0) : t == 2 ? float3(0, .75, 0) : t == 3 ? float3(0, .75, .75) : t == 4 ? float3(0, 0, .75) : float3(.75, 0, .75);
      float l = dot(patch, float3(.2126, .7152, .0722)); float2 target = float2((patch.b - l) / 1.8556, (patch.r - l) / 1.5748);
      float2 d = abs(c - target); if (max(d.x, d.y) < .014 && max(d.x, d.y) > .009) rgb = max(rgb, patch + .15);
    }
  }
  if (panel < 2 && (fract(x * 4) < .005 || fract(y * 4) < .005)) rgb = max(rgb, float3(.09));
  return float4(saturate(rgb), 1);
}
)MSL";
}  // namespace corevideo::modules
#endif
