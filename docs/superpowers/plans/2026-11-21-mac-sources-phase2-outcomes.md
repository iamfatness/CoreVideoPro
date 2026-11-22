# macOS Sources, Phase 2: outcomes

Plan: `2026-11-21-mac-sources-phase2.md`. Branch `mac-sources-phase2`, cut from `fc675541`. Not pushed.
Tasks 1-8 were done on 2026-11-21. A machine reboot interrupted the lane during Task 9. Task 9 (finished from the
uncommitted tree), Task 10, the review and Task 11 were done on 2026-11-22.

## Commits

| Task | Commit | What |
|---|---|---|
| plan | `c775b508` | the plan |
| 1 | `fab9de6c` | one inspector close control (D8); the Media status line never repeats the summary (D9) |
| 2 | `633f3794` | R1: facts only from revisioned rosters; frame age on stale rows; `W×H@fps` |
| 3 | `0304bfdb` | R2: cap note from the core's applied cap (D6); REC chip four states pinned through snapshots |
| 4 | `16d78d29` | S4 shell: stills and clips as slot sources (D5, D10) |
| 7 | `f120c4fb` | Mac capture liveness: AVF trusted, SCK untrusted (D7). **Program behaviour change, Mac only** |
| 8 | `49ed366d` | R3: shared evidence lines, round-trip test, `platform` field, Health rows |
| 5 | `68094110` | S4 core: a still in a slot is decoded for the wall without a scene route |
| 6 | `a1c8ba61` | PVW tally from the core (`previewFrame.videoSources`, D4) |
| 9 | `19d21422` | S6 core: the source sheet, demand-gated pass, `sourceSheet` node, Metal hook |
| 10 | `9d5d92d5` | S6 shell: demand lease, row and inspector thumbnails, tally borders, `slot` on the wire |
| review | `ca673da6` | thumbnails were upside down; the node claimed a sheet it never exported |
| 11 | `4c7167ef` | `mac-show-drill.py --source-preview-demand` |

## Verification (2026-11-22, final tree)

- **Shell:** `swift build -c release` + `COREVIDEO_SHELL_TESTS=1`: **6000 checks passed, 0 failed**. Baseline at
  `fc675541` was 5676. `scripts/lint-mac-design.sh` passed. ContractParity runs inside the shell suite.
- **Native metal** (`native/build-metal`, the CI `native-metal-macos` configure, Release): ctest 4/4.
- **Native stub** (`native/build-stub`, `-DCOREVIDEO_STUB=ON`, configured fresh because `Interfaces.h` changed): ctest 4/4.
- **Toolchain note:** the 2026-11-21 23:32 Command Line Tools update repointed `MacOSX.sdk` at a 27.2 SDK that the
  installed `ld` cannot read (`tapi error: unknown architecture`). Every build here ran with
  `SDKROOT=/Applications/Xcode.app/.../MacOSX26.5.sdk`. Another lane builds native with
  `DEVELOPER_DIR=/Library/Developer/CommandLineTools` instead. This is an environment issue, not a code bug.

## Task 9: S6 core

`core/SourceSheetPlan.h` (pure): a 5×2 grid of 640×360 cells, so 3200×720. Cell N is slot N+1, decided by the slot
number alone. An out-of-range or duplicate slot gets no cell, and the first claim of a slot wins. Each cell has no
border and uses `fit`. `MediaCore::buildSourceSheetPlan` annotates bus health, holds dropout (the monitor rule) and
applies per-source grades. It applies no program look and no key.

The pass runs in the non-isolated monitor branch only while `sourcePreviewDemand_.active()` and the wall has a
source. It runs on the 4th monitor cycle at phase 2, and the first one runs at once. A quarter of its CPU time joins
`monitorCycleCostNs`. With demand off there is no pass and the node is cleared. Metal's `renderSourceSheet` is
`renderSecondaryPass(sourceSheet_, …, /*wait*/ false)`. The hotspot edit in `MetalCompositorAdapter.mm` is one
member, one override and one defaulted parameter, as the plan allowed.

The `sourceSheet` node is published unconditionally. It carries `active`, `texture`, `columns`, `rows`, `cellWidth`,
`cellHeight` and all ten `cells`.

| Test | Result |
|---|---|
| `SourceSheetPlan.CellIsTheSlotForEveryListOrderAndSize` (lists of 1-10, 6 shuffles each) | pass |
| `SourceSheetPlan.AnOutOfRangeOrDuplicateSlotGetsNoCell` | pass |
| `SourceSheetPlan.EveryCellIsReportedWithItsState` (live / held / empty / unassigned) | pass |
| `MediaCoreSourceSheet.TheDemandLeaseGatesThePassAndTheNode` (0 passes off; 9-12 per 41 ticks on; 0 after release) | pass |
| `MediaCoreSourceSheet.ACompositorWithoutThePassNeverClaimsASheet` (review) | pass |
| `MetalCompositor.SourceSheetPutsEachSourceInItsSlotCell` (two BGRA sources, slots 0 and 6, IOSurface readback) | pass |

| Mutation | Result |
|---|---|
| cell geometry from list index instead of slot | **red**: 3 tests (plan, MediaCore, Metal readback) |
| pass with demand off | **red**: "no demand: no pass at all" |
| publish raw `active` (review fix 2 reverted) | **red**: the new compositor test |
| wait flag ignored | not observable in a unit test (stated in the plan); the drill covers it |

The Metal test sees the unwaited pass complete because it follows the pass with a waited Preview pass on the same
command queue, which runs its buffers in order.

## Task 10: S6 shell

- `SourceSheet.swift`:
  - `SourceSheetState.parse` returns nil unless the sheet is active and has a surface.
  - `SourceThumbnailPolicy.resolve` uses the Windows `PatchPreview` eligibility. Media is always eligible, capture
    only when LIVE, Zoom unless VIDEO OFF / NO INCOMING / IDLE.
  - Placeholders: "Not in the multiview", "Video off", "No signal", "Waiting for video", "Preview unavailable".
  - A row shows its cell only when `cells[slot-1].sourceId == slot.sourceKey` and the state is live or held.
  - Border colours follow `MultiviewInputRow.cs:45-51` on Studio tokens.
  - `SourcePreviewDemandLease` sends true on tab enter and every 1 s, false once on leave, and true again on
    `onConnected` while the tab is visible.
  - One shared 15 Hz `SourceThumbnailTicker` re-marks the thumbnails.
- **Spec correction 2 done:** `multiviewSourcePayloads` sends `slot: id - 1` for zoom, capture and media.
- The row (96×54) and inspector header (320×180) share `SourceThumbnail`.

| Test | Result |
|---|---|
| `sources2-s6/thumbnail-identity` (cell A/B/none × source A/B × live/held/empty × in show, slots 1/5/6/10) | pass |
| `sources2-s6/placeholder-reasons` | pass |
| `sources2-s6/sheet-parse` | pass |
| `sources2-s6/demand-lease-follows-tab` | pass |
| `sources2-s6/reassign-blanks-before-new-identity` | pass |
| `sources2-s6/border-colours` | pass |
| `sources2-s6/multiview-payload-carries-slot` | pass |
| `sources2-s6/cell-geometry` (frame maths + rendered four-band read through the real layers) | pass |

| Mutation | Result |
|---|---|
| drop the `sourceId` equality | **red**: the identity matrix (every mismatched-cell combination) |
| send the lease once, no renewal | **red**: "renewed every second" |
| old flipped container + top-down offset (review fix 1 reverted) | **red**: 13 checks, incl. both band reads |

**Not done:** the plan's snapshot images of the Sources tab at 1360 and 1920 with a seeded sheet. The real thumbnail
was not seen on screen either. That needs the app, a live core and an operator look.

## Review (independent pass, 2026-11-22)

