# Graphics Designer Slice 2 — Designer Surface Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An author opens a design in a dedicated Designer (mockup D1) — layers panel, canvas with safe-area/centre snapping and sample text, a per-layer inspector, Undo / Save / Save to playlist — and never needs the Advanced editor's layer pane again.

**Architecture:** Every Designer edit is a pure document edit in MediaCore (`GraphicsTemplateEditor` structure operations, `GraphicsDesignerModel` rows / drop targets / inspector, `GraphicsSnap`, `GraphicsPreviewSamples`) applied through the existing `GraphicsWorkspaceSession.EditTemplate` private draft, so nothing reaches Preview/Program until Show/Cue in Operate. A new thin WinUI `GraphicsDesignerControl` hosts those helpers beside the existing `GraphicsAuthoringCanvas` and `GraphicsVariablesPanel`; the Graphics page shows exactly one Design surface (Builder, Designer, or the old workspace). The native template format already carries everything this slice needs (groups, `visible`, free-form node `metadata`) — no native code changes; a native fixture test pins that. Task 1 first clears the Slice 1 review follow-ups (#1087) in the same workflow code.

**Tech Stack:** C# / .NET 9, WinUI 3 (code-built controls), xUnit; native C++17 gtest shim (fixture test only).

**Spec:** `docs/superpowers/specs/2026-11-01-graphics-designer-phase2-design.md` (§3, Slices item 2, Acceptance 1; owner rulings 1–2). Issue #1087 (N1, N2, N4, N5, (c); N3 folded into Task 6; (b) stays with Slice 4).

## Global Constraints

- Owner ruling: **the Designer replaces the Advanced editor**; the five-step builder stays as the quick path ("Build a graphic"); Design gets a second entry, **"Open in Designer"**.
- Everything edits the private draft. The Designer never sends `graphics.instance.*`, cue, show or update; its only native traffic is the design lifecycle's template install/remove on Save.
- Native template limits (GraphicsTemplate.cpp, unchanged): ≤ 64 layers; group nesting ≤ 8; ≤ 32 fields; node keys `id,type,x,y,width,height,opacity,visible,metadata` + group `children,flow,gap,padding` / rect `fill,cornerRadius` / text `text,font,fontWeight,fontSize,lineHeight,color,align,maxLines,overflow,collapseWhenEmpty` / image `asset,fit,align,verticalAlign,collapseWhenEmpty,missing`; non-flow `x`,`y` 0–8192; `width`,`height` 1–8192; `opacity` 0–1; `cornerRadius` 0–4096; `fontSize` 1–512; `fontWeight` 400/500/600/700; `maxLines` 1–3; `overflow` `ellipsis` | `wrap-then-ellipsis`; `fit` `contain` | `cover`; `align` `left|center|right`; `verticalAlign` `top|center|bottom`. Shapes: `rect` only (no stroke) this slice.
- Layer name = `metadata.label` (1–64 chars, trimmed); lock = `metadata.editorLocked` (the existing key — keep it); hide = native `"visible": false` (hidden on air too; showing removes the property).
- Canvas 1920×1080; safe area 1728×972 at (96, 54); centre guides x = 960, y = 540; snap threshold 8 canvas px; holding Ctrl while dragging disables snapping.
- Colours: brand tokens `brand.primary` "Brand · plate", `brand.accent` "Brand · accent", `brand.background` "Brand · background", `brand.foreground` "Brand · foreground", `brand.secondary` "Brand · secondary", `brand.onPrimary` "Brand · ink", `brand.onPrimaryMuted` "Brand · ink muted", or hex `#RRGGBB` / `#RRGGBBAA`.
- Typefaces: `brand.fontHeading` "Brand heading", `brand.fontBody` "Brand body", or one of `GraphicsBuilder.TypefaceChoices`.
- Sample text buttons: **Typical** = `current`, **Longest name** = `longest-name`, **Empty title** = `blank`.
- Layout budgets (Graphics page content, same measurement as Slice 1): 1360×768 → page 1320×640; 1600×900 → page 1560×772.
- Out of scope: per-layer motion, Play preview (Slice 3); ellipse/line/stroke, outline, drop shadow, Size to content, text shrink, image import (Slice 4); multi-select, rotation, rulers.
- No input injection; tests never launch/kill/attach to a running app (the layout probe is its own isolated process). Native tests from `native\build-dev` with `COREVIDEO_VCAM_SHM_DIR` set and an exact `--gtest_filter=Suite.Name`.
- Docs-updated is part of done (CLAUDE.md row, `docs/reference/graphics-playout.md`, `docs/BACKLOG.md`).
- Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Group, ungroup or move a layer into a group** — the layer stays where it was on the canvas (children are relative to the group origin + padding, and native rejects negative non-flow x/y). Pinned in Task 2 (`GroupThenUngroupKeepsCanvasPositions`, `MoveIntoAGroupKeepsCanvasPositionAndGrowsTheGroup`).
2. **Eye toggle on a layer whose visibility follows a variable** (`"visible":{"field":"show-title"}`) — refused with the variable named; the binding is kept. Pinned in Task 2 (`HidingAVariableBoundLayerIsRefused`).
3. **Delete or ungroup a group holding the required `name` layer** — a required variable can never be left unused (native `required-template-field-unused`). Pinned in Task 2 (`DeletingAGroupThatHoldsTheRequiredNameIsRefused`).
4. **Sample buttons on a design whose new variable (`location`) exists only in the unsaved draft** — Empty title / Longest name still cover it. Pinned in Task 4 (`SamplesCoverVariablesAddedInTheDraft`).
5. **Drag a group onto its own child, or drag a locked layer** — refused (no cycle, no silent move). Pinned in Task 3 (`DropTargetRefusesCyclesAndLockedLayers`).

---

### Task 1: Slice 1 follow-ups (#1087 N1, N2, N4, N5, (c))

**Files:**
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsWorkspaceSession.cs` (Observe browse-edit drop :118; add `Rebase`, `ForgetBrowseEdits` beside `Browse` :248-255)
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsDesignWorkflow.cs` (`SaveAsync` :65-96, `Carry` :141-147)
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsDesignService.cs` (`Install` :204-216, `EvictUnusedDesigns` :218-228, `Remove`)
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsPlayoutService.cs` (`Ensure` :300-315, `GraphicsTemplateInstall` :478-504)
- Create: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsDesignEviction.cs`
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsDesignServiceTests.cs`, `GraphicsPlayoutServiceTests.cs`

**Interfaces:**
- Produces on `GraphicsWorkspaceSession`:
  - `public void Rebase(NativeGraphicsDraft value)` — same template id+version as `Draft` only; sets `Draft` and the original to `value`, clears undo and gesture, no undo entry.
  - `public void ForgetBrowseEdits(string templateId, int keepVersion)` — drops remembered browse drafts of every other version of `templateId`.
- Produces: `internal static class GraphicsDesignEviction { public static Task<int> EvictUnusedAsync(GraphicsWorkspaceLibrary library, NativeGraphicsState state, Func<string, Dictionary<string, object?>, CancellationToken, Task<NativeGraphicsOperationResult>> send, Func<TimeSpan, CancellationToken, Task> delay, CancellationToken ct); public static Task<NativeGraphicsOperationResult> RemoveAsync(...same send/delay..., string templateId, int version, CancellationToken ct); }` — keep set = `GraphicsWorkspaceLibrary.ReferencedTemplates` ∪ every instance's draft/cue/program versions and images ∪ `library.EditorDraft` ∪ `library.UnsavedTemplateEdits`; removes only `GraphicsDesignIds.IsDesignTemplateId` versions; `RemoveAsync` keeps today's 3 × 100 ms `template-library-busy` retry.
- Changes `GraphicsTemplateInstall.RunAsync(..., CancellationToken cancellation, Func<CancellationToken, Task<int>>? evictWhenFull = null)` — on a `LibraryFull` refusal, runs `evictWhenFull` once and retries once when it removed ≥ 1. `GraphicsDesignService.Install` and `GraphicsPlayoutService.Ensure` both pass `ct => GraphicsDesignEviction.EvictUnusedAsync(...)` (one evictor, issue item (c)).

- [ ] **Step 1: Write the failing tests**

```csharp
// GraphicsDesignServiceTests
[Fact] public async Task MovingOffADesignKeepsItsTypedValuesStyleAndPlacement() // N1
{
    var (_, _, workflow) = BuildWorkflow();
    var a = (await workflow.CreateAsync("lower-third", null, 0, "Guest strap")).Design!;
    _session!.SetField("title", JsonSerializer.SerializeToElement("London"));
    _session.SetStyle(_session.Draft!.Style with { AccentColor = "#FF8800" });
    Assert.True((await workflow.CreateAsync("title", null, 0, "Opening title")).Ok);
    Assert.Contains(_commands, c => c.Action == "graphics.template.remove" && c.Body.GetProperty("templateId").GetString() == a.TemplateId); // still released
    Assert.True((await workflow.OpenAsync(a.DesignId)).Ok);
    Assert.Equal("London", _session.Draft!.Fields.GetProperty("title").GetString());
    Assert.Equal("#FF8800", _session.Draft.Style.AccentColor);
}
[Fact] public async Task SavingWhileTheOldVersionIsOnAirDoesNotPinItAsUnsaved() // N2
{   // create, AddField("location","text",40), put an instance with ProgramTemplateVersion = 1 on the design, SaveAsync
    // (reuse the capacity fake of OpeningTwelveDesignsInSequenceNeverFillsTheLibrary)
    Assert.DoesNotContain((design.TemplateId, 1), _session!.UnsavedTemplateEdits);
    // instance leaves air; fill the library (fake refuses install with template-library-capacity at 16 definitions); open another design
    Assert.Contains(_commands, c => c.Action == "graphics.template.remove" && c.Body.GetProperty("templateVersion").GetInt32() == 1);
}
[Fact] public async Task UndoRightAfterSaveKeepsTheCarriedValues() // N4
{   // create, SetField("title","London"), EditTemplate(AddField location), workflow.SaveAsync()
    Assert.False(_session!.CanUndo);
    _session.Undo();
    Assert.Equal("London", _session.Draft!.Fields.GetProperty("title").GetString());
    Assert.Equal(2, _session.Draft.TemplateVersion);
}
[Fact] public async Task DeletingADesignRemovesItsPins() // N5 — rewrite so Delete's own prune runs
{   // write a schema-3 document straight through GraphicsDocumentStore: one design + a Templates pin for its v1, no rows/cues/Studio assignment
    Assert.Contains(library.Document.Templates, t => t.TemplateId == design.TemplateId); // present before Delete
    Assert.True((await service.DeleteAsync(design.DesignId)).Ok);
    Assert.DoesNotContain(library.Document.Templates, t => t.TemplateId == design.TemplateId);
}
// GraphicsPlayoutServiceTests
[Fact] public async Task PlaylistReinstallEvictsUnusedDesignVersionsWhenTheLibraryIsFull() // (c)
{   // fake: install fails with template-library-capacity while 16 definitions are installed; 10 unreferenced design versions installed;
    // a playlist row pins design "design-guest-a1b2c3" v1 (pin in Document.Templates, not installed)
    var result = await service.EnsureInstanceAsync(entry);
    Assert.True(result.Accepted, result.Reason);
    Assert.Contains(_commands, c => c.Action == "graphics.template.remove");
    Assert.DoesNotContain(_commands, c => c.Action == "graphics.template.remove" && c.Body.GetProperty("templateId").GetString() == "design-guest-a1b2c3");
}
```

- [ ] **Step 2: Run** `dotnet test native-shell\CoreVideoPro.MediaCore.Tests --filter "FullyQualifiedName~GraphicsDesignServiceTests|FullyQualifiedName~GraphicsPlayoutServiceTests"` — Expected: the N1, N2, N4 and (c) tests FAIL. N5 is a test-only fix: it passes, and its "present before Delete" assert proves it now reaches Delete's own prune.
- [ ] **Step 3: Implement**
  - N1: in `Observe` keep a `_browseEdits` entry whose template version is no longer installed when `GraphicsDesignIds.IsDesignTemplateId(key.Item1)` and `Draft.Scratch is null` (value-only memory never pins a slot); epoch change still clears all.
  - N2: `SaveAsync` calls `_session.ForgetBrowseEdits(design.TemplateId, savedVersion)` after a stored save, before `Carry`.
  - N4: `Carry` calls `_session.Rebase(...)` instead of `_session.Edit(...)`.
  - (c): move `EvictUnusedDesigns` + `Remove` into `GraphicsDesignEviction`; both install paths pass it.
- [ ] **Step 4: Run** the filter, then the full `dotnet test native-shell\CoreVideoPro.MediaCore.Tests` — PASS.
- [ ] **Step 5: Commit** `Graphics designs: keep typed values on switch, forget superseded drafts, undo after save, one evictor (#1087)`.

### Task 2: Layer structure edits (rename, hide, reorder, group/ungroup, add) + native fixture

**Files:**
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsTemplateEditor.cs` (make the class `public static partial`; `Set` whitelist :55-56 and `LayerAllows` :143-148 gain image `verticalAlign`; `Duplicate` :119-127 gains an `out string copyId` overload and labels the copy)
- Create: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsTemplateEditor.Structure.cs`
- Create: `native/tests/fixtures/graphics-editor/designer-structure.json`
- Modify: `native/tests/GraphicsEditorShapesTest.cpp` (fixture list), `native/tests/GraphicsTemplateTest.cpp` (one test)
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsTemplateEditorStructureTests.cs`

**Native decision (no native change):** native already accepts groups (`children`, depth ≤ 8), `visible` (literal or boolean-field), and any JSON under node `metadata`; the registry publishes the exact document (`definition`), so labels and locks round-trip. Labels and locks are shell-only metadata; hide is native `visible:false`. The fixture pins it.

**Interfaces** (all on `GraphicsTemplateEditor`; throw `InvalidDataException` for bad input, `InvalidOperationException` for refused edits; every edit runs `KeepRequiredFieldsUsed`):
- `public static NativeGraphicsScratch SetLabel(NativeGraphicsScratch s, string id, string label)` — allowed on locked layers; message "Use a layer name of 1–64 characters."
- `public static NativeGraphicsScratch SetVisible(NativeGraphicsScratch s, string id, bool visible)` — allowed on locked layers; refused when `visible` is field-bound: `$"This layer's visibility follows the variable {fieldId}."`
- `public static NativeGraphicsScratch MoveTo(NativeGraphicsScratch s, string id, string parent, int frontIndex, (double X, double Y)? absolute = null)` — `parent` "" = root; `frontIndex` 0 = frontmost among the destination's siblings (document order is back-to-front), clamped. On a parent change, keeps the canvas position: destination origin = Σ(`x` + `padding`) of its non-flow ancestor chain; new x/y = (`absolute` ?? source chain origin + own x/y) − destination origin; entering a flow group sets x = y = 0. Then grows a non-flow destination group to contain its children (algorithm below). Refuses locked layer/destination and cycles ("A group cannot be moved inside itself.").
- `public static (NativeGraphicsScratch Scratch, string GroupId) Group(NativeGraphicsScratch s, string id)` — wraps the layer in a new `flow:"none"` group at the layer's x/y/width/height (layer → x = y = 0), id `group-<n>`, label "Group"; refuses beyond 64 layers / depth 8 with the existing messages.
- `public static NativeGraphicsScratch Ungroup(NativeGraphicsScratch s, string groupId)` — children replace the group at its index, keeping their order; each child x += group x + padding, y likewise; for a `vertical` flow group child y = group y + padding + Σ(earlier heights + gap) (all children counted), `horizontal` analogous on x; literal child `opacity` × group opacity (bound values unchanged); a hidden group hides its children. Refuses a locked group.
- `public static (NativeGraphicsScratch Scratch, string Id) AddLayer(NativeGraphicsScratch s, string kind)` — kinds `text`, `shape`, `clock`; appended frontmost at root; ids `<prefix>-<n>` (smallest free n ≥ 1; prefixes `text`, `shape`, `clock`); labels "Text", "Shape", "Clock". Text: `text` "Text", `font` {token brand.fontBody}, `fontSize` 42, `fontWeight` 600, `color` {token brand.onPrimary}, x 40, y 40, width 600, height 72. Shape: `rect`, `fill` {token brand.accent}, `cornerRadius` 0, x 0, y 0, width 400, height 120. Clock: text bound to field `time` (added when missing: `string`, `maxLength` 32, `default` "12:00:00"), width 320, height 72; only for family `clock`/`countdown`, else "Clocks tick in clock and countdown designs. Start one from the Clock look." Tokens used are added to `brandTokens`.
- `public static (NativeGraphicsScratch Scratch, string Id) AddImage(NativeGraphicsScratch s, string source)` — `source` `field:<imageFieldId>` | `asset:<declaredAssetId>` | `new-variable` (adds optional `image` field `image`, `image-2`, …); layer id `image-<n>`, label "Image", width 160, height 160, `fit` contain, `align` center, `verticalAlign` center, `collapseWhenEmpty` true when field-bound. Unknown asset: "Choose an image asset this design declares."
- `public static NativeGraphicsScratch Duplicate(NativeGraphicsScratch s, string id, out string copyId)` — copy label = original label + " copy".

Group growth (non-flow destination after `MoveTo`): `minX = min(0, min child x)`, `minY = min(0, min child y)`, `maxX = max(group.width, max child x+width)`, `maxY` likewise; group x += minX, y += minY, width = maxX − minX, height = maxY − minY; every child x −= minX, y −= minY. Child x/y are computed as signed doubles before this step, so a negative intermediate never reaches the document.

- [ ] **Step 1: Write the failing tests** (seed = `GraphicsDesignSeeds.Manifest("lower-third", "design-test-a1b2c3", 1, "Test")`: plate (0,0,1060,168), accent (0,0,8,168), name-label (32,27,996,62) bound to required `name`, title-label (32,94,996,42) bound to `title`)

```csharp
[Fact] public void GroupThenUngroupKeepsCanvasPositions()
{   var (grouped, g) = GraphicsTemplateEditor.Group(Seed, "title-label");
    Assert.Equal((32d, 94d, 996d, 42d), Box(grouped, g)); Assert.Equal((0d, 0d), Pos(grouped, "title-label"));
    var back = GraphicsTemplateEditor.Ungroup(grouped, g);
    Assert.Equal((32d, 94d), Pos(back, "title-label"));
    Assert.Equal(new[] { "plate", "accent", "name-label", "title-label" }, GraphicsTemplateEditor.Layers(back).Select(l => l.Id)); }
[Fact] public void MoveIntoAGroupKeepsCanvasPositionAndGrowsTheGroup()
{   var (grouped, g) = GraphicsTemplateEditor.Group(Seed, "title-label");
    var moved = GraphicsTemplateEditor.MoveTo(grouped, "name-label", g, 0);
    Assert.Equal((32d, 27d, 996d, 109d), Box(moved, g));
    Assert.Equal((0d, 0d), Pos(moved, "name-label")); Assert.Equal((0d, 67d), Pos(moved, "title-label"));
    Assert.Equal("name-label", ChildIds(moved, g).Last()); } // frontIndex 0 = last in document order
[Fact] public void UngroupingAFlowGroupUsesFlowPositions() // Corporate = the corporate-lower-third manifest (GraphicsTemplate.cpp:352-362) copied into the test; text-stack (24,20) vertical gap 8, children h 62 / 42
{   var flat = GraphicsTemplateEditor.Ungroup(Corporate, "text-stack");
    Assert.Equal((24d, 20d), Pos(flat, "name-text")); Assert.Equal((24d, 90d), Pos(flat, "title-text")); }
[Fact] public void UngroupMultipliesOpacity() // group opacity 0.5, child 0.8 -> 0.4
[Fact] public void HideAndShowWriteNativeVisibility()
{   var hidden = GraphicsTemplateEditor.SetVisible(Seed, "name-label", false);
    Assert.False(GraphicsTemplateEditor.Layer(hidden, "name-label").GetProperty("visible").GetBoolean());
    Assert.False(GraphicsTemplateEditor.Layers(hidden).Single(l => l.Id == "name-label").Visible);
    Assert.False(GraphicsTemplateEditor.Layer(GraphicsTemplateEditor.SetVisible(hidden, "name-label", true), "name-label").TryGetProperty("visible", out _)); }
[Fact] public void HidingAVariableBoundLayerIsRefused() // title-label visible = {"field":"show-title"} (boolean field added to the fixture)
{   var ex = Assert.Throws<InvalidOperationException>(() => GraphicsTemplateEditor.SetVisible(Bound, "title-label", false));
    Assert.Equal("This layer's visibility follows the variable show-title.", ex.Message); }
[Fact] public void RenameWritesMetadataLabelEvenWhenLocked()
{   var locked = GraphicsTemplateEditor.Lock(Seed, "plate", true);
    Assert.Equal("Plate shape", GraphicsTemplateEditor.Layers(GraphicsTemplateEditor.SetLabel(locked, "plate", "  Plate shape ")).Single(l => l.Id == "plate").Label);
    Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetLabel(Seed, "plate", new string('a', 65)));
    Assert.Throws<InvalidDataException>(() => GraphicsTemplateEditor.SetLabel(Seed, "plate", "  ")); }
