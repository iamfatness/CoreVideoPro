#include "compositor/D3D11BackdropBlur.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace corevideo::modules {
namespace {

// The full-screen triangle every compositor pass uses (kCompositorVertexShader).
constexpr char kBackdropVertexShader[] = R"(
float4 main(uint vid : SV_VertexID) : SV_Position {
  float2 uv = float2((vid << 1) & 2, vid & 2);
  return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
)";

// One source for the three stages; each entry point is compiled separately.
// Every expression mirrors compositor::backdropReference (BackdropBlur.h) and
// the Metal kernels in MetalBackdropBlur.mm: integer loads, explicit clamps,
// the same summation order.
constexpr char kBackdropPixelShaders[] = R"(
cbuffer Backdrop : register(b0) {
  int4 source;     // origin.xy, max.xy
  int4 low;        // lowWidth, lowHeight, downsample, radius
  int4 stepv;      // blur direction
  float4 weights[4];
  float4 plate;    // x0, y0, x1, y1 (target px)
  float4 tint;     // straight rgba
  float4 place;    // region x0, y0; radiusPx; opacity
};
Texture2D<float4> src : register(t0);

float weightAt(int k) { return weights[k >> 2][k & 3]; }

float4 downsample(float4 pos : SV_Position) : SV_Target {
  int2 cell = int2(pos.xy);
  int d = low.z;
  float3 sum = float3(0, 0, 0);
  for (int v = 0; v < d; ++v) {
    for (int u = 0; u < d; ++u) {
      int2 p = clamp(source.xy + cell * d + int2(u, v), int2(0, 0), source.zw);
      sum += src.Load(int3(p, 0)).rgb;
    }
  }
  return float4(sum / float(d * d), 1.0);
}

float4 blur(float4 pos : SV_Position) : SV_Target {
  int2 cell = int2(pos.xy);
  int r = low.w;
  float3 sum = float3(0, 0, 0);
  for (int k = -r; k <= r; ++k) {
    int2 p = clamp(cell + stepv.xy * k, int2(0, 0), low.xy - 1);
    sum += weightAt(abs(k)) * src.Load(int3(p, 0)).rgb;
  }
  return float4(sum, 1.0);
}

float backdropCoverage(float2 p) {
  float2 c = (plate.xy + plate.zw) * 0.5;
  float2 h = (plate.zw - plate.xy) * 0.5;
  float2 q = abs(p - c) - h + place.z;
  float sdf = length(max(q, float2(0, 0))) + min(max(q.x, q.y), 0.0) - place.z;
  return saturate(0.5 - sdf);
}

float4 composite(float4 pos : SV_Position) : SV_Target {
  float alpha = backdropCoverage(pos.xy) * place.w;
  if (alpha <= 0.0) discard;
  float2 q = (pos.xy - place.xy) / float(low.z) - 0.5;
  float2 f0 = floor(q);
  float2 f = q - f0;
  int2 i0 = clamp(int2(f0), int2(0, 0), low.xy - 1);
  int2 i1 = clamp(int2(f0) + 1, int2(0, 0), low.xy - 1);
  float3 t00 = src.Load(int3(i0.x, i0.y, 0)).rgb;
  float3 t10 = src.Load(int3(i1.x, i0.y, 0)).rgb;
  float3 t01 = src.Load(int3(i0.x, i1.y, 0)).rgb;
  float3 t11 = src.Load(int3(i1.x, i1.y, 0)).rgb;
  float3 top = t00 + (t10 - t00) * f.x;
  float3 bottom = t01 + (t11 - t01) * f.x;
  float3 value = top + (bottom - top) * f.y;
  value = value * (1.0 - tint.a) + tint.rgb * tint.a;
  return float4(value, alpha);
}
)";

ComPtrLite<ID3DBlob> compile(const char* source, const char* entry, const char* profile, std::string& error) {
  ComPtrLite<ID3DBlob> blob, errors;
  if (FAILED(D3DCompile(source, std::strlen(source), "CoreVideoBackdropBlur", nullptr, nullptr, entry, profile,
                        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS, 0, blob.putBlob(), errors.putBlob()))) {
    error = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
                   : std::string("D3DCompile failed without diagnostics.");
    return {};
  }
  return blob;
}

}  // namespace

