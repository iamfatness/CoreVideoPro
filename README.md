# CoreVideo Pro

CoreVideo Pro is a standalone native desktop production app for producing high-quality online conversations and building polished live and recorded shows directly from Zoom participants. This repository contains the native Studio shell, WinUI shell, C++ media core, typed product state, and engine contracts for Zoom capture, AI production, and output layers.

This is not intended to ship as a browser-hosted SPA, and the Electron app has been removed. The active app paths are the native C++ Studio in `studio/` and the WinUI native shell in `native-shell/`, with media capture, compositing, recording, streaming, diagnostics, and packaging handled by native desktop processes behind typed IPC contracts.

The product depends on a native media core for real-time Zoom ingest, GPU compositing, chroma key, overlays, audio mixing, recording, ISO capture, and streaming. Do not add new Electron work; use the native Studio/WinUI paths.

**Roadmap:** the demo-driven, sprint-by-sprint path to the first live demo lives in [`docs/roadmap/index.html`](docs/roadmap/index.html) — open it in a browser.

**Native production completion:** the plan and per-feature spec for turning the synthetic/telemetry native paths (UVC/AJA/DeckLink capture, SRT, NDI, recording, audio, RTMP, compositor framing, overlays, automation) into real implementations lives in [`docs/native-production-completion-plan.md`](docs/native-production-completion-plan.md).

## Current Slice

- React + Vite app shell for the operator console.
- Editable Zoom connection workflow for meeting URL/ID, producer display name, and webinar mode before joining.
- Deterministic simulated Zoom session with join, leave, feed refresh, active speaker rotation, captions, and feed health.
- Capture snapshot mapper that normalizes raw Zoom-like participant events into immutable app-level snapshots.
- Breakout-aware participant metadata and filters for isolating main-room or breakout-room feeds and screen shares.
- Producer role overrides for Zoom participants so Magic Scene and show routing can use stable Host, Presenter, Panelist, and Guest assignments.
- Simulated media frame model for participant video and screen share, including frame IDs, resolution, FPS, frame age, health, and smart-crop rectangles.
- Engine bundle injection so the UI can swap mock engines for native Zoom/media implementations without importing mock singletons.
- Native Zoom bridge adapter shell with typed command/response protocol for a future SDK process or desktop IPC transport.
- Shell-agnostic native host bridge bootstrap: the renderer uses a preload/native bridge when present and falls back to mock engines only for local development.
- Native media-core capability contract for raw Zoom media, GPU scene graph rendering, direct participant transforms, overlays, chroma key, program/ISO recording, and RTMP/NDI/SRT/WebRTC output.
- Native media-core command builder that serializes the Zoom source roster, active speaker, screen-share source, active scene graph, Zoom participant routes, participant transforms, color grade, enabled graphics, selected output profile, explicit recording targets/session lifecycle, and streaming destinations into shell-independent payloads for a future C++/Rust media engine.
- Renderer-to-media-core sync engine that pushes production state into native media-core snapshots and surfaces synced source count, resolved routes, render-plan layers, scene, frame, transform, overlay, output health, recording health, and warning status in the app.
- `native-core` workspace with the first backend media-core process boundary: a JSON-line service, spawnable client, runtime state machine, pluggable media-source adapter contract, deterministic test-pattern source, recording writer lifecycle model, and tests for applying scene graph, transform, overlay, recording, ISO, frame, and output commands.
- Runtime-selectable media-source adapters through `set-media-source-adapter`, currently covering deterministic test-pattern and clean local-camera-shaped sources behind the same contract future Zoom SDK ingest will use.
- Native source routing registry and render-plan snapshots that resolve fixed participant, active-speaker, spotlight, screen-share, and disabled routes into compositor-ready layers with operator warnings for missing feeds, muted isolated audio, duplicate video assignments, and unavailable screen share.
- Native compositor contract with stable render-plan IDs, program-frame snapshots, compositor health, reconfigure reasons, degraded/dropped frame counts, and a clear split between source frames for ISO capture and composed program frames for output.
- Native program-frame transport snapshot for the in-process preview/program path so the UI can inspect whether composed frames are being published before pixel transport lands.
- Native encoder target and lifecycle boundary that attaches recording, ISO, RTMP, NDI, SRT, and WebRTC outputs to the program-frame stream and surfaces per-target health, prepare/start/stop state, and output warnings before real sender implementations land.
- Native output sender session model for RTMP, NDI, SRT, and WebRTC program senders, including active sender counts, sent frame counts, latency, bitrate, retry warnings, and stopped sender diagnostics.
- Explicit runtime recovery commands for failed output senders and recording writers so diagnostics preserve failures until the operator or automation recovers the affected path.
- Backend recording-session snapshots with session IDs, target folders, encoder intent, program/ISO file paths, elapsed time, estimated disk rate, stream frame counts, byte counters, stopped/failed writer states, and warning state surfaced in the app's Native core readout.
- Native media-core diagnostic snapshots with source adapter health, scene/output state, program transport, compositor state, encoder targets, recording health, warnings, and command history for future support-bundle export.
- Native audio-mix and caption-track commands (`sync-participant-audio-mix`, `push-caption-cue`, `set-caption-enabled`) threaded through the typed media-core boundary with per-participant gain/limiter state, master loudness, mixed-frame counts, live caption cues, and Native core readout in the operator UI.
- Native audio routing gain matrix (`sync-audio-routing-matrix`) threaded through the typed media-core boundary so the WinUI Routing tab's source-to-bus crosspoints (PGM L/R, ISO 1/2, MON, STREAM) drive the audio mix, with per-bus source counts, routed-send totals, gain clamping to [-60, 10] dB, and unrouted-source warnings surfaced in the Native core readout.
- **F2 — real program audio bus (PCM mixing graph).** `AudioFrame` carries interleaved-float PCM; a header-only `AudioMixGraph` (`native/src/modules/AudioDsp.h`) runs the per-participant chain (gain → pan → noise-suppression seam → VST-insert seam → bus sends) with sample-acting solo/mute, sums into real PGM L/R / ISO 1–8 / MON / STREAM / AUX buses via routing-matrix crosspoints, applies a master true-peak limiter and ITU-R BS.1770 LUFS meter, and exposes program/per-ISO/MON PCM taps at 48 kHz stereo. Snapshot `programMaster` metrics (true peak, RMS, momentary/short-term/integrated LUFS, gain reduction) are MEASURED from the program bus and mirrored across `Protocol.h` ↔ `native-core/src/protocol.ts` ↔ `src/engine/nativeMediaCoreProtocol.ts` (asserted by `ContractParityTest`). A dev-gated `WasapiMonitorOutput` (`COREVIDEO_WITH_WASAPI_MONITOR`) plays the MON bus, replacing the `Beep()` fallback. ASIO capture and full VST3 hosting remain seams.
- Set & Forget auto-director stabilization with screen-share and scene-change holds, automatic brand-kit application to graphics and the native core (`set-brand-kit`), and one-click resume to automation after manual override.
- Interactive Media tab playback (`set-media-playback`) threaded through the typed media-core boundary so each media-bin asset has a Play/pause control that selects the clip, toggles playback, and pushes the selection and playing state to the native core, with a now-playing readout, paused/playing status, and empty-asset warnings surfaced in the media-playback snapshot.
- Native output profile snapshots for shared recording/RTMP/NDI/SRT/WebRTC resolution, FPS, and target bitrate decisions before real sender implementations land.
- Native output bridge adapter shell for recording, streaming, output-profile selection, output health, and output-session state.
- Simulated output session model that tracks recording, streaming, elapsed output time, recording file, stream target, and health.
- Configurable local recording settings for folder, filename prefix, format, and quality, with preflight validation carried through the output engine and show presets.
- Selected participant ISO recording feeds for program-plus-clean-guest capture, with ISO-aware output status, encoder load, and diagnostics runway estimates.
- Output profile controls for 1080p/4K and 30/60fps with bitrate and encoder-health simulation.
- Multi-destination output model for RTMP, NDI, and SRT targets with editable endpoint/key settings, armed/live state, latency, bitrate, and per-destination health.
- Output preflight checks that block streaming when armed destinations are missing required endpoints, stream keys, or protocol-compatible URLs.
- Diagnostics support bundle engine with redacted output secrets, human triage lines, output health, participant feed guidance, ISO runway estimates, and sanitized native media-core runtime summaries.
- Smart audio mix engine for per-participant gain, mute state, noise suppression, limiter status, master level, and loudness summary.
- Manual per-participant audio gain trim layered on top of smart leveling for fast producer correction.
- Adaptive caption and overlay engine with speaker attribution, confidence, latency, lower-third placement, and collision warnings.
- Graphics library model with toggleable brand bug, live banner, and call-to-action program overlays.
- Per-participant video effects for manual crop override, manual zoom, and chroma key state.
- Program-first scene workflow with optional Preview Monitor, Cut/Fade/Slide transitions, and explicit Take behavior.
- Keyboard command layer for Take, Record, Stream, Magic Scene, feed refresh, and optional Preview Monitor without stealing focus from text fields.
- Manual scene slot assignment so producers can choose which Zoom participants fill preview scene layouts before taking them live.
- Route-aware scene slots with fixed participant, active speaker, spotlight, screen-share, and none modes plus per-slot audio roles.
- Show preset engine for saving and reloading scenes, graphics, participant video effects, destinations, mode, and transitions.
- Scene-template layout model for intro, interview, speaker-plus-slides, panel grid, and closing layouts.
- Engine-backed Magic Scene interaction.
- Set & Forget auto-director that recommends, queues, and automatically takes scene layouts from live Zoom state.
- Scene list, program preview, lower-third, captions, participant roster, smart handling, audio/output health.
- TypeScript contracts for:
  - Zoom capture.
  - AI Magic Scene generation.
  - recording/streaming output.
- Vitest coverage for Magic Scene, simulated Zoom session state, and core UI controls.
- Mapper tests for raw participant normalization, video-off behavior, feed-health mapping, and immutable snapshot output.
- Breakout tests for raw Zoom metadata normalization and participant/screen-share filtering.
- Media-frame tests for low-resolution feeds, video-off handling, screen-share source identity, and active-speaker crop behavior.
- Output-session tests for idle, recording, streaming, combined output, and active elapsed-time behavior.
- Output-profile tests for 1080p/4K selection, profile-aware bitrate, and encoder health.
- Multi-destination and recording preflight tests for arming targets, output readiness, live target health, aggregate bitrate, and network warning behavior.
- ISO recording tests for selected participant feeds, output-session status, native bridge payloads, support-bundle runway, and UI controls.
- Native host and media-core protocol tests proving the UI shell is not the real-time video engine.
- Native media-core command tests proving production state can be handed to a native compositor without binding the renderer to browser capture APIs.
- Native audio-mix and caption-track tests in `native-core` and the renderer sync/mapper layers proving participant leveling and live caption cues round-trip through the media-core snapshot contract.
- Audio routing matrix tests in `native-core` (`mediaCore.test.ts`), the renderer command builder and sync engine (`nativeMediaCoreCommands.test.ts`, `mediaCoreSync.test.ts`), and the C++ media core (`ContractParityTest.cpp`, `MediaCoreCommandTest.cpp`) proving routed source-to-bus sends round-trip with gain clamping, per-bus source counts, and out-of-range/unrouted-source warnings.
- Auto-director hold tests and brand-kit command tests proving unattended production stays stable and on-brand through the media-core snapshot contract.
- Media playback tests in `native-core` (`mediaCore.test.ts`), the C++ media core (`ContractParityTest.cpp`, `MediaCoreCommandTest.cpp`), and the WinUI command builder (`MediaCoreCommandBuilderTests.cs`) proving `set-media-playback` selects a clip, reports playing/paused/idle status in the snapshot, and warns when no media asset id is supplied.
- Diagnostics tests for support bundle redaction, low-quality feed guidance, duplicate assignments, missing screen share, and UI export flow.
- Scene-layout tests proving template selection changes the rendered program preview.
- Audio-mix tests for smart leveling, producer mute/gain overrides, and UI control behavior.
- Caption-overlay tests for speaker attribution, adaptive placement, collision warnings, and scene-change updates.
- Graphics library tests proving overlays can be toggled into the program preview.
- Transition tests proving scenes queue to preview before being taken to program.
- Scene-slot tests proving manually assigned Zoom participants render in program after Take.
- Routing tests proving active-speaker slots and route metadata drive preview composition and diagnostics warnings.
- Auto-production tests for screen-share holds, panel recommendations, manual queueing, and Set & Forget auto-take behavior.
- Preset tests for serializing repeatable show setup, listing summaries, loading presets, and restoring UI state.
- Video-effects tests for chroma key toggles, manual crop zoom, and preview badges.

## Planned Native Runtime Shape

The operator surface talks to native desktop engines through a narrow bridge:

```text
CoreVideo Pro Native Studio / WinUI Shell
  -> Operator Surface
  -> EngineBundle
  -> ZoomCaptureEngine
  -> NativeZoomTransport
  -> CaptureSnapshotMapper / native output session mapper
  -> AiProductionEngine
  -> AudioMixEngine
  -> CaptionOverlayEngine
  -> PresetEngine
  -> OutputEngine
  -> NativeOutputEngineAdapter
  -> NativeMediaCoreCommands
  -> MediaCoreServiceClient
  -> native-core service boundary
  -> Zoom source registry / route resolver
  -> compositor render plan
  -> media source adapter (Zoom SDK / local camera / test pattern)
  -> program compositor / program frame stream
  -> in-process preview/program transport
  -> encoder target + lifecycle boundary
  -> output sender sessions (RTMP / NDI / SRT / WebRTC)
  -> recording writer lifecycle / program + ISO stream counters
  -> output profile model
  -> output health and diagnostics snapshots
  -> Native Media Core
  -> Zoom SDK raw audio/video ingest
  -> GPU compositor / scene graph / overlays / chroma key
  -> audio mixer / encoder / recorder / stream outputs
```

The current `src/engine/contracts.ts` and `src/engine/engineBundle.ts` files are the boundary for that work. The mock engines should be replaced incrementally, not rewritten, as each native desktop capability lands.

## Commands

```powershell
npm install
npm run dev
npm run dev:native-core
npm run test
npm run test:native-core
npm run typecheck
npm run build
npm run build:native-core
npm run build:studio
npm run run:studio
```

## MVP North Star

The first fully useful milestone is:

1. Join Zoom.
2. See clean participant feeds and metadata.
3. Click Magic Scene.
4. Get a polished show with lower-thirds, captions, smart framing, audio leveling, and RTMP/local recording controls.
