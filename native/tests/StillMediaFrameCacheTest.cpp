// Still-image media routes (logos/bugs): scene routes referencing an image
// asset must composite REAL decoded pixels — not the colorFromParticipantId
// placeholder. The decode seam (IStillImageDecoder) is injectable so these
// tests run on every platform with a fake decoder; the WIC implementation is
// covered by the Windows-gated test at the bottom decoding a tiny generated
// PNG. Harness note: no EXPECT_NEAR in this suite — use EXPECT_LT(std::abs()).

#include "modules/StillMediaFrameCache.h"

#include "compositor/CompositorLayout.h"
#include "core/MediaCore.h"
#include "modules/Interfaces.h"
#include "modules/ProgramFramePreview.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using corevideo::modules::IStillImageDecoder;
using corevideo::modules::StillImagePixels;
using corevideo::modules::StillMediaFrameCache;

// Counting fake decoder: left half of the image is an opaque signature color,
// right half fully transparent (proves straight-alpha compositing).
class FakeStillDecoder final : public IStillImageDecoder {
 public:
  static constexpr uint8_t kBlue = 0x11;
  static constexpr uint8_t kGreen = 0x22;
  static constexpr uint8_t kRed = 0x33;

  std::optional<uint64_t> fileVersion(const std::string& path) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++statCount;
    auto it = versions.find(path);
    if (it == versions.end()) {
      return missingByDefault ? std::optional<uint64_t>{} : std::optional<uint64_t>{1};
    }
    return it->second;
  }

  bool decode(const std::string& path, StillImagePixels& out, std::string& error) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++decodeCount;
    ++decodeCountByPath[path];
    if (failDecode) {
      error = "synthetic decode failure";
      return false;
    }
    out.width = width;
    out.height = height;
    auto pixels = std::make_shared<std::vector<uint8_t>>(
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0u);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
        if (x < width / 2) {
          (*pixels)[offset + 0] = kBlue;
          (*pixels)[offset + 1] = kGreen;
          (*pixels)[offset + 2] = kRed;
          (*pixels)[offset + 3] = 0xff;
        }  // right half stays 0,0,0,0 (fully transparent)
      }
    }
    out.bgra = std::move(pixels);
    return true;
  }

  void setVersion(const std::string& path, std::optional<uint64_t> version) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (version) {
      versions[path] = *version;
    } else {
      versions.erase(path);
    }
  }

  int decodesFor(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    return decodeCountByPath[path];
  }

  std::mutex mutex_;
  std::map<std::string, uint64_t> versions;
  std::map<std::string, int> decodeCountByPath;
  int decodeCount = 0;
  int statCount = 0;
  int width = 64;
  int height = 36;
  bool failDecode = false;
  bool missingByDefault = false;
};

corevideo::rpc::Json::Object stillRouteScene(const std::string& sceneId,
                                             const std::string& assetId,
                                             const std::string& kind,
                                             const std::string& path) {
  return corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"},
      {"sceneId", sceneId},
      {"routes", corevideo::rpc::Json::Array{
                     corevideo::rpc::Json::Object{
                         {"routeId", "bug"},
                         {"mode", "fixed"},
                         {"participantId", "media:" + assetId},
                         {"mediaAssetId", assetId},
                         {"mediaAssetName", "Logo"},
                         {"mediaAssetKind", kind},
                         {"mediaAssetPath", path},
                         {"rect", corevideo::rpc::Json::Object{
                                      {"x", 0.0}, {"y", 0.0}, {"width", 1.0}, {"height", 1.0}}},
                     },
                 }},
  };
}

}  // namespace

