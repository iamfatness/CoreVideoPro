#pragma once

// Overlay raster contract (Metal graphics backend spec 7.1; G3 Task 2).
//
// Admission and failure cases written ONCE as templates over a backend traits
// struct, instantiated by MetalOverlayRasterContractTest.mm (CoreTextOverlayRaster)
// and, in the Windows session, by D3D11GraphicsReferenceTest.cpp
// (CompositorOverlayRaster). Every expected string is copied verbatim from
// CompositorOverlayRaster.cpp rasterOverlayTexture / prepareRaster: that file is
// the reference, and these are the Windows behaviour in code, not invented
// output.
//
// A Backend supplies:
//   using Raster;                      // CoreTextOverlayRaster | CompositorOverlayRaster
//   using Device;                      // id<MTLDevice> | ID3D11Device*
//   static Device device();            // null -> print "[contract] unavailable" and return
//   static bool raster(Raster&, Device, const modules::CompositorOverlayContent&, int width, int height,
//                      bool strict, std::shared_ptr<const graphics::GraphicsPreparedResource> prepared);
//                                      // one render-loop call: beginFrame + rasterOverlayTexture; true = texture returned
//   static uint64_t failedPreparations(Raster&);   // the raster cache's `failed` counter
//   static uint64_t completedPreparations(Raster&); // the raster cache's `completed` counter
//   static std::vector<uint8_t> lastPixels(Raster&); // BGRA of the last call's texture, row-major (Metal-only cases)
//   static constexpr graphics::GraphicsPreparedResource::Backend kind;
//   static constexpr bool kCountsCacheFailures;     // false: assert state only (Windows)
//
// The vendored gtest has no GTEST_SKIP and ASSERT_* does not abort, so every
// case returns explicitly after a precondition fails.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "GraphicsAssetFixture.h"
#include "compositor/CompositorFaultInjection.h"
#include "graphics/GraphicsAssets.h"
#include "graphics/GraphicsMotion.h"
#include "graphics/GraphicsPreparedResource.h"
#include "graphics/GraphicsResourceBudget.h"
#include "graphics/GraphicsTemplate.h"
#include "modules/Interfaces.h"
#include "rpc/Json.h"

