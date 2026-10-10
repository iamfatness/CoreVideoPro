#pragma once
#include "compositor/CompositorShaders.h"
#include "modules/D3DDecoupledExport.h"
#include <sstream>

namespace corevideo::modules {
// Worker-owned optional analysis: GPU bins -> GPU scope image -> async export.
// No staging map, source readback, keyed-mutex wait, or production-context work.
inline constexpr char kGradeScopeCompute[] = R"(
Texture2D<float4> source : register(t0);
RWStructuredBuffer<uint> bins : register(u0);
cbuffer ScopeConstants : register(b0) { float4 modes; };
[numthreads(16,16,1)] void main(uint3 p : SV_DispatchThreadID) {
  if (p.x>=256 || p.y>=144) return;
  float2 region=(float2(p.xy)+.5)/float2(256,144)*2-1;
  if (modes.w>.5 && dot(region,region)>1) return;
  float3 rgb=saturate(source.Load(int3(p.xy,0)).rgb);
  float y=dot(rgb,float3(.2126,.7152,.0722));
  uint4 h=(uint4)(float4(rgb,y)*255);
  InterlockedAdd(bins[h.r],1); InterlockedAdd(bins[256+h.g],1);
  InterlockedAdd(bins[512+h.b],1); InterlockedAdd(bins[768+h.a],1);
  uint x=p.x; uint4 v=(uint4)(float4(rgb,y)*255);
  InterlockedAdd(bins[1024+x+256*v.r],1); InterlockedAdd(bins[66560+x+256*v.g],1);
  InterlockedAdd(bins[132096+x+256*v.b],1); InterlockedAdd(bins[197632+x+256*v.a],1);
  float2 c=saturate(float2((rgb.b-y)/1.8556,(rgb.r-y)/1.5748)+.5);
  uint2 bin=(uint2)(c*255); InterlockedAdd(bins[263168+bin.x+256*bin.y],1);
}
)";
inline constexpr char kGradeScopePeak[] = R"(
RWStructuredBuffer<uint> bins : register(u0);
[numthreads(256,1,1)] void main(uint3 p : SV_DispatchThreadID) {
  for(uint channel=0;channel<4;++channel) InterlockedMax(bins[328704+channel],bins[channel*256+p.x]);
}
)";
inline constexpr char kGradeScopePixel[] = R"(
StructuredBuffer<uint> bins : register(t0);
cbuffer ScopeConstants : register(b0) { float4 modes; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
  uint panel=min((uint)(uv.x*3),2); float x=frac(uv.x*3), y=1-uv.y;
  if(modes.z>.5) { panel=(uint)modes.z-1; x=uv.x; if(panel==2) { x=(uv.x-.5)*3+.5; if(x<0 || x>1) return float4(.012,.012,.012,1); } }
  float3 rgb=float3(.012,.012,.012);
  if (panel==0) {
    uint bin=min((uint)(x*256),255); float4 peak=max(1,float4(bins[328704],bins[328705],bins[328706],bins[328707]));
    peak.rgb=max(peak.r,max(peak.g,peak.b));
    float4 h=float4(bins[bin],bins[256+bin],bins[512+bin],bins[768+bin])/peak;
    if(modes.x<.5) rgb+=y<h.a ? .8 : 0;
    else rgb+=float3(y<h.r,y<h.g,y<h.b)*.75;
  } else if(panel==1) {
    uint bx=min((uint)(x*256),255), by=min((uint)(y*256),255);
    if(modes.y<.5) rgb+=saturate(log2(1+float(bins[197632+bx+256*by]))/5);
    else if(modes.y<1.5) rgb+=saturate(log2(1+float3(bins[1024+bx+256*by],bins[66560+bx+256*by],bins[132096+bx+256*by]))/5);
    else {
      uint c=min((uint)(x*3),2); bx=min((uint)(frac(x*3)*256),255);
      float v=saturate(log2(1+float(bins[1024+c*65536+bx+256*by]))/5);
      rgb+=float3(c==0,c==1,c==2)*v;
    }
  } else {
    uint bx=min((uint)(x*256),255),by=min((uint)(y*256),255);
    rgb+=float3(.35,1,.65)*saturate(log2(1+float(bins[263168+bx+256*by]))/6);
    float2 c=float2(x-.5,y-.5); float r=length(c);
    if(abs(r-.375)<.003 || abs(r-.25)<.002 || abs(c.x)<.002 || abs(c.y)<.002) rgb=max(rgb,.12);
    if(c.x<0 && abs(c.y+c.x*1.54)<.003) rgb=max(rgb,float3(.4,.25,.17));
    for(uint t=0;t<6;++t) {
      float3 patch=t==0 ? float3(.75,0,0) : t==1 ? float3(.75,.75,0) : t==2 ? float3(0,.75,0) : t==3 ? float3(0,.75,.75) : t==4 ? float3(0,0,.75) : float3(.75,0,.75);
      float l=dot(patch,float3(.2126,.7152,.0722)); float2 target=float2((patch.b-l)/1.8556,(patch.r-l)/1.5748);
      float2 d=abs(c-target); if(max(d.x,d.y)<.014 && max(d.x,d.y)>.009) rgb=max(rgb,patch+.15);
    }
  }
  if(panel<2 && (frac(x*4)<.005 || frac(y*4)<.005)) rgb=max(rgb,.09);
  return float4(saturate(rgb),1);
}
)";
class D3DGradeScopes {
 public:
  bool initialize(ID3D11Device* device) {
    std::string error;
    auto cs=compileShader(kGradeScopeCompute,"main","cs_5_0",error);
    auto peak=compileShader(kGradeScopePeak,"main","cs_5_0",error);
    auto ps=compileShader(kGradeScopePixel,"main","ps_5_0",error);
    if(!cs || !peak || !ps || FAILED(device->CreateComputeShader(cs->GetBufferPointer(),cs->GetBufferSize(),nullptr,compute_.put())) ||
       FAILED(device->CreateComputeShader(peak->GetBufferPointer(),peak->GetBufferSize(),nullptr,peak_.put())) ||
       FAILED(device->CreatePixelShader(ps->GetBufferPointer(),ps->GetBufferSize(),nullptr,pixel_.put()))) return false;
    D3D11_BUFFER_DESC bd{}; bd.ByteWidth=328708*4; bd.Usage=D3D11_USAGE_DEFAULT;
    bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE; bd.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride=4;
    if(FAILED(device->CreateBuffer(&bd,nullptr,bins_.put())) || FAILED(device->CreateUnorderedAccessView(bins_.get(),nullptr,uav_.put())) ||
       FAILED(device->CreateShaderResourceView(bins_.get(),nullptr,srv_.put()))) return false;
    bd={}; bd.ByteWidth=16; bd.Usage=D3D11_USAGE_DYNAMIC; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
    if(FAILED(device->CreateBuffer(&bd,nullptr,constants_.put()))) return false;
    D3D11_TEXTURE2D_DESC td{}; td.Width=1536; td.Height=512; td.MipLevels=td.ArraySize=1; td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_B8G8R8A8_UNORM; td.BindFlags=D3D11_BIND_RENDER_TARGET;
    if(FAILED(device->CreateTexture2D(&td,nullptr,texture_.put())) || FAILED(device->CreateRenderTargetView(texture_.get(),nullptr,rtv_.put()))) return false;
    exporter_=std::make_unique<D3DDecoupledExport>(device,1536,512,"grade-scopes",D3DDecoupledExport::Creation::Deferred); return true;
  }
  bool render(ID3D11DeviceContext* context,ID3D11ShaderResourceView* input,ID3D11VertexShader* vertex,int histogram,int waveform,int scopeView,int64_t token,bool circle=false) {
    if(!exporter_ || !exporter_->valid()) return false;
    if(!exporter_->ready()) return true; // Deferred creation is preparing, not failure.
    D3D11_MAPPED_SUBRESOURCE mapped{}; if(FAILED(context->Map(constants_.get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) return false;
    const float modes[4]={float(histogram),float(waveform),float(scopeView),circle?1.f:0.f}; memcpy(mapped.pData,modes,16); context->Unmap(constants_.get(),0);
    ID3D11Buffer* cb[]={constants_.get()}; context->CSSetConstantBuffers(0,1,cb);
    const UINT zero[4]{}; context->ClearUnorderedAccessViewUint(uav_.get(),zero);
    ID3D11UnorderedAccessView* outputs[]={uav_.get()}; ID3D11ShaderResourceView* inputs[]={input};
    context->CSSetShader(compute_.get(),nullptr,0); context->CSSetUnorderedAccessViews(0,1,outputs,nullptr); context->CSSetShaderResources(0,1,inputs);
    context->Dispatch(16,9,1);
    context->CSSetShader(peak_.get(),nullptr,0);context->Dispatch(1,1,1);
    ID3D11UnorderedAccessView* noUav[]={nullptr}; ID3D11ShaderResourceView* noSrv[]={nullptr};
    context->CSSetUnorderedAccessViews(0,1,noUav,nullptr); context->CSSetShaderResources(0,1,noSrv); context->CSSetShader(nullptr,nullptr,0);
    context->PSSetConstantBuffers(0,1,cb);
    ID3D11RenderTargetView* rt[]={rtv_.get()}; context->OMSetRenderTargets(1,rt,nullptr); context->OMSetBlendState(nullptr,nullptr,0xffffffff);
    D3D11_VIEWPORT viewport{}; viewport.Width=1536; viewport.Height=512; viewport.MaxDepth=1; context->RSSetViewports(1,&viewport);
    context->VSSetShader(vertex,nullptr,0); context->PSSetShader(pixel_.get(),nullptr,0);
    inputs[0]=srv_.get(); context->PSSetShaderResources(0,1,inputs); context->Draw(3,0);
    ID3D11RenderTargetView* noRt[]={nullptr}; context->OMSetRenderTargets(1,noRt,nullptr); context->PSSetShaderResources(0,1,noSrv);
    exporter_->submit(context,texture_.get(),token); return true;
  }
  ParticipantSharedTexture published() const {
    ParticipantSharedTexture out;
    if(!exporter_ || !exporter_->ready()) return out;
    out.frameNumber=exporter_->publishedFrameNumber()->load(std::memory_order_acquire); if(out.frameNumber<0) return {};
    out.width=1536; out.height=512; out.format="B8G8R8A8_UNORM";
    std::ostringstream handle; handle<<"0x"<<std::hex<<reinterpret_cast<uintptr_t>(exporter_->handle()); out.sharedHandleHex=handle.str(); return out;
  }
 private:
  ComPtrLite<ID3D11ComputeShader> compute_,peak_; ComPtrLite<ID3D11PixelShader> pixel_;
  ComPtrLite<ID3D11Buffer> bins_,constants_; ComPtrLite<ID3D11UnorderedAccessView> uav_;
  ComPtrLite<ID3D11ShaderResourceView> srv_; ComPtrLite<ID3D11Texture2D> texture_;
  ComPtrLite<ID3D11RenderTargetView> rtv_; std::unique_ptr<D3DDecoupledExport> exporter_;
};
}
