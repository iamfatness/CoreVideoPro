#pragma once
#include "rpc/Json.h"
#include <cmath>
#include <string>
namespace corevideo::compositor {
struct LowerThirdAppearance {
  int version=1;
  bool enabled=false, showLogo=true;
  std::string preset="compact-solid",anchor="lower-left",fontFamily="Segoe UI",nameColor="#F4F7FA",titleColor="#44C1A1";
  std::string backgroundColor="#0C1118",accentColor="#44C1A1";
  double nameSize=42,titleSize=28,backgroundOpacity=.9,padding=20,width=.6,cornerRadius=8,safeOffsetX=.05,safeOffsetY=.06,logoScale=1;
  static bool range(double n,double a,double b) { return std::isfinite(n) && n>=a && n<=b; }
  static bool color(const std::string& s) {
    if (s.size()!=7 || s[0]!='#') return false;
    for (size_t i=1;i<s.size();++i) if (!((s[i]>='0'&&s[i]<='9')||(s[i]>='a'&&s[i]<='f')||(s[i]>='A'&&s[i]<='F'))) return false;
    return true;
  }
  bool valid() const {
    return version==1 && (anchor=="lower-left"||anchor=="lower-right"||anchor=="upper-left"||anchor=="upper-right") && (preset=="compact-solid"||preset=="minimal-accent"||preset=="broadcast") && !fontFamily.empty() && fontFamily.size()<=128 &&
      range(nameSize,16,120)&&range(titleSize,12,80)&&range(backgroundOpacity,0,1)&&range(padding,0,64)&&range(width,.15,.95)&&
      range(cornerRadius,0,60)&&range(safeOffsetX,0,.25)&&range(safeOffsetY,0,.25)&&range(logoScale,.5,2)&&color(nameColor)&&color(titleColor)&&color(backgroundColor)&&color(accentColor);
  }
  static LowerThirdAppearance parse(const rpc::Json& j) {
    LowerThirdAppearance a; a.enabled=true;
    a.version=j.getNumber("version",1)==1?1:0;
    a.preset=j.getString("preset",a.preset); a.fontFamily=j.getString("fontFamily",a.fontFamily);
    a.anchor=j.getString("anchor",a.anchor);
    a.nameColor=j.getString("nameColor",a.nameColor); a.titleColor=j.getString("titleColor",a.titleColor);
    a.backgroundColor=j.getString("backgroundColor",a.backgroundColor);a.accentColor=j.getString("accentColor",a.accentColor);
    a.nameSize=j.getNumber("nameSize",a.nameSize); a.titleSize=j.getNumber("titleSize",a.titleSize);
    a.backgroundOpacity=j.getNumber("backgroundOpacity",a.backgroundOpacity); a.padding=j.getNumber("padding",a.padding);
    a.width=j.getNumber("width",a.width); a.cornerRadius=j.getNumber("cornerRadius",a.cornerRadius);
    a.safeOffsetX=j.getNumber("safeOffsetX",a.safeOffsetX); a.safeOffsetY=j.getNumber("safeOffsetY",a.safeOffsetY);
    a.logoScale=j.getNumber("logoScale",a.logoScale); a.showLogo=!j.get("showLogo")||j.get("showLogo")->asBool(); return a;
  }
};
}
