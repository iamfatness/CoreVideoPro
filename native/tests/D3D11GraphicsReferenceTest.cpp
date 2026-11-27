// Windows half of the graphics raster parity standard (Metal graphics backend
// G4 Task 7) and the D3D11 instantiation of the overlay raster contract
// (spec 7.1; OverlayRasterContractCases.h).
//
// Written on the Mac (no D3D11 headers there). First compiled on Windows
// 2026-11-26 (Release, Windows build 26300): it compiled unchanged. Keep it to
// CompositorOverlayRaster's public surface.
//
// - OverlayRasterContract.*: every contract case except the Metal-only emoji
//   case. A red case is a real Windows/Mac contract difference: record it, do
//   not edit the Windows raster in the parity plan.
// - GraphicsRasterReference.WriteWindowsReference: runs only when
//   COREVIDEO_WRITE_GRAPHICS_REFERENCE=<repo root> is set. Renders every
//   fixture in native/tests/fixtures/graphics-parity/index.json through
//   CompositorOverlayRaster (strict), reads the texture back through a staging
//   copy, writes native/tests/golden/windows-graphics/<name>.png (verbatim
//   premultiplied samples, see GraphicsRasterParity.h), re-reads it asserting
//   byte equality, and writes contracts/graphics-raster-reference.json.
// - GraphicsRasterReference.WindowsMatchesItsOwnReference: re-renders and
//   judges against the committed reference (catches a later Windows font or OS
//   update).

#if COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wincodec.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

#include "GraphicsRasterParity.h"
#include "OverlayRasterContractCases.h"
#include "compositor/ComPtrLite.h"
#include "compositor/CompositorOverlayRaster.h"
#include "modules/Sha256.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "version.lib")

namespace corevideo::graphics::parity {

namespace {
using corevideo::modules::ComPtrLite;

struct ComScope {
  bool ok = false;
  ComScope() { ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)); }
  ~ComScope() { if (ok) CoUninitialize(); }
};

ComPtrLite<IWICImagingFactory> wic() {
  ComPtrLite<IWICImagingFactory> factory;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put()));
  return factory;
}

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
  return out;
}
}  // namespace

// The Windows implementation of the verbatim-sample container. The WIC PNG
// codec's 8-bit RGBA format is GUID_WICPixelFormat32bppBGRA (straight in
// WIC's terms; 32bppRGBA is not a PNG codec format), so the premultiplied
// B,G,R,A bytes go in as given and are stored as the PNG's R,G,B,A samples,
// with no premultiply or gamma step: the same layout the Mac writes.
bool writeParityPng(const std::string& path, const ParityImage& image, std::string& error) {
  ComScope com;
  auto factory = wic();
  if (!factory) { error = "WIC unavailable"; return false; }
  std::vector<uint8_t> bgra = image.bgraPremultiplied;  // WritePixels takes a non-const buffer
  ComPtrLite<IWICStream> stream;
  ComPtrLite<IWICBitmapEncoder> encoder;
  ComPtrLite<IWICBitmapFrameEncode> frame;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  const auto wide = widen(path);
  if (FAILED(factory->CreateStream(stream.put())) || FAILED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE)) ||
      FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())) ||
      FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(frame.put(), nullptr)) || FAILED(frame->Initialize(nullptr)) ||
      FAILED(frame->SetSize(UINT(image.width), UINT(image.height))) || FAILED(frame->SetPixelFormat(&format)) ||
      !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) ||
      FAILED(frame->WritePixels(UINT(image.height), UINT(image.width) * 4, UINT(bgra.size()), bgra.data())) ||
      FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
    error = "PNG write failed: " + path;
    return false;
  }
  return true;
}

