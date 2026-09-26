# CoreVideo Pro

CoreVideo Pro is a **native desktop production studio for Windows and macOS** for building polished
live shows, recordings, and streams directly from Zoom participants — Magic Scene
auto-layout, lower-thirds, captions, smart framing, audio leveling, and
multi-destination output, in a single operator console.

**Work order lives in one place:** [`docs/BACKLOG.md`](docs/BACKLOG.md). This README is
as-built status, not a queue.

## Architecture

```text
WinUI 3 shell (.NET 9, native XAML)  native-shell/CoreVideoPro.WinUI  ← Windows UI
SwiftUI shell                      mac-shell/                       ← macOS UI
  └─ typed command/snapshot          JSON-line over child stdin/stdout; Windows DXGI
       protocol (JSON-line)          shared textures for program/preview/multiview
       └─ C++ media core             native/   (real-time pixels/PCM/transport)
          ├─ Zoom engine subprocess  native/zoom-engine/  (Zoom SDK → I420 shared memory)
          ├─ D3D11 / Metal compositor · audio mixer/DSP
          ├─ Media Foundation / AVFoundation recorder · output adapters
          └─ virtual-camera publisher · diagnostics / support bundles

Virtual camera (out-of-process)       native/virtualcam-dll/  → corevideo-virtualcam.dll
  loaded by the Windows Frame Server; reads the program from cross-session shared memory
  and presents "CoreVideo Pro Camera" to Zoom / Teams / OBS at 1080p60

Shared generated lifecycle models    contracts/   (schema + cross-language wire fixtures)
React + Vite dev/contract UI          src/         (mock-first development client)
Node media-core mirror               native-core/ (deterministic protocol test runtime)
```

The native core owns real-time media. Shells send production intent over child-process
stdin/stdout and consume snapshots; GPU surfaces and shared-memory media use separate
transports. Windows uses D3D11 and Media Foundation; macOS uses Metal and AVFoundation.

The additive [lifecycle schema](contracts/lifecycle.schema.json) generates C++, C#,
TypeScript, and Swift models and runtime validators. Golden wire fixtures run across
language suites. Legacy scene/audio/capture **and production command/capability**
protocols still have handwritten mirrors; see the [coverage inventory](contracts/README.md)
and [ownership map](docs/architecture-ownership.md). A listed capability is not yet an
admission check — `profileCapabilities()` follows compile flags more closely than
successfully constructed adapters ([#617](https://github.com/iamfatness/CoreVideoPro/issues/617)).
Unknown commands in a production-sync batch can be dropped
([#616](https://github.com/iamfatness/CoreVideoPro/issues/616)).

Recording intent, verified media activity, and file finalization are distinct.
A Stop acknowledgement does not certify a playable finalized file. Zoom join/auth
runs on a cancellable worker while the bounded command mailbox continues serving
operator requests. See [command semantics](docs/control-command-lifecycle.md).

The default in-container build is **stub-first** (`-DCOREVIDEO_STUB=ON`,
`-DCOREVIDEO_ENABLE_DEV_ADAPTERS=OFF`): every capability has a deterministic synthetic
implementation so contracts and tests run with no hardware. Real Zoom SDK, GPU, encoder,
and hardware-transport code lives behind `COREVIDEO_ENABLE_DEV_ADAPTERS` plus a per-feature
`COREVIDEO_WITH_*` flag. Windows and macOS builds select their platform adapters;
vendor integrations require the corresponding SDKs and runtimes.

Windows **merge CI compiles that stub core** plus the WinUI shell. The production D3D11,
Media Foundation, WASAPI, UVC, WGC, and virtual-camera configuration is a local-dev /
packaged-candidate build, not the merge gate
([#618](https://github.com/iamfatness/CoreVideoPro/issues/618)). macOS CI already builds
the real Metal/AVFoundation configuration.

## Capabilities & status

Status legend: **Real** = implemented and exercised in the portable/CI build · **Dev-gated**
= implemented behind a `COREVIDEO_WITH_*` flag, requires the relevant platform/runtime ·
**In progress** = wired through the contract but the native pixel/PCM path is unfinished.

| Area | Capability | Status |
|---|---|---|
| **Capture** | Zoom roster, active speaker, captions, feed health, breakout filters, producer roles | Real (simulated session) |
| | Real Zoom Meeting SDK ingest via the vendored `corevideo-zoom-engine` (raw I420 over shared memory) | Dev-gated (`COREVIDEO_WITH_ZOOM`) |
| | Test-pattern / local-camera source delivering real pixels into the core | Real |
| | Native UVC camera capture inside the core (Media Foundation source reader, 1080p60-targeted NV12/YUY2/MJPG negotiation, I420 → GPU shader convert with per-frame range/matrix, hot-unplug safe; WinUI shm bridge stays the per-device fallback; native capture is default-on and can be disabled with `COREVIDEO_NATIVE_UVC=0`). Uses first-frame confirmation before committing the native path; errors/timeouts fall back to the managed bridge | Dev-gated (`COREVIDEO_WITH_UVC`), default-on when available |
| | Live DeckLink/AJA frames reaching the core (not just WinUI preview) | In progress — probe/enumerate only; no pixels on the source bus ([#537](https://github.com/iamfatness/CoreVideoPro/issues/537)) |
| **Compositor** | Route resolver, render-plan layers, program/preview parity math | Real |
| | Per-source framing (fit/fill/stretch, zoom/pan, borders) | Real (D3D11 + CPU stub) |
| | Overlay / lower-third / caption **text & image rasterization** — shared layout (`computeOverlayTileLayout`), signature-cached | Real (CPU full-ASCII bitmap-font tile) · Dev-gated (DirectWrite/D2D + WIC, zero-copy GPU raster) |
| | D3D11 GPU compositor | Dev-gated (`COREVIDEO_WITH_D3D11`) |
| | Core-composited GPU multiview (single shared texture, 4 layout modes, WinUI overlay labels/tally/meters/clock) | Real (layout/tiles) · Dev-gated (D3D11 render) |
| **Audio** | PCM routing matrix, program/ISO taps, BS.1770 master meter, bus-insert dynamics, limiter | Real |
| | WASAPI monitor output · ASIO capture · VST3 insert host | Dev-gated / In progress |
| **Recording** | Program + ISO mux with real program audio, profile-driven resolution/fps | Implemented: Media Foundation on Windows, AVFoundation on macOS; candidate verification required |
| **Streaming** | RTMP with real program-audio feed + H.264/AAC compatibility matrix | Dev-gated (`COREVIDEO_WITH_RTMP_OUTPUT`, FFmpeg) |
| | NDI sender · SRT ingest decode | Dev-gated / In progress |
| | SRT **output** sender | In progress (not yet implemented) |
| | **Virtual camera** — the program feed appears as a "CoreVideo Pro Camera" webcam in Zoom / Teams / OBS / the Windows Camera app, at native **1080p60** | Implemented on Windows; candidate verification required |
| **Production** | Magic Scene, Set & Forget auto-director, presets, brand kit, media playback | Real (heuristic, no ML) |
| **Diagnostics** | Support bundle with redacted secrets, output/recording health, crash events | Real |

> **Release readiness.** The portable/CI contract surface is broad, but Windows production
> media paths (D3D11, MF encode, WASAPI, real Zoom, UVC/WGC, vcam) are proven on a
> packaged or dev-rig candidate, not by the stub merge job. Exact-candidate evidence
> still required: real Zoom join, GPU/encoder, record-and-stream, clean-machine install.
> See [`docs/alpha-plan.md`](docs/alpha-plan.md) and
> [`docs/native-production-completion-plan.md`](docs/native-production-completion-plan.md).

> **Architecture hardening.** Process isolation (Zoom child, FFmpeg, vcam, browser, VST)
> is the shipped shape; NDI remains in-process. Windows CI covers the WinUI suite and the
> stub core. Configuration saves use atomic replacement and backup recovery; LAN HTTP
> control requires authentication. Signed release candidates must pass automated checks
> and controlled-rig evidence before publication:
> [release evidence setup](docs/release-evidence.md). A green portable suite is not
> verified live-media evidence.

## Current priority (2026-09-25)

Ranked work is only in [`docs/BACKLOG.md`](docs/BACKLOG.md).

1. **Now:** finish the source bus ([#535](https://github.com/iamfatness/CoreVideoPro/issues/535)). Capture pictures and embedded PCM are mailbox delivery. The media decoder takes one clip request. Zoom audio is a mailbox drained outside the core lock. `IZoomCaptureSource` is gone. The no-meeting slate is a bus source, and Zoom engine audio is still polled outside the core lock before it is staged onto the bus. Browser sources are still polled. Capture dropout liveness stays [#562](https://github.com/iamfatness/CoreVideoPro/issues/562).
2. **Next:** make the wire honest ([#616](https://github.com/iamfatness/CoreVideoPro/issues/616) with [#619](https://github.com/iamfatness/CoreVideoPro/issues/619) / [#620](https://github.com/iamfatness/CoreVideoPro/issues/620)), then constructed capabilities ([#617](https://github.com/iamfatness/CoreVideoPro/issues/617)), then a Windows production-native CI compile lane ([#618](https://github.com/iamfatness/CoreVideoPro/issues/618)), then [#538](https://github.com/iamfatness/CoreVideoPro/issues/538) SRT/NDI edge I/O. Do not start #616 while #535 is open unless the owner re-ranks.
3. Live incidents (#615, #608, #624, #597 acceptance) stay unranked until the owner promotes them; they can preempt this list on a show night. Per-participant audio delay is [#627](https://github.com/iamfatness/CoreVideoPro/issues/627), also unranked.

Shipped on main since 2026-09-24, still waiting on a fleet show for [#601](https://github.com/iamfatness/CoreVideoPro/issues/601) and [#615](https://github.com/iamfatness/CoreVideoPro/issues/615):

- The GPU encoder keeps the newest prepared frame when publish is busy, and the program buffer waits on a high-resolution timer (`61e83ed1`, [#614](https://github.com/iamfatness/CoreVideoPro/pull/614)). Unsigned beta `beta-2026-09-24-61e83ed`.
- Capture polling is retired. Adapters push into a mailbox (`4519ade0`, [#625](https://github.com/iamfatness/CoreVideoPro/pull/625)). Unsigned beta `beta-2026-09-25-4519ade`.
- `IMediaDecoder` takes one `MediaDecodeRequest` (`02e61c43`, [#626](https://github.com/iamfatness/CoreVideoPro/pull/626)). Unsigned beta `beta-2026-09-25-02e61c4`.
- Zoom audio is drained from a mailbox outside the core lock. Headless clap on that build (`scripts/validate-av-clap.mjs`, 2026-09-25): 8 pairs, median skew -39.0 ms (audio lags video by 39 ms), spread 14.3 ms, inside the 50 ms gate. A person clapping in a live meeting was not recorded.

## Repository layout

| Path | Role |
|---|---|
| `src/` | React + Vite operator console, immutable production state, engine contracts |
| `mac-shell/` | SwiftUI desktop shell and macOS presentation |
| `native-shell/` | **WinUI 3 (.NET 9)** desktop shell — primary product path and packaging |
| `studio/` | Native C++ Win32 test shell for fast desktop validation |
| `native/` | C++20 media core (compositor, audio, encoder, output adapters) + vendored Zoom engine |
| `native-core/` | Node.js mirror of the media-core protocol/runtime for in-container tests |
| `services/` | Backend services (caption broker, license) |
| `scripts/` | PowerShell build / package / sign / validation scripts (Windows) |
| `docs/` | Ranked work order ([BACKLOG.md](docs/BACKLOG.md)), alpha plan, native completion plan, reference specs |

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

On macOS, build the Swift shell with `cd mac-shell && swift build -c release`.
Run its deterministic suite with `COREVIDEO_SHELL_TESTS=1 .build/release/CoreVideoProShell`.
The [macOS launch script](scripts/run-mac-shell.sh) documents the native-core and SDK
configuration. A successful shell build does not validate the real media adapters.

## MVP North Star

The first fully useful milestone:

1. Join Zoom.
2. See clean participant feeds and metadata.
3. Click Magic Scene.
4. Get a polished show with lower-thirds, captions, smart framing, audio leveling, and
   RTMP / local-recording controls.

## Further reading

- **Ranked work order:** [`docs/BACKLOG.md`](docs/BACKLOG.md).
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
