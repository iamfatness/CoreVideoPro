# Graphics Designer Slice 1 — Designs Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An author can start a graphic blank (or from a look), add variables and bind layers to them, save it as a design that lives in the show file and survives restarts, duplicate or delete it, and put it in the playlist — without code.

**Architecture:** A design is an authored template owned by the show: the shell stores its definition in `GraphicsShowDocument.Designs` (schema 3) under its own template id (`design-…`), saves each edit as a new version under that id, and installs designs into the core on demand. The native registry gains a `graphics-template-remove` command so superseded or deleted design versions free their slots. Blank designs start from shared seed manifests that both the shell and a native test load. Variables are template fields edited through `GraphicsTemplateEditor`. The Designer surface itself is Slice 2; this slice reaches designs through the existing Design workspace.

**Tech Stack:** C# / .NET 9, WinUI 3, xUnit; C++17 native core with the repo's gtest shim; JSON seed manifests under `contracts/graphics-seeds/`.

**Spec:** `docs/superpowers/specs/2026-11-01-graphics-designer-phase2-design.md` (§1, §2, §4, Slice 1)

## Global Constraints

- Design template ids: `design-<slug>-<6 lowercase hex>`, total ≤ 64 chars, `[a-z0-9-]`, first char a letter; never equal to a built-in id: the 6 curated templates in the registry (`corporate-lower-third`, `logo-accent`, `transparent-name`, `centered-title`, `corner-logo`, `clock-panel`) or the 3 compatibility looks outside the registry (`compact-solid`, `minimal-accent`, `broadcast`).
- `GraphicsShowDocument` schema **2 → 3**, additive: `Designs` defaults to empty; a v2 document loads with no designs; at most **32** designs; design ids unique; names 1–80 chars.
- Native registry limits are unchanged (16 definitions incl. the 6 curated templates — the 3 compatibility looks are outside the registry — and 256 KiB). New command `graphics-template-remove {templateId, templateVersion}`: refuses built-ins (`template-built-in`), versions referenced by any instance (`template-in-use`), unknown versions (`template-not-found`); accepted removal frees the slot and its bytes.
- Shell action id `graphics.template.remove` (params `templateId` string, `templateVersion` int), admitted by `GraphicsCommandGateway` and registered in `ControlActionRegistry` + `StudioControlSurface.SupportedActionIds`.
- Field limits stay native: ≤ 32 fields, field id `[a-z][a-z0-9-]{0,63}`, kinds `string` (maxLength), `number`, `color`, `boolean`, `image`.
- Seeds: exactly four — `lower-third`, `title`, `bug`, `full-screen` (family `lower-third`, `title`, `bug`, `title` respectively; canvas 1920×1080).
- No input injection; never launch/kill/attach to the app from tests; native tests from `native/build-dev` with `COREVIDEO_VCAM_SHM_DIR` set and an exact `--gtest_filter=Suite.Name`.
- Commit trailer: `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Review Focus

1. **Save a design while a playlist row shows its previous version on air** — Program keeps the on-air version; the old version is not removed while referenced. Pinned in Task 4.
2. **Core restart with designs in the show** — designs used by rows are reinstalled; others are not installed; no capacity error at startup. Pinned in Task 4.
3. **Library full (16 definitions)** — opening a design that cannot be installed first evicts unreferenced design versions, then reports "Delete designs you no longer use, or remove graphics from the playlist.", never a silent failure. Pinned in Task 4.
4. **Rename a variable that layers use** — every binding follows the rename; removing a used variable is refused naming the layers. Pinned in Task 5.
5. **v2 graphics document from the current beta** — loads, keeps playlist/layouts/pins, gains an empty Designs list. Pinned in Task 2.

---

### Task 1: Native `graphics-template-remove`

**Files:**
- Modify: `native/src/graphics/GraphicsTemplateRegistry.{h,cpp}` (add removal; caps ~142)
- Modify: `native/src/graphics/GraphicsInstances.cpp` (command field table ~320, dispatch beside `graphics-template-install` ~376; in-use check against instances)
- Modify: `native/src/core/Protocol.h` (both command-type arrays; counts), `native/src/rpc/JsonRpcServer.cpp` (~409 routing)
- Test: `native/tests/GraphicsTemplateRegistryTest.cpp` (or the existing registry test file), `native/tests/ContractParityTest.cpp`

**Interfaces:**
- Produces: command `graphics-template-remove {templateId: string, templateVersion: int}` → operation result reasons `template-removed` | `template-built-in` | `template-in-use` | `template-not-found`; the published template list no longer contains the version.

- [ ] **Step 1: Failing tests** — `GraphicsTemplateRegistry.RemoveFreesTheSlotAndBytes` (install a 1-layer declarative template, remove it, list excludes it, installing 10 more fits again); `GraphicsTemplateRegistry.RemoveRefusesBuiltIns` (`corner-logo` v1 → `template-built-in`); `GraphicsInstances.RemoveRefusesAVersionAnInstanceUses` (create an instance on the template → `template-in-use`); `GraphicsTemplateRegistry.RemoveUnknownIsNotFound`; ContractParity passes with the new type.
- [ ] **Step 2: Run each from `native/build-dev`** with the exact filter — FAIL.
- [ ] **Step 3: Implement** removal under the registry's existing lock; in-use is decided where instances live (GraphicsInstances) before calling the registry.
- [ ] **Step 4: Run** the new tests + `GraphicsTemplateRegistry.*`, `GraphicsInstances.*`, `ContractParity.*` — PASS.
- [ ] **Step 5: Commit** `Graphics: native graphics-template-remove for unused non-built-in versions`.

### Task 2: Designs in the show document (schema 3)

**Files:**
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsDocumentStore.cs` (record at 21-55, `CurrentSchemaVersion`, migration ~93-99, `Validate` ~145-180)
- Create: `native-shell/CoreVideoPro.MediaCore/Models/GraphicsDesign.cs`
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsDocumentStoreTests.cs`, new `GraphicsDesignIdsTests.cs`

**Interfaces:**
- Produces: `public sealed record GraphicsDesign(string DesignId, string Name, string Family, string TemplateId, int TemplateVersion, JsonElement Definition, IReadOnlyList<GraphicsDesignAsset> Assets, DateTimeOffset UpdatedAt)`; `public sealed record GraphicsDesignAsset(string AssetId, string Sha256, string RelativePath)`; `GraphicsShowDocument.Designs` (`IReadOnlyList<GraphicsDesign>`, default empty).
- Produces: `public static class GraphicsDesignIds { public static string NewTemplateId(string name, IEnumerable<string> existingTemplateIds, Func<string> hex6); public static bool IsDesignTemplateId(string templateId); }`.

- [ ] **Step 1: Failing tests** — v2 JSON fixture (copy of a real 10-31 document shape with playlist + layouts + pins) loads as schema 3 with empty `Designs` and everything else equal; round trip of 2 designs; validation refuses 33 designs, duplicate `DesignId`, a design whose `TemplateId` fails `IsDesignTemplateId`, empty/81-char names; `NewTemplateId("Panel name bar", [], () => "a1b2c3")` == `design-panel-name-bar-a1b2c3`; a long name is trimmed so the id ≤ 64; collision retries with a new hex; built-in ids are never produced.
- [ ] **Step 2: Run** `dotnet test native-shell/CoreVideoPro.MediaCore.Tests --filter "FullyQualifiedName~GraphicsDocumentStore|FullyQualifiedName~GraphicsDesignIds"` — FAIL.
- [ ] **Step 3: Implement** the records, schema 3 (load 1/2/3; save 3), validation, ids.
- [ ] **Step 4: Run** filter, then the full MediaCore suite — PASS.
- [ ] **Step 5: Commit** `Graphics: designs stored in the show document (schema 3)`.

### Task 3: Blank seeds shared by shell and native

**Files:**
- Create: `contracts/graphics-seeds/{lower-third,title,bug,full-screen}.json`
- Modify: `native-shell/CoreVideoPro.MediaCore/CoreVideoPro.MediaCore.csproj` (embed the four seeds)
- Create: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsDesignSeeds.cs`
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsDesignSeedsTests.cs`, `native/tests/GraphicsDesignSeedsTest.cpp` (+ CMake registration)

**Interfaces:**
- Produces: `public static class GraphicsDesignSeeds { public static IReadOnlyList<string> Kinds { get; } /* "lower-third","title","bug","full-screen" */; public static JsonObject Manifest(string kind, string templateId, int templateVersion, string name); }`.
- Seed content: lower-third = plate rect + accent rect + text `name` (field `name`, required, maxLength 128) + text `title` (field `title`, maxLength 256), `collapseWhenEmpty` on the title line; title = text `heading` + text `subheading`; bug = image slot bound to an `image` field `logo` (optional) + rect backing; full-screen = full-canvas rect + text `heading`. Each declares `defaultPlacement` and `bounds` matching its kind; `schemaVersion` 1, runtime declarative.

- [ ] **Step 1: Failing tests** — C#: each kind returns a manifest with the given id/version/name, the family from Global Constraints, and the listed fields; an unknown kind throws. Native: `GraphicsDesignSeeds.EverySeedLoadsAsADeclarativeTemplate` loads each JSON from `contracts/graphics-seeds/` through `GraphicsTemplate::load` (path from a CMake compile definition) after setting `templateId` `design-seed-test-000000`.
- [ ] **Step 2: Run** both — FAIL.
- [ ] **Step 3: Implement** the four JSON files (model them on `GraphicsCuratedTemplates.cpp`'s definitions), the embed, `GraphicsDesignSeeds`, the native test + CMake.
- [ ] **Step 4: Run** — PASS.
- [ ] **Step 5: Commit** `Graphics: four blank design seeds shared by shell and native`.

### Task 4: Design lifecycle service

**Files:**
- Create: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsDesignService.cs`
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsCommandGateway.cs` (~76-84 action table: `"graphics.template.remove" = ["templateId","templateVersion"]`; library-action rules ~129)
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsWorkspaceLibrary*.cs` (Save keeps `Designs`), `GraphicsWorkspaceSession.cs` (open a design draft: `Browse` of the installed design version)
- Modify: `native-shell/CoreVideoPro.Control/ControlActionRegistry.cs` (`graphics.template.remove`), `native-shell/CoreVideoPro.WinUI/Services/StudioControlSurface.cs` (case + `SupportedActionIds`)
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsDesignServiceTests.cs` (fake invoke/observe like `GraphicsPlayoutServiceTests`)

**Interfaces:**
- Consumes: `GraphicsDesign`, `GraphicsDesignIds` (Task 2), `GraphicsDesignSeeds` (Task 3), native remove (Task 1).
- Produces on `GraphicsDesignService` (ctor: `GraphicsWorkspaceLibrary library, Func<string,string,CancellationToken,Task<NativeGraphicsOperationResult>> invoke, Func<NativeGraphicsState?> observe`):
  - `Task<GraphicsDesignResult> CreateBlankAsync(string kind, string name, CancellationToken ct = default)`
  - `Task<GraphicsDesignResult> CreateFromAsync(string templateId, int templateVersion, string name, CancellationToken ct = default)`
  - `Task<GraphicsDesignResult> OpenAsync(string designId, CancellationToken ct = default)` (install current version if missing; returns the installed `NativeGraphicsTemplate`)
  - `Task<GraphicsDesignResult> SaveAsync(string designId, NativeGraphicsScratch scratch, CancellationToken ct = default)` (new version under the design's template id; replace stored definition; remove superseded versions not referenced by playlist rows, cues or instances)
  - `Task<GraphicsDesignResult> DuplicateAsync(string designId, string name, CancellationToken ct = default)`
  - `Task<GraphicsDesignResult> DeleteAsync(string designId, CancellationToken ct = default)` (refused with the row names when a playlist row pins it; removes its installed versions)
  - `Task EnsureInstalledForPlaylistAsync(CancellationToken ct = default)` (on core restart: install designs pinned by rows/cues only)
  - `public sealed record GraphicsDesignResult(bool Ok, string Message, GraphicsDesign? Design, NativeGraphicsTemplate? Template)`.
- Library-full message, exact: `The graphics library is full. Delete designs you no longer use, or remove graphics from the playlist.` (final review: the original wording named actions that do not exist).

- [ ] **Step 1: Failing tests** — blank create stores a design with a `design-…` id and installs v1; create-from copies a built-in under a new id (built-in untouched); save installs v2, stores it, removes v1 when nothing references it; save keeps v1 when a playlist row pins it (Review Focus 1); delete refused with "Used by: Guest name" when pinned, else removes versions and the design; open of an uninstalled design installs it; capacity refusal surfaces the exact library-full message (Review Focus 3); `EnsureInstalledForPlaylistAsync` installs only pinned designs (Review Focus 2); `graphics.template.remove` passes the gateway.
- [ ] **Step 2: Run** `dotnet test … --filter FullyQualifiedName~GraphicsDesignService` — FAIL.
- [ ] **Step 3: Implement**; reuse the pin/reinstall path in `GraphicsPlayoutService.Ensure` (~300-319) rather than a second install routine.
- [ ] **Step 4: Run** filter + full MediaCore, Control, WinUI suites — PASS (known pre-existing `AdapterConformanceTests` golden excepted).
- [ ] **Step 5: Commit** `Graphics: design lifecycle (create, save, duplicate, delete, install on demand)`.

### Task 5: Variables in the template editor

**Files:**
- Modify: `native-shell/CoreVideoPro.MediaCore/Services/GraphicsTemplateEditor.cs` (add field operations; editable whitelist ~52)
- Test: `native-shell/CoreVideoPro.MediaCore.Tests/GraphicsTemplateEditorTests.cs`

**Interfaces:**
- Produces on `GraphicsTemplateEditor`:
  - `NativeGraphicsScratch AddField(NativeGraphicsScratch s, string id, string kind, int? maxLength = null)`
  - `NativeGraphicsScratch RenameField(NativeGraphicsScratch s, string id, string newId)` (rewrites every `{"field":id}` binding)
  - `NativeGraphicsScratch RemoveField(NativeGraphicsScratch s, string id)` (throws `InvalidOperationException` "Used by: <layer labels>" when bound)
  - `NativeGraphicsScratch SetFieldOptions(NativeGraphicsScratch s, string id, int? maxLength, JsonElement? sample, bool hideWhenEmpty)`
  - `NativeGraphicsScratch BindLayer(NativeGraphicsScratch s, string layerId, string property, string? fieldId, JsonElement? literal)` (exactly one of fieldId/literal)
  - `IReadOnlyList<(string FieldId, IReadOnlyList<string> LayerLabels)> FieldUsage(NativeGraphicsScratch s)`
  - Kinds map to native field types: `text`→`string`, `number`, `colour`→`color`, `on-off`→`boolean`, `image`.

- [ ] **Step 1: Failing tests** — add `location` (text, 40) then bind a new text layer to it; rename `location`→`city` updates the binding (Review Focus 4); remove a used field throws naming the layer; remove an unused field succeeds; 33rd field refused; invalid id refused; `hideWhenEmpty` sets the layer's `collapseWhenEmpty`; the resulting manifest still loads (assert via the existing scratch-validation helper the session uses, or the native validation fixture path used by other editor tests).
- [ ] **Step 2: Run** — FAIL. **Step 3: Implement.** **Step 4: Run** filter + full MediaCore — PASS.
- [ ] **Step 5: Commit** `Graphics: variables (add, rename, remove, bind) in the template editor`.

### Task 6: Design-mode entry points, Variables panel, docs

**Files:**
- Modify: `native-shell/CoreVideoPro.WinUI/Controls/GraphicsBuilderControl.cs` (header: "Your designs" list with Open / Duplicate / Delete, "New design" opening a start dialog with the four blank kinds + "from a look" + name — mockup D2)
- Create: `native-shell/CoreVideoPro.WinUI/Controls/GraphicsDesignStartDialog.cs`, `native-shell/CoreVideoPro.WinUI/Controls/GraphicsVariablesPanel.cs`
- Modify: `native-shell/CoreVideoPro.WinUI/Controls/GraphicsWorkspaceControl*.cs` (open a design as the private draft; host the Variables panel in the inspector; Save → `GraphicsDesignService.SaveAsync`; "Save to playlist")
- Modify: `native-shell/CoreVideoPro.WinUI/ViewModels/StudioViewModel.Graphics*.cs` (construct `GraphicsDesignService`; core-restart hook calls `EnsureInstalledForPlaylistAsync`)
- Modify: `native-shell/CoreVideoPro.WinUI/Controls/GraphicsBuilderControl.cs` logo picker filter (drop `.svg` until Slice 4)
- Docs: `docs/reference/graphics-playout.md` (new "Designs" section), CLAUDE.md row, `docs/BACKLOG.md` row (Phase 2 slices 1–5 with the spec link)
- Test: layout probe (`scripts/test-studio-layout.ps1`) extended in `GraphicsWorkspaceLayoutProbe`: Design shows "Your designs" and "New design"; the start dialog lists the four kinds; the Variables panel renders with the seed's fields.

**Interfaces:**
- Consumes: `GraphicsDesignService` (Task 4), `GraphicsTemplateEditor` field ops (Task 5), `GraphicsDesignSeeds.Kinds` (Task 3).

- [ ] **Step 1: Extend the layout probe** with the three assertions; run it (no CoreVideo app running; new `-ReportDirectory` under `D:\CoreVideo-QA\tmp\`) — FAIL.
- [ ] **Step 2: Implement** the list, dialog, Variables panel (add / rename / delete / kind / max length / sample / if-empty; "Used by"), Save and Save to playlist wiring, restart hook, logo filter fix.
- [ ] **Step 3: Run** the probe — PASS; full WinUI suite — PASS (golden excepted).
- [ ] **Step 4: Docs** as listed.
- [ ] **Step 5: Commit** `Graphics: designs in Design mode (new, open, duplicate, delete) and the Variables panel`.
