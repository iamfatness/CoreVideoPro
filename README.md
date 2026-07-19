# CoreVideo Pro

CoreVideo Pro is a **native Windows desktop production studio** for building polished
live shows, recordings, and streams directly from Zoom participants — Magic Scene
auto-layout, lower-thirds, captions, smart framing, audio leveling, and
multi-destination output, in a single operator console.

## Architecture

```text
WinUI 3 shell (.NET 9, native XAML)  native-shell/CoreVideoPro.WinUI  ← the shipping UI
  └─ typed command/snapshot          JSON-line over named pipes; keyed-mutex DXGI
       protocol (JSON-line)          shared textures for program/preview/multiview
       └─ C++ media core             native/   (real-time pixels/PCM/transport)
          └─ Zoom engine subprocess  native/zoom-engine/  (Zoom SDK → I420 shared memory)
             └─ Zoom ingest · D3D11 compositor · audio mixer/DSP ·
                Media Foundation recorder · RTMP/NDI/SRT senders ·
                virtual-camera publisher · diagnostics / support bundles

Virtual camera (out-of-process)       native/virtualcam-dll/  → corevideo-virtualcam.dll
  loaded by the Windows Frame Server; reads the program from cross-session shared memory
  and presents "CoreVideo Pro Camera" to Zoom / Teams / OBS at 1080p60

React + Vite dev/contract UI         src/         (protocol source of truth + mock-first
Node media-core mirror               native-core/  dev UI and the in-container CI parity
                                                   surface — NOT embedded in the WinUI app)
```

The renderer never owns the real-time media pipeline. It serializes production state
([`src/domain/production.ts`](src/domain/production.ts)) into transport-neutral commands
([`src/engine/nativeMediaCoreCommands.ts`](src/engine/nativeMediaCoreCommands.ts)) and
reads back immutable `*Snapshot`s. The wire types exist three times and stay in lockstep
via a parity gate: the C++ core ([`native/src/core/Protocol.h`](native/src/core/Protocol.h)),
the Node mirror ([`native-core/src/protocol.ts`](native-core/src/protocol.ts)), and the
renderer mirror ([`src/engine/nativeMediaCoreProtocol.ts`](src/engine/nativeMediaCoreProtocol.ts)).

The default in-container build is **stub-first** (`-DCOREVIDEO_STUB=ON`,
`-DCOREVIDEO_ENABLE_DEV_ADAPTERS=OFF`): every capability has a deterministic synthetic
implementation so contracts and tests run with no hardware. Real Zoom SDK, GPU, encoder,
and hardware-transport code lives behind `COREVIDEO_ENABLE_DEV_ADAPTERS` plus a per-feature
`COREVIDEO_WITH_*` flag and is only built on a Windows dev rig with the vendor SDKs staged.

## Capabilities & status

Status legend: **Real** = implemented and exercised in the portable/CI build · **Dev-gated**
= implemented behind a `COREVIDEO_WITH_*` flag, runs only on a Windows rig with SDKs ·
**In progress** = wired through the contract but the native pixel/PCM path is unfinished.

