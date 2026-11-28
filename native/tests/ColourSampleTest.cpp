#include "core/ColourSample.h"
#include "core/MediaCore.h"
#include "modules/ProgramFramePreview.h"
#include "gtest/gtest.h"

using corevideo::core::ColourSample;

namespace {
std::string Encode(const uint8_t*, size_t size) { return "bytes:" + std::to_string(size); }
// A 4x2 BGRA frame: left half red, right half blue, bottom-right pixel white.
std::vector<uint8_t> Frame() {
  std::vector<uint8_t> bgra;
  for (int y = 0; y < 2; ++y)
    for (int x = 0; x < 4; ++x) {
      if (x == 3 && y == 1) { bgra.insert(bgra.end(), {255, 255, 255, 255}); continue; }
      if (x < 2) bgra.insert(bgra.end(), {0, 0, 230, 255}); else bgra.insert(bgra.end(), {200, 40, 10, 255});
    }
  return bgra;
}
}  // namespace

// Graphics styles slice 3: the eyedropper reads the frozen Program thumbnail, never a live frame.
TEST(ColourSample, PickReadsTheFrozenPixelUnderTheNormalizedPoint) {
  ColourSample sample;
  std::string error;
  ASSERT_TRUE(sample.freeze("program", 41, 4, 2, Frame(), Encode, error));
  auto node = sample.snapshot();
  EXPECT_EQ("frozen", node.getString("state"));
  EXPECT_EQ(41, node.getNumber("frameNumber"));
  EXPECT_EQ("bytes:32", node.getString("bgraBase64"));
  ASSERT_TRUE(sample.pick(0.1, 0.2, 41, "pick-one", error));
  EXPECT_EQ("#E60000", sample.snapshot().get("lastPick")->getString("hex"));
  ASSERT_TRUE(sample.pick(0.6, 0.0, 41, "pick-two", error));
  EXPECT_EQ("#0A28C8", sample.snapshot().get("lastPick")->getString("hex"));
  ASSERT_TRUE(sample.pick(1.0, 1.0, 41, "pick-edge", error));  // the far edge is the last pixel, not past it
  const auto pick = *sample.snapshot().get("lastPick");
  EXPECT_EQ("#FFFFFF", pick.getString("hex"));
  EXPECT_EQ(3, pick.getNumber("x")); EXPECT_EQ(1, pick.getNumber("y"));
  EXPECT_EQ("pick-edge", pick.getString("requestId"));
}

TEST(ColourSample, RefusesStaleFramesOutOfRangePointsAndEmptyFrames) {
  ColourSample sample;
  std::string error;
  EXPECT_FALSE(sample.pick(0.5, 0.5, 1, "early", error));
  EXPECT_EQ("colour-sample-not-frozen", error);
  EXPECT_FALSE(sample.freeze("program", 1, 0, 0, {}, Encode, error));
  EXPECT_EQ("colour-sample-no-frame", sample.snapshot().getString("lastError"));
  EXPECT_FALSE(sample.freeze("program", 1, 640, 360, std::vector<uint8_t>(640 * 360 * 4), Encode, error));  // larger than the thumbnail
  ASSERT_TRUE(sample.freeze("program", 7, 4, 2, Frame(), Encode, error));
  EXPECT_FALSE(sample.pick(0.5, 0.5, 8, "stale", error));
  EXPECT_EQ("colour-sample-frame-changed", error);
  EXPECT_FALSE(sample.pick(1.5, 0.5, 7, "outside", error));
  EXPECT_EQ("colour-sample-out-of-frame", sample.snapshot().getString("lastError"));
  EXPECT_FALSE(sample.pick(std::nan(""), 0.5, 7, "nan", error));
  EXPECT_TRUE(sample.snapshot().get("lastPick")->isNull());
  sample.release();
  const auto released = sample.snapshot();
  EXPECT_EQ("idle", released.getString("state"));
  EXPECT_FALSE(released.get("bgraBase64"));
}

// End to end through MediaCore::applyCommands: freeze the Program thumbnail the core
// already reads back, render more frames, then pick from the frozen copy.
TEST(ColourSample, MediaCoreFreezesProgramThumbnailAndPicksFromTheFrozenCopy) {
  using corevideo::rpc::Json;
  corevideo::core::MediaCore mediaCore;
  (void)mediaCore.applyCommands(Json::Array{});
  (void)mediaCore.applyCommands(Json::Array{});
  const auto frozen = mediaCore.applyCommands(Json::Array{Json::Object{{"type", "colour-sample"}, {"action", "freeze"}, {"source", "program"}}});
  const auto* node = frozen.get("colourSample");
  ASSERT_NE(node, nullptr);
  ASSERT_EQ("frozen", node->getString("state")) << node->getString("lastError");
  const int width = int(node->getNumber("width")), height = int(node->getNumber("height"));
  const auto frameNumber = node->getNumber("frameNumber");
  const auto bytes = corevideo::modules::base64Decode(node->getString("bgraBase64"));
  ASSERT_EQ(size_t(width) * size_t(height) * 4, bytes.size());
  (void)mediaCore.applyCommands(Json::Array{});
  const auto picked = mediaCore.applyCommands(Json::Array{Json::Object{{"type", "colour-sample"}, {"action", "pick"}, {"x", 0.5}, {"y", 0.5},
      {"frameNumber", frameNumber}, {"requestId", "pick-centre"}}});
  const auto* pick = picked.get("colourSample")->get("lastPick");
  ASSERT_TRUE(pick && pick->isObject()) << picked.get("colourSample")->getString("lastError");
  EXPECT_EQ("pick-centre", pick->getString("requestId"));
  const size_t offset = (size_t(height / 2) * size_t(width) + size_t(width / 2)) * 4;
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string expected = "#";
  for (const int channel : {int(bytes[offset + 2]), int(bytes[offset + 1]), int(bytes[offset])}) { expected += hex[channel >> 4]; expected += hex[channel & 15]; }
  EXPECT_EQ(expected, pick->getString("hex"));
  const auto stale = mediaCore.applyCommands(Json::Array{Json::Object{{"type", "colour-sample"}, {"action", "pick"}, {"x", 0.5}, {"y", 0.5},
      {"frameNumber", frameNumber + 1000}, {"requestId", "pick-stale"}}});
  EXPECT_EQ("colour-sample-frame-changed", stale.get("colourSample")->getString("lastError"));
  const auto unsupported = mediaCore.applyCommands(Json::Array{Json::Object{{"type", "colour-sample"}, {"action", "freeze"}, {"source", "zoom:7"}}});
  EXPECT_EQ("colour-sample-source-unsupported", unsupported.get("colourSample")->getString("lastError"));
  const auto released = mediaCore.applyCommands(Json::Array{Json::Object{{"type", "colour-sample"}, {"action", "release"}}});
  EXPECT_EQ("idle", released.get("colourSample")->getString("state"));
  EXPECT_EQ(nullptr, released.get("colourSample")->get("bgraBase64"));
}
