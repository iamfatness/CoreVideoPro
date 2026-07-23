#pragma once

// GPU compositor HLSL shader sources + the pure shader/format helpers, extracted
// VERBATIM from the anonymous namespace of D3D11CompositorAdapter.cpp (move-only
// refactor, no behavior change). This is the self-contained shader-string cluster
// named by the prior compositor split (PR #321): the vertex + pixel shader source
// strings, the constant-buffer struct layout, the per-frame YUV->RGB parameter
// descriptor + its pure helpers, and the D3DCompile wrapper. A single character
// change to any shader string breaks rendering, so they are moved byte-for-byte.
//
// Nothing here reads compositor member state (device_/context_/etc.) — those are
// the resource-lifetime cluster, a later split. Everything is a pure string,
// POD struct, or free helper, so the header carries `inline` definitions (one
// definition across TUs; the constants are `inline constexpr`).

// Only meaningful in the real D3D11 dev build; the stub build compiles this to
// nothing (same gate as D3D11CompositorAdapter.cpp / CompositorOverlayRaster.h).
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3dcompiler.h>

#include <cstring>
#include <string>

#include "compositor/ComPtrLite.h"
#include "modules/Interfaces.h"

namespace corevideo::modules {

struct LayerShaderConstants {
  float color[4];
  float exposure;
  float contrast;
  float saturation;
  float temperature;
  // Source-UV transform for framing: the on-screen [0,1] uv is mapped to the
  // sampled source region [uvOffset, uvOffset + uvScale]. Identity = offset 0,
  // scale 1. Used by the textured (framing) shader.
  float uvScale[2];
  float uvOffset[2];
  // Per-frame YUV->RGB conversion parameters consumed by the I420 shader only
  // (the other shaders declare a smaller cbuffer — binding a larger buffer is
  // legal). x = luma scale, y = luma offset (normalized), z = chroma scale,
  // w = unused. Identity (1, 0, 1) keeps the historical full-range behavior.
  float yuvTransform[4];
  // x = rV, y = gU, z = gV, w = bU matrix coefficients (BT.709 or BT.601).
  float yuvCoeffs[4];
};

// Per-frame YUV->RGB shader parameters. Defaults reproduce the historical
// full-range BT.709 constants exactly (Zoom frames), so the GPU output for
// existing sources is unchanged. Limited-range/BT.601 frames (native UVC
// cameras) get studio-swing expansion and the matching matrix in-shader — the
// per-pixel conversion stays on the GPU.
struct YuvShaderParams {
  float yScale = 1.f;
  float yOffset = 0.f;
  float chromaScale = 1.f;
  float rV = 1.57421875f;
  float gU = 0.1875f;
  float gV = 0.46875f;
  float bU = 1.85546875f;
};

inline YuvShaderParams yuvShaderParamsForFrame(const VideoFrame* frame) {
  YuvShaderParams params;
  if (!frame || !frame->hasI420()) {
    return params;
  }
  if (frame->i420Bt601) {
    params.rV = 1.402f;
    params.gU = 0.344136f;
    params.gV = 0.714136f;
    params.bU = 1.772f;
  }
  if (!frame->i420FullRange) {
    params.yScale = 255.f / 219.f;
    params.yOffset = 16.f / 255.f;
    params.chromaScale = 255.f / 224.f;
  }
  return params;
}

inline void applyYuvParams(LayerShaderConstants* constants, const YuvShaderParams& params) {
  constants->yuvTransform[0] = params.yScale;
  constants->yuvTransform[1] = params.yOffset;
  constants->yuvTransform[2] = params.chromaScale;
  constants->yuvTransform[3] = 0.f;
  constants->yuvCoeffs[0] = params.rV;
  constants->yuvCoeffs[1] = params.gU;
  constants->yuvCoeffs[2] = params.gV;
  constants->yuvCoeffs[3] = params.bU;
}

inline constexpr char kCompositorVertexShader[] = R"(
struct VSOut {
  float4 pos : SV_Position;
  float2 uv : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID) {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  VSOut output;
  output.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  output.uv = uv;
  return output;
}
)";

