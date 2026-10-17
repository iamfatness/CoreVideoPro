#pragma once
#include "compositor/LowerThirdAppearance.h"
#include <string>
namespace corevideo::graphics {
inline compositor::LowerThirdAppearance defaultAppearance(const std::string& templateId) {
  compositor::LowerThirdAppearance a;a.enabled=true;
  a.preset=templateId=="minimal-accent" || templateId=="broadcast" ? templateId : "compact-solid";
  if(templateId=="minimal-accent") {a.backgroundOpacity=0;a.nameSize=40;a.titleSize=26;a.cornerRadius=0;}
  else if(templateId=="broadcast") {a.nameSize=52;a.titleSize=32;a.padding=24;a.width=.75;a.cornerRadius=0;a.backgroundOpacity=1;}
  return a;
}
}
