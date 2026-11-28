#include "compositor/MetalBackdropBlur.h"

#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL

#include <algorithm>

namespace corevideo::modules {
namespace {

// Every expression mirrors compositor::backdropReference (BackdropBlur.h) and
// the HLSL in D3D11BackdropBlur.cpp: integer reads, explicit clamps, the same
// summation order. The constant struct is BackdropShaderConstants (160 bytes).
constexpr char kBackdropMetalSource[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct BackdropConstants {
  int4 source;     // origin.xy, max.xy
  int4 low;        // lowWidth, lowHeight, downsample, radius
  int4 stepv;      // blur direction
  float4 weights[4];
  float4 plate;    // x0, y0, x1, y1 (target px)
  float4 tint;     // straight rgba
  float4 place;    // region x0, y0; radiusPx; opacity
};

static float weightAt(constant BackdropConstants& c, int k) { return c.weights[k >> 2][k & 3]; }

kernel void backdropDownsample(texture2d<float, access::read> src [[texture(0)]],
                               texture2d<float, access::write> dst [[texture(1)]],
                               constant BackdropConstants& c [[buffer(0)]],
                               uint2 gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= c.low.x || int(gid.y) >= c.low.y) return;
  int2 cell = int2(gid);
  int d = c.low.z;
  float3 sum = float3(0.0);
  for (int v = 0; v < d; ++v) {
    for (int u = 0; u < d; ++u) {
      int2 p = clamp(c.source.xy + cell * d + int2(u, v), int2(0), c.source.zw);
      sum += src.read(uint2(p)).rgb;
    }
  }
  dst.write(float4(sum / float(d * d), 1.0), gid);
}

kernel void backdropBlur(texture2d<float, access::read> src [[texture(0)]],
                         texture2d<float, access::write> dst [[texture(1)]],
                         constant BackdropConstants& c [[buffer(0)]],
                         uint2 gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= c.low.x || int(gid.y) >= c.low.y) return;
  int2 cell = int2(gid);
  int r = c.low.w;
  float3 sum = float3(0.0);
  for (int k = -r; k <= r; ++k) {
    int2 p = clamp(cell + c.stepv.xy * k, int2(0), c.low.xy - 1);
    sum += weightAt(c, abs(k)) * src.read(uint2(p)).rgb;
  }
  dst.write(float4(sum, 1.0), gid);
}

struct BackdropVertexOut { float4 position [[position]]; };

vertex BackdropVertexOut backdropVertex(uint vid [[vertex_id]]) {
  float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
  BackdropVertexOut out;
  out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return out;
}

static float backdropCoverage(constant BackdropConstants& c, float2 p) {
  float2 centre = (c.plate.xy + c.plate.zw) * 0.5;
  float2 h = (c.plate.zw - c.plate.xy) * 0.5;
  float2 q = abs(p - centre) - h + c.place.z;
  float sdf = length(max(q, float2(0.0))) + min(max(q.x, q.y), 0.0) - c.place.z;
  return saturate(0.5 - sdf);
}

fragment float4 backdropComposite(BackdropVertexOut in [[stage_in]],
                                  texture2d<float, access::read> low [[texture(0)]],
                                  constant BackdropConstants& c [[buffer(0)]]) {
  float alpha = backdropCoverage(c, in.position.xy) * c.place.w;
  if (alpha <= 0.0) discard_fragment();
  float2 q = (in.position.xy - c.place.xy) / float(c.low.z) - 0.5;
  float2 f0 = floor(q);
  float2 f = q - f0;
  int2 i0 = clamp(int2(f0), int2(0), c.low.xy - 1);
  int2 i1 = clamp(int2(f0) + 1, int2(0), c.low.xy - 1);
  float3 t00 = low.read(uint2(i0.x, i0.y)).rgb;
  float3 t10 = low.read(uint2(i1.x, i0.y)).rgb;
  float3 t01 = low.read(uint2(i0.x, i1.y)).rgb;
  float3 t11 = low.read(uint2(i1.x, i1.y)).rgb;
  float3 top = t00 + (t10 - t00) * f.x;
  float3 bottom = t01 + (t11 - t01) * f.x;
  float3 value = top + (bottom - top) * f.y;
  value = value * (1.0 - c.tint.a) + c.tint.rgb * c.tint.a;
  return float4(value, alpha);
}
)MSL";

}  // namespace