TEST(StillMediaClassification, KindImageOrStillExtensionIsAStill) {
  using corevideo::modules::isStillImageMediaAsset;
  // kind says image -> still, regardless of extension.
  EXPECT_TRUE(isStillImageMediaAsset("image", "C:\\assets\\weird.dat"));
  EXPECT_TRUE(isStillImageMediaAsset("Image", "C:\\assets\\weird.dat"));
  // The shell's media bin classifies PNG logos under lower-third groups: the
  // extension must win regardless of kind.
  EXPECT_TRUE(isStillImageMediaAsset("lower-third", "C:\\assets\\logo.PNG"));
  EXPECT_TRUE(isStillImageMediaAsset("video", "C:\\assets\\frame.jpeg"));
  EXPECT_TRUE(isStillImageMediaAsset("", "file:///C:/assets/bug.gif"));
  EXPECT_TRUE(isStillImageMediaAsset("", "C:\\assets\\a.bmp"));
  // Real video assets are NOT stills.
  EXPECT_FALSE(isStillImageMediaAsset("video", "C:\\media\\intro.mp4"));
  EXPECT_FALSE(isStillImageMediaAsset("", "C:\\media\\clip.mov"));
}

TEST(StillMediaFrameCache, DecodesOnceAndPublishesPersistentFrame) {
  auto decoder = std::make_unique<FakeStillDecoder>();
  auto* fake = decoder.get();
  StillMediaFrameCache cache(std::move(decoder));

  cache.setDesired({{"media:logo-1", "C:\\assets\\logo.png"}});
  ASSERT_TRUE(cache.waitForIdle(5000));
  auto frames = cache.collectFrames(100);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].participantId, "media:logo-1");
  ASSERT_TRUE(frames[0].hasPixels());
  EXPECT_EQ(frames[0].pixelWidth, 64);
  EXPECT_EQ(frames[0].pixelHeight, 36);
  EXPECT_EQ(fake->decodeCount, 1);

  // Scene re-sync with the same asset must NOT re-decode (cache by path+version),
  // and the published frame stays stable (same frameId + same shared buffer =>
  // the GPU per-source texture cache never re-uploads).
  const auto firstFrameId = frames[0].frameId;
  const auto* firstBuffer = frames[0].pixels->data();
  cache.setDesired({{"media:logo-1", "C:\\assets\\logo.png"}});
  ASSERT_TRUE(cache.waitForIdle(5000));
  frames = cache.collectFrames(200);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(fake->decodeCount, 1);
  EXPECT_EQ(frames[0].frameId, firstFrameId);
  EXPECT_EQ(frames[0].pixels->data(), firstBuffer);
}

TEST(StillMediaFrameCache, FileVersionChangeRedecodes) {
  auto decoder = std::make_unique<FakeStillDecoder>();
  auto* fake = decoder.get();
  StillMediaFrameCache cache(std::move(decoder));

  cache.setDesired({{"media:logo-1", "C:\\assets\\logo.png"}});
  ASSERT_TRUE(cache.waitForIdle(5000));
  EXPECT_EQ(fake->decodeCount, 1);

  // The file changed on disk (new mtime/size): the next scene sync re-decodes.
  fake->setVersion("C:\\assets\\logo.png", 2);
  cache.setDesired({{"media:logo-1", "C:\\assets\\logo.png"}});
  ASSERT_TRUE(cache.waitForIdle(5000));
  EXPECT_EQ(fake->decodeCount, 2);
  const auto frames = cache.collectFrames(300);
  ASSERT_EQ(frames.size(), 1u);
  ASSERT_TRUE(frames[0].hasPixels());
}

TEST(StillMediaFrameCache, MissingFileKeepsPlaceholderAndWarns) {
  auto decoder = std::make_unique<FakeStillDecoder>();
  decoder->missingByDefault = true;
  StillMediaFrameCache cache(std::move(decoder));

  cache.setDesired({{"media:ghost", "C:\\assets\\missing.png"}});
  ASSERT_TRUE(cache.waitForIdle(5000));
  EXPECT_TRUE(cache.collectFrames(100).empty());  // placeholder behavior stands
  const auto warnings = cache.warnings();
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("media:ghost"), std::string::npos);
  EXPECT_NE(warnings[0].find("C:\\assets\\missing.png"), std::string::npos);
}

TEST(StillMediaFrameCache, DecodeFailureKeepsPlaceholderAndWarns) {
  auto decoder = std::make_unique<FakeStillDecoder>();
  decoder->failDecode = true;
  StillMediaFrameCache cache(std::move(decoder));

  cache.setDesired({{"media:bad", "C:\\assets\\corrupt.png"}});
  ASSERT_TRUE(cache.waitForIdle(5000));
  EXPECT_TRUE(cache.collectFrames(100).empty());
  const auto warnings = cache.warnings();
  ASSERT_EQ(warnings.size(), 1u);
  EXPECT_NE(warnings[0].find("synthetic decode failure"), std::string::npos);
}