bool readParityPng(const std::string& path, ParityImage& image, std::string& error) {
  ComScope com;
  image = ParityImage{};
  auto factory = wic();
  ComPtrLite<IWICBitmapDecoder> decoder;
  ComPtrLite<IWICBitmapFrameDecode> frame;
  const auto wide = widen(path);
  if (!factory || FAILED(factory->CreateDecoderFromFilename(wide.c_str(), nullptr, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnDemand, decoder.put())) ||
      FAILED(decoder->GetFrame(0, frame.put()))) {
    error = "PNG read failed: " + path;
    return false;
  }
  WICPixelFormatGUID format{};
  UINT w = 0, h = 0;
  frame->GetPixelFormat(&format);
  frame->GetSize(&w, &h);
  if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
    error = "not an 8-bit RGBA PNG (verbatim samples required): " + path;
    return false;
  }
  image.bgraPremultiplied.resize(size_t(w) * h * 4);
  if (FAILED(frame->CopyPixels(nullptr, w * 4, UINT(image.bgraPremultiplied.size()), image.bgraPremultiplied.data()))) {
    error = "PNG samples unavailable: " + path;
    image = ParityImage{};
    return false;
  }
  image.width = int(w);
  image.height = int(h);
  return true;
}

}  // namespace corevideo::graphics::parity

namespace {
using namespace corevideo::graphics::parity;
using corevideo::modules::ComPtrLite;
using corevideo::rpc::Json;
namespace fs = std::filesystem;

struct D3DDevice {
  ComPtrLite<ID3D11Device> device;
  ComPtrLite<ID3D11DeviceContext> context;
  D3DDevice() {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION,
                                 device.put(), nullptr, context.put())))
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, device.put(),
                        nullptr, context.put());
  }
};
D3DDevice& sharedDevice() {
  static D3DDevice device;
  return device;
}

// Reads an SRV's texture back through a staging copy.
bool readBack(ID3D11ShaderResourceView* view, ParityImage& image) {
  if (!view) return false;
  ComPtrLite<ID3D11Resource> resource;
  view->GetResource(resource.put());
  ComPtrLite<ID3D11Texture2D> texture;
  if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put())))) return false;
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  desc.MiscFlags = 0;
  ComPtrLite<ID3D11Texture2D> staging;
  auto& d = sharedDevice();
  if (FAILED(d.device->CreateTexture2D(&desc, nullptr, staging.put()))) return false;
  d.context->CopyResource(staging.get(), texture.get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(d.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
  image.width = int(desc.Width);
  image.height = int(desc.Height);
  image.bgraPremultiplied.resize(size_t(desc.Width) * desc.Height * 4);
  for (UINT y = 0; y < desc.Height; ++y)
    std::memcpy(image.bgraPremultiplied.data() + size_t(y) * desc.Width * 4,
                static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(desc.Width) * 4);
  d.context->Unmap(staging.get(), 0);
  return true;
}

// ── D3D11 contract backend ──────────────────────────────────────────────────

struct D3D11ContractBackend {
  using Raster = corevideo::modules::CompositorOverlayRaster;
  using Device = ID3D11Device*;
  static constexpr auto kind = corevideo::graphics::GraphicsPreparedResource::Backend::D3D11;
  // CompositorOverlayRaster exposes no cache counters: assert state only.
  static constexpr bool kCountsCacheFailures = false;
  static Device device() { return sharedDevice().device.get(); }
  static bool raster(Raster& raster, Device device, const corevideo::modules::CompositorOverlayContent& overlay,
                     int width, int height, bool strict, overlayContract::PreparedPtr prepared) {
    raster.beginFrame();
    return raster.rasterOverlayTexture(device, sharedDevice().context.get(), overlay, {0.f, 0.f, 1.f, 1.f}, width,
                                       height, {}, strict, std::move(prepared)) != nullptr;
  }
  static uint64_t failedPreparations(Raster&) { return 0; }
  static uint64_t completedPreparations(Raster&) { return 0; }
  static uint64_t refusedPreparations(Raster&) { return 0; }
};
using B = D3D11ContractBackend;

// ── Reference writer ────────────────────────────────────────────────────────

struct Rendered {
  ParityImage image;
  Json layout;
  std::string state, warning;
};

Rendered renderFixture(const ParityFixture& fixture, const std::string& fixturesDir) {
  Rendered out;
  ParityInput input;
  std::string error;
  if (!parityInput(fixture, fixturesDir, input, error)) { out.state = "input-failed"; out.warning = error; return out; }
  out.layout = fixtureLayout(fixture, input);
  corevideo::modules::CompositorOverlayRaster raster;
  ID3D11ShaderResourceView* view = nullptr;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    raster.beginFrame();
    view = raster.rasterOverlayTexture(sharedDevice().device.get(), sharedDevice().context.get(), input.overlay,
                                       {0.f, 0.f, 1.f, 1.f}, input.width, input.height, {}, true);
    out.state = raster.preparationState();
    if (out.state == "ready" || out.state == "failed") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (std::chrono::steady_clock::now() < deadline);
  out.warning = raster.lastWarning();
  readBack(view, out.image);
  return out;
}

std::string hex(const std::array<uint8_t, 32>& digest) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  for (auto b : digest) { s += digits[b >> 4]; s += digits[b & 15]; }
  return s;
}
std::string fileSha256(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
  return hex(corevideo::modules::hashing::sha256(bytes.data(), bytes.size()));
}
std::string fileVersion(const std::wstring& path) {
  DWORD handle = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
  if (!size) return "unknown";
  std::vector<uint8_t> data(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return "unknown";
  VS_FIXEDFILEINFO* info = nullptr;
  UINT length = 0;
  if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) return "unknown";
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
  return buffer;
}
// A font file carries no version resource: record its size and timestamp, which
// change with every Windows font update.
std::string fontStamp(const std::wstring& name) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  const auto path = std::wstring(L"C:\\Windows\\Fonts\\") + name;
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return "missing";
  ULARGE_INTEGER time{};
  time.LowPart = data.ftLastWriteTime.dwLowDateTime;
  time.HighPart = data.ftLastWriteTime.dwHighDateTime;
  return std::to_string((uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow) + "@" + std::to_string(time.QuadPart);
}
std::string windowsBuild() {
  char build[64] = {};
  DWORD size = sizeof(build);
  if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuild",
                   RRF_RT_REG_SZ, nullptr, build, &size) != ERROR_SUCCESS) return "unknown";
  return build;
}