[Fact] public void DeletingAGroupThatHoldsTheRequiredNameIsRefused()
{   var (grouped, g) = GraphicsTemplateEditor.Group(Seed, "name-label");
    var ex = Assert.Throws<InvalidOperationException>(() => GraphicsTemplateEditor.Remove(grouped, g));
    Assert.Equal("Required variable name must stay on a layer.", ex.Message); }
[Fact] public void GroupingRespectsDepthAndLayerLimits() // Group the same layer 8 times: succeeds (layer depth 8), the 9th throws "Template group nesting exceeds eight levels."; at 64 layers Group throws "A template supports at most 64 layers."
[Fact] public void LockedLayersCannotBeMovedGroupedOrDeleted() // MoveTo / Group / Remove on locked plate throw InvalidOperationException
[Fact] public void AddLayerUsesReadableIdsLabelsAndBrandTokens()
{   var (one, t1) = GraphicsTemplateEditor.AddLayer(Seed, "text"); var (two, t2) = GraphicsTemplateEditor.AddLayer(one, "text");
    Assert.Equal(("text-1", "text-2"), (t1, t2)); Assert.Equal("text-2", GraphicsTemplateEditor.Layers(two).Last().Id);
    var (shaped, s1) = GraphicsTemplateEditor.AddLayer(Seed, "shape");
    Assert.Equal("shape-1", s1); Assert.Equal("brand.accent", GraphicsTemplateEditor.Layer(shaped, s1).GetProperty("fill").GetProperty("token").GetString()); }
