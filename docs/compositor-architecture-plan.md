# CoreVideo Pro compositor architecture — path to 1080p60 × N, low latency

North-star ([[corevideopro-product-northstar]]): low-latency + high-quality A/V, every
Zoom source 1080p, up to 60fps, up to 8 participants — better than vMix / Ecamm / mimoLive.

## What the reference compositors do (OBS, CasparCG, Natron)

**OBS Studio** (`obsproject/obs-studio`)
- All compositing on the **GPU** (libobs graphics, D3D11/OpenGL). Each source renders to a
  texture; the scene is blended with shaders. **YUV→RGB conversion happens in a shader**, not
  on the CPU. Sources deliver frames asynchronously; frames are uploaded to GPU textures once.
- **Audio is a completely separate pipeline** on its own thread: sample-accurate mixing,
  continuous, synced to video by timestamps — never gated on the video render.
- A dedicated **video/graphics thread** renders on the output frame clock; the control thread
  only mutates state. No blocking GPU readback in the hot path; double/triple buffered.

**CasparCG Server** (`CasparCG/server`)
- Deterministic **frame clock**: producers → GPU **mixer (GLSL)** → consumers, all driven at
  the channel framerate. The mixer composites on the GPU. Audio mixed separately, aligned to
  frames. Strict separation of concerns + one steady clock.

**Natron** (`NatronGitHub/Natron`)
- Node-graph compositor (VFX, not realtime broadcast). Less directly applicable, but its
  **node graph + region-of-interest + frame caching** inform a clean scene-composition model
  and only-recompute-what-changed.

## How that maps to our current bottlenecks (measured this session)

| Our problem (measured) | Root | Reference fix |
|---|---|---|
| Render 15ms, ~3–12fps with Zoom | per-participant **CPU I420→BGRA** + per-tile uploads | **GPU shader** convert + composite (OBS/CasparCG) |
| lockWait 40–65ms, render starved | `media-core-sync` runs full renders under the core lock | **steady frame clock**; commands mutate state only (CasparCG) |
| audio broke when we cut render rate | audio is processed **inside the video render tick** | **separate audio thread**, continuous (OBS) |
| Zoom SDK crash at N×1080p raw | N concurrent 1080p CPU decodes + over-subscription | GPU offload removes CPU limit; manage SDK renderer concurrency |
| per-monitor swap-chain churn | WinUI rebuilt host collection | (fixed) stable collection + in-place sync |

## Plan (prioritized; each phase independently testable)

**Phase 1 — GPU I420→RGB convert + composite (the unlock).** Upload each participant's
I420 Y/U/V planes to GPU textures; convert + composite in one shader pass in
`D3D11CompositorAdapter` (it already GPU-composites BGRA — extend it to ingest I420 and
convert in-shader, deleting the CPU `readZoomEngineI420FrameSnapshot`/`i420ToRgbaPixel`
path). Removes the per-participant CPU cost → enables 1080p for **all** feeds.

**Phase 2 — Decouple audio from the video render.** A dedicated audio thread does
continuous, sample-accurate mixing (WASAPI-paced), independent of video. Video render does
video only; `applyCommands`/`syncParticipantAudioMix` only mutate state. Fixes the
lock-contention + the audio-rate regression properly (vs. the reverted shortcut).

**Phase 3 — Single render authority on a 60fps clock.** The dedicated render thread is the
SOLE renderer (already exists as `renderDisplayTick`); `applyCommands` NEVER renders. With
audio on its own thread (Phase 2), removing per-sync renders no longer starves audio →
eliminates the lockWait 40–65ms.

**Phase 4 — Zoom subscription for N×1080p.** With the CPU freed (Phase 1), pull all visible
feeds at 1080p. Empirically find the SDK's concurrent-renderer limit; subscribe on-demand
for visible tiles and recycle renderers rather than holding N forever. (Interim today:
active-speaker + share 1080p, other tiles 720p.)

**Phase 5 — Output encoder on its own cadence.** Recording/stream encoder consumes the
composited GPU frame at output fps on its own thread, not on the control-sync path.

## Done this session (foundation)
- GPU per-participant shared-texture multiview tiles (zero-copy present per tile).
- PROGRAM monitor = core GPU program composite (single swap chain).
- Capture hold-last-frame (no pink), skip-unchanged texture uploads (render 15→2.8ms),
  in-place multiview grid (no host churn), 60fps loop pacing + 1ms timer (34→56fps),
  Zoom resolution-by-purpose.