namespace overlayContract {

using corevideo::graphics::GraphicsPreparedResource;
using corevideo::modules::CompositorOverlayContent;
using PreparedPtr = std::shared_ptr<const GraphicsPreparedResource>;

#define CONTRACT_DEVICE_OR_RETURN(var)                                      \
  auto var = Backend::device();                                             \
  if (!var) {                                                               \
    std::fprintf(stderr, "[contract] unavailable: no graphics device\n");  \
    return;                                                                 \
  }

struct Outcome {
  bool texture = false;
  std::string state, warning;
};

template <class Backend>
Outcome call(typename Backend::Raster& raster, typename Backend::Device device, const CompositorOverlayContent& overlay,
             int width, int height, bool strict = false, PreparedPtr prepared = {}) {
  Outcome out;
  out.texture = Backend::raster(raster, device, overlay, width, height, strict, std::move(prepared));
  out.state = raster.preparationState();
  out.warning = raster.lastWarning();
  return out;
}

// Polls as a render loop does until a terminal state. Bounded at 5 s; asserts
// a state, never a duration.
template <class Backend>
Outcome waitTerminal(typename Backend::Raster& raster, typename Backend::Device device,
                     const CompositorOverlayContent& overlay, int width, int height, bool strict = false,
                     PreparedPtr prepared = {}) {
  Outcome out;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    out = call<Backend>(raster, device, overlay, width, height, strict, prepared);
    if (out.state == "ready" || out.state == "failed") return out;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (std::chrono::steady_clock::now() < deadline);
  return out;
}

inline bool startsWith(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}
inline bool endsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline CompositorOverlayContent legacyLowerThird(const std::string& name, const std::string& title) {
  CompositorOverlayContent overlay;
  overlay.text = name;
  overlay.title = title;
  overlay.brandColor = "#ff0000";
  return overlay;
}

// A template overlay as instances deliver it: definition, fields, brand and
// the content hash the raster keys on.
inline CompositorOverlayContent templateOverlay(const std::string& manifest,
                                                corevideo::graphics::GraphicsAssets assets = {},
                                                std::string* loadError = nullptr) {
  std::string error;
  auto input = std::make_shared<corevideo::graphics::GraphicsTemplateInput>();
  input->definition = corevideo::graphics::GraphicsTemplate::load(manifest, error, std::move(assets));
  if (loadError) *loadError = error;
  input->fields = input->definition ? input->definition->defaultFields() : corevideo::rpc::Json::Object{};
  input->brand = corevideo::rpc::Json::Object{};
  CompositorOverlayContent content;
  content.appearance.enabled = true;
  content.graphicsTemplate = input;
  if (input->definition) {
    content.graphicsContentHash = input->definition->hash();
    content.hasGraphicsContentHash = true;
  }
  return content;
}

inline std::string simpleManifest(const std::string& id, int width, int height, const std::string& layers,
                                  const std::string& fields = "[]") {
  return R"({"schemaVersion":1,"templateId":")" + id + R"(","templateVersion":1,"name":"Contract",)"
      R"("family":"title","canvas":{"width":1920,"height":1080},"bounds":{"width":)" + std::to_string(width) +
      R"(,"height":)" + std::to_string(height) + R"(},"brandTokens":[],"fields":)" + fields +
      R"(,"animations":{"in":{"type":"none"},"out":{"type":"none"}},"layers":)" + layers + "}";
}

// The packaged-logo template (GraphicsAssetFixture.h imageDocument) over the
// fixture PNG, with the asset bytes either intact or made undecodable.
//
// Fixture invariant (the corrupt image): the IDAT payload (the zlib stream) is
// zeroed and the chunk CRC repaired, so the PNG signature, IHDR (2x1, RGBA),
// chunk framing and IEND are intact and the asset passes the header checks
// (graphicsAssetFromBytes, which logoTemplate asserts, and the raster's
// openPackagedImage); only the pixel decode fails (0x0000 is not a zlib
// header). ImageIO does NOT refuse this at CGImageSourceCreateImageAtIndex: it
// returns a CGImage whose data provider yields no bytes and which draws
// nothing (measured on macOS 27.2), so the raster must force the decode to see
// the failure, as WIC's CreateBitmapFromWicBitmap does on Windows. Removing
// the IDAT chunk instead is NOT this case: ImageIO then drops the pixel size
// from the header properties and the asset fails the header check.
// sha256 is kept equal to the intact bytes, so the template's declared digest
// and its content hash are unchanged: it is "the same content" whose bytes no
// longer decode.
inline std::shared_ptr<const std::vector<uint8_t>> corruptPng() {
  auto bytes = std::make_shared<std::vector<uint8_t>>(*graphicsTest::png());
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

inline CompositorOverlayContent logoTemplate(bool corrupt, std::string* error = nullptr) {
  std::string assetError;
  auto good = corevideo::graphics::graphicsAssetFromBytes("logo", graphicsTest::png(), assetError);
  if (!good) {
    if (error) *error = assetError;
    return {};
  }
  std::shared_ptr<const corevideo::graphics::GraphicsAsset> asset = good;
  if (corrupt) {
    std::string headerError;
    if (!corevideo::graphics::graphicsAssetFromBytes("logo", corruptPng(), headerError)) {
      if (error) *error = "corrupt fixture fails the header check: " + headerError;
      return {};
    }
    auto broken = std::make_shared<corevideo::graphics::GraphicsAsset>(*good);
    broken->encoded = corruptPng();
    asset = broken;
  }
  return templateOverlay(graphicsTest::imageDocument(good).stringify(), {{"logo", asset}}, error);
}

// Process-global resident budget filler. Restores the budget on scope exit:
// later tests in the same binary depend on it.
struct ResidentHold {
  std::vector<std::shared_ptr<corevideo::graphics::GraphicsResourceBudget::Lease>> leases;
  bool fill() {
    auto& budget = corevideo::graphics::GraphicsResourceBudget::process();
    for (int i = 0; i < 64 && budget.counters().residentResources < budget.counters().residentLimit; ++i) {
      auto lease = budget.tryReserve(1, 1, "contract-hold-" + std::to_string(i), true);
      if (!lease) return false;
      lease->finishPreparation();
      leases.push_back(std::move(lease));
    }
    return budget.counters().residentResources == budget.counters().residentLimit;
  }
  void release() { leases.clear(); }
  ~ResidentHold() { release(); }
};

// Blocks the preparation worker (where the backend invokes the hook). The
// hook carries its own hard timeout; the scope releases it on exit.
struct BlockedWorker {
  static std::mutex& mutex() { static std::mutex m; return m; }
  static std::condition_variable& cv() { static std::condition_variable c; return c; }
  static std::atomic<bool>& blocked() { static std::atomic<bool> b{false}; return b; }
  static void hook() {
    std::unique_lock<std::mutex> lock(mutex());
    cv().wait_for(lock, std::chrono::milliseconds(3000), [] { return !blocked().load(); });
  }
  BlockedWorker() {
    blocked() = true;
    corevideo::compositor::setOverlayRasterPrepareHookForTest(&hook);
  }
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex());
      blocked() = false;
    }
    cv().notify_all();
  }
  ~BlockedWorker() {
    release();
    corevideo::compositor::setOverlayRasterPrepareHookForTest(nullptr);
  }
};