inline constexpr char kCompositorPixelShader[] = R"(
cbuffer LayerConstants : register(b0) {
  float4 color;
  float exposure;
  float contrast;
  float saturation;
  float temperature;
  float2 uvScale;
  float2 uvOffset;
};

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float3 rgb = color.rgb;
  rgb = (rgb - 0.5) * (1.0 + contrast) + 0.5 + exposure;
  float luma = dot(rgb, float3(0.299, 0.587, 0.114));
  rgb = lerp(float3(luma, luma, luma), rgb, 1.0 + saturation);
  rgb.r += temperature * 0.05;
  rgb.b -= temperature * 0.05;
  return float4(saturate(rgb), color.a);
}
)";

// Textured variant: samples a participant's decoded BGRA frame and applies the
// same color grade, falling back implicitly to the solid-color shader when a
// layer has no frame (the renderer binds the appropriate shader per layer).
inline constexpr char kCompositorTexturedPixelShader[] = R"(
cbuffer LayerConstants : register(b0) {
  float4 color;
  float exposure;
  float contrast;
  float saturation;
  float temperature;
  float2 uvScale;
  float2 uvOffset;
};

Texture2D layerTexture : register(t0);
SamplerState layerSampler : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  // Framing: map the on-screen [0,1] uv into the sampled source region. fit
  // (letterbox) shrinks the on-screen content rect (handled by the viewport),
  // fill (cover) and zoom/pan narrow the sampled region via uvScale/uvOffset.
  float2 sourceUv = uvOffset + uv * uvScale;
  float4 sampled = layerTexture.Sample(layerSampler, sourceUv);
  float3 rgb = sampled.rgb;
  rgb = (rgb - 0.5) * (1.0 + contrast) + 0.5 + exposure;
  float luma = dot(rgb, float3(0.299, 0.587, 0.114));
  rgb = lerp(float3(luma, luma, luma), rgb, 1.0 + saturation);
  rgb.r += temperature * 0.05;
  rgb.b -= temperature * 0.05;
  return float4(saturate(rgb), sampled.a * color.a);
}
)";

// Overlay variant: samples the DirectWrite/D2D overlay raster texture, whose
// content is PREMULTIPLIED alpha (D2D's native output). Fading premultiplied
// content means scaling ALL FOUR channels by the layer alpha; it must be drawn
// with the premultiplied blend state (SrcBlend = ONE), not the straight-alpha
// one, or the glyph edges fringe. No color grade: brand styling is baked into
// the raster.
inline constexpr char kCompositorOverlayPixelShader[] = R"(
cbuffer LayerConstants : register(b0) {
  float4 color;
  float exposure;
  float contrast;
  float saturation;
  float temperature;
  float2 uvScale;
  float2 uvOffset;
};

Texture2D layerTexture : register(t0);
SamplerState layerSampler : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float4 sampled = layerTexture.Sample(layerSampler, uvOffset + uv * uvScale);
  return sampled * color.a;
}
)";

