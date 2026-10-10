#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace corevideo::compositor {
struct ScopeRoiPixels { int x=0, y=0, width=0, height=0; };
struct ScopeRoi {
  bool enabled=false;
  double x=0, y=0, width=1, height=1;
  int64_t revision=0;
  std::string shape="rectangle";
  bool valid() const {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(width) && std::isfinite(height) &&
      x>=0 && y>=0 && width>0 && height>0 && x+width<=1.000000001 && y+height<=1.000000001 && revision>=0 && (shape=="rectangle" || shape=="circle");
  }
  bool operator==(const ScopeRoi& r) const {
    return enabled==r.enabled && x==r.x && y==r.y && width==r.width && height==r.height && revision==r.revision && shape==r.shape;
  }
  int sampleCount() const {
    if (!enabled || shape!="circle") return 256*144;
    static const int count=[] { int n=0; for(int y=0;y<144;++y) for(int x=0;x<256;++x) {
      const double nx=(x+.5)/256*2-1, ny=(y+.5)/144*2-1;
      if(nx*nx+ny*ny<=1) ++n;
    } return n; }();
    return count;
  }
  ScopeRoiPixels pixels(int w,int h) const {
    if (!valid() || w<=0 || h<=0) return {};
    if (!enabled) return {0,0,w,h};
    const int l=std::clamp(int(std::floor(x*w)),0,w-1), t=std::clamp(int(std::floor(y*h)),0,h-1);
    const int r=std::clamp(int(std::ceil((x+width)*w)),l+1,w), b=std::clamp(int(std::ceil((y+height)*h)),t+1,h);
    return {l,t,r-l,b-t};
  }
};
}
