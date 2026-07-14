# Browser Sources & Screen Capture — Architecture Spec

Status: owner-requested 2026-07-05 ("we need to add the ability for browser sources and
screencapture"). Written 2026-07-05 (overnight session). Companion docs:
`native-production-completion-plan.md` (UVC capture, the ingest pattern these reuse),
`audio-overhaul-spec.md` (bus routing the audio sides plug into).

## 1. What these are (product terms)

- **Screen capture**: any monitor or window as a first-class video source — assignable
  to Show Inputs, scenes, multiview, program. With optional per-application loopback
  audio as a mixer source. Table stakes vs vMix/Ecamm/OBS; needed for "share my deck /
  browser / scoreboard" shows.
- **Browser source**: any URL rendered as a live, optionally transparent source —
  overlays (lower thirds from Singular/H2R/uno), dashboards, chat widgets, countdowns.
  THE OBS-era workflow feature; mimoLive/vMix both charge for it. With optional page
  audio into the mixer.

Both are **video sources like any other** once ingested: they ride the existing
shared-texture/compositor path, appear in Sources, route via the matrix, and obey roles.

## 2. Boundary shape (the seven principles applied)

Renderer/shell stays dumb: it lists capturable targets and sends intent
(`capture.screen.add {monitorId|windowId, audio:bool}`, `capture.browser.add {url, w, h,
fps, audio:bool}`); the CORE owns capture, conversion, pacing; snapshots carry health +
telemetry per source. Mock-first: the stub core simulates both source kinds (animated
test card labeled with the URL/monitor) so shell UX and tests need no OS capture or
WebView at all. Health as data: `"starting" | "live" | "occluded" | "failed"` +
warnings[] (e.g. "window minimized — frames frozen"). Safety posture (principle 6):
browser content is untrusted — it never runs in the core process (see §4).

## 3. Screen capture (Phase SC)

**API: Windows.Graphics.Capture (WGC)** — the modern, compositor-fed, HDR-capable path
(Windows 10 1903+; our floor is Win 10 19041 ✓). Not GDI BitBlt (slow, no windows
composition), not DXGI Desktop Duplication (monitor-only, no per-window, admin quirks).

- Core-side `WgcCaptureAdapter` (new, `native/src/modules/`):
  `Direct3D11CaptureFramePool` (free-threaded, 2 buffers, BGRA8) → each frame arrives
  already as an ID3D11Texture2D on OUR device → copy into the compositor's ingest
  texture under keyed mutex. **Zero CPU pixels** — north-star compliant.
- Targets enumerated core-side (monitors via DXGI outputs; windows via
  EnumWindows+filters), published in the snapshot as `captureTargets[]`; the shell's
  picker is a plain list (no GraphicsCapturePicker dependency — we enumerate ourselves,
  no per-session user prompt).
- Cadence: WGC delivers on content change; the compositor samples latest-wins at its
  own 60fps tick (same discipline as Zoom video ingest — no event-driven work in hot
  paths; the beacon/poll lesson is spec law now).
- **App loopback audio** (Phase SC-A): `ActivateAudioInterfaceAsync` +
  `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK` (Win10 20H1+) captures ONE process
  tree's audio — the captured window's app — as a mixer source with the standard
  channel strip (routes like any source; latency declared per Z3 when it lands).
  Fallback below 20H1: endpoint loopback with a warning that it's whole-system audio.
- Lifecycle: window closed → source goes `"failed"` with warning, keeps last frame
  frozen 2s then slate; monitor unplugged → same. No crashes on target death — WGC
  gives `Closed` events.
- Tests: adapter state machine pure-tested (target add/remove/death transitions);
  stub-core simulation exercised by shell tests; rig validation = 1080p60 monitor +
  window capture composited with Zoom sources, lock-guardrail clean.

## 4. Browser source (Phase BR)

**STATUS: BR-1 (render-only) SHIPPED 2026-07-13** (branch claude/browser-source-br1).
What landed vs. this spec:

- `corevideo-browser-host.exe` (`native/browser-host/`, one process per source, vendored
  WebView2 SDK in `third_party/webview2/`) renders the URL in a **windowed**
  CoreWebView2Controller inside a borderless tool-window positioned OFF the virtual
  desktop, and **WGC-self-captures its own window** — a pragmatic composition of already
  proven pieces instead of the windowless CompositionController below. Measured on the
  rig: **real premultiplied BGRA alpha survives end-to-end** (transparent page regions
  arrive alpha=0) and an animated page sustains ~28fps offscreen (Chromium occlusion
  tracking disabled via `--disable-features=CalculateNativeWinOcclusion`).
- Frame delivery is the existing **capture-SHM seqlock** ingest (same layout as the
  WinUI bridge; `native/src/modules/BrowserSourceShm.h`), keyed `capture:browser:<n>`
  — compositor/routing/multiview needed zero changes. The keyed-mutex shared-texture
  path in this section remains the BR-1.5 upgrade (removes the per-frame CPU copy).
- Core: `browser-add` / `browser-remove` / `browser-reload` commands
  (`BrowserSourceHostAdapter` — CreateProcessA spawn, stdin `reload
` pipe, EOF = quit,
  death -> slate + LOUD warning + 5->60s capped backoff, give up after 5 straight
  failures; `browserSources` health node in the snapshot).
- Shell: "Add browser source" (URL + size preset) on Sources; Browser group in the
  unified picker; control API `browser.add` / `browser.remove` / `browser.reload`.
- NOT in BR-1: page audio (BR-A/BR-3), interactivity/custom-CSS (BR-2), zoom,
  `allowNavigation` origin policy (popups/downloads ARE blocked; navigation is free).

**Engine: WebView2 (Chromium, evergreen runtime)** — ships on Win11/Win10 via the
Evergreen runtime; we already require .NET 9 + WinUI so the WebView2 runtime is a soft
dependency we can bootstrap-install.

**Process model (the safety decision):** browser content NEVER runs inside
`corevideo-native.exe`. A dedicated `corevideo-browser-host.exe` (one per browser
source, WinUI-less Win32 + WebView2 in **windowless/visual hosting mode**) renders the
page and delivers frames to the core over the SAME shared-texture pattern the Zoom
engine uses (keyed-mutex DXGI shared texture; alpha preserved — BGRA premultiplied).
A crashed/renegade page kills its host process, the core shows `"failed"` + slate, the
show goes on — the Zoom-engine isolation lesson, reapplied.

- Composition capture: WebView2 `CoreWebView2CompositionController` renders into a
  visual we bring; frames land GPU-side; transparent background via
  `DefaultBackgroundColor = transparent` — real alpha overlays, the feature OBS users
  expect.
- Control surface: `browser.add {url, width, height, fps, zoom, audio, customCss?}`,
  `browser.reload`, `browser.setUrl`. Interactivity later (BR-2: forwarded mouse/key
  for dashboards); v1 is render-only.
- **Page audio** (Phase BR-A): WebView2 audio is a normal process audio stream → the
  SAME process-loopback capture as SC-A pointed at the browser-host PID. One mechanism,
  two features.
- Security posture: pages get NO access to local files (WebView2 default) + we disable
  downloads, popups, devtools in the host; the host runs at normal user integrity but
  is command-line-pinned to one URL origin policy (navigation to other origins allowed
  only when the operator set `allowNavigation`).
- Tests: host process protocol pure-tested (init/frame/teardown state machine);
  stub-core simulates a browser source (animated slate + URL label) for shell tests;
  rig validation = transparent lower-third URL over program at 60fps.

## 5. Sequencing & effort

1. **SC (screen video)** — smallest real win, all-native, reuses UVC/Zoom ingest
   patterns. ~1 session core + shell picker.
2. **SC-A (app loopback audio)** — rides the audio mixer; needs Z3 latency declaration
   for sync polish but ships useful before it. ~1 session.
3. **BR (browser video)** — the host process + composition capture is the substantial
   piece. ~2 sessions.
4. **BR-A (page audio)** — SC-A mechanism re-aimed. Small.
5. **BR-2 (interactivity, custom CSS/JS injection)** — later, demand-driven.

## 6. Invariants inherited

All of `zoom-audio-spec.md` §5 (the session laws) plus: no pixel work under shared
locks (the three-phase/ingest-thread lesson, 2026-07-05); every new stream kind gets a
fake/simulated twin in the stub core the day it lands (the soak-harness lesson: if the
rig can't drive it ear/eye-free, it isn't testable).