TEST(StillMediaFrameCache, OversizedStillIsDownscaledDuringDecode) {
  auto decoder = std::make_unique<FakeStillDecoder>();
  decoder->width = 7680;  // 2x the program max in both axes
  decoder->height = 4320;
  StillMediaFrameCache cache(std::move(decoder));

  cache.setDesired({{"media:huge", "C:\\assets\\huge.png"}});
  ASSERT_TRUE(cache.waitForIdle(15000));
  const auto frames = cache.collectFrames(100);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].pixelWidth, StillMediaFrameCache::kMaxStillWidth);
  EXPECT_EQ(frames[0].pixelHeight, StillMediaFrameCache::kMaxStillHeight);
  EXPECT_EQ(frames[0].pixels->size(),
            static_cast<size_t>(StillMediaFrameCache::kMaxStillWidth) *
                static_cast<size_t>(StillMediaFrameCache::kMaxStillHeight) * 4u);
}

TEST(StillMediaFrameCache, EvictsLeastRecentlyUsedWhenOverBudget) {
  auto decoder = std::make_unique<FakeStillDecoder>();
  auto* fake = decoder.get();
  fake->width = 160;
  fake->height = 90;  // 160*90*4 = 57600 bytes per still
  // Budget fits ONE still: desiring B while A is no longer referenced evicts A.
  StillMediaFrameCache cache(std::move(decoder), /*cacheBudgetBytes=*/100000);

  cache.setDesired({{"media:a", "C:\\assets\\a.png"}});
  ASSERT_TRUE(cache.waitForIdle(5000));
  EXPECT_EQ(fake->decodesFor("C:\\assets\\a.png"), 1);

  cache.setDesired({{"media:b", "C:\\assets\\b.png"}});
  ASSERT_TRUE(cache.waitForIdle(5000));
  EXPECT_EQ(fake->decodesFor("C:\\assets\\b.png"), 1);

  // A was evicted (loud log on stderr); re-desiring it re-decodes.
  cache.setDesired({{"media:a", "C:\\assets\\a.png"}});
  ASSERT_TRUE(cache.waitForIdle(5000));
  EXPECT_EQ(fake->decodesFor("C:\\assets\\a.png"), 2);
}