[Fact] public void AddClockNeedsAClockDesign() // lower-third -> exact message above; family "clock" -> field time (string, 32, "12:00:00") + "clock-1" bound to it
[Fact] public void AddImageOffersOnlyExistingSlots() // "new-variable" -> field "image" (type image) + "image-1" asset {"field":"image"}, collapseWhenEmpty true; "asset:missing" -> InvalidDataException "Choose an image asset this design declares."
[Fact] public void DuplicateLabelsTheCopy() // Duplicate(Seed,"plate",out id) -> label "plate copy", id != "plate"
[Fact] public void ReorderWithinSiblings() // MoveTo(Seed,"plate","",0) -> document order accent,name-label,title-label,plate
```

```cpp
// GraphicsTemplateTest.cpp
TEST(GraphicsTemplate, HiddenGroupDrawsNoChildrenAndKeepsEditorMetadata) {
  // load designer-structure.json; resolve with {name:"Ada",title:"Host",image:"",show-title:true}
  // EXPECT no primitive with id "name-label" or "title-label" (their group "group-1" has "visible":false)
  // EXPECT the node entry for "group-1" has visible == false
  // EXPECT document().get("layers") group-1 metadata label == "Name block" and editorLocked == true
}
```
`designer-structure.json`: the lower-third seed (id `design-fixture-structure`) with `name-label` + `title-label` inside `group-1` (`flow:"none"`, `"visible": false`, `metadata {label:"Name block", editorLocked:true}`), `title-label` `visible: {"field":"show-title"}` (boolean field `show-title`), and `image-1` bound to optional image field `image` with `collapseWhenEmpty: true`; also added to the `EditorOutputShapesLoadNatively` list.

- [ ] **Step 2: Run** `dotnet test native-shell\CoreVideoPro.MediaCore.Tests --filter FullyQualifiedName~GraphicsTemplateEditorStructureTests` — FAIL (methods missing). `npm run build:native-dev`, then from `native\build-dev` with `$env:COREVIDEO_VCAM_SHM_DIR` set: `.\corevideo-native-tests.exe --gtest_filter=GraphicsTemplate.HiddenGroupDrawsNoChildrenAndKeepsEditorMetadata` and `--gtest_filter=GraphicsEditorShapes.EditorOutputShapesLoadNatively` — FAIL (fixture missing).
- [ ] **Step 3: Implement** the operations in `GraphicsTemplateEditor.Structure.cs` reusing `Walk`, `KeepRequiredFieldsUsed`, `Save`; add the fixture.
- [ ] **Step 4: Run** both commands + full MediaCore suite + `--gtest_filter=GraphicsTemplate.*` — PASS (native expected to pass with no native code change; if it does not, stop and report — that changes this plan's native decision).
- [ ] **Step 5: Commit** `Graphics designer: layer rename, hide, reorder, group/ungroup and add (document edits)`.

### Task 3: Designer model — layer rows, drop targets, inspector

**Files:**
- Create: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsDesignerModel.cs`
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsDesignerModelTests.cs`

**Interfaces:**
- Consumes: Task 2 operations; `GraphicsTemplateEditor.BindLayer`, `Set`, `Layers`, `Layer`; `GraphicsBuilder.TypefaceChoices`.
- Produces:
  - `public sealed record GraphicsDesignerRow(string Id, string Kind, string Label, string Detail, int Depth, string Parent, bool Locked, bool Visible, bool VisibilityBound)` — `Kind` "Text" | "Shape" | "Image" | "Clock" (text bound to `time`) | "Group"; `Detail` `{field}` for a bound text/image, the fixed text cut to 24 chars, "shape", "image", or `group · {n} layers`.
  - `public static class GraphicsDesignerModel`:
    - `IReadOnlyList<GraphicsDesignerRow> Rows(NativeGraphicsScratch s)` — front-to-back depth-first (a group, then its children front-to-back).
    - `(string Parent, int FrontIndex)? DropTarget(IReadOnlyList<GraphicsDesignerRow> rows, string draggedId, int insertionRow, bool ontoGroup)` — `ontoGroup` on a group row → (group id, 0); otherwise insert before `rows[insertionRow]` → (its parent, its front index among siblings, −1 when the dragged layer was earlier among the same siblings); `insertionRow == rows.Count` → ("", root count − 1 when dragged is at root else root count). Null for a locked dragged layer, a locked destination group, or a destination inside the dragged group.
    - `GraphicsLayerInspection Inspect(NativeGraphicsScratch s, JsonElement draftFields, string id)`
    - `NativeGraphicsScratch SetColour(NativeGraphicsScratch s, string id, string property, string choice)` — `property` `color` | `fill`; token from the Global Constraints list → `{"token":…}`; hex → string; else "Use a brand colour or a colour like #RRGGBB."
    - `NativeGraphicsScratch SetTypeface(NativeGraphicsScratch s, string id, string choice)` — brand font token → `{"token":…}`; a `TypefaceChoices` family or the layer's current custom family → string; else "Choose a brand font or one of the listed typefaces."
    - `NativeGraphicsScratch SetShows(NativeGraphicsScratch s, string id, string? variable, string fixedText)` — wraps `BindLayer(s, id, "text", …)`.
  - `public sealed record GraphicsLayerInspection(string Id, string Kind, string Label, bool Locked, string? ShowsVariable, string FixedText, string FontChoice, double FontSize, int FontWeight, string ColourChoice, int MaxLines, string Overflow, string Align, double X, double Y, double Width, double Height, double Opacity, string FillChoice, bool FillGradient, double CornerRadius, string ImageSource, string Fit, string ImageAlign, string ImageVerticalAlign)` — field-bound numbers resolve to the draft value; `FontChoice`/`ColourChoice`/`FillChoice` hold the token id or the literal; `ImageSource` `field:<id>` | `asset:<id>` | `token:brand.logo`.
  - `public static class GraphicsDesignerChoices { Colours, Fonts, Weights ((400,"Regular"),(500,"Medium"),(600,"Semibold"),(700,"Bold")), Lines (1,2,3), Overflows (("ellipsis","Ellipsis"),("wrap-then-ellipsis","Wrap, then ellipsis")), Fits, Aligns, VerticalAligns }` as `IReadOnlyList<(string Id, string Label)>` (Lines as int).

- [ ] **Step 1: Write the failing tests**

```csharp
[Fact] public void RowsListFrontToBackWithKindsAndDetails()
{   var rows = GraphicsDesignerModel.Rows(Seed);
    Assert.Equal(new[] { "title-label", "name-label", "accent", "plate" }, rows.Select(r => r.Id));
    Assert.Equal(new[] { "{title}", "{name}", "shape", "shape" }, rows.Select(r => r.Detail));
    Assert.Equal(new[] { "Text", "Text", "Shape", "Shape" }, rows.Select(r => r.Kind)); }
[Fact] public void GroupRowsNestTheirChildren() // after Group(title-label): rows [group-1 (Depth 0, "group · 1 layers"), title-label (Depth 1, Parent "group-1"), name-label, accent, plate]
[Fact] public void DropTargetMapsListPositionsToMoveTo()
{   var rows = GraphicsDesignerModel.Rows(Seed);
    Assert.Equal(("", 0), GraphicsDesignerModel.DropTarget(rows, "plate", 0, false));
    Assert.Equal(("", 3), GraphicsDesignerModel.DropTarget(rows, "title-label", 4, false)); }
[Fact] public void DropTargetRefusesCyclesAndLockedLayers()
{   // Group(title-label) -> group-1; dropping group-1 onto its own child row -> null; Lock plate -> dragging plate -> null; dropping accent onto group-1 with ontoGroup -> ("group-1", 0)
}
[Fact] public void InspectorResolvesBoundValuesAndTokens()
{   var i = GraphicsDesignerModel.Inspect(Seed, Fields(name: "Darnell Reyes", title: "Guest"), "name-label");
    Assert.Equal(("name", "brand.fontBody", 42d, 600, "brand.onPrimary"), (i.ShowsVariable, i.FontChoice, i.FontSize, i.FontWeight, i.ColourChoice));
    Assert.Equal((32d, 27d, 996d, 62d, 1d), (i.X, i.Y, i.Width, i.Height, i.Opacity));
    Assert.Equal((1, "ellipsis", "left"), (i.MaxLines, i.Overflow, i.Align)); }
[Fact] public void SetColourAcceptsBrandTokensAndHexOnly()
{   Assert.Equal("brand.accent", Layer(GraphicsDesignerModel.SetColour(Seed, "plate", "fill", "brand.accent"), "plate").GetProperty("fill").GetProperty("token").GetString());
    Assert.Equal("#FF8800", Layer(GraphicsDesignerModel.SetColour(Seed, "plate", "fill", "#FF8800"), "plate").GetProperty("fill").GetString());
    Assert.Equal("Use a brand colour or a colour like #RRGGBB.", Assert.Throws<InvalidDataException>(() => GraphicsDesignerModel.SetColour(Seed, "plate", "fill", "orange")).Message); }
[Fact] public void SetTypefaceAcceptsBrandFontsAndListedFamilies() // "Bahnschrift" -> "Bahnschrift"; "brand.fontHeading" -> token; "Comic Sans MS" -> InvalidDataException "Choose a brand font or one of the listed typefaces."
[Fact] public void SetShowsSwitchesBetweenVariableAndFixedText() // SetShows(Seed, "title-label", null, "Chicago") -> text "Chicago"; then SetShows(.., "title-label", "title", "") -> {"field":"title"}
```

- [ ] **Step 2: Run** `dotnet test native-shell\CoreVideoPro.MediaCore.Tests --filter FullyQualifiedName~GraphicsDesignerModelTests` — FAIL (type missing).
- [ ] **Step 3: Implement** `GraphicsDesignerModel.cs` (records, choices, the static methods).
- [ ] **Step 4: Run** filter + full MediaCore suite — PASS.
- [ ] **Step 5: Commit** `Graphics designer: layer rows, drop targets and inspector model`.

### Task 4: Snap guides and sample text

**Files:**
- Create: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsSnap.cs`
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsPreviewSamples.cs:11-53` (fields from the scratch; `longest-name`)
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsWorkspaceSession.cs:75` (`SetPreviewSample` accepts `longest-name`)
- Modify: `native-shell/CoreVideoPro.WinUI/Controls/GraphicsAuthoringCanvas.cs` (fields :20-35; `Moved` :69-87; `Released`/`Cancel` :88-89)
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsSnapTests.cs`, `GraphicsPreviewSamplesTests.cs` (new)

**Interfaces:**
- Produces: `public readonly record struct GraphicsRect(double X, double Y, double Width, double Height)`; `public sealed record GraphicsGuide(char Axis, double Position, string Kind)` (`Kind` "safe" | "centre"); `public static class GraphicsSnap { public const double Threshold = 8; public static (double Dx, double Dy, IReadOnlyList<GraphicsGuide> Guides) Move(GraphicsRect proposed, double threshold = Threshold); public static (double Dw, double Dh, IReadOnlyList<GraphicsGuide> Guides) Resize(GraphicsRect proposed, double threshold = Threshold); }` — Move: per axis the candidate with the smallest |delta| ≤ threshold among left→96 / right→1824 / centre→960 (y: top→54 / bottom→1026 / centre→540); ties prefer the safe edge. Resize snaps only the right edge (1824, 960) and bottom edge (1026, 540).
- Produces: `GraphicsPreviewSamples.LongestName = "Maximiliana Alexandra Montgomery-Wellington Fitzgerald"`; sample `longest-name` sets the first text-content variable to it (UTF-8-limited to that field's `maxLength`, default 512) and leaves the others as typed.
- Produces on the canvas: `public bool SnapToGuides { get; set; }` (default false; the Designer sets true). While dragging with it on and Ctrl up, the move/resize delta is adjusted by `GraphicsSnap` in canvas space (divide by placement scale before `SetLayerNumbers`) and the active guides are drawn (cleared on release/cancel).

- [ ] **Step 1: Write the failing tests**

```csharp
[Fact] public void MoveSnapsToTheNearestSafeEdgeOrCentre()
{   Assert.Equal((-4d, 0d), Delta(GraphicsSnap.Move(new(100, 300, 400, 100))));
    Assert.Equal(new GraphicsGuide('x', 96, "safe"), GraphicsSnap.Move(new(100, 300, 400, 100)).Guides.Single());
    Assert.Equal((4d, 0d), Delta(GraphicsSnap.Move(new(1420, 300, 400, 100))));         // right 1820 -> 1824
    var centred = GraphicsSnap.Move(new(757, 488, 410, 100));                          // centre 962,538
    Assert.Equal((-2d, 2d), Delta(centred)); Assert.All(centred.Guides, g => Assert.Equal("centre", g.Kind));
    Assert.Empty(GraphicsSnap.Move(new(105, 300, 400, 100)).Guides); }                 // 9 px away: no snap
[Fact] public void ResizeSnapsOnlyRightAndBottomEdges() // (100,100,1720,920): right 1820 -> Dw 4, bottom 1020 -> Dh 6
[Fact] public void LongestNameFillsOnlyTheFirstTextVariable()
{   var v = GraphicsPreviewSamples.Apply(SeedDraft(name: "Ada", title: "Host"), SeedTemplate, "longest-name");
    Assert.Equal(GraphicsPreviewSamples.LongestName, v.GetProperty("name").GetString()); Assert.Equal("Host", v.GetProperty("title").GetString()); }
[Fact] public void LongestNameRespectsMaxLength() // name maxLength 10 -> "Maximilian"
[Fact] public void SamplesCoverVariablesAddedInTheDraft()
{   // draft.Scratch = seed + AddField("location","text",40) + AddLayer text bound to location; template (installed) lacks location
    var v = GraphicsPreviewSamples.Apply(draftWithLocation, SeedTemplate, "blank");
    Assert.Equal("", v.GetProperty("location").GetString()); Assert.Equal("", v.GetProperty("title").GetString()); }
// GraphicsWorkspaceSessionTests
[Fact] public void SessionAcceptsTheLongestNameSample() // SetPreviewSample("longest-name") -> PreviewSample == "longest-name"; SetPreviewSample("nope") keeps the previous value
```

- [ ] **Step 2: Run** `dotnet test native-shell\CoreVideoPro.MediaCore.Tests --filter "FullyQualifiedName~GraphicsSnapTests|FullyQualifiedName~GraphicsPreviewSamplesTests|FullyQualifiedName~GraphicsWorkspaceSessionTests"` — FAIL.
- [ ] **Step 3: Implement** `GraphicsSnap`; in `Apply` read fields from `GraphicsTemplateEditor.TemplateFields(draft.Scratch)` when the draft has a scratch (else the template's); add `longest-name`; session whitelist; canvas `SnapToGuides` + guide lines (two `Line`s reused, accent `#44C1A1`, solid, not hit-testable).
- [ ] **Step 4: Run** the filter + full MediaCore suite; `dotnet build native-shell\CoreVideoPro.WinUI -c Release -p:Platform=x64` — PASS / builds.
- [ ] **Step 5: Commit** `Graphics designer: safe-area and centre snapping, Longest name sample, samples follow draft variables`.

### Task 5: The Designer surface (WinUI) and its layout probe

**Files:**
- Create: `native-shell/CoreVideoPro.WinUI/Controls/GraphicsDesignerControl.cs` (+ `.Layers.cs`, `.Inspector.cs` partials)
- Create: `native-shell/CoreVideoPro.WinUI/Services/GraphicsDesignSurfaces.cs`
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.GraphicsMode.cs:30-58` (`GraphicsDesignerOpen`, `IsGraphicsDesignerVisible`, builder/advanced visibility via `GraphicsDesignSurfaces`)
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.GraphicsDesigns.cs:22-27` (Opened → `GraphicsDesignerOpen = true; GraphicsDesignAdvanced = false`); add `public string GraphicsDesignPlaylistHotkey(GraphicsDesign design)`
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsDesignWorkflow.cs` (add `PlaylistHotkey`)
- Modify: `native-shell/CoreVideoPro.WinUI/Views/OverlaysPage.xaml:43-50` (host `GraphicsDesignerControl x:Name="GraphicsDesigner"`), `OverlaysPage.xaml.cs:43,49-68` (one preview owner among Builder / Designer / Workspace)
- Modify: `native-shell/CoreVideoPro.WinUI/Services/GraphicsWorkspaceLayoutProbe.cs` (new `CheckDesignerAsync`, called after the Slice 1 design checks)
- Test: `native-shell/CoreVideoPro.WinUI.Tests/GraphicsDesignSurfacesTests.cs`; `GraphicsDesignServiceTests.PlaylistHotkeyIsTheRowsKeyOrTheNextFree`

**Interfaces:**
- Consumes: Tasks 2–4; `StudioViewModel.SaveGraphicsDesignAsync`, `SaveGraphicsDesignToPlaylistAsync`, `GraphicsDesignFor`, `GraphicsDesignOpened`; `GraphicsVariablesPanel`; `GraphicsAuthoringCanvas`.
- Produces: `public enum GraphicsDesignSurface { Playout, Builder, Designer, Workspace }`; `public static class GraphicsDesignSurfaces { public static GraphicsDesignSurface For(string mode, bool designerOpen, bool workspaceOpen); }` (playout → Playout; design + designerOpen → Designer; design + workspaceOpen → Workspace; else Builder).
- Produces: `public static string GraphicsDesignWorkflow.PlaylistHotkey(GraphicsDesign design, IReadOnlyList<GraphicsPlaylistRow> rows)` — the design's row hotkey, else `GraphicsBuilder.NextFreeHotkey(rows)`.
- Produces: `GraphicsDesignerControl : UserControl` with `Studio` DP, `SuspendPreview()` / `ResumePreview()` (same lease pattern as `GraphicsWorkspaceControl.UpdatePreviewLease`, own `ColorGradeEditorViewModel`).
- Layout (mockup D1): header = caption `{FAMILY} · DRAFT` + design name, "← Your designs" (sets `GraphicsDesignerOpen = false`), **Undo** (enabled = `CanUndo`), **Save** (AutomationName "Save design"), **Save to playlist · {F-key}** (AutomationName "Save to playlist"; plain "Save to playlist" when no key is free), status (AutomationName "Design status"). Columns 240 | * | 300 (220 | * | 280 below 1500 px). Left "LAYERS": rows from `GraphicsDesignerModel.Rows` (eye toggle "Hide {label}"/"Show {label}", disabled with tooltip when `VisibilityBound`; lock toggle "Lock {label}"; double-tap → inline rename, Enter commits, Esc cancels; drag/drop → `DropTarget` → `MoveTo` passing the native node's X/Y as `absolute`); toolbar Duplicate / Delete / Group / Ungroup; add buttons Text / Shape / Image… (flyout of `AddImage` sources) / Clock (disabled with the Clock message as tooltip unless family clock/countdown), AutomationNames "Add text layer", "Add shape layer", "Add image layer", "Add clock layer"; hint "Top of the list is in front. Drag to reorder, double-click to rename." Centre: canvas (`SnapToGuides = true`), then "Sample text:" Typical / Longest name / Empty title, caption "Safe area · snap to guides on · 1920 × 1080 · hold Ctrl to place freely". Right: header `{LABEL} · {KIND} LAYER`, tabs Layer / Variables (Variables scroll AutomationName "Design variables"); Layer tab per kind — Text: Shows, Typeface, Size, Weight, Colour, Lines, Overflow, Align; Shape: Fill, Corner radius; Image: Source, Fit, Align, Vertical align; all: X, Y, W, H, Opacity; locked → all inputs disabled except rename/lock. Every write goes through `_session.EditTemplate` / `SetLayerNumbers`; slider drags wrap `BeginGesture`/`EndGesture`; editing is paused while a save runs (Slice 1 rule). Errors land in the status line.

- [ ] **Step 1: Write the failing tests**

```csharp
// WinUI.Tests
[Theory]
[InlineData("playout", true, true, GraphicsDesignSurface.Playout)]
[InlineData("design", true, true, GraphicsDesignSurface.Designer)]
[InlineData("design", false, true, GraphicsDesignSurface.Workspace)]
[InlineData("design", false, false, GraphicsDesignSurface.Builder)]
public void OneDesignSurfaceAtATime(string mode, bool designer, bool workspace, GraphicsDesignSurface expected) =>
    Assert.Equal(expected, GraphicsDesignSurfaces.For(mode, designer, workspace));
// MediaCore
[Fact] public void PlaylistHotkeyIsTheRowsKeyOrTheNextFree() // row on F3 for the design -> "F3"; no row, F1..F5 taken -> "F6"; all twelve taken -> ""
```
Probe (`CheckDesignerAsync`, run at page 1320×640 then 1560×772; open the stored "Probe strap" design draft via `studio.GraphicsWorkspace.Browse(seedTemplate)` + `studio.GraphicsDesignerOpen = true`):
  - `GraphicsDesignerControl` visible; Builder and workspace collapsed.
  - Layer rows in order title-label, name-label, accent, plate with details "{title}", "{name}".
  - Each of "Add text layer", "Add shape layer", "Add image layer", "Add clock layer", "Undo", "Save design", "Save to playlist", "Typical", "Longest name", "Empty title" has `ActualHeight > 0` and lies inside the designer (x + w ≤ ActualWidth + 1, y + h ≤ ActualHeight + 1).
  - Designer `ActualWidth` ≤ page width (no horizontal overflow); canvas ≥ 640×360 at 1320×640 and ≥ 800×450 at 1560×772; the Layer-tab scroll viewport ≥ 300 px at 1320×640.
  - Variables tab: the panel lists `name` and `title` and "Used by: name-label".
  - Invoke "Add text layer" → 5 rows, `SelectedLayerId == "text-1"`; Invoke "Undo" → 4 rows.
  - Screenshots `graphics-designer-1360x768.png`, `graphics-designer-1600x900.png`.

- [ ] **Step 2: Run** `dotnet test native-shell\CoreVideoPro.WinUI.Tests -c Release -p:Platform=x64 -p:WindowsAppSDKBootstrapAutoInitializeOptions_Default=false -p:WindowsAppSDKBootstrapAutoInitializeOptions_None=true --filter FullyQualifiedName~GraphicsDesignSurfacesTests` and the MediaCore filter `FullyQualifiedName~PlaylistHotkeyIsTheRowsKeyOrTheNextFree` — FAIL. Build `dotnet build native-shell\CoreVideoPro.WinUI -c Release -p:Platform=x64`, then `powershell -File scripts\test-studio-layout.ps1 -ReportDirectory D:\CoreVideo-QA\tmp\gfx-slice2-probe-t5a` — graphics FAILS on the first designer check.
- [ ] **Step 3: Implement** the control, surfaces helper, VM properties, page hosting and preview hand-off, probe checks.
- [ ] **Step 4: Run** both test commands — PASS; rebuild; probe with a new `-ReportDirectory …-t5b` — all three kinds pass; look at both screenshots.
- [ ] **Step 5: Commit** `Graphics designer: the Designer surface (layers, canvas, inspector, header)`.

### Task 6: Retire the Advanced editor's editing pane; "Open in Designer"; docs

**Decision:** the Designer replaces the Advanced editor *as the editor*. The old workspace keeps the tools that have no other home and that probes, the control API and scripts rely on — show templates, saved cues and styles, package import/export, the Automation drawer, existing overlay controls, Cue/Show/Hide of private cues — under the Builder header button **"Cues & automation"**. Its layer-editing pane and design actions are removed. Control actions are untouched (they run on `GraphicsWorkspaceSession` / `GraphicsWorkspaceLibrary`, which stay).

**Files:**
- Delete: `native-shell/CoreVideoPro.WinUI/Controls/GraphicsTemplateLayerEditor.cs`
- Modify: `native-shell/CoreVideoPro.WinUI/Controls/GraphicsWorkspaceControl.cs` (field :35, Attach/Detach :73-91), `GraphicsWorkspaceControl.Layout.cs` (layer list fields :31-33 + :40, "Edit layers ↗" :101, layer panel :103-105, `_toolPanels["layers"]` :170, `SetGraphicsCustomization` :196-197, `RefreshLayerList` :231-242), `GraphicsWorkspaceControl.Designs.cs` (remove Save design / Save to playlist / Variables tab / `OnDesignOpened`; keep the Studio-look rows behaviour)
- Modify: `native-shell/CoreVideoPro.WinUI/Controls/GraphicsBuilderControl.cs:83-84` ("Advanced editor" → "Open in Designer" = open the selected design, else the New design dialog; plus "Cues & automation" → `GraphicsDesignAdvanced = true`) and `:352` (note: "Import a package in Cues & automation.")
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsBuilder.cs:40-46` (N3: `TemplatesFor` leaves out design template ids)
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsDesignWorkflow.cs` (add `DesignForRow`), `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.GraphicsMode.cs:54-58` (`OpenGraphicInDesign`: a design row opens the Designer)
- Modify: `native-shell/CoreVideoPro.WinUI/Services/GraphicsWorkspaceLayoutProbe.cs:40-67, 139-160` ("Advanced editor" → "Cues & automation"; drop the workspace Variables/Save-design checks, now in `CheckDesignerAsync`; assert the workspace has no "Edit layers ↗"), `StudioWorkspaceLayoutProbe.cs:136-138` (comment)
- Docs: `CLAUDE.md` (graphics-playout row), `docs/reference/graphics-playout.md` (new "## The Designer (Graphics Designer slice 2)" after :90-170 "Designs"; update "Build a graphic (Design)" :48-89 and "Where things live" :287-302), `docs/BACKLOG.md:428` (Slice 2 status)
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsBuilderTests.cs`, `GraphicsDesignServiceTests.cs`

**Interfaces:**
- Produces: `public GraphicsDesign? GraphicsDesignWorkflow.DesignForRow(GraphicsPlaylistRow row)` (by `row.TemplateId`).

- [ ] **Step 1: Write the failing tests**

```csharp
[Fact] public void TemplatesForLeavesDesignsToYourDesigns() // N3
{   var state = new NativeGraphicsState { Templates = [Template("corporate-lower-third", "lower-third"), Template("design-guest-a1b2c3", "lower-third")] };
    Assert.Equal(new[] { "corporate-lower-third" }, GraphicsBuilder.TemplatesFor(GraphicsBuilder.Kind("guest")!, state).Select(t => t.TemplateId)); }
[Fact] public async Task DesignForRowFindsTheRowsDesign() // row TemplateId = design.TemplateId -> design; built-in row -> null
```
Probe: "Cues & automation" reveals the workspace; the workspace contains no `Button` "Edit layers ↗" and no "Save design"; "Open in Designer" with "Probe strap" selected shows `GraphicsDesignerControl`.

- [ ] **Step 2: Run** `dotnet test native-shell\CoreVideoPro.MediaCore.Tests --filter "FullyQualifiedName~GraphicsBuilderTests|FullyQualifiedName~DesignForRowFindsTheRowsDesign"` — FAIL; probe (new `-ReportDirectory …-t6a`) — FAILS on "Cues & automation".
- [ ] **Step 3: Implement** the removals, the two Builder entries, N3, `DesignForRow` + `OpenGraphicInDesign`, probe edits; `rg -n "Edit layers|Template layer editor|Advanced editor" native-shell scripts docs CLAUDE.md` finds nothing outside the spec/plan history.
- [ ] **Step 4: Run** full `dotnet test native-shell\CoreVideoPro.MediaCore.Tests`, `dotnet test native-shell\CoreVideoPro.Control.Tests`, the WinUI.Tests command from Task 5, then the probe (`…-t6b`) — PASS (known pre-existing `AdapterConformanceTests` golden excepted). Write the docs: the Designer section (layers panel ops, inspector fields, snapping + Ctrl, sample buttons, what is Slice 3/4), the Builder's two entries, "Cues & automation", the hide = on-air rule, and that #1087 N1/N2/N3/N4/N5/(c) are fixed ((b) → Slice 4).
- [ ] **Step 5: Commit** `Graphics: the Designer replaces the Advanced editor's layer pane; Open in Designer; docs`.

---

## Acceptance hand-off (owner, installed build — spec Acceptance 1)

Design → New design → Lower third, blank → **Open in Designer**; Variables tab: add `location` (Text, 40); Add text layer → Shows: Variable `location`; **Save to playlist · F6**; Operate: type a location on the row, Show — under 2 minutes, no Advanced pane.