// Raster A prepares the legacy overlay at 600x120 and hands out its resource.
template <class Backend>
PreparedPtr prepareOn(typename Backend::Raster& raster, typename Backend::Device device,
                      const CompositorOverlayContent& overlay, int width, int height, bool strict = false) {
  const auto out = waitTerminal<Backend>(raster, device, overlay, width, height, strict);
  if (out.state != "ready") return {};
  // preparedResource() answers for the LAST call, which was the ready one.
  return raster.preparedResource();
}

// ── Cases ────────────────────────────────────────────────────────────────────

// Rule 1 (moved from MetalOverlayRaster.AdmissionRulesMatchWindowsWording).
template <class Backend> void contractMetadataBound() {
  CONTRACT_DEVICE_OR_RETURN(device);
  typename Backend::Raster raster;
  // Fixture invariant: 70000 bytes > the 65536-byte metadata bound.
  const auto out = call<Backend>(raster, device, legacyLowerThird(std::string(70000, 'x'), ""), 100, 100);
  EXPECT_FALSE(out.texture);
  EXPECT_EQ(out.state, "failed");
  EXPECT_EQ(out.warning, "Graphics content exceeds preparation metadata budget.");
  EXPECT_TRUE(raster.failedThisFrame());
}

// Rule 2 (moved from MetalOverlayRaster.AdmissionRulesMatchWindowsWording).
template <class Backend> void contractDimensions() {
  CONTRACT_DEVICE_OR_RETURN(device);
  typename Backend::Raster raster;
  // Fixture invariant: 5000 px > the 4096 px raster bound.
  const auto out = call<Backend>(raster, device, legacyLowerThird("A", ""), 5000, 100);
  EXPECT_FALSE(out.texture);
  EXPECT_EQ(out.state, "failed");
  EXPECT_EQ(out.warning, "Invalid graphics raster dimensions.");
}

