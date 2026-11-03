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
- **Show** puts the graphic on Program now. **Cue** stages it so it
  rides the next Take. **Hide** takes it off Program. **Clear cue** takes it off
  Preview. Hide always sits beside the thing it removes, in the row and in
  the strip.
- **The On Air strip** at the top of the Graphics tab and above the Studio
  console: ON PROGRAM chips (each with Hide), ON PREVIEW chips (each with
  Remove), an "auto" badge on graphics the native automation put up, an armed
  rule chip with Disarm, plus Clear all and Clear cued. The strip is built
  only from what the core is compositing (`NativeGraphicsState.Instances`:
  Program = `ProgramRevision > 0` and visibility not hidden or building-out,
  Preview = `CueRevision > 0`), never from button state.
- **Operate | Design.** The tab header toggle. Design is the existing graphics
  workspace (template library, starters, automation drawer, style designer),
  unchanged. Nothing is on air that the strip does not show.

Vocabulary (owner, 2026-10-31): Cue, Show, Hide, the verbs the graphics workspace
and automation already use; "Play In / Play Out" was dropped because playout has
its own meaning in production. The control-API ids keep their shape
(`graphics.playIn` = Show, `graphics.toPreview` = Cue, `graphics.playOut` = Hide,
`graphics.preview.remove` = Clear cue) so Companion manifests stay stable.

## Add a graphic

First open: the Graphics tab shows the dashed "Your graphics will live here" card.
"Add your first graphic" installs the default playlist for the first show starter:
Guest name (F1, plays out after 8 s), Show title (F2), Logo (F3), LIVE bug (F4)
and Sponsor card (F5, 15 s, no image until you pick one), plus the four preset
shot layouts below. With a playlist in place, new graphics are built in Design
(below) and land as rows. Rows are edited in the inspector: name, title line,
one box for each other text variable of the row's graphic (a design's
`location`, say; header = the variable's label, limit = its max length, or
512 characters, the core's limit, when it has none; Enter or leaving the box
saves it on the row, and Show, Cue and Take first save text still being typed,
so there is no need to press Enter before Show). Saving does not rebuild the
inspector, so the box keeps focus. A later **Save to playlist** from the
Designer re-pins the row but keeps every non-empty text value typed on it for
variables the new version still has; empty values and new variables take the
Designer draft's value, and variables the design dropped are removed. From
Sources, hides-after and hotkey. Those boxes appear on a Guest row that follows
Sources too: Sources fills only name and title, so a typed location goes on air
beside the Sources text. Not offered: name, title, time and label (handled
elsewhere) and variables with a fixed choice list. "Open in Design" reopens that row: a
design's row opens the design in the Designer (below), any other row opens in
the builder with its look and fields (also the fallback when the design cannot
be opened). Nothing is created in the core until a row is shown or cued.

## Build a graphic (Design)

