# CoreVideo Tiles T1 — the wall moves into the core

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move the CoreVideo Tiles wall from shell-emitted scene routes to a core-owned `tiles` render-plan layer that the core solves and expands every frame, with a pixel-level oracle that judges the result.

**Architecture:** The shell stops solving layout and instead sends one `tiles` layer carrying an ordered member list plus a style block. The core parses it into `TilesLayerState`, and `buildRenderPlanForScene` expands it into per-tile `CompositorRenderPlanLayer` entries on every render tick. Expansion-per-frame is the load-bearing choice: it is what lets T3 animate (the rects simply differ each tick) without any new command path, and per-tile *layers inside one composited texture* are the multiview pattern, not the retired per-tile-XAML-swap-chain pattern.

**Tech Stack:** C++20 (core, gtest), C# / .NET 9 (WinUI shell, xUnit), Node (headless oracle), D3D11/HLSL (compositor, from T2 on).

**Spec:** `docs/superpowers/specs/2026-08-15-corevideo-tiles-parity-design.md`
**Charter:** `docs/obs-plugin-parity-charter.md` (group T)
**Contract:** `docs/obs-plugin-tiles-behavioral-contract.md`

## Where T1 sits in the group

T1 is the first of five plans. Each produces working, shippable software:

| Plan | Delivers | Working software at the end |
|---|---|---|
| **T1 (this)** | tiles layer kind, wire, C++ solver, frame-reality veto, fill-crop, background colour, pixel oracle | Today's wall, rendered by the core, judged on pixels |
| T2 | borders, corner radius, glow shaders | The plugin's look |
| T3 | animation clock, per-tile overrides, editor over the core texture | Animated reflow + draggable pinned tiles |
| T4 | manual fill, never-show, background source, per-tile crop, prefs v10 | The plugin's full control set |
| T5 | `MetalCompositorAdapter` parity | Tiles on the Mac port |

Do not start T2 before T1's oracle is green — T2's whole job is drawing, and it needs a judge that looks at pixels.

## Global Constraints

Copied from the spec and CLAUDE.md. Every task's requirements implicitly include these.