// Three non-transparent pixels, spread through the image, so the Mac proves it
// reads the PNG verbatim.
Json samples(const ParityImage& image) {
  Json::Array out;
  std::vector<size_t> opaque;
  for (size_t i = 0; i < image.bgraPremultiplied.size(); i += 4)
    if (image.bgraPremultiplied[i + 3] > 0) opaque.push_back(i / 4);
  if (opaque.empty()) return out;
  for (size_t k : {opaque.size() / 4, opaque.size() / 2, (3 * opaque.size()) / 4}) {
    const size_t p = opaque[std::min(k, opaque.size() - 1)];
    const auto* s = image.bgraPremultiplied.data() + p * 4;
    out.push_back(Json::Object{{"x", double(p % image.width)}, {"y", double(p / image.width)},
                               {"bgra", Json::Array{double(s[0]), double(s[1]), double(s[2]), double(s[3])}}});
  }
  return out;
}

}  // namespace

// The container first: a wrong WIC format fails here in one test, before
// any fixture is rendered.
TEST(ParityPng, RoundTripKeepsPremultipliedSamplesVerbatim) {
  ParityImage image;
  image.width = 4;
  image.height = 1;
  image.bgraPremultiplied = {10, 20, 30, 40, 0, 0, 0, 0, 255, 255, 255, 255, 1, 0, 0, 1};
  const auto path = (fs::temp_directory_path() / "corevideo-parity-roundtrip.png").string();
  std::string error;
  ASSERT_TRUE(writeParityPng(path, image, error)) << error;
  ParityImage back;
  ASSERT_TRUE(readParityPng(path, back, error)) << error;
  EXPECT_TRUE(back.bgraPremultiplied == image.bgraPremultiplied) << "PNG samples were not stored verbatim";
  std::error_code ec;
  fs::remove(path, ec);
}

