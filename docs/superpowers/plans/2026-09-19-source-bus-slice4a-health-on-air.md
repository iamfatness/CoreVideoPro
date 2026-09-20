# Source Bus — Slice 4a (bus health on air: slates by health, per-source dropout policy) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Retire the per-kind "pink tile" (`colorFromParticipantId` placeholder) everywhere and render a layer whose source is not producing according to its **bus health** and the operator's **per-source dropout policy**, per the owner rulings of 2026-09-19: *warming* = neutral dark slate; *failed / missing* = dark slate with the source name; *stalled* (had frames, none for 200 ms+) = operator's choice per source on the Sources page, **hold last frame** (default) or **black**. This is #535 done-when #3 ("no per-kind special case for empty frames") delivered as one bus-health path across the CPU preview, D3D11 and Metal compositors, with the policy operator-set, persisted, and carried on the wire.

**Architecture:** MediaCore already knows every live source's health from the bus (`SourceBus::snapshot` derives warming/producing/stalled from `lastNewFrameNs`). The render plan layer gains three fields — `sourceHealth`, `dropoutPolicy`, `sourceDisplayName` — filled when the plan is built (one lookup per layer by the layer's frame key). The compositors stop inventing a colour from the id and resolve one rule: no content frame → slate (failed slate carries the name); content frame but health `stalled` and policy `black` → black; otherwise draw the frame (which for `hold` is the producer's held last frame — the bus already passes held frames through every tick). The policy is a per-source setting in the shell (Sources page ComboBox, persisted in `ProductionOutputPreferences.SourceDropoutPolicies`, default `hold`), shipped to the core with the source's display name on every production sync as `set-source-policy`, and echoed in `sources[]`.

**Tech Stack:** C++17 core (`native/`), GoogleTest via the repo shim (ONE wildcard per `--gtest_filter`), D3D11 (`native/src/modules/D3D11CompositorAdapter.cpp`), Metal (`native/src/compositor/MetalCompositorAdapter.mm`, CI-only), CPU preview (`native/src/modules/ProgramFramePreview.cpp`), WinUI 3 / C# shell (`native-shell/`), xUnit.

**Spec:** owner rulings recorded on #449 / #535 (2026-09-19) and `docs/superpowers/specs/2026-09-19-source-bus-slice4-scoping.md` item 4; source-bus design `docs/superpowers/specs/2026-09-18-source-bus-design.md` §5 slice 4 ("compositor's per-kind empty-frame fallbacks collapse to one bus-health path").

## Global Constraints

- **Colours (canonical, `native/src/compositor/CompositorLayout.h`):** `kWarmingSlateRgba = 0xff1b1f27u` (neutral dark), `kFailedSlateRgba = 0xff23181cu` (dark, faintly warm so a failed source is distinguishable on a multiview at a glance), `kDropoutBlackRgba = 0xff000000u`. RGBA packing is the existing `0xAARRGGBB` used by `colorFromParticipantId`. `colorFromParticipantId` STAYS (tests and Tiles membership use it) but no layer-resolution path may call it any more.
- **One resolution rule, identical in the CPU preview, D3D11 and Metal:**
  1. layer has a content frame (`frameHasContent`) and NOT (`sourceHealth == "stalled" && dropoutPolicy == "black"`) → draw the frame (unchanged path).
  2. layer has a content frame, `sourceHealth == "stalled"`, `dropoutPolicy == "black"` → solid `kDropoutBlackRgba`, frame ignored.
  3. no content frame, `sourceHealth == "failed"` → solid `kFailedSlateRgba` + the source name (D3D11 via the existing overlay text raster; Metal via `rasterOverlayTileCoreText` if it composes cleanly, else colour only with a `// TODO(4a-metal-text)` and a note in Task 5 docs; CPU preview colour only).
  4. no content frame, any other health (`warming`, `producing` momentarily, `unknown`, empty) → solid `kWarmingSlateRgba`.
  Deliberately sourceless layers (`hasFillColor`, overlays) are untouched.
- **Health strings on the layer:** `"producing" | "warming" | "stalled" | "failed" | ""`. Mapping at plan build: bus `healthFor(key)` present → its `sourceHealthName`; absent from the bus → `"warming"` if this tick's `videoFrames` contains a frame with that key (the Zoom roster's metadata-only frame = subscribed, not yet decoded), else `"failed"`. Frameless plan builds (audio worker, `sessionState`) leave `""`.
- **Frame key per layer** (the same key the compositors already look up): `layer.participantId` when non-empty (Zoom raw pid, `capture:<id>`), else `layer.sourceId.empty() ? "media:" + layer.mediaAssetId : layer.sourceId`.
- **Policy values:** `"hold"` (default when unset) | `"black"`. Anything else is rejected by the command with a warning and left unchanged.
- **Wire:** command `set-source-policy` `{ "sourceId": "<canonical: zoom:<pid> | capture:<id> | media:<id>>", "dropoutPolicy": "hold"|"black", "displayName": "<string, optional>" }`. The core keys policies by the frame key it will look up at plan time: `zoom:<pid>` → raw `<pid>`; `capture:<id>` and `media:<id>` unchanged. `sources[]` entries gain `"dropoutPolicy"` (echo) and `"displayName"` (if known). The shell re-sends every persisted policy on each production sync (idempotent, like colour grade).
- **Zero pixel work under `coreMutex`; no new per-frame allocation** beyond one string per layer per plan build.
- **Regression gates (must stay green):** full Windows dev suite (0 failed), stub gate `scripts/test-native.ps1`, `validate-multiview.mjs`, `validate-tiles.mjs`, `scripts/qa/zoom-gap-hold-ab.py` (luma holds — and its slate signature CHANGES: the dropout measurement compares against the new slate constants), the MediaCore shell test project, the WinUI test project. The 18 existing assertions on `colorFromParticipantId` placeholder colours across 9 test files are REWRITTEN to the new constants (never deleted; each keeps asserting the same scenario).
- **Branch:** `codex/535-slice4a-health-on-air` off `origin/main` (ce46cc1 or later). Build with `--config Release`; Release core ~2.27 MB.