// Rule 5. Fixture invariant: B has never seen O, so only adoption of P can
// return a texture on B's first call.
template <class Backend> void contractCompatibleSameSizeIsAdoptedOnTheFirstCall() {
  CONTRACT_DEVICE_OR_RETURN(device);
  const auto overlay = legacyLowerThird("ADOPTED NAME", "ADOPTED TITLE");
  typename Backend::Raster a;
  const auto prepared = prepareOn<Backend>(a, device, overlay, 600, 120);
  ASSERT_TRUE(prepared != nullptr);
  if (!prepared) return;
  EXPECT_TRUE(prepared->backend == Backend::kind);
  typename Backend::Raster b;
  const auto out = call<Backend>(b, device, overlay, 600, 120, false, prepared);
  EXPECT_TRUE(out.texture);
  EXPECT_EQ(out.state, "ready");
}

// Rule 4. A resource from a foreign device identity, or with a different
// content hash, is never adopted.
template <class Backend> void contractForeignDeviceOrHashIsNeverAdopted() {
  CONTRACT_DEVICE_OR_RETURN(device);
  const auto overlay = legacyLowerThird("FOREIGN NAME", "FOREIGN TITLE");
  typename Backend::Raster a;
  const auto prepared = prepareOn<Backend>(a, device, overlay, 600, 120);
  ASSERT_TRUE(prepared != nullptr);
  if (!prepared) return;
  {
    auto foreign = std::make_shared<GraphicsPreparedResource>(*prepared);
    foreign->deviceIdentity += 1;
    typename Backend::Raster b;
    const auto out = call<Backend>(b, device, overlay, 600, 120, false, foreign);
    EXPECT_FALSE(out.texture) << "a resource from another device was adopted";
    EXPECT_EQ(out.state, "preparing");
  }
  {
    auto rehashed = std::make_shared<GraphicsPreparedResource>(*prepared);
    rehashed->contentHash[0] ^= 1;
    typename Backend::Raster b;
    const auto out = call<Backend>(b, device, overlay, 600, 120, false, rehashed);
    EXPECT_FALSE(out.texture) << "a resource with a different content hash was adopted";
    EXPECT_EQ(out.state, "preparing");
  }
}

// Rule 7, pending. B holds P (600x120) and is asked for 1200x240 while the
// worker is blocked: it draws the accepted raster and says so.
template <class Backend> void contractScaleRetentionDrawsTheAcceptedRaster() {
  CONTRACT_DEVICE_OR_RETURN(device);
  const auto overlay = legacyLowerThird("RETAINED NAME", "RETAINED TITLE");
  typename Backend::Raster a;
  const auto prepared = prepareOn<Backend>(a, device, overlay, 600, 120);
  ASSERT_TRUE(prepared != nullptr);
  if (!prepared) return;
  BlockedWorker blocked;
  typename Backend::Raster b;
  for (int i = 0; i < 3; ++i) {
    const auto out = call<Backend>(b, device, overlay, 1200, 240, false, prepared);
    EXPECT_TRUE(out.texture) << "call " << i << ": the accepted raster was not retained";
    EXPECT_EQ(out.state, "preparing");
    EXPECT_EQ(out.warning, "Graphics scale preparation pending; retaining the accepted raster.");
  }
  blocked.release();
  const auto settled = waitTerminal<Backend>(b, device, overlay, 1200, 240, false, prepared);
  EXPECT_EQ(settled.state, "ready");
}