| Area | Capability | Status |
|---|---|---|
| **Capture** | Zoom roster, active speaker, captions, feed health, breakout filters, producer roles | Real (simulated session) |
| | Real Zoom Meeting SDK ingest via the vendored `corevideo-zoom-engine` (raw I420 over shared memory) | Dev-gated (`COREVIDEO_WITH_ZOOM`) |
| | Test-pattern / local-camera source delivering real pixels into the core | Real |
| | Native UVC camera capture inside the core (Media Foundation source reader, 1080p60-targeted NV12/YUY2/MJPG negotiation, I420 → GPU shader convert with per-frame range/matrix, hot-unplug safe; WinUI shm bridge stays the fallback via `COREVIDEO_NATIVE_UVC=1` opt-in). Eliminates the shell's per-frame managed copy (the operator-lag root cause), but a last-mile "native frame reaches the multiview tile" gap keeps it opt-in — see [`docs/operator-performance-plan.md`](docs/operator-performance-plan.md) | Dev-gated (`COREVIDEO_WITH_UVC`), opt-in, display gap open |
| | Live DeckLink/AJA frames reaching the core (not just WinUI preview) | In progress |
| **Compositor** | Route resolver, render-plan layers, program/preview parity math | Real |
| | Per-source framing (fit/fill/stretch, zoom/pan, borders) | Real (D3D11 + CPU stub) |
| | Overlay / lower-third / caption **text & image rasterization** — shared layout (`computeOverlayTileLayout`), signature-cached | Real (CPU full-ASCII bitmap-font tile) · Dev-gated (DirectWrite/D2D + WIC, zero-copy GPU raster) |
| | D3D11 GPU compositor | Dev-gated (`COREVIDEO_WITH_D3D11`) |
| | Core-composited GPU multiview (single shared texture, 4 layout modes, WinUI overlay labels/tally/meters/clock) | Real (layout/tiles) · Dev-gated (D3D11 render) |
| **Audio** | PCM routing matrix, program/ISO taps, BS.1770 master meter, bus-insert dynamics, limiter | Real |
| | WASAPI monitor output · ASIO capture · VST3 insert host | Dev-gated / In progress |
| **Recording** | Program + ISO mux with real program audio, profile-driven resolution/fps | Real path (MF encoder is Windows) |
| **Streaming** | RTMP with real program-audio feed + H.264/AAC compatibility matrix | Dev-gated (`COREVIDEO_WITH_RTMP_OUTPUT`, FFmpeg) |
| | NDI sender · SRT ingest decode | Dev-gated / In progress |
| | SRT **output** sender | In progress (not yet implemented) |
| | **Virtual camera** — the program feed appears as a "CoreVideo Pro Camera" webcam in Zoom / Teams / OBS / the Windows Camera app, at native **1080p60** | Real (rig-verified in Zoom) |
| **Production** | Magic Scene, Set & Forget auto-director, presets, brand kit, media playback | Real (heuristic, no ML) |
| **Diagnostics** | Support bundle with redacted secrets, output/recording health, crash events | Real |

> **Release readiness.** The contract surface is broad and well-tested, but the native
> hardware paths above have not yet passed a Windows dev-rig validation pass (real Zoom
> join, GPU/encoder, record-and-stream on a clean machine). See
> [`docs/alpha-plan.md`](docs/alpha-plan.md) and
> [`docs/native-production-completion-plan.md`](docs/native-production-completion-plan.md)
> for the exit bar and the remaining real-implementation work.

> **Current focus (2026-07-10).** The **virtual camera** now works end-to-end in Zoom
> at native 1080p60 — cross-session file-backed shared memory (Frame Server serves from
> session 0, the core publishes from session 1), a hold-last-frame DLL (no flashing),
> live-clock PTS (no latency drift), and an off-render-thread dedicated-D3D-device readback
> so the tap costs the render loop ~1ms. In parallel we ran a full **operator-performance
> investigation** (PresentMon + dotnet-trace): the lag/stutter/crash is the WinUI shell's
> managed webcam-capture bridge copying every frame (~50% CPU → 2–3GB heap → crash), *not*
> the core or GPU (render holds 60fps on the RTX 4090). Native UVC capture removes that cost
> (verified memory 2.4GB→267MB) but a last-mile frame-display gap keeps it opt-in. Earlier
> landings still current: GPU core-composited multiview, Phase 2 audio/output worker
> decouple, zero-copy Zoom I420 ingest + 60fps pacer, multi-layer PREVIEW bus, clean/soaked
> audio. Active work: complete native-UVC display, the **alpha validation pass**
> ([`docs/alpha-plan.md`](docs/alpha-plan.md) Tracks A–F), and DeckLink/AJA capture. Build,
> run, the multi-participant test harness, the virtual-camera pipeline, the perf-profiling
> workflow, and the `CoreMessagingXP 0xc000027b` crash class are documented in
> [`CLAUDE.md`](CLAUDE.md).

