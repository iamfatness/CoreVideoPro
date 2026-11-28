#pragma once
#include "graphics/GraphicsTemplate.h"
namespace corevideo::graphics {
// Package worker only. Named unsupported states on non-native typography builds.
// `palette` (token -> #RRGGBBAA; null = reference appearance) recolours a
// declarative look the way Program would (graphics styles slice 11).
rpc::Json renderGraphicsThumbnail(const std::string& id,std::shared_ptr<const GraphicsTemplate> definition = {},const rpc::Json& palette = {});
}
