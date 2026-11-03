#include "graphics/GraphicsTemplate.h"
#include "modules/Sha256.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <locale>
#include <stdexcept>
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwrite.h>
#include "compositor/ComPtrLite.h"
#endif

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
struct TextSize { double width; unsigned lines; };

TextSize measureText(const std::string& text, const std::string& font, double size, int weight,
    double wrapWidth = 8192, bool wrap = false) {
  if (text.empty()) return {0, 1};
#if !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS && COREVIDEO_WITH_D3D11
  const auto widen = [](const std::string& value) {
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), nullptr, 0);
    std::wstring wide(size_t(length), L'\0');
    if (length) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), int(value.size()), wide.data(), length);
    return wide;
  };
  using modules::ComPtrLite;
  thread_local ComPtrLite<IDWriteFactory> factory;
  if (!factory) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(factory.put()));
  const auto family = widen(font), value = widen(text);
  ComPtrLite<IDWriteTextFormat> format;
  ComPtrLite<IDWriteTextLayout> layout;
  DWRITE_TEXT_METRICS metrics{};
  if (factory && !family.empty() && !value.empty() &&
      SUCCEEDED(factory->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT(weight),
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, float(size), L"", format.put())) &&
      SUCCEEDED(format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP)) &&
      SUCCEEDED(factory->CreateTextLayout(value.data(), UINT32(value.size()), format.get(),
          float(wrapWidth), 8192.f, layout.put())) && SUCCEEDED(layout->GetMetrics(&metrics)))
    return {metrics.widthIncludingTrailingWhitespace, metrics.lineCount};
#else
  (void)font; (void)weight;
