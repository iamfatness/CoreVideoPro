# Current state notes, July 2026 (history)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

## Current state addendum (2026-07-13, the zero-audio recording bug)

**Recordings muxed ZERO audio while the master bus carried signal — FIXED.** Root
cause (proven headless with the fake tone engine + stderr gates): the live flow calls
`encoder->start()` TWICE per recording (start-program-output arms it, then
start-recording-session restarts it), and `MediaFoundationEncoderAdapter`'s `Mp4Writer`
is REUSED across those generations. `finalize()` never reset `audioConfigured_`, so on
the generation-2 writer `ensureAudioStream` early-returned without `AddStream` — every
audio `WriteSample` then hit a missing stream index and failed with
`MF_E_INVALIDSTREAMNUMBER` (0xC00D36B3) for the whole session, while video muxed
perfectly (its stream index IS refreshed in `open()`). The warning lived only in
`encoderSession.warnings`; `recording.warning` stayed null → invisible. Fixes:
(1) `Mp4Writer::open()` resets ALL per-session state; (2) the audio worker now publishes
the encoder's `recordingWarning` into `recordingWarning_` → snapshot `recording.warning`
(+ rate-limited `[recording]` stderr), so a video-only recording can never look healthy;
(3) regression tests in `EncoderRecordingSessionTest.cpp` (real-MF double-start test —
fails 0xC00D36B3 pre-fix — and a MediaCore warning-propagation test).

**Audio worker pacer: bounded catch-up (same PR).** The absolute-deadline pacer used to
RE-ANCHOR on any blown 20ms deadline ("skipped slots carry no lost samples" — false:
`steadyAudioFrameFeed` emits max ONE tick per tick and sheds its FIFO past 6 ticks, so
every skipped slot permanently loses 20ms of real-time audio → recordings' audio track
ran 3.1% short of video, i.e. ~1s of A/V drift per 30s). Now a blown deadline ticks
again immediately (blocks stay exactly 960 frames — spec 4.2 intact) and only re-anchors
past 5 ticks behind (logged). Measured: 48.2 → 50.0 ticks/s, FIFO sheds 0, and the shed
site itself now logs (`AudioFeedState.shedSamples`).

**Headless recording-audio proof (no WinUI, no port 8011):**
`node scripts/validate-record-audio.mjs` — spawns the core over stdio with
`COREVIDEO_ZOOM_ENGINE_PATH` pointed at `corevideo-zoom-engine-fake.exe` (NO binary
copy/restore dance needed for core-only tests; the env var is honored by
`ZoomEngineRuntime::loadConfig`), joins, routes zoom-mix → master, records, and fails
unless audio packets flow AND ffprobe shows video+audio with |start delta| < 50ms and
|duration delta| < 200ms (rig-measured 2026-07-13: 1.8ms / 123ms over 60s @1080p60).
Gotcha it guards: `validate:record-stream` alone proves nothing about audio (headless
master is silent without a source).

## Current state addendum (2026-07-05, the audio war + the soak rig)

**Audio is CLEAN and machine-verified.** The 2026-07-05 marathon: pull-model monitor
(docs/audio-pull-monitor-spec.md - SPSC ring, event-driven render thread, ring-depth
rate trim), Zoom audio rebuilt per docs/zoom-audio-spec.md (128-slot SHM rings,
poll-drain ingest with persistent regions, 1Hz discovery-beacon events, ONE live mix
stream, resumption declick, Z1 exclusive routing: zoom-mix -> program, ISO unrouted by
default). Video ingest: beacons + a dedicated ingest thread (three-phase: peek locked /
snapshot UNLOCKED / publish locked) - **LAW: no pixel work under shared locks or hot
ticks, ever** (it collapsed the audio worker to 8 ticks/s).

**The soak rig (tools/audio/)**: `powershell -File tools/audio/soak.ps1 -Minutes N`
swaps in the fake engine (tone mode: deterministic per-participant sines + 330Hz mix,
COREVIDEO_FAKE_NO_CHURN=1 + COREVIDEO_FAKE_NO_VIDEO=1 for audio soaks), UIA-joins,
Engine On via the control API (:8011), captures taps, runs tone-scan.cjs, prints
SOAK PASS/FAIL, ALWAYS restores the real engine. First SOAK PASS 2026-07-05 (run 18:
clicks:0 on a full-length capture). Debug taps hold files OPEN across ticks (fopen
per tick on the worker costs ~13ms). tap-ring-<key>.f32 = ring-reader output (splits
ring vs downstream).

