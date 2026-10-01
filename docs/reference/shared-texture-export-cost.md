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
  - **Multiview and preview exports are unchanged** (created once at startup, on the render
    thread). A resize of either still builds a new exporter instead of calling `resize()`.
- **Measured (streaming stand-in harness, `youtube-same-run-av.mjs --local-standin`):**
  before, 5 of 5 runs missed slots 17-18 and/or 21-22; after, 0 of 6. Participant creation on
  the render thread went from about 21,000 us to 60-1,500 us.
- **Not covered:** the real app with a real meeting; the ~100 ms / 4-6 slot stall the issue
  first reported (this harness showed two slots per event); other first-use costs on the
  render thread (the MF recording open is on its own thread already).
- **`validate-gpu-encode.mjs` is flaky on main:** it greps the lossy process log for
  `[gpu-encode] path=` and failed 2 of 5 runs on both the `cdd2e87` core and this change,
  with the stream itself healthy each time. Read the sender snapshot before believing it.
- **Tests:** `D3DDecoupledExportTest.cpp` (deferred creation is off the caller's thread, then
  publishes; resize and destroy while pending).
