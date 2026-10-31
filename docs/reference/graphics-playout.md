# Graphics playout (the Graphics tab's top level)

_Added 2026-10-31 for issue #1076 from the owner-approved mockups (NewBlue Titler
Live / Captivate playout model). This is the spec's Phase 0 plus the playlist half
of Phase 1 (`docs/titler-service-spec.md`); the designer, data binding beyond
Sources text, crawls and external key/fill are later phases._

## The operator model

Three verbs, one list, nothing hidden:

- **Playlist.** Every graphic in the show is a row: Guest name (one row, one
  entry per show-input slot bound to a person or local source), Show title, Logo,
  LIVE bug, Sponsor card. Each row has a hotkey (F1..F12 by default, in row
  order) and a "plays out after" of manual, 8 s or 15 s.
- **Play In** puts the graphic on Program now. **To Preview** stages it so it
  rides the next Take. **Play Out** takes it off Program. **Remove** takes it off
  Preview. Play Out always sits beside the thing it removes, in the row and in
  the strip.
- **The On Air strip** at the top of the Graphics tab and above the Studio
  console: ON PROGRAM chips (each with Play Out), ON PREVIEW chips (each with
  Remove), an "auto" badge on graphics the native automation put up, an armed
  rule chip with Disarm, plus Play Out all and Clear Preview. The strip is built
  only from what the core is compositing (`NativeGraphicsState.Instances`:
  Program = `ProgramRevision > 0` and visibility not hidden or building-out,
  Preview = `CueRevision > 0`), never from button state.
- **Playout | Design.** The tab header toggle. Design is the existing graphics
  workspace (template library, starters, automation drawer, style designer),
  unchanged. Nothing is on air that the strip does not show.

Vocabulary mapping to the spec: Play In = take the graphic to Program, Play Out
= hide. The control-API ids use playIn/playOut so a Companion button reads like
the screen.

## Add a graphic

First open: the Graphics tab shows the dashed "Your graphics will live here" card.
"Add your first graphic" installs the default playlist for the first show starter:
Guest name (F1, plays out after 8 s), Show title (F2), Logo (F3), LIVE bug (F4)
and Sponsor card (F5, 15 s, no image until you pick one), plus the four preset
shot layouts below. With a playlist in place, "+ Add graphic" opens Design, where
a new template or image becomes a row through `graphics.playlist.row.set`. Rows
are edited in the inspector: name, title line, From Sources, plays-out-after and
hotkey. Nothing is created in the core until a row is played in or cued.

## Play In vs To Preview

**Play In** puts the graphic on Program now: the row's native instance is created
or refreshed, cued, and shown from that cue in one step. **To Preview** stops
after the cue: the graphic appears in the Preview composite and rides the next
Take, so the operator can stage a lower third with the next scene and have both
land together. A graphic on Preview offers Play In (now) or Remove; a graphic on
Program offers Play Out. The strip shows which is which from core truth.

## Shot Layouts

A shot layout is one button that cues a scene and the graphics that belong with
it, so a single Take changes the picture and the graphics together (that is what
the core-side "cued graphics ride the Take" work exists for). The SHOT LAYOUTS
row sits under the On Air strip in the Graphics tab (full form only). Click a
layout to cue it; the selected layout offers "Cue and Take"; "Edit layouts" opens
the flyout to name it, pick the scene, tick the playlist rows, set its Shift+F
key and choose whether it plays out other graphics.

`ShotLayout { Id, Name, SceneId, RowIds[], Hotkey ("Shift+F1".."Shift+F11" or
blank), PlayOutOthers }` persists on the graphics document as `ShotLayouts`
(schema stays 2; older documents load it empty). The default playlist installs
four presets:

| Layout | Scene | Rows | Hotkey | Notes |
|---|---|---|---|---|
| Open | `intro` | Show title, Logo | Shift+F1 | |
| Two-up | `interview` | Guest name | Shift+F2 | |
| Panel | `panel` | Guest name, Logo | Shift+F3 | |
| Clean | none | none | Shift+F11 | `PlayOutOthers`: plays every Program graphic out; Shift+F12 is already Play Out all |