TEST(OverlayRasterContract, MetadataBound) { overlayContract::contractMetadataBound<B>(); }
TEST(OverlayRasterContract, Dimensions) { overlayContract::contractDimensions<B>(); }
TEST(OverlayRasterContract, CompatibleSameSizeIsAdoptedOnTheFirstCall) {
  overlayContract::contractCompatibleSameSizeIsAdoptedOnTheFirstCall<B>();
}
TEST(OverlayRasterContract, ForeignDeviceOrHashIsNeverAdopted) { overlayContract::contractForeignDeviceOrHashIsNeverAdopted<B>(); }
// CompositorOverlayRaster::prepareRaster calls
// compositor::invokeOverlayRasterPrepareHook(), so this case blocks the D3D11
// worker the same way it blocks the Core Text worker.
TEST(OverlayRasterContract, ScaleRetentionDrawsTheAcceptedRaster) { overlayContract::contractScaleRetentionDrawsTheAcceptedRaster<B>(); }
// KNOWN RED ON D3D11 (2026-11-26), a recorded Windows/Mac difference: the
// corrupt-pixel image (valid header, zeroed IDAT payload) prepares "ready"
// with no warning on Windows, so neither the failed state nor the decode
// wording appears. Metal fails it (forced decode, outcomes bug 1). Not fixed
// here: the parity plan does not edit the Windows raster.
TEST(OverlayRasterContract, FailedScaleKeepsTheAcceptedRaster) { overlayContract::contractFailedScaleKeepsTheAcceptedRaster<B>(); }
TEST(OverlayRasterContract, CapacityWordingAtTheResidentLimit) { overlayContract::contractCapacityWordingAtTheResidentLimit<B>(); }
TEST(OverlayRasterContract, BudgetWaitIsRetryableNotSticky) { overlayContract::contractBudgetWaitIsRetryableNotSticky<B>(); }
TEST(OverlayRasterContract, AtlasSpaceFailureWording) { overlayContract::contractAtlasSpaceFailureWording<B>(); }
// KNOWN RED ON D3D11 (2026-11-26): same cause as FailedScaleKeepsTheAcceptedRaster.
TEST(OverlayRasterContract, ImageDecodeFailureWording) { overlayContract::contractImageDecodeFailureWording<B>(); }
// CompositorOverlayRaster.cpp admits through graphics::overlayAdmissionBytes,
// so a crawl at the item limit is admitted at its atlas bound on D3D11 too.
TEST(OverlayRasterContract, CrawlAtTheItemLimitPreparesOrFailsWithAtlasWording) {
  overlayContract::contractCrawlAtTheItemLimitPreparesOrFailsWithAtlasWording<B>();
}

TEST(GraphicsRasterReference, WriteWindowsReference) {
  const char* root = std::getenv("COREVIDEO_WRITE_GRAPHICS_REFERENCE");
  if (!root || !*root) {
    std::fprintf(stderr, "[reference] not requested (set COREVIDEO_WRITE_GRAPHICS_REFERENCE=<repo root>)\n");
    return;
  }
  ASSERT_TRUE(sharedDevice().device) << "no D3D11 device";
  if (!sharedDevice().device) return;  // the vendored ASSERT_* does not abort
  const std::string repo = root;
  const std::string fixturesDir = repo + "/native/tests/fixtures/graphics-parity";
  const std::string outDir = repo + "/native/tests/golden/windows-graphics";
  fs::create_directories(outDir);
  std::string error;
  const auto fixtures = loadParityIndex(fixturesDir + "/index.json", error);
  ASSERT_TRUE(error.empty()) << error;
  Json::Array rows;
  for (const auto& fixture : fixtures) {
    const auto rendered = renderFixture(fixture, fixturesDir);
    // A reference of a failure would pin the failure.
    EXPECT_EQ("ready", rendered.state) << fixture.name << ": " << rendered.warning;
    EXPECT_FALSE(rendered.image.bgraPremultiplied.empty()) << fixture.name << ": no pixels read back";
    Json::Object row{{"name", fixture.name}, {"state", rendered.state}, {"warning", rendered.warning},
                     {"layout", rendered.layout}, {"width", double(rendered.image.width)},
                     {"height", double(rendered.image.height)}};
    if (!rendered.image.bgraPremultiplied.empty()) {
      const std::string relative = "native/tests/golden/windows-graphics/" + fixture.name + ".png";
      const auto path = repo + "/" + relative;
      const bool written = writeParityPng(path, rendered.image, error);
      EXPECT_TRUE(written) << error;
      if (!written) continue;
      ParityImage back;
      const bool reread = readParityPng(path, back, error);
      EXPECT_TRUE(reread) << error;
      if (!reread) continue;
      EXPECT_TRUE(back.bgraPremultiplied == rendered.image.bgraPremultiplied) << fixture.name << ": PNG not verbatim";
      row["png"] = relative;
      row["sha256"] = fileSha256(path);
      row["samples"] = samples(rendered.image);
    }
    rows.push_back(std::move(row));
  }
  const char* commit = std::getenv("COREVIDEO_GRAPHICS_REFERENCE_COMMIT");
  const Json reference = Json::Object{
      {"schemaVersion", 1}, {"generator", "GraphicsRasterReference.WriteWindowsReference"},
      {"commit", commit ? commit : "unknown"},
      {"environment", Json::Object{{"windowsBuild", windowsBuild()},
          {"fonts", Json::Object{{"arial.ttf", fontStamp(L"arial.ttf")}, {"georgia.ttf", fontStamp(L"georgia.ttf")},
                                 {"segoeui.ttf", fontStamp(L"segoeui.ttf")}, {"seguisb.ttf", fontStamp(L"seguisb.ttf")}}},
          {"d2d1.dll", fileVersion(L"C:\\Windows\\System32\\d2d1.dll")},
          {"dwrite.dll", fileVersion(L"C:\\Windows\\System32\\dwrite.dll")}}},
      {"fixtures", std::move(rows)}};
  fs::create_directories(repo + "/contracts");
  std::ofstream(repo + "/contracts/graphics-raster-reference.json", std::ios::binary) << reference.stringify();
  std::fprintf(stderr, "[reference] wrote %zu fixtures to %s\n", fixtures.size(), outDir.c_str());
}

