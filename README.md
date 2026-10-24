# CoreVideo Pro

CoreVideo Pro is a **native desktop production studio for Windows and macOS** for building polished
live shows, recordings, and streams from Zoom participants, cameras, screen capture and
media: Magic Scene auto-layout, lower-thirds, graphics, audio leveling, and
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
```

The native core owns real-time media. Shells send production intent over child-process
stdin/stdout and consume snapshots; GPU surfaces and shared-memory media use separate
transports. Windows uses D3D11 and Media Foundation; macOS uses Metal and AVFoundation.

The additive [lifecycle schema](contracts/lifecycle.schema.json) generates C++, C#,
and Swift models and runtime validators. Golden wire fixtures run across
language suites. Legacy scene/audio/capture **and production command/capability**
protocols still have handwritten mirrors; see the [coverage inventory](contracts/README.md)
and [ownership map](docs/architecture-ownership.md). The native hello profile now
reports capabilities from constructed adapters. `capabilityStates` distinguishes
available, omitted, and failed-to-construct paths; the legacy `capabilities` list
contains only available paths. An available path still needs live endpoint
acceptance ([#617](https://github.com/iamfatness/CoreVideoPro/issues/617)).
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

Windows merge CI runs the stub core plus the WinUI shell, and a separate
`native-production-windows` job compiles the D3D11 and Media Foundation adapters. The real
Zoom SDK is still not compiled in CI ([#618](https://github.com/iamfatness/CoreVideoPro/issues/618)):
Zoom, WASAPI, UVC, WGC and the virtual camera are proven only on local and packaged builds.
macOS CI builds the real Metal/AVFoundation configuration.

## Capabilities & status

As built on `main`, October 23, 2026. Three states, and only three:

- **Proven** — shipped in a beta and measured on a real run, with the evidence named.
- **Shipped, not live-verified** — in the beta, covered by unit or hosted tests, never
  measured on a real show or installed run.
- **Not shipped** — code exists behind a flag or on a branch, or only a probe exists.

Every Windows beta is built with the production adapters on (D3D11, Media Foundation,
WASAPI, Zoom, UVC, WGC, virtual camera). The stub core is a test configuration, not a
product tier. The reference run below is the 30-minute outputs soak of October 22, 2026
on `beta-2026-10-22-f96a136` (ruling and summary in [`docs/BACKLOG.md`](docs/BACKLOG.md), "Owner gate
ruling, October 22"; raw evidence in the local `artifacts/qualification-1008-25`, not in git): eight 1080p Zoom feeds, webcam, moving screen capture,
Program + eight ISO recordings on hardware encoders, the registered OS camera at 1080p60 and
an SRT stream, with zero missing frame identities on every output. The later
`beta-2026-10-23-1072a50` has not been soaked.

| Area | Capability | Status |
|---|---|---|
| **Zoom** | Join by link, roster, active speaker, raw 1080p video and PCM per participant through the vendored `corevideo-zoom-engine` subprocess (I420 over shared memory) | Proven: eight 1080p feeds for 30 minutes, and weekly live shows since September |
| | Guest network quality in the roster | Shipped, not live-verified: read from the SDK callback since [#1023](https://github.com/iamfatness/CoreVideoPro/pull/1023), absent until reported |
| **Capture** | Webcam and capture cards (Media Foundation in the core, 1080p60, hot-unplug safe; WinUI bridge fallback; opt out with `COREVIDEO_NATIVE_UVC=0`) | Proven: webcam in the soak with exact ISO identity |
| | Screen and window capture (WGC) | Proven: moving screen in the soak |
| | A disconnected capture device drops from Program instead of freezing | Not shipped ([#758](https://github.com/iamfatness/CoreVideoPro/issues/758), [#562](https://github.com/iamfatness/CoreVideoPro/issues/562)) |
| | DeckLink / AJA | Not shipped: probe only ([#537](https://github.com/iamfatness/CoreVideoPro/issues/537)) |
| **Media** | Local video and still playback with core-owned cue/live/pause | Shipped, not live-verified; ProRes fails to start in the installed build ([#473](https://github.com/iamfatness/CoreVideoPro/issues/473)); stutter reported ([#701](https://github.com/iamfatness/CoreVideoPro/issues/701)) |
| **Compositor** | D3D11 Program/Preview/Multiview, per-source framing, borders, grade presets and curves, lower-thirds and graphics templates | Proven: Program render held every deadline in the soak (minimum slack 3.6 ms) |
| | Set & Forget and Magic Scene automation driven by the core's slot bindings | Shipped, not live-verified: bound in [#1021](https://github.com/iamfatness/CoreVideoPro/pull/1021), unit-tested to the coordinator |
| | Captions | Not shipped: the controls were removed in #1021 because no caption source exists; `push-caption-cue` remains for external cue feeds |
| **Audio** | Routing matrix, Program and ISO taps, BS.1770 meter, bus dynamics and limiter, WASAPI monitor output | Proven on live shows; recorded Program A/V skew measured near -20 ms, live monitor skew -46 ms after [#655](https://github.com/iamfatness/CoreVideoPro/pull/655) ([#652](https://github.com/iamfatness/CoreVideoPro/issues/652) open on the limit) |
| | Per-participant audio delay | Not shipped ([#627](https://github.com/iamfatness/CoreVideoPro/issues/627)) |
| **Recording** | Program + ISO MP4 on Media Foundation with hardware encoders and isolated write queues; resumes after a core restart | Proven: Program and eight ISOs frame-exact for 30 minutes; resume proven on a drill, not a show |
| | Local recording with no Zoom meeting | Shipped, not live-verified ([#1025](https://github.com/iamfatness/CoreVideoPro/pull/1025)) |
| | macOS AVFoundation recording | Shipped, not live-verified |
| **Streaming** | SRT caller with GPU-direct HEVC/H.264 NVENC | Proven: frame-exact for 30 minutes to a local sink; YouTube caller send verified once ([#669](https://github.com/iamfatness/CoreVideoPro/pull/669)) |
| | RTMP (YouTube) | Shipped; full-show continuity unproven after the September 13 incident ([#615](https://github.com/iamfatness/CoreVideoPro/issues/615)); bytes sent are measured since #1023 |
| | NDI send with group name · HLS push | Shipped, not live-verified: NDI group reaches the sender since #1023 but no receiver with a group filter has been tested; HLS needs an HTTP PUT origin |
| | AV1 | Not shipped: refused until a playable receiver-side stream exists ([#565](https://github.com/iamfatness/CoreVideoPro/issues/565)) |
| **Virtual camera** | "CoreVideo Pro Camera" through the Windows Frame Server at 1080p60, owned machine runtime with serving-DLL provenance | Proven: zero missing identities for 30 minutes; Frame Server can keep serving an old DLL after an upgrade ([#781](https://github.com/iamfatness/CoreVideoPro/issues/781)) |
| **Operations** | Control API on loopback (`/snapshot`, `/state`, `/invoke`); LAN control requires a token | Proven: every soak and drill drives the app through it |
| | Outputs drill as one command (`scripts/qa/outputs-drill.ps1`) | Shipped, not live-verified ([#1024](https://github.com/iamfatness/CoreVideoPro/pull/1024)) |
| | Support bundle with redacted secrets | Shipped; sections that were always empty were removed in #1021 |
| | Crash upload and telemetry | Not shipped: the uploader reads its endpoint from an environment variable only, so the beta uploads nowhere ([#434](https://github.com/iamfatness/CoreVideoPro/issues/434)) |
| | Signed releases | Not shipped: the release workflow has both signing routes and no certificate ([#423](https://github.com/iamfatness/CoreVideoPro/issues/423), [#437](https://github.com/iamfatness/CoreVideoPro/issues/437)) |
| **Survival** | Core restart with output resume; Zoom helper crash absorbed | Proven on drills; one unexplained helper exit in a real meeting ([#856](https://github.com/iamfatness/CoreVideoPro/issues/856)) |
| | Production survives a WinUI crash (EngineHost) | Shipped off by default in [#965](https://github.com/iamfatness/CoreVideoPro/pull/965); not enabled, not live-verified |
| | Idle WinUI finalizer crash after about an hour | Known, upstream ([#513](https://github.com/iamfatness/CoreVideoPro/issues/513)) |

Every operator-visible value is measured or absent; a placeholder is never shown
([honest reporting](docs/reference/honest-reporting.md)). The release gate is per-frame
identity of every output over the full workload (a writer-reported held frame during a source
stall passes as a labelled, counted repeat, owner ruling 2026-10-23; see
[the outputs drill](docs/qa/outputs-drill.md)); on-screen Studio cells are a tolerance
metric because Windows DWM skips frames at the mixed-refresh monitor beat for every
application ([`scripts/qa/desktop-picture-cadence.py`](scripts/qa/desktop-picture-cadence.py)).

What stands between this build and a release candidate is tracked only in
[`docs/BACKLOG.md`](docs/BACKLOG.md). The non-engineering items are a code-signing
certificate, the Zoom SDK redistribution answer, reference machines and a tester feedback
channel ([#423](https://github.com/iamfatness/CoreVideoPro/issues/423) to [#427](https://github.com/iamfatness/CoreVideoPro/issues/427)); the controlled-rig two-hour soak in
[`docs/release-evidence.md`](docs/release-evidence.md) has not been run.

## Repository layout

| Path | Role |
|---|---|
| `src/config/` | Embedded Zoom Meeting SDK / OAuth public-client manifests read by the shell and scripts |
| `mac-shell/` | SwiftUI desktop shell and macOS presentation |
| `native-shell/` | **WinUI 3 (.NET 9)** desktop shell — primary product path and packaging |
| `studio/` | Native C++ Win32 test shell for fast desktop validation |
| `native/` | C++20 media core (compositor, audio, encoder, output adapters) + vendored Zoom engine |
| `services/` | Cloudflare Workers (telemetry-ingest crash/event upload, ops-monitor uptime probe) |
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
npm run test:unit           # vitest: services + Companion module tests
npm run test:show-engine    # show-engine workspace tests
npm run test:scripts        # node:test suites under scripts/
npm run build:studio        # C++ media core + native Studio shell (Windows)
npm run run:studio          # launch the native Studio shell (Windows)
npm run pack:native         # stage the WinUI shell + native core for distribution
```

Full Windows gate (contracts + show-engine/native/shell suites): `npm run test:gate`.
Offline readiness report: `npm run alpha:preflight`.

On macOS, build the Swift shell with `cd mac-shell && swift build -c release`.
Run its deterministic suite with `COREVIDEO_SHELL_TESTS=1 .build/release/CoreVideoProShell`.
The [macOS launch script](scripts/run-mac-shell.sh) documents the native core and SDK
configuration. A successful shell build does not validate the real media adapters.

## MVP North Star

The first fully useful milestone:

1. Join Zoom.
2. See clean participant feeds and metadata.
3. Click Magic Scene.
4. Get a polished show with lower-thirds, graphics, audio leveling, and
   stream / local-recording / virtual-camera controls.

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
