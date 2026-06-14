# Sprint 1 Goal — Join Zoom & See Live Feeds (self-driving)

Hand this to Codex (Track B, on the dev machine) as a goal, not a checklist. It
is written to iterate the whole slice to "done" on its own, pausing only for the
human-in-the-loop checkpoints it genuinely needs.

---

**North-star goal (this is the definition of done — keep iterating until ALL of
it is true):**
In the CoreVideo Pro dev Electron app, a user joins a real Zoom meeting and sees
**live individual participant video tiles**, with the **roster** and the
**active speaker** correct against the real call. First frame in under ~1s; ≥3
participants render simultaneously; runs end-to-end in the app, not just tests.

**Treat this as a goal, not a checklist.** Loop autonomously: identify the next
gap between current state and the goal → implement it → build → test → verify →
commit → repeat. Do not stop after one step. Keep going until every exit
criterion above passes.

**The path is decided** (see `06-decision-zoom-capture-path.md`): use the
**vendored engine** (`native/zoom-engine/`, `corevideo-zoom-engine`). Do **not**
advance the parked `ZoomMeetingSdkAdapter`.

**Work to do, in dependency order (self-sequence as needed):**
1. Build `corevideo-zoom-engine` on the dev machine:
   `-DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DZOOM_SDK_DIR=…`. Resolve any
   include/link issues from the vendoring.
2. Frame path: read I420 from the engine's shared memory (port the plugin's read
   loop from `zoom-source.cpp` `output_video_from_shared_memory`), downscale +
   convert to RGBA thumbnails (~320×180, 10–15fps/participant), emit over the
   `onZoomVideoFrame` channel → the renderer's `<canvas>` tiles already consume
   it (`src/ParticipantVideoCanvas.tsx`, `src/engine/zoomVideoFrames.ts`).
3. Join wiring: have `MediaCoreSupervisor` (`desktop/mediaCoreClient.ts`) spawn /
   supervise the engine; translate `ZoomMediaSpineSessionController` join/sync/
   leave into the engine's `ZoomEngineClient` IPC (`init`/`join`/`subscribe`);
   map roster + active speaker into `ZoomMediaSpineNativeSnapshot`.
4. Verify against a real meeting in the Electron app; iterate on whatever's wrong
   (no frames, wrong colors, wrong roster, latency) until the goal holds.

**Guardrails (hold these on every iteration):**
- Default in-container build stays green: `cmake -DCOREVIDEO_STUB=ON … && ctest`,
  `npm run typecheck`, `npm run test`. The engine is dev-gated only.
- Don't edit the TS protocol files without mirroring native + keeping
  `ContractParityTest` green.
- Commit per working increment; keep an open **draft PR** updated.

**Stop and ask the human only for:**
(a) a live test meeting to join, (b) confirmation the Zoom **raw-data privilege**
is enabled on the app (without it you'll join but get no per-participant video —
surface this immediately if frames are empty), (c) a visual screenshot check that
the video looks correct, or (d) a genuinely ambiguous architectural fork.
Otherwise, keep iterating.