#endif
  // Portable contract validation has no native typography. Keep a bounded
  // estimate for its layout; the Windows renderer measures the shaped runs.
  const auto characters = std::count_if(text.begin(), text.end(), [](unsigned char byte) { return (byte & 0xc0) != 0x80; });
  const double width = double(characters) * size * .55;
  if (!wrap) return {width, 1};
  return {std::min(width, wrapWidth), unsigned(std::max(1., std::ceil(width / wrapWidth)))};
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
    const auto& layers = document_.get("layers")->asArray();
    const double plateWidth = hasAutoPlate(layers) ? contentWidth(layers, "none", 0, 0) : 0;
    for (const auto& node : layers) visit(node, 0, 0, root, 1, 0, false, true, {}, -1, plateWidth);
    return result;
  }
  bool hasUnanimatedLayer() const { return hasUnanimatedLayer_; }
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
  static bool autoWidth(const Json& node) { return node.getString("width") == "auto"; }
  static bool hasAutoPlate(const Json::Array& nodes) {
    return std::any_of(nodes.begin(), nodes.end(), [](const auto& child) {
      return child.getString("type") == "rect" && autoWidth(child);
    });
  }
  double clampWidth(const Json& node, double width) {
    const double minimum = quantity(node, "minWidth", 1, 8192);
    const double maximum = quantity(node, "maxWidth", minimum, 8192);
    return std::clamp(width, minimum, maximum);
  }
  double nodeWidth(const Json& node, int depth, double plateWidth) {
    if (!autoWidth(node)) return quantity(node, "width", 1, 8192);
    const auto type = node.getString("type");
    if (type == "rect") return clampWidth(node, plateWidth - quantity(node, "x", 0, 8192, 0));
    need(type == "group" && depth <= 8, "template-auto-width-type");
    if (const auto cached = autoWidths_.find(&node); cached != autoWidths_.end()) return cached->second;
    const auto* children = node.get("children");
    need(children && children->isArray() && children->asArray().size() <= 64, "template-children");
    const auto flow = stringProperty(node, "flow", "none");
    const double gap = quantity(node, "gap", 0, 256, 0), padding = quantity(node, "padding", 0, 256, 0);
    const auto width = clampWidth(node, contentWidth(children->asArray(), flow, gap, depth + 1) + 2 * padding);
    autoWidths_.emplace(&node, width);
    return width;
  }
  double contentWidth(const Json::Array& layers, const std::string& flow, double gap, int depth) {
    need(depth <= 9, "template-layer-limit");
    need(flow != "horizontal" || !hasAutoPlate(layers), "template-auto-width-cycle");
    double extent = 0, offset = 0;
    bool populated = false;
    for (const auto& node : layers) {
      measuredNodes_.insert(&node);
      need(measuredNodes_.size() <= 64, "template-layer-limit");
      const auto type = node.getString("type");
      if (type == "rect" && autoWidth(node)) continue;
      if (const auto* property = node.get("visible"); property && !resolve(property, "boolean").asBool()) continue;
      auto width = nodeWidth(node, depth, 0);
      if (type == "text") {
        const auto text = resolve(node.get("text"), "string").asString();
        if (text.empty() && boolean(node, "collapseWhenEmpty")) continue;
        const auto font = resolve(node.get("font"), "string").asString();
        width = std::min(width, measureText(text, font, quantity(node, "fontSize", 1, 512, 28),
            int(quantity(node, "fontWeight", 400, 700, 400))).width);
      } else if (type == "image" && boolean(node, "collapseWhenEmpty") &&
          resolve(node.get("asset"), "image").asString().empty()) continue;
      const double step = flow == "horizontal" ? offset + (populated ? gap : 0) : 0;
      extent = std::max(extent, step + quantity(node, "x", 0, 8192, 0) + width);
      offset = step + width;
      populated = true;
    }
    return extent;
  }
  bool table(const Json& node, const GraphicsPrimitive& base, bool visible) {
    need(!hasTable_, "template-table-limit");
    hasTable_ = true;
    const auto* columns = node.get("columns");
    const auto* rows = node.get("rows");
    need(columns && columns->isArray() && !columns->asArray().empty() && columns->asArray().size() <= 8 &&
        rows && rows->isArray() && !rows->asArray().empty() && rows->asArray().size() <= 12, "template-table-dimensions");
    const double rowHeight = quantity(node, "rowHeight", 8, 512);
    need(rowHeight * (rows->asArray().size() + 1) <= base.box.height, "template-table-height");
    const auto font = resolve(node.get("font"), "string");
    need(font.isString() && !font.asString().empty() && font.asString().size() <= 128 && utf8(font.asString()), "template-table-font");
    const double fontSize = quantity(node, "fontSize", 1, rowHeight);
    const auto textColor = color(resolve(node.get("color"), "color").asString());
    const auto headerFill = color(resolve(node.get("headerFill"), "color").asString());
    const auto rowFill = color(resolve(node.get("rowFill"), "color").asString());
    const auto alternateFill = node.get("alternateFill") ? color(resolve(node.get("alternateFill"), "color").asString()) : rowFill;
    struct Column { std::string heading, align; double weight; };
    std::vector<Column> layout;
    double totalWeight = 0;
    for (const auto& column : columns->asArray()) {
      keys(column, {"heading", "align", "weight"});
      const auto heading = column.getString("heading");
      const auto align = stringProperty(column, "align", "left");
      need(!heading.empty() && heading.size() <= 512 && utf8(heading) &&
          (align == "left" || align == "center" || align == "right"), "template-table-column");
      const auto weight = quantity(column, "weight", .1, 100, 1);
      totalWeight += weight;
      layout.push_back({heading, align, weight});
    }
    auto appendRow = [&](size_t index, uint32_t fill, const Json::Array* cells) {
      if (!visible) return;
      auto background = base;
      background.type = "rect";
      background.box.y += rowHeight * index;
      background.box.height = rowHeight;
      background.clip = intersect(base.clip, background.box);
      background.color = fill;
      draw_->primitives.push_back(background);
      double x = base.box.x;
      for (size_t column = 0; column < layout.size(); ++column) {
        const auto width = base.box.width * layout[column].weight / totalWeight;
        const auto padding = std::min(12., width * .08);
        auto cell = background;
        cell.type = "text";
        cell.box.x = x + padding;
        cell.box.width = width - 2 * padding;
        cell.clip = intersect(background.clip, cell.box);
        cell.text = cells ? (*cells)[column].asString() : layout[column].heading;
        cell.font = font.asString();
        cell.fontSize = fontSize;
        cell.fontWeight = cells ? 400 : 600;
        cell.align = layout[column].align;
        cell.overflow = "ellipsis";
        cell.color = textColor;
        draw_->primitives.push_back(std::move(cell));
        x += width;
      }
    };
    appendRow(0, headerFill, nullptr);
    size_t index = 1;
    for (const auto& row : rows->asArray()) {
      need(row.isObject() && row.get("field"), "template-table-row-field");
      const auto value = resolve(&row, "string");
      need(value.isString() && value.asString().size() <= 4096, "template-table-row");
      if (value.asString().empty()) continue;
      const auto cells = Json::parse(value.asString());
      need(cells && cells->isArray() && cells->asArray().size() == layout.size(), "template-table-row");
      for (const auto& cell : cells->asArray())
        need(cell.isString() && utf8(cell.asString()), "template-table-cell");
      appendRow(index, index % 2 ? rowFill : alternateFill, &cells->asArray());
      ++index;
    }
    return visible;
  }
  bool visit(const Json& node, double originX, double originY, GraphicsBox clip, double opacity, int depth, bool flowChild, bool parentVisible = true, const std::string& parentId = {}, int unit = -1, double plateWidth = 0, bool fitChildren = false) {
    need(depth <= 8 && ++nodes_ <= 64, "template-layer-limit");
    const auto nodeId = node.getString("id"), type = node.getString("type"); need(validTemplateId(nodeId) && ids_.insert(nodeId).second, "template-layer-id");
    std::set<std::string> allowed{"id", "type", "x", "y", "width", "height", "opacity", "visible", "metadata", "animations"};
    if (type == "group") allowed.insert({"children", "flow", "gap", "padding", "minWidth", "maxWidth"});
    else if (type == "rect") allowed.insert({"fill", "cornerRadius", "stroke", "strokeWidth", "minWidth", "maxWidth"});
    else if (type == "ellipse") allowed.insert({"fill", "stroke", "strokeWidth"});
    else if (type == "line") allowed.insert({"diagonal", "stroke", "strokeWidth"});
    else if (type == "text") allowed.insert({"text", "font", "fontWeight", "fontSize", "lineHeight", "color", "align", "maxLines", "overflow", "collapseWhenEmpty", "outline", "shadow", "minFontSize"});
    else if (type == "table") allowed.insert({"columns", "rows", "rowHeight", "font", "fontSize", "color", "headerFill", "rowFill", "alternateFill"});
    else if (type == "image") allowed.insert({"asset", "fit", "align", "verticalAlign", "collapseWhenEmpty", "missing"});
    else need(false, "unsupported-template-node-type");
    keys(node, allowed);
    need(autoWidth(node) || (!node.get("minWidth") && !node.get("maxWidth")), "template-auto-width-type");
    bool visible = parentVisible;
    if (const auto* property = node.get("visible")) {
      const auto value = resolve(property,"boolean"); need(value.isBool(),"template-visible-boolean"); visible = visible && value.asBool();
    }
    if (validate_) visible = true;
    GraphicsPrimitive p; p.id = nodeId; p.type = type;
    p.box = {originX + quantity(node, "x", 0, 8192, flowChild ? 0 : -1), originY + quantity(node, "y", 0, 8192, flowChild ? 0 : -1),
        nodeWidth(node, depth, plateWidth), quantity(node, "height", 1, 8192)};
    if (fitChildren && type == "text")
      p.box.width = std::clamp(clip.x + clip.width - p.box.x, 1., p.box.width);
    p.clip = intersect(clip, p.box); p.opacity = opacity * quantity(node, "opacity", 0, 1, 1);
    if (const auto* animations = node.get("animations")) {
      need(animations->isObject() && !animations->asObject().empty(), "template-layer-motion");
      GraphicsMotionUnit motion{nodeId, unit, unit < 0 && depth == 0, {}, {}, p.box, clip};
      for (const auto& [phase, value] : animations->asObject()) {
        need(phase == "in" || phase == "out", "template-layer-motion");
        (phase == "in" ? motion.in : motion.out) = GraphicsLayerMotion::read(value);
      }
      need(draw_->units.size() < 16, "template-motion-layer-limit");
      unit = static_cast<int>(draw_->units.size());
      draw_->units.push_back(std::move(motion));
    }
    p.unit = unit;
    if (unit < 0 && type != "group") hasUnanimatedLayer_ = true;
    draw_->nodes.push_back({nodeId,parentId,type,p.box,p.clip,visible && p.opacity>0});
    if (type == "group") {
      const auto flow = stringProperty(node, "flow", "none"); need(flow == "none" || flow == "vertical" || flow == "horizontal", "template-flow");
      const auto padding = quantity(node, "padding", 0, 256, 0), gap = quantity(node, "gap", 0, 256, 0);
      const auto* children = node.get("children"); need(children && children->isArray() && children->asArray().size() <= 64, "template-children");
      const double childPlateWidth = hasAutoPlate(children->asArray()) ? contentWidth(children->asArray(), flow, gap, depth + 1) : 0;
      double offset = 0; bool populated = false;
      for (const auto& child : children->asArray()) {
        const auto before = draw_->primitives.size();
        const double step = offset + (populated && flow != "none" ? gap : 0);
        if (visit(child, p.box.x + padding + (flow == "horizontal" ? step : 0), p.box.y + padding + (flow == "vertical" ? step : 0),
            intersect(p.clip, {p.box.x + padding, p.box.y + padding, std::max(0., p.box.width - padding * 2), std::max(0., p.box.height - padding * 2)}), p.opacity, depth + 1, flow != "none", visible, nodeId, unit, childPlateWidth, autoWidth(node))) {
          if (flow != "none") offset = step + (flow == "vertical" ? quantity(child, "height", 1, 8192) : nodeWidth(child, depth + 1, childPlateWidth));
          populated = populated || draw_->primitives.size() > before;
        }
      }
      return populated;
    }
    if (type == "table") return table(node, p, visible);
    if (type == "rect" || type == "ellipse" || type == "line") {
      const auto* stroke = node.get("stroke");
      need(stroke || (type != "line" && !node.get("strokeWidth")), "template-stroke-required");
      if (stroke) {
        p.strokeColor = color(resolve(stroke, "color").asString());
        p.strokeWidth = std::min(quantity(node, "strokeWidth", 0, 4096, 1), std::min(p.box.width, p.box.height));
      }
      if (type == "line") {
        const auto diagonal = stringProperty(node, "diagonal", "down");
        need(diagonal == "up" || diagonal == "down", "template-line-diagonal");
        p.lineRises = diagonal == "up";
        if (!visible) return false;
        draw_->primitives.push_back(std::move(p));
        return true;
      }
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
      if (node.get("minFontSize")) {
        const double minimum = std::min(p.fontSize, quantity(node, "minFontSize", 1, 512));
        const auto fits = [&](double size) {
          const auto measured = measureText(p.text, p.font, size, p.fontWeight, p.box.width, p.overflow == "wrap-then-ellipsis");
          return measured.width <= p.box.width && measured.lines <= unsigned(p.maxLines) &&
              measured.lines * size * p.lineHeight <= p.box.height;
        };
        if (!fits(p.fontSize)) {
          double low = minimum, high = p.fontSize;
          for (int step = 0; step < 8; ++step) {
            const double size = (low + high) / 2;
            if (fits(size)) low = size;
            else high = size;
          }
          p.fontSize = low;
        }
      }
      if (const auto* outline = node.get("outline")) {
        keys(*outline, {"color", "width"});
        p.outline = GraphicsTextOutline{color(resolve(outline->get("color"), "color").asString()),
            quantity(*outline, "width", 0, 8)};
      }
      if (const auto* shadow = node.get("shadow")) {
        keys(*shadow, {"color", "x", "y", "blur"});
        p.shadow = GraphicsTextShadow{color(resolve(shadow->get("color"), "color").asString()),
            quantity(*shadow, "x", -256, 256, 0), quantity(*shadow, "y", -256, 256, 0),
            quantity(*shadow, "blur", 0, 16, 0)};
      }
      if (boolean(node, "collapseWhenEmpty") && p.text.empty()) {draw_->nodes.back().visible=false;return false;}
    }
    if (!visible) return false;
    draw_->primitives.push_back(std::move(p)); return true;
  }
  const Json& document_; const Json& fields_; const Json& brand_; const GraphicsAssets& assets_; bool validate_;
  std::shared_ptr<const GraphicsAsset> logo_;
  std::map<const Json*, double> autoWidths_;
  std::set<const Json*> measuredNodes_;
  bool hasUnanimatedLayer_ = false;
  bool hasTable_ = false;
  std::set<std::string> declared_, ids_; size_t nodes_ = 0; GraphicsDrawing* draw_ = nullptr;
};
} // namespace

