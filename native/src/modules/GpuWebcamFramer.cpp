#include "modules/GpuWebcamFramer.h"
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cmath>
#if defined(_WIN32) && COREVIDEO_WITH_D3D11
#include <d3d11.h>
#include "WebcamFramerShader.h"
#include <wincodec.h>
#include <wrl/client.h>
#include "WebcamFramerAsset.h"
#endif

namespace corevideo::modules {
#if defined(_WIN32) && COREVIDEO_WITH_D3D11
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr, const char* operation) {
  if (FAILED(hr)) throw std::runtime_error(std::string("OH Framer: ") + operation + " failed (" + std::to_string(hr) + "). Clean video continues.");
}
// Work directly on packed NV12. Alpha-zero pixels retain the exact source byte.
// Chroma uses the mean of the four alpha-weighted overlay pixels in each 2x2 block.

}
struct GpuWebcamFramer::Impl {
  std::string error;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11ComputeShader> shader;
  ComPtr<ID3D11Texture2D> asset;
  ComPtr<ID3D11ShaderResourceView> assetView, inputView;
  ComPtr<ID3D11UnorderedAccessView> outputView;
  ComPtr<ID3D11Buffer> input, output, staging, constants;
  int width = 0, height = 0;
  std::shared_ptr<std::vector<uint8_t>> pixels;
  void initialize() {
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1,
      D3D11_SDK_VERSION, &device, &level, &context), "GPU device creation");
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
      IID_PPV_ARGS(&factory)), "PNG decoder creation");
    std::vector<BYTE> encoded(webcamFramerPng, webcamFramerPng + sizeof(webcamFramerPng));
    ComPtr<IWICStream> stream;
    check(factory->CreateStream(&stream), "PNG stream creation");
    check(stream->InitializeFromMemory(encoded.data(), static_cast<DWORD>(encoded.size())), "embedded PNG stream");
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder), "PNG decode");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "PNG frame");
    UINT w=0,h=0;
    check(frame->GetSize(&w,&h), "PNG dimensions");
    if(w != 1920 || h != 1080) throw std::runtime_error("OH Framer: invalid built-in dimensions.");
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter), "PNG converter");
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
      nullptr, 0, WICBitmapPaletteTypeCustom), "straight-alpha decode");
    std::vector<uint8_t> rgba(static_cast<size_t>(w)*h*4);
    check(converter->CopyPixels(nullptr, w*4, static_cast<UINT>(rgba.size()), rgba.data()), "PNG pixels");
    D3D11_TEXTURE2D_DESC td{};
    td.Width=w; td.Height=h; td.MipLevels=1; td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1;
    td.Usage=D3D11_USAGE_IMMUTABLE; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{rgba.data(), w*4, 0};
    check(device->CreateTexture2D(&td, &data, &asset), "overlay texture");
    check(device->CreateShaderResourceView(asset.Get(), nullptr, &assetView), "overlay view");
    check(device->CreateComputeShader(webcamFramerShaderBytecode, sizeof(webcamFramerShaderBytecode), nullptr, &shader), "GPU shader");
    D3D11_BUFFER_DESC cb{}; cb.ByteWidth=32; cb.Usage=D3D11_USAGE_DEFAULT; cb.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    check(device->CreateBuffer(&cb, nullptr, &constants), "dimensions buffer");
  }
  void resize(int w, int h) {
    if(width==w && height==h) return;
    inputView.Reset(); outputView.Reset(); input.Reset(); output.Reset(); staging.Reset();
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth=static_cast<UINT>(w*h*3/2); bd.Usage=D3D11_USAGE_DEFAULT;
    bd.BindFlags=D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride=4;
    check(device->CreateBuffer(&bd,nullptr,&input), "input buffer");
    check(device->CreateShaderResourceView(input.Get(),nullptr,&inputView), "input view");
    bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    check(device->CreateBuffer(&bd,nullptr,&output), "output buffer");
    check(device->CreateUnorderedAccessView(output.Get(),nullptr,&outputView), "output view");
    bd.Usage=D3D11_USAGE_STAGING; bd.BindFlags=0; bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    bd.MiscFlags=0; bd.StructureByteStride=0;
    check(device->CreateBuffer(&bd,nullptr,&staging), "readback buffer");
    width=w; height=h;
  }
};
#else
struct GpuWebcamFramer::Impl { std::string error = "OH Framer requires the Windows D3D11 build. Clean video continues."; };
#endif
GpuWebcamFramer::GpuWebcamFramer() : impl_(std::make_unique<Impl>()) {}
GpuWebcamFramer::~GpuWebcamFramer() = default;
const std::string& GpuWebcamFramer::warning() const { return impl_->error; }
bool GpuWebcamFramer::prepare(int w, int h) {
#if defined(_WIN32) && COREVIDEO_WITH_D3D11
  if (!impl_->error.empty()) return false;
  try {
    if (w<2 || h<2 || w>7680 || h>4320 || (w&1) || (h&1) || (static_cast<size_t>(w)*h*3/2)%4)
      throw std::runtime_error("OH Framer: invalid camera dimensions. Clean video continues.");
    if (!impl_->device) impl_->initialize();
    impl_->resize(w,h);
    const auto size=static_cast<size_t>(w)*h*3/2;
    if (!impl_->pixels || impl_->pixels->size()!=size) impl_->pixels=std::make_shared<std::vector<uint8_t>>(size);
    return true;
  } catch(const std::exception& e) { impl_->error=e.what(); return false; }
#else
  (void)w; (void)h; return false;
#endif
}
std::shared_ptr<const std::vector<uint8_t>> GpuWebcamFramer::apply(
    const std::shared_ptr<const std::vector<uint8_t>>& clean, int w, int h, bool mirror, WebcamLoudnessOverlay loudness) {
#if defined(_WIN32) && COREVIDEO_WITH_D3D11
  if(!impl_->error.empty()) return {};
  if(!clean || w<2 || h<2 || w>7680 || h>4320 || (w&1) || (h&1)
     || (static_cast<size_t>(w)*h*3/2)%4 || clean->size()!=static_cast<size_t>(w)*h*3/2) {
    impl_->error="OH Framer: invalid NV12 frame. Clean video continues.";
    return {};
  }
  try {
    if(!prepare(w,h)) return {};
    auto& s=*impl_;
    s.context->UpdateSubresource(s.input.Get(),0,nullptr,clean->data(),0,0);
    const bool ready=loudness.ready && std::isfinite(loudness.lufs);
    const int tenths=!ready ? -1200 : loudness.lufs<=-119 ? -1200 :
        static_cast<int>(std::round((std::max)(-99.9,(std::min)(99.9,loudness.lufs))*10));
    const UINT dims[] = {static_cast<UINT>(w),static_cast<UINT>(h),static_cast<UINT>(clean->size()/4),mirror?1u:0u,
        loudness.show?1u:0u,ready?1u:0u,static_cast<UINT>(tenths),0};
    s.context->UpdateSubresource(s.constants.Get(),0,nullptr,dims,0,0);
    ID3D11ShaderResourceView* views[]={s.inputView.Get(),s.assetView.Get()};
    ID3D11UnorderedAccessView* targets[]={s.outputView.Get()};
    ID3D11Buffer* cb[]={s.constants.Get()};
    s.context->CSSetShader(s.shader.Get(),nullptr,0);
    s.context->CSSetShaderResources(0,2,views); s.context->CSSetUnorderedAccessViews(0,1,targets,nullptr);
    s.context->CSSetConstantBuffers(0,1,cb);
    const UINT groups = (dims[2]+63)/64;
    s.context->Dispatch(groups > 65535 ? 65535 : groups, (groups+65534)/65535, 1);
    ID3D11UnorderedAccessView* emptyTargets[]={nullptr}; ID3D11ShaderResourceView* emptyViews[]={nullptr,nullptr};
    s.context->CSSetUnorderedAccessViews(0,1,emptyTargets,nullptr); s.context->CSSetShaderResources(0,2,emptyViews);
    s.context->CopyResource(s.staging.Get(),s.output.Get());
    // Reuse storage only when no consumer retains the previous immutable result.
    if (!s.pixels || s.pixels.use_count() != 1 || s.pixels->size() != clean->size())
      s.pixels=std::make_shared<std::vector<uint8_t>>(clean->size());
    auto result=s.pixels;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(s.context->Map(s.staging.Get(),0,D3D11_MAP_READ,0,&mapped), "GPU readback");
    std::memcpy(result->data(),mapped.pData,result->size());
    s.context->Unmap(s.staging.Get(),0);
    return result;
  } catch(const std::exception& e) { impl_->error=e.what(); return {}; }
#else
  (void)clean; (void)w; (void)h; (void)mirror; (void)loudness;
  return {};
#endif
}
}