- **Never notify a condition variable under `coreMutex`.** Lock order is `coreMutex` → `audioOutputMutex_`, never reversed.
- **No pixel work under `coreMutex` or on a hot tick.** Solving geometry is fine; touching pixel buffers is not.
- **Never replace a bound WinUI collection at frame rate.** Sync in place or diff. This is the `CoreMessagingXP 0xc000027b` fail-fast class.
- **A one-shot command is silently lost on core respawn.** Everything in T1 rides the repeating scene-sync channel; do not add a launch-time one-shot.
- **Protocol changes move in FOUR mirrors in lockstep:** `native/src/core/Protocol.h` (capability), `native-core/src/protocol.ts` (types), `src/engine/nativeMediaCoreProtocol.ts` (the legacy TS engine's capability union), and the core parser in `MediaCore.cpp`. **There are FOUR mirrors, not three** — corrected 2026-08-15 after the Task 1 implementer found the fourth. `native/src/rpc/Protocol.h` does not exist; the header lives under `core/`.
- **Failures are loud, never silent.** A rejected member, a clamped value, or a truncated list emits a warning; it never disappears quietly.
- **Do not port the plugin's colour-range constant.** Its shader hardcodes full-range BT.709; that is an open defect on our side. Tiles consumes whatever the Zoom ingest path decides.
- **Verify pixels, not proxies.** A test that asserts "N rects exist" does not prove the wall renders. See Task 6.
- Spacing math uses the divisor form `h / (100/pct)`, never `h * pct / 100`.
  **Scope correction (2026-08-15, from Task 2's review):** this constraint binds
  at PIXEL conversion, not in the normalized solver. In normalized `[0,1]` space
  — where the gutter is a fraction, not pixels — `1.0/(100.0/0.741)` and
  `0.741/100.0` are bit-identical doubles; the discrepancy only appears at pixel
  magnitudes (~1.8e-15 at h=1080), below every tolerance in the suite. Keep the
  divisor form for consistency with the C# reference, but do not write a test in
  the solver claiming to pin it — there is nothing to pin at that layer. The
  guard belongs wherever normalized rects become pixels, which is T2's draw code.

**Codebase API facts, verified 2026-08-15 during Task 1** — the plan's earlier
code samples got several of these wrong, so trust this block over any snippet:

- `MediaCore` is in namespace **`corevideo::core`**, not `corevideo`
  (`MediaCore.h:26`). `LayerRect` and the tiles helpers are in
  `corevideo::compositor`; render-plan types are in `corevideo::modules`.
- **`rpc::Json` has no `.set()`**, and `Json::parse` returns
  **`std::optional<Json>`** (`Json.h:48`). Build commands with the
  `Json::Object` / `Json::Array` literal pattern used throughout
  `native/tests/MediaCoreCommandTest.cpp` — copy that file's style rather than
  inventing one.
- **`EXPECT_DOUBLE_EQ` does not exist in this repo's vendored gtest.** Use
  `EXPECT_NEAR` with an explicit tolerance, or `EXPECT_EQ` where the arithmetic
  is genuinely exact. There is a comment recording this at
  `native/tests/AudioMasteringTest.cpp:350`.
- New structs used by `MediaCore`'s public test accessors go at **namespace
  scope above `class MediaCore`**, not nested beside `SceneRouteState`.
- `Protocol.h` is at `native/src/core/Protocol.h`. The capability string that
  `ContractParityTest` actually checks lives in
  `src/engine/nativeMediaCoreProtocol.ts`; `native-core/src/protocol.ts` carries
  types only and has no capability array.

---

### Task 1: Parse the `tiles` layer into core state

**Files:**
- Modify: `native/src/core/MediaCore.h` (add `TilesLayerState`, `tilesLayer_`, near `SceneRouteState` ~line 302)
- Modify: `native/src/core/MediaCore.cpp:1403` (the `load-scene-graph` routes parse) and `:2584` (the preview-scene/spine parse)
- Modify: `native/src/core/Protocol.h` (capability list)
- Modify: `src/engine/nativeMediaCoreProtocol.ts` (legacy TS capability union)
- Modify: `native-core/src/protocol.ts` (wire types)
- Test: `native/tests/TilesLayerTest.cpp` (create)
- Modify: `native/CMakeLists.txt:595` (register the test file)

**Interfaces:**
- Consumes: nothing (first task).
- Produces:
  ```cpp
  namespace corevideo {
  struct TilesStyle {
    std::string tileAspect = "16:9";     // 16:9|4:3|5:4|1:1|3:4|9:16|custom
    double customAspectRatio = 16.0 / 9.0;
    double gutterPercent = 0.741;
    double marginPercent = 0.741;
    std::string backgroundColor = "#000000";
  };
  struct TilesLayerState {
    bool present = false;
    std::string layerId;
    int order = 0;
    // NOTE the namespace: MediaCore.h is in `corevideo`, the rect type is in
    // `corevideo::modules`. Both CompositorLayerRect and the compositor's
    // LayerRect are {x, y, width, height} floats with identical layout, so
    // braced conversion between them in Task 4 is safe — but the qualification
    // is not optional and will not compile without it.
    modules::CompositorLayerRect rect{0.f, 0.f, 1.f, 1.f};
    std::vector<std::string> members;   // ordered "zoom:<pid>" / "capture:<id>"
    TilesStyle style;
  };
  }  // namespace corevideo
  ```

  The wire uses `w`/`h` for the rect while the struct uses `width`/`height`;
  that mismatch is deliberate and the parse in Step 3 bridges it.
  `MediaCore::tilesLayer_` holds it; `MediaCore::tilesLayerForTest()` returns a const ref.

- [ ] **Step 1: Write the failing test**

Create `native/tests/TilesLayerTest.cpp`. The snippet below is illustrative of
the assertions required; build the command objects with the `Json::Object` /
`Json::Array` literal style from `MediaCoreCommandTest.cpp` and mind the API
facts in Global Constraints.

**`loadSceneGraph` is PRIVATE** (`MediaCore.h:164`). Drive it the way
`MediaCoreCommandTest` already does — `applyCommands` with a `load-scene-graph`
command — which also exercises the real dispatch path rather than a back door.

```cpp
#include "core/MediaCore.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <string>

namespace {

using corevideo::core::MediaCore;

// Drive the real command path, matching MediaCoreCommandTest's pattern.
// Json has no .set() and Json::parse returns std::optional<Json>; build the
// command with the Json::Object/Json::Array literals MediaCoreCommandTest uses.
void loadScene(MediaCore& core, corevideo::rpc::Json::Object fields) {
  fields.emplace("type", corevideo::rpc::Json{"load-scene-graph"});
  (void)core.applyCommands(
      corevideo::rpc::Json::Array{corevideo::rpc::Json{std::move(fields)}});
}

// A minimal load-scene-graph command carrying one tiles layer.
const char* kSceneWithTiles = R"({
  "sceneId": "scene-1",
  "routes": [],
  "tiles": {
    "layerId": "tiles:scene-1",
    "order": 0,
    "rect": {"x": 0.0, "y": 0.0, "w": 1.0, "h": 1.0},
    "members": ["zoom:101", "zoom:102", "capture:cam-a"],
    "style": {
      "tileAspect": "4:3",
      "gutterPercent": 1.5,
      "marginPercent": 2.0,
      "backgroundColor": "#101418"
    }
  }
})";

TEST(TilesLayer, LoadSceneGraphParsesMembersAndStyleInOrder) {
  MediaCore core;
  loadScene(core, kSceneWithTiles);

  const auto& tiles = core.tilesLayerForTest();
  ASSERT_TRUE(tiles.present);
  EXPECT_EQ(tiles.layerId, "tiles:scene-1");
  ASSERT_EQ(tiles.members.size(), 3u);
  EXPECT_EQ(tiles.members[0], "zoom:101");
  EXPECT_EQ(tiles.members[2], "capture:cam-a");
  EXPECT_EQ(tiles.style.tileAspect, "4:3");
  EXPECT_NEAR(tiles.style.gutterPercent, 1.5, 1e-9);
  EXPECT_EQ(tiles.style.backgroundColor, "#101418");
}

TEST(TilesLayer, AbsentTilesNodeLeavesTheLayerUnset) {
  MediaCore core;
  loadScene(core, R"({"sceneId":"s","routes":[]})");
  EXPECT_FALSE(core.tilesLayerForTest().present);
}

// A scene that previously had a wall must not keep it when the next sync omits
// it — stale walls are the respawn/one-shot failure class in miniature.
TEST(TilesLayer, ReloadWithoutTilesClearsThePreviousWall) {
  MediaCore core;
  loadScene(core, kSceneWithTiles);
  ASSERT_TRUE(core.tilesLayerForTest().present);
  loadScene(core, R"({"sceneId":"s2","routes":[]})");
  EXPECT_FALSE(core.tilesLayerForTest().present);
}

// An unknown aspect token is a legal value we did not expect; fall back rather
// than guess, and say so.
TEST(TilesLayer, UnknownAspectFallsBackTo16x9AndWarns) {
  MediaCore core;
  loadScene(core, R"({
    "sceneId": "s",
    "routes": [],
    "tiles": {"members": ["zoom:1"], "style": {"tileAspect": "banana"}}
  })");
  EXPECT_EQ(core.tilesLayerForTest().style.tileAspect, "16:9");
  EXPECT_FALSE(core.sceneValidationWarningsForTest().empty());
}

}  // namespace
```

- [ ] **Step 2: Register the test and run it to verify it fails**

Add `tests/TilesLayerTest.cpp` to the source list in `native/CMakeLists.txt` (alphabetically near line 595, beside `tests/StillMediaFrameCacheTest.cpp`).

Run:
```powershell
cmake --build native\build-dev --config Release --target corevideo-native-tests
native\build-dev\corevideo-native-tests.exe --gtest_filter=TilesLayer.*
```
Expected: FAIL to compile — `tilesLayerForTest` is not a member of `MediaCore`.

**Run the binary the build just wrote.** `native/build-dev/Release/` is stale and nothing updates it; a run from there silently omits every test added since. If `TilesLayer.*` reports "0 tests ran", check which binary you ran before suspecting CMake.

- [ ] **Step 3: Add the state and the parse**

In `native/src/core/MediaCore.h`, beside `SceneRouteState` (~line 302), add `TilesStyle` and `TilesLayerState` exactly as given in **Interfaces** above. Add to the private section:

```cpp
  TilesLayerState tilesLayer_;
```

and to the public section:

```cpp
  const TilesLayerState& tilesLayerForTest() const { return tilesLayer_; }
  const std::vector<std::string>& sceneValidationWarningsForTest() const {
    return sceneValidationWarnings_;
  }
```

In `MediaCore.cpp`, add a free helper above `loadSceneGraph`:

```cpp
namespace {

// Tiles style tokens the wall understands. An unrecognised token is a legal
// string we did not expect, so fall back loudly rather than guess a shape.
std::string normalizeTileAspect(const std::string& value, bool* fellBack) {
  static const char* kKnown[] = {"16:9", "4:3", "5:4", "1:1", "3:4", "9:16", "custom"};
  for (const char* known : kKnown) {
    if (value == known) {
      return value;
    }
  }
  *fellBack = !value.empty();
  return "16:9";
}

corevideo::TilesLayerState parseTilesLayer(const corevideo::rpc::Json& node,
                                           std::vector<std::string>* warnings) {
  corevideo::TilesLayerState tiles;
  tiles.present = true;
  tiles.layerId = node.getString("layerId");
  tiles.order = static_cast<int>(node.getNumber("order", 0.0));
  if (const corevideo::rpc::Json* rect = node.get("rect"); rect && rect->isObject()) {
    tiles.rect = {static_cast<float>(rect->getNumber("x", 0.0)),
                  static_cast<float>(rect->getNumber("y", 0.0)),
                  static_cast<float>(rect->getNumber("w", 1.0)),
                  static_cast<float>(rect->getNumber("h", 1.0))};
  }
  if (const corevideo::rpc::Json* members = node.get("members"); members && members->isArray()) {
    for (const auto& member : members->asArray()) {
      const std::string id = member.asString();
      if (!id.empty()) {
        tiles.members.push_back(id);
      }
    }
  }
  if (const corevideo::rpc::Json* style = node.get("style"); style && style->isObject()) {
    bool aspectFellBack = false;
    tiles.style.tileAspect = normalizeTileAspect(style->getString("tileAspect"), &aspectFellBack);
    if (aspectFellBack) {
      warnings->push_back("Tiles layer requested an unknown tile aspect; using 16:9.");
    }
    tiles.style.customAspectRatio = style->getNumber("customAspectRatio", 16.0 / 9.0);
    tiles.style.gutterPercent = style->getNumber("gutterPercent", 0.741);
    tiles.style.marginPercent = style->getNumber("marginPercent", 0.741);
    const std::string background = style->getString("backgroundColor");
    if (!background.empty()) {
      tiles.style.backgroundColor = background;
    }
  }
  return tiles;
}

}  // namespace
```

In `loadSceneGraph`, immediately after `sceneRoutes_.clear();` (line 1390) add `tilesLayer_ = {};` — the reset is what makes the third test pass. Then after the `routes` block (after line 1483) add:

```cpp
  if (const rpc::Json* tiles = command.get("tiles"); tiles && tiles->isObject()) {
    tilesLayer_ = parseTilesLayer(*tiles, &sceneValidationWarnings_);
  }
```

Apply the identical reset-then-parse at the preview-scene parse site (`:2584`), reading `previewScene.get("tiles")`.

- [ ] **Step 4: Mirror the protocol**

In `native/src/core/Protocol.h`, add `"tiles-layer"` to the advertised capability list beside `"iso-recording"`.

In `native-core/src/protocol.ts`, add beside the scene-graph types:

```ts
export interface TilesStyleWire {
  tileAspect?: '16:9' | '4:3' | '5:4' | '1:1' | '3:4' | '9:16' | 'custom';
  customAspectRatio?: number;
  gutterPercent?: number;
  marginPercent?: number;
  backgroundColor?: string;
}

export interface TilesLayerWire {
  layerId?: string;
  order?: number;
  rect?: { x: number; y: number; w: number; h: number };
  members: string[];
  style?: TilesStyleWire;
}
```

and add `tiles?: TilesLayerWire;` to the scene-graph command interface.

- [ ] **Step 5: Run tests to verify they pass**

Run:
```powershell
cmake --build native\build-dev --config Release --target corevideo-native-tests
native\build-dev\corevideo-native-tests.exe --gtest_filter=TilesLayer.*
```
Expected: PASS, 4 tests.

Then the whole suite, to prove nothing regressed:
```powershell
native\build-dev\corevideo-native-tests.exe
```
Expected: 0 failures. Record the total; it is the baseline for later tasks.

- [ ] **Step 6: Commit**

```bash
git add native/src/core/MediaCore.h native/src/core/MediaCore.cpp \
        native/src/core/Protocol.h native-core/src/protocol.ts \
        src/engine/nativeMediaCoreProtocol.ts \
        native/tests/TilesLayerTest.cpp native/CMakeLists.txt
git commit -m "feat(tiles): the core parses a tiles layer off the scene sync"
```

---

### Task 2: Port the grid solver to C++

**Files:**
- Create: `native/src/compositor/TilesLayout.h`
- Test: `native/tests/TilesLayoutTest.cpp` (create)
- Modify: `native/CMakeLists.txt` (register the test)
- Reference only (do not modify): `native-shell/CoreVideoPro.WinUI/Services/DynamicGalleryLayoutService.cs`, `native-shell/CoreVideoPro.WinUI.Tests/DynamicGalleryLayoutServiceTests.cs`

**Interfaces:**
- Consumes: `TilesStyle` from Task 1.
- Produces:
  ```cpp
  namespace corevideo::compositor {
  // Normalized canvas-space rects, one per tile, in member order.
  std::vector<LayerRect> solveTilesLayout(int tileCount,
                                          double canvasAspectRatio,
                                          const std::string& tileAspectPreset,
                                          double customAspectRatio,
                                          double gutterPercent,
                                          double marginPercent);
  double resolveTileAspectRatio(const std::string& preset, double customAspectRatio);
  }  // namespace corevideo::compositor
  ```

The C# service is the reference implementation. Read it before writing the port; the C++ must agree with it rect-for-rect.

- [ ] **Step 1: Write the failing test**

Create `native/tests/TilesLayoutTest.cpp`. These cases mirror `DynamicGalleryLayoutServiceTests` — the C# suite is the acceptance criteria, so the port is verified against known-good behaviour rather than against itself.

```cpp
#include "compositor/TilesLayout.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using corevideo::compositor::LayerRect;
using corevideo::compositor::resolveTileAspectRatio;
using corevideo::compositor::solveTilesLayout;

constexpr double kTol = 1e-9;

std::vector<LayerRect> solve(int n) {
  return solveTilesLayout(n, 16.0 / 9.0, "16:9", 16.0 / 9.0, 0.741, 0.741);
}

TEST(TilesLayout, ZeroTilesProducesNoRects) {
  EXPECT_TRUE(solve(0).empty());
  EXPECT_TRUE(solve(-3).empty());
}

TEST(TilesLayout, OneTileIsCentered) {
  const auto rects = solve(1);
  ASSERT_EQ(rects.size(), 1u);
  EXPECT_NEAR(rects[0].x + rects[0].width / 2.0, 0.5, 1e-6);
  EXPECT_NEAR(rects[0].y + rects[0].height / 2.0, 0.5, 1e-6);
}

TEST(TilesLayout, EveryTileIsTheSameSize) {
  for (int n = 1; n <= 16; ++n) {
    const auto rects = solve(n);
    ASSERT_EQ(rects.size(), static_cast<size_t>(n)) << "n=" << n;
    for (const auto& rect : rects) {
      EXPECT_NEAR(rect.width, rects[0].width, kTol) << "n=" << n;
      EXPECT_NEAR(rect.height, rects[0].height, kTol) << "n=" << n;
    }
  }
}

TEST(TilesLayout, NoTileEscapesTheCanvas) {
  for (int n = 1; n <= 16; ++n) {
    for (const auto& rect : solve(n)) {
      EXPECT_GE(rect.x, -kTol) << "n=" << n;
      EXPECT_GE(rect.y, -kTol) << "n=" << n;
      EXPECT_LE(rect.x + rect.width, 1.0 + kTol) << "n=" << n;
      EXPECT_LE(rect.y + rect.height, 1.0 + kTol) << "n=" << n;
    }
  }
}

TEST(TilesLayout, NoTwoTilesOverlap) {
  for (int n = 2; n <= 12; ++n) {
    const auto rects = solve(n);
    for (size_t a = 0; a < rects.size(); ++a) {
      for (size_t b = a + 1; b < rects.size(); ++b) {
        const bool disjoint =
            rects[a].x + rects[a].width <= rects[b].x + kTol ||
            rects[b].x + rects[b].width <= rects[a].x + kTol ||
            rects[a].y + rects[a].height <= rects[b].y + kTol ||
            rects[b].y + rects[b].height <= rects[a].y + kTol;
        EXPECT_TRUE(disjoint) << "n=" << n << " a=" << a << " b=" << b;
      }
    }
  }
}

// A short last row is centered, not left-aligned — five tiles is 3+2 with the
// pair centered under the trio.
TEST(TilesLayout, ShortLastRowIsCentered) {
  const auto rects = solve(5);
  ASSERT_EQ(rects.size(), 5u);
  const double topRowCenter = (rects[0].x + rects[2].x + rects[2].width) / 2.0;
  const double lastRowCenter = (rects[3].x + rects[4].x + rects[4].width) / 2.0;
  EXPECT_NEAR(topRowCenter, lastRowCenter, 1e-6);
  EXPECT_NEAR(lastRowCenter, 0.5, 1e-6);
}

TEST(TilesLayout, AspectPresetsResolve) {
  EXPECT_NEAR(resolveTileAspectRatio("16:9", 1.0), 16.0 / 9.0, kTol);
  EXPECT_NEAR(resolveTileAspectRatio("1:1", 1.0), 1.0, kTol);
  EXPECT_NEAR(resolveTileAspectRatio("9:16", 1.0), 9.0 / 16.0, kTol);
  EXPECT_NEAR(resolveTileAspectRatio("custom", 2.5), 2.5, kTol);
  EXPECT_NEAR(resolveTileAspectRatio("banana", 1.0), 16.0 / 9.0, kTol);
  // Custom ratios are clamped to the same [0.25, 4] band as the shell.
  EXPECT_NEAR(resolveTileAspectRatio("custom", 99.0), 4.0, kTol);
  EXPECT_NEAR(resolveTileAspectRatio("custom", 0.01), 0.25, kTol);
}

// Spacing uses the DIVISOR form. h / (100/pct) round-trips exactly to the
// historic canvas_height/135.0; h * pct / 100 disagrees in the last bit.
TEST(TilesLayout, GutterUsesTheDivisorForm) {
  const double pct = 0.741;
  const double h = 1080.0;
  EXPECT_NEAR(h / (100.0 / pct), 8.0028, 1e-9);
}

}  // namespace
```

- [ ] **Step 2: Register and run to verify it fails**

Add `tests/TilesLayoutTest.cpp` to `native/CMakeLists.txt`.

Run:
```powershell
cmake --build native\build-dev --config Release --target corevideo-native-tests
native\build-dev\corevideo-native-tests.exe --gtest_filter=TilesLayout.*
```
Expected: FAIL to compile — `compositor/TilesLayout.h` does not exist.

- [ ] **Step 3: Write the solver**

Create `native/src/compositor/TilesLayout.h`. This is a direct port of `DynamicGalleryLayoutService.BuildRects`; keep the structure recognisably the same so the two can be diffed by eye.

```cpp
#pragma once

#include "compositor/CompositorLayout.h"

#include <algorithm>
#include <string>
#include <vector>

namespace corevideo::compositor {

// Ported from the shell's DynamicGalleryLayoutService, which remains the
// reference implementation (its tests are this port's acceptance criteria).
// The shell no longer solves at runtime — two live solvers would drift, and the
// day they disagree the editor draws boxes where the program is not rendering.
inline std::string normalizeTileAspectPreset(const std::string& value) {
  if (value == "4:3" || value == "5:4" || value == "1:1" || value == "3:4" ||
      value == "9:16" || value == "custom") {
    return value;
  }
  return "16:9";
}

inline double resolveTileAspectRatio(const std::string& preset, double customAspectRatio) {
  const std::string normalized = normalizeTileAspectPreset(preset);
  if (normalized == "4:3") return 4.0 / 3.0;
  if (normalized == "5:4") return 5.0 / 4.0;
  if (normalized == "1:1") return 1.0;
  if (normalized == "3:4") return 3.0 / 4.0;
  if (normalized == "9:16") return 9.0 / 16.0;
  if (normalized == "custom") return std::clamp(customAspectRatio, 0.25, 4.0);
  return 16.0 / 9.0;
}

inline std::vector<LayerRect> solveTilesLayout(int tileCount,
                                               double canvasAspectRatio,
                                               const std::string& tileAspectPreset,
                                               double customAspectRatio,
                                               double gutterPercent,
                                               double marginPercent) {
  if (tileCount <= 0) {
    return {};
  }

  const double canvasAspect = std::clamp(canvasAspectRatio, 0.25, 4.0);
  const double tileAspect = resolveTileAspectRatio(tileAspectPreset, customAspectRatio);
  // Divisor form, deliberately: see TilesLayout.GutterUsesTheDivisorForm.
  const double gutterY = gutterPercent <= 0.0 ? 0.0 : std::clamp(1.0 / (100.0 / gutterPercent), 0.0, 0.1);
  const double marginY = marginPercent <= 0.0 ? 0.0 : std::clamp(1.0 / (100.0 / marginPercent), 0.0, 0.2);
  const double gutterX = gutterY / canvasAspect;
  const double marginX = marginY / canvasAspect;

  int bestColumns = 0;
  int bestRows = 0;
  double bestWidth = 0.0;
  double bestHeight = 0.0;
  double bestArea = -1.0;

  for (int columns = 1; columns <= tileCount; ++columns) {
    const int rows = static_cast<int>(std::ceil(static_cast<double>(tileCount) / columns));
    const double availableWidth = 1.0 - (2 * marginX) - ((columns - 1) * gutterX);
    const double availableHeight = 1.0 - (2 * marginY) - ((rows - 1) * gutterY);
    if (availableWidth <= 0.0 || availableHeight <= 0.0) {
      continue;
    }
    const double width = std::min(availableWidth / columns,
                                  (availableHeight / rows) * tileAspect / canvasAspect);
    const double height = width * canvasAspect / tileAspect;
    const double area = width * height;
    if (bestArea < 0.0 || area > bestArea + 1e-7 ||
        (std::abs(area - bestArea) < 1e-7 && columns < bestColumns)) {
      bestColumns = columns;
      bestRows = rows;
      bestWidth = width;
      bestHeight = height;
      bestArea = area;
    }
  }

  if (bestArea < 0.0) {
    bestColumns = 1;
    bestRows = tileCount;
    bestWidth = 1.0;
    bestHeight = 1.0 / tileCount;
  }

  const double gridHeight = (bestRows * bestHeight) + ((bestRows - 1) * gutterY);
  const double top = (1.0 - gridHeight) / 2.0;
  std::vector<LayerRect> result;
  result.reserve(static_cast<size_t>(tileCount));

  for (int row = 0; row < bestRows; ++row) {
    const int rowStart = row * bestColumns;
    const int rowCount = std::min(bestColumns, tileCount - rowStart);
    const double rowWidth = (rowCount * bestWidth) + ((rowCount - 1) * gutterX);
    const double left = (1.0 - rowWidth) / 2.0;
    for (int column = 0; column < rowCount; ++column) {
      result.push_back(LayerRect{
          static_cast<float>(left + column * (bestWidth + gutterX)),
          static_cast<float>(top + row * (bestHeight + gutterY)),
          static_cast<float>(bestWidth),
          static_cast<float>(bestHeight)});
    }
  }

  return result;
}

}  // namespace corevideo::compositor
```

- [ ] **Step 4: Run tests to verify they pass**

Run:
```powershell
cmake --build native\build-dev --config Release --target corevideo-native-tests
native\build-dev\corevideo-native-tests.exe --gtest_filter=TilesLayout.*
```
Expected: PASS, 8 tests.

If `EveryTileIsTheSameSize` or `ShortLastRowIsCentered` fails, diff your port against the C# line by line before adjusting a tolerance — the C# is the oracle here, not the test's tolerance.

- [ ] **Step 5: Commit**

```bash
git add native/src/compositor/TilesLayout.h native/tests/TilesLayoutTest.cpp native/CMakeLists.txt
git commit -m "feat(tiles): port the gallery grid solver to the core"
```

---

### Task 3: The frame-reality veto

**Files:**
- Create: `native/src/compositor/TilesMembership.h`
- Test: `native/tests/TilesMembershipTest.cpp` (create)
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from Tasks 1–2 at compile time (pure function over ids and ages).
- Produces:
  ```cpp
  namespace corevideo::compositor {
  // How long a member may go without a fresh frame before it leaves the wall.
  // Long enough that an ordinary frame gap or a Zoom resolution ramp never
  // reflows; short enough that a dead feed does not hold a slot visibly.
  constexpr int64_t kTilesStaleFrameMs = 1500;

  struct TilesMemberFrameAge {
    std::string sourceId;
    bool hasFrame = false;
    int64_t lastFrameAgeMs = 0;
  };

  // Returns the members that should be drawn, in the caller's order.
  std::vector<std::string> admitTilesMembers(
      const std::vector<std::string>& members,
      const std::vector<TilesMemberFrameAge>& ages,
      int64_t staleAfterMs = kTilesStaleFrameMs);
  }  // namespace corevideo::compositor
  ```

Do NOT reuse `warnUnmatchedCaptureLayer`'s 5s interval as this threshold — that is a *warning rate limit*, not a staleness rule, and borrowing it would ship a reflow cadence nobody chose.

- [ ] **Step 1: Write the failing test**

Create `native/tests/TilesMembershipTest.cpp`:

```cpp
#include "compositor/TilesMembership.h"

#include <gtest/gtest.h>

namespace {

using corevideo::compositor::admitTilesMembers;
using corevideo::compositor::kTilesStaleFrameMs;
using corevideo::compositor::TilesMemberFrameAge;

TEST(TilesMembership, FreshMembersAreAdmittedInOrder) {
  const std::vector<std::string> members{"zoom:1", "zoom:2", "capture:a"};
  const std::vector<TilesMemberFrameAge> ages{
      {"zoom:1", true, 16}, {"zoom:2", true, 33}, {"capture:a", true, 0}};
  EXPECT_EQ(admitTilesMembers(members, ages), members);
}

TEST(TilesMembership, AMemberThatNeverDeliveredAFrameIsNotDrawn) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, 16}, {"zoom:2", false, 0}};
  const auto admitted = admitTilesMembers({"zoom:1", "zoom:2"}, ages);
  ASSERT_EQ(admitted.size(), 1u);
  EXPECT_EQ(admitted[0], "zoom:1");
}

TEST(TilesMembership, AMemberWithNoAgeEntryAtAllIsNotDrawn) {
  const auto admitted = admitTilesMembers({"zoom:1", "zoom:missing"}, {{"zoom:1", true, 16}});
  ASSERT_EQ(admitted.size(), 1u);
  EXPECT_EQ(admitted[0], "zoom:1");
}

// The edges of the threshold, both pinned: an ordinary gap must not reflow the
// wall, and a dead feed must actually leave it.
TEST(TilesMembership, AnOrdinaryFrameGapKeepsTheMemberOnTheWall) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, kTilesStaleFrameMs - 1}};
  EXPECT_EQ(admitTilesMembers({"zoom:1"}, ages).size(), 1u);
}

TEST(TilesMembership, AStaleFeedLeavesTheWall) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, kTilesStaleFrameMs + 1}};
  EXPECT_TRUE(admitTilesMembers({"zoom:1"}, ages).empty());
}

// Re-admission is immediate: one fresh frame puts the guest back, in their
// original slot order.
TEST(TilesMembership, AReturningFeedIsReadmittedInOriginalOrder) {
  const std::vector<std::string> members{"zoom:1", "zoom:2", "zoom:3"};
  const std::vector<TilesMemberFrameAge> stale{
      {"zoom:1", true, 16}, {"zoom:2", true, kTilesStaleFrameMs + 500}, {"zoom:3", true, 16}};
  ASSERT_EQ(admitTilesMembers(members, stale).size(), 2u);

  const std::vector<TilesMemberFrameAge> recovered{
      {"zoom:1", true, 16}, {"zoom:2", true, 0}, {"zoom:3", true, 16}};
  EXPECT_EQ(admitTilesMembers(members, recovered), members);
}

TEST(TilesMembership, DuplicateMembersAreDrawnOnce) {
  const std::vector<TilesMemberFrameAge> ages{{"zoom:1", true, 0}};
  const auto admitted = admitTilesMembers({"zoom:1", "zoom:1"}, ages);
  EXPECT_EQ(admitted.size(), 1u);
}

}  // namespace
```

- [ ] **Step 2: Register and run to verify it fails**

Add `tests/TilesMembershipTest.cpp` to `native/CMakeLists.txt`.

Run:
```powershell
cmake --build native\build-dev --config Release --target corevideo-native-tests
native\build-dev\corevideo-native-tests.exe --gtest_filter=TilesMembership.*
```
Expected: FAIL to compile — header does not exist.

- [ ] **Step 3: Write the membership filter**

Create `native/src/compositor/TilesMembership.h`:

```cpp
#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace corevideo::compositor {

// The shell decides who is ELIGIBLE for the wall; the core decides who is
// actually DRAWN. Only the core knows whether frames are arriving, and a wall
// that holds a slot for a dead feed shows a black square. The plugin accepts
// that trade; we do not have to.
constexpr int64_t kTilesStaleFrameMs = 1500;

struct TilesMemberFrameAge {
  std::string sourceId;
  bool hasFrame = false;
  int64_t lastFrameAgeMs = 0;
};

inline std::vector<std::string> admitTilesMembers(
    const std::vector<std::string>& members,
    const std::vector<TilesMemberFrameAge>& ages,
    int64_t staleAfterMs = kTilesStaleFrameMs) {
  std::vector<std::string> admitted;
  std::unordered_set<std::string> seen;
  admitted.reserve(members.size());
  for (const auto& member : members) {
    if (!seen.insert(member).second) {
      continue;
    }
    for (const auto& age : ages) {
      if (age.sourceId == member) {
        if (age.hasFrame && age.lastFrameAgeMs <= staleAfterMs) {
          admitted.push_back(member);
        }
        break;
      }
    }
  }
  return admitted;
}

}  // namespace corevideo::compositor
```

- [ ] **Step 4: Run tests to verify they pass**

Run:
```powershell
cmake --build native\build-dev --config Release --target corevideo-native-tests
native\build-dev\corevideo-native-tests.exe --gtest_filter=TilesMembership.*
```
Expected: PASS, 7 tests.

- [ ] **Step 5: Commit**

```bash
git add native/src/compositor/TilesMembership.h native/tests/TilesMembershipTest.cpp native/CMakeLists.txt
git commit -m "feat(tiles): the core drops members with no fresh frame"
```

---

### Task 4: Expand the tiles layer in the render plan

**Files:**
- Modify: `native/src/core/MediaCore.cpp` — `buildRenderPlanForScene` (signature at `:4211`, route loop at `:4249`)
- Modify: `native/src/core/MediaCore.h` (the `buildRenderPlanForScene` declaration)
- Test: `native/tests/TilesRenderPlanTest.cpp` (create)
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: `TilesLayerState` (Task 1), `solveTilesLayout` (Task 2), `admitTilesMembers` (Task 3).
- Produces: render-plan layers with `layerId` of the form `tile:<sourceId>` and `kind` `"participant-video"`, plus one `kind` `"tiles-background"` layer beneath them. Later plans (T2, T3) modify these same layers rather than adding a parallel path.

The expansion runs on the render tick, once per frame. That is deliberate: T3's animation changes only the rects this produces, needing no new command and no new frequency.

- [ ] **Step 1: Write the failing test**

Create `native/tests/TilesRenderPlanTest.cpp`:

```cpp
#include "core/MediaCore.h"
#include "rpc/Json.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace {

using corevideo::core::MediaCore;

int countLayersOfKind(const corevideo::modules::CompositorRenderPlan& plan,
                      const std::string& kind) {
  return static_cast<int>(std::count_if(
      plan.layers.begin(), plan.layers.end(),
      [&](const auto& layer) { return layer.kind == kind; }));
}

const corevideo::modules::CompositorRenderPlanLayer* findLayer(
    const corevideo::modules::CompositorRenderPlan& plan, const std::string& layerId) {
  for (const auto& layer : plan.layers) {
    if (layer.layerId == layerId) {
      return &layer;
    }
  }
  return nullptr;
}

// Build the command with the Json::Object/Json::Array literal pattern used
// throughout MediaCoreCommandTest.cpp. Json has no .set(), and Json::parse
// returns std::optional<Json> — see the API facts in Global Constraints.
void loadWall(MediaCore& core, const std::vector<std::string>& members) {
  corevideo::rpc::Json::Array memberJson;
  for (const auto& member : members) {
    memberJson.push_back(corevideo::rpc::Json{member});
  }
  (void)core.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json{corevideo::rpc::Json::Object{
          {"type", corevideo::rpc::Json{"load-scene-graph"}},
          {"sceneId", corevideo::rpc::Json{"s"}},
          {"routes", corevideo::rpc::Json{corevideo::rpc::Json::Array{}}},
          {"tiles", corevideo::rpc::Json{corevideo::rpc::Json::Object{
              {"layerId", corevideo::rpc::Json{"tiles:s"}},
              {"members", corevideo::rpc::Json{memberJson}},
              {"style", corevideo::rpc::Json{corevideo::rpc::Json::Object{
                  {"backgroundColor", corevideo::rpc::Json{"#101418"}}}}}}}}}}});
}

TEST(TilesRenderPlan, EachAdmittedMemberBecomesOneTileLayer) {
  MediaCore core; loadWall(core, {"zoom:1", "zoom:2", "zoom:3"});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:2", true, 0}, {"zoom:3", true, 0}});

  const auto plan = core.lastRenderPlanForTest();
  EXPECT_EQ(countLayersOfKind(plan, "participant-video"), 3);
  ASSERT_NE(findLayer(plan, "tile:zoom:2"), nullptr);
  EXPECT_EQ(findLayer(plan, "tile:zoom:2")->sourceId, "zoom:2");
}

TEST(TilesRenderPlan, TheWallDrawsABackgroundBeneathEveryTile) {
  MediaCore core; loadWall(core, {"zoom:1"});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}});

  const auto plan = core.lastRenderPlanForTest();
  const auto* background = findLayer(plan, "tiles-bg:tiles:s");
  ASSERT_NE(background, nullptr);
  EXPECT_EQ(background->kind, "tiles-background");
  EXPECT_EQ(background->borderColor, "#101418");
  for (const auto& layer : plan.layers) {
    if (layer.kind == "participant-video") {
      EXPECT_LT(background->order, layer.order);
    }
  }
}

// Fill, never letterbox — a wall of mixed cameras stays even because tiles crop
// their sides rather than growing bars.
TEST(TilesRenderPlan, EveryTileFillsRatherThanFits) {
  MediaCore core; loadWall(core, {"zoom:1", "zoom:2"});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:2", true, 0}});

  for (const auto& layer : core.lastRenderPlanForTest().layers) {
    if (layer.kind == "participant-video") {
      EXPECT_EQ(layer.fitMode, "fill");
    }
  }
}

// T1 ships no styling: a border here would composite chrome into PROGRAM, the
// virtual camera, and every recording. T2 adds it deliberately.
TEST(TilesRenderPlan, TilesCarryNoBorderBeforeStylingShips) {
  MediaCore core; loadWall(core, {"zoom:1"});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}});

  for (const auto& layer : core.lastRenderPlanForTest().layers) {
    if (layer.kind == "participant-video") {
      EXPECT_EQ(layer.borderStyle, "none");
      EXPECT_FLOAT_EQ(layer.borderThickness, 0.f);
    }
  }
}

TEST(TilesRenderPlan, AStaleMemberIsNotDrawnAndTheWallReflows) {
  MediaCore core; loadWall(core, {"zoom:1", "zoom:2"});
  core.setTilesMemberFrameAgesForTest({{"zoom:1", true, 0}, {"zoom:2", true, 0}});
  const auto twoUp = core.lastRenderPlanForTest();
  const float pairedWidth = findLayer(twoUp, "tile:zoom:1")->rect.width;

  core.setTilesMemberFrameAgesForTest(
      {{"zoom:1", true, 0}, {"zoom:2", true, corevideo::compositor::kTilesStaleFrameMs + 1}});
  const auto soloPlan = core.lastRenderPlanForTest();

  EXPECT_EQ(countLayersOfKind(soloPlan, "participant-video"), 1);
  EXPECT_GT(findLayer(soloPlan, "tile:zoom:1")->rect.width, pairedWidth);
}

TEST(TilesRenderPlan, NoTilesLayerLeavesTheOrdinaryRoutePlanUntouched) {
  MediaCore core;
  loadWall(core, {});
  const auto plan = core.lastRenderPlanForTest();
  EXPECT_EQ(countLayersOfKind(plan, "tiles-background"), 0);
}

}  // namespace
```

- [ ] **Step 2: Register and run to verify it fails**

Add `tests/TilesRenderPlanTest.cpp` to `native/CMakeLists.txt`.

Run:
```powershell
cmake --build native\build-dev --config Release --target corevideo-native-tests
native\build-dev\corevideo-native-tests.exe --gtest_filter=TilesRenderPlan.*
```
Expected: FAIL to compile — `setTilesMemberFrameAgesForTest` and `lastRenderPlanForTest` do not exist.

- [ ] **Step 3: Add the test seams and the expansion**

In `MediaCore.h`, add to the public section:

```cpp
  void setTilesMemberFrameAgesForTest(std::vector<compositor::TilesMemberFrameAge> ages) {
    tilesMemberFrameAges_ = std::move(ages);
  }
  // buildRenderPlanForScene takes 10+ arguments (MediaCore.cpp:4206) — do NOT
  // try to call it from a test. Cache what the render tick actually built and
  // expose THAT, so the test observes the production path's own output.
  const modules::CompositorRenderPlan& lastRenderPlanForTest() const {
    return lastRenderPlan_;
  }
```

and to the private section:

```cpp
  std::vector<compositor::TilesMemberFrameAge> tilesMemberFrameAges_;
  modules::CompositorRenderPlan lastRenderPlan_;
```

Assign `lastRenderPlan_ = plan;` at the `buildRenderPlanForScene` call site
(`MediaCore.cpp:4164`) so the seam always reflects the real render tick. The
tests drive it by applying a `load-scene-graph` command, which reaches that call
site through the ordinary path — no back door.

**Also publish the solved wall in `sessionState()`** so downstream consumers can
see what the core actually drew. Add a `tiles` node carrying `{layerId,
members: [{sourceId, rect:{x,y,width,height}}]}` built from the same expansion.
Task 6's oracle reads it to know where to sample, and T3's canvas editor reads it
to position drag handles. Without it the oracle would have to re-derive rects
with the solver it is supposed to be judging.

Include `compositor/TilesLayout.h` and `compositor/TilesMembership.h` in `MediaCore.cpp`.

In `buildRenderPlanForScene`, after the `sceneRoutes` loop closes (after `:4292`'s block ends), add the expansion:

```cpp
  // The wall is expanded HERE, on the render tick, rather than sent as N layers
  // by the shell. That is what lets T3 animate: the rects simply differ each
  // frame, with no new command and no new frequency. N layers inside ONE
  // composited texture is the multiview pattern; it is NOT the retired per-tile
  // XAML swap-chain pattern.
  if (tilesLayer_.present) {
    const auto admitted = compositor::admitTilesMembers(tilesLayer_.members, tilesMemberFrameAges_);
    const auto rects = compositor::solveTilesLayout(
        static_cast<int>(admitted.size()), 16.0 / 9.0, tilesLayer_.style.tileAspect,
        tilesLayer_.style.customAspectRatio, tilesLayer_.style.gutterPercent,
        tilesLayer_.style.marginPercent);

    const int tilesBaseOrder = tilesLayer_.order;
    modules::CompositorRenderPlanLayer background;
    background.layerId = "tiles-bg:" + tilesLayer_.layerId;
    background.kind = "tiles-background";
    background.order = tilesBaseOrder;
    background.rect = tilesLayer_.rect;
    background.fitMode = "fill";
    // The background colour rides borderColor until T2 gives the wall its own
    // style block on the layer; the compositor reads it as a solid fill.
    background.borderColor = tilesLayer_.style.backgroundColor;
    background.borderStyle = "none";
    background.borderThickness = 0.f;
    renderPlan.layers.push_back(std::move(background));

    for (size_t index = 0; index < admitted.size() && index < rects.size(); ++index) {
      modules::CompositorRenderPlanLayer layer;
      layer.layerId = "tile:" + admitted[index];
      layer.kind = "participant-video";
      layer.sourceId = admitted[index];
      // Tile rects are solved in the WALL's space; map them into canvas space so
      // a wall can occupy part of the canvas beside other layers.
      layer.rect = {tilesLayer_.rect.x + rects[index].x * tilesLayer_.rect.width,
                    tilesLayer_.rect.y + rects[index].y * tilesLayer_.rect.height,
                    rects[index].width * tilesLayer_.rect.width,
                    rects[index].height * tilesLayer_.rect.height};
      layer.order = tilesBaseOrder + 1 + static_cast<int>(index);
      // Fill, never letterbox — the rule that keeps a wall of mixed cameras even.
      layer.fitMode = "fill";
      // T1 ships NO styling. A border here composites chrome into PROGRAM, the
      // virtual camera, and every recording. T2 adds it deliberately.
      layer.borderStyle = "none";
      layer.borderThickness = 0.f;
      if (const size_t colon = admitted[index].find(':'); colon != std::string::npos) {
        layer.participantId = admitted[index].substr(colon + 1);
      }
      renderPlan.layers.push_back(std::move(layer));
    }
  }
```

Populate `tilesMemberFrameAges_` from the live frame gather in the caller at `:4164`, deriving each member's age from the same `videoFrames` the render plan already consults. Keep it under the existing `coreMutex` scope — this is geometry bookkeeping, not pixel work.

- [ ] **Step 4: Run tests to verify they pass**

Run:
```powershell
cmake --build native\build-dev --config Release --target corevideo-native-tests
native\build-dev\corevideo-native-tests.exe --gtest_filter=TilesRenderPlan.*
```
Expected: PASS, 6 tests.

Then the full suite:
```powershell
native\build-dev\corevideo-native-tests.exe
```
Expected: 0 failures, total ≥ the Task 1 baseline.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/MediaCore.cpp native/src/core/MediaCore.h \
        native/tests/TilesRenderPlanTest.cpp native/CMakeLists.txt
git commit -m "feat(tiles): the core expands the wall into the render plan each frame"
```

---

### Task 5: The shell sends the wall instead of solving it

**Files:**
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs:12524` (`ReconcileDynamicGalleryRoutes`)
- Modify: the scene-sync payload builder in `native-shell/CoreVideoPro.MediaCore/` (the type that emits `routes`; find it with `grep -rn "\"routes\"" native-shell/CoreVideoPro.MediaCore/`)
- Create: `native-shell/CoreVideoPro.MediaCore/Services/TilesLayerPayloadBuilder.cs`
- Test: `native-shell/CoreVideoPro.WinUI.Tests/TilesLayerPayloadBuilderTests.cs` (create)
- Do NOT delete: `Services/DynamicGalleryLayoutService.cs` — it stays as the reference implementation the C++ port is tested against, and its tests keep running.

**Interfaces:**
- Consumes: `DynamicGallerySettings` (existing, `Models/ProductionModels.cs:96`).
- Produces:
  ```csharp
  public static class TilesLayerPayloadBuilder
  {
      public static TilesLayerPayload? Build(
          Scene scene,
          IReadOnlyList<Participant> roomVideoParticipants);
  }

  public sealed record TilesLayerPayload(
      string LayerId,
      int Order,
      IReadOnlyList<string> Members,
      TilesStylePayload Style);

  public sealed record TilesStylePayload(
      string TileAspect,
      double CustomAspectRatio,
      double GutterPercent,
      double MarginPercent,
      string BackgroundColor);
  ```

- [ ] **Step 1: Write the failing test**

Create `native-shell/CoreVideoPro.WinUI.Tests/TilesLayerPayloadBuilderTests.cs`:

```csharp
using CoreVideoPro.WinUI.Models;
using CoreVideoPro.WinUI.Services;
using Xunit;

namespace CoreVideoPro.WinUI.Tests;

public class TilesLayerPayloadBuilderTests
{
    private static Scene GalleryScene(DynamicGallerySettings? settings = null) => new()
    {
        Id = "scene-1",
        Name = "CoreVideo Tiles",
        Layout = "dynamic-gallery",
        DynamicGallery = settings ?? new DynamicGallerySettings()
    };

    private static Participant Guest(string id, FeedHealth health = FeedHealth.Live) =>
        new() { Id = id, Name = $"Guest {id}", Health = health };

    [Fact]
    public void Build_ReturnsNullForANonGalleryScene()
    {
        var scene = GalleryScene();
        scene.DynamicGallery = null;
        Assert.Null(TilesLayerPayloadBuilder.Build(scene, [Guest("zoom:1")]));
    }

    [Fact]
    public void Build_CarriesEligibleMembersInRosterOrder()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(), [Guest("zoom:1"), Guest("zoom:2"), Guest("zoom:3")]);

        Assert.NotNull(payload);
        Assert.Equal(["zoom:1", "zoom:2", "zoom:3"], payload!.Members);
    }

    [Fact]
    public void Build_SkipsParticipantsWithVideoOff()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(), [Guest("zoom:1"), Guest("zoom:2", FeedHealth.VideoOff)]);

        Assert.Equal(["zoom:1"], payload!.Members);
    }

    [Fact]
    public void Build_TruncatesToMaxTiles()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(new DynamicGallerySettings { MaxTiles = 2 }),
            [Guest("zoom:1"), Guest("zoom:2"), Guest("zoom:3")]);

        Assert.Equal(["zoom:1", "zoom:2"], payload!.Members);
    }

    [Fact]
    public void Build_CarriesTheOperatorsStyleVerbatim()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(new DynamicGallerySettings
            {
                TileAspect = "1:1",
                GutterPercent = 2.5,
                MarginPercent = 3.0
            }),
            [Guest("zoom:1")]);

        Assert.Equal("1:1", payload!.Style.TileAspect);
        Assert.Equal(2.5, payload.Style.GutterPercent);
        Assert.Equal(3.0, payload.Style.MarginPercent);
    }

    // The shell must NOT pre-filter on its own idea of liveness beyond video-off.
    // Deciding who actually has frames is the core's job; duplicating it here is
    // how the two ends drift.
    [Fact]
    public void Build_DoesNotDropAParticipantMerelyBecauseItLooksUnhealthy()
    {
        var payload = TilesLayerPayloadBuilder.Build(
            GalleryScene(), [Guest("zoom:1", FeedHealth.Degraded)]);

        Assert.Equal(["zoom:1"], payload!.Members);
    }
}
```

If `FeedHealth` has no `Degraded` member, substitute any non-`VideoOff`, non-`Live` value the enum defines; the point of the case is that only `VideoOff` filters.

- [ ] **Step 2: Run to verify it fails**

Run:
```powershell
dotnet test native-shell/CoreVideoPro.WinUI.Tests/CoreVideoPro.WinUI.Tests.csproj -c Release -p:Platform=x64 --filter TilesLayerPayloadBuilderTests
```
Expected: FAIL to compile — `TilesLayerPayloadBuilder` does not exist.

- [ ] **Step 3: Write the builder and stop emitting gallery routes**

Create `native-shell/CoreVideoPro.MediaCore/Services/TilesLayerPayloadBuilder.cs` with the records from **Interfaces** and:

```csharp
public static TilesLayerPayload? Build(Scene scene, IReadOnlyList<Participant> roomVideoParticipants)
{
    if (scene.DynamicGallery is not { } settings)
    {
        return null;
    }

    // MEMBERSHIP POLICY ONLY. Whether a member actually has frames is the core's
    // decision (compositor::admitTilesMembers) — it is the process receiving them.
    // Deciding it twice is how the two ends drift.
    var members = roomVideoParticipants
        .Where(participant => participant.Health != FeedHealth.VideoOff)
        .Select(participant => participant.Id)
        .Distinct(StringComparer.Ordinal)
        .Take(Math.Clamp(settings.MaxTiles, 1, 64))
        .ToList();

    return new TilesLayerPayload(
        LayerId: $"tiles:{scene.Id}",
        Order: 0,
        Members: members,
        Style: new TilesStylePayload(
            TileAspect: DynamicGalleryLayoutService.NormalizeAspectPreset(settings.TileAspect),
            CustomAspectRatio: settings.CustomAspectRatio,
            GutterPercent: settings.GutterPercent,
            MarginPercent: settings.MarginPercent,
            BackgroundColor: "#000000"));
}
```

In `StudioViewModel.ReconcileDynamicGalleryRoutes` (`:12524`), delete the rect-solving and route-emitting body. A gallery scene now contributes **no** routes; it contributes a tiles payload. Keep the method as the place membership policy lives, and have the scene-sync payload builder call `TilesLayerPayloadBuilder.Build` and attach the result as the command's `tiles` node.

Serialize the payload camelCase to match `TilesLayerWire` from Task 1.

- [ ] **Step 4: Run tests to verify they pass**

Run:
```powershell
dotnet test native-shell/CoreVideoPro.WinUI.Tests/CoreVideoPro.WinUI.Tests.csproj -c Release -p:Platform=x64 --filter TilesLayerPayloadBuilderTests
```
Expected: PASS, 6 tests.

Then the whole shell suite:
```powershell
dotnet test native-shell/CoreVideoPro.WinUI.Tests/CoreVideoPro.WinUI.Tests.csproj -c Release -p:Platform=x64
```
Expected: 0 failures. Baseline before this plan was **725 passed / 0 failed** (verified 2026-08-15). `DynamicGalleryLayoutServiceTests` must still be running and green — it is the C++ port's oracle. If those tests vanished from the count, you deleted the service; restore it.

- [ ] **Step 5: Commit**

```bash
git add native-shell/CoreVideoPro.MediaCore/Services/TilesLayerPayloadBuilder.cs \
        native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs \
        native-shell/CoreVideoPro.WinUI.Tests/TilesLayerPayloadBuilderTests.cs
git commit -m "feat(tiles): the shell sends wall membership and stops solving layout"
```

---

### Task 6: The pixel oracle

**Files:**
- Create: `scripts/validate-tiles.mjs`
- Modify: `package.json` (add `"validate:tiles": "node scripts/validate-tiles.mjs"`)
- Reference: `scripts/validate-multiview.mjs` (harness shape — but note it judges STRUCTURE, never pixels; this one must do better), `scripts/validate-iso-record.mjs` (ffprobe/ffmpeg usage)

**Interfaces:**
- Consumes: the whole T1 stack, end to end.
- Produces: `npm run validate:tiles` → exit 0 on pass, non-zero with a named failure on fail.

This is the task that makes T2 safe to build. A wall validator that counts rects proves nothing — this repo has twice shipped a defect past a validator that checked a proxy which survived the bug: a recording that muxed a 320×180 thumbnail for months while every validator checked stream presence, and a sender whose container FPS read healthy while structurally capped at 50.

- [ ] **Step 1: Write the oracle**

Create `scripts/validate-tiles.mjs`. It must:

1. Spawn the core over stdio with `COREVIDEO_ZOOM_ENGINE_PATH` pointed at `corevideo-zoom-engine-fake.exe` — the env-var path, so no binary copy/restore dance is needed (`ZoomEngineRuntime::loadConfig` honours it).
2. Join, and wait until N fake participants are delivering frames. The fake engine emits **distinct animated I420 per participant**, which is what makes per-tile identification possible.
3. Send a scene graph carrying a `tiles` layer with those N members and a known `backgroundColor` (use a colour no fake participant produces, e.g. `#101418`).
4. Record a short program clip, then stop and wait until **ffprobe can read a duration** before killing the core — `stop-recording-session` returns before the MP4 moov atom lands, and a size-based wait reads an unfinalized file that decodes as zero frames, which looks exactly like a dead feed.
5. Extract a frame with ffmpeg and assert, against the solved rects the core published in its snapshot:
   - **Per-tile identity:** sample the centre of each solved rect; the sampled colour must match the fake participant assigned to that slot, and must NOT match its neighbours. This is the assertion that proves the wall draws the right person in the right box — the one a rect-count test cannot make.
   - **Background:** sample the gutter between two tiles and the margin at the canvas edge; both must be `#101418` within tolerance.
   - **Fill, not letterbox:** with a tile aspect deliberately mismatched to the source (set `tileAspect` to `1:1` against 16:9 sources), the tile must contain no background-coloured bars along its left/right edges.
   - **Reflow:** drop one participant, wait past `kTilesStaleFrameMs`, grab another frame, and assert the remaining tiles are larger than before and still identify correctly.
6. Print a machine-checkable summary line per assertion and exit non-zero naming the first failure.

Sample colours as a small median-filtered patch, not a single pixel — a single pixel lands on an animated feature and flaps.

- [ ] **Step 2: Run it against the current build to verify it passes**

Run:
```powershell
npm run validate:tiles
```
Expected: PASS on every assertion.

If per-tile identity fails while the count is right, the wall is drawing the wrong source in a slot — check the `sourceId` on the expanded layers before touching the oracle's tolerances.

- [ ] **Step 3: Deliberately break it, to prove the oracle bites**

Temporarily reverse the admitted-member order in the Task 4 expansion (`std::reverse(admitted.begin(), admitted.end());`), rebuild, and re-run.

Expected: **FAIL** on per-tile identity. A wall validator that still passes with the members reversed is checking a proxy, not the pixels — fix the oracle before continuing.

Revert the deliberate break and re-run; expected PASS.

- [ ] **Step 4: Commit**

```bash
git add scripts/validate-tiles.mjs package.json
git commit -m "test(tiles): judge the wall on pixels, not on rect counts"
```

---

## Definition of done for T1

- [ ] `native\build-dev\corevideo-native-tests.exe` — 0 failures, total ≥ baseline + 25 new tests.
- [ ] `dotnet test native-shell/CoreVideoPro.WinUI.Tests/...` — 0 failures, ≥ 731 passed (725 baseline + 6 new), with `DynamicGalleryLayoutServiceTests` still present and green.
- [ ] `npm run validate:tiles` — passes, and **provably fails** when member order is reversed.
- [ ] A gallery scene emits no scene routes; the wall is entirely core-expanded.
- [ ] `python scripts/mac-show-drill.py --seconds 40 --load 8` on real hardware shows no regression against its pre-T1 numbers. Mean fps is not a health metric — read frame DELIVERY and the coreMutex over-budget ratio, and confirm the fake engine actually sourced the load you asked for.
- [ ] CLAUDE.md gains a Tiles section: the wall is core-expanded per frame, the shell owns membership only, and `DynamicGalleryLayoutService` is retained as the port's reference oracle rather than as live code. Per the standing directive, docs-updated is part of done.

## Notes for whoever picks up T2

The expansion in Task 4 is where borders, corner radius, and glow attach — extend those layers, do not add a parallel path. The glow is an analytic SDF falloff on an expanded quad with a null texture, **not** a blur; its softness coefficient caps at 2 for a derived reason. Read `docs/obs-plugin-tiles-behavioral-contract.md` §5 and the spec's Draw section before writing any HLSL, and note the three traps in the spec that fail silently — the spacing divisor form, crop UVs from the truncated integer rect, and the even-snap epsilon.
