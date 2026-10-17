#include "graphics/GraphicsAssets.h"
#include "modules/Sha256.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace corevideo::graphics {
namespace {
void need(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
bool id(const std::string& text) {
  return !text.empty() && text.size() <= 64 && text[0] >= 'a' && text[0] <= 'z' &&
      std::all_of(text.begin(), text.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
std::string hash(const std::vector<uint8_t>& bytes) {
  const auto digest = modules::hashing::sha256(bytes.data(), bytes.size());
  static constexpr char hex[] = "0123456789abcdef"; std::string result;
  for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; } return result;
}
uint32_t be32(const std::vector<uint8_t>& b, size_t i) {
  return (uint32_t(b[i]) << 24) | (uint32_t(b[i + 1]) << 16) | (uint32_t(b[i + 2]) << 8) | b[i + 3];
}
void header(const std::vector<uint8_t>& b, int& width, int& height) {
  constexpr uint8_t png[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (b.size() >= 33 && std::equal(std::begin(png), std::end(png), b.begin())) {
    need(be32(b, 8) == 13 && b[12] == 'I' && b[13] == 'H' && b[14] == 'D' && b[15] == 'R', "graphics-png-header");
    const auto w = be32(b, 16), h = be32(b, 20);
    need(w > 0 && w <= 4096 && h > 0 && h <= 4096 && uint64_t(w) * h <= 16ull * 1024 * 1024, "graphics-image-dimensions");
    width = int(w); height = int(h); return;
  }
  need(b.size() >= 4 && b[0] == 0xff && b[1] == 0xd8, "graphics-image-format");
  for (size_t p = 2; p + 4 <= b.size();) {
    need(b[p++] == 0xff, "graphics-jpeg-marker");
    while (p < b.size() && b[p] == 0xff) ++p;
    need(p < b.size(), "graphics-jpeg-marker"); const auto marker = b[p++];
    if (marker == 0xd9 || marker == 0xda) break;
    if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
    need(p + 2 <= b.size(), "graphics-jpeg-segment");
    const size_t length = (size_t(b[p]) << 8) | b[p + 1]; need(length >= 2 && length <= b.size() - p, "graphics-jpeg-segment");
    if (marker == 0xc0 || marker == 0xc1 || marker == 0xc2) {
      need(length >= 8, "graphics-jpeg-header");
      height = (int(b[p + 3]) << 8) | b[p + 4]; width = (int(b[p + 5]) << 8) | b[p + 6];
      need(width > 0 && width <= 4096 && height > 0 && height <= 4096 && uint64_t(width) * height <= 16ull * 1024 * 1024, "graphics-image-dimensions"); return;
    }
    p += length;
  }
  need(false, "graphics-jpeg-header");
}
void noLinks(std::filesystem::path path) {
  for (;;) {
    std::error_code ec; const auto status = std::filesystem::symlink_status(path, ec);
    need(!std::filesystem::is_symlink(status), "graphics-package-link");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    need(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT), "graphics-package-reparse-point");
#endif
    const auto parent = path.parent_path(); if (parent.empty() || parent == path) break; path = parent;
  }
}
}
std::shared_ptr<const GraphicsAsset> graphicsAssetFromBytes(const std::string& assetId,
    std::shared_ptr<const std::vector<uint8_t>> bytes, std::string& error) {
  try {
    need(id(assetId) && bytes && !bytes->empty() && bytes->size() <= 32 * 1024 * 1024, "graphics-asset-size-or-id");
    auto asset = std::make_shared<GraphicsAsset>(); asset->id = assetId; asset->encoded = std::move(bytes);
    header(*asset->encoded, asset->width, asset->height); asset->sha256 = hash(*asset->encoded); error.clear(); return asset;
  } catch (const std::exception& e) { error = e.what(); return {}; }
}
bool loadGraphicsAssets(const rpc::Json& document, const std::string& packageRoot, GraphicsAssets& assets, std::string& error) {
  try {
    assets.clear(); const auto* declarations = document.get("assets");
    if (!declarations || (declarations->isArray() && declarations->asArray().empty())) { error.clear(); return true; }
    need(declarations->isArray() && declarations->asArray().size() <= 127, "graphics-assets-limit");
    need(!packageRoot.empty() && packageRoot.size() <= 2048 && packageRoot.find('\0') == std::string::npos, "graphics-package-root-required");
    const auto root = std::filesystem::absolute(std::filesystem::u8path(packageRoot)).lexically_normal();
    noLinks(root); need(std::filesystem::is_directory(root), "graphics-package-root-unavailable");
    std::set<std::string> files, ids; size_t bytes = 0;
    for (const auto& declaration : declarations->asArray()) {
      need(declaration.isObject(), "graphics-asset-declaration");
      for (const auto& entry : declaration.asObject()) need(entry.first == "id" || entry.first == "file" || entry.first == "sha256" || entry.first == "optional" || entry.first == "metadata", "graphics-asset-property");
      const auto assetId = declaration.getString("id"), file = declaration.getString("file"), digest = declaration.getString("sha256");
      need(id(assetId) && ids.insert(assetId).second && digest.size() == 64 && std::all_of(digest.begin(), digest.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }), "graphics-asset-id-or-hash");
      const auto extension = std::filesystem::u8path(file).extension().string();
      need((extension == ".png" || extension == ".jpg" || extension == ".jpeg") && file == "assets/" + digest + extension && files.insert(file).second, "graphics-asset-path");
      const auto path = root / std::filesystem::u8path(file); noLinks(path);
      const auto* optional = declaration.get("optional"); need(!optional || optional->isBool(), "graphics-asset-optional");
      if (optional && optional->asBool() && !std::filesystem::exists(path)) continue;
      const auto length = std::filesystem::file_size(path);
      need(length > 0 && length <= 32 * 1024 * 1024 && length <= 128 * 1024 * 1024 - bytes, "graphics-package-asset-bytes");
      std::ifstream input(path, std::ios::binary); need(bool(input), "graphics-asset-open");
      auto data = std::make_shared<std::vector<uint8_t>>(size_t(length));
      input.read(reinterpret_cast<char*>(data->data()), std::streamsize(length));
      need(input.gcount() == std::streamsize(length) && input.peek() == std::char_traits<char>::eof(), "graphics-asset-changed-during-read");
      auto asset = graphicsAssetFromBytes(assetId, data, error); if (!asset) throw std::runtime_error(error);
      need(asset->sha256 == digest, "graphics-asset-hash-mismatch");
      const bool jpeg = data->size() >= 2 && (*data)[0] == 0xff && (*data)[1] == 0xd8;
      need((extension == ".png") != jpeg, "graphics-asset-extension-mismatch");
      bytes += size_t(length); assets.emplace(assetId, std::move(asset));
    }
    error.clear(); return true;
  } catch (const std::exception& e) { assets.clear(); error = e.what(); return false; }
}
} // namespace corevideo::graphics