// Rule 7, failed. P is a strict template with an image; the new size carries
// the same content whose image bytes no longer decode.
template <class Backend> void contractFailedScaleKeepsTheAcceptedRaster() {
  CONTRACT_DEVICE_OR_RETURN(device);
  std::string error;
  const auto good = logoTemplate(false, &error);
  ASSERT_TRUE(good.graphicsTemplate && good.graphicsTemplate->definition) << error;
  const auto broken = logoTemplate(true, &error);
  ASSERT_TRUE(broken.graphicsTemplate && broken.graphicsTemplate->definition) << error;
  if (!good.graphicsTemplate || !broken.graphicsTemplate || !good.graphicsTemplate->definition ||
      !broken.graphicsTemplate->definition) return;
  // Fixture invariant: same content hash, so P stays compatible.
  ASSERT_TRUE(good.graphicsContentHash == broken.graphicsContentHash);
  typename Backend::Raster a;
  const auto prepared = prepareOn<Backend>(a, device, good, 360, 180, true);
  ASSERT_TRUE(prepared != nullptr);
  if (!prepared) return;
  typename Backend::Raster b;
  const auto out = waitTerminal<Backend>(b, device, broken, 720, 360, true, prepared);
  EXPECT_TRUE(out.texture) << "the accepted raster was dropped on a failed scale";
  EXPECT_EQ(out.state, "failed");
  EXPECT_TRUE(startsWith(out.warning, "Graphics image could not be decoded: ")) << out.warning;
  EXPECT_TRUE(endsWith(out.warning, " Retaining the accepted raster.")) << out.warning;
}

