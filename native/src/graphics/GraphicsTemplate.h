#pragma once

#include "rpc/Json.h"
#include "graphics/GraphicsTimeline.h"
#include "graphics/GraphicsAssets.h"
#include "graphics/GraphicsPlacement.h"
#include <array>
#include <memory>
#include <string>
#include <vector>
#include <set>

namespace corevideo::graphics {
class GraphicsTemplateDraft;
struct GraphicsBox { double x = 0, y = 0, width = 0, height = 0; };
struct GraphicsPrimitive {
  std::string id, type, text, font, align, overflow;
  GraphicsBox box, clip;
  uint32_t color = 0xffffffff, endColor = 0xffffffff;
  double opacity = 1, cornerRadius = 0, fontSize = 28, lineHeight = 1.2;
  bool gradient = false, verticalGradient = false;
  int fontWeight = 400, maxLines = 1;
  std::shared_ptr<const GraphicsAsset> image;
  std::string fit = "contain", imageAlign = "center", imageVerticalAlign = "center";
};
struct GraphicsNodeGeometry { std::string id,parent,type; GraphicsBox box,clip; bool visible=true; };
struct GraphicsDrawing {
  double width = 0, height = 0;
  std::vector<GraphicsPrimitive> primitives;
  std::vector<std::string> warnings;
  std::set<std::string> usedFields;
  std::vector<GraphicsNodeGeometry> nodes;
};

// Immutable, bounded declarative definitions. Parsing/installation never occurs
// on a render caller; field/brand resolution and layout run on the raster owner.
class GraphicsTemplate {
 public:
  static std::shared_ptr<const GraphicsTemplate> load(const std::string& manifest, std::string& error, GraphicsAssets assets = {});
  static std::shared_ptr<const GraphicsTemplate> corporateLowerThird();
  static const std::vector<std::shared_ptr<const GraphicsTemplate>>& curated();
  const rpc::Json& document() const { return document_; }
  const std::array<uint8_t, 32>& hash() const { return hash_; }
  const GraphicsAssets& assets() const { return assets_; }
  std::string id() const { return document_.getString("templateId"); }
  int version() const { return int(document_.getNumber("templateVersion")); }
  double width() const { return document_.get("bounds")->getNumber("width"); }
  double height() const { return document_.get("bounds")->getNumber("height"); }
  rpc::Json defaultFields() const;
  std::optional<GraphicsPlacement> defaultPlacement() const { std::optional<GraphicsPlacement> value; if (const auto* p = document_.get("defaultPlacement")) GraphicsPlacement::read(*p,value); return value; }
  bool validateFields(const rpc::Json& fields, bool requireValues, std::string& error) const;
  bool resolve(const rpc::Json& fields, const rpc::Json& brand, GraphicsDrawing& drawing, std::string& error, std::shared_ptr<const GraphicsAsset> logo = {}) const;
 private:
  rpc::Json document_;
  std::array<uint8_t, 32> hash_{};
  GraphicsAssets assets_;
};

struct GraphicsTemplateInput {
  std::shared_ptr<const GraphicsTemplate> definition;
  rpc::Json fields, brand;
  std::shared_ptr<const GraphicsAsset> logo;
  std::optional<GraphicsPlacement> placement;
  std::shared_ptr<GraphicsTemplateDraft> draft;
  std::string draftKey;
  mutable std::shared_ptr<const GraphicsTemplateInput> resolvedDraft;
};
} // namespace corevideo::graphics