// I420 (YUV 4:2:0 planar) variant: samples a participant's Y/U/V planes from
// three single-channel (R8_UNORM) textures, converts to RGB in-shader, then
// applies the same color grade as the textured BGRA path. This moves the Zoom
// I420->BGRA color conversion off the CPU and onto the GPU (the way OBS /
// CasparCG do it). The chroma planes are half resolution; sampling them with the
// same [0,1] UV (and a linear sampler) up-samples them for free.
inline constexpr char kCompositorYuvPixelShader[] = R"(
cbuffer LayerConstants : register(b0) {
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

Texture2D yTexture : register(t0);
Texture2D uTexture : register(t1);
Texture2D vTexture : register(t2);
SamplerState layerSampler : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float2 sourceUv = uvOffset + uv * uvScale;
  float Y = (yTexture.Sample(layerSampler, sourceUv).r - yuvTransform.y) * yuvTransform.x;
  float U = (uTexture.Sample(layerSampler, sourceUv).r - 0.5) * yuvTransform.z;
  float V = (vTexture.Sample(layerSampler, sourceUv).r - 0.5) * yuvTransform.z;
  // Parametrized YCbCr->RGB. The default constants (yuvTransform = 1,0,1;
  // BT.709 coefficients 403/256, 48/256, 120/256, 475/256) reproduce the prior
  // hard-coded full-range BT.709 math bit-for-bit for Zoom frames. Limited
  // range (native UVC cameras) sets yScale/yOffset/chromaScale to expand
  // studio swing, and BT.601 frames swap the coefficient set.
  float3 rgb;
  rgb.r = Y + yuvCoeffs.x * V;
  rgb.g = Y - yuvCoeffs.y * U - yuvCoeffs.z * V;
  rgb.b = Y + yuvCoeffs.w * U;
  rgb = saturate(rgb);
  rgb = (rgb - 0.5) * (1.0 + contrast) + 0.5 + exposure;
  float luma = dot(rgb, float3(0.299, 0.587, 0.114));
  rgb = lerp(float3(luma, luma, luma), rgb, 1.0 + saturation);
  rgb.r += temperature * 0.05;
  rgb.b -= temperature * 0.05;
  return float4(saturate(rgb), color.a);
}
)";

// Virtual-camera GPU BGRA->NV12 convert (tap device2, see vcamTapLoop). Two
// fullscreen-triangle passes turn the 1080p BGRA shared texture into NV12
// planes ON the GPU, so the tap thread reads back ~3MB of finished NV12 (R8
// luma + half-res R8G8 chroma stagings) instead of ~8MB of BGRA that a ~15ms
// scalar CPU loop then converts. The math below is EXACTLY convertBgraToNv12's
// BT.601 studio-swing fixed-point matrix (VirtualCameraFrame.h):
//   Y = (66 R + 129 G + 25 B)/256 + 16
//   U = (-38 R -  74 G + 112 B)/256 + 128
//   V = (112 R -  94 G -  18 B)/256 + 128
// expressed on [0,1] UNORM values (byte offsets /255); the UNORM render-target
// store rounds to nearest, matching the CPU's "+128 then shift" rounding class.
inline constexpr char kVcamNv12YPixelShader[] = R"(
Texture2D srcTex : register(t0);
SamplerState srcSampler : register(s0);

float main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float3 rgb = srcTex.Sample(srcSampler, uv).rgb;
  return dot(rgb, float3(66.0, 129.0, 25.0) / 256.0) + 16.0 / 255.0;
}
)";

// Chroma pass at half resolution (960x540 for the fixed 1080p vcam). Each
// target pixel's interpolated uv lands exactly on the source 2x2 block CENTER
// ((2i+1)/W, (2j+1)/H), so a single linear-filtered sample IS the 2x2 box
// average convertBgraToNv12 computes before its U/V dot products. R8G8 output
// = interleaved U (.x) then V (.y), NV12's UV-plane byte order.
inline constexpr char kVcamNv12UvPixelShader[] = R"(
Texture2D srcTex : register(t0);
SamplerState srcSampler : register(s0);

float2 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  float3 rgb = srcTex.Sample(srcSampler, uv).rgb;
  float u = dot(rgb, float3(-38.0, -74.0, 112.0) / 256.0) + 128.0 / 255.0;
  float v = dot(rgb, float3(112.0, -94.0, -18.0) / 256.0) + 128.0 / 255.0;
  return float2(u, v);
}
)";

inline ComPtrLite<ID3DBlob> compileShader(const char* source, const char* entry, const char* profile, std::string& error) {
  ComPtrLite<ID3DBlob> blob;
  ComPtrLite<ID3DBlob> errors;
  const HRESULT result = D3DCompile(
      source,
      std::strlen(source),
      "CoreVideoD3D11Compositor",
      nullptr,
      nullptr,
      entry,
      profile,
      D3DCOMPILE_ENABLE_STRICTNESS,
      0,
      blob.putBlob(),
      errors.putBlob());
  if (FAILED(result)) {
    if (errors) {
      error.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    } else {
      error = "D3DCompile failed without diagnostics.";
    }
    return {};
  }
  return blob;
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