---

### Task 1: `SourceBus::healthFor` + plan-layer fields + `set-source-policy` in MediaCore

**Files:**
- Modify: `native/src/core/SourceBus.h` (add `healthFor`)
- Modify: `native/src/modules/Interfaces.h` (`CompositorRenderPlanLayer`: 3 fields)
- Modify: `native/src/core/MediaCore.h` / `native/src/core/MediaCore.cpp` (policy map, command, `sources[]` echo, layer annotation in every layer builder)
- Modify: `native/src/compositor/CompositorLayout.h` (the three colour constants — needed by Task 2 and the tests here)
- Test: `native/tests/SourceBusTest.cpp`, `native/tests/MediaCoreCommandTest.cpp`

**Interfaces:**
- Produces: `std::optional<SourceHealth> SourceBus::healthFor(const std::string& sourceId, int64_t nowNs) const` (same derivation as `snapshot()`; `nullopt` when absent).
- Produces on `CompositorRenderPlanLayer`: `std::string sourceHealth;` `std::string dropoutPolicy = "hold";` `std::string sourceDisplayName;`
- Produces: MediaCore command `set-source-policy` (see Wire), member `std::unordered_map<std::string, SourcePolicy> sourcePolicies_` where `struct SourcePolicy { std::string dropoutPolicy = "hold"; std::string displayName; };` keyed by frame key; `sources[]` snapshot entries add `dropoutPolicy` + `displayName`.
- Produces: `void MediaCore::annotateLayerSource(modules::CompositorRenderPlanLayer& layer, const std::vector<modules::VideoFrame>& videoFrames, int64_t nowNs) const` — sets the three fields per Global Constraints. Called for every non-overlay, non-fill layer in `buildCompositorRenderPlan` / the preview plan / Tiles slot layers / the grid fallback (every place that sets `layer.participantId` or `layer.mediaAssetId`).
- Consumes: `lastRenderPlanForTest()` (existing seam, `MediaCore.h` ~line 302) to assert layer fields in tests.

- [ ] **Step 1: Failing tests**

```cpp
// SourceBusTest.cpp
TEST(SourceBus, HealthForReportsWarmingProducingStalledAndAbsent) {
  corevideo::core::SourceBus bus;
  EXPECT_FALSE(bus.healthFor("capture:cam", 0).has_value());
  auto cam = std::make_shared<corevideo::core::CaptureDeviceSource>("capture:cam", 640, 360);
  bus.add(cam);
  EXPECT_EQ(bus.healthFor("capture:cam", 1000), corevideo::core::SourceHealth::Warming);
  cam->setLatest(bgraFrame("capture:cam", 640, 360, 1));
  (void)bus.ingest(0, 1000);
  EXPECT_EQ(bus.healthFor("capture:cam", 1000), corevideo::core::SourceHealth::Producing);
  EXPECT_EQ(bus.healthFor("capture:cam", 1000 + 300'000'000), corevideo::core::SourceHealth::Stalled);
}
```

```cpp
// MediaCoreCommandTest.cpp — policy command round-trips and annotates the plan.
TEST(MediaCoreCommand, SourcePolicyCommandIsEchoedAndAnnotatesRouteLayers) {
  corevideo::core::MediaCore mediaCore(corevideo::modules::createStubModules());
  // capture:decklink-1 is connected with signal in the stub set (CaptureIngest tests).
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{
      corevideo::rpc::Json::Object{{"type", "set-source-policy"}, {"sourceId", "capture:decklink-1"},
                                   {"dropoutPolicy", "black"}, {"displayName", "Camera 1"}},
      corevideo::rpc::Json::Object{{"type", "set-source-policy"}, {"sourceId", "zoom:16778240"},
                                   {"dropoutPolicy", "bogus"}},   // rejected, warns, stays default
      corevideo::rpc::Json::Object{{"type", "load-scene-graph"}, {"sceneId", "cam"},
          {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
              {"routeId", "cam-0"}, {"mode", "capture-input"}, {"captureDeviceId", "decklink-1"},
              {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}}}}}});
  mediaCore.renderDisplayTick();
  const auto& plan = mediaCore.lastRenderPlanForTest();
  const auto layer = std::find_if(plan.layers.begin(), plan.layers.end(),
      [](const auto& l) { return l.participantId == "capture:decklink-1"; });
  ASSERT_NE(layer, plan.layers.end());
  EXPECT_EQ(layer->sourceHealth, "producing");
  EXPECT_EQ(layer->dropoutPolicy, "black");
  EXPECT_EQ(layer->sourceDisplayName, "Camera 1");
  const auto state = mediaCore.sessionState();
  bool echoed = false;
  for (const auto& s : state.get("sources")->asArray()) {
    if (s.getString("sourceId") == "capture:decklink-1") { echoed = true; EXPECT_EQ(s.getString("dropoutPolicy"), "black"); EXPECT_EQ(s.getString("displayName"), "Camera 1"); }
  }
  EXPECT_TRUE(echoed);
  // A layer whose key is on nobody's bus and in no frame reads "failed".
  (void)mediaCore.applyCommands(corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
      {"type", "load-scene-graph"}, {"sceneId", "gone"},
      {"routes", corevideo::rpc::Json::Array{corevideo::rpc::Json::Object{
          {"routeId", "gone-0"}, {"mode", "capture-input"}, {"captureDeviceId", "no-such-device"},
          {"rect", corevideo::rpc::Json::Object{{"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}}}}}}}});
  mediaCore.renderDisplayTick();
  const auto& plan2 = mediaCore.lastRenderPlanForTest();
  ASSERT_FALSE(plan2.layers.empty());
  EXPECT_EQ(plan2.layers.front().sourceHealth, "failed");
  EXPECT_EQ(plan2.layers.front().dropoutPolicy, "hold");  // default when unset
}
```
If the route mode/field names for a capture route differ from `"capture-input"` / `captureDeviceId` in this repo, copy them from `CaptureIngest.CaptureFrameCompositesRealPixelsIntoProgramPreview`. If `lastRenderPlanForTest()` is only refreshed by a specific call (read its comment at `MediaCore.h` ~302), use that call instead of `renderDisplayTick()`.

- [ ] **Step 2: Build → tests fail** (no `healthFor`, no fields, unknown command ignored → `sourceHealth` empty).
- [ ] **Step 3: Implement.** `healthFor` mirrors the health block in `snapshot()` (factor a private `healthOf(const Entry&, nowNs)` and use it in both). Add the three fields to `CompositorRenderPlanLayer` with the comment "bus health on air (#535 slice 4a): filled by MediaCore at plan build; compositors resolve slate/black/frame from these, never from the id". Add `SourcePolicy`, `sourcePolicies_`, the command handler `setSourcePolicy(const rpc::Json&)` (validate policy; strip `zoom:` prefix into the raw pid key; store; unknown policy → push a scene-validation warning "set-source-policy: unknown dropoutPolicy '<x>' for <id>"), wire `type == "set-source-policy"` next to `set-color-grade`. Add the constants to `CompositorLayout.h`. Implement `annotateLayerSource` and call it from every layer builder (grep `layer.participantId = ` and `layer.mediaAssetId = ` in `MediaCore.cpp`; the Tiles slot layers and the grid fallback included). `sources[]` echo: look up `sourcePolicies_` by the entry's sourceId.
- [ ] **Step 4: Run** `*HealthFor*`, `*SourcePolicyCommand*`, `*SourceBus*`, `*MediaCoreCommand*`, `*CaptureIngest*`, `*TilesRenderPlan*`, full suite (0 failed).
- [ ] **Step 5: Commit** `feat(#535): bus healthFor, set-source-policy, and health/policy/name on render-plan layers (slice 4a)`