GraphicsLayerMotion GraphicsLayerMotion::read(const Json& value) {
  constexpr auto error = "template-layer-motion";
  need(value.isObject(), error);
  for (const auto& [key, item] : value.asObject()) {
    need(key == "type" || key == "startMs" || key == "durationMs" ||
        key == "easing" || key == "direction" || key == "distance", error);
  }
  const auto string = [&](const char* key, const char* fallback) {
    const auto* property = value.get(key);
    need(!property || property->isString(), error);
    return property ? property->asString() : std::string(fallback);
  };
  const auto number = [&](const char* key, double fallback, double minimum, double maximum) {
    const auto* property = value.get(key);
    need(!property || property->isNumber(), error);
    const double result = property ? property->asNumber() : fallback;
    need(std::isfinite(result) && result >= minimum && result <= maximum, error);
    return result;
  };
  GraphicsLayerMotion motion;
  motion.type = string("type", "");
  const bool slide = motion.type == "slide", grow = motion.type == "grow", rise = motion.type == "rise";
  const bool cut = motion.type == "cut";
  need(cut || slide || grow || rise || motion.type == "fade" || motion.type == "wipe" || motion.type == "pop", error);
  motion.easing = string("easing", "ease-out");
  need(motion.easing == "linear" || motion.easing == "ease-in" ||
      motion.easing == "ease-out" || motion.easing == "ease-in-out", error);
  motion.startMs = number("startMs", 0, 0, 3000);
  motion.durationMs = number("durationMs", 0, cut ? 0 : 1, cut ? 0 : 3000);
  need(motion.startMs + motion.durationMs <= 3000, "template-motion-too-long");
  need(!value.get("direction") || slide || grow || motion.type == "wipe", error);
  motion.direction = string("direction", grow ? "down" : "left");
  need(motion.direction == "left" || motion.direction == "right" ||
      motion.direction == "up" || motion.direction == "down", error);
  const auto* distance = value.get("distance");
  need(!distance || slide || rise, error);
  motion.distanceEdge = distance ? distance->isString() && distance->asString() == "edge" : slide;
  if (!motion.distanceEdge) motion.distance = number("distance", 24, 1, 4096);
  return motion;
}

Json GraphicsLayerMotion::json() const {
  Json::Object value{{"type", type}, {"startMs", startMs}, {"durationMs", durationMs}, {"easing", easing}};
  if (type == "slide" || type == "wipe" || type == "grow") value["direction"] = direction;
  if (type == "slide" || type == "rise") value["distance"] = distanceEdge ? Json("edge") : Json(distance);
  return value;
}

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
    const auto defaults = definition->defaultFields();
    const Json brand = Json::Object{};
    Layout layout(d, defaults, brand, definition->assets_, true);
    const auto analysis = layout.run();
    auto& motion = definition->motion_;
    motion.units = analysis.units.size();
    motion.layered = motion.units != 0;
    motion.inFollows = motion.outFollows = !motion.layered || layout.hasUnanimatedLayer();
    for (const auto& unit : analysis.units) {
      if (unit.in) motion.inMs = std::max(motion.inMs, unit.in->startMs + unit.in->durationMs);
      if (unit.out) motion.outMs = std::max(motion.outMs, unit.out->startMs + unit.out->durationMs);
      if (unit.root && !unit.in) motion.inFollows = true;
      if (unit.root && !unit.out) motion.outFollows = true;
    }
    motion.anyFollows = motion.inFollows || motion.outFollows;
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