1. **Thumbnails were upside down (real, fixed `ca673da6`).** The first cut put the sheet in a geometry-flipped
   container, believing contents keep their orientation under a flipped ancestor. I measured it with
   `CALayer.render(in:)` on a four-band image: a flipped container draws its sublayer's contents upside down. The
   container is now unflipped like `SurfaceView`'s own layer, with a bottom-up offset. The test renders the real
   layer structure with a CGImage stand-in, because IOSurface pixels can't be read back through `render(in:)`.
   `CARenderer` could not be used headlessly; it drew nothing. `contentsRect` was avoided deliberately.
2. **`sourceSheet.active` claimed a sheet on compositors without the pass (real, fixed `ca673da6`).** D3D11 and the
   stub inherit `ICompositor`'s empty default. `active` now also requires an exported surface.
3. **Checked, no change:**
   - The pass is in the non-isolated branch only.
   - `holdDropoutForMonitoring` touches only `dropoutPolicy`, so "held" is reachable.
   - Feed keys match frame keys for zoom (raw pid), capture (`capture:<id>`) and media (`media:<id>`).
   - The only `MetalCompositorAdapter.mm` edits are the three allowed ones.
4. **Accepted, stated:**
   - A non-isolated D3D11 core (non-default `COREVIDEO_ISOLATE_MONITORS=0`) with the Windows Sources tab open builds
     the sheet plan every 4th monitor cycle for nothing. That cost is CPU only and small.
   - The lease follows the selected tab. A minimised window on the Sources tab keeps the sheet rendering.

## Task 11: drill gate

Command: `COREVIDEO_FAKE_ENGINE_FPS=60 python3 scripts/mac-show-drill.py --seconds 40 --load 8 [--source-preview-demand]`,
against this worktree's Release metal core. The runs were interleaved: off, on, off, on. **The machine was loaded**
(load average 4.3-6.3; four other lanes were building and testing).

| Run | Demand | Sustained | Dropped | Delivery | Deadline misses | Underruns | Shed entered / shed ticks | coreMutex over-budget | Sheet |
|---|---|---|---|---|---|---|---|---|---|
| 1 | off | 59.9 | 0 | **68% FAIL** (worst window 59%) | 0 | 0 | 0 / 0 | 0% | inactive |
| 2 | on | 60.0 | 0 | 99% | **13** | 0 | **2 / 166** | 2% | active, 8 live cells |
| 3 | off | 60.0 | 0 | 97% | 0 | 0 | 0 / 0 | 0% | inactive |
| 4 | on | 60.1 | 0 | 99% | 0 | 0 | 0 / 0 | 0% | active, 8 live cells |

**Reading:** no delta repeats. Run 2's shed episode and 13 deadline misses did not recur in run 4. Run 1's delivery
failure was on demand **off**, where the sheet cannot cause it, and no harness-shortfall line was printed. This rig
has noise at that size, so **the spec's S6 gate is NOT passed and NOT failed**. It must be re-run on a quiet machine
before S6 ships: at least two runs each way, judged on underruns, deadline misses and shed entries. If a shed
episode repeats with demand on, the spec's fallback applies (candidate C, Metal monitor worker spec).

## Tasks 1-8: mutation results

The plan names mutations for each task. The interrupted session did not record their results (they are not in the
commit bodies or any doc), and they were **not re-run** in this session. Their tests pass in the 6000-check suite and
the native ctest above, but the red half is **unverified** for Tasks 1-8.

## Open items

1. S6 drill gate on a quiet machine (above). This blocks shipping S6.
2. Tasks 1-8 mutation results (above).
3. Task 10 snapshot images (1360/1920) and an on-screen look at a real thumbnail.
4. **D7 is a Program behaviour change** (Mac cameras now stall to hold/black at 1 s and fail-slate at 5 s per their
   dropout policy) and needs the owner's acknowledgement.
5. The /Library core-copy rule: nothing in this lane loads a file from next to the core executable. The sheet is an
   IOSurface handed over by id. No `CoreLaunchEnvironment.extras` entry is needed.
6. Merge hotspot: `MetalCompositorAdapter.mm` (3 small edits), and shared `MediaCore.cpp/.h`,
   `CoreStateCapture.cpp/.h` and `Interfaces.h` (additive). After merging with another lane that touches
   `Interfaces.h`, rebuild clean (`--clean-first`) before believing any crash.
