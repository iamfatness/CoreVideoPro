#pragma once

#include "rpc/Json.h"
#include "graphics/GraphicsTimeline.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace corevideo::graphics {
struct GraphicsBox { double x = 0, y = 0, width = 0, height = 0; };
struct GraphicsPrimitive {
  std::string id, type, text, font, align, overflow;
  GraphicsBox box, clip;
  uint32_t color = 0xffffffff, endColor = 0xffffffff;
  double opacity = 1, cornerRadius = 0, fontSize = 28, lineHeight = 1.2;
  bool gradient = false, verticalGradient = false;
  int fontWeight = 400, maxLines = 1;
};
struct GraphicsDrawing {
  double width = 0, height = 0;
  std::vector<GraphicsPrimitive> primitives;
};

// Immutable, bounded declarative definitions. Parsing/installation never occurs
// on a render caller; field/brand resolution and layout run on the raster owner.
class GraphicsTemplate {
 public:
  static std::shared_ptr<const GraphicsTemplate> load(const std::string& manifest, std::string& error);
  static std::shared_ptr<const GraphicsTemplate> corporateLowerThird();
  const rpc::Json& document() const { return document_; }
  const std::array<uint8_t, 32>& hash() const { return hash_; }
  std::string id() const { return document_.getString("templateId"); }
  int version() const { return int(document_.getNumber("templateVersion")); }
  double width() const { return document_.get("bounds")->getNumber("width"); }
  double height() const { return document_.get("bounds")->getNumber("height"); }
  rpc::Json defaultFields() const;
  bool validateFields(const rpc::Json& fields, bool requireValues, std::string& error) const;
  bool resolve(const rpc::Json& fields, const rpc::Json& brand, GraphicsDrawing& drawing, std::string& error) const;
 private:
  rpc::Json document_;
  std::array<uint8_t, 32> hash_{};
};

struct GraphicsTemplateInput {
  std::shared_ptr<const GraphicsTemplate> definition;
  rpc::Json fields, brand;
};
} // namespace corevideo::graphics
