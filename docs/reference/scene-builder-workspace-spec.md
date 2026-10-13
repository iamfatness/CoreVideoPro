# Scene Builder workspace redesign

Design proposal for [#879](https://github.com/iamfatness/CoreVideoPro/issues/879), requested on October 12, 2026. Replace the repeated source-property cards with a scene canvas, an ordered Sources list and a selected-source inspector. CoreVideo Tiles must participate in this workspace and render beside ordinary sources. Implementation priority remains in `../BACKLOG.md`; this proposal does not change the live application.

## Operator workflow

Choose a scene, add or reuse a source, arrange it on the canvas, and adjust the selected source. The Sources list answers what is in the scene and which layer appears in front. The inspector answers how the selected source looks. Save changes stores the scene; Take applies a captured Preview revision to Program. Neither selecting nor editing the on-air scene changes Program implicitly.

Use the familiar OBS ordering convention: the first source row is the frontmost layer. Drag rows to change order; keyboard and accessible Move forward/back actions provide equivalent operations. Visibility and transform-lock controls belong on each row. This convention follows the [OBS Sources Guide](https://obsproject.com/kb/sources-guide). It does not require copying OBS's entire interface.

## Current shortcomings

Layer forward/back, remove and opacity controls already exist inside the current property cards. The problem is discoverability and a missing unified layer stack, not absence of every underlying operation. The canvas indexes its selection and drawing by route position, which is unsuitable as persistent identity once rows are reordered.

Tiles has a separate functional gap. `ReconcileDynamicGalleryRoutes` clears ordinary routes, while `HasPreviewSlotEditors` tests only `PreviewCanvasLayers.Count`. The page hides a valid zero-route Tiles scene even when its native composed Preview is available. The current gallery sync also deliberately sends no ordinary routes. Mixed scenes therefore need an explicit model for authored layers alongside the native wall; changing the empty-state predicate alone does not provide complete layering.

## Workspace layout

| Region | Purpose | Scrolling |
|---|---|---|
| Header | Workspace title, Undo/Redo, Save changes | Fixed |
| Left dock | Scene selection above the ordered Sources list; Add, duplicate and remove at the bottom | Each list scrolls vertically inside its bounded region |
| Center | Selected scene name, Preview/draft status, canvas, fit/guides/snapping toolbar and explicit Take | Fits available space; canvas panning only after intentional zoom |
| Right inspector | Properties of the single selected source | Independent vertical scroll, with Transform, Appearance and Source sections |

The canvas receives the remaining width and height. No page-wide horizontal scrolling is allowed for routine building. At a narrow desktop width, the inspector becomes an explicitly opened Properties drawer while the Sources list and canvas remain usable. Test 1366×768, 1920×1080 and 1024×768, plus 125%, 150% and 200% scaling; measure effective viewport size rather than assuming physical pixels guarantee space.

Keep Add source, scene selection, Undo and Take reachable without scrolling through properties. Display the complete source name in a tooltip and the inspector when a compact row truncates it. Do not shrink text indefinitely to fit more cards.

## Sources and selection

A source row has a stable scene-item ID, operator-authored name, type, visibility, transform-lock state and concise availability indicator. A source alias is independent of the underlying feed's changing participant name. Reusing a guest, camera or media feed creates a scene instance, not a second capture or SDK subscription.

Canvas selection, source-row selection and inspector selection refer to the same stable ID. Reordering preserves selection and focus. Roster/media refresh updates facts without rebuilding rows, closing an open menu, changing the selected item or resetting scroll position. Missing sources remain visible in the list with an explicit status and the existing dropout policy.

Add source is a searchable type picker followed by a feed picker. The catalog includes existing show inputs, Zoom guests/roles, cameras, screen/window capture, images/media, text, color backgrounds and CoreVideo Tiles. It separates an existing source from creating a new source. Removing a scene instance does not disconnect a feed still used elsewhere.

Use a common scene-item collection for authored visual layers. Each item carries identity, reference/type, front-to-back order, visibility, transform, opacity and editor lock. Map that collection consistently to persistence, editor overlays and the native compositor; an inspector setting must not be UI-only.

## CoreVideo Tiles as a layer

Show the wall as a named entry in Sources. Selecting it highlights its wall bounds and opens wall properties. The Source section exposes membership; Appearance exposes spacing, margins, background and borders. Automatic members reflow inside the wall. Their generated draw commands are not ordinary authored scene routes.

Allow a logo, title, background or camera instance above or below the wall. Preserve explicit authored layers when compiling a Tiles scene; suppress duplicate generated participant routes by their ownership/type, not by clearing every route in a scene containing Tiles.

The native contract must implement the wall's root bounds, visibility, opacity and shared layer order before those controls ship. Audit existing fields: `MediaCoreTilesLayerWire` currently carries wall identity/order, membership and tile styling, but does not express a general wall-instance transform. Add the required typed fields and native behavior rather than stretching a fake canvas preview. Source grading continues to operate on the underlying source; it must not silently become a wall-wide grade.

Keep the first implementation to one native Tiles wall per scene unless independent wall-instance identity and lifecycle are implemented and verified. Multiple walls, arbitrary nested groups and nested scenes are subsequent capabilities, not mockup behavior that implies current support.

## Canvas and inspector interactions

Use the native composed Preview as the canvas picture. Editor overlays add selection, handles, guides and hit testing; they never become output pixels. A valid scene remains editable with zero ordinary routes. An empty scene displays a usable empty canvas and Add source, rather than disabling the builder.

Provide direct move/resize, aspect-constrained resizing, fit/fill/center actions, snapping, arrow-key nudge and a scoped reset. Transform fields use output-canvas pixels with conversion to the existing normalized representation. Crop settings identify their source-relative units. Continuous adjustments use synchronized sliders and exact entry according to the [operator control specification](operator-adjustment-controls-spec.md). Invalid entry preserves the previous value and explains the valid range.

A pointer gesture is one Undo action. Escape cancels it; lost capture resolves or cancels deterministically. Keyboard alternatives support source selection, order, visibility, lock and transforms. Labels and visible state accompany icons; color alone is not the state signal. Locked sources can be selected and inspected without accidental transform edits.

## Scene state and persistence

Separate the saved scene definition, an editable Preview draft, and the snapshot currently on Program. Save stores a draft without changing the on-air instance. Take publishes one captured draft revision; later edits do not race that revision onto Program. Preserve existing scene IDs, show-input references, authored lower-third text, grading, framing and source dropout settings through migration.

Migrate an existing gallery into a wall item without generating duplicate guest routes. Preserve the previous visual order of ordinary scenes when converting between ascending native z-order and the frontmost-first list. Add visibility/lock defaults that reproduce old output. If a conversion cannot preserve a scene, retain the original and show a recoverable explanation.

## Implementation boundaries

Extract a focused workspace view model, stable scene-item model, selection/history controller and inspector views. Do not grow the existing `StudioViewModel` into another editor implementation. Adapt existing scene commands, canvas interactions and source-bus ownership behind those types.

Establish native mixed-wall ordering and persistence before exposing unsupported wall transform controls. Then replace the page layout and bind its source list, canvas and inspector to that same model. Migrate old scenes and validate all affected output paths before installed acceptance. This is a dependency sequence for #879, not a separately ranked backlog.

Preview and source thumbnails must reuse published surfaces. Do not render every saved scene at full rate or add CPU readbacks for the scene list. Cached thumbnails identify their scene/revision; only the edited Preview must represent the current draft. Coalesce intermediate edits, preserve the final value, and keep UI refresh/device work outside production locks.

## Acceptance

| Operator task | Required result |
|---|---|
| Open a zero-route Tiles scene | Moving native composition appears in the editor; no erroneous empty-scene panel |
| Place a logo and a camera over Tiles | Native Preview, Program, recording and webcam show the same intended ordering and geometry |
| Reorder, hide, lock, duplicate and remove | Picture matches the ordered list; stable selection/focus; changes survive save/reopen |
| Build an empty scene | Add source works immediately; canvas remains visible |
| Edit a scene currently on Program | Preview changes; Program remains the captured on-air instance until explicit Take |
| Resize or use high DPI | Essential actions remain reachable; no page-wide horizontal scrolling or unreadable controls |
| Refresh a live roster while editing | No selection, focus, menu or scroll jumps; Tiles membership updates without duplicate source routes |
| Drag and type exact values | Slider/field agree, precision survives, one gesture can be undone, invalid input is recoverable |
| Load upgraded saved scenes | Existing source assignments, visual order and appearance are preserved |
| Run the matched media workload | No quality reduction, larger production buffer, added pixel readback or output-loss waiver; retain failed evidence |

The interactive HTML review demonstrates layout and sample interactions only. Its media is illustrative, its Program action is simulated, and it is not installed functionality. Use the critique to settle panel placement, source-row readability and the workflow before implementing the native/editor changes.

## CoreVideo Tiles inspector review

The October 12 owner review accepted the workspace mockup and requested rendered Tiles source controls. The prototype now separates wall controls into four tabs:

| Tab | Controls and expected feedback |
|---|---|
| Layout | Tile aspect ratio, spacing and outer margin as percentages of wall width, maximum visible tiles, and a sample roster change that demonstrates reflow |
| Style | Clean/Rounded/Broadcast presets, tile shape, corner radius, border width/color, wall background transparency/color, opacity, glow size/color and scoped reset |
| Members | Membership policy, included feeds, per-wall Never show exclusions, selected-member left/right crop, pin position/size and return to automatic layout |
| Transform | Position and size of the whole wall, fit and center actions, separate from individual member framing |

Continuous controls pair sliders with exact entry. Color controls pair a swatch with editable hex. Member inclusion, cropping and styling update the illustrative composition immediately; wall styling preserves member crop and inclusion. Selecting another source restores its source-specific inspector. Compact layouts expose the same controls through a closable Properties drawer.

The membership modes use six sample feeds, not live Zoom assignments. The pin demonstration gives automatic members the largest space remaining around the first pin; this is not validation of the full native multi-pin algorithm. Pin coordinates in this proposed inspector are wall-local pixels. Implementation must convert them to the existing normalized canvas override contract and preserve current saved overrides during migration. Root wall transforms remain a native contract change. Native animation, custom aspect entry, manual slot assignment/reordering, imported/live wall backgrounds and the complete glow model still require implementation and review; this mockup does not claim to exercise those behaviors.
