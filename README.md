# CoreVideo Pro

CoreVideo Pro is a standalone cross-platform desktop production app for producing high-quality online conversations and building polished live and recorded shows directly from Zoom participants. This repository currently contains the first MVP execution slice: a desktop-ready production-console renderer, typed product state, and engine contracts for the native Zoom capture, AI production, and output layers.

This is not intended to ship as a browser-hosted SPA. The React/Vite surface is the desktop operator renderer that should run inside a native Mac/Windows shell, with media capture, compositing, recording, streaming, diagnostics, and packaging handled by native desktop processes behind typed IPC contracts.

The shell choice must stay replaceable. Electron, Tauri, or a custom native shell can host the renderer, but the end-state product depends on a native media core for real-time Zoom ingest, GPU compositing, chroma key, overlays, audio mixing, recording, ISO capture, and streaming.

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
- Native media-core command builder that serializes the active scene graph, Zoom participant routes, participant transforms, enabled graphics, selected output profile, explicit recording targets/session lifecycle, and streaming destinations into shell-independent payloads for a future C++/Rust media engine.
- Renderer-to-media-core sync engine that pushes production state into native media-core snapshots and surfaces synced scene, route, frame, transform, overlay, output health, recording health, and warning status in the app.
- `native-core` workspace with the first backend media-core process boundary: a JSON-line service, spawnable client, runtime state machine, deterministic fake frame producer, recording writer lifecycle model, and tests for applying scene graph, transform, overlay, recording, ISO, frame, and output commands.
- Backend recording-session snapshots with session IDs, target folders, encoder intent, program/ISO file paths, elapsed time, estimated disk rate, stream frame counts, byte counters, stopped/failed writer states, and warning state surfaced in the app's Native core readout.
- Native media-core diagnostic snapshots with scene/output state, recording health, warnings, and command history for future support-bundle export.
- Native output profile snapshots for shared recording/RTMP/NDI/SRT/WebRTC resolution, FPS, and target bitrate decisions before real sender implementations land.
- Native output bridge adapter shell for recording, streaming, output-profile selection, output health, and output-session state.
- Simulated output session model that tracks recording, streaming, elapsed output time, recording file, stream target, and health.
- Configurable local recording settings for folder, filename prefix, format, and quality, with preflight validation carried through the output engine and show presets.
- Selected participant ISO recording feeds for program-plus-clean-guest capture, with ISO-aware output status, encoder load, and diagnostics runway estimates.
- Output profile controls for 1080p/4K and 30/60fps with bitrate and encoder-health simulation.
- Multi-destination output model for RTMP, NDI, and SRT targets with editable endpoint/key settings, armed/live state, latency, bitrate, and per-destination health.
- Output preflight checks that block streaming when armed destinations are missing required endpoints, stream keys, or protocol-compatible URLs.
- Diagnostics support bundle engine with redacted output secrets, human triage lines, output health, participant feed guidance, and ISO runway estimates.
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
- Native media-core command tests proving production state can be handed to a native compositor without binding the renderer to Electron, OBS, or browser capture APIs.
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

## Planned Desktop Runtime Shape

The operator renderer should talk to native desktop engines through a narrow bridge:

```text
CoreVideo Pro Desktop Shell
  -> React Operator Renderer
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
  -> deterministic fake frame producer
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
```

## MVP North Star

The first fully useful milestone is:

1. Join Zoom.
2. See clean participant feeds and metadata.
3. Click Magic Scene.
4. Get a polished show with lower-thirds, captions, smart framing, audio leveling, and RTMP/local recording controls.
