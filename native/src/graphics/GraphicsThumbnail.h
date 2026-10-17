#pragma once
#include "graphics/GraphicsTemplate.h"
namespace corevideo::graphics {
// Package worker only. Named unsupported states on non-native typography builds.
rpc::Json renderGraphicsThumbnail(const std::string& id,std::shared_ptr<const GraphicsTemplate> definition = {});
}
