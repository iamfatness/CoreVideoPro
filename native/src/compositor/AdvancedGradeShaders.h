#pragma once
// A separate, cached resource block leaves the established 192-byte legacy
// layer constants intact. Curves are encoded Rec.709; exposure uses linear light.
namespace corevideo::modules {
inline constexpr char kAdvancedGradeHlsl[] = R"(
struct GradeOp { float4 meta; float4 primary; float4 balance; float4 domainMin; float4 domainMax; };
cbuffer AdvancedConstants : register(b1) { float4 gradeHeader; GradeOp gradeOps[8]; };
Texture2D<float2> gradeCurves : register(t3);
Texture3D<float4> gradeLuts[8] : register(t4);
SamplerState gradeLutSampler : register(s1);
float3 sampleCube(uint i,float3 uv) {
  if(i==0) return gradeLuts[0].SampleLevel(gradeLutSampler,uv,0).rgb;
  if(i==1) return gradeLuts[1].SampleLevel(gradeLutSampler,uv,0).rgb;
  if(i==2) return gradeLuts[2].SampleLevel(gradeLutSampler,uv,0).rgb;
  if(i==3) return gradeLuts[3].SampleLevel(gradeLutSampler,uv,0).rgb;
  if(i==4) return gradeLuts[4].SampleLevel(gradeLutSampler,uv,0).rgb;
  if(i==5) return gradeLuts[5].SampleLevel(gradeLutSampler,uv,0).rgb;
  if(i==6) return gradeLuts[6].SampleLevel(gradeLutSampler,uv,0).rgb;
  if(i==7) return gradeLuts[7].SampleLevel(gradeLutSampler,uv,0).rgb;
  return uv;
}
float3 rec709Linear(float3 v) { return float3(v.r < .081 ? v.r / 4.5 : pow((v.r+.099)/1.099, 1.0/.45), v.g < .081 ? v.g / 4.5 : pow((v.g+.099)/1.099, 1.0/.45), v.b < .081 ? v.b / 4.5 : pow((v.b+.099)/1.099, 1.0/.45)); }
float3 rec709Encode(float3 v) { return float3(v.r < .018 ? 4.5*v.r : 1.099*pow(v.r,.45)-.099, v.g < .018 ? 4.5*v.g : 1.099*pow(v.g,.45)-.099, v.b < .018 ? 4.5*v.b : 1.099*pow(v.b,.45)-.099); }
float curveValue(float x, uint row, uint channel) {
  x=saturate(x); uint y=row*4+channel; float2 a=gradeCurves.Load(int3(0,y,0));
  for(uint i=1;i<16;++i) { float2 b=gradeCurves.Load(int3(i,y,0));
    if(x<=b.x) return lerp(a.y,b.y,(x-a.x)/(b.x-a.x)); a=b;
  }
  return a.y;
}

float3 advancedGrade(float3 rgb,float3 original) {
  if (gradeHeader.z < .5) return rgb;
  for (uint i = 0; i < (uint)gradeHeader.x; ++i) {
    GradeOp o = gradeOps[i]; if (o.meta.y <= 0) continue;
    float3 v = rgb;
    if (o.meta.x < 1.5) {
      v = rec709Encode(rec709Linear(saturate(v))*exp2(o.primary.x));
      v = (v-o.primary.z)*o.primary.y+o.primary.z;
      float y = dot(v,float3(.2126,.7152,.0722)); v = lerp(y.xxx,v,o.primary.w);
      v += float3(o.balance.x*.1-o.balance.y*.05,o.balance.y*.1,-o.balance.x*.1-o.balance.y*.05);
      v = pow(max((v+o.balance.z)*o.meta.z,0),1.0/o.balance.w);
    } else if(o.meta.x > 2.5) {
      float3 uv=saturate((v-o.domainMin.xyz)/(o.domainMax.xyz-o.domainMin.xyz));
      uv=(uv*(o.meta.w-1)+.5)/o.meta.w; v=sampleCube(i,uv);
    } else {
      v = float3(curveValue(v.r,i,0),curveValue(v.g,i,0),curveValue(v.b,i,0));
      v = float3(curveValue(v.r,i,1),curveValue(v.g,i,2),curveValue(v.b,i,3));
    }
    rgb = lerp(rgb,saturate(v),o.meta.y);
  }
  return lerp(original,rgb,gradeHeader.y);
}
)";
}
