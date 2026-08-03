#pragma once

// Metal Shading Language twins of the D3D11 compositor shaders
// (CompositorShaders.h). Ported 1:1 — the color-grade expression order, the
// Rec.601 luma weights, and the parameterized YCbCr->RGB math are kept
// byte-identical in expression so GPU output matches the D3D11 adapter and
// the CPU preview's expectations. LayerConstants mirrors the 80-byte
// LayerShaderConstants wire layout (CompositorShaderParams.h): float4 (0),
// four floats (16..32), two float2 (32..48), two float4 (48..80) — MSL and
// HLSL agree on every offset, no padding.
//
// Compiled at runtime via newLibraryWithSource: (the Metal equivalent of the
// D3DCompile-at-startup approach) — fail-loud at pipeline init, no offline
// metallib step. Gated like the adapter; the stub build compiles this to
// nothing.

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL

namespace corevideo::modules {

inline constexpr char kMetalCompositorShaderSource[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct LayerConstants {
  float4 color;
  float exposure;
  float contrast;
  float saturation;
  float temperature;
  float2 uvScale;
  float2 uvOffset;
  float4 yuvTransform; // x = yScale, y = yOffset, z = chromaScale, w = unused
  float4 yuvCoeffs;    // x = rV, y = gU, z = gV, w = bU
};

struct VSOut {
  float4 pos [[position]];
  float2 uv;
};

// Fullscreen triangle from the vertex id — the same (vid << 1) & 2 trick as
// the HLSL vertex shader; clip-space y is flipped so uv (0,0) is top-left.
vertex VSOut compositorVertex(uint vid [[vertex_id]]) {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  VSOut out;
  out.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  out.uv = uv;
  return out;
}

static inline float3 applyGrade(float3 rgb, constant LayerConstants& c) {
  rgb = (rgb - 0.5) * (1.0 + c.contrast) + 0.5 + c.exposure;
  float luma = dot(rgb, float3(0.299, 0.587, 0.114));
  rgb = mix(float3(luma, luma, luma), rgb, 1.0 + c.saturation);
  rgb.r += c.temperature * 0.05;
  rgb.b -= c.temperature * 0.05;
  return saturate(rgb);
}

fragment float4 compositorSolid(VSOut in [[stage_in]],
                                constant LayerConstants& c [[buffer(0)]]) {
  return float4(applyGrade(c.color.rgb, c), c.color.a);
}

// Textured variant: samples a decoded BGRA frame and applies the same grade.
fragment float4 compositorTextured(VSOut in [[stage_in]],
                                   constant LayerConstants& c [[buffer(0)]],
                                   texture2d<float> layerTexture [[texture(0)]],
                                   sampler layerSampler [[sampler(0)]]) {
  float2 sourceUv = c.uvOffset + in.uv * c.uvScale;
  float4 sampled = layerTexture.sample(layerSampler, sourceUv);
  return float4(applyGrade(sampled.rgb, c), sampled.a * c.color.a);
}

// Overlay variant: the raster texture is PREMULTIPLIED alpha; fading it means
// scaling ALL FOUR channels by the layer alpha, drawn with the premultiplied
// blend pipeline. No color grade: brand styling is baked into the raster.
fragment float4 compositorOverlay(VSOut in [[stage_in]],
                                  constant LayerConstants& c [[buffer(0)]],
                                  texture2d<float> layerTexture [[texture(0)]],
                                  sampler layerSampler [[sampler(0)]]) {
  return layerTexture.sample(layerSampler, c.uvOffset + in.uv * c.uvScale) * c.color.a;
}

// I420 variant: three single-channel planes, parameterized YCbCr->RGB (BT.709
// or BT.601, full or studio swing) then the shared grade — the same math as
// kCompositorYuvPixelShader.
fragment float4 compositorI420(VSOut in [[stage_in]],
                               constant LayerConstants& c [[buffer(0)]],
                               texture2d<float> yTexture [[texture(0)]],
                               texture2d<float> uTexture [[texture(1)]],
                               texture2d<float> vTexture [[texture(2)]],
                               sampler layerSampler [[sampler(0)]]) {
  float2 sourceUv = c.uvOffset + in.uv * c.uvScale;
  float Y = (yTexture.sample(layerSampler, sourceUv).r - c.yuvTransform.y) * c.yuvTransform.x;
  float U = (uTexture.sample(layerSampler, sourceUv).r - 0.5) * c.yuvTransform.z;
  float V = (vTexture.sample(layerSampler, sourceUv).r - 0.5) * c.yuvTransform.z;
  float3 rgb;
  rgb.r = Y + c.yuvCoeffs.x * V;
  rgb.g = Y - c.yuvCoeffs.y * U - c.yuvCoeffs.z * V;
  rgb.b = Y + c.yuvCoeffs.w * U;
  rgb = saturate(rgb);
  return float4(applyGrade(rgb, c), c.color.a);
}
)MSL";

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL
