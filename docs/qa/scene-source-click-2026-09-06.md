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

Post-fix live-video verification is **pending**: rejoin returned `Zoom sign-in expired. Sign in with Zoom and try again.` Testing was paused when the user's next message indicated testing might not be allowed at that moment. No successful post-fix live click/drag pass is claimed.

When testing resumes, confirm plain clicks preserve automatic/fixed routes, drag and resize preserve automatic routing, and explicit source selection still changes the intended layer.
