#include "graphics/GraphicsThumbnail.h"
#include "graphics/GraphicsCompatibility.h"
#include "compositor/CompositorOverlayRaster.h"
#include "modules/ProgramFramePreview.h"
#include <cmath>
#include <algorithm>

namespace corevideo::graphics {
rpc::Json renderGraphicsThumbnail(const std::string& id,std::shared_ptr<const GraphicsTemplate> definition) {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
  modules::CompositorOverlayContent overlay;
  overlay.text="Example guest";overlay.title="Producer";overlay.keyPhase="on-air";overlay.keyProgress=1;
  overlay.appearance=defaultAppearance(id);overlay.fontFamily=overlay.appearance.fontFamily;
  double width=overlay.appearance.width*1920;
  double height=overlay.appearance.nameSize+overlay.appearance.titleSize+overlay.appearance.padding*3;
  if(definition) {
    auto input=std::make_shared<GraphicsTemplateInput>();input->definition=definition;input->fields=definition->defaultFields();
    auto fields=input->fields.asObject();
    if(const auto found=fields.find("name");found!=fields.end() && found->second.isString() && found->second.asString().empty()) fields["name"]="Example guest";
    input->fields=std::move(fields);
    const auto& a=overlay.appearance;
    input->brand=rpc::Json::Object{{"brand.primary",a.backgroundColor},{"brand.onPrimary",a.nameColor},{"brand.onPrimaryMuted",a.titleColor},
      {"brand.accent",a.accentColor},{"brand.fontBody",a.fontFamily},{"brand.fontHeading",a.fontFamily},
      {"brand.secondary",a.titleColor},{"brand.background",a.backgroundColor},{"brand.foreground",a.nameColor},{"brand.logo",definition->assets().count("logo")?"logo":""}};
    overlay.graphicsTemplate=std::move(input);width=definition->width();height=definition->height();
  }
  const double scale=std::min(320./width,180./height);
  const int w=std::max(1,int(std::lround(width*scale))),h=std::max(1,int(std::lround(height*scale)));
  modules::CompositorOverlayRaster raster;
  std::string warning;auto png=raster.thumbnailPng(overlay,w,h,warning);
  if(png.empty()) return rpc::Json::Object{{"state","unavailable"},{"reason",warning},{"backend","d3d11-native"}};
  return rpc::Json::Object{{"state","ready"},{"reason",warning},{"backend","d3d11-native"},{"appearance","reference"},
    {"width",w},{"height",h},{"pngBase64",modules::base64Encode(png.data(),png.size())}};
#else
  (void)id;(void)definition;
  return rpc::Json::Object{{"state","unsupported"},{"reason","Native library thumbnails are not available on this backend."}};
#endif
}
}
