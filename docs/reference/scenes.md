# Scenes: the rail, the catalog, removal and restore

Read before touching the scene list (`StudioViewModel._scenes`), scene persistence
(`ProductionOutputPreferences.CustomScenes` / `RemovedCatalogSceneIds`), the New scene
starters, or anything that cues a scene by id (Magic Scene presets, shot layouts, the
outputs drill). The Scene Builder canvas itself is
[`scene-builder-workspace-spec.md`](scene-builder-workspace-spec.md).

## What a scene is, and where they come from

A `Scene` (`Models/ProductionModels.cs`) is an id, a name, a layout wire name and an
automation label; its routes live beside it in `_sceneRoutes[sceneId]` and are built lazily
from `SceneRoutingService.GetStarterRoutes` the first time anyone asks. The rail is seeded on
every launch from `ProductionCatalog.Scenes`, the five **catalog scenes**:

| Id | Name | Layout |
|---|---|---|
| `intro` | Intro | `host-focus` |
| `interview` | Interview | `two-up` |
| `speaker-slides` | Speaker + Slides | `speaker-slides` |
| `panel` | Panel | `smart-grid` |
| `closing` | Closing | `outro` |

Everything else is a **custom scene** (`custom-<8 hex>`): New scene, Duplicate, Save from the
canvas, CoreVideo Tiles. Two hidden `multiview-solo-a/b` scenes back the multiview
double-click one-up; they never appear on the rail (`VisibleScenes`).

## Removal (#1070, 2026-10-31)

Owner, during live-show prep: "Not sure why we have scenes that can't be deleted. Intro -
Closing don't have the delete option." Until then only `custom-*` ids were removable.

The rule is now in `Services/SceneCatalogPolicy.RemovalRefusal` and applies to catalog and
custom scenes alike:

- the **on-air** scene (`ActiveSceneId`) cannot be removed ("The on-air scene cannot be removed");
- the **last visible** scene cannot be removed ("The last scene cannot be removed"); hidden
  multiview-solo scenes neither count as survivors nor are protected;
- anything else can go. Removing the Preview scene moves Preview to Program.

`SceneDisplayItem.CanRemove` feeds the trash button on the Studio rail; the command path
(`RemoveSceneCommand`) answers with the same refusal text in `CommandStatus`.

### Persistence

Custom scenes are saved whole (`CustomScenes`, scenes redesign S2). Catalog scenes are
re-seeded at launch, so a removal is persisted **by id**: `RemovedCatalogSceneIds` (prefs
v15) is captured on every save as "catalog ids absent from the rail" and applied in
`ApplyProductionOutputPreferences` **after** the custom scenes are restored, so the
last-scene guard counts them (`SceneCatalogPolicy.RemovalsToApply`). When the construction
default Program/Preview scene (`speaker-slides`) is in the removed set, both are moved to the
first surviving scene through the backing fields before the removal (the O1 vcam pattern: the
setters would schedule core syncs the bridge cannot take during construction). A persisted
set that would empty the rail is applied only as far as one visible scene remains. Absent field
= nothing removed; an older build drops the field and the catalog scenes simply reappear.

### Getting a scene back

- **New scene** (Sources tab, `+`) has a starter for every catalog layout: Solo (`host-focus`),
  Two people (`two-up`), Screen + presenter (`speaker-slides`), **Panel grid** (`smart-grid`,
  Inputs 1 to 6) and **Closing** (`outro`), `StarterSceneFactory`. Each binds stable Show
  Inputs, never the active speaker.
- **Restore starter scenes** (same menu; control API `scene.restoreStarters`) re-adds every
  missing catalog scene at its catalog position with fresh starter routes
  (`SceneCatalogPolicy.MissingCatalogScenes`). Existing scenes, Program and Preview are
  untouched; a restored scene can be deleted again.

### Consumers that name catalog ids

- **Magic Scene / show-type presets** (`ProductionStateHelper.BuildAutomationRecommendation`)
  recommend `intro` / `interview` / `speaker-slides` / `panel`. When the recommended scene is
  not on the rail the recommendation names the **first scene** and says so in its reason
  (`SceneCatalogPolicy.ResolveRecommendedSceneId`), but as a suggestion only: confidence 0,
  action `hold`, no slot bindings, so `MagicSceneCoordinator` stays on its hold path and
  Set & Forget never auto-Takes a scene the operator did not choose with another layout's
  bindings. The native director branch already required the scene to exist.
- **Shot layouts** (`graphics.layout.*`) carry `sceneExists` in their listing and a layout whose
  scene is gone is reported, not cued (#1076).
- **Outputs drill** (`scripts/qa/outputs-drill.ps1`) cues `speaker-slides` (or `intro` with
  `-NoWebcam`) by id. It reads `scene.list` first, invokes `scene.restoreStarters` once when the
  planned scene is missing, re-reads, and refuses to start any output if it is still absent
  (`docs/qa/outputs-drill.md` step 6). The restore does not outlive the run: the drill puts the
  preferences file back byte-for-byte after the app exits. An installed app that predates
  `scene.list` (HTTP 400) skips the check with a note.
- `DirectorBindingHoldPolicy` compares ids only; a missing id simply never matches.

## Control API

| Action | Effect |
|---|---|
| `scene.list` | read: `{scenes:[{id,name,layout}], previewSceneId, activeSceneId}` under `data` |
| `scene.restoreStarters` | re-add any removed catalog scene; no-op when all are present |
| `scene.select` | now refuse-closed like the two below: an unknown scene id is a `422` that changes nothing, and the reply carries `previewSceneId` when applied (it used to answer `ok` and only write a status line) |
| `scene.cueWithRoutes`, `scene.route.set` | unchanged; an unknown scene id is a `422` that changes nothing |

## Tests

`SceneCatalogPolicyTests` (rules, persistence set, restore order, recommendation fallback),
`StarterSceneFactoryTests.CatalogOnlyLayoutsHaveStartersBoundToDistinctInputs`,
`AutomationPolicyTests.RecommendationFallsBackToTheFirstSceneWhenThePresetSceneWasRemoved`,
`ProductionOutputPreferencesStoreTests.Serializer_RoundTripsRemovedCatalogSceneIds`,
`SceneSelectArgsTests`, `ControlActionRegistryTests.CoreActions_ArePresent` /
`SceneReadAndRestoreActionsTakeNoArgs` and
`StudioControlSurfaceCoverageTests.Adapter_HandlesEveryRegisteredAction` (new ids), and the
drill lib check in `scripts/qa/outputs-drill.test.ps1`. `StudioViewModel` is not constructible
in tests, so the launch-time removal path (`ApplyPersistedCatalogSceneRemovals`), the
Preview-moves-to-Program rule and the `scene.list` payload are covered through the policy
statics (`SurvivorAfterRemovals`, `RelocateAfterRemovals`, `PreviewAfterRemoval`,
`SceneListJson`), not end to end.
