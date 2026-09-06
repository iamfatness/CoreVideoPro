# Scene source click regression — 2026-09-06

## Reproduced before the fix

Using the actual Scenes page in the running test meeting, clicking Source 4 once changed its route summary from Anika to Jamal and changed the corresponding image. No drag or source selection was performed.

Two paths combined: the canvas committed a route on every pointer release, and SceneCanvasLayerViewModel represented an automatic source with the first roster participant when no fixed ID existed. Applying that representation converted ActiveSpeaker into Fixed/Jamal.

## Repair

- A click selects the canvas layer without committing a route. A three-DIP movement threshold starts an actual drag; drag updates and the final release still commit edits.
- Automatic and unassigned sources no longer fall back to the first participant or capture device. Missing fixed references retain their identity.
- The source picker shows an explicit automatic/empty entry. Explicit source choices still pin a source; refresh callbacks and transient null selection cannot change it.
- Unchanged option lists keep their identity; changed options notify bindings. Canvas refresh preserves the selected layer.

## Validation status

Seven model regressions added; two reproduced the original fallback before the fix. Full Release WinUI suite: **821 passed**. The corrected app was published to `artifacts/live-verified/win-unpacked` and launched.

Testing subsequently resumed. A live eight-guest check found another picker defect:
the dropdown displayed Input 07 / John while the route and image remained unchanged.
The handler now takes the actual `SelectionChanged.AddedItems` option and the
explicit `x:Bind` template owner instead of relying on `SelectedValue` event
ordering and inherited `DataContext`. A regression covers the Input 07 selection.

The revised Release x64 WinUI suite passed **826 tests**. In the live candidate at
12:30:45 Eastern, selecting John changed the route summary and first native preview
tile to John; subsequent observations showed video continuing to advance. A plain
Source 4 click preserved routing, and a small Source 4 drag and reverse drag did
not pin the automatic source to Jamal. The test did not press Save/Update scene.
Dedicated resize-handle verification is still outstanding.
