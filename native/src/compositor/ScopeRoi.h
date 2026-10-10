#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace corevideo::compositor {
struct ScopeRoiPixels { int x=0, y=0, width=0, height=0; };
struct ScopeRoi {
  bool enabled=false;
  double x=0, y=0, width=1, height=1;
  int64_t revision=0;
  bool valid() const {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(width) && std::isfinite(height) &&
      x>=0 && y>=0 && width>0 && height>0 && x+width<=1.000000001 && y+height<=1.000000001 && revision>=0;
  }
  bool operator==(const ScopeRoi& r) const {
    return enabled==r.enabled && x==r.x && y==r.y && width==r.width && height==r.height && revision==r.revision;
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
