# GPU multiview — implementation plan (core-composited single texture)

_Status: 2026-06-29 — **IMPLEMENTED + MERGED** (Phase 1 native `f64b4ac`, Phase 2–3 WinUI
`d0a169b`). The core composites the whole grid into ONE keyed-mutex shared texture and the
WinUI presents it in ONE VideoSurfaceHost + transparent click-overlay; the per-frame tile
churn is retired. Build green; survived 4.5 min fake-engine churn with zero
`CoreMessagingXP 0xc000027b`. NEEDS a human GUI pass: assign 2–3 Show Inputs and confirm
the grid presents smoothly + tapping a tile cues it to preview. The plan below is the
as-built design (kept for reference)._

_Supersedes the earlier per-tile-shared-texture approach, which FAILED: giving each
participant tile its own DXGI swap chain fail-fasts the WinUI (`CoreMessagingXP
0xc000027b`) under live roster/active-speaker churn — N swap chains created/reloaded per
change. CPU/base64 tiles were a stable-but-choppy, non-scaling stopgap (the owner
correctly rejected CPU for 8 Zoom + 2 capture)._

## The approach

Have the **core composite the entire multiview grid into ONE GPU shared texture**, the
same way program/preview already work, and have the WinUI present it in **one**
`VideoSurfaceHost` (one swap chain) with a thin transparent click-overlay for tile taps.
This is the OBS/CasparCG/broadcast-multiviewer model. It scales (one composite, same cost
class as program), and it removes the per-tile churn crash **by construction**.

## Producer (native) — reuse the program path

- `D3D11CompositorAdapter.cpp`: add a second pass `renderMultiview(plan, frames)` that
  mirrors `render()` — its own `multiviewRenderTarget_` + keyed-mutex
  `multiviewSharedTexture_` (copy `ensureRenderTarget`/`ensureSharedTexture`, minus the
  CPU staging readback). Save/restore `targetWidth_/targetHeight_` around the pass (the
  draw helpers read those members — risk #1). Reuse `resolveLayers`/`drawLayer` so Zoom
  tiles get the GPU I420 shader and capture tiles get BGRA for free. Bake the
  active-speaker border into the texture (no WinUI churn on speaker change). **Delete**
  `exportParticipantTextures`/`renderI420ToParticipantTexture`/`participantTextures_`.
- `MediaCore.cpp`: new `set-multiview-layout` command (the WinUI sends the ordered Show
  Input roster: sourceId/kind/participantId|deviceId|assetId, slot, label, grid shape).
  New `buildMultiviewRenderPlan(videoFrames)` (sibling of `buildCompositorRenderPlan`) —
  one layer per layout entry, rect from a grid-cell helper. In `renderSyntheticTick`,
  after the program render, call `renderMultiview` and fill
  `lastProgramFrame_.multiviewSharedTexture` + normalized tile rects. Stays on the light
  60fps `videoOnly` tick, no CPU readback.
- Transport (`ProgramFramePreview` + `JsonRpcServer`): emit a `multiview-shared-texture`
  event `{ texture{handle,w,h,frame}, canvasW/H, tiles[{sourceId,participantId,slot,
  label,activeSpeaker,x,y,w,h}] }` on **structural** change only; add to the cold-start
  snapshot.

## Consumer (WinUI)

- Parse `multiview-shared-texture` (mirror `participant-shared-texture`); `VideoSurfaceCoordinator`
  stores one `MultiviewSurface` + the tile-rect list, fires `SurfacesChanged` on structural
  change only.
- `ShowMultiviewHost`: replace the per-tile grid with ONE `VideoSurfaceHost`
  (`SurfaceKey="multiview"`, re-added to `UsesGpuSharedTexture` — now a single stable swap
  chain, the proven program model) + a transparent `Canvas` of click targets positioned
  from the normalized rects mapped through the **same** letterbox transform the swap chain
  uses (risk #3), invoking the existing preview/program tile command.
- `StudioViewModel`: send `set-multiview-layout` from the Show Input roster; **delete**
  `MultiviewGridTiles`/`RefreshMultiviewGridTiles` + the per-frame `MultiviewTiles` rebuild
  in `RefreshSurfaceBindings` (the old crash vector goes away entirely). The aspect-aware
  grid-shape math now in `ShowMultiviewHost.ResolveGridShape` should move to / be mirrored
  by the core's `buildMultiviewRenderPlan` so both agree on rows×cols.

## Sequencing (flag-gated `COREVIDEO_MULTIVIEW_GPU`, smallest safe steps)

1. Native: `renderMultiview` + second RT/shared texture, driven by a hard-coded layout;
   assert non-empty handle (headless). 2. Transport: event + parser + bridge, log receipt.
3. Single-host present behind the flag. 4. Real layout from the Show Input roster.
5. Click overlay (tap-to-preview/program parity). 6. Active-speaker border in-core.
7. Delete the dead per-tile/`participant-shared-texture`/`MultiviewGridTiles` paths; flip
   the flag on. Validate each step with `corevideo-zoom-engine-fake.exe` + UIA join under
   sustained churn (the 0xc000027b repro) — expect zero fail-fast and lower GPU memory
   (one texture replaces N).

## Riskiest parts

1. Save/restore of shared D3D state (`targetWidth_/Height_`, bound RT/shaders) between the
   program and multiview passes (subtle corruption, not a crash).
2. Confirming the single multiview swap chain stays stable under sustained churn (mitigated
   by reusing the proven program present path + deleting the N-tile path).
3. Click-overlay coordinate mapping through the letterbox transform
   (`Direct3D11InteropService.ApplyPanelTransform`).