// Rule 8. With the process resident budget full, a template overlay waits
// with the capacity wording, and is prepared once capacity returns.
template <class Backend> void contractCapacityWordingAtTheResidentLimit() {
  CONTRACT_DEVICE_OR_RETURN(device);
  const auto overlay = templateOverlay(simpleManifest("contract-capacity", 320, 90,
      R"([{"id":"r","type":"rect","x":0,"y":0,"width":320,"height":90,"fill":"#3366CC"}])"));
  ASSERT_TRUE(overlay.hasGraphicsContentHash);
  if (!overlay.hasGraphicsContentHash) return;
  ResidentHold hold;
  ASSERT_TRUE(hold.fill());
  typename Backend::Raster raster;
  const auto out = call<Backend>(raster, device, overlay, 320, 90, true);
  EXPECT_FALSE(out.texture);
  EXPECT_EQ(out.state, "preparing");
  EXPECT_EQ(out.warning, "Graphics preparation pending at the shared 16-revision capacity limit.");
  hold.release();
  const auto settled = waitTerminal<Backend>(raster, device, overlay, 320, 90, true);
  EXPECT_EQ(settled.state, "ready") << settled.warning;
}

// A budget refusal is retryable, never a cached failure: after release the
// SAME content reaches ready without being changed.
template <class Backend> void contractBudgetWaitIsRetryableNotSticky() {
  CONTRACT_DEVICE_OR_RETURN(device);
  const auto overlay = templateOverlay(simpleManifest("contract-budget-wait", 320, 90,
      R"([{"id":"r","type":"rect","x":0,"y":0,"width":320,"height":90,"fill":"#CC6633"}])"));
  ASSERT_TRUE(overlay.hasGraphicsContentHash);
  if (!overlay.hasGraphicsContentHash) return;
  ResidentHold hold;
  ASSERT_TRUE(hold.fill());
  typename Backend::Raster raster;
  const auto failedBefore = Backend::failedPreparations(raster);
  const auto completedBefore = Backend::completedPreparations(raster);
  // Let the worker run against the full budget at least once.
  for (int i = 0; i < 50; ++i) {
    const auto out = call<Backend>(raster, device, overlay, 320, 90, true);
    EXPECT_NE(out.state, "failed") << "a budget wait was published as a failure (call " << i << ")";
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  // A retryable refusal is neither failed nor completed. The state loop above
  // cannot see a wrongly published failure on its own: the refusal requests a
  // cache trim, which retires the unleased failed entry before the next call.
  if (Backend::kCountsCacheFailures) {
    EXPECT_EQ(Backend::failedPreparations(raster), failedBefore);
    EXPECT_EQ(Backend::completedPreparations(raster), completedBefore) << "a budget wait was published as a result";
  }
  hold.release();
  const auto settled = waitTerminal<Backend>(raster, device, overlay, 320, 90, true);
  EXPECT_EQ(settled.state, "ready") << settled.warning;
}

// The atlas bound. Fixture invariant: six full-height layers with their own
// motion at a 4096 px raster need six 4096-px slices, more than one 16384-px
// atlas holds; the test proves that with planRuns + packAtlas first.
template <class Backend> void contractAtlasSpaceFailureWording() {
  CONTRACT_DEVICE_OR_RETURN(device);
  std::string layers = "[";
  for (int i = 0; i < 6; ++i) {
    if (i) layers += ",";
    layers += R"({"id":"bar-)" + std::to_string(i) + R"(","type":"rect","x":0,"y":0,"width":100,"height":100,)"
        R"("fill":"#20304)" + std::to_string(i) + R"(","animations":{"in":{"startMs":)" + std::to_string(i * 50) +
        R"(,"easing":"ease-out","durationMs":300,"type":"wipe","direction":"left"}}})";
  }
  layers += "]";
  std::string error;
  const auto overlay = templateOverlay(simpleManifest("contract-atlas", 100, 100, layers), {}, &error);
  ASSERT_TRUE(overlay.graphicsTemplate && overlay.graphicsTemplate->definition) << error;
  if (!overlay.graphicsTemplate || !overlay.graphicsTemplate->definition) return;
  ASSERT_TRUE(overlay.graphicsTemplate->definition->motionSummary().layered);
  corevideo::graphics::GraphicsDrawing drawing;
  ASSERT_TRUE(overlay.graphicsTemplate->definition->resolve(overlay.graphicsTemplate->fields,
      overlay.graphicsTemplate->brand, drawing, error)) << error;
  auto runs = corevideo::graphics::planRuns(drawing);
  std::vector<corevideo::graphics::GraphicsAtlasSlice> slices;
  int atlasWidth = 0, atlasHeight = 0;
  const bool packs = corevideo::graphics::packAtlas(runs, 4096.0 / drawing.height, slices, atlasWidth, atlasHeight) &&
      atlasWidth <= 16384 && atlasHeight <= 16384;
  ASSERT_FALSE(packs) << "fixture fits one atlas (" << atlasWidth << "x" << atlasHeight << "); it proves nothing";
  typename Backend::Raster raster;
  const auto out = waitTerminal<Backend>(raster, device, overlay, 4096, 4096, true);
  EXPECT_FALSE(out.texture);
  EXPECT_EQ(out.state, "failed");
  EXPECT_EQ(out.warning, "Graphics motion needs more texture space than one graphic allows; give fewer layers their own motion.");
}

// A strict template whose image header decodes but whose pixels do not.
template <class Backend> void contractImageDecodeFailureWording() {
  CONTRACT_DEVICE_OR_RETURN(device);
  std::string error;
  const auto broken = logoTemplate(true, &error);
  ASSERT_TRUE(broken.graphicsTemplate && broken.graphicsTemplate->definition) << error;
  if (!broken.graphicsTemplate || !broken.graphicsTemplate->definition) return;
  typename Backend::Raster raster;
  const auto out = waitTerminal<Backend>(raster, device, broken, 360, 180, true);
  EXPECT_FALSE(out.texture);
  EXPECT_EQ(out.state, "failed");
  EXPECT_TRUE(startsWith(out.warning, "Graphics image could not be decoded: ")) << out.warning;
}

inline std::string limitCrawlManifest() {
  // Fixture invariant: 64 items is the template crawl maximum
  // (GraphicsTemplate.cpp "template-crawl-items"), and the whole list must fit
  // its 4096-byte bound: 64 items of 60 'W' decode to 4033 bytes (64 of 64
  // would be 4289 and is refused before any raster runs). The field maxLength
  // is the manifest maximum, 4096.
  std::string items = "[";
  for (int i = 0; i < 64; ++i) items += std::string(i ? "," : "") + "\\\"" + std::string(60, 'W') + "\\\"";
  items += "]";
  return simpleManifest("contract-crawl-limit", 1920, 96,
      R"([{"id":"crawl","type":"crawl","x":0,"y":0,"width":1920,"height":96,"speed":240,"mode":"loop",)"
      R"("font":"Arial","fontSize":72,"color":"#FFFFFF","items":{"field":"crawl-items"}}])",
      R"([{"id":"crawl-items","type":"string","default":")" + items + R"(","maxLength":4096}])");
}

// Review Focus 1. A crawl at the item maximum in a long font prepares, or
// fails with the atlas wording (both are Windows-legal). It never fails any
// other way, and every call made while it prepares returns at once with
// "preparing" (no call waits for the worker).
template <class Backend> void contractCrawlAtTheItemLimitPreparesOrFailsWithAtlasWording() {
  CONTRACT_DEVICE_OR_RETURN(device);
  std::string error;
  const auto overlay = templateOverlay(limitCrawlManifest(), {}, &error);
  ASSERT_TRUE(overlay.graphicsTemplate && overlay.graphicsTemplate->definition) << error;
  if (!overlay.graphicsTemplate || !overlay.graphicsTemplate->definition) return;
  typename Backend::Raster raster;
  Outcome out;
  int calls = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
    out = call<Backend>(raster, device, overlay, 1920, 96, true);
    ++calls;
    if (out.state == "ready" || out.state == "failed") break;
    EXPECT_FALSE(out.texture);
    EXPECT_EQ(out.state, "preparing");
    if (calls < 20) continue;  // the first 20 calls back to back, no sleep
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  } while (std::chrono::steady_clock::now() < deadline);
  std::fprintf(stderr, "[contract] crawl-at-limit: %s after %d calls (%s)\n", out.state.c_str(), calls,
               out.warning.c_str());
  ASSERT_TRUE(out.state == "ready" || out.state == "failed") << out.state;
  if (out.state == "failed")
    EXPECT_EQ(out.warning, "Graphics motion needs more texture space than one graphic allows; give fewer layers their own motion.");
  else
    EXPECT_TRUE(out.texture);
}

// Review Focus 2 (Metal-only instantiation). A strict Segoe UI field with
// emoji and CJK text prepares through the substitution table and the Core
// Text cascade, with the substitution warning and real ink.
template <class Backend> void contractStrictSubstitutedTextWithEmojiAndCjkPrepares() {
  CONTRACT_DEVICE_OR_RETURN(device);
  std::string error;
  const auto overlay = templateOverlay(simpleManifest("contract-emoji-cjk", 400, 100,
      "[{\"id\":\"t\",\"type\":\"text\",\"x\":10,\"y\":10,\"width\":380,\"height\":80,"
      "\"text\":\"Caf\xC3\xA9 \xE6\x9D\xB1\xE4\xBA\xAC \xF0\x9F\x8E\xAC\",\"font\":\"Segoe UI\",\"fontSize\":48,"
      "\"color\":\"#FFFFFF\",\"maxLines\":1,\"overflow\":\"ellipsis\"}]"), {}, &error);
  ASSERT_TRUE(overlay.graphicsTemplate && overlay.graphicsTemplate->definition) << error;
  if (!overlay.graphicsTemplate || !overlay.graphicsTemplate->definition) return;
  typename Backend::Raster raster;
  const auto out = waitTerminal<Backend>(raster, device, overlay, 400, 100, true);
  EXPECT_EQ(out.state, "ready") << out.warning;
  EXPECT_EQ(out.warning, "Font Segoe UI is not installed on this Mac; using Selawik.");
  ASSERT_TRUE(out.texture);
  if (!out.texture) return;
  const auto pixels = Backend::lastPixels(raster);
  size_t ink = 0;
  for (size_t i = 3; i < pixels.size(); i += 4)
    if (pixels[i] > 128) ++ink;
  EXPECT_GT(ink, 200u) << "no glyph coverage";
}

}  // namespace overlayContract
