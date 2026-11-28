#include "graphics/GraphicsPack.h"

#include "graphics/GraphicsAssets.h"
#include "graphics/GraphicsTemplate.h"
#include "rpc/Json.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
#endif

namespace corevideo::graphics {
using rpc::Json;
namespace fs = std::filesystem;

namespace {

fs::path pathFromUtf8(const std::string& utf8) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string utf8FromPath(const fs::path& path) {
  const auto value = path.u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

bool readBytes(const fs::path& path, uint64_t limit, std::string& out) {
  std::error_code ec;
  const auto size = fs::file_size(path, ec);
  if (ec || size > limit) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out.assign(size_t(size), '\0');
  if (size && !in.read(out.data(), std::streamsize(size))) return false;
  return true;
}

std::string lower(std::string value) {
  for (auto& c : value) c = char(std::tolower(static_cast<unsigned char>(c)));
  return value;
}

bool validPackId(const std::string& id) {
  if (id.empty() || id.size() > 64 || id[0] < 'a' || id[0] > 'z') return false;
  return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

// "<dir>/<name>.<ext>": one level, plain file name, no traversal or separators
// beyond the single expected folder.
bool validRelative(const std::string& value, const std::string& folder, const std::vector<std::string>& extensions) {
  if (value.size() > 200 || value.rfind(folder + "/", 0) != 0) return false;
  const auto name = value.substr(folder.size() + 1);
  if (name.empty() || name[0] == '.' || name.find_first_of("/\\:*?\"<>|") != std::string::npos) return false;
  if (name.find("..") != std::string::npos) return false;
  const auto dot = name.rfind('.');
  if (dot == std::string::npos) return false;
  const auto extension = lower(name.substr(dot + 1));
  return std::find(extensions.begin(), extensions.end(), extension) != extensions.end();
}

bool isHexColour(const std::string& value) {
  if (value.size() < 4 || value[0] != '#') return false;
  const auto digits = value.size() - 1;
  if (digits != 3 && digits != 4 && digits != 6 && digits != 8) return false;
  return std::all_of(value.begin() + 1, value.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

bool isPaletteValue(const std::string& value) {
  return value.size() == 9 && isHexColour(value);
}

bool fontSignature(const std::string& bytes) {
  if (bytes.size() < 12) return false;
  const std::string head = bytes.substr(0, 4);
  return head == std::string("\x00\x01\x00\x00", 4) || head == "OTTO" || head == "true";
}

bool fileHasFontSignature(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  char head[12] = {};
  if (!in || !in.read(head, sizeof(head))) return false;
  return fontSignature(std::string(head, sizeof(head)));
}

bool pngSignature(const std::string& bytes) {
  static const std::string signature("\x89PNG\r\n\x1a\n", 8);
  return bytes.size() >= 8 && bytes.compare(0, 8, signature) == 0;
}

// Property names whose string values are content or enums, never colours. A
// text layer that says "#1" is not a raw hex colour.
const std::set<std::string>& nonColourKeys() {
  static const std::set<std::string> keys{
      "id", "type", "text", "items", "rows", "columns", "heading", "font", "metadata", "asset", "fit", "align",
      "verticalAlign", "overflow", "direction", "easing", "flow", "textTransform", "fontStyle", "mode", "missing",
      "field", "token", "shape", "by", "children", "animations"};
  return keys;
}

void findRawHex(const Json& value, const std::string& where, std::vector<std::string>& out) {
  if (value.isString()) {
    if (isHexColour(value.asString())) out.push_back(where + " = " + value.asString());
  } else if (value.isArray()) {
    for (size_t i = 0; i < value.asArray().size(); ++i)
      findRawHex(value.asArray()[i], where + "[" + std::to_string(i) + "]", out);
  } else if (value.isObject()) {
    for (const auto& [key, item] : value.asObject()) {
      if (nonColourKeys().count(key)) continue;
      findRawHex(item, where + "." + key, out);
    }
  }
}

struct DeclaredFace { int weight; bool italic; };

void lintLayers(const Json& layers, const std::string& templateName,
                const std::map<std::string, std::vector<DeclaredFace>>& faces, std::vector<std::string>& problems,
                size_t& count) {
  if (!layers.isArray()) return;
  for (const auto& node : layers.asArray()) {
    if (!node.isObject()) continue;
    ++count;
    const auto id = node.getString("id");
    const std::string where = templateName + ": layer " + id;
    const auto* fixed = node.get("fixed");
    if (fixed && !fixed->isBool()) problems.push_back(where + ": \"fixed\" must be true or false");
    if (!(fixed && fixed->isBool() && fixed->asBool())) {
      std::vector<std::string> hex;
      for (const auto& [key, item] : node.asObject()) {
        if (nonColourKeys().count(key)) continue;
        findRawHex(item, key, hex);
      }
      for (const auto& h : hex)
        problems.push_back(where + ": raw hex colour " + h + " (use a palette token, or mark the layer \"fixed\": true)");
    }
    if (const auto* font = node.get("font"); font && font->isString()) {
      const auto found = faces.find(lower(font->asString()));
      if (found == faces.end()) {
        problems.push_back(where + ": font \"" + font->asString() + "\" is not one of the pack's fonts");
      } else if (node.getString("type") == "text") {
        const int weight = int(node.getNumber("fontWeight", 400));
        const bool italic = node.getString("fontStyle") == "italic";
        const bool shipped = std::any_of(found->second.begin(), found->second.end(),
                                         [&](const DeclaredFace& f) { return f.weight == weight && f.italic == italic; });
        if (!shipped)
          problems.push_back(where + ": the pack ships no " + std::to_string(weight) + (italic ? " italic" : "") +
                             " face of \"" + font->asString() + "\" (packs ship every weight they use)");
      }
    }
    if (const auto* children = node.get("children")) lintLayers(*children, templateName, faces, problems, count);
  }
}

std::vector<std::string> folderFiles(const fs::path& folder) {
  std::vector<std::string> names;
  std::error_code ec;
  if (!fs::is_directory(folder, ec)) return names;
  for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) names.push_back(utf8FromPath(it->path().filename()));
  std::sort(names.begin(), names.end());
  return names;
}

std::string executableDirectory() {
#if defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
  if (length == 0 || length >= buffer.size()) return ".";
  buffer.resize(length);
  return utf8FromPath(fs::path(buffer).parent_path());
#elif defined(__APPLE__)
  char raw[PATH_MAX] = {};
  uint32_t size = sizeof(raw);
  if (_NSGetExecutablePath(raw, &size) != 0) return ".";
  char resolved[PATH_MAX] = {};
  const std::string executable = realpath(raw, resolved) ? resolved : raw;
  const auto slash = executable.rfind('/');
  return slash == std::string::npos ? std::string(".") : executable.substr(0, slash);
#else
  std::error_code ec;
  const auto self = fs::read_symlink("/proc/self/exe", ec);
  return ec ? std::string(".") : utf8FromPath(self.parent_path());
#endif
}

// Discovery state for the pack font registry.
struct Registry {
  std::mutex mutex;
  bool scanned = false;
  std::string overrideDirectory;
  std::vector<GraphicsPackFontFile> fonts;
};
Registry& registry() {
  static auto* value = new Registry;  // outlives static destruction: raster workers may run late
  return *value;
}
std::atomic<uint64_t> gGeneration{1};
std::atomic<int> gStrictOverride{-1};

void scanLocked(Registry& r) {
  if (r.scanned) return;
  r.scanned = true;
  r.fonts.clear();
  const auto root = pathFromUtf8(r.overrideDirectory.empty() ? graphicsPacksDirectory() : r.overrideDirectory);
  for (const auto& name : folderFiles(root)) {
    GraphicsPackManifest manifest;
    std::string error;
    if (!readGraphicsPackManifest(utf8FromPath(root / pathFromUtf8(name)), manifest, error)) continue;
    for (const auto& font : manifest.fonts) {
      GraphicsPackFontFile file;
      file.packId = manifest.id;
      file.family = font.family;
      file.fallback = font.fallback;
      file.weight = font.weight;
      file.italic = font.style == "italic";
      const auto path = pathFromUtf8(manifest.root) / pathFromUtf8(font.file);
      file.path = utf8FromPath(path);
      std::error_code ec;
      file.present = fs::is_regular_file(path, ec) && fileHasFontSignature(path);
      r.fonts.push_back(std::move(file));
      if (r.fonts.size() >= 256) return;
    }
  }
}

}  // namespace

const std::vector<std::string>& graphicsPackRequiredPaletteTokens() {
  static const std::vector<std::string> tokens{"brand.primary", "brand.secondary", "brand.accent", "brand.onPrimary",
                                               "brand.onSecondary", "brand.onAccent", "brand.surface"};
  return tokens;
}

bool graphicsPackPaletteTokenKnown(const std::string& token) {
  static const std::set<std::string> optional{"brand.teamA", "brand.teamB", "brand.onPrimaryMuted", "brand.background",
                                              "brand.foreground"};
  const auto& required = graphicsPackRequiredPaletteTokens();
  return optional.count(token) || std::find(required.begin(), required.end(), token) != required.end();
}

bool readGraphicsPackManifest(const std::string& dir, GraphicsPackManifest& out, std::string& error) {
  out = {};
  const auto root = pathFromUtf8(dir);
  std::string text;
  if (!readBytes(root / "pack.json", 64 * 1024, text)) { error = "pack.json is missing or larger than 64 KiB"; return false; }
  std::string parseError;
  const auto parsed = Json::parse(text, &parseError);
  if (!parsed || !parsed->isObject()) { error = "pack.json is not a JSON object" + (parseError.empty() ? "" : ": " + parseError); return false; }
  const auto& d = *parsed;
  static const std::set<std::string> allowed{"id", "name", "genre", "version", "palettes", "fonts", "templates", "metadata"};
  for (const auto& [key, _] : d.asObject())
    if (!allowed.count(key)) { error = "pack.json: unsupported key \"" + key + "\""; return false; }
  std::error_code ec;
  const auto absolute = fs::absolute(root, ec);
  out.root = utf8FromPath(ec ? root : absolute.lexically_normal());
  out.id = d.getString("id");
  out.name = d.getString("name");
  out.genre = d.getString("genre");
  out.version = d.getString("version");
  if (!validPackId(out.id)) { error = "pack.json: id must be lowercase letters, digits and hyphens, leading letter, at most 64"; return false; }
  if (out.name.empty() || out.name.size() > 128) { error = "pack.json: name is required (at most 128 bytes)"; return false; }
  if (out.genre.empty() || out.genre.size() > 64) { error = "pack.json: genre is required (at most 64 bytes)"; return false; }
  if (out.version.empty() || out.version.size() > 32) { error = "pack.json: version is required (at most 32 bytes)"; return false; }
  const auto strings = [&](const char* key, const char* folder, const std::vector<std::string>& extensions, size_t limit,
                           std::vector<std::string>& into) {
    const auto* list = d.get(key);
    if (!list || !list->isArray() || list->asArray().size() > limit) {
      error = std::string("pack.json: ") + key + " must be an array of at most " + std::to_string(limit);
      return false;
    }
    std::set<std::string> seen;
    for (const auto& item : list->asArray()) {
      if (!item.isString() || !validRelative(item.asString(), folder, extensions) || !seen.insert(lower(item.asString())).second) {
        error = std::string("pack.json: ") + key + " entries must be unique \"" + folder + "/<name>.json\" paths";
        return false;
      }
      into.push_back(item.asString());
    }
    return true;
  };
  if (!strings("palettes", "palettes", {"json"}, kGraphicsPackPaletteLimit, out.palettes)) return false;
  if (!strings("templates", "templates", {"json"}, kGraphicsPackTemplateLimit, out.templates)) return false;
  if (out.templates.empty()) { error = "pack.json: a pack ships at least one template"; return false; }
  const auto* fonts = d.get("fonts");
  if (!fonts || !fonts->isArray() || fonts->asArray().size() > kGraphicsPackFontLimit) {
    error = "pack.json: fonts must be an array of at most " + std::to_string(kGraphicsPackFontLimit);
    return false;
  }
  std::set<std::string> files, faces;
  for (const auto& item : fonts->asArray()) {
    if (!item.isObject()) { error = "pack.json: each font is an object"; return false; }
    static const std::set<std::string> fontKeys{"family", "file", "weight", "style", "fallback"};
    for (const auto& [key, _] : item.asObject())
      if (!fontKeys.count(key)) { error = "pack.json: unsupported font key \"" + key + "\""; return false; }
    GraphicsPackFont font;
    font.family = item.getString("family");
    font.file = item.getString("file");
    font.weight = int(item.getNumber("weight", 400));
    font.style = item.getString("style", "normal");
    font.fallback = item.getString("fallback");
    if (font.family.empty() || font.family.size() > 128 || font.fallback.size() > 128) { error = "pack.json: a font needs a family (at most 128 bytes)"; return false; }
    if (!validRelative(font.file, "fonts", {"ttf", "otf"}) || !files.insert(lower(font.file)).second) {
      error = "pack.json: font file must be a unique \"fonts/<name>.ttf|otf\" path: " + font.file;
      return false;
    }
    const double weight = item.getNumber("weight", 400);
    if (weight < 100 || weight > 900 || weight != double(font.weight) || font.weight % 100 != 0) {
      error = "pack.json: font weight must be 100..900 in steps of 100: " + font.family;
      return false;
    }
    if (font.style != "normal" && font.style != "italic") { error = "pack.json: font style must be normal or italic: " + font.family; return false; }
    if (!faces.insert(lower(font.family) + "/" + std::to_string(font.weight) + "/" + font.style).second) {
      error = "pack.json: the same family, weight and style is declared twice: " + font.family;
      return false;
    }
    out.fonts.push_back(std::move(font));
  }
  error.clear();
  return true;
}

std::vector<std::string> lintGraphicsPack(const std::string& dir) {
  std::vector<std::string> problems;
  GraphicsPackManifest manifest;
  std::string error;
  if (!readGraphicsPackManifest(dir, manifest, error)) {
    problems.push_back(error);
    return problems;
  }
  const auto root = pathFromUtf8(manifest.root);
  if (utf8FromPath(root.filename()) != manifest.id)
    problems.push_back("pack.json: id \"" + manifest.id + "\" must match the folder name \"" + utf8FromPath(root.filename()) + "\"");

  // Palettes.
  for (const auto& relative : manifest.palettes) {
    std::string text;
    if (!readBytes(root / pathFromUtf8(relative), 16 * 1024, text)) { problems.push_back(relative + ": missing or larger than 16 KiB"); continue; }
    const auto palette = Json::parse(text);
    if (!palette || !palette->isObject()) { problems.push_back(relative + ": not a JSON object"); continue; }
    for (const auto& [token, value] : palette->asObject()) {
      if (!graphicsPackPaletteTokenKnown(token)) problems.push_back(relative + ": unknown token \"" + token + "\"");
      else if (!value.isString() || !isPaletteValue(value.asString()))
        problems.push_back(relative + ": " + token + " must be \"#RRGGBBAA\"");
    }
    for (const auto& token : graphicsPackRequiredPaletteTokens())
      if (!palette->get(token)) problems.push_back(relative + ": missing required token " + token);
  }
  for (const auto& name : folderFiles(root / "palettes")) {
    const auto relative = "palettes/" + name;
    if (std::none_of(manifest.palettes.begin(), manifest.palettes.end(), [&](const auto& p) { return lower(p) == lower(relative); }))
      problems.push_back(relative + ": not listed in pack.json");
  }

  // Fonts.
  std::map<std::string, std::vector<DeclaredFace>> faces;
  uint64_t fontBytes = 0;
  for (const auto& font : manifest.fonts) {
    faces[lower(font.family)].push_back({font.weight, font.style == "italic"});
    const auto path = root / pathFromUtf8(font.file);
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec) { problems.push_back(font.file + ": declared font file is missing"); continue; }
    fontBytes += size;
    if (!fileHasFontSignature(path)) problems.push_back(font.file + ": not a TrueType/OpenType font");
  }
  if (fontBytes > kGraphicsPackFontBytesLimit) problems.push_back("fonts/: more than 8 MB of font files");
  const auto fontFiles = folderFiles(root / "fonts");
  if (!manifest.fonts.empty() && std::find(fontFiles.begin(), fontFiles.end(), "OFL.txt") == fontFiles.end())
    problems.push_back("fonts/OFL.txt: the font licence text is missing (packs ship OFL fonts with their licence)");
  for (const auto& name : fontFiles) {
    if (name == "OFL.txt") continue;
    const auto relative = "fonts/" + name;
    if (std::none_of(manifest.fonts.begin(), manifest.fonts.end(), [&](const auto& f) { return lower(f.file) == lower(relative); }))
      problems.push_back(relative + ": not declared in pack.json fonts");
  }

  // Assets: PNG only, 2 MB per pack.
  uint64_t assetBytes = 0;
  for (const auto& name : folderFiles(root / "assets")) {
    const auto relative = "assets/" + name;
    const auto path = root / "assets" / pathFromUtf8(name);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) { problems.push_back(relative + ": not a regular file"); continue; }
    const auto size = fs::file_size(path, ec);
    assetBytes += ec ? 0 : size;
    std::string bytes;
    if (lower(name.substr(name.rfind('.') == std::string::npos ? name.size() : name.rfind('.'))) != ".png" ||
        !readBytes(path, kGraphicsPackAssetBytesLimit, bytes) || !pngSignature(bytes))
      problems.push_back(relative + ": assets are PNG only (at most 2 MB per pack)");
  }
  if (assetBytes > kGraphicsPackAssetBytesLimit) problems.push_back("assets/: more than 2 MB in one pack");

  // Templates.
  std::set<std::string> templateIds;
  for (const auto& relative : manifest.templates) {
    std::string text;
    if (!readBytes(root / pathFromUtf8(relative), 256 * 1024, text)) { problems.push_back(relative + ": missing or larger than 256 KiB"); continue; }
    const auto document = Json::parse(text);
    if (!document || !document->isObject()) { problems.push_back(relative + ": not a JSON object"); continue; }
    GraphicsAssets assets;
    std::string loadError;
    if (document->get("assets") && !loadGraphicsAssets(*document, manifest.root, assets, loadError))
      problems.push_back(relative + ": assets: " + loadError);
    const auto definition = GraphicsTemplate::load(text, loadError, std::move(assets));
    if (!definition) problems.push_back(relative + ": the template does not load: " + loadError);
    else if (!templateIds.insert(definition->id()).second)
      problems.push_back(relative + ": templateId \"" + definition->id() + "\" is used twice in the pack");
    // The lint's own walk, so a pack sees every problem at once.
    if (const auto* fields = document->get("fields"); fields && fields->isArray() && fields->asArray().size() > 32)
      problems.push_back(relative + ": more than 32 fields");
    size_t layers = 0;
    if (const auto* list = document->get("layers")) lintLayers(*list, relative, faces, problems, layers);
    if (layers > 64) problems.push_back(relative + ": more than 64 layers");
  }
  for (const auto& name : folderFiles(root / "templates")) {
    const auto relative = "templates/" + name;
    if (std::none_of(manifest.templates.begin(), manifest.templates.end(), [&](const auto& t) { return lower(t) == lower(relative); }))
      problems.push_back(relative + ": not listed in pack.json");
  }
  return problems;
}

bool graphicsPackInstallManifests(const std::string& dir, std::vector<std::string>& manifests, std::string& error) {
  manifests.clear();
  const auto problems = lintGraphicsPack(dir);
  if (!problems.empty()) { error = problems.front(); return false; }
  GraphicsPackManifest manifest;
  if (!readGraphicsPackManifest(dir, manifest, error)) return false;
  for (const auto& relative : manifest.templates) {
    std::string text;
    if (!readBytes(pathFromUtf8(manifest.root) / pathFromUtf8(relative), 256 * 1024, text)) {
      error = relative + ": missing or larger than 256 KiB";
      manifests.clear();
      return false;
    }
    manifests.push_back(std::move(text));
  }
  error.clear();
  return true;
}

std::string graphicsPacksDirectory() {
  if (const char* override = std::getenv("COREVIDEO_GRAPHICS_PACKS_DIR"); override && *override) return override;
  return utf8FromPath(pathFromUtf8(executableDirectory()) / "packs");
}

std::vector<GraphicsPackFontFile> graphicsPackFonts() {
  auto& r = registry();
  std::lock_guard lock(r.mutex);
  scanLocked(r);
  return r.fonts;
}

std::optional<GraphicsPackFontFile> graphicsPackFontDeclaration(const std::string& family) {
  if (family.empty() || family.size() > 256) return std::nullopt;
  auto& r = registry();
  std::lock_guard lock(r.mutex);
  scanLocked(r);
  const auto wanted = lower(family);
  std::optional<GraphicsPackFontFile> found;
  for (const auto& font : r.fonts) {
    if (lower(font.family) != wanted) continue;
    // Any present face makes the family available; report the first one.
    if (!found || (!found->present && font.present)) found = font;
  }
  return found;
}

uint64_t graphicsPackFontsGeneration() {
  // Scanning is lazy; make sure a caller keying a cache on the generation sees
  // the scan the generation names.
  auto& r = registry();
  std::lock_guard lock(r.mutex);
  scanLocked(r);
  return gGeneration.load();
}

std::string graphicsPackFontFallback(const GraphicsPackFontFile& font) {
  return font.fallback.empty() ? std::string("Segoe UI") : font.fallback;
}

std::string graphicsPackFontSubstitutionWarning(const std::string& family, const std::string& packId,
                                                const std::string& substitute) {
  return "Font " + family + " is missing from pack " + packId + "; using " + substitute + ".";
}

std::string graphicsPackFontMissingError(const std::string& family, const std::string& packId) {
  return "Pack font is missing: " + family + " (pack " + packId + ")";
}

bool graphicsPackFontsStrict() {
  const int forced = gStrictOverride.load();
  if (forced >= 0) return forced == 1;
  const char* value = std::getenv("COREVIDEO_GRAPHICS_STRICT_PACK_FONTS");
  return value && std::string(value) == "1";
}

void setGraphicsPackFontsStrictForTesting(bool strict) { gStrictOverride.store(strict ? 1 : 0); }

void setGraphicsPacksDirectoryForTesting(const std::string& dir) {
  auto& r = registry();
  std::lock_guard lock(r.mutex);
  r.overrideDirectory = dir;
  r.scanned = false;
  gGeneration.fetch_add(1);
}

}  // namespace corevideo::graphics