bool D3D11BackdropBlur::initialize(ID3D11Device* device, std::string& error) {
  ready_ = false;
  if (!device) { error = "no device"; return false; }
  const auto vs = compile(kBackdropVertexShader, "main", "vs_5_0", error);
  const auto down = vs ? compile(kBackdropPixelShaders, "downsample", "ps_5_0", error) : ComPtrLite<ID3DBlob>{};
  const auto blur = down ? compile(kBackdropPixelShaders, "blur", "ps_5_0", error) : ComPtrLite<ID3DBlob>{};
  const auto comp = blur ? compile(kBackdropPixelShaders, "composite", "ps_5_0", error) : ComPtrLite<ID3DBlob>{};
  if (!comp) return false;
  if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vertex_.put())) ||
      FAILED(device->CreatePixelShader(down->GetBufferPointer(), down->GetBufferSize(), nullptr, downsample_.put())) ||
      FAILED(device->CreatePixelShader(blur->GetBufferPointer(), blur->GetBufferSize(), nullptr, blur_.put())) ||
      FAILED(device->CreatePixelShader(comp->GetBufferPointer(), comp->GetBufferSize(), nullptr, composite_.put()))) {
    error = "backdrop shader creation failed"; return false;
  }
  D3D11_BUFFER_DESC cb{};
  cb.ByteWidth = sizeof(compositor::BackdropShaderConstants);
  cb.Usage = D3D11_USAGE_DYNAMIC; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(device->CreateBuffer(&cb, nullptr, constants_.put()))) { error = "backdrop constant buffer failed"; return false; }
  D3D11_BLEND_DESC blend{};
  blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  if (FAILED(device->CreateBlendState(&blend, opaque_.put()))) { error = "backdrop blend failed"; return false; }
  // The compositor's straight-alpha content blend (blendState_).
  blend.RenderTarget[0].BlendEnable = TRUE;
  blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  if (FAILED(device->CreateBlendState(&blend, straightAlpha_.put()))) { error = "backdrop blend failed"; return false; }
  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID; raster.CullMode = D3D11_CULL_NONE;
  if (FAILED(device->CreateRasterizerState(&raster, plain_.put()))) { error = "backdrop rasterizer failed"; return false; }
  raster.ScissorEnable = TRUE;
  if (FAILED(device->CreateRasterizerState(&raster, scissor_.put()))) { error = "backdrop rasterizer failed"; return false; }
  ready_ = true;
  return true;
}

bool D3D11BackdropBlur::reserve(ID3D11Device* device, int width, int height) {
  if (!ready_ || !device || width <= 0 || height <= 0) return false;
  if (copy_ && width <= capacityWidth_ && height <= capacityHeight_) return true;
  width = std::max(width, capacityWidth_); height = std::max(height, capacityHeight_);
  copy_ = {}; copyView_ = {}; lowA_ = {}; lowB_ = {}; lowAView_ = {}; lowBView_ = {}; lowATarget_ = {}; lowBTarget_ = {};
  capacityWidth_ = capacityHeight_ = 0;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = UINT(width); desc.Height = UINT(height); desc.MipLevels = desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, copy_.put())) ||
      FAILED(device->CreateShaderResourceView(copy_.get(), nullptr, copyView_.put()))) { copy_ = {}; return false; }
  // Every plan downsamples by at least 2 (kBackdropMinDownsample).
  desc.Width = UINT((width + 1) / 2); desc.Height = UINT((height + 1) / 2);
  desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, lowA_.put())) ||
      FAILED(device->CreateTexture2D(&desc, nullptr, lowB_.put())) ||
      FAILED(device->CreateShaderResourceView(lowA_.get(), nullptr, lowAView_.put())) ||
      FAILED(device->CreateShaderResourceView(lowB_.get(), nullptr, lowBView_.put())) ||
      FAILED(device->CreateRenderTargetView(lowA_.get(), nullptr, lowATarget_.put())) ||
      FAILED(device->CreateRenderTargetView(lowB_.get(), nullptr, lowBTarget_.put()))) { copy_ = {}; return false; }
  capacityWidth_ = width; capacityHeight_ = height;
  return true;
}

