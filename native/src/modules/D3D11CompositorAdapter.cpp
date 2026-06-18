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
};

constexpr char kCompositorVertexShader[] = R"(
float4 main(uint vid : SV_VertexID) : SV_Position {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

constexpr char kCompositorPixelShader[] = R"(
cbuffer LayerConstants : register(b0) {
  float4 color;
  float exposure;
  float contrast;
  float saturation;
  float temperature;
};

float4 main(float4 pos : SV_Position) : SV_Target {
  float3 rgb = color.rgb;
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

uint32_t rgbaToSignature(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

CompositorRenderPlan sortedRenderPlan(CompositorRenderPlan renderPlan) {
  std::stable_sort(
      renderPlan.layers.begin(),
      renderPlan.layers.end(),
      [](const CompositorRenderPlanLayer& left, const CompositorRenderPlanLayer& right) {
        if (left.order != right.order) {
          return left.order < right.order;
        }
        if (left.layerId != right.layerId) {
          return left.layerId < right.layerId;
        }
        return left.sourceId < right.sourceId;
      });
  return renderPlan;
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
    const auto deterministicPlan = sortedRenderPlan(renderPlan);
    ProgramFrame frame;
    frame.width = deterministicPlan.width;
    frame.height = deterministicPlan.height;
    frame.layerCount = deterministicPlan.layers.empty() ? static_cast<int>(frames.size()) : static_cast<int>(deterministicPlan.layers.size());
    frame.frameNumber = frameNumber_;
    frame.renderPlanId = deterministicPlan.renderPlanId;
    frame.renderer = "d3d11";
    frame.health = deterministicPlan.warnings.empty() ? "live" : "degraded";

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
    frame.programPixelSignature = readProgramPixelSignature(deterministicPlan.width / 2, deterministicPlan.height / 2);
    frame.preview = readProgramFramePreview();
    exportSharedTexture(frame);
    context_->Flush();
    return frame;
  }

 private:
  struct ResolvedLayer {
    CompositorRenderPlanLayer plan;
    uint32_t color = 0xff808080;
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
          [](const CompositorRenderPlanLayer& layer) { return layer.kind != "overlay"; }));
      for (auto planLayer : renderPlan.layers) {
        ResolvedLayer layer;
        layer.plan = std::move(planLayer);
        if (layer.plan.rect.width <= 0.f || layer.plan.rect.height <= 0.f) {
          if (layer.plan.kind == "overlay") {
            const auto layout = layer.plan.layerId.find("lower") != std::string::npos ? compositor::lowerThirdOverlay()
                                                                                        : compositor::topRightOverlay();
            layer.plan.rect = {layout.x, layout.y, layout.width, layout.height};
          } else {
            const auto layout = compositor::gridCell((std::max)(1, videoLayerCount), videoIndex);
            layer.plan.rect = {layout.x, layout.y, layout.width, layout.height};
            ++videoIndex;
          }
        }
        if (layer.plan.kind == "overlay") {
          layer.color = 0xff2a3548;
        } else if (!layer.plan.participantId.empty()) {
          layer.color = compositor::colorFromParticipantId(layer.plan.participantId);
        } else if (videoIndex > 0 && videoIndex - 1 < static_cast<int>(frames.size())) {
          layer.color = compositor::colorFromParticipantId(frames[static_cast<size_t>(videoIndex - 1)].participantId);
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
      layers.push_back(std::move(layer));
    }
    return layers;
  }

  void drawLayer(const ResolvedLayer& layer, const CompositorRenderPlan& renderPlan) {
    const auto& rect = layer.plan.rect;
    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = rect.x * static_cast<float>(targetWidth_);
    viewport.TopLeftY = rect.y * static_cast<float>(targetHeight_);
    viewport.Width = rect.width * static_cast<float>(targetWidth_);
    viewport.Height = rect.height * static_cast<float>(targetHeight_);
    viewport.MinDepth = 0.f;
    viewport.MaxDepth = 1.f;
    context_->RSSetViewports(1, &viewport);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(constantBuffer_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      return;
    }

    auto* constants = static_cast<LayerShaderConstants*>(mapped.pData);
    constants->color[0] = static_cast<float>((layer.color >> 16) & 0xff) / 255.f;
    constants->color[1] = static_cast<float>((layer.color >> 8) & 0xff) / 255.f;
    constants->color[2] = static_cast<float>(layer.color & 0xff) / 255.f;
    constants->color[3] = layer.plan.opacity;
    constants->exposure = renderPlan.colorGrade.exposure * 0.1f;
    constants->contrast = renderPlan.colorGrade.contrast * 0.1f;
    constants->saturation = renderPlan.colorGrade.saturation * 0.1f;
    constants->temperature = renderPlan.colorGrade.temperature * 0.1f;
    context_->Unmap(constantBuffer_.get(), 0);
    ID3D11Buffer* buffers[] = {constantBuffer_.get()};
    context_->PSSetConstantBuffers(0, 1, buffers);

    context_->Draw(3, 0);
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
    textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
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

    context_->CopyResource(sharedTexture_.get(), renderTarget_.get());
    frame.sharedTexture.sharedHandleHex = handleToHex(sharedHandle_);
    frame.sharedTexture.width = targetWidth_;
    frame.sharedTexture.height = targetHeight_;
    frame.sharedTexture.format = "B8G8R8A8_UNORM";
    frame.sharedTexture.frameNumber = frame.frameNumber;
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
  ComPtrLite<ID3D11Buffer> constantBuffer_;
  ComPtrLite<ID3D11BlendState> blendState_;
  ComPtrLite<ID3D11RasterizerState> rasterizerState_;
  ComPtrLite<ID3D11Texture2D> renderTarget_;
  ComPtrLite<ID3D11RenderTargetView> renderTargetView_;
  ComPtrLite<ID3D11Texture2D> stagingTexture_;
  ComPtrLite<ID3D11Texture2D> sharedTexture_;
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
