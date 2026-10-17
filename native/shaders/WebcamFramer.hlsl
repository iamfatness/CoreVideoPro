StructuredBuffer<uint> source : register(t0);
Texture2D<float4> overlay : register(t1);
RWStructuredBuffer<uint> result : register(u0);
cbuffer Dimensions : register(b0) {
  uint width; uint height; uint words; uint mirror;
  uint meterShow; uint meterReady; int meterTenths; uint reserved;
};
// Fixed 5x7 glyphs: digits, P G M L U F S minus I N. No dynamic text textures.
static const uint font[140] = {
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
bool glyph(uint code, uint x, uint y) {
  if(code>=20 || x>=5 || y>=7) return false;
  return (font[code*7+y] & (1u << (4-x))) != 0;
}
float4 meterPixel(uint x, uint y) {
  uint px=x-32, py=y-932;
  float4 color=float4(0,0,0,.85);
  if (px>=12 && px<132 && py>=10 && py<24) {
    static const uint label[10]={10,11,12,99,13,14,15,16,17,16};
    uint cell=(px-12)/12;
    if(glyph(label[cell], ((px-12)%12)/2, (py-10)/2)) color=float4(1,1,1,1);
  }
  if(px>=12 && px<162 && py>=34 && py<69) {
    uint cell=(px-12)/30, gx=((px-12)%30)/5, gy=(py-34)/5;
    uint code=99;
    if(meterReady==0) { if(cell<2) code=17; }
    else if(meterTenths<=-1190) {
      if(cell==0) code=17; if(cell==1) code=18; if(cell==2) code=19; if(cell==3) code=15;
    } else {
      uint number=(uint)abs(meterTenths);
      if(cell==0 && meterTenths<0) code=17;
      if(cell==1) code=(number/100)%10;
      if(cell==2) code=(number/10)%10;
      if(cell==4) code=number%10;
      if(cell==3 && gx==1 && gy==6) color=float4(1,1,1,1);
    }
    if(glyph(code,gx,gy)) color=float4(1,1,1,1);
  }
  if(px>=12 && px<348 && py>=88 && py<100) {
    color=float4(.16,.19,.22,1);
    float filled=saturate((meterTenths+600)/600.0)*336;
    if(meterReady!=0 && px-12<filled) color=float4(.12,.75,.95,1);
  }
  return color;
}
float4 pixel(uint x, uint y) {
  if (mirror != 0) x = width - 1 - x;
  uint ox=min(1919u,x*1920/width), oy=min(1079u,y*1080/height);
  if(meterShow!=0 && ox>=32 && ox<392 && oy>=932 && oy<1040) return meterPixel(ox,oy);
  return overlay.Load(int3(ox,oy,0));
}
float3 yuv(float3 rgb) {
  return float3(16 + 219 * dot(rgb, float3(.2126,.7152,.0722)),
    128 + 224 * dot(rgb, float3(-.114572,-.385428,.5)),
    128 + 224 * dot(rgb, float3(.5,-.454153,-.045847)));
}
uint blend(uint byteIndex, uint original) {
  uint planeSize = width * height;
  if (byteIndex < planeSize) {
    float4 p = pixel(byteIndex % width, byteIndex / width);
    if (p.a == 0) return original;
    return (uint)clamp(floor(lerp((float)original, yuv(p.rgb).x, p.a) + .5), 0, 255);
  }
  uint uv = byteIndex - planeSize;
  uint x = (uv % width) & ~1u, y = (uv / width) * 2;
  float alpha = 0, contribution = 0;
  [unroll] for (uint dy=0; dy<2; ++dy) [unroll] for(uint dx=0; dx<2; ++dx) {
    float4 p = pixel(x + dx, y + dy);
    alpha += p.a * .25;
    contribution += ((uv & 1) == 0 ? yuv(p.rgb).y : yuv(p.rgb).z) * p.a * .25;
  }
  if (alpha == 0) return original;
  return (uint)clamp(floor(original * (1-alpha) + contribution + .5), 0, 255);
}
[numthreads(64,1,1)] void main(uint3 id : SV_DispatchThreadID) {
  uint word = id.x + id.y * (65535u * 64u);
  if(word >= words) return;
  uint input = source[word], output = 0;
  [unroll] for(uint b=0; b<4; ++b)
    output |= blend(word*4+b, (input >> (8*b)) & 255) << (8*b);
  result[word] = output;
}