bool D3D11BackdropBlur::writeConstants(ID3D11DeviceContext* context, const compositor::BackdropShaderConstants& value) {
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(constants_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
  std::memcpy(mapped.pData, &value, sizeof(value));
  context->Unmap(constants_.get(), 0);
  return true;
}

bool D3D11BackdropBlur::apply(ID3D11DeviceContext* context, ID3D11Texture2D* targetTexture,
                              ID3D11RenderTargetView* targetView, int targetWidth, int targetHeight,
                              const compositor::BackdropDraw& draw) {
  const auto& region = draw.region;
  if (!ready_ || !copy_ || !context || !targetTexture || !targetView) return false;
  if (targetWidth > capacityWidth_ || targetHeight > capacityHeight_) return false;
  if (region.copyWidth <= 0 || region.copyHeight <= 0 || region.lowWidth <= 0 || region.lowHeight <= 0 ||
      region.lowWidth > (capacityWidth_ + 1) / 2 || region.lowHeight > (capacityHeight_ + 1) / 2 || draw.bounds.empty())
    return false;
  D3D11_TEXTURE2D_DESC desc{};
  targetTexture->GetDesc(&desc);
  if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.SampleDesc.Count != 1) return false;

  const D3D11_BOX box{UINT(region.x0), UINT(region.y0), 0, UINT(region.x0 + region.copyWidth),
                      UINT(region.y0 + region.copyHeight), 1};
  context->CopySubresourceRegion(copy_.get(), 0, 0, 0, 0, targetTexture, 0, &box);

  ID3D11ShaderResourceView* none[] = {nullptr};
  ID3D11Buffer* buffers[] = {constants_.get()};
  context->IASetInputLayout(nullptr);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vertex_.get(), nullptr, 0);
  context->RSSetState(plain_.get());
  context->OMSetBlendState(opaque_.get(), nullptr, 0xffffffffu);
  context->PSSetConstantBuffers(0, 1, buffers);
  const D3D11_VIEWPORT lowViewport{0.f, 0.f, float(region.lowWidth), float(region.lowHeight), 0.f, 1.f};
  context->RSSetViewports(1, &lowViewport);
  const auto stage = [&](ID3D11PixelShader* shader, ID3D11ShaderResourceView* input, ID3D11RenderTargetView* output,
                         const compositor::BackdropShaderConstants& constants) {
    context->PSSetShaderResources(0, 1, none);
    context->OMSetRenderTargets(1, &output, nullptr);
    if (!writeConstants(context, constants)) return false;
    context->PSSetShader(shader, nullptr, 0);
    context->PSSetShaderResources(0, 1, &input);
    context->Draw(3, 0);
    return true;
  };
  // The copy starts at the region origin: origin 0, last valid texel copy - 1.
  const bool ok =
      stage(downsample_.get(), copyView_.get(), lowATarget_.get(),
            compositor::backdropConstants(draw, 0, 0, region.copyWidth - 1, region.copyHeight - 1)) &&
      stage(blur_.get(), lowAView_.get(), lowBTarget_.get(), compositor::backdropConstants(draw, 0, 0, 0, 0, 1, 0)) &&
      stage(blur_.get(), lowBView_.get(), lowATarget_.get(), compositor::backdropConstants(draw, 0, 0, 0, 0, 0, 1));
  context->PSSetShaderResources(0, 1, none);
  context->OMSetRenderTargets(1, &targetView, nullptr);
  if (!ok) return false;
  const D3D11_VIEWPORT full{0.f, 0.f, float(targetWidth), float(targetHeight), 0.f, 1.f};
  context->RSSetViewports(1, &full);
  const D3D11_RECT scissor{draw.bounds.x0, draw.bounds.y0, draw.bounds.x1, draw.bounds.y1};
  context->RSSetState(scissor_.get());
  context->RSSetScissorRects(1, &scissor);
  context->OMSetBlendState(straightAlpha_.get(), nullptr, 0xffffffffu);
  if (!writeConstants(context, compositor::backdropConstants(draw, 0, 0, 0, 0))) return false;
  ID3D11ShaderResourceView* blurred[] = {lowAView_.get()};
  context->PSSetShader(composite_.get(), nullptr, 0);
  context->PSSetShaderResources(0, 1, blurred);
  context->Draw(3, 0);
  context->PSSetShaderResources(0, 1, none);
  return true;
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
