#pragma once
// The SOURCE SHEET (Sources Phase 2 plan, Task 9; Sources spec §4.5, slice S6).
//
// One fixed 5 x 2 grid of 640 x 360 cells. Cell N is Show Input slot N+1, ALWAYS: the
// geometry depends on nothing but the slot number, never on the wall's layout mode, tile
// count or list order. So a Sources row's thumbnail is identity-safe by construction, the
// Windows rule ("a wall reflow can never supply or move an identity-bound thumbnail",
// ShowInputsCoordinatorTests.CompositeLayoutCannotSupplyOrMoveAnIdentityBoundThumbnail),
// reached on Metal without per-source exports or a monitor worker.
//
// Pure: plan geometry and cell states only. MediaCore annotates health, holds dropout and
// applies per-source grades (buildSourceSheetPlan), and renders it only while the Sources
// tab holds the demand lease.
#include "core/MultiviewFeed.h"
#include "modules/Interfaces.h"

#include <array>
#include <string>
#include <vector>

namespace corevideo::core {

inline constexpr int kSourceSheetColumns = 5, kSourceSheetRows = 2;
inline constexpr int kSourceSheetCellWidth = 640, kSourceSheetCellHeight = 360;
inline constexpr int kSourceSheetCells = kSourceSheetColumns * kSourceSheetRows;

struct SourceSheetInput {
  int slot = -1;  // 0-based Show Input slot (the multiview entry's `slot`)
  ResolvedMultiviewFeed feed;
};

struct SourceSheetCell {
  int slot = 0;
  std::string sourceId;  // "" for an unassigned slot
  std::string state;     // live | held | empty
};

// The key a compositor looks a layer's frame up by (annotateLayerSource's rule).
inline std::string sourceSheetFrameKey(const modules::CompositorRenderPlanLayer& layer) {
  return !layer.participantId.empty() ? layer.participantId : layer.sourceId;
}

inline modules::CompositorRenderPlan buildSourceSheetRenderPlan(const std::vector<SourceSheetInput>& inputs) {
  modules::CompositorRenderPlan plan;
  plan.sceneId = "source-sheet";
  plan.renderPlanId = "source-sheet";
  plan.width = kSourceSheetColumns * kSourceSheetCellWidth;
  plan.height = kSourceSheetRows * kSourceSheetCellHeight;
  plan.skipCpuReadback = true;
  std::array<bool, kSourceSheetCells> claimed{};
  for (const auto& input : inputs) {
    // Out of range or already claimed: no cell. The FIRST claim of a slot wins.
    if (input.slot < 0 || input.slot >= kSourceSheetCells || claimed[static_cast<size_t>(input.slot)]) continue;
    if (input.feed.sourceId.empty()) continue;
    claimed[static_cast<size_t>(input.slot)] = true;
    modules::CompositorRenderPlanLayer layer;
    layer.layerId = "source-sheet:" + std::to_string(input.slot);
    layer.kind = input.feed.kind;
    layer.sourceId = input.feed.sourceId;
    layer.participantId = input.feed.participantId;
    layer.mediaAssetId = input.feed.mediaAssetId;
    const float column = static_cast<float>(input.slot % kSourceSheetColumns);
    const float row = static_cast<float>(input.slot / kSourceSheetColumns);
    layer.rect = {column / kSourceSheetColumns, row / kSourceSheetRows,
                  1.f / kSourceSheetColumns, 1.f / kSourceSheetRows};
    layer.hasClipRect = true;
    layer.clipRect = layer.rect;
    layer.fitMode = "fit";        // the source in 16:9, nothing cropped
    layer.borderStyle = "none";   // no border, no label: the shell draws the tally
    layer.borderThickness = 0.f;
    layer.order = input.slot;
    plan.layers.push_back(std::move(layer));
  }
  return plan;
}

// All ten cells, in slot order. live: this tick has a picture for the cell's source and its
// health is producing; held: a picture, but the source stalled (the cell holds its last
// frame, as the multiview does); empty: no picture, or no source.
inline std::vector<SourceSheetCell> sourceSheetCells(const modules::CompositorRenderPlan& plan,
                                                     const std::vector<modules::VideoFrame>& frames) {
  std::vector<SourceSheetCell> cells(kSourceSheetCells);
  for (int slot = 0; slot < kSourceSheetCells; ++slot) cells[static_cast<size_t>(slot)] = {slot, "", "empty"};
  for (const auto& layer : plan.layers) {
    if (layer.order < 0 || layer.order >= kSourceSheetCells) continue;
    auto& cell = cells[static_cast<size_t>(layer.order)];
    cell.sourceId = layer.sourceId;
    const auto key = sourceSheetFrameKey(layer);
    bool hasPicture = false;
    for (const auto& frame : frames) {
      if (frame.participantId == key && (frame.hasContent() || frame.preparedGpu)) { hasPicture = true; break; }
    }
    if (!hasPicture) continue;
    cell.state = (layer.sourceHealth == "stalled" || layer.sourceHealth == "failed") ? "held" : "live";
  }
  return cells;
}

}  // namespace corevideo::core