TEST(GraphicsRasterReference, WindowsMatchesItsOwnReference) {
  const std::string repo = COREVIDEO_REPO_ROOT;
  const auto referencePath = repo + "/contracts/graphics-raster-reference.json";
  if (!fs::exists(referencePath)) {
    std::fprintf(stderr, "[reference] UNVERIFIED: no committed Windows reference yet\n");
    return;
  }
  ASSERT_TRUE(sharedDevice().device) << "no D3D11 device";
  if (!sharedDevice().device) return;
  std::ifstream file(referencePath, std::ios::binary);
  const auto reference = Json::parse(std::string(std::istreambuf_iterator<char>(file), {}));
  ASSERT_TRUE(reference && reference->get("fixtures"));
  if (!reference || !reference->get("fixtures")) return;
  std::string error;
  const auto fixturesDir = repo + "/native/tests/fixtures/graphics-parity";
  const auto fixtures = loadParityIndex(fixturesDir + "/index.json", error);
  std::map<std::string, Json> rows;
  for (const auto& row : reference->get("fixtures")->asArray()) rows[row.getString("name")] = row;
  for (const auto& fixture : fixtures) {
    const auto found = rows.find(fixture.name);
    EXPECT_TRUE(found != rows.end()) << fixture.name << ": missing from the reference";
    if (found == rows.end() || found->second.getString("png").empty()) continue;
    EXPECT_EQ("ready", found->second.getString("state")) << fixture.name << ": the reference pins a failure";
    ParityImage committed;
    const bool read = readParityPng(repo + "/" + found->second.getString("png"), committed, error);
    EXPECT_TRUE(read) << error;
    if (!read) continue;
    const auto rendered = renderFixture(fixture, fixturesDir);
    EXPECT_EQ("ready", rendered.state) << fixture.name << ": " << rendered.warning;
    if (rendered.image.bgraPremultiplied.empty()) continue;
    const auto regions = regionsFromLayout(*found->second.get("layout"));
    for (const auto& verdict : {judgeGeometry(committed, rendered.image, regions),
                                judgeText(committed, rendered.image, regions, fixture.textTolerance),
                                judgeLayout(*found->second.get("layout"), rendered.layout)})
      for (const auto& f : verdict.failures) EXPECT_TRUE(false) << fixture.name << ": " << f;
  }
}

#endif  // COREVIDEO_WITH_D3D11 && !COREVIDEO_STUB && COREVIDEO_ENABLE_DEV_ADAPTERS
