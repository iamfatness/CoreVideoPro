#pragma once
#include "graphics/GraphicsAssets.h"
#include "rpc/Json.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace graphicsTest {
// A standalone PNG encoder fixture: two known RGBA pixels (opaque red and
// half-alpha green), stored deflate block, standard PNG CRC/Adler. Native WIC
// decoding and GPU composition must reproduce these independent source values.
inline std::shared_ptr<const std::vector<uint8_t>> png(uint8_t redAlpha = 255) {
  std::vector<uint8_t> b{137,80,78,71,13,10,26,10};
  const auto be = [](std::vector<uint8_t>& out, uint32_t n) { out.push_back(uint8_t(n>>24)); out.push_back(uint8_t(n>>16)); out.push_back(uint8_t(n>>8)); out.push_back(uint8_t(n)); };
  const auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    be(b, uint32_t(data.size())); const auto start = b.size();
    b.insert(b.end(), type, type+4); b.insert(b.end(), data.begin(), data.end());
    uint32_t crc = 0xffffffff;
    for (size_t i=start;i<b.size();++i) { crc ^= b[i]; for (int j=0;j<8;++j) crc = (crc>>1) ^ ((crc&1) ? 0xedb88320u : 0); }
    be(b, crc ^ 0xffffffff);
  };
  chunk("IHDR", {0,0,0,2,0,0,0,1,8,6,0,0,0});
  const std::vector<uint8_t> raw{0,255,0,0,redAlpha,0,255,0,128};
  std::vector<uint8_t> z{0x78,0x01,0x01,9,0,0xf6,0xff}; z.insert(z.end(), raw.begin(), raw.end());
  uint32_t a=1,c=0; for (auto byte:raw) { a=(a+byte)%65521; c=(c+a)%65521; } be(z,(c<<16)|a);
  chunk("IDAT",z); chunk("IEND",{}); return std::make_shared<const std::vector<uint8_t>>(std::move(b));
}
// The fixture PNG with its IDAT payload zeroed and the chunk CRC repaired:
// header valid, pixels undecodable (see OverlayRasterContractCases.h).
inline std::shared_ptr<const std::vector<uint8_t>> pngWithUndecodablePixels() {
  auto bytes = std::make_shared<std::vector<uint8_t>>(*png());
  const std::string idat = "IDAT";
  auto found = std::search(bytes->begin(), bytes->end(), idat.begin(), idat.end());
  if (found == bytes->end()) return bytes;
  const size_t typeAt = size_t(found - bytes->begin());
  const size_t length = (size_t((*bytes)[typeAt - 4]) << 24) | (size_t((*bytes)[typeAt - 3]) << 16) |
                        (size_t((*bytes)[typeAt - 2]) << 8) | size_t((*bytes)[typeAt - 1]);
  std::fill(bytes->begin() + std::ptrdiff_t(typeAt + 4), bytes->begin() + std::ptrdiff_t(typeAt + 4 + length), 0);
  uint32_t crc = 0xffffffff;  // over type + data, as GraphicsAssetFixture.h writes it
  for (size_t i = typeAt; i < typeAt + 4 + length; ++i) {
    crc ^= (*bytes)[i];
    for (int j = 0; j < 8; ++j) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
  }
  crc ^= 0xffffffff;
  const size_t crcAt = typeAt + 4 + length;
  for (int k = 0; k < 4; ++k) (*bytes)[crcAt + size_t(k)] = uint8_t(crc >> (24 - 8 * k));
  return bytes;
}

inline corevideo::rpc::Json imageDocument(const std::shared_ptr<const corevideo::graphics::GraphicsAsset>& asset) {
  using J = corevideo::rpc::Json;
  return J::Object{{"schemaVersion",1},{"templateId","packaged-logo"},{"templateVersion",1},{"name","Packaged logo"},{"family","bug"},
      {"canvas",J::Object{{"width",1920},{"height",1080}}},{"bounds",J::Object{{"width",360},{"height",180}}},
      {"fields",J::Array{}},{"brandTokens",J::Array{}},
      {"assets",J::Array{J::Object{{"id","logo"},{"file","assets/"+asset->sha256+".png"},{"sha256",asset->sha256}}}},
      {"layers",J::Array{J::Object{{"id","image"},{"type","image"},{"asset","logo"},{"x",0},{"y",0},{"width",360},{"height",180},{"fit","contain"}}}},
      {"animations",J::Object{{"in",J::Object{{"type","none"}}},{"out",J::Object{{"type","none"}}}}}};
}
struct Package {
  std::filesystem::path root, file;
  explicit Package(const std::shared_ptr<const corevideo::graphics::GraphicsAsset>& asset) {
    static std::atomic<unsigned> next{0};
    root = std::filesystem::canonical(std::filesystem::temp_directory_path()) / ("corevideo-graphics-asset-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++next));
    std::filesystem::create_directories(root/"assets"); file=root/"assets"/(asset->sha256+".png");
    std::ofstream out(file,std::ios::binary); out.write(reinterpret_cast<const char*>(asset->encoded->data()),std::streamsize(asset->encoded->size()));
  }
  ~Package() { std::error_code ec; std::filesystem::remove(file,ec); std::filesystem::remove(root/"assets",ec); std::filesystem::remove(root,ec); }
};
} // namespace graphicsTest
