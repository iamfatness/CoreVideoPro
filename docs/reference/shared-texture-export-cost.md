# Creating a shared-texture export costs a D3D device: never on the render thread (#724, 2026-10-01)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Program lost two delivery slots whenever something new was first exported: a clip taken to
Program, a guest joining, a capture device connecting, a stream or recording starting.

- **Cause:** `D3DDecoupledExport` owns its own D3D device (that is what decouples the render
  context from a slow consumer). Its constructor called `D3D11CreateDevice` on the caller's
  thread, which is the render thread: 13 to 21 ms measured, logged as
  `[d3d-participant-create] ... cost_us=`. Resizes already reused the device; first creation
  did not.
- **Fix:** `D3DDecoupledExport::Creation::Deferred` creates the device and textures on the
  exporter's own worker thread. Until `ready()` it accepts no frame and has no handle.
  - **Participant tiles** use it: a new source's tile handle appears a frame or two late.
  - **The encoder export** is created deferred as soon as there is a render target
    (`prewarmEncoderExport`, every render) and kept. A stream's encode path is chosen from the
    handle on its STARTING frame, so that caller waits (bounded, 250 ms) if a stream starts
    inside the creation window. Cost: one idle device and five Program-sized textures.
  - **Multiview and preview exports** are created deferred too (2026-10-02), and a resize of
    either calls `resize()`, which reuses the exporter's device. Neither has a handle for a
    pass or two after first creation; the core repeats that pass every tick until it does.
    Tests that expected the multiview event on the first tick now tick until it arrives.
- **Measured (streaming stand-in harness, `youtube-same-run-av.mjs --local-standin`):**
  before, 5 of 5 runs missed slots 17-18 and/or 21-22; after, 0 of 6. Participant creation on
  the render thread went from about 21,000 us to 60-1,500 us.
- **The recorded harness now judges frame delivery (2026-10-02).**
  `scripts/qa/program-buffer-recorded-av.mjs` hard-coded `framePerformancePassed: false` and
  never looked. It now fails a run whose Program buffer reports any underrun, overflow,
  GPU-not-ready, deadline miss or sequence gap since core launch, which covers the media
  Take at the start. Watched failing: a core frozen for 150 ms mid-run reported 10 and 11
  underruns and the run failed.
  - Measured with it: 0 underruns in 22 sessions on `main` and after this change, and 0 in
    20 sessions on two older betas. **The 4-6 slot stall first reported did not reproduce on
    any build**, so nothing here claims to have fixed it.
  - Its A/V check is a separate, unreliable gate: audio lands 13 to 18 ms after video for
    the media clip, which straddles its one-frame limit (16.7 ms), so it fails about one run
    in four with nothing wrong. Two sessions in 14 on `main` read 34.5 ms. Filed as #750.
- **Not covered:** a real meeting with a real media Take in the app; other first-use costs on
  the render thread (the MF recording open is on its own thread already).
- **Tests:** `D3DDecoupledExportTest.cpp` (deferred creation is off the caller's thread, then
  publishes; resize and destroy while pending).
