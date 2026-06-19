# CoreVideo Pro — Alpha Readiness Plan

_Status snapshot: 2026-06-19. Owner: production. This is the working plan for
getting from the current codebase to a usable alpha build._

## 1. Where we are today

CoreVideo Pro is a native Windows production app (OBS-class) that builds live
shows from Zoom participants. The architecture is a typed renderer ↔ native-core
boundary:

```
WinUI shell (C#) ──▶ MediaCore bridge (C#, JSON-RPC) ──▶ native media core (C++)
                                                          ├─ Zoom SDK ingest (vendored, proven)
                                                          ├─ D3D11 compositor (dev adapter)
                                                          ├─ Media Foundation MP4 recorder (dev adapter)
                                                          └─ RTMP/NDI/SRT/WebRTC senders
```

The codebase is well-architected with a clean mock→native seam, and is roughly
**~70% of the way to a credible alpha**. The hardest piece (real Zoom raw
ingest) is already proven in the vendored OBS-plugin engine.

### What is real vs. simulated

| Area | State | Notes |
|---|---|---|
| Operator UI (WinUI + scene builder) | **Real, functional** | Needs UX restructure (this plan) |
| Zoom raw ingest | **Proven, gated** | `COREVIDEO_BUILD_ZOOM_ENGINE=ON`, Windows + SDK |
| GPU compositor (D3D11) | **Real, gated** | `COREVIDEO_ENABLE_DEV_ADAPTERS + COREVIDEO_WITH_D3D11` |
| MP4 recording (Media Foundation) | **Real, gated** | Needs output-path wiring + on-disk validation |
| RTMP streaming | **Probe + sender scaffold** | Real send needs FFmpeg runtime staged |
| Audio DSP (leveling/NS/limiter) | **Simulated state** | Metrics modeled; real DSP is post-alpha |
| Captions/transcription | **Rule-based overlay** | No real transcription engine yet |
| NDI/SRT/WebRTC output | **Contracts only** | Deferred past alpha |
| Hardware capture (Blackmagic/AJA) | **Stubs + gated adapters** | Optional for alpha |
| JSON-RPC bridge + contract parity | **Real, tested** | TS↔C++ parity gate in CI |

### Build & test status

- TypeScript: `typecheck`, unit (vitest), integration — green, cross-platform.
- C++ native core: stub build + GoogleTest + contract-parity — green in Linux CI.
- Windows-only gates (`test:native-shell`, `test:studio-workflow`, smoke) require
  a Windows runner / dev machine.
- Real Zoom and record/stream validation are **manual** runbook steps today.

## 2. Alpha exit criteria

An alpha is "working" when, on a Windows dev machine with the Zoom SDK staged, an
operator can:

1. Join a real Zoom meeting and see clean participant feeds + metadata.
2. Map participants/devices to **Inputs 1–10**.
3. Route video and audio with a clear, fast UI (Dante-style matrix — see §4).
4. Build scenes on a **fully-visible 16:9 canvas** (no scroll-to-build).
5. Click Magic Scene / Set & Forget and get a polished, stable show.
6. Record a stable 1080p MP4 to disk.
7. Stream to one RTMP destination.
8. Recover gracefully from feed/encoder/output failures (diagnostics + support bundle).

## 3. Gap-closing workstreams (to alpha)

### Track A — Operator UX restructure (this is the active track)
- [x] **Scene builder canvas scales to the window** — full 16:9 always visible,
      OBS-style, no scroll-to-build. _(done — see SourcesPage/SceneCanvasEditor)_
- [ ] **Inputs / Sources / Routing / Scenes IA** — split the overloaded Scenes
      screen into a clean signal-flow (see §4). Pending design sign-off.
- [ ] Error/empty/loading states, onboarding, and state-transition clarity.

### Track B — Real media path activation (Windows)
- [ ] Stage Zoom SDK; activate `NativeZoomEngineAdapter`; validate raw ingest
      under participant churn + screen share.
- [ ] Wire Media Foundation recorder output path; validate on-disk MP4 (A/V sync,
      duration, no dropped frames over a 30-min show).
- [ ] Stage FFmpeg; validate one live RTMP destination end-to-end.

### Track C — Stability & diagnostics
- [ ] Crash/restart supervision for the native core under real load.
- [ ] First-frame + recording + output health surfaced and recoverable in-shell.
- [ ] Support-bundle export validated on a real failed-run.

### Track D — Packaging
- [ ] MSIX package + signing path green; one-click launch validated on a clean box.

## 4. Proposed signal-flow IA (Sources → Inputs → Routing → Scenes)

The current "Scenes" tab does too much: canvas editing, per-source route-mode
dropdowns, capture-device discovery, feed health, and the Input 1–10 multiview
all live on one scrolling page. The proposal separates concerns along the path a
signal actually travels, which mirrors how vMix / TriCaster / Dante Controller
organize the same problem:

```
 Sources ──▶ Inputs (1–10) ──▶ Routing (matrix) ──▶ Scenes (canvas) ──▶ Outputs
 (devices,    (stable slots a    (Dante-style       (compositor /        (record,
  Zoom,        scene/route can    crosspoint grid    16:9 canvas with      stream)
  media)       reference)         for V + A)         Inputs as layers)
```

- **Sources tab** — discover and bind what feeds each Input: Zoom participants,
  capture devices (Blackmagic/AJA/UVC), media. This is today's `ShowInputSlot`
  editor + capture discovery + feed health, made the home for "what is plugged
  in." Maps source → Input 01–10.

- **Routing tab (new)** — two Dante-controller-style crosspoint matrices:
  - **Video matrix:** sources (Inputs 01–10, Active Speaker, Screen Share, Media)
    × destinations (Program, Preview, ISO A–D, Multiview, Aux/Stream). Click a
    crosspoint to route.
  - **Audio matrix:** audio sources (Input mics, Zoom mix, Media) × buses
    (Program L/R, ISO tracks, Monitor/Aux, Stream), crosspoints carry on/off
    (+ gain). This is the Dante audio experience.
  - This is where today's per-layer `Mode` and `AudioRole` move to.

- **Scenes tab** — pure compositor. The 16:9 canvas (now always fully visible).
  "Add source" lists **Inputs 1–10** (plus Active Speaker / Screen Share / Media).
  A scene layer = an Input reference + canvas rect + z-index. Audio is governed by
  the Audio matrix, not per-layer dropdowns.

This maps cleanly onto existing models (`ShowInputSlot`, `SourceRoute`,
`NormalizedCanvasRect`) and onto the native `load-scene-graph` command, so it is a
re-organization + one new matrix model, not a rewrite.

## 5. Risks

- **Zoom SDK entitlements + real-world load** (churn, network, screen share) can
  only be validated against live meetings — schedule real test calls early.
- Windows-only build/test means the matrix + IA work cannot be visually verified
  in Linux CI; pair every layout change with a Windows smoke pass.
- Audio routing semantics (crosspoint on/off vs. full fader matrix) is a product
  decision that affects DSP scope — lock it before building the audio matrix.
