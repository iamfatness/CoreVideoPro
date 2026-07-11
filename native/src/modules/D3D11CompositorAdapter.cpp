#include "modules/Interfaces.h"

// The GPU compositor is intentionally a dev-machine adapter. COREVIDEO_STUB
// builds must stay portable and resolve this factory to nullptr unless all
// three gates are explicit: non-stub, dev adapters, and D3D11.
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d2d1.h>
#include <d3d11.h>
#include <d3d11_4.h>  // ID3D11Multithread (thread-safe immediate context)
#include <d3dcompiler.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wincodec.h>

#include "compositor/CompositorLayout.h"
#include "modules/ImageResize.h"          // resizeBgraBilinear (vcam downscale on the tap thread)
#include "modules/OverlayTileRaster.h"
#include "modules/ProgramFramePreview.h"
#include "modules/VirtualCameraFrame.h"  // convertBgraToNv12 (on the tap thread)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <sstream>
#include <utility>

namespace corevideo::modules {
namespace {

template <typename T>
class ComPtrLite {
 public:
  ComPtrLite() = default;
  ~ComPtrLite() { reset(); }
  ComPtrLite(const ComPtrLite&) = delete;
  ComPtrLite& operator=(const ComPtrLite&) = delete;
  ComPtrLite(ComPtrLite&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  ComPtrLite& operator=(ComPtrLite&& other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }

  T** put() {
    reset();
    return &value_;
  }

  ID3DBlob** putBlob() { return reinterpret_cast<ID3DBlob**>(put()); }

  T* get() const { return value_; }
  T* operator->() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }

 private:
  void reset() {
    if (value_) {
      value_->Release();
      value_ = nullptr;
    }
  }

