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
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include "compositor/CompositorLayout.h"
#include "modules/ProgramFramePreview.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <map>
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
};

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
    exportSharedTexture(frame);
    exportParticipantTextures(frames, frame);
    context_->Flush();
    return frame;
  }

 private:
  struct ResolvedLayer {
    CompositorRenderPlanLayer plan;
    uint32_t color = 0xff808080;
    const VideoFrame* frame = nullptr;
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
        } else if (!layer.plan.mediaAssetId.empty()) {
          layer.color = compositor::colorFromParticipantId("media:" + layer.plan.mediaAssetId);
          const std::string frameSourceId = layer.plan.sourceId.empty() ? "media:" + layer.plan.mediaAssetId : layer.plan.sourceId;
          layer.frame = frameForParticipant(frames, frameSourceId);
        } else if (videoIndex > 0 && videoIndex - 1 < static_cast<int>(frames.size())) {
          const auto& fallbackFrame = frames[static_cast<size_t>(videoIndex - 1)];
          layer.color = compositor::colorFromParticipantId(fallbackFrame.participantId);
          if (fallbackFrame.hasPixels()) {
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
      if (frames[static_cast<size_t>(index)].hasPixels()) {
        layer.frame = &frames[static_cast<size_t>(index)];
      }
      layers.push_back(std::move(layer));
    }
    return layers;
  }

  static const VideoFrame* frameForParticipant(const std::vector<VideoFrame>& frames, const std::string& participantId) {
    if (participantId.empty()) {
      return nullptr;
    }
    for (const auto& frame : frames) {
      if (frame.participantId == participantId && frame.hasPixels()) {
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

    const bool textured = layer.frame != nullptr && layer.frame->hasPixels() && uploadLayerTexture(*layer.frame);
    if (textured) {
      context_->PSSetShader(texturedPixelShader_.get(), nullptr, 0);
      ID3D11ShaderResourceView* views[] = {layerTextureView_.get()};
      ID3D11SamplerState* samplers[] = {samplerState_.get()};
      context_->PSSetShaderResources(0, 1, views);
      context_->PSSetSamplers(0, 1, samplers);
    } else {
      context_->PSSetShader(pixelShader_.get(), nullptr, 0);
    }
    context_->RSSetState(scissorRasterizerState_.get());
    setScissorFromRect(rect);
    context_->Draw(3, 0);
    context_->RSSetState(rasterizerState_.get());
    if (textured) {
      ID3D11ShaderResourceView* nullViews[] = {nullptr};
      context_->PSSetShaderResources(0, 1, nullViews);
      context_->PSSetShader(pixelShader_.get(), nullptr, 0);
    }

    // --- Item 8: border pass around the full layer rect. ---
    const auto border = compositor::computeBorderFraming(
        layer.plan.borderStyle, layer.plan.borderColor, layer.plan.borderThickness);
    drawBorderPass(layer, renderPlan, rect, border, layerAlpha);
  }

  // --- Item 9: overlay/lower-third/caption raster stage. ---
  // Rasters the overlay's real content (DirectWrite text + WIC image on Windows)
  // into a texture, then composites it with the animated keyPhase transform.
  // NEEDS WINDOWS SMOKE: the DirectWrite/D2D/WIC text+image raster is currently
  // a deterministic GPU-side band + accent draw (so the build stays portable and
  // the math matches the CPU stub); wiring the real DirectWrite/WIC rasterizer
  // into `overlayTexture_` is the remaining Windows-only step (see
  // rasterOverlayTexture()). The animated transform/alpha and brand styling are
  // applied here and are validated headless against the CPU mirror.
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
    if (rasterOverlayTexture(overlay, animated)) {
      setViewportFromRect(animated);
      if (writeLayerConstants(layer, renderPlan, 0xffffffffu, alpha, 1.f, 1.f, 0.f, 0.f)) {
        context_->PSSetShader(texturedPixelShader_.get(), nullptr, 0);
        ID3D11ShaderResourceView* views[] = {overlayTextureView_.get()};
        ID3D11SamplerState* samplers[] = {samplerState_.get()};
        context_->PSSetShaderResources(0, 1, views);
        context_->PSSetSamplers(0, 1, samplers);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* nullViews[] = {nullptr};
        context_->PSSetShaderResources(0, 1, nullViews);
        context_->PSSetShader(pixelShader_.get(), nullptr, 0);
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

  // Rasters overlay text/image into `overlayTexture_` using DirectWrite/D2D +
  // WIC. Returns true when a texture was produced. Currently returns false
  // (portable build): the real Windows rasterizer is the remaining smoke-pass
  // step. Kept as a seam so drawOverlayLayer() composites it transparently once
  // implemented.
  bool rasterOverlayTexture(const CompositorOverlayContent& /*overlay*/, const compositor::LayerRect& /*rect*/) {
    return false;
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
      if (FAILED(device_->CreateTexture2D(&textureDesc, nullptr, layerTexture_.put()))) {
        layerTextureWidth_ = 0;
        layerTextureHeight_ = 0;
        return false;
      }
      if (FAILED(device_->CreateShaderResourceView(layerTexture_.get(), nullptr, layerTextureView_.put()))) {
        layerTexture_ = {};
        layerTextureWidth_ = 0;
        layerTextureHeight_ = 0;
        return false;
      }
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(layerTexture_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      return false;
    }
    const auto* source = frame.pixels->data();
    const auto sourceStride = static_cast<size_t>(frame.pixelStride);
    auto* destination = static_cast<uint8_t*>(mapped.pData);
    const size_t rowBytes = static_cast<size_t>(width) * 4u;
    for (int y = 0; y < height; ++y) {
      std::memcpy(destination + static_cast<size_t>(y) * mapped.RowPitch, source + static_cast<size_t>(y) * sourceStride, rowBytes);
    }
    context_->Unmap(layerTexture_.get(), 0);
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
  // context. Keyed-mutex textures are DEFAULT usage, so upload via UpdateSubresource
  // (not Map). Producer acquires key 0 / writes / releases key 1; non-blocking so a
  // busy consumer just keeps the last frame.
  void exportParticipantTextures(const std::vector<VideoFrame>& frames, ProgramFrame& outFrame) {
    if (!device_ || !context_) {
      return;
    }
    for (const auto& f : frames) {
      if (f.participantId.empty() || !f.hasPixels()) {
        continue;
      }
      auto& pt = participantTextures_[f.participantId];
      if (!pt.texture || pt.width != f.pixelWidth || pt.height != f.pixelHeight) {
        pt = ParticipantTex{};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(f.pixelWidth);
        desc.Height = static_cast<UINT>(f.pixelHeight);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtrLite<IDXGIResource> dxgiResource;
        HANDLE handle = nullptr;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, pt.texture.put())) ||
            FAILED(pt.texture->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(dxgiResource.put()))) ||
            FAILED(dxgiResource->GetSharedHandle(&handle)) || !handle ||
            FAILED(pt.texture->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(pt.mutex.put())))) {
          participantTextures_.erase(f.participantId);
          continue;
        }
        pt.handle = handle;
        pt.width = f.pixelWidth;
        pt.height = f.pixelHeight;
      }
      if (pt.mutex && pt.mutex->AcquireSync(0, 0) == S_OK) {
        context_->UpdateSubresource(pt.texture.get(), 0, nullptr, f.pixels->data(),
                                    static_cast<UINT>(f.pixelStride), 0);
        pt.mutex->ReleaseSync(1);
      }
      ParticipantSharedTexture info;
      info.participantId = f.participantId;
      info.sharedHandleHex = handleToHex(pt.handle);
      info.width = pt.width;
      info.height = pt.height;
      info.frameNumber = outFrame.frameNumber;
      outFrame.participantSharedTextures.push_back(std::move(info));
    }
    // Drop textures for participants that are no longer delivering pixels.
    for (auto it = participantTextures_.begin(); it != participantTextures_.end();) {
      bool present = false;
      for (const auto& f : frames) {
        if (f.participantId == it->first && f.hasPixels()) {
          present = true;
          break;
        }
      }
      it = present ? std::next(it) : participantTextures_.erase(it);
    }
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

  ComPtrLite<ID3D11Device> device_;
  ComPtrLite<ID3D11DeviceContext> context_;
  ComPtrLite<ID3D11VertexShader> vertexShader_;
  ComPtrLite<ID3D11PixelShader> pixelShader_;
  ComPtrLite<ID3D11PixelShader> texturedPixelShader_;
  ComPtrLite<ID3D11SamplerState> samplerState_;
  ComPtrLite<ID3D11Buffer> constantBuffer_;
  ComPtrLite<ID3D11BlendState> blendState_;
  ComPtrLite<ID3D11RasterizerState> rasterizerState_;
  ComPtrLite<ID3D11RasterizerState> scissorRasterizerState_;
  ComPtrLite<ID3D11Texture2D> renderTarget_;
  ComPtrLite<ID3D11RenderTargetView> renderTargetView_;
  ComPtrLite<ID3D11Texture2D> stagingTexture_;
  ComPtrLite<ID3D11Texture2D> sharedTexture_;
  ComPtrLite<IDXGIKeyedMutex> sharedKeyedMutex_;
  // Per-participant keyed-mutex shared textures for the GPU multiview tiles.
  struct ParticipantTex {
    ComPtrLite<ID3D11Texture2D> texture;
    ComPtrLite<IDXGIKeyedMutex> mutex;
    HANDLE handle = nullptr;
    int width = 0;
    int height = 0;
  };
  std::map<std::string, ParticipantTex> participantTextures_;
  ComPtrLite<ID3D11Texture2D> layerTexture_;
  ComPtrLite<ID3D11ShaderResourceView> layerTextureView_;
  ComPtrLite<ID3D11Texture2D> overlayTexture_;
  ComPtrLite<ID3D11ShaderResourceView> overlayTextureView_;
  int layerTextureWidth_ = 0;
  int layerTextureHeight_ = 0;
  HANDLE sharedHandle_ = nullptr;
  int sharedWidth_ = 0;
  int sharedHeight_ = 0;
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