Cue (`graphics.layout.cue`) selects the scene to Preview through the same
`SelectScene` path as the Scenes page and `scene.select`, then does To Preview for
each row. A Guest row cues the entries bound to the slots that scene routes (Show
Input slot routes and fixed Zoom participant routes); when none resolve it cues
the current Program subject. `PlayOutOthers` plays out Program graphics the layout
does not carry. Everything is resolved before the first side effect: a layout
whose scene is no longer in the show, or that names a playlist row that no longer
exists, is refused whole with the reason, never silently skipped. Cue and Take
(`graphics.layout.take`) runs the ordinary Take afterwards (a layout with no scene
and no rows only plays out).

Control API: `graphics.layout.list` (read; layouts, the show's scenes, preview
and active scene ids), `graphics.layout.set` (`layoutJson`, upsert by id; refused
when the scene or a row does not exist; a hotkey held by another layout moves),
`graphics.layout.remove` (`layoutId`), `graphics.layout.cue` (`layoutId`),
`graphics.layout.take` (`layoutId`). `ControlState.GraphicsLayouts` carries id,
name, sceneId and hotkey; OSC feedback adds
`graphics/layout/<id>/name|scene|hotkey|onPreview`.

## Guest names from Sources

The Guest row is one row with one entry per Show Input slot bound to a source
(Zoom participant, capture device, media). With "From Sources" on (the default)
each entry's name and title line come from the Sources page text for that source
(`SourceLowerThirdText`), so renaming a person in Sources renames their lower
third; the inspector shows the title read-only and offers "Edit names in
Sources". A bare F1 or "LT in" plays in the entry for the source currently on
Program. Entries are re-projected whenever the roster or the Sources text
changes; nothing is pre-created in the core.

## One truth for Preview and Program (#1072)

Before this change a lower third could show in Preview and vanish on Take:
Preview rendered the cued scene's graphics while Program keyed a separate legacy
overlay (`key:lower-third`) under its own enabled/suppressed flags, and the Take
copied routes, overlays and captions but never graphics.

Now Play In / To Preview / Play Out drive native **graphics instances** only
(`native/src/graphics/GraphicsInstances.cpp`), one instance per playlist entry
(`playlist-<rowId>` or `playlist-<rowId>-<sha256(sourceId)[:12]>`), and the core
makes the Take carry them:

- `graphics-instance-cue` accepts `rideTake` (default true). A Rehearsal
  automation cue sends `rideTake=false`.
- `MediaCore::beginTakeTransition` calls `GraphicsInstances::promoteCuedForTake()`:
  every cued, ready, `rideTake` instance is shown from its cue and its cue is
  cleared, so it leaves Preview and does not ride the next Take.
- `graphics-instance-trigger action=show target=cue` (Play In from Preview) also
  clears the cue. `action=uncue` (Remove) clears exactly that instance's cue and
  touches nothing on Program.
- The lower-third family authority flips to `instances` only after the first
  Play In is accepted, so the legacy key keeps working until a playlist graphic
  has actually rendered. The Studio "LT in / LT out" button and
  `graphics.lowerThird.toggle/set` route into the Guest row for the current
  Program subject; `automation.lowerThirds.set` keeps driving the native
  automation rules, whose cues and shows appear in the strip as "auto".

## Where things live

| Thing | Code |
|---|---|
| Rows, entries, strip model, Play In/Out sequence, auto-out timer, shot layout cue | `native-shell/CoreVideoPro.MediaCore/Services/GraphicsPlayoutService.cs`, `Models/GraphicsPlaylist.cs` (`GraphicsPlaylistRow`, `ShotLayout`) |
| Persistence (additive `Playlist` and `ShotLayouts` on the show document, schema 2) | `Services/GraphicsDocumentStore.cs`, `%LOCALAPPDATA%\CoreVideoPro\graphics\default.json` |
| Default playlist and preset layouts from the curated templates | `Services/GraphicsWorkspaceLibrary.Playlist.cs` (`DefaultPlaylistFor`, `DefaultShotLayouts`, `SaveShotLayoutsAsync`) |
| ViewModel seam (strangler partial; scene select + routed guests for layouts) | `ViewModels/StudioViewModel.GraphicsPlayout.cs`, routing in `StudioViewModel.StudioLowerThird.cs` |
| Control API and state | `CoreVideoPro.Control/ControlActionRegistry.cs`, `ControlState.cs` (`GraphicsStrip`, `GraphicsPlaylist`, `GraphicsLayouts`, `LowerThirdOnAir`), `WinUI/Services/StudioControlSurface.cs` |
| UI | `Controls/GraphicsOnAirStripControl.cs`, `Controls/GraphicsPlayoutControl.cs` (full and `Compact`), `Views/OverlaysPage.xaml` (toggle), `Views/StudioWorkspace.xaml` (strip + compact playlist), `Services/GraphicsHotkeyRouter.cs` |
| Core | `native/src/graphics/GraphicsInstances.*` (`rideTake`, `promoteCuedForTake`, `uncue`), `native/src/core/MediaCore.cpp` (`beginTakeTransition`) |

