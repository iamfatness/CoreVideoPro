#include "graphics/GraphicsThumbnail.h"
#include "graphics/GraphicsCompatibility.h"
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
#include "compositor/CompositorOverlayRaster.h"
#elif !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL
#include "compositor/CoreTextThumbnail.h"
#endif
#include "modules/ProgramFramePreview.h"
#include <cmath>
#include <algorithm>

namespace corevideo::graphics {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && (COREVIDEO_WITH_D3D11 || COREVIDEO_WITH_METAL)
namespace {
// The library thumbnail's overlay and size, shared by the D3D11 and Metal
// branches (Metal graphics G5; folded in the Windows session). Only the raster
// call and the `backend` label differ by platform.
struct ThumbnailInput {
  modules::CompositorOverlayContent overlay;
  int width = 0, height = 0;
};

ThumbnailInput thumbnailInput(const std::string& id, const std::shared_ptr<const GraphicsTemplate>& definition, const rpc::Json& palette) {
  ThumbnailInput out;
  auto& overlay = out.overlay;
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
    input->brand=graphicsBrand(a,a.backgroundColor,definition->assets().count("logo")!=0,palette);
    overlay.graphicsTemplate=std::move(input);width=definition->width();height=definition->height();
  }
  const double scale=std::min(320./width,180./height);
  out.width=std::max(1,int(std::lround(width*scale)));out.height=std::max(1,int(std::lround(height*scale)));
  return out;
}

rpc::Json thumbnailResult(const std::vector<uint8_t>& png, const std::string& warning, int w, int h, const char* backend) {
  if(png.empty()) return rpc::Json::Object{{"state","unavailable"},{"reason",warning},{"backend",backend}};
  return rpc::Json::Object{{"state","ready"},{"reason",warning},{"backend",backend},{"appearance","reference"},
    {"width",w},{"height",h},{"pngBase64",modules::base64Encode(png.data(),png.size())}};
}
}  // namespace
#endif

rpc::Json renderGraphicsThumbnail(const std::string& id,std::shared_ptr<const GraphicsTemplate> definition,const rpc::Json& palette) {
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
  const auto input=thumbnailInput(id,definition,palette);
  modules::CompositorOverlayRaster raster;
  std::string warning;auto png=raster.thumbnailPng(input.overlay,input.width,input.height,warning);
  return thumbnailResult(png,warning,input.width,input.height,"d3d11-native");
#elif !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_METAL
  const auto input=thumbnailInput(id,definition,palette);
  std::string warning;auto png=modules::coreTextThumbnailPng(input.overlay,input.width,input.height,warning);
  return thumbnailResult(png,warning,input.width,input.height,"metal-native");
#else
  (void)id;(void)definition;(void)palette;
  return rpc::Json::Object{{"state","unsupported"},{"reason","Native library thumbnails are not available on this backend."}};
#endif
}
}