---

### Task 2: One resolution rule in the three compositors; retire the pink tile; rewrite the placeholder assertions

**Files:**
- Modify: `native/src/modules/ProgramFramePreview.cpp` (layer colour resolution ~lines 380-402 and the synthetic-fill draw), `native/src/modules/D3D11CompositorAdapter.cpp` (`resolveLayers` ~700-750; `drawLayer` for the black-on-stalled rule and the failed-slate name), `native/src/compositor/MetalCompositorAdapter.mm` (~800-865, same rule)
- Modify (assertions rewritten, scenarios kept): `native/tests/CaptureIngestTest.cpp`, `D3D11CompositorTest.cpp` (5), `MediaCoreCommandTest.cpp` (3), `MediaPlaybackTimelineTest.cpp`, `MetalCompositorTest.cpp`, `ProgramPixelContinuityTest.cpp` (2, comments), `SourceBusTest.cpp`, `StillMediaFrameCacheTest.cpp` (2), `TilesRenderPlanTest.cpp` (2, comments)
- Test (new): `native/tests/ProgramFramePreviewHealthTest.cpp` (register in `native/CMakeLists.txt` next to `CaptureIngestTest.cpp`)

**Interfaces:**
- Consumes: the three layer fields and constants from Task 1. `frameHasContent` (D3D11) / `hasPixels` (CPU). D3D11 overlay text: `overlayRaster_.rasterOverlayTexture(device, context, CompositorOverlayContent{ .title = layer.plan.sourceDisplayName }, rect, targetWidth_, targetHeight_)` drawn with the overlay pixel shader over the slate (reuse the body of `drawOverlayLayer`'s textured branch; no animation).
- Produces: `inline uint32_t compositor::slateColorFor(std::string_view sourceHealth)` in `CompositorLayout.h` (`"failed"` → `kFailedSlateRgba`, else `kWarmingSlateRgba`) and `inline bool compositor::blackOnStalled(std::string_view health, std::string_view policy)` — the single rule both compositors and the CPU preview call, so the rule cannot drift.

- [ ] **Step 1: Failing tests** (`ProgramFramePreviewHealthTest.cpp`, CPU path so it runs everywhere; build the plan/frames directly the way `D3D11CompositorTest.cpp` builds a `CompositorRenderPlan` and `previewPixelRgba` reads pixels — copy that helper):

```cpp
// One full-canvas layer per case; sample the centre pixel of the CPU preview.
TEST(ProgramFramePreviewHealth, NoFrameWarmingDrawsTheWarmingSlate)   { /* layer.participantId="capture:a"; sourceHealth="warming"; frames={} → kWarmingSlateRgba */ }
TEST(ProgramFramePreviewHealth, NoFrameFailedDrawsTheFailedSlate)     { /* sourceHealth="failed" → kFailedSlateRgba */ }
TEST(ProgramFramePreviewHealth, NoFrameUnknownHealthDrawsTheWarmingSlateNotAnIdColour) { /* sourceHealth="" → kWarmingSlateRgba, and != colorFromParticipantId("capture:a") */ }
TEST(ProgramFramePreviewHealth, StalledWithBlackPolicyDrawsBlackEvenWithAHeldFrame) { /* frame with solid green pixels; sourceHealth="stalled"; dropoutPolicy="black" → kDropoutBlackRgba */ }
TEST(ProgramFramePreviewHealth, StalledWithHoldPolicyDrawsTheHeldFrame) { /* same frame; dropoutPolicy="hold" → green */ }
TEST(ProgramFramePreviewHealth, MediaLayerUsesTheSameRule) { /* layer.mediaAssetId="clip-1", no frame, sourceHealth="failed" → kFailedSlateRgba */ }
```
Write each body fully (plan of 1 layer at rect 0,0,1,1; `renderProgramFramePreview`/the CPU entry the existing tests call; assert with `previewPixelRgba`).

- [ ] **Step 2: Build → the new tests fail** (CPU preview still paints `colorFromParticipantId`).
- [ ] **Step 3: Implement** in all three renderers per the Global rule; D3D11 failed-slate name via the overlay raster (skip text when `sourceDisplayName` is empty); Metal: text via `rasterOverlayTileCoreText` if the existing overlay path can be reused in ≤ 30 lines, otherwise colour-only with the TODO. Then rewrite the 18 assertions: each existing scenario that expected `colorFromParticipantId(X)` for a *placeholder* now expects `kWarmingSlateRgba` (or `kFailedSlateRgba` where the test's layer is annotated failed — most existing tests build plans without health, so they read `""` → warming slate). Keep `colorFromParticipantId` in tests that use it for *distinct-source identity* only if the pixels drawn come from real frames (read each test before changing it; the two comment-only references stay comments updated to the new rule).
- [ ] **Step 4: Run** `*ProgramFramePreviewHealth*`, `*D3D11Compositor*`, `*CaptureIngest*`, `*StillMediaFrameCache*`, `*TilesRenderPlan*`, `*ProgramPixelContinuity*`, `*MediaPlaybackTimeline*`, `*MediaCoreCommand*`, `*SourceBus*`, full suite (0 failed). Metal compiles only on macOS CI: mirror the D3D11 change carefully; the stub gate + CI's `native-metal-macos` job is the check.
- [ ] **Step 5: Commit** `feat(#535): compositors render by bus health and dropout policy; pink placeholder retired (slice 4a)`

---

### Task 3: Shell — per-source "On dropout" setting, persisted, shipped on every sync, control action

**Files:**
- Modify: `native-shell/CoreVideoPro.WinUI/Models/ProductionModels.cs` (`FeedHealthRow.DropoutPolicy`, and `BuildFeedHealthRows` takes the policy map)
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.cs` (`_sourceDropoutPolicies`, `SetSourceDropoutPolicy(string sourceId, string policy)`, `DropoutPolicyOptions`, persistence load/save next to `SourceDisplayNames` ~lines 11842 / 12049, sync context population)
- Modify: `native-shell/CoreVideoPro.WinUI/Services/ProductionOutputPreferencesStore.cs` (`Dictionary<string,string> SourceDropoutPolicies`, `CurrentVersion` 12 → 13, migration = empty map)
- Modify: `native-shell/CoreVideoPro.MediaCore/Models/MediaCoreProductionSyncContext.cs` (`IReadOnlyDictionary<string, MediaCoreSourcePolicyWire> SourcePolicies`), `native-shell/CoreVideoPro.MediaCore/Services/MediaCoreCommandBuilder.cs` (`BuildSourcePolicyCommands` emitted right after `BuildColorGradeCommand` at line ~37)
- Modify: `native-shell/CoreVideoPro.WinUI/Views/SourcesInputsPage.xaml` + `.xaml.cs` (an `OnDropoutCombo` per guest row next to `ProductionRoleCombo`, and one per capture-device row next to "Take offline"; same Loaded/ElementPrepared sync + SelectionChanged pattern as the role combo, verbatim)
- Modify: `native-shell/CoreVideoPro.Control/ControlActionRegistry.cs` + `native-shell/CoreVideoPro.WinUI/Services/StudioControlSurface.cs` (`source.dropout.set` with params `sourceId` (s, required, "zoom:<pid> | capture:<id>"), `policy` (s, required, "hold | black"))
- Tests: `native-shell/CoreVideoPro.MediaCore.Tests/MediaCoreCommandBuilderTests.cs` (command shape + keying), `native-shell/CoreVideoPro.WinUI.Tests/` (preferences round-trip incl. v12→v13 migration; `BuildFeedHealthRows` carries the policy; control-surface binding test in the existing surface test class)

**Interfaces:**
- Produces: `public sealed record MediaCoreSourcePolicyWire(string SourceId, string DropoutPolicy, string? DisplayName);` command `set-source-policy` with exactly the Wire fields. Policy strings `"hold" | "black"`; options labels "Hold last frame" / "Black".
- Behavior: the shell sends a `set-source-policy` for every source that has EITHER a persisted policy OR a display name (so the core learns names for the failed slate) — canonical ids `zoom:<pid>` / `capture:<id>`; the core strips `zoom:`.

- [ ] **Step 1: Failing tests** (builder: given a context with `SourcePolicies = { "zoom:16778240": ("black","Jamal"), "capture:cam": ("hold", null) }` the command list contains two `set-source-policy` commands with those fields, in id order; preferences: serialize/deserialize a v13 with two policies round-trips, and a v12 JSON deserializes with an empty map and `migratedFromOlderVersion == true`; rows: `BuildFeedHealthRows(participants, roles, subs, policies)` puts `"black"` on the matching row and `"hold"` on others).
- [ ] **Step 2: Run → fail (types missing).**
- [ ] **Step 3: Implement** per Files; XAML: `<ComboBox x:Name="DropoutPolicyCombo" Header="On dropout" ItemsSource="{Binding ViewModel.DropoutPolicyOptions, ElementName=SourcesInputsRoot}" SelectedValuePath="Value" Tag="{x:Bind}" Loaded="OnDropoutPolicyComboLoaded" SelectionChanged="OnDropoutPolicyChanged"/>`; code-behind mirrors `SyncProductionRoleCombo`; VM method persists, updates rows, and `_ = TrySyncMediaCoreAsync()`.
- [ ] **Step 4: Run** `dotnet test native-shell/CoreVideoPro.MediaCore.Tests/...` and the WinUI test project (see `package.json` `test:native-shell` for the exact flags) — 0 failed.
- [ ] **Step 5: Commit** `feat(shell): per-source "On dropout" (hold last frame | black) on the Sources page, persisted and shipped as set-source-policy (#535 slice 4a)`

---

### Task 4: Gates, live check, docs

**Files:** `CLAUDE.md` (source-bus section: slice 4a paragraph; the "pink tile" guardrail paragraph in the compositor notes updated to the new rule), `docs/BACKLOG.md` (#535 row: 4a ready on branch; done-when #3 status), `docs/superpowers/specs/2026-09-18-source-bus-design.md` §5 (slice 4 → 4a shipped-on-branch / 4b remaining: interface retirement per the scoping note), `scripts/qa/zoom-gap-hold-ab.py` docstring (the slate luma signature is now `kWarmingSlateRgba`, not the pink; state its Y value).

- [ ] **Step 1:** full dev suite; stub gate; `validate-multiview.mjs`; `validate-tiles.mjs`; gap-hold recording — compute the expected luma of `kWarmingSlateRgba` (BT.709 from R=0x1b,G=0x1f,B=0x27 ≈ 30) and confirm the dropout window shows the HELD frame (default policy `hold`, luma ~188, no dip) — then a second run after sending `set-source-policy {sourceId: "zoom:103", dropoutPolicy: "black"}` via the harness's spine/sync path (add a `--policy black` flag to the harness that issues the command before the dropout) and confirm the window drops to black (luma ~16) for exactly the gap. Both runs recorded and archived under `artifacts/qa/slice4a/`.
- [ ] **Step 2 (live, if the owner is around; otherwise state "not live-tested"):** launch the app, join the test meeting, set one guest to "Black" on the Sources page, confirm the row persists across a relaunch and the snapshot `sources[]` echoes it.
- [ ] **Step 3: Docs** as listed; numbers from Step 1.
- [ ] **Step 4: Commit** `docs(#535): slice 4a — bus health on air, dropout policy, pink tile retired`.

---

## Self-Review

**Rulings coverage:** warming = neutral dark slate → Task 2 rule 4 + constant; failed = slate with name → rule 3 (name on D3D11, best-effort Metal, colour-only CPU, stated); stalled = per-source hold/black on the Sources page → Task 1 (policy on the wire + layer), Task 2 (rule 2), Task 3 (UI + persistence + sync); pink retired for every kind → Task 2 (all three renderers, all layer kinds, assertions rewritten). Done-when #3: compositors consume health, no per-kind empty-frame branch remains (the `participantId` / `mediaAssetId` branches now differ only in the key they look up).

**Placeholder scan:** Task 2 Step 1 lists six tests as one-line intents with the exact expected constants and instructs full bodies; Task 3 Step 1 states each assertion's inputs and expectations. No TBD.

**Type consistency:** `sourceHealth` / `dropoutPolicy` / `sourceDisplayName` (Task 1) read by `slateColorFor` / `blackOnStalled` (Task 2); `set-source-policy` fields identical in Task 1 (core) and Task 3 (builder); policy strings `"hold"|"black"` everywhere; constants named identically in `CompositorLayout.h` and every test.

**Risk:** Task 2 touches the draw path of both live compositors; the CPU test file pins the rule, the D3D11 tests are rewritten scenario-for-scenario, and the gap-hold harness now measures the policy end-to-end (hold vs black). Metal is CI-only: mirror exactly and rely on `native-metal-macos`.