## Repository layout

| Path | Role |
|---|---|
| `src/` | React + Vite operator console, immutable production state, engine contracts |
| `native-shell/` | **WinUI 3 (.NET 9)** desktop shell — primary product path and packaging |
| `studio/` | Native C++ Win32 test shell for fast desktop validation |
| `native/` | C++20 media core (compositor, audio, encoder, output adapters) + vendored Zoom engine |
| `native-core/` | Node.js mirror of the media-core protocol/runtime for in-container tests |
| `services/` | Backend services (caption broker, license) |
| `scripts/` | PowerShell build / package / sign / validation scripts (Windows) |
| `docs/` | Roadmap, alpha plan, native completion plan, and reference specs |

## Commands

**Run the app (Windows), one command:**

```powershell
npm install
npm run app                 # builds the best-available native core, then builds + launches the WinUI app
```

`npm run app` auto-picks the media-core tier it can build on your machine — **full**
(Zoom + D3D11 + RTMP + audio, needs the Zoom SDK), **audio** (real Windows audio,
synthetic video), or **stub** (all synthetic) — rebuilds it only when stale, then
launches and prints which tier you got. Force a rebuild with `npm run app -- -Rebuild`.

Other commands:

```powershell
npm run dev                 # operator UI against mock engines (any platform)
npm run dev:native-core     # Node media-core service (in-container parity)
npm run typecheck
npm run test                # vitest unit + integration
npm run test:native-core    # Node media-core tests
npm run build               # tsc + vite production bundle
npm run build:studio        # C++ media core + native Studio shell (Windows)
npm run run:studio          # launch the native Studio shell (Windows)
npm run pack:native         # stage the WinUI shell + native core for distribution
```

Full Windows gate (typecheck + all renderer/native/shell suites): `npm run test:gate`.
Offline readiness report: `npm run alpha:preflight`.

## MVP North Star

The first fully useful milestone:

1. Join Zoom.
2. See clean participant feeds and metadata.
3. Click Magic Scene.
4. Get a polished show with lower-thirds, captions, smart framing, audio leveling, and
   RTMP / local-recording controls.

## Further reading

- **Working guide (build/run/architecture/crash class/current state):** [`CLAUDE.md`](CLAUDE.md).
- GPU multiview implementation plan: [`docs/gpu-multiview-plan.md`](docs/gpu-multiview-plan.md).
- Operator performance investigation & plan: [`docs/operator-performance-plan.md`](docs/operator-performance-plan.md) · present-stutter fix spec: [`docs/present-stutter-fix-spec.md`](docs/present-stutter-fix-spec.md).
- Sources redesign, preview/program direct positioning, browser sources: [`docs/sources-redesign-spec.md`](docs/sources-redesign-spec.md) (builds on [`docs/capture-sources-spec.md`](docs/capture-sources-spec.md)).
- Compositor architecture (OBS/CasparCG/Natron-informed): [`docs/compositor-architecture-plan.md`](docs/compositor-architecture-plan.md).
- Sprint-by-sprint demo roadmap: [`docs/roadmap/index.html`](docs/roadmap/index.html) (open in a browser).
- Alpha build plan & exit bar: [`docs/alpha-plan.md`](docs/alpha-plan.md).
- **Focus & competitiveness plan (cut line, demos, 90-day sequencing):** [`docs/FOCUS_PLAN.md`](docs/FOCUS_PLAN.md).
- Native production completion plan: [`docs/native-production-completion-plan.md`](docs/native-production-completion-plan.md).
- Product spec & positioning: [`COREVIDEO_PRO_PRODUCT_SPEC.md`](COREVIDEO_PRO_PRODUCT_SPEC.md).
- WinUI shell setup: [`native-shell/README.md`](native-shell/README.md) · Studio shell: [`studio/README.md`](studio/README.md).