bool MetalBackdropBlur::initialize(id<MTLDevice> device, std::string& error) {
  ready_ = false;
  if (!device) { error = "no Metal device"; return false; }
  MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  options.fastMathEnabled = NO;  // match the reference's IEEE arithmetic (Windows compiles IEEE-strict)
#pragma clang diagnostic pop
  NSError* nsError = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:kBackdropMetalSource]
                                                options:options
                                                  error:&nsError];
  if (!library) {
    error = nsError ? nsError.localizedDescription.UTF8String : "backdrop library compile failed";
    return false;
  }
  id<MTLFunction> down = [library newFunctionWithName:@"backdropDownsample"];
  id<MTLFunction> blur = [library newFunctionWithName:@"backdropBlur"];
  id<MTLFunction> vertexFn = [library newFunctionWithName:@"backdropVertex"];
  id<MTLFunction> fragmentFn = [library newFunctionWithName:@"backdropComposite"];
  if (!down || !blur || !vertexFn || !fragmentFn) { error = "backdrop shader functions missing"; return false; }
  downsample_ = [device newComputePipelineStateWithFunction:down error:&nsError];
  blur_ = downsample_ ? [device newComputePipelineStateWithFunction:blur error:&nsError] : nil;
  if (!downsample_ || !blur_) {
    error = nsError ? nsError.localizedDescription.UTF8String : "backdrop compute pipeline failed";
    return false;
  }
  MTLRenderPipelineDescriptor* desc = [[MTLRenderPipelineDescriptor alloc] init];
  desc.vertexFunction = vertexFn;
  desc.fragmentFunction = fragmentFn;
  auto* color = desc.colorAttachments[0];
  color.pixelFormat = MTLPixelFormatBGRA8Unorm;
  // The compositor's straight-alpha content blend (MetalCompositorAdapter makePipeline).
  color.blendingEnabled = YES;
  color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
  color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  color.sourceAlphaBlendFactor = MTLBlendFactorOne;
  color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  composite_ = [device newRenderPipelineStateWithDescriptor:desc error:&nsError];
  if (!composite_) {
    error = nsError ? nsError.localizedDescription.UTF8String : "backdrop render pipeline failed";
    return false;
  }
  ready_ = true;
  return true;
}

bool MetalBackdropBlur::reserve(id<MTLDevice> device, int width, int height) {
  if (!ready_ || !device || width <= 0 || height <= 0) return false;
  if (lowA_ && width <= capacityWidth_ && height <= capacityHeight_) return true;
  width = std::max(width, capacityWidth_); height = std::max(height, capacityHeight_);
  lowA_ = nil; lowB_ = nil; capacityWidth_ = capacityHeight_ = 0;
  // Every plan downsamples by at least 2 (kBackdropMinDownsample).
  MTLTextureDescriptor* desc =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                                         width:NSUInteger((width + 1) / 2)
                                                        height:NSUInteger((height + 1) / 2)
                                                     mipmapped:NO];
  desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  desc.storageMode = MTLStorageModePrivate;
  lowA_ = [device newTextureWithDescriptor:desc];
  lowB_ = [device newTextureWithDescriptor:desc];
  if (!lowA_ || !lowB_) { lowA_ = nil; lowB_ = nil; return false; }
  capacityWidth_ = width; capacityHeight_ = height;
  return true;
}

bool MetalBackdropBlur::apply(id<MTLCommandBuffer> commandBuffer, id<MTLTexture> target,
                              const compositor::BackdropDraw& draw) {
  const auto& region = draw.region;
  if (!ready_ || !lowA_ || !commandBuffer || !target || draw.bounds.empty()) return false;
  const int targetWidth = int(target.width), targetHeight = int(target.height);
  if (target.pixelFormat != MTLPixelFormatBGRA8Unorm || !(target.usage & MTLTextureUsageShaderRead) ||
      targetWidth > capacityWidth_ || targetHeight > capacityHeight_)
    return false;
  if (region.lowWidth <= 0 || region.lowHeight <= 0 || region.lowWidth > int(lowA_.width) ||
      region.lowHeight > int(lowA_.height))
    return false;
  // Program is read in place: origin = region origin, max = last target texel.
  const auto down = compositor::backdropConstants(draw, region.x0, region.y0, targetWidth - 1, targetHeight - 1);
  const auto horizontal = compositor::backdropConstants(draw, 0, 0, 0, 0, 1, 0);
  const auto vertical = compositor::backdropConstants(draw, 0, 0, 0, 0, 0, 1);
  const auto place = compositor::backdropConstants(draw, 0, 0, 0, 0);
  const MTLSize group = MTLSizeMake(16, 16, 1);
  const MTLSize groups = MTLSizeMake(NSUInteger((region.lowWidth + 15) / 16), NSUInteger((region.lowHeight + 15) / 16), 1);
  id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
  if (!compute) return false;
  [compute setComputePipelineState:downsample_];
  [compute setTexture:target atIndex:0];
  [compute setTexture:lowA_ atIndex:1];
  [compute setBytes:&down length:sizeof(down) atIndex:0];
  [compute dispatchThreadgroups:groups threadsPerThreadgroup:group];
  [compute setComputePipelineState:blur_];
  [compute setTexture:lowA_ atIndex:0];
  [compute setTexture:lowB_ atIndex:1];
  [compute setBytes:&horizontal length:sizeof(horizontal) atIndex:0];
  [compute dispatchThreadgroups:groups threadsPerThreadgroup:group];
  [compute setTexture:lowB_ atIndex:0];
  [compute setTexture:lowA_ atIndex:1];
  [compute setBytes:&vertical length:sizeof(vertical) atIndex:0];
  [compute dispatchThreadgroups:groups threadsPerThreadgroup:group];
  [compute endEncoding];

  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> render = [commandBuffer renderCommandEncoderWithDescriptor:pass];
  if (!render) return false;
  MTLViewport viewport{0.0, 0.0, double(targetWidth), double(targetHeight), 0.0, 1.0};
  [render setViewport:viewport];
  MTLScissorRect scissor{NSUInteger(draw.bounds.x0), NSUInteger(draw.bounds.y0),
                         NSUInteger(draw.bounds.width()), NSUInteger(draw.bounds.height())};
  [render setScissorRect:scissor];
  [render setRenderPipelineState:composite_];
  [render setFragmentTexture:lowA_ atIndex:0];
  [render setFragmentBytes:&place length:sizeof(place) atIndex:0];
  [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [render endEncoding];
  return true;
}

}  // namespace corevideo::modules

#endif  // !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL
