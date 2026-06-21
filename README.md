# CoreVideo Pro

CoreVideo Pro is a **native Windows desktop production studio** for building polished
live shows, recordings, and streams directly from Zoom participants — Magic Scene
auto-layout, lower-thirds, captions, smart framing, audio leveling, and
multi-destination output, in a single operator console.

This is **not** a browser-hosted SPA, and **Electron has been removed**. The active
product paths are the WinUI native shell in `native-shell/` and the native C++ Studio
shell in `studio/`, with capture, compositing, recording, streaming, diagnostics, and
packaging handled by native desktop processes behind typed IPC contracts. Do not add
new Electron work.

## Architecture

```text
WinUI 3 shell (.NET 9)              native-shell/CoreVideoPro.WinUI
  └─ embedded React + Vite UI       src/  (operator console, immutable production state)
       └─ typed command/snapshot    src/engine/contracts.ts, engineBundle.ts
            protocol (JSON-line)
            └─ C++ media core        native/   (real-time pixels/PCM/transport)
               (Node mirror for CI)  native-core/  (protocol + runtime parity, in-container)
                  └─ Zoom ingest · D3D11 compositor · audio mixer/DSP ·
                     Media Foundation recorder · RTMP/NDI/SRT/WebRTC senders ·
                     diagnostics / support bundles
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
| | Live UVC camera & DeckLink/AJA frames reaching the core (not just WinUI preview) | In progress |
| **Compositor** | Route resolver, render-plan layers, program/preview parity math | Real |
| | Per-source framing (fit/fill/stretch, zoom/pan, borders) | Real (D3D11 + CPU stub) |
| | Overlay / lower-third / caption **text & image rasterization** (DirectWrite/WIC) | In progress |
| | D3D11 GPU compositor | Dev-gated (`COREVIDEO_WITH_D3D11`) |
| **Audio** | PCM routing matrix, program/ISO taps, BS.1770 master meter, bus-insert dynamics, limiter | Real |
| | WASAPI monitor output · ASIO capture · VST3 insert host | Dev-gated / In progress |
| **Recording** | Program + ISO mux with real program audio, profile-driven resolution/fps | Real path (MF encoder is Windows) |
| **Streaming** | RTMP with real program-audio feed + H.264/AAC compatibility matrix | Dev-gated (`COREVIDEO_WITH_RTMP_OUTPUT`, FFmpeg) |
| | NDI sender · SRT ingest decode | Dev-gated / In progress |
| | SRT **output** sender | In progress (not yet implemented) |
| **Production** | Magic Scene, Set & Forget auto-director, presets, brand kit, media playback | Real (heuristic, no ML) |
| **Diagnostics** | Support bundle with redacted secrets, output/recording health, crash events | Real |

> **Release readiness.** The contract surface is broad and well-tested, but the native
> hardware paths above have not yet passed a Windows dev-rig validation pass (real Zoom
> join, GPU/encoder, record-and-stream on a clean machine). See
> [`docs/alpha-plan.md`](docs/alpha-plan.md) and
> [`docs/native-production-completion-plan.md`](docs/native-production-completion-plan.md)
> for the exit bar and the remaining real-implementation work.

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

- Sprint-by-sprint demo roadmap: [`docs/roadmap/index.html`](docs/roadmap/index.html) (open in a browser).
- Alpha build plan & exit bar: [`docs/alpha-plan.md`](docs/alpha-plan.md).
- Native production completion plan: [`docs/native-production-completion-plan.md`](docs/native-production-completion-plan.md).
- Product spec & positioning: [`COREVIDEO_PRO_PRODUCT_SPEC.md`](COREVIDEO_PRO_PRODUCT_SPEC.md).
- WinUI shell setup: [`native-shell/README.md`](native-shell/README.md) · Studio shell: [`studio/README.md`](studio/README.md).