Entries are binding-shaped: a row's `Fields` are the template's variables and a
guest entry is `Bindings` of those variables to a source (today `name` and
`title` from Sources text, #593). Phase 1 data sources slot in without renaming.

## Control API

| Action | Arguments |
|---|---|
| `graphics.playlist.list` | read; `data` = rows, guest entries, `programSubjectSourceId` |
| `graphics.playlist.row.set` | `rowJson` (one row, upsert by id) |
| `graphics.playlist.row.remove` | `rowId` |
| `graphics.playlist.hotkey.set` | `rowId`, `key` (F1..F12 or blank; a key held by another row moves) |
| `graphics.playlist.default` | `starterId` (optional; refused when a playlist exists) |
| `graphics.playIn` | `rowId`, `sourceId` (optional; a Guest row defaults to the Program subject) |
| `graphics.toPreview` | `rowId`, `sourceId` (optional) |
| `graphics.playOut` | `instanceId`, or `rowId` + `sourceId` (a Guest row with no source plays out every entry on Program) |
| `graphics.preview.remove` | `instanceId` |
| `graphics.playOutAll`, `graphics.preview.clear` | none |
| `graphics.strip` | read; `data` = the strip |
| `graphics.layout.list` | read; `data` = layouts (with `sceneExists`), scenes, `previewSceneId`, `activeSceneId` |
| `graphics.layout.set` | `layoutJson` (one layout, upsert by id) |
| `graphics.layout.remove` | `layoutId` |
| `graphics.layout.cue` | `layoutId` (scene to Preview + rows to Preview; refused whole when a scene or row is missing) |
| `graphics.layout.take` | `layoutId` (cue, then Take) |

Read actions return `{"ok":true,"data":...}` over HTTP. OSC feedback adds
`graphics/onProgramCount`, `graphics/onPreviewCount`, `graphics/automationMode`,
`graphics/automationArmed` and per row `graphics/row/<id>/onProgram|onPreview|hotkey|name`.

## Hotkeys

F1..F12 unmodified: Play In of the row with that hotkey (a Guest row plays in
the entry for the current Program subject). Shift+F1..F11: cue the shot layout
with that hotkey (`graphics.layout.cue`). Shift+F12: Play Out all. Keys are
ignored while a text field has focus and are active only on the Studio and
Graphics tabs (`GraphicsHotkeyRouter`, pure and unit-tested). Assign keys from
the inspector (rows) or the Edit layouts flyout (layouts); one key belongs to one
row or layout and moves when reassigned.

## Rules to keep

- Never add a Show or toggle on the Graphics tab without its Play Out / Remove in
  the same place while the graphic is live (owner, 2026-10-31).
- The strip derives from the core's instance state only. A chip that is not
  there is a bug in the mapping, not a reason to add local state.
- Do not flip lower-third authority to instances before an accepted Play In.
- Instance capacity is eight per bus: entries are created lazily on Play In /
  To Preview and hidden ones are removed; a 10-guest roster never pre-creates.
- Outputs drill: unaffected (it reads `graphics.coreEpoch` only); never rename
  an action id, the Companion manifest is generated from the registry.

## Acceptance (owner at the keyboard, installed build)

Each from a cold open in under 3 clicks and 15 s: name a person on Program,
edit a lower third's text, add a logo or title, see what is live versus staged
and clear it. For every Show on the tab, find and use its Play Out within 5 s
while it is on Program.

## Later (the spec's phases)

Per-tile names in Tiles (#1073, native placement per tile rect), Zoom participant
metadata, clocks and sheets as data sources, the visual designer, crawls, NDI
fill+key. Shot layouts landed in slice 2 (above).