  T* value_ = nullptr;
};

// UTF-8 -> UTF-16 for DirectWrite (overlay text arrives as UTF-8 JSON strings,
// including non-ASCII — the core's \uXXXX parser decodes to UTF-8).
std::wstring widenUtf8(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
  return wide;
}

// Brand colors are 0xAARRGGBB (same packing writeLayerConstants consumes).
D2D1_COLOR_F d2dColorFromArgb(uint32_t argb) {
  return D2D1::ColorF(
      static_cast<float>((argb >> 16) & 0xff) / 255.f,
      static_cast<float>((argb >> 8) & 0xff) / 255.f,
      static_cast<float>(argb & 0xff) / 255.f,
      static_cast<float>((argb >> 24) & 0xff) / 255.f);
}

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

YuvShaderParams yuvShaderParamsForFrame(const VideoFrame* frame) {
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

void applyYuvParams(LayerShaderConstants* constants, const YuvShaderParams& params) {
  constants->yuvTransform[0] = params.yScale;
  constants->yuvTransform[1] = params.yOffset;
  constants->yuvTransform[2] = params.chromaScale;
  constants->yuvTransform[3] = 0.f;
  constants->yuvCoeffs[0] = params.rV;
  constants->yuvCoeffs[1] = params.gU;
  constants->yuvCoeffs[2] = params.gV;
  constants->yuvCoeffs[3] = params.bU;
}

constexpr char kCompositorVertexShader[] = R"(
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

constexpr char kCompositorPixelShader[] = R"(
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
constexpr char kCompositorTexturedPixelShader[] = R"(
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
constexpr char kCompositorOverlayPixelShader[] = R"(
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
constexpr char kCompositorYuvPixelShader[] = R"(
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

ComPtrLite<ID3DBlob> compileShader(const char* source, const char* entry, const char* profile, std::string& error) {
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

// A frame is drawable when it carries either decoded BGRA pixels or raw I420
// planes (the GPU converts the latter in-shader).
bool frameHasContent(const VideoFrame& frame) {
  return frame.hasPixels() || frame.hasI420();
}

// True when a color grade is a no-op (every axis neutral). The per-participant
// export path uses this to keep its fast copy for the common ungraded source and
// only pay the extra shader pass when a grade actually needs to be baked in.
bool gradeIsIdentity(const CompositorColorGrade& grade) {
  return grade.exposure == 0.f && grade.contrast == 0.f &&
         grade.saturation == 0.f && grade.temperature == 0.f;
}

uint32_t rgbaToSignature(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

std::string handleToHex(HANDLE handle) {
  if (!handle) {
    return {};
  }
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase << reinterpret_cast<uintptr_t>(handle);
  return stream.str();
}

class D3D11Compositor final : public ICompositor {
 public:
  D3D11Compositor(ComPtrLite<ID3D11Device> device, ComPtrLite<ID3D11DeviceContext> context)
      : device_(std::move(device)), context_(std::move(context)) {
    initializePipeline();
  }

  ~D3D11Compositor() override { stopVcamTap(); }

  std::string rendererName() const override { return "d3d11"; }

  ProgramFrame render(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) override {
    ++frameNumber_;
    const auto deterministicPlan = sortCompositorRenderPlan(renderPlan);
    ProgramFrame frame;
    frame.width = deterministicPlan.width;
    frame.height = deterministicPlan.height;
    frame.layerCount = deterministicPlan.layers.empty() ? static_cast<int>(frames.size()) : static_cast<int>(deterministicPlan.layers.size());
    frame.frameNumber = frameNumber_;
    frame.renderPlanId = deterministicPlan.renderPlanId;
    frame.renderer = "d3d11";
    frame.health = deterministicPlan.warnings.empty() ? "live" : "degraded";
    frame.warnings = deterministicPlan.warnings;
    frame.renderPlanSignature = renderPlanSignature(deterministicPlan);

    if (!pipelineReady_ || !device_ || !context_) {
      return frame;
    }

    if (!ensureRenderTarget(deterministicPlan.width, deterministicPlan.height)) {
      frame.health = "degraded";
      return frame;
    }

    const float background[4] = {0.047f, 0.067f, 0.094f, 1.f};
    ID3D11RenderTargetView* renderTargets[] = {renderTargetView_.get()};
    context_->OMSetRenderTargets(1, renderTargets, nullptr);
    context_->ClearRenderTargetView(renderTargetView_.get(), background);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.get(), nullptr, 0);
    context_->OMSetBlendState(blendState_.get(), nullptr, 0xffffffffu);
    context_->RSSetState(rasterizerState_.get());

    const auto layers = resolveLayers(deterministicPlan, frames);
    for (const auto& layer : layers) {
      drawLayer(layer, deterministicPlan);
    }

    frame.gpuComposed = true;
    // The pixel-signature and base64 preview both do a blocking GPU->CPU Map
    // readback. On the light ~60fps display tick we skip them — only the GPU shared
    // texture is needed for the on-screen program, and stalling the CPU on a Map
    // every frame caps the render rate far below what the GPU can do.
    if (!renderPlan.skipCpuReadback) {
      frame.programPixelSignature = readProgramPixelSignature(deterministicPlan.width / 2, deterministicPlan.height / 2);
      frame.preview = readProgramFramePreview();
    }
    if (renderPlan.fullProgramReadback) {
      exportVcamSharedTexture();  // fast GPU->GPU copy only; a dedicated device+thread reads it back
    } else if (vcamThread_.joinable()) {
      stopVcamTap();  // vcam disabled -> tear down the tap thread + second device so it
                      // stops spinning keyed-mutex waits that hitch the render (runs once)
    }
    exportSharedTexture(frame);
    exportParticipantTextures(deterministicPlan, frames, frame);
    // Evict cached source textures no pass has sampled recently (participant
    // left / source unrouted). 300 program frames ≈ 5s at 60fps — long enough
    // that a source cycling through preview keeps its cache, short enough that
    // leavers free their GPU memory promptly. (Multiview/preview passes stamp
    // lastUsedFrame too; they run right after this render each tick.)
    constexpr int64_t kSourceTexEvictAfterFrames = 300;
    for (auto it = sourceTextures_.begin(); it != sourceTextures_.end();) {
      it = (frameNumber_ - it->second.lastUsedFrame > kSourceTexEvictAfterFrames)
               ? sourceTextures_.erase(it)
               : std::next(it);
    }
    context_->Flush();
    return frame;
  }

  // Second compositor pass: render the whole multiview grid into its own
  // keyed-mutex DXGI shared texture, reusing the exact program pipeline +
  // resolveLayers/drawLayer path (so participant + capture tiles work for free).
  // Runs after render() each tick; saves/restores the shared targetWidth_/Height_
  // members the draw helpers read (risk #1 — corruption, not a crash) so the
  // program pass is never affected.
  ProgramFrameSharedTexture renderMultiview(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) override {
    ProgramFrameSharedTexture out;
    if (!pipelineReady_ || !device_ || !context_) {
      return out;
    }
    const auto deterministicPlan = sortCompositorRenderPlan(renderPlan);
    const int width = deterministicPlan.width;
    const int height = deterministicPlan.height;
    if (width <= 0 || height <= 0) {
      return out;
    }
    if (!ensureMultiviewRenderTarget(width, height)) {
      return out;
    }

    // Save the program pass's target dims and point the draw helpers at the
    // multiview canvas. Restored before returning so the next program render()
    // and its exportSharedTexture() see the original dimensions.
    const int savedWidth = targetWidth_;
    const int savedHeight = targetHeight_;
    targetWidth_ = width;
    targetHeight_ = height;

    const float background[4] = {0.047f, 0.067f, 0.094f, 1.f};
    ID3D11RenderTargetView* renderTargets[] = {multiviewRenderTargetView_.get()};
    context_->OMSetRenderTargets(1, renderTargets, nullptr);
    context_->ClearRenderTargetView(multiviewRenderTargetView_.get(), background);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.get(), nullptr, 0);
    context_->OMSetBlendState(blendState_.get(), nullptr, 0xffffffffu);
    context_->RSSetState(rasterizerState_.get());

    const auto layers = resolveLayers(deterministicPlan, frames);
    for (const auto& layer : layers) {
      drawLayer(layer, deterministicPlan);
    }

    exportMultiviewSharedTexture(out);

    targetWidth_ = savedWidth;
    targetHeight_ = savedHeight;
    context_->Flush();
    return out;
  }

  // Third compositor pass: render the whole PREVIEW scene into its own keyed-mutex
  // DXGI shared texture, reusing the exact program pipeline + resolveLayers/drawLayer
  // path (identical to renderMultiview, just a different target). Runs after render()
  // each tick when a preview scene is set; saves/restores targetWidth_/Height_ so the
  // program pass is never affected.
  ProgramFrameSharedTexture renderPreview(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) override {
    ProgramFrameSharedTexture out;
    if (!pipelineReady_ || !device_ || !context_) {
      return out;
    }
    const auto deterministicPlan = sortCompositorRenderPlan(renderPlan);
    const int width = deterministicPlan.width;
    const int height = deterministicPlan.height;
    if (width <= 0 || height <= 0) {
      return out;
    }
    if (!ensurePreviewRenderTarget(width, height)) {
      return out;
    }

    const int savedWidth = targetWidth_;
    const int savedHeight = targetHeight_;
    targetWidth_ = width;
    targetHeight_ = height;

    const float background[4] = {0.047f, 0.067f, 0.094f, 1.f};
    ID3D11RenderTargetView* renderTargets[] = {previewRenderTargetView_.get()};
    context_->OMSetRenderTargets(1, renderTargets, nullptr);
    context_->ClearRenderTargetView(previewRenderTargetView_.get(), background);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.get(), nullptr, 0);
    context_->OMSetBlendState(blendState_.get(), nullptr, 0xffffffffu);
    context_->RSSetState(rasterizerState_.get());

    const auto layers = resolveLayers(deterministicPlan, frames);
    for (const auto& layer : layers) {
      drawLayer(layer, deterministicPlan);
    }

    exportPreviewSharedTexture(out);

    targetWidth_ = savedWidth;
    targetHeight_ = savedHeight;
    context_->Flush();
    return out;
  }

  [[nodiscard]] CompositorSourceTexStats sourceTexStats() const override { return sourceTexStats_; }

 private:
  struct ResolvedLayer {
    CompositorRenderPlanLayer plan;
    uint32_t color = 0xff808080;
    const VideoFrame* frame = nullptr;
  };

  // Per-participant keyed-mutex shared texture for the GPU multiview tiles.
  // Declared up here so member-function signatures (renderI420ToParticipantTexture)
  // can take it by reference.
  struct ParticipantTex {
    ComPtrLite<ID3D11Texture2D> texture;
    ComPtrLite<IDXGIKeyedMutex> mutex;
    // Render-target view used by the I420->BGRA GPU convert path.
    ComPtrLite<ID3D11RenderTargetView> rtv;
    HANDLE handle = nullptr;
    int width = 0;
    int height = 0;
    int64_t lastFrameId = -1;  // skip re-uploading an unchanged (held) frame
    // Grade last baked into this export; re-upload if it changes even when the
    // frame is held (color-grade slider drag on a static source).
    CompositorColorGrade lastGrade;
  };

  static bool gradesEqual(const CompositorColorGrade& a, const CompositorColorGrade& b) {
    return a.exposure == b.exposure && a.contrast == b.contrast &&
           a.saturation == b.saturation && a.temperature == b.temperature;
  }

  // Per-source GPU texture cache entry. Every source with a stable identity
  // (participantId — Zoom participants AND capture sources carry one) gets its
  // OWN Y/U/V or BGRA texture set, uploaded only when the frame content changes
  // (frameId + buffer identity, the same held-frame dedup the participant export
  // uses). All composite passes (program, multiview, preview) and the export
  // converts sample the cached textures, so a source shown in several places
  // costs ONE upload per new frame instead of one ~3MB Map+memcpy per draw —
  // previously the dominant render-tick cost, and the shared scratch textures
  // additionally thrashed create/destroy whenever mixed-resolution sources
  // alternated draws.
  struct SourceTex {
    ComPtrLite<ID3D11Texture2D> y;
    ComPtrLite<ID3D11ShaderResourceView> ySrv;
    ComPtrLite<ID3D11Texture2D> u;
    ComPtrLite<ID3D11ShaderResourceView> uSrv;
    ComPtrLite<ID3D11Texture2D> v;
    ComPtrLite<ID3D11ShaderResourceView> vSrv;
    ComPtrLite<ID3D11Texture2D> bgra;
    ComPtrLite<ID3D11ShaderResourceView> bgraSrv;
    int width = 0;
    int height = 0;
    bool isI420 = false;
    int64_t lastFrameId = -1;
    // Identity backstop for frameId==0 sources: the zero-copy buffers are
    // immutable, so a pointer change means new content. Never dereferenced.
    const void* lastBuffer = nullptr;
    int64_t lastUsedFrame = 0;  // program frameNumber_ stamp for eviction
  };

  void initializePipeline() {
    std::string error;
    const auto vertexBlob = compileShader(kCompositorVertexShader, "main", "vs_5_0", error);
    if (!vertexBlob) {
      initError_ = "vertex shader: " + error;
      return;
    }
    const auto pixelBlob = compileShader(kCompositorPixelShader, "main", "ps_5_0", error);
    if (!pixelBlob) {
      initError_ = "pixel shader: " + error;
      return;
    }

    if (FAILED(device_->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, vertexShader_.put()))) {
      initError_ = "CreateVertexShader failed.";
      return;
    }
    if (FAILED(device_->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, pixelShader_.put()))) {
      initError_ = "CreatePixelShader failed.";
      return;
    }
    const auto texturedPixelBlob = compileShader(kCompositorTexturedPixelShader, "main", "ps_5_0", error);
    if (!texturedPixelBlob) {
      initError_ = "textured pixel shader: " + error;
      return;
    }
    if (FAILED(device_->CreatePixelShader(texturedPixelBlob->GetBufferPointer(), texturedPixelBlob->GetBufferSize(), nullptr, texturedPixelShader_.put()))) {
      initError_ = "CreatePixelShader (textured) failed.";
      return;
    }
    const auto yuvPixelBlob = compileShader(kCompositorYuvPixelShader, "main", "ps_5_0", error);
    if (!yuvPixelBlob) {
      initError_ = "yuv pixel shader: " + error;
      return;
    }
    if (FAILED(device_->CreatePixelShader(yuvPixelBlob->GetBufferPointer(), yuvPixelBlob->GetBufferSize(), nullptr, yuvPixelShader_.put()))) {
      initError_ = "CreatePixelShader (yuv) failed.";
      return;
    }
    const auto overlayPixelBlob = compileShader(kCompositorOverlayPixelShader, "main", "ps_5_0", error);
    if (!overlayPixelBlob) {
      initError_ = "overlay pixel shader: " + error;
      return;
    }
    if (FAILED(device_->CreatePixelShader(overlayPixelBlob->GetBufferPointer(), overlayPixelBlob->GetBufferSize(), nullptr, overlayPixelShader_.put()))) {
      initError_ = "CreatePixelShader (overlay) failed.";
      return;
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0.f;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&samplerDesc, samplerState_.put()))) {
      initError_ = "CreateSamplerState failed.";
      return;
    }

    D3D11_BUFFER_DESC constantDesc{};
    constantDesc.ByteWidth = sizeof(LayerShaderConstants);
    constantDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&constantDesc, nullptr, constantBuffer_.put()))) {
      initError_ = "CreateBuffer failed.";
      return;
    }

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&blendDesc, blendState_.put()))) {
      initError_ = "CreateBlendState failed.";
      return;
    }
    // Premultiplied-alpha variant for the D2D overlay raster texture (D2D emits
    // premultiplied BGRA; blending it with SRC_ALPHA would double-multiply and
    // fringe the glyph edges).
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    if (FAILED(device_->CreateBlendState(&blendDesc, premultipliedBlendState_.put()))) {
      initError_ = "CreateBlendState (premultiplied) failed.";
      return;
    }

    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    if (FAILED(device_->CreateRasterizerState(&rasterDesc, rasterizerState_.put()))) {
      initError_ = "CreateRasterizerState failed.";
      return;
    }
    rasterDesc.ScissorEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rasterDesc, scissorRasterizerState_.put()))) {
      initError_ = "CreateRasterizerState (scissor) failed.";
      return;
    }

    pipelineReady_ = true;
  }

  bool ensureRenderTarget(int width, int height) {
    if (width <= 0 || height <= 0) {
      return false;
    }
    if (renderTarget_ && targetWidth_ == width && targetHeight_ == height) {
      return true;
    }

    renderTarget_ = {};
    renderTargetView_ = {};
    stagingTexture_ = {};
    targetWidth_ = width;
    targetHeight_ = height;

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&textureDesc, nullptr, renderTarget_.put()))) {
      return false;
    }
    if (FAILED(device_->CreateRenderTargetView(renderTarget_.get(), nullptr, renderTargetView_.put()))) {
      return false;
    }

    textureDesc.Usage = D3D11_USAGE_STAGING;
    textureDesc.BindFlags = 0;
    textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return SUCCEEDED(device_->CreateTexture2D(&textureDesc, nullptr, stagingTexture_.put()));
  }

  std::vector<ResolvedLayer> resolveLayers(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames) const {
    std::vector<ResolvedLayer> layers;
    if (!renderPlan.layers.empty()) {
      layers.reserve(renderPlan.layers.size());
      int videoIndex = 0;
      const int videoLayerCount = static_cast<int>(std::count_if(
          renderPlan.layers.begin(),
          renderPlan.layers.end(),
          [](const CompositorRenderPlanLayer& layer) { return !compositorLayerIsOverlay(layer); }));
      for (auto planLayer : renderPlan.layers) {
        ResolvedLayer layer;
        layer.plan = std::move(planLayer);
        if (layer.plan.rect.width <= 0.f || layer.plan.rect.height <= 0.f) {
          if (compositorLayerIsOverlay(layer.plan)) {
            const auto layout = compositorLayerIsLowerThird(layer.plan) ? compositor::lowerThirdOverlay()
                                                                        : compositor::topRightOverlay();
            layer.plan.rect = {layout.x, layout.y, layout.width, layout.height};
          } else {
            const auto layout = compositor::gridCell((std::max)(1, videoLayerCount), videoIndex);
            layer.plan.rect = {layout.x, layout.y, layout.width, layout.height};
            ++videoIndex;
          }
        }
        if (compositorLayerIsOverlay(layer.plan)) {
          layer.color = 0xff2a3548;
        } else if (!layer.plan.participantId.empty()) {
          layer.color = compositor::colorFromParticipantId(layer.plan.participantId);
          layer.frame = frameForParticipant(frames, layer.plan.participantId);
          if (layer.frame == nullptr) {
            warnUnmatchedCaptureLayer(layer.plan.participantId, frames);
          }
        } else if (!layer.plan.mediaAssetId.empty()) {
          layer.color = compositor::colorFromParticipantId("media:" + layer.plan.mediaAssetId);
          const std::string frameSourceId = layer.plan.sourceId.empty() ? "media:" + layer.plan.mediaAssetId : layer.plan.sourceId;
          layer.frame = frameForParticipant(frames, frameSourceId);
        } else if (videoIndex > 0 && videoIndex - 1 < static_cast<int>(frames.size())) {
          const auto& fallbackFrame = frames[static_cast<size_t>(videoIndex - 1)];
          layer.color = compositor::colorFromParticipantId(fallbackFrame.participantId);
          if (frameHasContent(fallbackFrame)) {
            layer.frame = &fallbackFrame;
          }
        }
        layers.push_back(std::move(layer));
      }
      return layers;
    }

    layers.reserve(frames.size());
    const int count = static_cast<int>(frames.size());
    for (int index = 0; index < count; ++index) {
      ResolvedLayer layer;
      layer.plan.layerId = "fallback:" + std::to_string(index);
      layer.plan.kind = "participant-video";
      layer.plan.participantId = frames[static_cast<size_t>(index)].participantId;
      layer.plan.sourceId = "zoom:" + layer.plan.participantId;
      layer.plan.order = index;
      const auto layout = compositor::gridCell((std::max)(1, count), index);
      layer.plan.rect = {layout.x, layout.y, layout.width, layout.height};
      layer.color = compositor::colorFromParticipantId(layer.plan.participantId);
      if (frameHasContent(frames[static_cast<size_t>(index)])) {
        layer.frame = &frames[static_cast<size_t>(index)];
      }
      layers.push_back(std::move(layer));
    }
    return layers;
  }

  // Guardrail: a capture layer that resolves to NO frame renders as a solid
  // colorFromParticipantId() placeholder (the "pink tile"). That is almost always a
  // key mismatch (the layer's participantId matches no frame's) or a dead feed, and it
  // used to fail SILENTLY — the native-UVC key mismatch cost a multi-session hunt.
  // Make it loud: log, rate-limited per key, dumping the available capture-frame keys
  // so any mismatch is obvious at a glance. Capture sources only (Zoom participants
  // legitimately come and go, so a missing frame there is not necessarily a bug).
  static void warnUnmatchedCaptureLayer(const std::string& participantId,
                                        const std::vector<VideoFrame>& frames) {
    if (participantId.rfind("capture:", 0) != 0) {
      return;
    }
    static std::mutex logMutex;
    static std::map<std::string, int64_t> lastLogMs;
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    {
      std::lock_guard<std::mutex> lock(logMutex);
      int64_t& last = lastLogMs[participantId];
      if (last != 0 && now - last < 5000) {
        return;
      }
      last = now;
    }
    std::string available;
    for (const auto& frame : frames) {
      if (frame.participantId.rfind("capture:", 0) != 0) {
        continue;
      }
      if (!available.empty()) {
        available += ", ";
      }
      available += frame.participantId;
      if (!frameHasContent(frame)) {
        available += "(empty)";
      }
    }
    std::fprintf(stderr,
                 "[compositor] capture layer '%s' has NO matching frame (pink tile) - "
                 "available capture frames: [%s]\n",
                 participantId.c_str(), available.empty() ? "none" : available.c_str());
  }

  static const VideoFrame* frameForParticipant(const std::vector<VideoFrame>& frames, const std::string& participantId) {
    if (participantId.empty()) {
      return nullptr;
    }
    for (const auto& frame : frames) {
      if (frame.participantId == participantId && frameHasContent(frame)) {
        return &frame;
      }
    }
    return nullptr;
  }

  // Sets a viewport from a normalized rect (clamped to the render target).
  void setViewportFromRect(const compositor::LayerRect& rect) {
    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = rect.x * static_cast<float>(targetWidth_);
    viewport.TopLeftY = rect.y * static_cast<float>(targetHeight_);
    viewport.Width = std::max(0.f, rect.width) * static_cast<float>(targetWidth_);
    viewport.Height = std::max(0.f, rect.height) * static_cast<float>(targetHeight_);
    viewport.MinDepth = 0.f;
    viewport.MaxDepth = 1.f;
    context_->RSSetViewports(1, &viewport);
  }

  void setScissorFromRect(const compositor::LayerRect& rect) {
    D3D11_RECT scissor{};
    scissor.left = std::clamp(
        static_cast<LONG>(std::floor(rect.x * static_cast<float>(targetWidth_))),
        0L,
        static_cast<LONG>(targetWidth_));
    scissor.top = std::clamp(
        static_cast<LONG>(std::floor(rect.y * static_cast<float>(targetHeight_))),
        0L,
        static_cast<LONG>(targetHeight_));
    scissor.right = std::clamp(
        static_cast<LONG>(std::ceil((rect.x + rect.width) * static_cast<float>(targetWidth_))),
        0L,
        static_cast<LONG>(targetWidth_));
    scissor.bottom = std::clamp(
        static_cast<LONG>(std::ceil((rect.y + rect.height) * static_cast<float>(targetHeight_))),
        0L,
        static_cast<LONG>(targetHeight_));
    context_->RSSetScissorRects(1, &scissor);
  }

  // Uploads the layer's color grade + an explicit color + UV transform into the
  // shared constant buffer. Returns false if the buffer could not be mapped.
  bool writeLayerConstants(
      const ResolvedLayer& layer,
      const CompositorRenderPlan& renderPlan,
      uint32_t colorArgb,
      float alpha,
      float uvScaleX,
      float uvScaleY,
      float uvOffsetX,
      float uvOffsetY) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(constantBuffer_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      return false;
    }
    auto* constants = static_cast<LayerShaderConstants*>(mapped.pData);
    constants->color[0] = static_cast<float>((colorArgb >> 16) & 0xff) / 255.f;
    constants->color[1] = static_cast<float>((colorArgb >> 8) & 0xff) / 255.f;
    constants->color[2] = static_cast<float>(colorArgb & 0xff) / 255.f;
    constants->color[3] = alpha;
    const auto grade = layer.plan.hasColorGrade ? layer.plan.colorGrade : renderPlan.colorGrade;
    constants->exposure = grade.exposure * 0.1f;
    constants->contrast = grade.contrast * 0.1f;
    constants->saturation = grade.saturation * 0.1f;
    constants->temperature = grade.temperature * 0.1f;
    constants->uvScale[0] = uvScaleX;
    constants->uvScale[1] = uvScaleY;
    constants->uvOffset[0] = uvOffsetX;
    constants->uvOffset[1] = uvOffsetY;
    applyYuvParams(constants, yuvShaderParamsForFrame(layer.frame));
    context_->Unmap(constantBuffer_.get(), 0);
    ID3D11Buffer* buffers[] = {constantBuffer_.get()};
    context_->PSSetConstantBuffers(0, 1, buffers);
    return true;
  }

  // Draws a solid-color quad filling the current viewport (set the viewport to
  // the target rect before calling). Used for borders, letterbox bars, and
  // overlay backgrounds.
  void drawSolidQuad(
      const ResolvedLayer& layer,
      const CompositorRenderPlan& renderPlan,
      const compositor::LayerRect& rect,
      uint32_t colorArgb,
      float alpha) {
    if (rect.width <= 0.f || rect.height <= 0.f || alpha <= 0.f) {
      return;
    }
    setViewportFromRect(rect);
    if (!writeLayerConstants(layer, renderPlan, colorArgb, alpha, 1.f, 1.f, 0.f, 0.f)) {
      return;
    }
    context_->PSSetShader(pixelShader_.get(), nullptr, 0);
    context_->Draw(3, 0);
  }

  // Strokes a border around `rect` by drawing its four edge quads.
  void drawBorderPass(
      const ResolvedLayer& layer,
      const CompositorRenderPlan& renderPlan,
      const compositor::LayerRect& rect,
      const compositor::BorderFraming& border,
      float alpha) {
    if (!border.visible || alpha <= 0.f) {
      return;
    }
    const float smaller = std::min(rect.width, rect.height);
    float stroke = border.thickness * smaller;
    stroke = std::max(stroke, 1.f / static_cast<float>(std::max(targetWidth_, targetHeight_)));
    const float strokeX = std::min(stroke, rect.width * 0.5f);
    const float strokeY = std::min(stroke, rect.height * 0.5f);
    const uint32_t color = border.colorRgba;
    const float borderAlpha = std::clamp(alpha * (static_cast<float>((color >> 24) & 0xff) / 255.f), 0.f, 1.f);
    drawSolidQuad(layer, renderPlan, {rect.x, rect.y, rect.width, strokeY}, color, borderAlpha);
    drawSolidQuad(layer, renderPlan, {rect.x, rect.y + rect.height - strokeY, rect.width, strokeY}, color, borderAlpha);
    drawSolidQuad(layer, renderPlan, {rect.x, rect.y, strokeX, rect.height}, color, borderAlpha);
    drawSolidQuad(layer, renderPlan, {rect.x + rect.width - strokeX, rect.y, strokeX, rect.height}, color, borderAlpha);
  }

  void drawLayer(const ResolvedLayer& layer, const CompositorRenderPlan& renderPlan) {
    const compositor::LayerRect rect{
        layer.plan.rect.x, layer.plan.rect.y, layer.plan.rect.width, layer.plan.rect.height};
    const float layerAlpha = compositorLayerOpacity(layer.plan);

    // Overlay/lower-third/caption layers go through the raster stage.
    if (layer.plan.hasOverlayContent && compositorLayerIsOverlay(layer.plan)) {
      drawOverlayLayer(layer, renderPlan, rect, layerAlpha);
      return;
    }

    // --- Item 8: per-source framing. ---
    // Compute the framing in canvas-pixel proportions so aspect comparisons are
    // correct (the rect is normalized to the canvas), then resolve the on-screen
    // content rect (letterbox) and the sampled source UV window (cover/zoom/pan).
    int sourceWidth = renderPlan.width > 0 ? renderPlan.width : 16;
    int sourceHeight = renderPlan.height > 0 ? renderPlan.height : 9;
    if (layer.frame != nullptr) {
      sourceWidth = layer.frame->naturalWidth > 0
                        ? layer.frame->naturalWidth
                        : layer.frame->width > 0
                              ? layer.frame->width
                              : layer.frame->pixelWidth > 0 ? layer.frame->pixelWidth : sourceWidth;
      sourceHeight = layer.frame->naturalHeight > 0
                         ? layer.frame->naturalHeight
                         : layer.frame->height > 0
                               ? layer.frame->height
                               : layer.frame->pixelHeight > 0 ? layer.frame->pixelHeight : sourceHeight;
    }
    const compositor::LayerRect aspectRect{
        0.f,
        0.f,
        rect.width * static_cast<float>(targetWidth_),
        rect.height * static_cast<float>(targetHeight_)};
    const auto framing = compositor::computeSourceFraming(
        sourceWidth,
        sourceHeight,
        aspectRect,
        layer.plan.fitMode,
        layer.plan.sourceScale,
        layer.plan.sourceOffsetX,
        layer.plan.sourceOffsetY);

    // Letterbox bars: paint the full layer rect dark first when the content is
    // inset, so fit/contain shows bars (matching the CPU preview).
    if (framing.hasLetterbox) {
      drawSolidQuad(layer, renderPlan, rect, 0xff05080cu, layerAlpha);
    }

    const float imageFracX = aspectRect.width > 0.f ? framing.imageX / aspectRect.width : 0.f;
    const float imageFracY = aspectRect.height > 0.f ? framing.imageY / aspectRect.height : 0.f;
    const float imageFracW = aspectRect.width > 0.f ? framing.imageW / aspectRect.width : 1.f;
    const float imageFracH = aspectRect.height > 0.f ? framing.imageH / aspectRect.height : 1.f;
    const compositor::LayerRect imageRect{
        rect.x + imageFracX * rect.width,
        rect.y + imageFracY * rect.height,
        imageFracW * rect.width,
        imageFracH * rect.height};

    // Main content pass: viewport = full rendered source layer; scissor = slot.
    // This keeps source X/Y relative to the original layer, then clips the
    // result to the source box, matching a layer-based SuperSource model.
    setViewportFromRect(imageRect);
    if (!writeLayerConstants(layer, renderPlan, layer.color, layerAlpha, 1.f, 1.f, 0.f, 0.f)) {
      return;
    }

    // Prefer the GPU I420->RGB path for Zoom participants; fall back to the
    // BGRA textured path for capture/media frames, and to the solid-color shader
    // when the layer has no decoded pixels at all. Sources with a stable id bind
    // their cached per-source textures (uploaded only on content change); frames
    // without one (media layers) take the legacy shared-scratch upload.
    const bool isI420 = layer.frame != nullptr && layer.frame->hasI420();
    SourceTex* sourceTex = layer.frame != nullptr ? acquireSourceTex(*layer.frame) : nullptr;
    const bool textured = sourceTex != nullptr ||
        (layer.frame != nullptr &&
         (isI420 ? uploadLayerI420Texture(*layer.frame)
                 : (layer.frame->hasPixels() && uploadLayerTexture(*layer.frame))));
    if (textured) {
      ID3D11SamplerState* samplers[] = {samplerState_.get()};
      context_->PSSetSamplers(0, 1, samplers);
      if (isI420) {
        context_->PSSetShader(yuvPixelShader_.get(), nullptr, 0);
        ID3D11ShaderResourceView* views[] = {
            sourceTex ? sourceTex->ySrv.get() : yTextureView_.get(),
            sourceTex ? sourceTex->uSrv.get() : uTextureView_.get(),
            sourceTex ? sourceTex->vSrv.get() : vTextureView_.get()};
        context_->PSSetShaderResources(0, 3, views);
      } else {
        context_->PSSetShader(texturedPixelShader_.get(), nullptr, 0);
        ID3D11ShaderResourceView* views[] = {sourceTex ? sourceTex->bgraSrv.get() : layerTextureView_.get()};
        context_->PSSetShaderResources(0, 1, views);
      }
    } else {
      context_->PSSetShader(pixelShader_.get(), nullptr, 0);
    }
    context_->RSSetState(scissorRasterizerState_.get());
    setScissorFromRect(rect);
    context_->Draw(3, 0);
    context_->RSSetState(rasterizerState_.get());
    if (textured) {
      ID3D11ShaderResourceView* nullViews[] = {nullptr, nullptr, nullptr};
      context_->PSSetShaderResources(0, isI420 ? 3 : 1, nullViews);
      context_->PSSetShader(pixelShader_.get(), nullptr, 0);
    }

    // --- Item 8: border pass around the full layer rect. ---
    const auto border = compositor::computeBorderFraming(
        layer.plan.borderStyle, layer.plan.borderColor, layer.plan.borderThickness);
    drawBorderPass(layer, renderPlan, rect, border, layerAlpha);
  }

  // --- Item 9: overlay/lower-third/caption raster stage. ---
  // Rasters the overlay's real content (DirectWrite text + WIC image) into a
  // GPU texture via a D2D DXGI-surface render target, then composites it with
  // the animated keyPhase transform. The texture is cached by content signature
  // (see rasterOverlayTexture) so the raster cost is paid only when the text,
  // brand styling, or target size changes — never per frame. Falls back to the
  // deterministic band + accent draw if D2D/DirectWrite is unavailable.
  void drawOverlayLayer(
      const ResolvedLayer& layer,
      const CompositorRenderPlan& renderPlan,
      const compositor::LayerRect& rect,
      float layerAlpha) {
    const auto& overlay = layer.plan.overlay;
    const auto key = compositor::computeOverlayKeyTransform(
        overlay.keyPhase, overlay.keyProgress, overlay.keyPosition);
    if (!key.visible || key.alpha <= 0.001f) {
      return;
    }
    const float alpha = std::clamp(layerAlpha * key.alpha, 0.f, 1.f);
    if (alpha <= 0.f) {
      return;
    }

    // Animated slide + content scale about the rect center (mirror of CPU math).
    compositor::LayerRect animated = rect;
    animated.x += key.slideX;
    animated.y += key.slideY;
    const float centerX = animated.x + animated.width * 0.5f;
    const float centerY = animated.y + animated.height * 0.5f;
    animated.width *= key.contentScale;
    animated.height *= key.contentScale;
    animated.x = centerX - animated.width * 0.5f;
    animated.y = centerY - animated.height * 0.5f;

    const uint32_t background = compositor::parseHexColorRgba(overlay.brandBackgroundColor, 0xff0c1118u);
    const uint32_t accent = compositor::parseHexColorRgba(overlay.brandColor, 0xff44c1a1u);

    // Brand-styled band background.
    drawSolidQuad(layer, renderPlan, animated, background, alpha);

    // If a DirectWrite/WIC raster is available, composite it over the band; the
    // texture carries the real text/image pixels. Otherwise fall back to the
    // accent bar so the overlay region is still non-uniform/brand-styled.
    // Sizing/caching uses the un-animated rect so a build-in/out animation
    // scales the SAME cached texture (via the viewport) instead of re-rastering
    // every frame of the key transition.
    if (ID3D11ShaderResourceView* overlayView = rasterOverlayTexture(overlay, rect)) {
      setViewportFromRect(animated);
      if (writeLayerConstants(layer, renderPlan, 0xffffffffu, alpha, 1.f, 1.f, 0.f, 0.f)) {
        context_->OMSetBlendState(premultipliedBlendState_.get(), nullptr, 0xffffffffu);
        context_->PSSetShader(overlayPixelShader_.get(), nullptr, 0);
        ID3D11ShaderResourceView* views[] = {overlayView};
        ID3D11SamplerState* samplers[] = {samplerState_.get()};
        context_->PSSetShaderResources(0, 1, views);
        context_->PSSetSamplers(0, 1, samplers);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* nullViews[] = {nullptr};
        context_->PSSetShaderResources(0, 1, nullViews);
        context_->PSSetShader(pixelShader_.get(), nullptr, 0);
        context_->OMSetBlendState(blendState_.get(), nullptr, 0xffffffffu);
      }
    } else {
      // Accent bar: left edge for lower-thirds, top edge for captions.
      if (overlay.isCaption) {
        drawSolidQuad(layer, renderPlan, {animated.x, animated.y, animated.width, animated.height * 0.10f}, accent, alpha);
      } else {
        drawSolidQuad(layer, renderPlan, {animated.x, animated.y, animated.width * 0.04f, animated.height}, accent, alpha);
      }
    }
  }

  // Lazily creates the D2D/DirectWrite (and best-effort WIC) factories used by
  // the overlay raster. A failure latches so we don't retry per frame; the
  // caller then keeps the band+accent fallback.
  bool ensureOverlayRasterFactories() {
    if (d2dFactory_ && dwriteFactory_) {
      return true;
    }
    if (overlayRasterUnavailable_) {
      return false;
    }
    if (!d2dFactory_ &&
        FAILED(D2D1CreateFactory(
            D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory), nullptr,
            reinterpret_cast<void**>(d2dFactory_.put())))) {
      overlayRasterUnavailable_ = true;
      return false;
    }
    if (!dwriteFactory_ &&
        FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwriteFactory_.put())))) {
      overlayRasterUnavailable_ = true;
      return false;
    }
    // WIC is optional (image overlays only). CoInitializeEx may return S_FALSE
    // (already initialized) or RPC_E_CHANGED_MODE (STA thread) — CoCreateInstance
    // still works in both cases, so only the factory failure disables images.
    if (!wicFactory_) {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(wicFactory_.put()));
    }
    return true;
  }

  // Decodes `imageUri` (a local path or file:/// URI) via WIC into a D2D bitmap
  // for the given render target. Returns an empty ComPtrLite on any failure —
  // the raster then simply omits the image.
  ComPtrLite<ID2D1Bitmap> decodeOverlayImage(ID2D1RenderTarget* target, const std::string& imageUri) {
    ComPtrLite<ID2D1Bitmap> bitmap;
    if (!wicFactory_ || !target || imageUri.empty()) {
      return bitmap;
    }
    std::string path = imageUri;
    if (path.rfind("file:///", 0) == 0) {
      path = path.substr(8);
    } else if (path.rfind("file://", 0) == 0) {
      path = path.substr(7);
    }
    const std::wstring widePath = widenUtf8(path);
    if (widePath.empty()) {
      return bitmap;
    }
    ComPtrLite<IWICBitmapDecoder> decoder;
    if (FAILED(wicFactory_->CreateDecoderFromFilename(
            widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()))) {
      return bitmap;
    }
    ComPtrLite<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) {
      return bitmap;
    }
    ComPtrLite<IWICFormatConverter> converter;
    if (FAILED(wicFactory_->CreateFormatConverter(converter.put()))) {
      return bitmap;
    }
    // 32bppPBGRA = premultiplied BGRA, D2D's native interchange format.
    if (FAILED(converter->Initialize(
            frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
            nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
      return bitmap;
    }
    if (FAILED(target->CreateBitmapFromWicBitmap(converter.get(), nullptr, bitmap.put()))) {
      bitmap = {};
    }
    return bitmap;
  }

  // Draws one line of text into `box`, vertically centered, trimmed with an
  // ellipsis when it overflows. Returns false only on factory/format failure.
  bool drawOverlayTextLine(
      ID2D1RenderTarget* target,
      const std::string& text,
      const std::string& fontFamily,
      DWRITE_FONT_WEIGHT weight,
      const D2D1_RECT_F& box,
      const D2D1_COLOR_F& color) {
    if (text.empty() || box.right <= box.left || box.bottom <= box.top) {
      return true;
    }
    const std::wstring wideText = widenUtf8(text);
    if (wideText.empty()) {
      return true;
    }
    const std::wstring wideFamily = widenUtf8(fontFamily.empty() ? "Segoe UI" : fontFamily);
    // Em size ~72% of the line box: leaves room for ascenders/descenders so the
    // layout's vertical centering doesn't clip.
    const float fontSize = (std::max)(4.f, (box.bottom - box.top) * 0.72f);
    ComPtrLite<IDWriteTextFormat> format;
    if (FAILED(dwriteFactory_->CreateTextFormat(
            wideFamily.c_str(), nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, fontSize, L"en-us", format.put()))) {
      return false;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtrLite<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwriteFactory_->CreateEllipsisTrimmingSign(format.get(), ellipsis.put()))) {
      DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
      format->SetTrimming(&trimming, ellipsis.get());
    }
    ComPtrLite<ID2D1SolidColorBrush> brush;
    if (FAILED(target->CreateSolidColorBrush(color, brush.put()))) {
      return false;
    }
    target->DrawText(
        wideText.c_str(), static_cast<UINT32>(wideText.size()), format.get(), box, brush.get());
    return true;
  }

  // Rasters the overlay's accent bar + text (+ optional WIC image) into a GPU
  // texture using DirectWrite/D2D and returns its SRV, or nullptr to fall back
  // to the accent-bar draw. The texture holds PREMULTIPLIED alpha over a
  // transparent background (the brand band is drawn separately as a quad so its
  // alpha animation matches the CPU mirror exactly). Cached by
  // overlayContentSignature: re-rastered only when text/brand/size changes.
  // Geometry comes from computeOverlayTileLayout (OverlayTileRaster.h) — the
  // same resolver the portable CPU tile rasters — so preview and program agree
  // on placement by construction.
  ID3D11ShaderResourceView* rasterOverlayTexture(const CompositorOverlayContent& overlay, const compositor::LayerRect& rect) {
    const bool hasText = !overlay.title.empty() || !overlay.org.empty() ||
                         !overlay.text.empty() || !overlay.speaker.empty();
    if (!hasText && overlay.imageUri.empty()) {
      return nullptr;
    }
    const int widthPx = std::clamp(
        static_cast<int>(std::lround(rect.width * static_cast<float>(targetWidth_))), 0, 4096);
    const int heightPx = std::clamp(
        static_cast<int>(std::lround(rect.height * static_cast<float>(targetHeight_))), 0, 4096);
    if (widthPx < 8 || heightPx < 8) {
      return nullptr;
    }
    if (!ensureOverlayRasterFactories()) {
      return nullptr;
    }

    const uint64_t signature = overlayContentSignature(overlay, widthPx, heightPx);

    ++overlayRasterClock_;
    if (auto existing = overlayTextTextures_.find(signature); existing != overlayTextTextures_.end()) {
      existing->second.lastUsed = overlayRasterClock_;
      return existing->second.view.get();
    }

    OverlayRasterTex entry;
    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(widthPx);
    textureDesc.Height = static_cast<UINT>(heightPx);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&textureDesc, nullptr, entry.texture.put()))) {
      return nullptr;
    }
    ComPtrLite<IDXGISurface> surface;
    if (FAILED(entry.texture->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(surface.put())))) {
      return nullptr;
    }
    const auto targetProperties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f, 96.f);
    ComPtrLite<ID2D1RenderTarget> d2dTarget;
    if (FAILED(d2dFactory_->CreateDxgiSurfaceRenderTarget(surface.get(), &targetProperties, d2dTarget.put()))) {
      return nullptr;
    }

    // D2D's EndDraw flushes through the shared device and can clobber the
    // immediate context's bound state mid-pass. Rasters only happen on content
    // change, so snapshot + restore the pipeline state we depend on.
    ComPtrLite<ID3D11RenderTargetView> savedRtv;
    context_->OMGetRenderTargets(1, savedRtv.put(), nullptr);
    ComPtrLite<ID3D11BlendState> savedBlend;
    FLOAT savedBlendFactor[4] = {0.f, 0.f, 0.f, 0.f};
    UINT savedSampleMask = 0xffffffffu;
    context_->OMGetBlendState(savedBlend.put(), savedBlendFactor, &savedSampleMask);
    ComPtrLite<ID3D11RasterizerState> savedRasterizer;
    context_->RSGetState(savedRasterizer.put());
    D3D11_VIEWPORT savedViewport{};
    UINT viewportCount = 1;
    context_->RSGetViewports(&viewportCount, &savedViewport);
    D3D11_PRIMITIVE_TOPOLOGY savedTopology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    context_->IAGetPrimitiveTopology(&savedTopology);
    ComPtrLite<ID3D11VertexShader> savedVs;
    context_->VSGetShader(savedVs.put(), nullptr, nullptr);
    const auto restorePipelineState = [&]() {
      ID3D11RenderTargetView* rtvs[] = {savedRtv.get()};
      context_->OMSetRenderTargets(1, rtvs, nullptr);
      context_->OMSetBlendState(savedBlend.get(), savedBlendFactor, savedSampleMask);
      context_->RSSetState(savedRasterizer.get());
      if (viewportCount > 0) {
        context_->RSSetViewports(1, &savedViewport);
      }
      context_->IASetPrimitiveTopology(savedTopology);
      context_->VSSetShader(savedVs.get(), nullptr, 0);
    };

    // Geometry + per-line colors come from the shared layout resolver, so this
    // DirectWrite raster and the portable CPU bitmap-font tile
    // (rasterizeOverlayTileBgra) place identical content — preview and program
    // agree by construction. The band background is NOT painted here (it stays
    // a separate quad so its alpha animation matches the CPU mirror).
    const auto layout = computeOverlayTileLayout(overlay, widthPx, heightPx);

    d2dTarget->BeginDraw();
    d2dTarget->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));

    ComPtrLite<ID2D1SolidColorBrush> accentBrush;
    if (SUCCEEDED(d2dTarget->CreateSolidColorBrush(d2dColorFromArgb(layout.accentArgb), accentBrush.put()))) {
      d2dTarget->FillRectangle(
          D2D1::RectF(layout.accentBar.x, layout.accentBar.y,
                      layout.accentBar.x + layout.accentBar.width,
                      layout.accentBar.y + layout.accentBar.height),
          accentBrush.get());
    }

    // Real WIC decode aspect-fitted inside the layout's image slot (the CPU
    // tile draws its deterministic checker in the same slot).
    if (layout.hasImage) {
      if (auto image = decodeOverlayImage(d2dTarget.get(), overlay.imageUri)) {
        const auto imageSize = image->GetSize();
        float drawWidth = layout.imageRect.width;
        float drawHeight = layout.imageRect.height;
        if (imageSize.width > 0.f && imageSize.height > 0.f) {
          const float scale = (std::min)(
              layout.imageRect.width / imageSize.width, layout.imageRect.height / imageSize.height);
          drawWidth = imageSize.width * scale;
          drawHeight = imageSize.height * scale;
        }
        const float imageLeft = layout.imageRect.x + (layout.imageRect.width - drawWidth) * 0.5f;
        const float imageTop = layout.imageRect.y + (layout.imageRect.height - drawHeight) * 0.5f;
        d2dTarget->DrawBitmap(
            image.get(), D2D1::RectF(imageLeft, imageTop, imageLeft + drawWidth, imageTop + drawHeight));
      }
    }

    // First line is the emphasis line (lower-third title, or caption speaker
    // attribution when present) — semibold; the rest render normal weight.
    for (size_t lineIndex = 0; lineIndex < layout.textLines.size(); ++lineIndex) {
      const auto& line = layout.textLines[lineIndex];
      const bool emphasis =
          lineIndex == 0 && (!overlay.isCaption || !overlay.speaker.empty());
      drawOverlayTextLine(
          d2dTarget.get(), line.text, layout.fontFamily,
          emphasis ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
          D2D1::RectF(line.rect.x, line.rect.y,
                      line.rect.x + line.rect.width, line.rect.y + line.rect.height),
          d2dColorFromArgb(line.colorArgb));
    }

    const HRESULT endDrawResult = d2dTarget->EndDraw();
    restorePipelineState();
    if (FAILED(endDrawResult)) {
      return nullptr;
    }
    if (FAILED(device_->CreateShaderResourceView(entry.texture.get(), nullptr, entry.view.put()))) {
      return nullptr;
    }
    entry.lastUsed = overlayRasterClock_;

    // Bounded cache: evict the least-recently-used raster beyond 8 entries
    // (program + preview + multiview lower-third/caption variants all fit).
    while (overlayTextTextures_.size() >= 8) {
      auto oldest = overlayTextTextures_.begin();
      for (auto it = overlayTextTextures_.begin(); it != overlayTextTextures_.end(); ++it) {
        if (it->second.lastUsed < oldest->second.lastUsed) {
          oldest = it;
        }
      }
      overlayTextTextures_.erase(oldest);
    }
    const auto inserted = overlayTextTextures_.emplace(signature, std::move(entry));
    return inserted.first->second.view.get();
  }

  bool uploadLayerTexture(const VideoFrame& frame) {
    if (!frame.hasPixels()) {
      return false;
    }
    const int width = frame.pixelWidth;
    const int height = frame.pixelHeight;
    if (!layerTexture_ || layerTextureWidth_ != width || layerTextureHeight_ != height) {
      layerTexture_ = {};
      layerTextureView_ = {};
      layerTextureWidth_ = width;
      layerTextureHeight_ = height;
      if (!createBgraDynamicTexture(width, height, layerTexture_, layerTextureView_)) {
        layerTexture_ = {};
        layerTextureView_ = {};
        layerTextureWidth_ = 0;
        layerTextureHeight_ = 0;
        return false;
      }
    }

    if (!uploadBgraPixels(layerTexture_.get(), frame)) {
      return false;
    }
    ++sourceTexStats_.scratchUploads;
    return true;
  }

  // Creates a dynamic (CPU-writable) BGRA texture + SRV.
  bool createBgraDynamicTexture(int width, int height, ComPtrLite<ID3D11Texture2D>& texture,
                                ComPtrLite<ID3D11ShaderResourceView>& view) {
    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DYNAMIC;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateTexture2D(&textureDesc, nullptr, texture.put()))) {
      return false;
    }
    if (FAILED(device_->CreateShaderResourceView(texture.get(), nullptr, view.put()))) {
      texture = {};
      return false;
    }
    return true;
  }

  // Maps a dynamic BGRA texture and copies the frame's pixel rows (honoring both
  // the source stride and the mapped RowPitch).
  bool uploadBgraPixels(ID3D11Texture2D* texture, const VideoFrame& frame) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      return false;
    }
    const auto* source = frame.pixels->data();
    const auto sourceStride = static_cast<size_t>(frame.pixelStride);
    auto* destination = static_cast<uint8_t*>(mapped.pData);
    const size_t rowBytes = static_cast<size_t>(frame.pixelWidth) * 4u;
    for (int y = 0; y < frame.pixelHeight; ++y) {
      std::memcpy(destination + static_cast<size_t>(y) * mapped.RowPitch, source + static_cast<size_t>(y) * sourceStride, rowBytes);
    }
    context_->Unmap(texture, 0);
    return true;
  }

  // Creates a single-channel (R8_UNORM) dynamic texture + SRV for one I420 plane.
  bool createR8Texture(int width, int height, ComPtrLite<ID3D11Texture2D>& texture,
                       ComPtrLite<ID3D11ShaderResourceView>& view) {
    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_R8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DYNAMIC;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateTexture2D(&textureDesc, nullptr, texture.put()))) {
      return false;
    }
    if (FAILED(device_->CreateShaderResourceView(texture.get(), nullptr, view.put()))) {
      texture = {};
      return false;
    }
    return true;
  }

  // Maps a single-channel plane and copies it row by row (honoring RowPitch).
  bool uploadPlane(ID3D11Texture2D* texture, const uint8_t* source, int sourceStride, int width, int height) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(texture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      return false;
    }
    auto* destination = static_cast<uint8_t*>(mapped.pData);
    for (int y = 0; y < height; ++y) {
      std::memcpy(destination + static_cast<size_t>(y) * mapped.RowPitch,
                  source + static_cast<size_t>(y) * static_cast<size_t>(sourceStride),
                  static_cast<size_t>(width));
    }
    context_->Unmap(texture, 0);
    return true;
  }

  // Uploads a frame's I420 planes into the shared Y/U/V GPU textures (recreating
  // them when the dimensions change). The YUV pixel shader then converts to RGB.
  bool uploadLayerI420Texture(const VideoFrame& frame) {
    if (!frame.hasI420()) {
      return false;
    }
    const int width = frame.i420Width;
    const int height = frame.i420Height;
    const int chromaWidth = width / 2;
    const int chromaHeight = height / 2;
    if (!yTexture_ || yuvTextureWidth_ != width || yuvTextureHeight_ != height) {
      yTexture_ = {};
      yTextureView_ = {};
      uTexture_ = {};
      uTextureView_ = {};
      vTexture_ = {};
      vTextureView_ = {};
      yuvTextureWidth_ = 0;
      yuvTextureHeight_ = 0;
      if (!createR8Texture(width, height, yTexture_, yTextureView_) ||
          !createR8Texture(chromaWidth, chromaHeight, uTexture_, uTextureView_) ||
          !createR8Texture(chromaWidth, chromaHeight, vTexture_, vTextureView_)) {
        yTexture_ = {};
        uTexture_ = {};
        vTexture_ = {};
        return false;
      }
      yuvTextureWidth_ = width;
      yuvTextureHeight_ = height;
    }

    const auto* base = frame.i420->data();
    const auto yLength = static_cast<size_t>(width) * static_cast<size_t>(height);
    const auto* yPlane = base;
    const auto* uPlane = base + yLength;
    const auto* vPlane = uPlane + static_cast<size_t>(chromaWidth) * static_cast<size_t>(chromaHeight);
    if (!uploadPlane(yTexture_.get(), yPlane, width, width, height) ||
        !uploadPlane(uTexture_.get(), uPlane, chromaWidth, chromaWidth, chromaHeight) ||
        !uploadPlane(vTexture_.get(), vPlane, chromaWidth, chromaWidth, chromaHeight)) {
      return false;
    }
    ++sourceTexStats_.scratchUploads;
    return true;
  }

  // Resolves the cached per-source textures for `frame`, uploading its pixels
  // only when the content changed since the last upload. Returns nullptr when
  // the frame has no stable source identity (or the GPU resources fail) — the
  // caller then falls back to the shared scratch upload path.
  SourceTex* acquireSourceTex(const VideoFrame& frame) {
    const bool isI420 = frame.hasI420();
    if (frame.participantId.empty() || (!isI420 && !frame.hasPixels())) {
      return nullptr;
    }
    const int width = isI420 ? frame.i420Width : frame.pixelWidth;
    const int height = isI420 ? frame.i420Height : frame.pixelHeight;
    auto& entry = sourceTextures_[frame.participantId];
    const bool haveTextures = isI420 ? static_cast<bool>(entry.y) : static_cast<bool>(entry.bgra);
    if (!haveTextures || entry.width != width || entry.height != height || entry.isI420 != isI420) {
      entry = SourceTex{};
      bool created = false;
      if (isI420) {
        created = createR8Texture(width, height, entry.y, entry.ySrv) &&
                  createR8Texture(width / 2, height / 2, entry.u, entry.uSrv) &&
                  createR8Texture(width / 2, height / 2, entry.v, entry.vSrv);
      } else {
        created = createBgraDynamicTexture(width, height, entry.bgra, entry.bgraSrv);
      }
      if (!created) {
        sourceTextures_.erase(frame.participantId);
        return nullptr;
      }
      entry.width = width;
      entry.height = height;
      entry.isI420 = isI420;
      ++sourceTexStats_.textureCreates;
    }
    const void* buffer = isI420 ? static_cast<const void*>(frame.i420->data())
                                : static_cast<const void*>(frame.pixels->data());
    if (frame.frameId != entry.lastFrameId || buffer != entry.lastBuffer) {
      bool uploaded = false;
      if (isI420) {
        const auto* base = frame.i420->data();
        const int chromaWidth = width / 2;
        const int chromaHeight = height / 2;
        const auto yLength = static_cast<size_t>(width) * static_cast<size_t>(height);
        const auto* uPlane = base + yLength;
        const auto* vPlane = uPlane + static_cast<size_t>(chromaWidth) * static_cast<size_t>(chromaHeight);
        uploaded = uploadPlane(entry.y.get(), base, width, width, height) &&
                   uploadPlane(entry.u.get(), uPlane, chromaWidth, chromaWidth, chromaHeight) &&
                   uploadPlane(entry.v.get(), vPlane, chromaWidth, chromaWidth, chromaHeight);
      } else {
        uploaded = uploadBgraPixels(entry.bgra.get(), frame);
      }
      if (!uploaded) {
        sourceTextures_.erase(frame.participantId);
        return nullptr;
      }
      entry.lastFrameId = frame.frameId;
      entry.lastBuffer = buffer;
      ++sourceTexStats_.cachedUploads;
    } else {
      ++sourceTexStats_.cacheHits;
    }
    entry.lastUsedFrame = frameNumber_;
    return &entry;
  }

  // Writes neutral/identity layer constants (white opaque, no color grade,
  // identity UV) — used when rendering an I420 frame to a per-participant export
  // texture, where no framing or grade applies.
  // Writes white-opaque, identity-UV layer constants carrying `grade` — used by
  // the per-participant export passes. The *0.1 axis scaling MUST match
  // writeLayerConstants so a source graded in the program composite gets the
  // identical grade in its export texture (preview/multiview match program).
  bool writeGradeConstants(const CompositorColorGrade& grade, const VideoFrame* frame = nullptr) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(constantBuffer_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      return false;
    }
    auto* constants = static_cast<LayerShaderConstants*>(mapped.pData);
    constants->color[0] = constants->color[1] = constants->color[2] = constants->color[3] = 1.f;
    constants->exposure = grade.exposure * 0.1f;
    constants->contrast = grade.contrast * 0.1f;
    constants->saturation = grade.saturation * 0.1f;
    constants->temperature = grade.temperature * 0.1f;
    constants->uvScale[0] = constants->uvScale[1] = 1.f;
    constants->uvOffset[0] = constants->uvOffset[1] = 0.f;
    applyYuvParams(constants, yuvShaderParamsForFrame(frame));
    context_->Unmap(constantBuffer_.get(), 0);
    ID3D11Buffer* buffers[] = {constantBuffer_.get()};
    context_->PSSetConstantBuffers(0, 1, buffers);
    return true;
  }

  uint32_t readProgramPixelSignature(int x, int y) const {
    if (!renderTarget_ || !stagingTexture_ || !context_) {
      return 0;
    }
    context_->CopyResource(stagingTexture_.get(), renderTarget_.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(stagingTexture_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return 0;
    }

    const auto* row = static_cast<const uint8_t*>(mapped.pData);
    const int clampedX = (std::max)(0, (std::min)(x, targetWidth_ - 1));
    const int clampedY = (std::max)(0, (std::min)(y, targetHeight_ - 1));
    const auto* pixel = row + static_cast<size_t>(clampedY) * mapped.RowPitch + static_cast<size_t>(clampedX) * 4u;
    const uint8_t b = pixel[0];
    const uint8_t g = pixel[1];
    const uint8_t r = pixel[2];
    const uint8_t a = pixel[3];
    context_->Unmap(stagingTexture_.get(), 0);
    return rgbaToSignature(r, g, b, a);
  }

  bool ensureSharedTexture(int width, int height) {
    if (width <= 0 || height <= 0) {
      return false;
    }
    if (sharedTexture_ && sharedWidth_ == width && sharedHeight_ == height) {
      return true;
    }

    sharedTexture_ = {};
    sharedKeyedMutex_ = {};
    sharedHandle_ = nullptr;
    sharedWidth_ = width;
    sharedHeight_ = height;

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    // Keyed mutex so the producer (this core) and the consumer (the WinUI shell)
    // never touch the texture concurrently across processes — unsynchronized
    // SHARED access caused the GPU to serialize/stall and the render rate to decay.
    textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(device_->CreateTexture2D(&textureDesc, nullptr, sharedTexture_.put()))) {
      return false;
    }

    ComPtrLite<IDXGIResource> dxgiResource;
    if (FAILED(sharedTexture_->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(dxgiResource.put())))) {
      sharedTexture_ = {};
      return false;
    }

    HANDLE handle = nullptr;
    if (FAILED(dxgiResource->GetSharedHandle(&handle)) || !handle) {
      sharedTexture_ = {};
      return false;
    }

    if (FAILED(sharedTexture_->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(sharedKeyedMutex_.put())))) {
      sharedTexture_ = {};
      return false;
    }

    sharedHandle_ = handle;
    return true;
  }

  void exportSharedTexture(ProgramFrame& frame) {
    if (!renderTarget_ || !context_ || targetWidth_ <= 0 || targetHeight_ <= 0) {
      return;
    }
    if (!ensureSharedTexture(targetWidth_, targetHeight_)) {
      return;
    }

    // Producer side: acquire key 0, write, release key 1 (handing the texture to
    // the consumer). Non-blocking — if the consumer still holds it, keep this
    // frame in renderTarget_ and copy on the next tick rather than stalling.
    if (sharedKeyedMutex_) {
      if (sharedKeyedMutex_->AcquireSync(0, 0) != S_OK) {
        // Still publish the (unchanged) handle metadata so the consumer keeps the
        // last frame; just skip this copy.
        frame.sharedTexture.sharedHandleHex = handleToHex(sharedHandle_);
        frame.sharedTexture.width = targetWidth_;
        frame.sharedTexture.height = targetHeight_;
        frame.sharedTexture.format = "B8G8R8A8_UNORM";
        frame.sharedTexture.frameNumber = frame.frameNumber;
        return;
      }
    }
    context_->CopyResource(sharedTexture_.get(), renderTarget_.get());
    if (sharedKeyedMutex_) {
      // No explicit Flush — the keyed mutex orders the producer's copy before the
      // consumer's read on the GPU timeline; a per-frame Flush stalls the CPU.
      sharedKeyedMutex_->ReleaseSync(1);
    }
    frame.sharedTexture.sharedHandleHex = handleToHex(sharedHandle_);
    frame.sharedTexture.width = targetWidth_;
    frame.sharedTexture.height = targetHeight_;
    frame.sharedTexture.format = "B8G8R8A8_UNORM";
    frame.sharedTexture.frameNumber = frame.frameNumber;
  }

  // Export one keyed-mutex shared texture per participant for the multiview tiles,
  // so the WinUI tiles present on the GPU instead of decoding base64 on the UI
  // thread. Runs on the (single) render thread, reusing the compositor device/
  // context. The shared texture is BGRA (the cross-process consumer expects BGRA):
  // BGRA source frames are copied via UpdateSubresource; I420 source frames are
  // converted to BGRA on the GPU with the YUV shader (render into the texture's
  // render-target view). Producer acquires key 0 / writes / releases key 1;
  // non-blocking so a busy consumer just keeps the last frame.
  // Resolves the color grade the PROGRAM composite would apply to this source:
  // the source's own per-route grade if it carries one, otherwise the scene-global
  // grade. Sources that are not a current program layer fall back to the global
  // grade — what the program would apply to them if shown. Baking this into the
  // per-participant export makes the PREVIEW / multiview tiles (which present the
  // export texture) match the graded PROGRAM look for the very same source instead
  // of showing an ungraded, brighter/warmer copy.
  CompositorColorGrade effectiveParticipantGrade(const CompositorRenderPlan& plan, const VideoFrame& frame) const {
    for (const auto& layer : plan.layers) {
      const bool matches =
          (!layer.participantId.empty() && layer.participantId == frame.participantId) ||
          (!layer.sourceId.empty() && layer.sourceId == frame.participantId);
      if (matches) {
        return layer.hasColorGrade ? layer.colorGrade : plan.colorGrade;
      }
    }
    return plan.colorGrade;
  }

  void exportParticipantTextures(const CompositorRenderPlan& renderPlan, const std::vector<VideoFrame>& frames, ProgramFrame& outFrame) {
    if (!device_ || !context_) {
      return;
    }
    for (const auto& f : frames) {
      if (f.participantId.empty() || !frameHasContent(f)) {
        continue;
      }
      const CompositorColorGrade grade = effectiveParticipantGrade(renderPlan, f);
      const bool useI420 = f.hasI420();
      const int width = useI420 ? f.i420Width : f.pixelWidth;
      const int height = useI420 ? f.i420Height : f.pixelHeight;
      auto& pt = participantTextures_[f.participantId];
      if (!pt.texture || pt.width != width || pt.height != height) {
        pt = ParticipantTex{};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        // RENDER_TARGET so the I420->BGRA shader can render into it; SHADER_RESOURCE
        // for the cross-process consumer. UpdateSubresource (BGRA path) still works
        // on a DEFAULT-usage render-target texture.
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtrLite<IDXGIResource> dxgiResource;
        HANDLE handle = nullptr;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, pt.texture.put())) ||
            FAILED(pt.texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(dxgiResource.put()))) ||
            FAILED(dxgiResource->GetSharedHandle(&handle)) || !handle ||
            FAILED(pt.texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(pt.mutex.put()))) ||
            FAILED(device_->CreateRenderTargetView(pt.texture.get(), nullptr, pt.rtv.put()))) {
          participantTextures_.erase(f.participantId);
          continue;
        }
        pt.handle = handle;
        pt.width = width;
        pt.height = height;
      }
      // Only re-upload when the frame OR the effective grade changed. Capture frames
      // are now HELD and re-emitted every render tick (so the program composite never
      // starves -> no pink), but re-uploading an unchanged frame to the per-participant
      // export texture every tick cost ~15ms/tick (3+ capture + Zoom at 1080p) and
      // collapsed the render to ~11fps. The texture already holds these pixels; just
      // keep emitting the handle. The grade check additionally re-bakes when the
      // operator drags the color-grade sliders on a held/static source, so preview
      // keeps matching the graded program look.
      const bool gradeChanged = !gradesEqual(grade, pt.lastGrade);
      const bool frameChanged = (f.frameId != pt.lastFrameId) || gradeChanged;
      if (frameChanged && pt.mutex) {
        // The per-participant export texture is a single-consumer keyed-mutex texture, but its
        // consumer (the preview host, when it shows this source) is INTERMITTENT: the preview
        // bus cycles its primary source (active-speaker), and capture / idle participants may
        // have NO consumer at all. A plain producer AcquireSync(0) only succeeds after the
        // consumer releases key 0; once the consumer moves away (leaving the mutex at key 1,
        // which the producer released) the producer can NEVER re-acquire key 0, so the texture
        // WEDGES on its last frame. That is exactly the "preview frozen when the same source is
        // also in program" bug: the preview's per-participant handle stops advancing while the
        // always-released program/multiview composites keep going.
        //
        // Fix: if AcquireSync(0) fails (no consumer returned the key), reclaim our own released
        // slot via AcquireSync(1). The producer then ALWAYS makes progress and the texture stays
        // fresh — like the always-released program/multiview composites — so a returning or
        // newly-attached consumer immediately sees live frames. The keyed mutex still guarantees
        // mutual exclusion (a consumer mid-copy holds the mutex, so the reclaim just fails that
        // tick and we skip the upload), so there is no tearing. Non-blocking (0ms) throughout, so
        // the render thread never stalls.
        HRESULT acquire = pt.mutex->AcquireSync(0, 0);
        if (acquire != S_OK) {
          acquire = pt.mutex->AcquireSync(1, 0);
        }
        if (acquire == S_OK) {
          bool uploaded = false;
          if (useI420) {
            uploaded = renderI420ToParticipantTexture(f, pt, width, height, grade);
          } else if (gradeIsIdentity(grade)) {
            // Fast path for the common ungraded source: a straight BGRA copy, no
            // shader pass (preserves the perf note above — no per-tick convert cost).
            context_->UpdateSubresource(pt.texture.get(), 0, nullptr, f.pixels->data(),
                                        static_cast<UINT>(f.pixelStride), 0);
            uploaded = true;
          } else {
            // Graded BGRA source: render through the textured grade shader so the
            // export carries the same look the program applies to this source.
            uploaded = renderBgraToParticipantTexture(f, pt, width, height, grade);
          }
          pt.mutex->ReleaseSync(1);
          if (uploaded) {
            pt.lastFrameId = f.frameId;
            pt.lastGrade = grade;
          }
        }
      }
      ParticipantSharedTexture info;
      info.participantId = f.participantId;
      info.sharedHandleHex = handleToHex(pt.handle);
      info.width = pt.width;
      info.height = pt.height;
      info.frameNumber = outFrame.frameNumber;
      outFrame.participantSharedTextures.push_back(std::move(info));
    }
    // Drop textures for participants that are no longer delivering content.
    for (auto it = participantTextures_.begin(); it != participantTextures_.end();) {
      bool present = false;
      for (const auto& f : frames) {
        if (f.participantId == it->first && frameHasContent(f)) {
          present = true;
          break;
        }
      }
      it = present ? std::next(it) : participantTextures_.erase(it);
    }
  }

  // Converts a participant's I420 frame to BGRA on the GPU by rendering a
  // full-screen triangle with the YUV shader into the participant export
  // texture's render-target view. The caller holds the keyed-mutex lock. Returns
  // true when the convert was issued. Clobbers render state (render target,
  // viewport, shaders) — only called from exportParticipantTextures, which runs
  // after the program composite + share, so the next render() re-binds its state.
  bool renderI420ToParticipantTexture(const VideoFrame& frame, ParticipantTex& pt, int width, int height,
                                      const CompositorColorGrade& grade) {
    if (!pt.rtv) {
      return false;
    }
    // Sample the per-source cache (already uploaded by this tick's composite
    // passes when the source is on screen); fall back to the scratch upload.
    SourceTex* sourceTex = acquireSourceTex(frame);
    if (!sourceTex && !uploadLayerI420Texture(frame)) {
      return false;
    }
    if (!beginParticipantExportPass(pt, width, height, grade, &frame)) {
      return false;
    }
    context_->PSSetShader(yuvPixelShader_.get(), nullptr, 0);
    ID3D11ShaderResourceView* views[] = {
        sourceTex ? sourceTex->ySrv.get() : yTextureView_.get(),
        sourceTex ? sourceTex->uSrv.get() : uTextureView_.get(),
        sourceTex ? sourceTex->vSrv.get() : vTextureView_.get()};
    context_->PSSetShaderResources(0, 3, views);
    ID3D11SamplerState* samplers[] = {samplerState_.get()};
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullViews[] = {nullptr, nullptr, nullptr};
    context_->PSSetShaderResources(0, 3, nullViews);
    endParticipantExportPass();
    return true;
  }

  // Grade-baking variant of the BGRA export: instead of a raw UpdateSubresource
  // copy, render the decoded frame through the textured grade shader so a graded
  // capture/media source's export matches the program look (the raw-copy fast path
  // in exportParticipantTextures still handles the common ungraded case). Same
  // effective color pipeline the program's drawLayer BGRA path uses.
  bool renderBgraToParticipantTexture(const VideoFrame& frame, ParticipantTex& pt, int width, int height,
                                      const CompositorColorGrade& grade) {
    if (!pt.rtv) {
      return false;
    }
    SourceTex* sourceTex = acquireSourceTex(frame);
    if (!sourceTex && !uploadLayerTexture(frame)) {
      return false;
    }
    if (!beginParticipantExportPass(pt, width, height, grade)) {
      return false;
    }
    context_->PSSetShader(texturedPixelShader_.get(), nullptr, 0);
    ID3D11ShaderResourceView* views[] = {sourceTex ? sourceTex->bgraSrv.get() : layerTextureView_.get()};
    context_->PSSetShaderResources(0, 1, views);
    ID3D11SamplerState* samplers[] = {samplerState_.get()};
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullViews[] = {nullptr};
    context_->PSSetShaderResources(0, 1, nullViews);
    endParticipantExportPass();
    return true;
  }

  // Shared render-state setup for the per-participant export convert passes:
  // targets the export RTV, sets a full-texture viewport, binds the vertex shader,
  // opaque-overwrite blend (the export is a 1:1 copy) and identity-UV constants
  // carrying the source's effective color grade. Returns false (and unbinds the
  // target) if the constant upload fails.
  bool beginParticipantExportPass(ParticipantTex& pt, int width, int height, const CompositorColorGrade& grade,
                                  const VideoFrame* frame = nullptr) {
    ID3D11RenderTargetView* renderTargets[] = {pt.rtv.get()};
    context_->OMSetRenderTargets(1, renderTargets, nullptr);

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.f;
    viewport.MaxDepth = 1.f;
    context_->RSSetViewports(1, &viewport);

    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->RSSetState(rasterizerState_.get());
    // Opaque overwrite (no source-over blend) — the export texture is a 1:1 copy.
    context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
    context_->VSSetShader(vertexShader_.get(), nullptr, 0);
    if (!writeGradeConstants(grade, frame)) {
      ID3D11RenderTargetView* nullTargets[] = {nullptr};
      context_->OMSetRenderTargets(1, nullTargets, nullptr);
      return false;
    }
    return true;
  }

  void endParticipantExportPass() {
    ID3D11RenderTargetView* nullTargets[] = {nullptr};
    context_->OMSetRenderTargets(1, nullTargets, nullptr);
    // Restore the blend state the program composite expects for the next frame.
    context_->OMSetBlendState(blendState_.get(), nullptr, 0xffffffffu);
  }

  // Multiview render target: mirrors ensureRenderTarget but with NO CPU staging
  // texture (the multiview path never does a GPU->CPU readback — it only exports
  // the GPU shared texture).
  bool ensureMultiviewRenderTarget(int width, int height) {
    if (width <= 0 || height <= 0) {
      return false;
    }
    if (multiviewRenderTarget_ && multiviewWidth_ == width && multiviewHeight_ == height) {
      return true;
    }

    multiviewRenderTarget_ = {};
    multiviewRenderTargetView_ = {};
    multiviewWidth_ = width;
    multiviewHeight_ = height;

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&textureDesc, nullptr, multiviewRenderTarget_.put()))) {
      return false;
    }
    return SUCCEEDED(device_->CreateRenderTargetView(multiviewRenderTarget_.get(), nullptr, multiviewRenderTargetView_.put()));
  }

  // Multiview keyed-mutex shared texture: an exact copy of ensureSharedTexture
  // for the second (multiview) texture, so the WinUI consumer presents it the
  // same proven way it presents the program shared texture.
  bool ensureMultiviewSharedTexture(int width, int height) {
    if (width <= 0 || height <= 0) {
      return false;
    }
    if (multiviewSharedTexture_ && multiviewSharedWidth_ == width && multiviewSharedHeight_ == height) {
      return true;
    }

    multiviewSharedTexture_ = {};
    multiviewKeyedMutex_ = {};
    multiviewSharedHandle_ = nullptr;
    multiviewSharedWidth_ = width;
    multiviewSharedHeight_ = height;

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(device_->CreateTexture2D(&textureDesc, nullptr, multiviewSharedTexture_.put()))) {
      return false;
    }

    ComPtrLite<IDXGIResource> dxgiResource;
    if (FAILED(multiviewSharedTexture_->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(dxgiResource.put())))) {
      multiviewSharedTexture_ = {};
      return false;
    }

    HANDLE handle = nullptr;
    if (FAILED(dxgiResource->GetSharedHandle(&handle)) || !handle) {
      multiviewSharedTexture_ = {};
      return false;
    }

    if (FAILED(multiviewSharedTexture_->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(multiviewKeyedMutex_.put())))) {
      multiviewSharedTexture_ = {};
      return false;
    }

    multiviewSharedHandle_ = handle;
    return true;
  }

  // Mirrors exportSharedTexture for the multiview texture: acquire key 0, copy
  // the multiview render target, release key 1. Non-blocking — if the consumer
  // still holds the texture, publish the unchanged handle metadata and skip the
  // copy rather than stalling.
  void exportMultiviewSharedTexture(ProgramFrameSharedTexture& out) {
    if (!multiviewRenderTarget_ || !context_ || multiviewWidth_ <= 0 || multiviewHeight_ <= 0) {
      return;
    }
    if (!ensureMultiviewSharedTexture(multiviewWidth_, multiviewHeight_)) {
      return;
    }

    if (multiviewKeyedMutex_) {
      if (multiviewKeyedMutex_->AcquireSync(0, 0) != S_OK) {
        out.sharedHandleHex = handleToHex(multiviewSharedHandle_);
        out.width = multiviewWidth_;
        out.height = multiviewHeight_;
        out.format = "B8G8R8A8_UNORM";
        out.frameNumber = frameNumber_;
        return;
      }
    }
    context_->CopyResource(multiviewSharedTexture_.get(), multiviewRenderTarget_.get());
    if (multiviewKeyedMutex_) {
      multiviewKeyedMutex_->ReleaseSync(1);
    }
    out.sharedHandleHex = handleToHex(multiviewSharedHandle_);
    out.width = multiviewWidth_;
    out.height = multiviewHeight_;
    out.format = "B8G8R8A8_UNORM";
    out.frameNumber = frameNumber_;
  }

  // Preview render target: mirrors ensureMultiviewRenderTarget (no CPU staging —
  // the preview path never reads back; only the GPU shared texture is presented).
  bool ensurePreviewRenderTarget(int width, int height) {
    if (width <= 0 || height <= 0) {
      return false;
    }
    if (previewRenderTarget_ && previewWidth_ == width && previewHeight_ == height) {
      return true;
    }

    previewRenderTarget_ = {};
    previewRenderTargetView_ = {};
    previewWidth_ = width;
    previewHeight_ = height;

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&textureDesc, nullptr, previewRenderTarget_.put()))) {
      return false;
    }
    return SUCCEEDED(device_->CreateRenderTargetView(previewRenderTarget_.get(), nullptr, previewRenderTargetView_.put()));
  }

  // Preview keyed-mutex shared texture: an exact copy of ensureMultiviewSharedTexture
  // so the WinUI consumer presents it the same proven way it presents program.
  bool ensurePreviewSharedTexture(int width, int height) {
    if (width <= 0 || height <= 0) {
      return false;
    }
    if (previewSharedTexture_ && previewSharedWidth_ == width && previewSharedHeight_ == height) {
      return true;
    }

    previewSharedTexture_ = {};
    previewKeyedMutex_ = {};
    previewSharedHandle_ = nullptr;
    previewSharedWidth_ = width;
    previewSharedHeight_ = height;

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = static_cast<UINT>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(device_->CreateTexture2D(&textureDesc, nullptr, previewSharedTexture_.put()))) {
      return false;
    }

    ComPtrLite<IDXGIResource> dxgiResource;
    if (FAILED(previewSharedTexture_->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(dxgiResource.put())))) {
      previewSharedTexture_ = {};
      return false;
    }

    HANDLE handle = nullptr;
    if (FAILED(dxgiResource->GetSharedHandle(&handle)) || !handle) {
      previewSharedTexture_ = {};
      return false;
    }

    if (FAILED(previewSharedTexture_->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(previewKeyedMutex_.put())))) {
      previewSharedTexture_ = {};
      return false;
    }

    previewSharedHandle_ = handle;
    return true;
  }

  // Mirrors exportMultiviewSharedTexture for the preview texture: acquire key 0,
  // copy the preview render target, release key 1. Non-blocking.
  void exportPreviewSharedTexture(ProgramFrameSharedTexture& out) {
    if (!previewRenderTarget_ || !context_ || previewWidth_ <= 0 || previewHeight_ <= 0) {
      return;
    }
    if (!ensurePreviewSharedTexture(previewWidth_, previewHeight_)) {
      return;
    }

    if (previewKeyedMutex_) {
      if (previewKeyedMutex_->AcquireSync(0, 0) != S_OK) {
        out.sharedHandleHex = handleToHex(previewSharedHandle_);
        out.width = previewWidth_;
        out.height = previewHeight_;
        out.format = "B8G8R8A8_UNORM";
        out.frameNumber = frameNumber_;
        return;
      }
    }
    context_->CopyResource(previewSharedTexture_.get(), previewRenderTarget_.get());
    if (previewKeyedMutex_) {
      previewKeyedMutex_->ReleaseSync(1);
    }
    out.sharedHandleHex = handleToHex(previewSharedHandle_);
    out.width = previewWidth_;
    out.height = previewHeight_;
    out.format = "B8G8R8A8_UNORM";
    out.frameNumber = frameNumber_;
  }

  ProgramFramePreviewPixels readProgramFramePreview() const {
    ProgramFramePreviewPixels preview;
    if (!renderTarget_ || !stagingTexture_ || !context_ || targetWidth_ <= 0 || targetHeight_ <= 0) {
      return preview;
    }

    context_->CopyResource(stagingTexture_.get(), renderTarget_.get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(stagingTexture_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
      return preview;
    }

    downscaleBgraNearestNeighbor(
        static_cast<const uint8_t*>(mapped.pData),
        targetWidth_,
        targetHeight_,
        static_cast<int>(mapped.RowPitch),
        preview);
    context_->Unmap(stagingTexture_.get(), 0);
    return preview;
  }

  // RENDER THREAD (cheap, ~microseconds): copy the finished program frame into a
  // GPU-only keyed-mutex SHARED texture (GPU->GPU, no CPU read, no staging). A
  // second D3D device on a dedicated thread (vcamTapLoop) reads it back. Producer
  // side: acquire key 0, copy, release key 1. Non-blocking - if the tap still
  // holds it, skip this frame.
  void exportVcamSharedTexture() {
    if (!renderTarget_ || !context_ || targetWidth_ <= 0 || targetHeight_ <= 0) {
      return;
    }
    // The shared texture is a FIXED 1920x1080 (the vcam's native format); the
    // program may be 4K. Downscale on the GPU here with a linear scale-blit, so
    // the render-thread copy and the tap readback are both 1080p (not 4K) - keeps
    // the render tick cheap (Take stays responsive) and the readback fast.
    if (!ensureVcamTap(kVcamOutW, kVcamOutH) || !vcamShared1Rtv_ || !vcamMutex1_) {
      return;
    }
    if (!ensureVcamSourceSrv()) {
      return;
    }
    if (vcamMutex1_->AcquireSync(0, 0) != S_OK) {
      return;  // tap holds it; keep this frame in renderTarget_, blit next tick
    }
    // Scale-blit: draw the full program (SRV) into the 1080p shared RTV. State
    // set here is re-established by the next render() tick (and the passes that
    // run after this one set their own), so no save/restore is needed - but we
    // must unbind the source SRV afterward so next frame can bind it as an RTV.
    ID3D11RenderTargetView* rtv = vcamShared1Rtv_.get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0.f, 0.f, static_cast<float>(kVcamOutW), static_cast<float>(kVcamOutH), 0.f, 1.f};
    context_->RSSetViewports(1, &vp);
    D3D11_RECT scissor{0, 0, kVcamOutW, kVcamOutH};
    context_->RSSetScissorRects(1, &scissor);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.get(), nullptr, 0);
    context_->PSSetShader(texturedPixelShader_.get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {vcamSourceSrv_.get()};
    context_->PSSetShaderResources(0, 1, srvs);
    ID3D11SamplerState* samplers[] = {samplerState_.get()};
    context_->PSSetSamplers(0, 1, samplers);
    writeVcamIdentityConstants();
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv[] = {nullptr};
    context_->PSSetShaderResources(0, 1, nullSrv);
    vcamMutex1_->ReleaseSync(1);
    vcamNewFrame_.store(true, std::memory_order_release);
    vcamCv_.notify_one();
  }

  // (Re)creates the SRV over renderTarget_ so the scale-blit can sample the
  // program. Rebuilt when renderTarget_ is recreated (output-size change).
  bool ensureVcamSourceSrv() {
    if (vcamSourceSrv_ && vcamSourceSrvFrom_ == renderTarget_.get()) {
      return true;
    }
    vcamSourceSrv_ = {};
    vcamSourceSrvFrom_ = nullptr;
    if (!renderTarget_) return false;
    if (FAILED(device_->CreateShaderResourceView(renderTarget_.get(), nullptr, vcamSourceSrv_.put()))) {
      return false;
    }
    vcamSourceSrvFrom_ = renderTarget_.get();
    return true;
  }

  // Identity layer constants for the pass-through scale-blit (no grade, full UV).
  void writeVcamIdentityConstants() {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(constantBuffer_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      return;
    }
    auto* c = static_cast<LayerShaderConstants*>(mapped.pData);
    c->color[0] = c->color[1] = c->color[2] = c->color[3] = 1.f;
    c->exposure = c->contrast = c->saturation = c->temperature = 0.f;
    c->uvScale[0] = c->uvScale[1] = 1.f;
    c->uvOffset[0] = c->uvOffset[1] = 0.f;
    context_->Unmap(constantBuffer_.get(), 0);
    ID3D11Buffer* buffers[] = {constantBuffer_.get()};
    context_->PSSetConstantBuffers(0, 1, buffers);
  }

  // Lazily builds the vcam readback rig: a dedicated D3D device + a keyed-mutex
  // shared texture the render device writes and the tap device reads, plus the
  // tap thread. Rebuilt if the program size changes. Returns false on failure.
  bool ensureVcamTap(int width, int height) {
    if (vcamShared1_ && vcamTapW_ == width && vcamTapH_ == height) {
      return true;
    }
    stopVcamTap();  // tear down any prior-size rig first
    vcamTapW_ = width;
    vcamTapH_ = height;

    // Keyed-mutex shared texture on the RENDER device (producer).
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, vcamShared1_.put()))) {
      return false;
    }
    if (FAILED(device_->CreateRenderTargetView(vcamShared1_.get(), nullptr, vcamShared1Rtv_.put()))) {
      vcamShared1_ = {};
      return false;
    }
    ComPtrLite<IDXGIResource> dxgiRes;
    HANDLE shared = nullptr;
    if (FAILED(vcamShared1_->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(dxgiRes.put()))) ||
        FAILED(dxgiRes->GetSharedHandle(&shared)) || shared == nullptr ||
        FAILED(vcamShared1_->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(vcamMutex1_.put())))) {
      vcamShared1_ = {};
      vcamMutex1_ = {};
      return false;
    }

    // Second D3D device (consumer) - its own immediate context, so the slow read
    // never touches the render context (no multithread tax, no coreMutex).
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                 vcamDevice2_.put(), &fl, vcamContext2_.put()))) {
      stopVcamTap();
      return false;
    }
    if (FAILED(vcamDevice2_->OpenSharedResource(shared, __uuidof(ID3D11Texture2D),
                                                reinterpret_cast<void**>(vcamShared2_.put()))) ||
        FAILED(vcamShared2_->QueryInterface(__uuidof(IDXGIKeyedMutex),
                                            reinterpret_cast<void**>(vcamMutex2_.put())))) {
      stopVcamTap();
      return false;
    }
    // Staging on the SECOND device (CPU read) - the ~8MB read lands here, off the
    // render thread entirely.
    D3D11_TEXTURE2D_DESC sdesc = desc;
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.BindFlags = 0;
    sdesc.MiscFlags = 0;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(vcamDevice2_->CreateTexture2D(&sdesc, nullptr, vcamStaging2_.put()))) {
      stopVcamTap();
      return false;
    }
    vcamTapStop_.store(false, std::memory_order_release);
    vcamThread_ = std::thread([this] { vcamTapLoop(); });
    std::fprintf(stderr, "[vcam-tap] rig built OK %dx%d (device2 + shared + staging + thread)\n", width, height);
    return true;
  }

  // DEDICATED TAP THREAD (own device): the slow GPU->staging copy + Map + ~8MB CPU
  // read happen HERE, touching neither the render thread/coreMutex nor the audio
  // worker. The converted NV12 is stashed in vcamLatestNv12_; the output worker
  // fetches it with a cheap CPU copy via takeVcamNv12().
  void vcamTapLoop() {
    // Throughput work, not latency work: this thread reads ~8MB/frame of UNCACHED
    // (write-combined) staging memory + converts to NV12 - the most memory-bus-hostile
    // load in the app. Run it BELOW_NORMAL so under system pressure it loses slices
    // before the OS audio engine / other apps do: a dropped vcam frame is invisible,
    // system-wide audio glitching (owner's browser+DAW stress test) is not.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (!vcamTapStop_.load(std::memory_order_acquire)) {
      {
        std::unique_lock<std::mutex> lk(vcamCvMutex_);
        vcamCv_.wait_for(lk, std::chrono::milliseconds(16), [this] {
          return vcamNewFrame_.load(std::memory_order_acquire) ||
                 vcamTapStop_.load(std::memory_order_acquire);
        });
        vcamNewFrame_.store(false, std::memory_order_release);
      }
      if (vcamTapStop_.load(std::memory_order_acquire)) break;
      if (!vcamMutex2_ || !vcamShared2_ || !vcamStaging2_ || !vcamContext2_) continue;
      if (vcamMutex2_->AcquireSync(1, 8) != S_OK) continue;  // producer still writing; try later
      vcamContext2_->CopyResource(vcamStaging2_.get(), vcamShared2_.get());
      vcamMutex2_->ReleaseSync(0);
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (SUCCEEDED(vcamContext2_->Map(vcamStaging2_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        const int w = vcamTapW_;
        const int h = vcamTapH_;
        const size_t rowBytes = static_cast<size_t>(w) * 4u;
        // Copy the mapped staging (slow uncached GPU memory) into a cached buffer
        // ONCE, then Unmap, then convert - reading from the cached copy is much
        // faster than converting straight out of mapped staging memory.
        vcamTapBuffer_.resize(rowBytes * static_cast<size_t>(h));
        const auto* src = static_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < h; ++y) {
          std::memcpy(vcamTapBuffer_.data() + static_cast<size_t>(y) * rowBytes,
                      src + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
        }
        vcamContext2_->Unmap(vcamStaging2_.get(), 0);
        // The program renders at the output resolution (e.g. 4K); the virtual
        // camera serves a FIXED 1920x1080 (the DLL's media type). Downscale to
        // that here on the tap thread (off every hot path), then convert to NV12.
        const std::uint8_t* srcBgra = vcamTapBuffer_.data();
        int cw = w;
        int ch = h;
        if (w != kVcamOutW || h != kVcamOutH) {
          if (resizeBgraBilinear(vcamTapBuffer_.data(), w, h, kVcamOutW, kVcamOutH, vcamResized_)) {
            srcBgra = vcamResized_.data();
            cw = kVcamOutW;
            ch = kVcamOutH;
          }
        }
        // Expensive BGRA->NV12 conversion, on THIS tap thread.
        std::vector<uint8_t> nv12;
        if (convertBgraToNv12(srcBgra, cw, ch, nv12)) {
          std::lock_guard<std::mutex> lock(vcamNv12Mutex_);
          vcamLatestW_ = cw;
          vcamLatestH_ = ch;
          vcamLatestNv12_.swap(nv12);
          ++vcamLatestGen_;
        }
      }
    }
  }

  void stopVcamTap() {
    vcamTapStop_.store(true, std::memory_order_release);
    vcamCv_.notify_all();
    if (vcamThread_.joinable()) {
      vcamThread_.join();
    }
    vcamStaging2_ = {};
    vcamShared2_ = {};
    vcamMutex2_ = {};
    vcamContext2_ = {};
    vcamDevice2_ = {};
    vcamShared1_ = {};
    vcamShared1Rtv_ = {};
    vcamMutex1_ = {};
    vcamSourceSrv_ = {};
    vcamSourceSrvFrom_ = nullptr;
    vcamTapW_ = 0;
    vcamTapH_ = 0;
  }

  // OUTPUT-WORKER THREAD (cheap): hand back the tap's most recent NV12 frame with
  // a plain ~3MB CPU copy - no GPU Map, no per-pixel conversion here, so it never
  // starves the audio/output worker. Returns false when there's no new frame
  // since the last take.
  bool takeVcamNv12(std::vector<uint8_t>& outNv12, int& w, int& h) override {
    std::lock_guard<std::mutex> lock(vcamNv12Mutex_);
    if (vcamLatestNv12_.empty() || vcamLatestGen_ == vcamLastTakenGen_) {
      return false;
    }
    vcamLastTakenGen_ = vcamLatestGen_;
    w = vcamLatestW_;
    h = vcamLatestH_;
    outNv12 = vcamLatestNv12_;  // copy out (caller owns it)
    return true;
  }

  ComPtrLite<ID3D11Device> device_;
  ComPtrLite<ID3D11DeviceContext> context_;
  ComPtrLite<ID3D11VertexShader> vertexShader_;
  ComPtrLite<ID3D11PixelShader> pixelShader_;
  ComPtrLite<ID3D11PixelShader> texturedPixelShader_;
  ComPtrLite<ID3D11PixelShader> yuvPixelShader_;
  ComPtrLite<ID3D11PixelShader> overlayPixelShader_;
  ComPtrLite<ID3D11SamplerState> samplerState_;
  ComPtrLite<ID3D11Buffer> constantBuffer_;
  ComPtrLite<ID3D11BlendState> blendState_;
  ComPtrLite<ID3D11BlendState> premultipliedBlendState_;
  ComPtrLite<ID3D11RasterizerState> rasterizerState_;
  ComPtrLite<ID3D11RasterizerState> scissorRasterizerState_;
  ComPtrLite<ID3D11Texture2D> renderTarget_;
  ComPtrLite<ID3D11RenderTargetView> renderTargetView_;
  ComPtrLite<ID3D11Texture2D> stagingTexture_;
  // Virtual-camera readback rig (see exportVcamSharedTexture / vcamTapLoop). The
  // render device writes vcamShared1_ (fast GPU->GPU); a SECOND device reads it
  // via vcamShared2_ + a staging texture on a dedicated thread, so the slow ~8MB
  // CPU read touches neither the render thread/coreMutex nor the audio worker.
  ComPtrLite<ID3D11Texture2D> vcamShared1_;   // render device, keyed-mutex shared (1080p)
  ComPtrLite<ID3D11RenderTargetView> vcamShared1Rtv_;
  ComPtrLite<IDXGIKeyedMutex> vcamMutex1_;
  ComPtrLite<ID3D11ShaderResourceView> vcamSourceSrv_;  // SRV of renderTarget_ (the 4K program)
  ID3D11Texture2D* vcamSourceSrvFrom_ = nullptr;        // detects renderTarget_ recreation
  ComPtrLite<ID3D11Device> vcamDevice2_;      // dedicated readback device
  ComPtrLite<ID3D11DeviceContext> vcamContext2_;
  ComPtrLite<ID3D11Texture2D> vcamShared2_;   // vcamShared1_ opened on device2
  ComPtrLite<IDXGIKeyedMutex> vcamMutex2_;
  ComPtrLite<ID3D11Texture2D> vcamStaging2_;  // CPU-read staging on device2
  int vcamTapW_ = 0;
  int vcamTapH_ = 0;
  std::thread vcamThread_;
  std::atomic<bool> vcamTapStop_{true};
  std::atomic<bool> vcamNewFrame_{false};
  std::mutex vcamCvMutex_;
  std::condition_variable vcamCv_;
  // The virtual camera serves a fixed 1920x1080 (the DLL's NV12 media type); the
  // program may render larger (up to 4K), so the tap downscales to this.
  static constexpr int kVcamOutW = 1920;
  static constexpr int kVcamOutH = 1080;
  // The tap reads BGRA into vcamTapBuffer_ (tap-thread only), downscales into
  // vcamResized_ if needed, and converts to NV12 (the expensive per-pixel step,
  // done HERE off the output worker). The output worker then copies out the small
  // NV12 result cheaply via takeVcamNv12.
  std::vector<uint8_t> vcamTapBuffer_;
  std::vector<uint8_t> vcamResized_;
  std::mutex vcamNv12Mutex_;
  std::vector<uint8_t> vcamLatestNv12_;
  int vcamLatestW_ = 0;
  int vcamLatestH_ = 0;
  uint64_t vcamLatestGen_ = 0;
  uint64_t vcamLastTakenGen_ = 0;
  ComPtrLite<ID3D11Texture2D> sharedTexture_;
  ComPtrLite<IDXGIKeyedMutex> sharedKeyedMutex_;
  // Per-participant keyed-mutex shared textures for the GPU multiview tiles.
  // (ParticipantTex is declared near the top of the class.)
  std::map<std::string, ParticipantTex> participantTextures_;
  // Per-source texture cache (SourceTex is declared near the top of the class):
  // one Y/U/V or BGRA texture set per stable source id, shared by every
  // composite pass + the export converts; entries evicted in render() when
  // unused. The counters feed ICompositor::sourceTexStats().
  std::map<std::string, SourceTex> sourceTextures_;
  CompositorSourceTexStats sourceTexStats_;
  // Multiview pass: a second render target + keyed-mutex shared texture mirroring
  // the program members above (no CPU staging — multiview never reads back).
  ComPtrLite<ID3D11Texture2D> multiviewRenderTarget_;
  ComPtrLite<ID3D11RenderTargetView> multiviewRenderTargetView_;
  ComPtrLite<ID3D11Texture2D> multiviewSharedTexture_;
  ComPtrLite<IDXGIKeyedMutex> multiviewKeyedMutex_;
  // Preview pass: a third render target + keyed-mutex shared texture mirroring the
  // program/multiview members (no CPU staging — preview never reads back).
  ComPtrLite<ID3D11Texture2D> previewRenderTarget_;
  ComPtrLite<ID3D11RenderTargetView> previewRenderTargetView_;
  ComPtrLite<ID3D11Texture2D> previewSharedTexture_;
  ComPtrLite<IDXGIKeyedMutex> previewKeyedMutex_;
  ComPtrLite<ID3D11Texture2D> layerTexture_;
  ComPtrLite<ID3D11ShaderResourceView> layerTextureView_;
  // I420 plane textures (R8_UNORM) shared by the program-composite draw and the
  // per-participant export convert; recreated when the source dimensions change.
  ComPtrLite<ID3D11Texture2D> yTexture_;
  ComPtrLite<ID3D11ShaderResourceView> yTextureView_;
  ComPtrLite<ID3D11Texture2D> uTexture_;
  ComPtrLite<ID3D11ShaderResourceView> uTextureView_;
  ComPtrLite<ID3D11Texture2D> vTexture_;
  ComPtrLite<ID3D11ShaderResourceView> vTextureView_;
  int yuvTextureWidth_ = 0;
  int yuvTextureHeight_ = 0;
  // DirectWrite/D2D overlay raster cache: overlayContentSignature (FNV-1a over
  // text/brand/font/size, deliberately excluding animation state) -> rendered
  // text texture. Rasters happen only on content/size change; frames reuse the
  // SRV.
  struct OverlayRasterTex {
    ComPtrLite<ID3D11Texture2D> texture;
    ComPtrLite<ID3D11ShaderResourceView> view;
    uint64_t lastUsed = 0;
  };
  std::map<uint64_t, OverlayRasterTex> overlayTextTextures_;
  uint64_t overlayRasterClock_ = 0;
  ComPtrLite<ID2D1Factory> d2dFactory_;
  ComPtrLite<IDWriteFactory> dwriteFactory_;
  ComPtrLite<IWICImagingFactory> wicFactory_;
  bool overlayRasterUnavailable_ = false;
  int layerTextureWidth_ = 0;
  int layerTextureHeight_ = 0;
  HANDLE sharedHandle_ = nullptr;
  int sharedWidth_ = 0;
  int sharedHeight_ = 0;
  HANDLE multiviewSharedHandle_ = nullptr;
  int multiviewWidth_ = 0;
  int multiviewHeight_ = 0;
  int multiviewSharedWidth_ = 0;
  int multiviewSharedHeight_ = 0;
  HANDLE previewSharedHandle_ = nullptr;
  int previewWidth_ = 0;
  int previewHeight_ = 0;
  int previewSharedWidth_ = 0;
  int previewSharedHeight_ = 0;
  int targetWidth_ = 0;
  int targetHeight_ = 0;
  int64_t frameNumber_ = 0;
  bool pipelineReady_ = false;
  std::string initError_;
};

}  // namespace

std::unique_ptr<ICompositor> createD3D11Compositor() {
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL selectedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
  constexpr D3D_FEATURE_LEVEL featureLevels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };

  const HRESULT result = D3D11CreateDevice(
      nullptr,
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      featureLevels,
      static_cast<UINT>(std::size(featureLevels)),
      D3D11_SDK_VERSION,
      device.put(),
      &selectedFeatureLevel,
      context.put());

  if (FAILED(result) || !device || !context) {
    return nullptr;
  }

  return std::make_unique<D3D11Compositor>(std::move(device), std::move(context));
}

}  // namespace corevideo::modules

#else

namespace corevideo::modules {

std::unique_ptr<ICompositor> createD3D11Compositor() {
  return nullptr;
}

}  // namespace corevideo::modules

#endif
