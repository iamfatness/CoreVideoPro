#pragma once
#include "rpc/Json.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace corevideo::graphics {
struct GraphicsAsset {
  std::string id, sha256;
  std::shared_ptr<const std::vector<uint8_t>> encoded;
  int width = 0, height = 0;
};
using GraphicsAssets = std::map<std::string, std::shared_ptr<const GraphicsAsset>>;
// Package worker only. PNG/JPEG header bounds precede pixel decoding. The
// immutable bytes are later decoded by the raster owner under its CPU budget.
bool loadGraphicsAssets(const rpc::Json& document, const std::string& packageRoot,
                        GraphicsAssets& assets, std::string& error);
std::shared_ptr<const GraphicsAsset> graphicsAssetFromBytes(const std::string& id,
    std::shared_ptr<const std::vector<uint8_t>> bytes, std::string& error);
} // namespace corevideo::graphics
