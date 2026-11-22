// Sources Phase 2 plan, Task 9 (S6): the source sheet. One fixed 5x2 IOSurface; cell N is
// Show Input slot N+1, ALWAYS, whatever the wall's layout or the order of its list. That is
// what makes a row thumbnail identity-safe by construction (the Windows rule that a wall
// reflow can never supply or move a thumbnail: ShowInputsCoordinatorTests
// .CompositeLayoutCannotSupplyOrMoveAnIdentityBoundThumbnail).
#include "core/SourceSheetPlan.h"
#include "core/MediaCore.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <random>

using corevideo::core::SourceSheetInput;
namespace modules = corevideo::modules;

namespace {
SourceSheetInput zoom(int slot, const std::string& pid) {
  return {slot, corevideo::core::resolveMultiviewFeed("participant-video", "zoom:" + pid, pid, "", "")};
}
}  // namespace

// Quantified over lists of 1..10 sources, each in every order a seeded shuffle produces.
// FIXTURE INVARIANT: slot numbers are unique and in 0..9, so each must own exactly one cell.
TEST(SourceSheetPlan, CellIsTheSlotForEveryListOrderAndSize) {
  std::mt19937 shuffle(1215);
  for (int count = 1; count <= 10; ++count) {
    for (int round = 0; round < 6; ++round) {
      std::vector<int> slots(10);
      for (int i = 0; i < 10; ++i) slots[i] = i;
      std::shuffle(slots.begin(), slots.end(), shuffle);
      std::vector<SourceSheetInput> inputs;
      for (int i = 0; i < count; ++i) inputs.push_back(zoom(slots[i], std::to_string(100 + slots[i])));
      const auto plan = corevideo::core::buildSourceSheetRenderPlan(inputs);
      ASSERT_EQ(plan.width, 3200);
      ASSERT_EQ(plan.height, 720);
      ASSERT_EQ(plan.layers.size(), static_cast<size_t>(count)) << "one layer per assigned slot";
      for (const auto& layer : plan.layers) {
        const int slot = std::stoi(layer.participantId) - 100;      // the fixture's own numbering
        EXPECT_NEAR(layer.rect.x, static_cast<float>(slot % 5) / 5.f, 1e-6f) << "slot " << slot;
        EXPECT_NEAR(layer.rect.y, static_cast<float>(slot / 5) / 2.f, 1e-6f) << "slot " << slot;
        EXPECT_NEAR(layer.rect.width, 0.2f, 1e-6f);
        EXPECT_NEAR(layer.rect.height, 0.5f, 1e-6f);
        EXPECT_EQ(layer.borderStyle, "none") << "no border: the shell draws the tally";
        EXPECT_NEAR(layer.borderThickness, 0.f, 1e-6f);
        EXPECT_EQ(layer.fitMode, "fit");
      }
    }
  }
}

TEST(SourceSheetPlan, AnOutOfRangeOrDuplicateSlotGetsNoCell) {
  const auto plan = corevideo::core::buildSourceSheetRenderPlan(
      {zoom(-1, "1"), zoom(10, "2"), zoom(3, "3"), zoom(3, "4")});
  ASSERT_EQ(plan.layers.size(), 1u) << "only slot 3, first claim wins";
  EXPECT_EQ(plan.layers[0].participantId, "3");
}

TEST(SourceSheetPlan, EveryCellIsReportedWithItsState) {
  auto plan = corevideo::core::buildSourceSheetRenderPlan({zoom(0, "101"), zoom(1, "102"), zoom(6, "107")});
  for (auto& layer : plan.layers) layer.sourceHealth = layer.participantId == "102" ? "stalled" : "producing";
  modules::VideoFrame a; a.participantId = "101"; a.pixelWidth = a.pixelHeight = 2; a.pixelStride = 8;
  a.pixels = std::make_shared<std::vector<uint8_t>>(16, 0xff);
  modules::VideoFrame b = a; b.participantId = "102";
  const auto cells = corevideo::core::sourceSheetCells(plan, {a, b});
  ASSERT_EQ(cells.size(), 10u) << "all ten cells, assigned or not";
  std::map<int, std::pair<std::string, std::string>> bySlot;
  for (const auto& cell : cells) bySlot[cell.slot] = {cell.sourceId, cell.state};
  EXPECT_EQ(bySlot[0], (std::pair<std::string, std::string>{"zoom:101", "live"}));
  EXPECT_EQ(bySlot[1], (std::pair<std::string, std::string>{"zoom:102", "held"})) << "a stalled frame is held";
  EXPECT_EQ(bySlot[6], (std::pair<std::string, std::string>{"zoom:107", "empty"})) << "no frame: empty";
  EXPECT_EQ(bySlot[2], (std::pair<std::string, std::string>{"", "empty"})) << "an unassigned slot";
}

