# Scene Builder approved-design parity audit

October 13, 2026. Evidence for [#879](https://github.com/iamfatness/CoreVideoPro/issues/879) and [PR #880](https://github.com/iamfatness/CoreVideoPro/pull/880), not a separate work queue.

The owner rejected the installed workspace because both controls and the overall design differ significantly from the approved HTML. Removing the canvas size limit does not resolve that rejection. The feature remains incomplete.

## Reference and candidate identities

The reference is `artifacts/design-review/scene-builder-review.html` plus its `tiles-controls.js` extension, served at `http://127.0.0.1:8767/scene-builder-review.html`. Preserve that reference; do not modify it to accommodate missing implementation.

Installed candidate: `beta-2026-10-12-43356cb`. Prepared, uninstalled candidate: `beta-2026-10-13-811bfca`. The corresponding main-based feature source inspected here is `94ac009`. The prepared candidate fixes some layout problems, but has the following unresolved gaps. A cancelled elevation attempt is not an installed correction; there is no outstanding UAC prompt from that attempt.

## Control and interaction comparison

| Approved interaction | Implementation evidence | Required correction or qualification |
|---|---|---|
| Searchable Add source catalog, then choose/create/reuse underlying source | `SourcesPage.xaml.cs:PopulateAddSourceFlyout` offers Tiles plus existing feed entries; separate overlay flyout | One searchable, categorized entry point; distinguish adding a scene instance from creating a feed. |
| Catalog includes Tiles, Camera/Zoom, screen share, image/logo, editable Text, Color background | The HTML catalog and source-specific text editor explicitly include these; the page has no authored text/color creation or editors | Implement actual scene-item adapters and persisted properties. Text/color cannot be declared illustrative simply because the mock media is simulated. Verify native output for both. |
| Source names, source types and concise availability are readable in the layer stack | Rows have name/type; generic camera glyph used for several kinds; no explicit availability field | Accurate icons, live/missing availability and stable aliases; keep roster refresh from replacing editor selection. |
| Tiles selected-source header includes wall/member context | Header exposes basic source-kind label | Include actual membership context; distinguish configured intent from available/delivered feeds. |
| Layout grouped into Automatic layout and Wall capacity, with explanations of units and the tile ceiling | Most existing controls form an undivided stack, with extra animation/custom-aspect controls | Restore grouping, concise help and conditional advanced controls without replacing approved controls. |
| Aspect choices include all six reviewed shapes | Existing option collection needs explicit comparison | Verify 16:9, 4:3, 5:4, 1:1, 3:4, 9:16; additional custom choice must not clutter normal editing. |
| Style grouped into Quick looks, Tile edges, Wall background, Glow | Style currently mostly a long stack | Match hierarchy, separators and compact spacing. |
| Rounded-only radius and opaque-only background color | Controls remain displayed regardless of applicability | Show applicable controls and explain state; retain saved values while hidden. |
| Color swatch paired with editable hex | Hex and generic Pick buttons open color pickers | Display the current color on the clickable swatch and synchronize hex/picker. |
| Wall opacity in Style / Wall background | Opacity currently in common Transform | Put it beside transparency/background, synchronized with persisted wall opacity. |
| Reset wall style preserves membership, crops and placement | Existing handler resets wall fields | Verify scoped reset and undo against native Preview; state the chosen default consistently with Quick looks. |
| Member rows have inclusion checkbox, selectable name and Auto/Pinned/Excluded/Unused state | Current rows have Exclude checkbox and Eligible/Pinned/Never show labels | Separate inclusion from Never show; reflect native membership policy rather than a UI-only check. Preserve routed/eligible/manual semantics. |
| Selected-member Never show toggle | Separate Never show and Allow buttons | One stateful toggle in the selected-member framing section, separate from inclusion. |
| Crop sliders update the picture while adjusted | Bound values stage `GalleryTileCropLeft/Right`; OnApplyTileCrop commits them | Live Preview updates with one undo checkpoint per gesture. Preserve pin rectangle, z-order and membership. |
| Reset this tile crop | No dedicated control | Zero only the selected member's crop; preserve placement, visibility and other members. |
| Pin member toggle, conditional fields and Return this tile to automatic | Expander, percentage fields, Apply pin; ClearTilesOverride removes the entire override | State-based pin toggle and immediate geometry feedback. Unpin must clear placement while preserving crop. |
| Pin coordinates are wall-local pixels | Existing fields display normalized percentages | Convert wall-local pixel input to the actual native override contract, including moved/resized walls; preserve old saved pin intent. |
| Common Transform uses paired X/Y and width/height pixel fields | Current fields are four full-width sliders | Compact paired position/size controls, sensible bounds and validation; retain exact entry and direct manipulation. |
| Quick transform offers Fit, Center, Reset transform, Move to back | Current tab has Fit and Center; ordering buttons are above every tab | Restore all four scoped actions and place them in Transform; reset geometry without replacing feed/grade/appearance. |
| Fit/Fill/Stretch in Transform framing section | Fitting currently lives in ordinary Source tab | Match location and semantics; whole-wall transform stays distinct from per-member crop. |
| Ordinary Appearance offers opacity explanation and Reset opacity | Current tab adds border controls but no scoped opacity reset | Restore reset/help and group supported extra appearance controls below the primary adjustment. |
| Lock allows inspection but prevents property edits | Transform ContentControl disables editing; ordinary Appearance/Source need full audit | Apply a consistent lock guard to every source/member/property path, with a visible notice. |
| Sample roster-change action demonstrates reflow | Sample action is intentionally not a real meeting command | Treat only this explicitly sample-labeled control as a demo. Validate real roster reflow separately; no fabricated live participants. |

## Visual and workspace comparison

| Approved design | Candidate difference | Acceptance requirement |
|---|---|---|
| Workspace uses nearly the full available editing surface | SourcesPage is wrapped in the general Studio panel with additional margin/padding; global output controls consume substantial height | Review the entire window composition, not only canvas dimensions. Make scene building occupy the intended space while keeping meaningful live-output status/actions reachable. |
| Compact 62px header, 13px base typography and restrained hierarchy | Native header, parent shell, default WinUI controls and property spacing have different scale | Compare font size, weight, control height, padding, gaps and contrast at matched effective viewport/DPI. |
| Unified dark palette with mint selection/action states | Local panel colors coexist with default WinUI selection/control styling | Apply scoped workspace resources to buttons, sliders, lists, fields and tabs; retain visible keyboard focus and disabled state. |
| Scene list has recognizable miniature scene previews | Current scene rows show the same generic glyph | Use cached scene/revision previews or a truthful scene-specific visual, without adding full-rate scene renders or production CPU readback. |
| Bounded left dock has independent scene/source scrolling and compact action footer | Fixed 220px scene section and extra overlay action can dominate short windows | Compare short/tall viewports; keep source editing and actions reachable without nested scrolling surprises. |
| Flat inspector tab strip fits all four Tiles labels | Current candidate mounts the relevant PivotItems, but still uses WinUI Pivot/default spacing | Verify all labels visible and compare actual styling/spacing to the reference, not merely item names. |
| Bordered stage contains toolbar and footer with canvas dimensions/interaction help | Candidate toolbar and hint sit outside the picture border; dimensions/status arrangement differs | Reproduce the intended framed stage and its information hierarchy. |
| Preview/off-air badge and draft status are distinct | Candidate shows a single fixed PREVIEW editing line | Display truthful draft/saved/on-air state and avoid implying a save publishes Program. |
| Inspector has grouped sections, independent scroll and fixed Preview-draft footer | Candidate lacks that footer and much section structure | Restore those pieces; long advanced controls must not drown out primary controls. |
| Compact Properties drawer preserves the same controls | Width threshold is implemented but parity at DPI/short windows is unverified | Verify 1024x768, 1366x768 and 1920x1080 effective layouts plus 125/150/200% scaling, keyboard access and no routine horizontal scrolling. |

## Evidence required before the next installation handoff

Prior managed/native test results establish useful behavior but do not establish design parity. The large-canvas assertion and exact tab-label assertion cover only two narrow properties.

Compare the unchanged mockup and the running candidate at matched effective viewport sizes with the same scene/source selection. Capture the whole workspace and every applicable inspector tab, including conditional pin/radius/background states, the source catalog and compact drawer. Record each remaining deviation explicitly instead of claiming a match based on selected screenshots.

Exercise live crop, crop reset, pin/unpin, inclusion/exclusion, scoped resets, lock, undo/redo, save/reopen and Program isolation. Verify native composition for authored text/color and mixed Tiles layers across enabled output paths. Keep reference screenshots and candidate identities together. The existing successful owner bake belongs to its earlier binary; it is not qualification for subsequent changes.

Only package and request administrator consent after the control and visual comparison is complete. Do not ask the owner to install another partial correction as though it were the approved complete feature. Never report the candidate installed unless Setup completed and shortcut/webcam ownership and payload identity were verified.

## October 13 implementation and pre-install evidence

The audit above describes the rejected baseline, not the corrected implementation. The new pass implements the searchable six-category catalog; real persisted text/color sources; source types, availability and aliases; live crop and crop-only reset; separate inclusion/Never-show intent; pin toggle and wall-local pixel fields; crop-preserving unpin; compact exact transforms and scoped resets; grouped inspector sections, swatches and conditional controls; bounded docks, scene-specific layout schematics, a framed stage and Preview-draft footer. Existing advanced controls remain available. Locked editing and reordering are guarded. Numeric hydration does not author an unchanged route.

The unchanged reference and candidate were inspected at a matched 1426 by 893 viewport. Candidate captures are `artifacts/design-review/parity-layout.png`, `parity-style.png`, `parity-members-offline.png`, `parity-catalog.png` and `parity-color-native.png`. Native WinUI retains the app navigation/status header and native dialog/focus behavior; scene thumbnails are layout schematics, not the mock's illustrative camera pictures. These are deliberate platform/data differences, not missing primary controls. There is no fake meeting roster in the running app; unavailable-member controls disable correctly.

Managed suite: 1,640 passed. The compiled operator-settings probe passed crop/reset, pin pixel conversion/unpin, inclusion/exclusion, missing-member selection, lock, layering/history/persistence, pending title typing across refresh/focus loss, Program isolation and compact Properties layout. Evidence: `workspace-parity-tests.log` and `workspace-parity-operator-settings.json` in the design-review artifact directory. Earlier hands-on title editing exposed a reverted edit; the corrected root editor, scoped commit and media-catalog draft preservation now pass the compiled probe and the hands-on edit/save/relaunch check. Actual native Preview displayed COREVIDEO REVIEW and a newly authored color layer; visibility removed the text and locking disabled its fields. Program remained Speaker + Slides.

Pre-install tests used a temporary publish and preserved/restored the owner's fresh scene, roster and layout files. Native rendering binaries were not changed in this pass. Installation identity, webcam ownership and installed screenshots must be recorded separately. A new live SDK bake, receiver cadence measurement and full physical DPI matrix have not been completed; previous successful owner bakes are not evidence for this revised build. Owner visual acceptance remains open.

Final managed rerun: 1,640 passed. The immediately preceding run and targeted telemetry recheck failed the existing accepted-send watermark assertion; unchanged rerun passed. Failed evidence is retained and tracked as #881. It is not attributed to Scene Builder. Locked Move-to-back and drag reordering are now guarded alongside exact transforms.
