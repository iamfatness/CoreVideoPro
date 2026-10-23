#include "graphics/GraphicsTemplate.h"
#include "modules/Sha256.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <locale>
#include <stdexcept>

namespace corevideo::graphics {
using rpc::Json;
namespace {
void need(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
bool validTemplateId(const std::string& s) {
  return !s.empty() && s.size() <= 64 && s[0] >= 'a' && s[0] <= 'z' &&
      std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
bool utf8(const std::string& s) {
  for (size_t i = 0; i < s.size();) {
    const auto first = uint8_t(s[i++]); if (first < 128) continue;
    int count = first >= 0xc2 && first <= 0xdf ? 1 : first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : -1;
    if (count < 0 || i + count > s.size()) return false;
    uint32_t cp = first & (count == 1 ? 31 : count == 2 ? 15 : 7);
    for (int j = 0; j < count; ++j) { const auto c = uint8_t(s[i++]); if ((c & 0xc0) != 0x80) return false; cp = (cp << 6) | (c & 63); }
    if ((count == 1 && cp < 128) || (count == 2 && cp < 2048) || (count == 3 && cp < 65536) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
  }
  return true;
}

// Separate strict package parser: the established RPC parser's permissive wire
// behavior is unchanged. Bound bytes before recursion, decoded values and keys
// before retaining them; reject duplicate keys and malformed JSON numbers.
class DocumentParser {
 public:
  explicit DocumentParser(const std::string& text) : text_(text) {
    need(text.size() <= 256 * 1024 && utf8(text), "template-json-size-or-encoding");
  }
  Json parse() { auto result = value(0); space(); need(pos_ == text_.size(), "template-json-trailing-data"); return result; }
 private:
  void space() { while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\r' || text_[pos_] == '\n' || text_[pos_] == '\t')) ++pos_; }
  bool eat(char c) { space(); if (pos_ < text_.size() && text_[pos_] == c) { ++pos_; return true; } return false; }
  void expect(char c) { need(eat(c), "template-json-syntax"); }
  unsigned hex4() {
    need(pos_ + 4 <= text_.size(), "template-json-escape"); unsigned n = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++]; const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      need(digit >= 0, "template-json-escape"); n = n * 16 + digit;
    } return n;
  }
  std::string string() {
    space(); const auto begin = pos_; expect('"');
    bool closed = false;
    while (pos_ < text_.size()) {
      const auto c = uint8_t(text_[pos_++]);
      if (c == '"') { closed = true; break; }
      need(c >= 32 && pos_ - begin <= 24578, "template-json-string");
      if (c != '\\') continue;
      need(pos_ < text_.size(), "template-json-escape"); const char e = text_[pos_++];
      if (e == 'u') {
        const auto cp = hex4();
        if (cp >= 0xd800 && cp <= 0xdbff) {
          need(pos_ + 2 <= text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u', "template-json-surrogate");
          pos_ += 2; const auto low = hex4(); need(low >= 0xdc00 && low <= 0xdfff, "template-json-surrogate");
        } else need(cp < 0xdc00 || cp > 0xdfff, "template-json-surrogate");
      } else need(std::string("\"\\/bfnrt").find(e) != std::string::npos, "template-json-escape");
    }
    need(closed, "template-json-string");
    const auto decoded = Json::parse(text_.substr(begin, pos_ - begin));
    need(decoded && decoded->isString() && decoded->asString().size() <= 4096, "template-json-string-limit");
    return decoded->asString();
  }
  Json value(int depth) {
    space(); need(depth <= 32 && ++values_ <= 4096 && pos_ < text_.size(), "template-json-structure-limit");
    const char c = text_[pos_];
    if (c == '"') return string();
    if (eat('{')) {
      Json::Object object; if (eat('}')) return object;
      do { const auto key = string(); need(key.size() <= 128 && !object.count(key), "template-json-duplicate-or-long-key"); expect(':'); object.emplace(key, value(depth + 1)); } while (eat(','));
      expect('}'); return object;
    }
    if (eat('[')) {
      Json::Array array; if (eat(']')) return array;
      do { array.push_back(value(depth + 1)); } while (eat(',')); expect(']'); return array;
    }
    for (const auto* literal : {"true", "false", "null"}) {
      const std::string word = literal;
      if (text_.compare(pos_, word.size(), word) == 0) { pos_ += word.size(); return word == "true" ? Json(true) : word == "false" ? Json(false) : Json{}; }
    }
    const auto start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    need(pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9', "template-json-number");
    if (text_[pos_] == '0') ++pos_; else while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    const auto digits = [&] { const auto before = pos_; while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_; need(pos_ > before, "template-json-number"); };
    if (pos_ < text_.size() && text_[pos_] == '.') { ++pos_; digits(); }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) { ++pos_; if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_; digits(); }
    need(pos_ - start <= 64, "template-json-number");
    std::istringstream numeric(text_.substr(start, pos_ - start)); numeric.imbue(std::locale::classic()); double number = 0;
    numeric >> number; need(!numeric.fail() && numeric.eof() && std::isfinite(number), "template-json-number"); return number;
  }
  const std::string& text_; size_t pos_ = 0, values_ = 0;
};

double number(const Json& object, const std::string& key, double low, double high, double fallback = -1) {
  const auto* v = object.get(key); const double n = v ? v->asNumber(NAN) : fallback;
  need((!v || v->isNumber()) && std::isfinite(n) && n >= low && n <= high, "template-geometry-or-number"); return n;
}
void keys(const Json& value, const std::set<std::string>& allowed) {
  need(value.isObject(), "template-object-required");
  for (const auto& [key, _] : value.asObject()) need(allowed.count(key) != 0, "unsupported-template-property");
}
bool boolean(const Json& value, const std::string& key, bool fallback = false) {
  const auto* v = value.get(key); need(!v || v->isBool(), "template-boolean-required"); return v ? v->asBool() : fallback;
}
std::string stringProperty(const Json& value, const std::string& key, const std::string& fallback) {
  const auto* v = value.get(key); need(!v || v->isString(), "template-string-required"); return v ? v->asString() : fallback;
}
uint32_t color(const std::string& s) {
  need((s.size() == 7 || s.size() == 9) && s[0] == '#', "template-color-required"); uint32_t n = 0;
  for (size_t i = 1; i < s.size(); ++i) { const char c = s[i]; const int h = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; need(h >= 0, "template-color-required"); n = (n << 4) | h; }
  return s.size() == 7 ? 0xff000000u | n : ((n & 255) << 24) | (n >> 8);
}
const std::set<std::string> tokens{"brand.primary", "brand.secondary", "brand.accent", "brand.background", "brand.foreground", "brand.onPrimary", "brand.onPrimaryMuted", "brand.fontHeading", "brand.fontBody", "brand.logo"};
Json typedValue(const Json& field, const Json& value) {
  const auto type = field.getString("type");
  if (type == "string" || type == "color" || type == "image") {
    need(value.isString() && utf8(value.asString()), "template-field-string-required");
    need(value.asString().size() <= number(field, "maxLength", 0, 4096, type == "string" ? 512 : 128), "template-field-length");
    if (type == "color") color(value.asString());
    if (type == "image") need(value.asString().empty() || validTemplateId(value.asString()), "template-image-id-required");
  } else if (type == "number") {
    const auto minimum = number(field, "minimum", -1e9, 1e9, -1e9), maximum = number(field, "maximum", minimum, 1e9, 1e9);
    need(value.isNumber() && std::isfinite(value.asNumber()) && value.asNumber() >= minimum && value.asNumber() <= maximum, "template-field-range");
  } else if (type == "boolean") need(value.isBool(), "template-field-boolean-required");
  else need(false, "unsupported-template-field-type");
  if (const auto* choices = field.get("choices")) {
    need(choices->isArray() && !choices->asArray().empty() && choices->asArray().size() <= 32, "template-field-choices");
    need(std::any_of(choices->asArray().begin(), choices->asArray().end(), [&](const auto& choice) { return choice.stringify() == value.stringify(); }), "template-field-choice");
  }
  return value;
}
GraphicsBox intersect(GraphicsBox a, GraphicsBox b) {
  const auto right = std::min(a.x + a.width, b.x + b.width), bottom = std::min(a.y + a.height, b.y + b.height);
  a.x = std::max(a.x, b.x); a.y = std::max(a.y, b.y); a.width = std::max(0., right - a.x); a.height = std::max(0., bottom - a.y); return a;
}
class Layout {
 public:
  Layout(const Json& document, const Json& fields, const Json& brand, const GraphicsAssets& assets, bool validate = false, std::shared_ptr<const GraphicsAsset> logo = {})
      : document_(document), fields_(fields), brand_(brand), assets_(assets), validate_(validate), logo_(std::move(logo)) {
    for (const auto& item : document.get("brandTokens")->asArray()) declared_.insert(item.asString());
  }
  GraphicsDrawing run() {
    GraphicsDrawing result; result.width = document_.get("bounds")->getNumber("width"); result.height = document_.get("bounds")->getNumber("height");
    draw_ = &result; const GraphicsBox root{0, 0, result.width, result.height};
    for (const auto& node : document_.get("layers")->asArray()) visit(node, 0, 0, root, 1, 0, false);
    return result;
  }
 private:
  Json resolve(const Json* value, const std::string& type) {
    need(value != nullptr, "template-property-required");
    if (!value->isObject()) return *value;
    need(value->asObject().size() == 1, "template-reference-shape");
    if (const auto* field = value->get("field")) {
      need(field->isString(), "template-field-reference");
      const auto& definitions = document_.get("fields")->asArray();
      const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const auto& f) { return f.getString("id") == field->asString(); });
      need(found != definitions.end() && found->getString("type") == type, "template-field-reference-type");
      draw_->usedFields.insert(field->asString());
      const auto* supplied = fields_.get(field->asString()); need(supplied, "template-field-reference"); return *supplied;
    }
    const auto token = value->getString("token"); need(tokens.count(token) && declared_.count(token), "template-brand-reference");
    need((type == "string" && (token == "brand.fontBody" || token == "brand.fontHeading")) ||
        (type == "color" && token != "brand.fontBody" && token != "brand.fontHeading" && token != "brand.logo") ||
        (type == "image" && token == "brand.logo"), "template-brand-reference-type");
    if (validate_) return type == "color" ? Json("#FFFFFF") : type == "image" ? Json("") : Json("Segoe UI");
    const auto* supplied = brand_.get(token); need(supplied && supplied->isString(), "template-brand-value-missing"); return *supplied;
  }
  double quantity(const Json& node, const std::string& key, double minimum, double maximum, double fallback = -1) {
    const auto* value = node.get(key); const auto scalar = value ? resolve(value, "number") : Json(fallback);
    need(scalar.isNumber() && std::isfinite(scalar.asNumber()) && scalar.asNumber() >= minimum && scalar.asNumber() <= maximum, "template-geometry-or-number");
    return scalar.asNumber();
  }
  bool visit(const Json& node, double originX, double originY, GraphicsBox clip, double opacity, int depth, bool flowChild, bool parentVisible = true, const std::string& parentId = {}) {
    need(depth <= 8 && ++nodes_ <= 64, "template-layer-limit");
    const auto nodeId = node.getString("id"), type = node.getString("type"); need(validTemplateId(nodeId) && ids_.insert(nodeId).second, "template-layer-id");
    std::set<std::string> allowed{"id", "type", "x", "y", "width", "height", "opacity", "visible", "metadata"};
    if (type == "group") allowed.insert({"children", "flow", "gap", "padding"});
    else if (type == "rect") allowed.insert({"fill", "cornerRadius"});
    else if (type == "text") allowed.insert({"text", "font", "fontWeight", "fontSize", "lineHeight", "color", "align", "maxLines", "overflow", "collapseWhenEmpty"});
    else if (type == "image") allowed.insert({"asset", "fit", "align", "verticalAlign", "collapseWhenEmpty", "missing"});
    else need(false, "unsupported-template-node-type");
    keys(node, allowed);
    bool visible = parentVisible;
    if (const auto* property = node.get("visible")) {
      const auto value = resolve(property,"boolean"); need(value.isBool(),"template-visible-boolean"); visible = visible && value.asBool();
    }
    if (validate_) visible = true;
    GraphicsPrimitive p; p.id = nodeId; p.type = type;
    p.box = {originX + quantity(node, "x", 0, 8192, flowChild ? 0 : -1), originY + quantity(node, "y", 0, 8192, flowChild ? 0 : -1),
        quantity(node, "width", 1, 8192), quantity(node, "height", 1, 8192)};
    p.clip = intersect(clip, p.box); p.opacity = opacity * quantity(node, "opacity", 0, 1, 1);
    draw_->nodes.push_back({nodeId,parentId,type,p.box,p.clip,visible && p.opacity>0});
    if (type == "group") {
      const auto flow = stringProperty(node, "flow", "none"); need(flow == "none" || flow == "vertical" || flow == "horizontal", "template-flow");
      const auto padding = quantity(node, "padding", 0, 256, 0), gap = quantity(node, "gap", 0, 256, 0);
      const auto* children = node.get("children"); need(children && children->isArray() && children->asArray().size() <= 64, "template-children");
      double offset = 0; bool populated = false;
      for (const auto& child : children->asArray()) {
        const auto before = draw_->primitives.size();
        const double step = offset + (populated && flow != "none" ? gap : 0);
        if (visit(child, p.box.x + padding + (flow == "horizontal" ? step : 0), p.box.y + padding + (flow == "vertical" ? step : 0),
            intersect(p.clip, {p.box.x + padding, p.box.y + padding, std::max(0., p.box.width - padding * 2), std::max(0., p.box.height - padding * 2)}), p.opacity, depth + 1, flow != "none", visible, nodeId)) {
          if (flow != "none") offset = step + quantity(child, flow == "vertical" ? "height" : "width", 1, 8192);
          populated = populated || draw_->primitives.size() > before;
        }
      }
      return populated;
    }
    if (type == "rect") {
      const auto* fill = node.get("fill"); need(fill, "template-fill-required");
      if (fill->isObject() && fill->get("stops")) {
        keys(*fill, {"stops", "direction"}); const auto& stops = fill->get("stops")->asArray(); need(fill->get("stops")->isArray() && stops.size() == 2, "template-gradient-stops");
        p.color = color(resolve(&stops[0], "color").asString()); p.endColor = color(resolve(&stops[1], "color").asString()); p.gradient = true;
        const auto direction = stringProperty(*fill, "direction", "horizontal"); need(direction == "horizontal" || direction == "vertical", "template-gradient-direction"); p.verticalGradient = direction == "vertical";
      } else p.color = color(resolve(fill, "color").asString());
      p.cornerRadius = std::min(quantity(node, "cornerRadius", 0, 4096, 0), std::min(p.box.width, p.box.height) / 2);
    } else if (type == "image") {
      const auto source = resolve(node.get("asset"), "image"); need(source.isString(), "template-image-reference");
      const auto assetId = source.asString();
      p.fit = stringProperty(node, "fit", "contain"); p.imageAlign = stringProperty(node, "align", "center"); p.imageVerticalAlign = stringProperty(node, "verticalAlign", "center");
      const auto missing = stringProperty(node, "missing", "error");
      need((p.fit == "contain" || p.fit == "cover") && (p.imageAlign == "left" || p.imageAlign == "center" || p.imageAlign == "right") &&
          (p.imageVerticalAlign == "top" || p.imageVerticalAlign == "center" || p.imageVerticalAlign == "bottom") && (missing == "error" || missing == "hide"), "template-image-layout");
      const bool collapse = boolean(node, "collapseWhenEmpty");
      if (assetId.empty()) { need(validate_ || collapse || !visible, "template-image-required"); draw_->nodes.back().visible=false; return false; }
      need(validTemplateId(assetId), "template-image-id-required");
      const auto* declarations = document_.get("assets"); need(declarations && declarations->isArray(), "template-image-undeclared");
      const auto declared = std::find_if(declarations->asArray().begin(), declarations->asArray().end(), [&](const auto& d) { return d.getString("id") == assetId; });
      need(declared != declarations->asArray().end(), "template-image-undeclared");
      const auto asset = assets_.find(assetId);
      if (!validate_ && asset == assets_.end()) {
        need(!visible || (boolean(*declared, "optional") && missing == "hide"), "template-image-asset-missing");
        if (visible) draw_->warnings.push_back("Optional graphics asset omitted: " + assetId); draw_->nodes.back().visible=false; return false;
      }
      if (assetId == "logo" && logo_) p.image = logo_;
      else if (asset != assets_.end()) p.image = asset->second;
    } else {
      const auto text = resolve(node.get("text"), "string"), font = resolve(node.get("font"), "string");
      need(text.isString() && font.isString() && text.asString().size() <= 4096 && !font.asString().empty() && font.asString().size() <= 128 && utf8(font.asString()), "template-text-or-font");
      p.text = text.asString(); p.font = font.asString(); p.color = color(resolve(node.get("color"), "color").asString());
      p.fontSize = quantity(node, "fontSize", 1, 512); p.lineHeight = quantity(node, "lineHeight", .5, 3, 1.2);
      const auto weight = quantity(node, "fontWeight", 400, 700, 400); need(weight == 400 || weight == 500 || weight == 600 || weight == 700, "template-font-weight"); p.fontWeight = int(weight);
      const auto lines = quantity(node, "maxLines", 1, 3, 1); need(std::floor(lines) == lines, "template-max-lines"); p.maxLines = int(lines);
      p.align = stringProperty(node, "align", "left"); need(p.align == "left" || p.align == "center" || p.align == "right", "template-text-alignment");
      p.overflow = stringProperty(node, "overflow", "ellipsis"); need(p.overflow == "ellipsis" || p.overflow == "wrap-then-ellipsis", "template-text-overflow");
      if (boolean(node, "collapseWhenEmpty") && p.text.empty()) {draw_->nodes.back().visible=false;return false;}
    }
    if (!visible) return false;
    draw_->primitives.push_back(std::move(p)); return true;
  }
  const Json& document_; const Json& fields_; const Json& brand_; const GraphicsAssets& assets_; bool validate_;
  std::shared_ptr<const GraphicsAsset> logo_;
  std::set<std::string> declared_, ids_; size_t nodes_ = 0; GraphicsDrawing* draw_ = nullptr;
};
} // namespace

Json GraphicsTemplate::defaultFields() const {
  Json::Object values; for (const auto& field : document_.get("fields")->asArray()) {
    const auto* supplied = field.get("default"); values[field.getString("id")] = supplied ? *supplied : Json("");
  } return values;
}
bool GraphicsTemplate::validateFields(const Json& values, bool requireValues, std::string& error) const {
  try {
    need(values.isObject(), "template-fields-required");
    const auto& fields = document_.get("fields")->asArray();
    for (const auto& entry : values.asObject()) {
      // Apple's supported older Clang cannot capture a structured binding in
      // this predicate even in C++20 mode. Ordinary references preserve it.
      const auto& key = entry.first;
      const auto& value = entry.second;
      const auto field = std::find_if(fields.begin(), fields.end(), [&](const auto& f) { return f.getString("id") == key; });
      need(field != fields.end(), "unknown-template-field"); typedValue(*field, value);
    }
    for (const auto& field : fields) {
      const auto* value = values.get(field.getString("id")); need(value, "template-field-missing"); typedValue(field, *value);
      if (requireValues && boolean(field, "required")) need(!value->isString() || !value->asString().empty(), "required-template-field-empty");
    }
    error.clear(); return true;
  } catch (const std::exception& e) { error = e.what(); return false; }
}
bool GraphicsTemplate::resolve(const Json& fields, const Json& brand, GraphicsDrawing& drawing, std::string& error, std::shared_ptr<const GraphicsAsset> logo) const {
  try {
    if (!validateFields(fields, true, error)) throw std::runtime_error(error);
    need(brand.isObject(), "template-brand-required"); drawing = Layout(document_, fields, brand, assets_, false, std::move(logo)).run(); error.clear(); return true;
  } catch (const std::exception& e) { error = e.what(); return false; }
}
std::shared_ptr<const GraphicsTemplate> GraphicsTemplate::load(const std::string& manifest, std::string& error, GraphicsAssets assets) {
  try {
    auto definition = std::shared_ptr<GraphicsTemplate>(new GraphicsTemplate);
    definition->document_ = DocumentParser(manifest).parse(); const auto& d = definition->document_;
    keys(d, {"schemaVersion", "templateId", "templateVersion", "name", "family", "canvas", "bounds", "fields", "layers", "animations", "brandTokens", "assets", "metadata", "defaultPlacement"});
    need(number(d, "schemaVersion", 1, 1) == 1 && validTemplateId(d.getString("templateId")), "template-header");
    const auto version = number(d, "templateVersion", 1, 2147483647.); need(std::floor(version) == version, "template-version");
    need(!d.getString("name").empty() && d.getString("name").size() <= 128, "template-name");
    need(d.getString("family") == "lower-third" || d.getString("family") == "title" || d.getString("family") == "bug" || d.getString("family") == "clock" || d.getString("family") == "countdown", "template-family");
    const auto* canvas = d.get("canvas"); need(canvas, "template-canvas"); keys(*canvas, {"width", "height"}); number(*canvas, "width", 1920, 1920); number(*canvas, "height", 1080, 1080);
    if (const auto* p = d.get("defaultPlacement")) { std::optional<GraphicsPlacement> placement; need(GraphicsPlacement::read(*p,placement),"template-placement"); }
    if (const auto* p = d.get("defaultPlacement")) { std::optional<GraphicsPlacement> placement; need(GraphicsPlacement::read(*p,placement),"template-placement"); }
    const auto* bounds = d.get("bounds"); need(bounds, "template-bounds"); keys(*bounds, {"width", "height"}); number(*bounds, "width", 1, 8192); number(*bounds, "height", 1, 8192);
    const auto* fields = d.get("fields"); need(fields && fields->isArray() && fields->asArray().size() <= 32, "template-fields-limit"); std::set<std::string> ids;
    for (const auto& field : fields->asArray()) {
      keys(field, {"id", "type", "default", "required", "bindable", "maxLength", "minimum", "maximum", "choices", "metadata"});
      need(validTemplateId(field.getString("id")) && ids.insert(field.getString("id")).second, "template-field-id"); boolean(field, "required"); boolean(field, "bindable");
      if (field.get("maxLength")) { const auto maximum = number(field, "maxLength", 0, 4096); need(std::floor(maximum) == maximum, "template-field-length"); }
      if (const auto* choices = field.get("choices")) {
        need(choices->isArray() && !choices->asArray().empty() && choices->asArray().size() <= 32, "template-field-choices");
        auto plain = field.asObject(); plain.erase("choices");
        std::set<std::string> seen;
        for (const auto& choice : choices->asArray()) { typedValue(Json(plain), choice); need(seen.insert(choice.stringify()).second, "template-field-duplicate-choice"); }
      }
    }
    if (!definition->validateFields(definition->defaultFields(), false, error)) throw std::runtime_error(error);
    const auto* brands = d.get("brandTokens"); need(brands && brands->isArray() && brands->asArray().size() <= 10, "template-brand-limit"); ids.clear();
    for (const auto& token : brands->asArray()) need(token.isString() && tokens.count(token.asString()) && ids.insert(token.asString()).second, "template-brand-token");
    const auto* layers = d.get("layers"); need(layers && layers->isArray() && !layers->asArray().empty() && layers->asArray().size() <= 64, "template-layers-limit");
    ids.clear(); std::set<std::string> paths;
    if (const auto* declarations = d.get("assets")) {
      need(declarations->isArray() && declarations->asArray().size() <= 127, "graphics-assets-limit");
      for (const auto& declaration : declarations->asArray()) {
        keys(declaration, {"id", "file", "sha256", "optional", "metadata"});
        const auto assetId = declaration.getString("id"), digest = declaration.getString("sha256"), file = declaration.getString("file");
        need(validTemplateId(assetId) && ids.insert(assetId).second && digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "graphics-asset-id-or-hash");
        need((file == "assets/" + digest + ".png" || file == "assets/" + digest + ".jpg" || file == "assets/" + digest + ".jpeg") && paths.insert(file).second, "graphics-asset-path");
        boolean(declaration, "optional");
        if (const auto found = assets.find(assetId); found != assets.end()) need(found->second && found->second->sha256 == digest && found->second->encoded, "graphics-asset-hash-mismatch");
      }
    }
    for (const auto& asset : assets) need(ids.count(asset.first) != 0, "graphics-asset-undeclared");
    definition->assets_ = std::move(assets);
    const auto* animations = d.get("animations"); need(animations, "template-animations"); keys(*animations, {"in", "out"});
    for (const auto& [_, transition] : animations->asObject()) { GraphicsTransition parsed; need(GraphicsTransition::read(transition, parsed), "template-animation"); }
    const auto analysis = Layout(d, definition->defaultFields(), Json::Object{}, definition->assets_, true).run();
    for (const auto& field : fields->asArray()) if (boolean(field,"required"))
      need(analysis.usedFields.count(field.getString("id")) != 0,"required-template-field-unused");
    const auto canonical = d.stringify(); definition->hash_ = modules::hashing::sha256(reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size());
    error.clear(); return definition;
  } catch (const std::exception& e) { error = e.what(); return {}; }
}

std::shared_ptr<const GraphicsTemplate> GraphicsTemplate::corporateLowerThird() {
  static const auto definition = [] {
    std::string error;
    auto result = load(R"({"schemaVersion":1,"templateId":"corporate-lower-third","templateVersion":1,"name":"Corporate lower third","family":"lower-third",
      "canvas":{"width":1920,"height":1080},"bounds":{"width":1050,"height":176},
      "brandTokens":["brand.primary","brand.onPrimary","brand.onPrimaryMuted","brand.fontBody"],
      "fields":[{"id":"name","type":"string","default":"","required":true,"bindable":true,"maxLength":256},
        {"id":"title","type":"string","default":"","required":false,"bindable":true,"maxLength":512}],
      "layers":[{"id":"plate","type":"rect","x":0,"y":0,"width":1050,"height":176,"fill":{"token":"brand.primary"},"opacity":0.92,"cornerRadius":8},
        {"id":"text-stack","type":"group","x":24,"y":20,"width":1002,"height":136,"flow":"vertical","gap":8,"children":[
          {"id":"name-text","type":"text","text":{"field":"name"},"width":1002,"height":62,"font":{"token":"brand.fontBody"},"fontWeight":600,"fontSize":42,"lineHeight":1.2,"color":{"token":"brand.onPrimary"},"align":"left","maxLines":1,"overflow":"ellipsis"},
          {"id":"title-text","type":"text","text":{"field":"title"},"width":1002,"height":42,"font":{"token":"brand.fontBody"},"fontWeight":400,"fontSize":28,"lineHeight":1.2,"color":{"token":"brand.onPrimaryMuted"},"align":"left","maxLines":1,"overflow":"ellipsis","collapseWhenEmpty":true}]}],
      "animations":{"in":{"type":"slide","direction":"left","durationMs":400,"easing":"ease-out"},"out":{"type":"fade","durationMs":300,"easing":"linear"}}})", error);
    if (!result) throw std::runtime_error("Invalid built-in graphics definition: " + error);
    return result;
  }();
  return definition;
}
} // namespace corevideo::graphics
