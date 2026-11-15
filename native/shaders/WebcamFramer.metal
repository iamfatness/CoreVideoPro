// macOS OH Framer (roadmap M9.2 Task 9): a line-for-line MSL port of
// WebcamFramer.hlsl. Keep the two files in step; the Windows oracle values in
// GpuWebcamFramerTest.cpp are the bar for both. Compiled at runtime from a
// generated source string (CMake embeds this file), never as a .metallib.
#include <metal_stdlib>
using namespace metal;

struct Dimensions {
  uint width; uint height; uint words; uint mirror;
  uint meterShow; uint meterReady; int meterTenths; uint reserved;
};
// Fixed 5x7 glyphs: digits, P G M L U F S minus I N. No dynamic text textures.
constant uint font[140] = {
  14,17,19,21,25,17,14, 4,12,4,4,4,4,14,
  14,17,1,2,4,8,31, 30,1,1,14,1,1,30,
  2,6,10,18,31,2,2, 31,16,16,30,1,1,30,
  14,16,16,30,17,17,14, 31,1,2,4,8,8,8,
  14,17,17,14,17,17,14, 14,17,17,15,1,1,14,
  30,17,17,30,16,16,16, 14,17,16,23,17,17,15,
  17,27,21,21,17,17,17, 16,16,16,16,16,16,31,
  17,17,17,17,17,17,14, 31,16,16,30,16,16,16,
  15,16,16,14,1,1,30, 0,0,0,31,0,0,0,
  14,4,4,4,4,4,14, 17,25,21,19,17,17,17
};
constant uint label[10]={10,11,12,99,13,14,15,16,17,16};
static bool glyph(uint code, uint x, uint y) {
  if(code>=20 || x>=5 || y>=7) return false;
  return (font[code*7+y] & (1u << (4-x))) != 0;
}
static float4 meterPixel(uint x, uint y, constant Dimensions& d) {
  uint px=x-32, py=y-932;
  float4 color=float4(0,0,0,.85);
  if (px>=12 && px<132 && py>=10 && py<24) {
    uint cell=(px-12)/12;
    if(glyph(label[cell], ((px-12)%12)/2, (py-10)/2)) color=float4(1,1,1,1);
  }
  if(px>=12 && px<162 && py>=34 && py<69) {
    uint cell=(px-12)/30, gx=((px-12)%30)/5, gy=(py-34)/5;
    uint code=99;
    if(d.meterReady==0) { if(cell<2) code=17; }
    else if(d.meterTenths<=-1190) {
      if(cell==0) code=17; if(cell==1) code=18; if(cell==2) code=19; if(cell==3) code=15;
    } else {
      uint number=(uint)abs(d.meterTenths);
      if(cell==0 && d.meterTenths<0) code=17;
      if(cell==1) code=(number/100)%10;
      if(cell==2) code=(number/10)%10;
      if(cell==4) code=number%10;
      if(cell==3 && gx==1 && gy==6) color=float4(1,1,1,1);
    }
    if(glyph(code,gx,gy)) color=float4(1,1,1,1);
  }
  if(px>=12 && px<348 && py>=88 && py<100) {
    color=float4(.16,.19,.22,1);
    float filled=saturate((d.meterTenths+600)/600.0)*336;
    if(d.meterReady!=0 && px-12<filled) color=float4(.12,.75,.95,1);
  }
  return color;
}
static float4 pixel(uint x, uint y, constant Dimensions& d, texture2d<float, access::read> overlay) {
  if (d.mirror != 0) x = d.width - 1 - x;
  uint ox=min(1919u,x*1920/d.width), oy=min(1079u,y*1080/d.height);
  if(d.meterShow!=0 && ox>=32 && ox<392 && oy>=932 && oy<1040) return meterPixel(ox,oy,d);
  return overlay.read(uint2(ox,oy));
}
static float3 yuv(float3 rgb) {
  return float3(16 + 219 * dot(rgb, float3(.2126,.7152,.0722)),
    128 + 224 * dot(rgb, float3(-.114572,-.385428,.5)),
    128 + 224 * dot(rgb, float3(.5,-.454153,-.045847)));
}
static uint blend(uint byteIndex, uint original, constant Dimensions& d,
                  texture2d<float, access::read> overlay) {
  uint planeSize = d.width * d.height;
  if (byteIndex < planeSize) {
    float4 p = pixel(byteIndex % d.width, byteIndex / d.width, d, overlay);
    if (p.a == 0) return original;
    return (uint)clamp(floor(mix((float)original, yuv(p.rgb).x, p.a) + .5), 0.0, 255.0);
  }
  uint uv = byteIndex - planeSize;
  uint x = (uv % d.width) & ~1u, y = (uv / d.width) * 2;
  float alpha = 0, contribution = 0;
  for (uint dy=0; dy<2; ++dy) for(uint dx=0; dx<2; ++dx) {
    float4 p = pixel(x + dx, y + dy, d, overlay);
    alpha += p.a * .25;
    contribution += ((uv & 1) == 0 ? yuv(p.rgb).y : yuv(p.rgb).z) * p.a * .25;
  }
  if (alpha == 0) return original;
  return (uint)clamp(floor(original * (1-alpha) + contribution + .5), 0.0, 255.0);
}
// One thread per 32-bit word (4 NV12 bytes), as the HLSL. Dispatched 1-D with
// dispatchThreads, so the HLSL's 65535-group y-split is not needed.
kernel void webcamFramer(device const uint* source [[buffer(0)]],
                         device uint* result [[buffer(1)]],
                         constant Dimensions& d [[buffer(2)]],
                         texture2d<float, access::read> overlay [[texture(0)]],
                         uint word [[thread_position_in_grid]]) {
  if(word >= d.words) return;
  uint input = source[word], output = 0;
  for(uint b=0; b<4; ++b)
    output |= blend(word*4+b, (input >> (8*b)) & 255, d, overlay) << (8*b);
  result[word] = output;
}