**Mastering chain M1 + B1** (docs/mastering-chain-spec.md,
docs/master-vst-round2-spec.md §B1): AudioMastering.h on the master bus (trim →
filters → tone → LUFS ride → glue → width → ceiling; mastering{} params on the
audio sync command; ride dB is snapshot telemetry). Topology CLOSED: mastering
applies ONCE on master, pgm-l/r/stream/mon inherit (owner-confirmed 2026-07-06).
B1 (2026-07-19): the ceiling is a TRUE-PEAK limiter (4x polyphase detector,
16-sample lookahead/delay, +0.064ms per 20ms tick measured); glue
ratio/attack/release/makeup are exposed (defaults = old fixed values,
bit-exact); optional 3-band LR4 multiband glue (`glueMultiband`, 200Hz/3kHz,
per-band trims) — single-band stays DEFAULT until the owner's listening pass.
House laws it obeys: every stage bit-identical bypass at neutral, all DSP state
(incl. crossovers per band per channel) persists across ticks.
B2 (2026-07-19, stacked on B1): the master rack PERSISTS — prefs schema **v6**
carries the full mastering block, both A/B slots + active slot, and user-saved
presets (MasteringPresetLibrary; built-in names reserved); restore rides the
ApplyProductionOutputPreferences BACKING-FIELD pattern into the initial full
sync (the O1 vcam shape — property setters would sync a core that isn't up).
Rack meters are the POST-mastering master (`audioMixSession.masterMeter`; the
meter tap sits after processMasteringChain) with target/ceiling guide lines;
the TP meter detector is STREAMING (`streamingTruePeakBlockDbfs` — the
finite-buffer computeTruePeakDbfs rings ~+0.4dB at block edges and must never
drive an operator meter). Rack stages render in DSP order with bright/dim
engage opacity mirroring the exact neutral-bypass conditions (honesty rule:
dim = arithmetically a no-op).
New specs: docs/capture-sources-spec.md (browser sources via WebView2 host process,
screen capture via Windows.Graphics.Capture).

## Current state (2026-07-04)

Working: Zoom video stable under multi-participant churn; program-zoom on the GPU I420
path (zero-copy ingest + 60fps pacer); **GPU core-composited multiview** live (single
shared texture, 4 layout modes, overlay labels/tally/meters/clock, multi-layer PREVIEW
composite bus); **Phase 2 audio/output worker decouple** live (all increments incl. the
lock-hold guardrail + engine sender thread); routing honored by Sources + multiview.

**Audio is REAL (2026-07-03/04, spec `docs/audio-overhaul-spec.md` in delivery):** Zoom ISO
PCM ingest engine→SHM→core→mixer (rig-verified), absolute-deadline 50Hz output pacer,
`RecordingPtsClock` shared-epoch A/V PTS, feedback-loop guard (monitor endpoint ==
loopback endpoint → warning), monitor underrun telemetry. **Audio tab redesign B1–B4
shipped** (`docs/audio-tab-redesign.md`): grid hydrates from the core's published sends
(select-never-destroys), System-default device entries, editable strips + Solo on the
tab, shared routing-matrix panel on both Audio and Routing tabs. Remaining: 4.4 channel
inserts/EQ/gate actually processing, B5 shared strip pop-out, 4.5 VST host.

**Still-media routes render real pixels (2026-07-13,
`docs/sources-redesign-spec.md` §B):** scene routes referencing a media asset used
to composite the colorFromParticipantId placeholder forever — no consumer ever
published a VideoFrame keyed `media:<assetId>`, which made POS-2 logo bugs render
as colored rectangles. `modules/StillMediaFrameCache` (owned by MediaCore) now
decodes STILL images once — kind `image` OR a still extension
(.png/.jpg/.jpeg/.bmp/.gif/.tif; the media bin files PNG logos under lower-third
kinds) — via WIC on a dedicated background worker (never under coreMutex; leaf
mutex only, mirrors the startPluginHostScan law), cached by (path, mtime+size)
with a 64MB LRU budget and a >3840x2160 downscale guard, then injects one
persistent straight-alpha BGRA frame per still into the render gather so program,
preview bus and multiview all match it (stable frameId + shared buffer = zero
per-tick copies/uploads). Alpha works end-to-end: the video-layer blend is
straight SRC_ALPHA on the GPU and blendPixelBgra on the CPU preview — decode to
32bppBGRA, NOT premultiplied PBGRA. Failures are LOUD: missing/undecodable files
keep the placeholder + rate-limited (5s/key) stderr + render-plan warnings, and
`warnUnmatchedCaptureLayer` now also fires for `media:` layers. Both scene parse
sites (load-scene-graph AND set-preview-scene/spine) feed the desired set. The
MF media adapter no longer WIC-decodes route stills on the render thread (it
keeps background stills + video playout). Live VIDEO media routes without active
playout still composite the placeholder — per-route decode sessions are a
follow-up. Test seam: `MediaCore::setStillImageDecoderForTest` injects a fake
decoder (`tests/StillMediaFrameCacheTest.cpp`).

**Scenes redesign S1–S3a + R1 shipped** (`docs/scenes-tab-redesign.md`): layer
delete/reorder/opacity, non-destructive presets + undo, duplicate/no-clobber save,
custom scenes persist across restarts, live-scene DRAFT editing (program untouched until
Update), numeric rect fields + snap guides + arrow-key nudge, and **production roles**
(session-only assignment on the Inputs tab; role-targeted routes resolve at sync time;
the assigned role rides the participant wire to the core director). Remaining: S3b
(aspect-lock, edge handles, selection sync), S4 polish, role templates/automation (R2).

**Direct positioning POS-1 + POS-2 shipped** (`docs/sources-redesign-spec.md` §B):
POS-1 (2026-07-11) put the Scenes canvas editor on the Studio preview header ("Edit
layout" pencil), driving the S2b preview DRAFT. POS-2 (2026-07-12) adds **"Add
overlay" bug placement** on BOTH the preview header and the Scenes tab: pick a media
asset (listed per-asset; empty state is a loud disabled row) or any Add-source option
(inputs/active-speaker/screen-share/roles), pick a corner/center/free preset, and a
NEW top-most route lands in the preview scene at ~15% canvas width, aspect-locked to
the asset's natural size (16:9 fallback), inside a 5% safe-area margin
(`OverlayLayerService` — pure/static, unit-tested; the margin is a constant until the
POS-1 settings increment ships a "default bug margin %" setting). Gotchas encoded in
it: seed rects via `EnsureCanvasRects` BEFORE appending (it re-applies the preset to
EVERY route when any rect is missing — would stomp the bug rect), set
`SourceFramingModified=true` when forcing `FitMode="fit"` (normalization otherwise
resets it), and ALWAYS insert through `GetPreviewEditableRoutes()` (the S2b draft) so
PROGRAM is untouched until Take/Update. The overlay flyouts are rebuilt on `Opening`
(transient menu, not a bound collection — outside the 0xc000027b rules). Remaining in
§B: POS-3 (program-side editing, settings-gated) + the POS-1 settings increment.

In progress / next (2026-07-12): the road to alpha is **verification and stability,
not feature building** — see `docs/alpha-plan.md` (rewritten 2026-07-12) for the
gates: G0 system-audio citizenship re-test (fixes shipped, owner verdict pending),
G1 native UVC default-ON (validated end-to-end on this rig 2026-07-10, still opt-in
via `COREVIDEO_NATIVE_UVC=1`), G2 A/V sync proof (clap test + packaged-run audio
track), G3 a full show drill (record + RTMP + vcam simultaneously, 30-min soak),
G4 stability debt (engine-off teardown audit, OAuth token refresh, resize soak),
G5 packaging-lite. Beta scope (signing/installer/updates, onboarding, licensing,
crash pipeline, hardware matrix) lives in `docs/beta-plan.md`. The audio overhaul
(4.1–4.4b incl. the console) and the Scenes redesign (S1–S3, R1) are SHIPPED; VST
host P1/P2a/P2b/**P2c** are shipped (P3 channel inserts + params remaining).
DONE 2026-07-20: **VST round-2 A2/A3 — params + state + latency compensation**
(docs/master-vst-round2-spec.md §A2/§A3, stacked on #311). Param bridge:
`IEditController` raw COM-ABI in `vst-abi.h` (+ `IBStream` for state, layout
static_asserts). The out-of-process host publishes the active selection's param
surface — first 64 params by controller index + real total count
(id/title/units/plugin-display/step/value) — and drains a latest-wins set-param
ring on a DEDICATED event; the core copies it out of the SHM block only on a
param-generation change → `pluginHost.serve.params[]`; the shell renders generic
sliders in the insert flyout (rebuilt on Opening). The host is the value
authority (`setParamNormalized` on the controller + a queued process change), so
an open editor and the sliders never fight. State persistence:
`IComponent::get/setState` over a raw-ABI **memory IBStream** in the host,
get-state pull command (base64) + set-state over a single-shot **1 MiB** block
area (chunking deliberately NOT built — bounded loud contract; larger states
fail with their size). `host-transport.h` magic bumped **CVP2 → CVP3** (stale
host fails loud). Blobs persist per SELECTION in `ProductionOutputPreferences`
**v6** (one instance per selection in the host), captured on a debounce after
param/editor activity, restored on load, and **re-injected into every host
generation including after respawn** (closes respawn-loses-state). A3 latency
(owner: COMPENSATE): `latencySamples` in the block →
`serve.{latencySamples,latencyMs}` telemetry + per-insert "+N.N ms" badge;
CHANNEL-level compensating delay lines (`AudioDsp.h applyCompensatingDelay`,
declick ramp, default-ON behind `COREVIDEO_VST_LATENCY_ALIGN`) delay dry sibling
channels to the plugin latency; `RecordingPtsClock` latches the content latency
at the first audio buffer (clamped to the epoch) so recordings stay A/V-synced.
DEFERRED honestly: cross-BUS per-path latency attribution (single-slot telemetry
can't express it — a multi-slot protocol follow-up). All param/state traffic
uses SEPARATE events — the 4 ms audio exchange is never stalled. CLI proof:
`corevideo-plugin-host --state-roundtrip <bundle> <class>`.
DONE 2026-07-19: **VST round-2 A1 — editor launch fix + host reliability**
(docs/master-vst-round2-spec.md §A1). Root cause of "Open controls shows no
plugin UI, ever": the shell sends `open-vst-editor` as a TOP-LEVEL RPC and
`JsonRpcServer::handle` had no route for it — protocol-error, silently
discarded by the supervisor. Now routed (+ regression test), the supervisor
surfaces ok:false as status text, the host window opens centered + raised
best-effort (background processes lack foreground rights — topmost pulse +
FlashWindowEx; proven headless with Waves Curves AQ), WM_CLOSE detaches
cleanly (`removed()` before DestroyWindow) and republishes idle status, one
editor at a time. Serve respawn rides `PluginHostRespawnPolicy`
(5→10→20→40→60s, give up after 5 → loud auto-bypass via
`serve.respawn{attempts,gaveUp}` + chip BYPASS; healthy ≥30s runs and operator
actions reset). Headless editor drills: `native/build-dev/probe/` in a
worktree (spawn `--serve`, drive the SHM editor event, EnumWindows the host).
DONE 2026-07-12: **VST P2c — real VST3 instantiation + processing in the
out-of-process host.** Raw COM-ABI (NO VST3 SDK — GPLv3 house rule) in
`native/plugin-host/vst-abi.h` (layout static_asserts) + `vst-processor.h`
(lifecycle/process machinery, factory-injectable for tests). Bus-insert naming:
`vst:<class or plugin name>` (or `vst:<bundle>/<class>` for Waves-style shells)
selects a scanned plugin; plain `vst` keeps the -6dB test processor. Selection
rides the SHM block; the host loads on demand ON ITS OWN THREAD (core
deadline-bypasses during loads) and caches per selection; host status/errors ride
back in the block → `pluginHost.serve{activePlugin,lastError,statusCode}`.
Unresolvable names bypass LOUDLY (never fake). Terminal proof:
`corevideo-plugin-host --process <bundle> <class>` pushes 1s of 440Hz and prints a
process-result JSON verdict. The safety posture is unchanged: 4ms deadline bypass,
bypass-on-host-death, plugin code never in the core.
DONE 2026-07-03: **per-instance engine IPC names (OBS collision fix)** — the engine's
pipes/sockets/SHM regions were fixed names on the shared `ZoomObsPlugin_` base, so a
running OBS zoom plugin made every join time out ("Timed out connecting to Zoom engine
IPC"). `ZoomEngineProcessClient` now mints a `<pid>-<spawn#>` token, passes it via
`--ipc-token`, and both sides splice it into every name (`ipc_pipe_p2e`/`ipc_sock_p2e`/
`ipc_shm_prefix` in `engine-ipc.h`; engine reads it via `ipc_token_from_args` +
`EngineIpc::set_shm_prefix`). Also unblocks two app instances side by side. DONE
2026-07-02: **Phase 2 increments 3+6**
— engine sends now go through `ZoomEngineRuntime`'s outbound queue + dedicated sender
thread (no engine pipe I/O under `coreMutex`; ordering preserved; restart/shutdown
drop+log; dedup at enqueue time) and `core/LockHoldGuardrail` enforces the sub-ms
`coreMutex`-hold contract with rate-capped warnings + per-site telemetry (strict
abort opt-in via `COREVIDEO_LOCK_GUARDRAIL_STRICT=1`); the `native-stub-tsan` CI job
exercises the new sender handoff. DONE 2026-07-02: **overlay/lower-third/caption text
rasterization** —
`OverlayTileRaster::computeOverlayTileLayout` is the single source of overlay geometry;
the CPU preview rasters it with a full-ASCII 5x7 bitmap-font tile
(`rasterizeOverlayTileBgra`), and `D3D11CompositorAdapter::rasterOverlayTexture` renders
the same layout with real DirectWrite text (+ WIC images) via a D2D DXGI-surface render
target into a cached GPU texture (content-signature cache, rig-validated at 60fps);
premultiplied alpha needs the dedicated blend state + overlay shader, and the raster
snapshots/restores the immediate-context state around EndDraw.
