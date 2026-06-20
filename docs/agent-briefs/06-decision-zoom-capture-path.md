# Decision: Zoom capture path → vendored engine

**Status:** Decided (2026-06-14). Applies from Sprint 1 onward.

## Decision

The **vendored standalone Zoom capture engine** (`native/zoom-engine/`, built as
`corevideo-zoom-engine`) is the **primary and only active** Zoom capture path for
Demo 1 and the MVP. It is the proven engine from the shipping CoreVideo plugin —
it already joins Zoom and delivers per-participant raw I420 video/audio over
shared memory, on both macOS and Windows.

## Why

- Proven in production in prior CoreVideo work — lowest risk to "join + see feeds".
- Cross-platform (mac + Windows) vs. the adapter's Windows-only state.
- Crash-isolated separate process, matching the `MediaCoreSupervisor` model.
- Lets Demo 1 land in days, not weeks — reuse over rewrite.

## What this means for the existing `ZoomMeetingSdkAdapter`

Codex's in-core `ZoomMeetingSdkAdapter` (`native/src/modules/ZoomMeetingSdkAdapter.cpp`,
`COREVIDEO_WITH_ZOOM`) is **parked, not deleted**:

- Keep the code, keep it building under its flag, keep its tests green.
- It is **not** the capture source we wire for Demo 1 — do not invest further in
  it for now. Treat it as a deferred/experimental alternative for a possible
  future "capture inside the compositor process" design (Sprint 3+, only if a
  concrete need appears).
- Do not add new features that assume the adapter is the capture path.

## Action items

**Codex (Track B) — Sprint 1**
- Build `corevideo-zoom-engine` on the dev machine against the Zoom SDK.
- Implement the engine→core→renderer frame path: read I420 from the engine's
  shared memory, downscale + convert to RGBA thumbnails, emit over the frame
  channel. Stop further `ZoomMeetingSdkAdapter` work.

**Agent A (renderer/desktop) — Sprint 1**
- Point the join path at the engine: spawn/supervise `corevideo-zoom-engine` via
  `MediaCoreSupervisor`, drive `init`/`join`/`subscribe` through the engine's IPC
  (mapped from `ZoomMediaSpineSessionController`), and feed
  `onZoomVideoFrame` from the engine's frames.

**Reconciliation**
- The spine's `ZoomMediaSpineServiceTransport` / `syncZoomMediaSpine` seam is
  implemented by translating spine requests into the vendored engine's
  `ZoomEngineClient` calls (see `00-native-build-plan.md` mapping). The TS spine
  stays; only the native transport binds to the engine.

## Revisit when

A clear need for in-process capture (e.g. eliminating the shared-memory hop for
the GPU compositor, or a platform the plugin engine can't serve) — at which point
re-evaluate the adapter against the vendored engine with data.