// End-to-end: a still-media scene route composites the decoded pixels into the
// program preview (stub compositor path — mirrors the
// PerRouteOpacityReachesTheCompositedPixels / CompositesMediaRoutePixels
// pattern), including straight-alpha transparency, and a scene re-sync does
// not re-decode.
TEST(StillMediaFrameCache, StillMediaRouteCompositesDecodedPixels) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  auto decoder = std::make_unique<FakeStillDecoder>();
  auto* fake = decoder.get();
  mediaCore.setStillImageDecoderForTest(std::move(decoder));

  // The media bin files PNG logos under "lower-third" kinds — the extension
  // must classify this as a still.
  const auto scene = stillRouteScene("bug-scene", "logo-1", "lower-third", "C:\\assets\\logo.png");
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{scene});
  (void)mediaCore.drainProgramFramePreviewEvents();

  ASSERT_NE(mediaCore.stillMediaCacheForTest(), nullptr);
  ASSERT_TRUE(mediaCore.stillMediaCacheForTest()->waitForIdle(5000));
  // The program-frame-preview event is throttled (~30fps); space the second
  // tick out so it re-emits with the decoded pixels.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{scene});

  EXPECT_EQ(fake->decodeCount, 1);  // the re-sync did NOT re-decode

  const auto previews = mediaCore.drainProgramFramePreviewEvents();
  ASSERT_FALSE(previews.empty());
  const auto* preview = previews.back().get("preview");
  ASSERT_NE(preview, nullptr);
  const int previewWidth = static_cast<int>(preview->get("width")->asNumber());
  const int previewHeight = static_cast<int>(preview->get("height")->asNumber());
  const auto decoded = corevideo::modules::base64Decode(preview->getString("bgraBase64"));
  ASSERT_EQ(decoded.size(),
            static_cast<size_t>(previewWidth) * static_cast<size_t>(previewHeight) * 4u);

  // Left half of the (full-canvas) still is the opaque signature color.
  const size_t leftOffset =
      ((static_cast<size_t>(previewHeight / 2) * previewWidth) + previewWidth / 4) * 4u;
  ASSERT_TRUE(leftOffset + 3 < decoded.size());
  EXPECT_EQ(decoded[leftOffset + 0], FakeStillDecoder::kBlue);
  EXPECT_EQ(decoded[leftOffset + 1], FakeStillDecoder::kGreen);
  EXPECT_EQ(decoded[leftOffset + 2], FakeStillDecoder::kRed);

  // Right half is fully transparent: the preview background survives (straight
  // alpha compositing), and neither half shows the synthetic placeholder color.
  const size_t rightOffset =
      ((static_cast<size_t>(previewHeight / 2) * previewWidth) + (previewWidth * 3) / 4) * 4u;
  ASSERT_TRUE(rightOffset + 3 < decoded.size());
  EXPECT_EQ(decoded[rightOffset + 0], 0x18);  // 0xff0c1118 preview background
  EXPECT_EQ(decoded[rightOffset + 1], 0x11);
  EXPECT_EQ(decoded[rightOffset + 2], 0x0c);

  const uint32_t placeholder = corevideo::compositor::colorFromParticipantId("media:logo-1");
  const bool leftIsPlaceholder =
      decoded[leftOffset + 0] == static_cast<uint8_t>(placeholder & 0xff) &&
      decoded[leftOffset + 1] == static_cast<uint8_t>((placeholder >> 8) & 0xff) &&
      decoded[leftOffset + 2] == static_cast<uint8_t>((placeholder >> 16) & 0xff);
  EXPECT_FALSE(leftIsPlaceholder);
}

TEST(StillMediaFrameCache, KindImageRouteIsDecodedRegardlessOfExtension) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  auto decoder = std::make_unique<FakeStillDecoder>();
  auto* fake = decoder.get();
  mediaCore.setStillImageDecoderForTest(std::move(decoder));

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      stillRouteScene("kind-scene", "still-2", "image", "C:\\assets\\slide.export")});
  ASSERT_TRUE(mediaCore.stillMediaCacheForTest()->waitForIdle(5000));
  EXPECT_EQ(fake->decodeCount, 1);
  const auto frames = mediaCore.stillMediaCacheForTest()->collectFrames(100);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].participantId, "media:still-2");
}

TEST(StillMediaFrameCache, VideoMediaRoutesStayOffTheStillPath) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  auto decoder = std::make_unique<FakeStillDecoder>();
  auto* fake = decoder.get();
  mediaCore.setStillImageDecoderForTest(std::move(decoder));

  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      stillRouteScene("video-scene", "clip-1", "video", "C:\\media\\intro.mp4")});
  ASSERT_TRUE(mediaCore.stillMediaCacheForTest()->waitForIdle(5000));
  EXPECT_EQ(fake->decodeCount, 0);  // live video media = follow-up, not this path
  EXPECT_TRUE(mediaCore.stillMediaCacheForTest()->collectFrames(100).empty());
}

TEST(StillMediaFrameCache, PreviewSceneStillRoutesAreDecodedToo) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  auto decoder = std::make_unique<FakeStillDecoder>();
  auto* fake = decoder.get();
  mediaCore.setStillImageDecoderForTest(std::move(decoder));

  // ONLY the preview scene references the still (second parse site).
  (void)mediaCore.applyCommand(corevideo::rpc::Json::Object{
      {"type", "set-preview-scene"},
      {"sceneId", "pvw-bug"},
      {"routes", corevideo::rpc::Json::Array{
                     corevideo::rpc::Json::Object{
                         {"routeId", "pvw-logo"},
                         {"mode", "fixed"},
                         {"participantId", "media:logo-9"},
                         {"mediaAssetId", "logo-9"},
                         {"mediaAssetKind", "lower-third"},
                         {"mediaAssetPath", "C:\\assets\\pvw-logo.png"},
                         {"rect", corevideo::rpc::Json::Object{
                                      {"x", 0.8}, {"y", 0.05}, {"width", 0.15}, {"height", 0.15}}},
                     },
                 }},
  });
  ASSERT_TRUE(mediaCore.stillMediaCacheForTest()->waitForIdle(5000));
  EXPECT_EQ(fake->decodeCount, 1);
  const auto frames = mediaCore.stillMediaCacheForTest()->collectFrames(100);
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].participantId, "media:logo-9");
}

