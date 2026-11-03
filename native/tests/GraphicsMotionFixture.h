#pragma once

#include "graphics/GraphicsTemplate.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace corevideo::graphics::test {
inline std::string motionFixture(const char* name) {
  std::ifstream file(std::filesystem::path(COREVIDEO_REPO_ROOT) /
      "native/tests/fixtures/graphics-motion" / name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), {});
}

inline rpc::Json motionBrand() {
  return rpc::Json::Object{{"brand.primary", "#203040"}, {"brand.onPrimary", "#FFFFFF"},
      {"brand.accent", "#44C1A1"}, {"brand.fontBody", "Segoe UI"}};
}

inline GraphicsDrawing motionDrawing(const char* name) {
  std::string error;
  const auto definition = GraphicsTemplate::load(motionFixture(name), error);
  if (!definition) throw std::runtime_error(error);
  GraphicsDrawing result;
  if (!definition->resolve(definition->defaultFields(), motionBrand(), result, error))
    throw std::runtime_error(error);
  return result;
}
} // namespace corevideo::graphics::test