Design opens on the **builder** (slice 3; the spec's "basic designer"), five
numbered steps left to right:

1. **What are you making**: Lower third, Title, Logo or bug, Clock or countdown,
   Sponsor card. The choice narrows the looks to one template family and decides
   the row kind it saves as (a lower third saves as a Guest row when "Names from
   Sources" is on, otherwise as a fixed-text Title-kind row; a clock template turns
   the row clock on).
2. **Pick a look**: every installed template of that family, newest version of
   each, with the core's native thumbnail. Your own designs are not repeated
   here; they are listed once, under Your designs (#1087 N3). A sponsor card is an image you choose
   instead (PNG/JPG, full frame), pinned through `AssignPlaylistImageAsync`.
3. **Preview**: the workspace session's private draft on the native authoring
   canvas. It is never on air.
4. **Fill it in**: the template's own fields (text, choices, numbers, colors,
   bundled images), a logo slot with "Use the brand kit logo", and four color
   swatches (accent, background, name, second line) with "Use brand kit colors".
   Text commits 350 ms after you stop typing, or on focus loss. **Motion** picks
   how the graphic comes in and goes out on air (Fade default, Cut, Slide from
   left/right/up/down) at Quick/Normal/Slow (150/300/600 ms); it is stored on the
   row as `motion` (the core's `{in,out}` transition shape, whitelisted at save)
   and sent with the instance on Show/Cue. Rows without one keep the playout
   default fade. **Position / Size** (nine anchors, 50–150 %) and dragging the
   preview set the row's `placement`; it is stored only when it differs from the
   template default and is sent with the instance ("Template default" clears it).
   **Text** offers a typeface (safe Windows families only; a package font shows
   as unset and is left alone) and name / second-line sizes, written to the
   row's style.
5. **Save to playlist**: name, hotkey (defaults to the lowest free F-key; taken
   keys say who owns them), hides after (stays / 8 s / 15 s), then **Add to
   playlist** or, for a row opened from Operate or the "Your graphics" chips,
   **Save changes** (the id is kept so shot layouts still find it). "Open in
   Operate →" appears after a save.

The builder's header has two more entries (top right):

- **Open in Designer** opens the design selected under Your designs in the
  Designer (if it is already the private draft, the Designer shows it with its
  unsaved edits). With no design yet it opens New design.
- **Cues & automation** is the previous workspace without its layer editor and
  design actions: show templates, saved cues and styles, package import and
  export, the Automation drawer, the existing overlay controls, and Cue / Show /
  Hide of private cues. Probes, the control API and scripts still rely on it;
  its actions run on `GraphicsWorkspaceSession` / `GraphicsWorkspaceLibrary`.
  **← Build a graphic** (beside the title) returns to the builder.

Exactly one Design surface (builder, Designer or Cues & automation) shows and
holds the private preview lease at a time (`GraphicsDesignSurfaces`). The pure decisions
(`GraphicsBuilder`: kinds, template filter, slug ids, free hotkey, row shape,
upsert order) are unit-tested; the control is `GraphicsBuilderControl`, hosted by
`OverlaysPage` beside the workspace and the playlist.

## Designs (Graphics Designer slice 1)

A **design** is a graphic the operator authored, stored in the show's graphics
document (`GraphicsShowDocument.Designs`, schema 3; a v2 file loads with none and
is kept as `<name>.v2.bak` on the first v3 save). It survives restarts and
belongs to that show. Spec: [2026-11-01-graphics-designer-phase2-design.md](../superpowers/specs/2026-11-01-graphics-designer-phase2-design.md).

- **Your designs** (a bar under the builder's header) lists the show's designs
  (at most 32). Click a design to select it; **Open design** (or double-click the
  design) installs the design's current
  version if the core lacks it and opens it in the **Designer** as the
  workspace's *private draft*. **Duplicate** makes a new design (new id, same
  definition, "<name> copy"). **Delete** asks first and is refused while a
  playlist row uses the design ("Used by: <row names>") and while the design is
  open in the editor ("Open another design or look first"); otherwise it removes
  the design, its template pins and every installed version no saved cue or live
  instance uses. Duplicate and Delete work while the core is down (Delete then
  skips the native tidy; a restarted core starts without the design).
- **New design** opens the start dialog (mockup D2): **Start blank** with one of
  four seeds (Lower third, Title, Bug or corner, Full screen; manifests in
  `contracts/graphics-seeds`), **or start from a look** (any design or installed
  declarative template, always copied under a new id; built-ins are never edited
  in place), a **Name** (1–80 characters), then **Open in the designer**. A
  refusal (library full, too many designs, invalid name) keeps the dialog open
  with the reason.
- **Variables** (mockup D3, the Designer inspector's Variables tab): each
  variable with its kind, limit and **Used by** (the layers bound to it); add
  (Text with max length, Number, Colour, On/off, Image), rename (every binding
  follows), delete (refused while a layer uses it), max length, sample value and
  **If empty: hide its layer / show the sample**. **Apply** with a new sample
  value also sets the draft's value while that value still equals the previous
  sample (always true for a just-added variable, whose value starts empty), so
  the Typical preview shows the line and Save to playlist carries it to the
  row. A value typed elsewhere (the builder form) is kept. Native limits stay: 32
  variables, ids `[a-z][a-z0-9-]{0,63}`. Edits change the private draft only
  (the template scratch plus draft values kept in step, since the core refuses
  unknown or missing fields). The Designer's **Shows** and **Image…** lists offer
  the edited variables, so a new variable can be bound before saving.
- **Save** (Save design) and **Save to playlist · F-key** sit in the Designer's
  header, with its status line; editing is paused while a save runs. **Save design** installs the edited manifest as the design's next template
  version (`design-<slug>-<6 hex>`, never a built-in id), stores it as the
  design's definition, removes superseded versions nothing references, and
  re-opens the draft on the new version. Saving a look that is not a design
  first makes it a new design "<look> copy". The values typed into the draft,
  its style, position and motion carry onto the re-opened version as its new
  baseline (Undo right after Save does not revert them), and remembered drafts
  of the design's older versions are forgotten so they never hold an old (even
  once on-air) version against eviction. **Save to
  playlist** saves, then adds (or re-pins) one playlist row for the design on
  the next free F-key, carrying those values; the version the row pinned before
  is uninstalled when nothing else uses it.
- **Native library slots**: the core holds 16 definitions, 6 of them the curated
  templates (the 3 compatibility looks `compact-solid`, `minimal-accent` and
  `broadcast` are outside the registry), so designs share 10 slots. A design
  version is installed when it is created or opened; moving the editor to
  another design or look uninstalls the previous design's versions nothing
  references (unless its draft holds unsaved template edits). The draft's typed
  values, style, position and motion are remembered without holding a slot and
  come back when the design is opened again (a core restart clears them). When
  an install meets a full library, every installed design version that is not
  referenced (playlist row, saved cue, Studio lower third, live instance), not
  the open draft and holds no unsaved edits is removed and the install retried
  once. The design lifecycle and the playlist's pin reinstall (Show / To
  Preview of a row whose design is not installed) share this one evictor
  (`GraphicsDesignEviction`). Only then is "The graphics library is full.
  Delete designs you no longer use, or remove graphics from the playlist." shown.
- **Template pins**: every graphics-document save keeps only the pins a playlist
  row (template or image), a saved cue (draft or image) or the Studio lower
  third still references (`GraphicsWorkspaceLibrary.ReferencedTemplates`), so
  repeated Save to playlist never reaches the 16-pin document limit.
- **Core restart**: designs nobody uses stay uninstalled (the native registry
  holds 16 definitions incl. the 6 curated templates). On a new core generation
  the shell reinstalls the design versions that playlist rows, saved cues and the
  Studio lower third pin
  (`EnsureInstalledForPlaylistAsync`, armed from `OnBridgeProfileChanged` and run
  once per new core epoch when its graphics state arrives; `GraphicsDesignResync`).
  `OnBridgeProfileChanged` also fires on the first handshake, so the graphics
  document now loads at app launch even if the Graphics tab is never opened.

One `GraphicsDesignService` (MediaCore) owns the lifecycle and one
`GraphicsDesignWorkflow` (MediaCore, unit-tested) the operator steps: open =
install + browse as the private draft; Save on a look creates the copy, moves the
draft (with its edits) onto it and then saves, so a failed save is retried into
the same copy; a stored design is reported as stored even if the editor was busy;
`PlaylistRow` re-pins the design's existing row or takes the next free F-key. The
ViewModel seam is `StudioViewModel.GraphicsDesigns.cs`; the UI is `GraphicsBuilderControl` (Your
designs), `GraphicsDesignStartDialog`, `GraphicsVariablesPanel` and
`GraphicsDesignerControl` (Save / Save to playlist). The builder's
logo picker offers PNG and JPEG only; SVG returns with rasterised import
(slice 4).

**Layer structure edits (slice 2).**
`GraphicsTemplateEditor.Structure.cs` holds the pure edits the Designer's layers panel
calls: `SetLabel` (layer name = `metadata.label`, 1–64 characters; allowed on locked
layers), `SetVisible` (hide = native `"visible": false`, show removes it; allowed on locked
layers; refused when a variable drives visibility), `MoveTo` (reorder or reparent; front
index 0 = frontmost; a parent change keeps the canvas position and grows a free-layout
group to fit inside its padding, never past its parent's origin), `Group` / `Ungroup` (positions, flow steps,
opacity and hidden state carried over; refused when a variable-driven opacity or visibility cannot be), `AddLayer` (text / shape / clock, readable ids `text-1`…, brand tokens; clocks only
in clock and countdown designs) and `AddImage` (an image variable, a declared asset or a
new optional image variable); `Duplicate` names the copy "… copy". Every edit keeps the
native limits (64 layers, nesting 8, literal x/y 0–8192) and keeps required variables on a layer. Names and
locks are shell-only metadata; native round-trips them untouched, pinned by the
`designer-structure` fixture (`GraphicsTemplate.HiddenGroupDrawsNoChildrenAndKeepsEditorMetadata`).

## The Designer (Graphics Designer slice 2)

The Designer is where a design's layers are edited (mockup D1). It opens from
**Open in Designer**, **Open design** / double-click under Your designs, New
design, and Operate's "Open in Design" on a design's row. A design that is
already the private draft is shown as is (unsaved edits kept, no core needed);
one open runs at a time ("Graphics is busy. Try Open design again in a
moment."). When Operate's "Open in Design" cannot open the design, the builder
opens the row instead and its design status says why. It replaces the old
workspace's layer pane. Everything you do here edits the private draft;
nothing reaches Preview or Program until you Show or Cue the row in Operate.

**Header.** `LOWER THIRD · DRAFT` and the design name; **← Your designs** (back
to the builder; unsaved edits stay in the draft); **Undo**; **Save** ("Save as
new design" when the draft is a look, not a design); **Save to playlist · F6**
(the key it will use: the design's existing row's key, else the next free one);
and a two-line status (full text in its tooltip). Editing is paused while a save
runs; afterwards the inspector stays on the tab you were on, with the same layer
selected (when the saved version still has it).

**Use for Zoom lower thirds** (lower-third designs only) saves the design and puts
it on the playlist's **first Guest row**, the row the footer LT button, the
lower-third hotkeys and automation always use (a later Guest row is never used
for them). The row keeps its name, hotkey, "Hides after" and Sources link; a
Guest row is added when the playlist has none. After further edits, **Save to
playlist** re-pins that same row, so the Zoom lower thirds follow the design.

**Layers panel (left).** One row per layer, front to back: "Top of the list is
in front. Drag to reorder, double-click to rename." Each row shows the layer's
name and what it shows (`{name}` for a variable, or its kind).

- **Eye** hides or shows the layer. **A hidden layer is hidden on air too**:
  the hide is saved in the design (native `"visible": false`), not just in the
  editor. A layer whose visibility a variable drives cannot be hidden here (the
  eye is disabled and says why).
- **Lock**: a locked layer cannot be moved, resized, edited in the inspector or
  dragged elsewhere; it can still be renamed and hidden.
- **Rename**: double-click, type, Enter (Esc or clicking away cancels).
- **Drag** a row onto the upper or lower half of another to put it before or
  after it, onto the middle of a group to put it inside, or to the end of the
  list to send it to the back. A layer moved into a group may shift so it fits
  inside the group; the status then says "Moved to fit inside the group."
- **Duplicate** (the copy goes just in front, named "… copy"), **Delete** (Undo
  brings it back), **Group** (wraps the selected layer in a new group) and
  **Ungroup** (replaces the selected group with its layers; positions are kept).
- **Add**: **Text**, **Shape**, **Image…** (an image variable, an image already
  in the design, or a new image variable chosen when the graphic is used) and
  **Clock** (only in clock and countdown designs; elsewhere the button says
  "Clocks tick in clock and countdown designs. Start one from the Clock look.").
  The new layer is selected. A new text layer goes 8 px under the lowest text
  layer (same x and width) when it fits inside the graphic's bounds; otherwise
  (the Lower third blank: 168 px tall, its title line ends at 136) it lands at
  40,40 over the other lines. Drag it into place (snapping helps) and shrink the
  name and title lines to make room: the graphic is drawn at its bounds, so
  anything below them is cut off.

**Canvas (centre).** The native preview of the draft. Click a layer to select
it, drag to move it, drag its corner to resize it. Moves and edge resizes
**snap** to the safe area and the centre lines (within 8 px), and the guide is
drawn while they hold; **hold Ctrl to place freely**. Resizes that keep the
aspect ratio (image layers, or Shift held), scaling the whole graphic, and
keyboard nudges do not snap.
**Sample text** buttons under the canvas change only the preview: **Typical**
(your draft's values; drags work on this one), **Longest name** (the first
text variable filled to its limit, to check the layout; the other variables keep
their draft values) and **Empty title** (the second line left blank).

**Inspector (right), Layer tab.** `NAME · KIND LAYER` and the selected layer's
properties; a locked layer shows them disabled.

| Layer | Fields |
|---|---|
| Text, Clock | **Shows** (a variable, or Fixed text with its box), Typeface (brand fonts plus the layer's own), Size, Colour (brand colours, or type `#RRGGBB`), Weight, Lines, Overflow, Align |
| Shape | Fill (same colour box; a gradient fill says choosing a colour replaces it), Corner radius |
| Image | Source, Fit, Align, Vertical align |
| Every layer | X, Y, W, H, Opacity |

The **Variables tab** is the Variables panel (above).

**Not in this slice.** Slice 3: per-layer motion (each layer's own in / out)
and a Play preview of it. Slice 4: stroke, ellipse and line shapes, text
outline and shadow, size to content, text that shrinks to fit, and image
import into the design.

**#1087** (Slice 1 review follow-ups): N1, N2, N4, N5 and (c) were fixed at the
start of this slice, and N3 (designs listed twice in the builder) with the
Designer; (b) (a design's own asset folder after a core restart) moves to
Slice 4.

Code: `GraphicsDesignerControl` (+ `.Layers` / `.Inspector`) over MediaCore
`GraphicsDesignerModel` (rows, drop targets, inspector), `GraphicsSnap`,
`GraphicsPreviewSamples` and the `GraphicsTemplateEditor` structure edits;
`GraphicsDesignWorkflow.OpenInDesignerAsync` / `OpenForRowAsync` (over `DesignForRow` / `IsEditing`) decide what Open in
Designer and Operate's "Open in Design" do.

## Motion (Graphics Designer slice 3)

Select a layer and open **Motion**. Use **In** and **Out** in the strip beneath the
canvas to choose which move you are editing. **Show motion** opens the strip when it
starts collapsed on a smaller display. The strip lists layers in paint order, back to
front. Select a bar to select its layer. Drag the bar to change its start or drag an
end to change its length. Timing snaps to 50 ms, or 10 ms while holding Ctrl. The arrow
keys move the selected bar by 50 ms and Shift+arrow changes its duration.

| Move | What you see |
|---|---|
| Cut | The layer switches at its start time. |
| Fade | The layer changes opacity in place. |
| Fade + rise | The layer fades as it rises into place. |
| Slide | The layer enters from the chosen side. |
| Wipe | The layer is revealed from the chosen edge. |
| Grow | The layer scales from one edge. |
| Pop | The layer grows from its centre, overshoots slightly, and settles. |

**Starts at**, **Takes**, and **Feel** set the delay, duration and easing.
Slide, Wipe, and Grow have direction controls. Fade + rise and Slide have a
travel-distance control. **Moves with the graphic** restores the layer's
inherited motion. Per-layer moves are masked by their parent's area. Each
phase must finish within 3.0 seconds, and up to 16 layers can have their own
motion.

**Out mirrors In** starts on and makes the Out view read-only. Turn it off to
edit Out separately. **Build left to right**, **Everything fades**, and
**Bar first, words after** fill the strip. You can retime individual bars after
applying a preset.

**Play preview** plays In, holds one second, and plays Out. **Play In** and
**Play Out** rehearse one phase. These one-shot previews stop on an edit,
sample switch, or Undo. **Loop** keeps playing and restarts on edits. All
four controls use the private Designer canvas. Program and the Studio Preview
bus stay as they were.

**Save to playlist** pins the design version on its row. **Show** in Operate
uses the latest saved pin even if the row already has an instance. A row's own
**Motion** makes the whole graphic move as one. With row Motion unset, a
design row uses its saved motion and a built-in row uses the default fade.
The instance leaves air when its last layer finishes. Show during Out and
Play Out during In reverse the current move from its current position.

### Acceptance 3

1. Open a lower-third design. On **Plate**, choose **Wipe**, set **From left**, and set **Takes** to 0.35 s.
2. On **Name**, choose **Fade + rise** and set **Starts at** to 0.20 s.
3. Press **Play preview**. The plate begins before the name in the private canvas.
4. Press **Save to playlist**. Go to **Operate** and press **Show** on that row.
5. Confirm the plate wipes in first on Program and the name starts 0.20 s later. Confirm the instance leaves air after its last layer finishes Out.

## Show vs Cue

**Show** puts the graphic on Program now: the row's native instance is created
or refreshed, cued, and shown from that cue in one step. **Cue** stops
after the cue: the graphic appears in the Preview composite and rides the next
Take, so the operator can stage a lower third with the next scene and have both
land together. A graphic on Preview offers Show (now) or Remove; a graphic on
Program offers Hide. The strip shows which is which from core truth.

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
| Clean | none | none | Shift+F11 | `PlayOutOthers`: plays every Program graphic out; Shift+F12 is already Clear all |

Cue (`graphics.layout.cue`) selects the scene to Preview through the same
`SelectScene` path as the Scenes page and `scene.select`, then does Cue for
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

## Names list (one row, many people)

A lower third built in Design can carry a **names list**: one line per person,
`Name, Title` (a two-column CSV pastes or imports straight in; a `name,title`
header is skipped; quotes are stripped). The row saves as a Title-kind row with
`variants[] { id, name, title }`; Names from Sources is off for a list because
the list is the binding. In Operate the row expands like a Guest row, one entry
per line with its own Show / Cue / Hide, and the row's bare hotkey (or
`graphics.playIn <rowId>` with no sourceId) shows the line **after** whichever
line is on Program, wrapping to the first. `sourceId` for a line is
`variant:<id>`; ids are slugs of the name and survive a re-save so a line on air
keeps its instance. Hide on the row with no sourceId takes every line of that
row off Program.

## Clocks and countdowns

A "Clock or countdown" graphic built in Design stores its settings on the row
as `clock` (`mode` wall|countdown, `format` hms|hm, `finish` hold-zero|hide,
`durationSeconds`; zone is the computer's local time). When the row is cued or
shown the service sends `graphics.clock.control configure` to its instance
(with the instance revision and its current clock revision, 1 on a fresh instance;
the saved settings change; a template without a clock refuses harmlessly). A
countdown never starts by itself: Operate shows the live readout and Start /
Pause / Resume / Reset chips under the row, which call
`graphics.playlist.clock <rowId> <action>` (refused until the row has an
instance, i.e. after Cue or Show). Start never shows the graphic and Reset never
hides it.

## One truth for Preview and Program (#1072)

Before this change a lower third could show in Preview and vanish on Take:
Preview rendered the cued scene's graphics while Program keyed a separate legacy
overlay (`key:lower-third`) under its own enabled/suppressed flags, and the Take
copied routes, overlays and captions but never graphics.

Now Cue / Show / Hide drive native **graphics instances** only
(`native/src/graphics/GraphicsInstances.cpp`), one instance per playlist entry
(`playlist-<rowId>` or `playlist-<rowId>-<sha256(sourceId)[:12]>`), and the core
makes the Take carry them:

- `graphics-instance-cue` accepts `rideTake` (default true). A Rehearsal
  automation cue sends `rideTake=false`.
- `MediaCore::beginTakeTransition` calls `GraphicsInstances::promoteCuedForTake()`:
  every cued, ready, `rideTake` instance is shown from its cue and its cue is
  cleared, so it leaves Preview and does not ride the next Take.
- `graphics-instance-trigger action=show target=cue` (Show from Preview) also
  clears the cue. `action=uncue` (Remove) clears exactly that instance's cue and
  touches nothing on Program.
- The lower-third family authority flips to `instances` only after the first
  Show is accepted, so the legacy key keeps working until a playlist graphic
  has actually rendered. The Studio "LT in / LT out" button and
  `graphics.lowerThird.toggle/set` route into the Guest row for the current
  Program subject; `automation.lowerThirds.set` keeps driving the native
  automation rules, whose cues and shows appear in the strip as "auto".

## Where things live

| Thing | Code |
|---|---|
| Rows, entries, strip model, Show/Out sequence, auto-out timer, shot layout cue | `native-shell/CoreVideoPro.MediaCore/Services/GraphicsPlayoutService.cs`, `Models/GraphicsPlaylist.cs` (`GraphicsPlaylistRow`, `ShotLayout`) |
| Persistence (additive `Playlist` and `ShotLayouts` on the show document, schema 2) | `Services/GraphicsDocumentStore.cs`, `%LOCALAPPDATA%\CoreVideoPro\graphics\default.json` |
| Default playlist and preset layouts from the curated templates | `Services/GraphicsWorkspaceLibrary.Playlist.cs` (`DefaultPlaylistFor`, `DefaultShotLayouts`, `SaveShotLayoutsAsync`) |
| ViewModel seam (strangler partial; scene select + routed guests for layouts) | `ViewModels/StudioViewModel.GraphicsPlayout.cs`, routing in `StudioViewModel.StudioLowerThird.cs` |
| Control API and state | `CoreVideoPro.Control/ControlActionRegistry.cs`, `ControlState.cs` (`GraphicsStrip`, `GraphicsPlaylist`, `GraphicsLayouts`, `LowerThirdOnAir`), `WinUI/Services/StudioControlSurface.cs` |
| Design surfaces | `Controls/GraphicsBuilderControl.cs` (builder, Your designs, Open in Designer, Cues & automation), `Controls/GraphicsDesignerControl*.cs` (Designer), `Controls/GraphicsWorkspaceControl*.cs` (Cues & automation), `Services/GraphicsDesignSurfaces.cs` (one surface at a time); decisions in MediaCore `GraphicsBuilder`, `GraphicsDesignWorkflow`, `GraphicsDesignerModel`, `GraphicsSnap`, `GraphicsPreviewSamples` |
| Motion | `GraphicsDesignerControl.Motion*.cs`, `GraphicsTemplateEditor.Motion.cs`, `GraphicsMotionModel.cs`; native units, phase clock and atlas layout in `GraphicsMotion.*`, drawn by the Windows D3D11 compositor |
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

### OSC and Stream Deck

Every action above is reachable over OSC with no extra wiring: the OSC router
maps `/cvp/<action with dots as slashes>` to the registry id, so a Stream Deck
running Bitfocus Companion (or any OSC sender) gets:

| Button | OSC address | Arguments |
|---|---|---|
| Show the guest lower third for whoever is on Program | `/cvp/graphics/playIn` | `guest` |
| Show a specific guest | `/cvp/graphics/playIn` | `guest` `zoom:<id>` (or `media:` / `capture:`) |
| Cue the title to ride the next Take | `/cvp/graphics/toPreview` | `title` |
| Hide one row | `/cvp/graphics/playOut` | `title` (or an instance id) |
| Next name in a names list | `/cvp/graphics/playIn` | `panelists` (the list row's id) |
| Start the break countdown | `/cvp/graphics/playlist/clock` | `live-bug` `start` |
| Hide everything | `/cvp/graphics/playOutAll` | |
| Fire a shot layout | `/cvp/graphics/layout/cue` | `<layoutId>` |

Feedback arrives under `/cvp/state/graphics/onProgramCount`, `/cvp/state/graphics/onPreviewCount`, `/cvp/state/graphics/automationMode` and `/cvp/state/graphics/automationArmed`. Actions are
loopback-only by default unless the OSC exposure policy opens them to the LAN.

## Hotkeys

F1..F12 unmodified: Show of the row with that hotkey (a Guest row plays in
the entry for the current Program subject). Shift+F1..F11: cue the shot layout
with that hotkey (`graphics.layout.cue`). Shift+F12: Clear all. Keys are
ignored while a text field has focus and are active only on the Studio and
Graphics tabs (`GraphicsHotkeyRouter`, pure and unit-tested). Assign keys from
the inspector (rows) or the Edit layouts flyout (layouts); one key belongs to one
row or layout and moves when reassigned.

## Rules to keep

- Never add a Show or toggle on the Graphics tab without its Hide / Remove in
  the same place while the graphic is live (owner, 2026-10-31).
- The strip derives from the core's instance state only. A chip that is not
  there is a bug in the mapping, not a reason to add local state.
- Do not flip lower-third authority to instances before an accepted Show.
- Apply a saved row's pinned version and motion to the cue copy before Show.
  Keep the previous Program revision until the new cue is ready.
- Keep the private Play preview request and its clock out of the Program
  render plan and instance state.
- Instance capacity is eight per bus: entries are created lazily on Show /
  Cue and hidden ones are removed; a 10-guest roster never pre-creates.
- Outputs drill: unaffected (it reads `graphics.coreEpoch` only); never rename
  an action id, the Companion manifest is generated from the registry.

## Acceptance (owner at the keyboard, installed build)

Designer acceptance 1 (spec §4): Design → New design → Lower third blank →
Open in Designer; Variables tab: add `location` (Text, 40), optionally set its
Sample value and Apply (the preview then shows it); Layer tab: Add Text, then
Shows: Variable `location`, drag it into place; **Save to playlist · F-key**.
In Operate select the row: the inspector has a **Location** box; type the
location, then Show (no Enter needed): name and title come from Sources (Guest
row; a source must be assigned on the Sources page for the row to offer Show)
and the location is the typed text. The Lower third blank has no room under its
title line (168 px tall, the title ends at 136), so place the location line on
the title row, e.g. X 628, Y 94, W 400, H 42, Size 28, Align right; give
`location` a Sample value first so the Designer preview shows it. Saving to the
playlist again keeps the location typed on the row.

Each from a cold open in under 3 clicks and 15 s: name a person on Program,
edit a lower third's text, add a logo or title, see what is live versus staged
and clear it. For every Show on the tab, find and use its Hide within 5 s
while it is on Program.

## Designer content (Slice 4)

Use **Shape** to add a rectangle, ellipse or line. In the **Layer** inspector,
set the stroke on rectangles and ellipses or choose the line's stroke and
diagonal. Text layers can use an outline and shadow. The shadow is drawn
behind the outline and fill. Set a minimum font size to let long values
shrink before the selected overflow behavior applies.

Enable **Auto width** for a group or plate and set its minimum and maximum
widths. The renderer measures the resolved text and other content each time
the graphic is prepared. A plate follows the content around it and is excluded
from its own measurement. The Designer's sample values are useful for checking
how a long name will fit.

Use **Image…** to import PNG, JPEG or SVG into a design. The builder logo
picker also accepts SVG. Static SVG artwork becomes a transparent PNG at
twice the layer dimensions, capped at 4096 pixels per side. Files with active
content or external references are rejected. Save to playlist pins the
revision and its assets. Operate installs that revision before showing it,
including after a core restart.

For Acceptance 4, import a transparent SVG logo into a lower third and
inspect it on the private canvas. Save to playlist, reopen the design, then
show the row from Operate. Check the logo's size and transparent edges at
1080p on Program.

## Sources columns and names lists

In the Designer's **Variables** tab, select an extra variable such as
`location` and choose **Add Sources column**. In Sources, expand **Graphics
columns** to see the values by slot. Inspect an assigned slot, choose
**Graphics columns…**, enter its values and select **Save values**. Values
stay with the slot when its source changes.

Save the design to the playlist. In Operate, enable **From Sources** on the
Guest row and choose the column under **Location source**. The typed Location
on the row remains the fallback for a missing or invalid slot value. Show
uses the value for the selected guest. The next Show picks up saved changes.

In Operate, open **Names list…** on the Guest row to enter or import its list.
A names list can use a CSV header such as `name,title,location`. Use the
variable ID for each extra column. Quote cells containing commas and double
any quotes inside a quoted cell. A missing or bare empty cell falls back to
the row value. A quoted empty string clears a text variable. Supported
extra values are text, numbers, colours and on/off. Enter `true` or `false`
for on/off. The old one- and two-column lists still work.

## Local data tables and leaderboards

In Design, choose **Data table…** and open a UTF-8 CSV or JSON file. CSV
uses a header row. JSON uses an array of objects with the same columns in
every row. Choose a unique row key or keep file order, select the columns
to display, and set the visible row count. Selecting a numeric sort column
adds a rank and sorts high to low. Equal scores use the row key for order.

Create the design, check it on the private canvas, and use **Save to playlist**.
In Operate, Cue or Show the saved table as you would a title. **Load CSV /
JSON…** stages a new revision. **Publish data** saves that revision for the
next Cue or Show and asks an on-air instance of the same design version to
update in place. A cue already on Preview keeps the revision it captured.
If preparation fails, Program keeps the last accepted pixels. Use the
existing Hide action to take the table out.

A table displays up to 12 rows and eight columns, including the rank column
on a leaderboard. The first layout uses equal-width data columns, a narrower
rank column, one line per cell and ellipsis. An encoded display row is capped
at 4096 UTF-8 bytes. The show stores the latest staged source and the
materialized rows pinned to each playlist entry. Loading a file has no
effect on Program by itself.

## Later (the spec's phases)

Per-tile names in Tiles (#1073, native placement per tile rect), clocks and
sheets as data sources, in/out transition choices in the builder, the full visual
designer (Graphics Designer slices 2–5: Designer surface, per-layer motion,
shapes/outline/size-to-content/SVG import, Sources columns; slice 1 "Designs"
above), crawls, SDI
key/fill. Shot layouts landed in slice 2, the template-first builder in slice 3
(above). NDI fill+key landed as a second NDI source carrying the On Air graphics
alone, straight alpha: [ndi-graphics-key.md](ndi-graphics-key.md).
