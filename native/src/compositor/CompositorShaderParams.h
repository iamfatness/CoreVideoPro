#pragma once

// Portable GPU-compositor shader parameter structs + pure helpers, extracted
// VERBATIM from CompositorShaders.h (move-only, no behavior change) so the
// Metal adapter can share them: nothing here touches D3D11 or Metal — POD
// structs and free helpers only. The layout of LayerShaderConstants is the
// wire format both shader languages declare (D3D11 cbuffer / MSL constant
// struct); it is 80 bytes with no padding on both, guarded by the
// static_assert below.

#include <cstdint>

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
  // Chroma key. xyz = key colour in linear 0..1 RGB, w = 1 when keying is on
  // (0 disables it in-shader, so an unkeyed layer costs one compare).
  float chromaKeyColor[4];
  // x = similarity, y = smoothness, z = spill suppression, w = unused.
  float chromaKeyParams[4];
};

static_assert(sizeof(LayerShaderConstants) == 112,
              "LayerShaderConstants layout is the GPU wire format shared by the "
              "HLSL cbuffer and the MSL constant struct — do not add padding");

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

}  // namespace corevideo::modules