#if defined(_WIN32)
// Windows-gated coverage of the REAL WIC decoder: a tiny generated 2x2 PNG
// (red / green / blue / transparent) written from a hardcoded byte array must
// decode to straight-alpha BGRA with the alpha preserved.
TEST(StillMediaFrameCache, WicDecodesGeneratedPngWithAlpha) {
  static const unsigned char kPng[] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
      0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72,
      0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xae, 0xce, 0x1c,
      0xe9, 0x00, 0x00, 0x00, 0x04, 0x67, 0x41, 0x4d, 0x41, 0x00, 0x00, 0xb1, 0x8f, 0x0b, 0xfc,
      0x61, 0x05, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0e, 0xc3, 0x00,
      0x00, 0x0e, 0xc3, 0x01, 0xc7, 0x6f, 0xa8, 0x64, 0x00, 0x00, 0x00, 0x14, 0x49, 0x44, 0x41,
      0x54, 0x18, 0x57, 0x63, 0xf8, 0xcf, 0xc0, 0xf0, 0x1f, 0x0c, 0x19, 0x18, 0xfe, 0x83, 0x08,
      0x06, 0x00, 0x3f, 0xd2, 0x05, 0xfb, 0x8e, 0xe8, 0xfc, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x49,
      0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};

  const auto path = std::filesystem::temp_directory_path() / "corevideo-still-media-test.png";
  {
    std::FILE* file = nullptr;
    ASSERT_EQ(fopen_s(&file, path.string().c_str(), "wb"), 0);
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(std::fwrite(kPng, 1, sizeof(kPng), file), sizeof(kPng));
    std::fclose(file);
  }

  auto decoder = corevideo::modules::createPlatformStillImageDecoder();
  ASSERT_NE(decoder, nullptr);
  EXPECT_TRUE(decoder->fileVersion(path.string()).has_value());

  StillImagePixels out;
  std::string error;
  ASSERT_TRUE(decoder->decode(path.string(), out, error)) << error;
  ASSERT_EQ(out.width, 2);
  ASSERT_EQ(out.height, 2);
  ASSERT_NE(out.bgra, nullptr);
  ASSERT_EQ(out.bgra->size(), 16u);
  const auto& px = *out.bgra;
  // (0,0) opaque red — BGRA.
  EXPECT_EQ(px[0], 0x00);
  EXPECT_EQ(px[1], 0x00);
  EXPECT_EQ(px[2], 0xff);
  EXPECT_EQ(px[3], 0xff);
  // (1,0) opaque green.
  EXPECT_EQ(px[4], 0x00);
  EXPECT_EQ(px[5], 0xff);
  EXPECT_EQ(px[6], 0x00);
  EXPECT_EQ(px[7], 0xff);
  // (0,1) opaque blue.
  EXPECT_EQ(px[8], 0xff);
  EXPECT_EQ(px[9], 0x00);
  EXPECT_EQ(px[10], 0x00);
  EXPECT_EQ(px[11], 0xff);
  // (1,1) fully transparent — the alpha channel must survive the convert.
  EXPECT_EQ(px[15], 0x00);

  std::error_code cleanup;
  std::filesystem::remove(path, cleanup);
}

TEST(StillMediaFrameCache, WicReportsMissingFileAsUnreadable) {
  auto decoder = corevideo::modules::createPlatformStillImageDecoder();
  ASSERT_NE(decoder, nullptr);
  EXPECT_FALSE(decoder->fileVersion("C:\\definitely\\not\\a\\real\\file.png").has_value());
}
#endif  // _WIN32