// ── MediaCore: the pass runs only while the Sources tab holds the demand lease ──

namespace {
class CountingCompositor final : public modules::ICompositor {
 public:
  std::string rendererName() const override { return "counting"; }
  modules::ProgramFrame render(const modules::CompositorRenderPlan&, const std::vector<modules::VideoFrame>&) override {
    return {};
  }
  modules::ProgramFrameSharedTexture renderSourceSheet(const modules::CompositorRenderPlan& plan,
                                                      const std::vector<modules::VideoFrame>&) override {
    ++sheets;
    lastPlan = plan;
    modules::ProgramFrameSharedTexture out;
    out.iosurfaceId = 77; out.width = plan.width; out.height = plan.height;
    return out;
  }
  int sheets = 0;
  modules::CompositorRenderPlan lastPlan;
};

corevideo::rpc::Json::Object wall() {
  using corevideo::rpc::Json;
  return Json::Object{{"type", "set-multiview-layout"}, {"canvasWidth", 1920}, {"canvasHeight", 1080},
                      {"sources", Json::Array{Json::Object{{"sourceId", "zoom:101"}, {"kind", "participant-video"},
                                                           {"participantId", "101"}, {"slot", 4}, {"label", "A"}}}}};
}
}  // namespace

TEST(MediaCoreSourceSheet, TheDemandLeaseGatesThePassAndTheNode) {
  auto modules = modules::createStubModules();
  auto counting = std::make_unique<CountingCompositor>();
  auto* compositor = counting.get();
  modules.compositor = std::move(counting);
  corevideo::core::MediaCore core(std::move(modules));
  using corevideo::rpc::Json;
  (void)core.applyCommands(Json::Array{wall()});
  for (int i = 0; i < 40; ++i) (void)core.applyCommands(Json::Array{});
  EXPECT_EQ(compositor->sheets, 0) << "no demand: no pass at all";
  const auto off = core.sessionState();
  ASSERT_NE(off.get("sourceSheet"), nullptr) << "the node is published unconditionally";
  EXPECT_FALSE(off.get("sourceSheet")->get("active")->asBool());

  (void)core.applyCommands(Json::Array{Json::Object{{"type", "set-source-preview-demand"}, {"enabled", true}}});
  for (int i = 0; i < 40; ++i) (void)core.applyCommands(Json::Array{});
  EXPECT_GE(compositor->sheets, 9) << "every 4th tick (+ the first) over 41 ticks";
  EXPECT_LE(compositor->sheets, 12) << "never every tick";
  ASSERT_EQ(compositor->lastPlan.layers.size(), 1u);
  EXPECT_NEAR(compositor->lastPlan.layers[0].rect.x, 0.8f, 1e-6f) << "slot 4 is cell 4, not list index 0";
  const auto on = core.sessionState();
  const auto* sheet = on.get("sourceSheet");
  EXPECT_TRUE(sheet->get("active")->asBool());
  EXPECT_EQ(sheet->get("texture")->getNumber("iosurfaceId"), 77);
  EXPECT_EQ(sheet->getNumber("columns"), 5);
  EXPECT_EQ(sheet->get("cells")->asArray().size(), 10u);
  EXPECT_EQ(sheet->get("cells")->asArray()[4].getString("sourceId"), "zoom:101");

  (void)core.applyCommands(Json::Array{Json::Object{{"type", "set-source-preview-demand"}, {"enabled", false}}});
  const int before = compositor->sheets;
  for (int i = 0; i < 20; ++i) (void)core.applyCommands(Json::Array{});
  EXPECT_EQ(compositor->sheets, before) << "demand released: zero sheet cost";
  EXPECT_FALSE(core.sessionState().get("sourceSheet")->get("active")->asBool());
}

// A compositor without the pass (ICompositor's default: D3D11, the stub) has no sheet; the
// node must not claim one even while the Sources tab holds the lease.
TEST(MediaCoreSourceSheet, ACompositorWithoutThePassNeverClaimsASheet) {
  corevideo::core::MediaCore core(modules::createStubModules());
  using corevideo::rpc::Json;
  (void)core.applyCommands(Json::Array{wall(),
      Json::Object{{"type", "set-source-preview-demand"}, {"enabled", true}}});
  for (int i = 0; i < 12; ++i) (void)core.applyCommands(Json::Array{});
  const auto state = core.sessionState();   // held: get() points into it
  const auto* sheet = state.get("sourceSheet");
  ASSERT_NE(sheet, nullptr);
  EXPECT_FALSE(sheet->get("active")->asBool()) << "no exported surface: no sheet";
  EXPECT_EQ(sheet->get("texture")->getNumber("iosurfaceId"), 0);
}
