# CLAUDE.md — working guide for CoreVideo Pro

Operational notes for working in this repo. Product/positioning lives in `README.md`
and `COREVIDEO_PRO_PRODUCT_SPEC.md`; this file is the "how to build, run, and not break
it" guide.

## North star (non-negotiable)

Low-latency **and** high-quality A/V is the entire product. It must beat vMix, Ecamm,
and mimoLive. All Zoom is 1080p and up to 60fps — never downgrade quality to dodge a
performance problem; fix the pipeline. CPU per-pixel work does not scale to 8 Zoom + 2
capture @1080p60 — the compositor path must stay on the GPU.

## What this app is

Three processes, not a web app:

- **WinUI 3 (.NET 9) shell** — `native-shell/CoreVideoPro.WinUI/` — the operator console
  (the product). It owns no real-time media; it sends commands and renders shared textures.
- **C++ media core** — `native/` → `corevideo-native.exe` — real-time pixels/PCM:
  D3D11 compositor, audio mixer, recorder, output senders.
- **Zoom engine subprocess** — `native/zoom-engine/` → `corevideo-zoom-engine.exe` —
  speaks the Zoom Meeting SDK, writes raw **I420** frames to shared memory.

IPC: JSON-line commands/snapshots over named pipes; video as keyed-mutex **DXGI shared
textures** (cross-process) for program/preview, and shared-memory I420 for Zoom frames.

Process boundaries + where spine features (ISO/NDI/SRT/browser) plug in: `docs/architecture-seams.md`.

## StudioViewModel strangler (maintainability, FOCUS_PLAN §9)

`StudioViewModel` (the shell god object) is reduced by **vertical-slice extraction** — new
behavior goes in focused `MagicScene*` / `Transport*` types, never new methods on the god
file. **PR1 (done):** `MagicSceneCoordinator` + `IMagicSceneHost` (Magic Scene / Set & Forget
automation) and `TransportStatusFormatter` (pure transport status/rollback/validation
statics). Move-only, XAML x:Bind unchanged (same-named forwarders on StudioViewModel + a
PropertyChanged bridge; StudioViewModel implements `IMagicSceneHost` over `this`, the
Transport/Overlays sub-VM pattern). The extracted types are independently constructible so
they carry real characterization tests (`MagicSceneCoordinatorTests`) — StudioViewModel itself
is still NOT constructible in tests (field-init `DispatcherQueue.GetForCurrentThread()` + ctor
hard-`new()`s ~10 services + launches the core; a later DI-seam PR). **PR2 (done):** the
`IMediaCoreBridge` DI seam + `TransportCoordinator` (`ITransportHost` + `ITransportDispatcher`)
owning the Engine/Take/Record/Stream async command bodies, in-flight guards, #286 rollback,
backpressure-retry, and sender-proof — constructible + characterization-tested
(`TransportCoordinatorTests`). Same move-only façade rules: the `[RelayCommand]` objects stay
generated on StudioViewModel as thin forwarders (XAML + external `NotifyCanExecuteChanged` pokes
unchanged); bound transport state stays `[ObservableProperty]` on the god file, written through
`ITransportHost`. **PR3 (done, stacked on PR2):** the **ShowInputs** cluster —
`ShowInputsCoordinator` behind `IShowInputsHost` (+ injected `IShowInputRosterStore`/`IMediaCoreBridge`)
owning roster persistence, the signature-gated roster→`ShowInputEditors` projection, auto-assign,
unassign/take-offline, SRT-ingest add/remove, and the per-source **ISO selection** (the ISO-4
ISO×ShowInputs integration). The coordinator OWNS `ShowInputEditors` (StudioViewModel exposes it via a
same-named forwarder property so x:Bind is unchanged) + `IsoSelectedSourceIds` (v8 persistence routes
through it); constructible + characterization-tested (`ShowInputsCoordinatorTests`, incl. an
ISO-survives-a-roster-refresh test). The 0xc000027b signature-gating + in-place diff-update + ISO
re-projection are preserved exactly. **Deferred (verification finding):** dual-capture selection is
entangled with capture-fleet enumeration + `[ObservableProperty]`-bound → a future **CaptureFleet**
extraction, not the roster cluster. **PR4+** = the C++ hot core.

## Build & run (Windows)

```powershell
npm run app                       # build best-available native core + build/launch WinUI
npm run app -- -StubOnly          # skip the native core rebuild, just (re)launch the shell
npm run app -- -Rebuild           # force a native rebuild
npm run build:native-dev          # build the dev native core (needs ZOOM_SDK_DIR set)
dotnet build native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj -c Release -p:Platform=x64
```

`ZOOM_SDK_DIR` must point at the staged Zoom SDK x64 dir for the full core. `npm run app`
runs `scripts/app.ps1`; the dev launcher is `scripts/run-studio.ps1` (now respects a
pre-set `COREVIDEO_ZOOM_ENGINE_PATH`).

**Run the binary the build just wrote.** `native/build-dev/` is a single-config
generator — the current binaries are `native/build-dev/corevideo-native.exe` and
`corevideo-native-tests.exe`. A `native/build-dev/Release/` directory also exists,
left by an older VS-generator build, and **nothing updates it**: a test run from
there reported a confident "380 tests passed" from a binary a MONTH old, which
silently omitted every test file added since. The real suite is 529 tests. If a
newly added test does not appear in the output, check which binary you ran before
suspecting CMake.

Logs: `%LOCALAPPDATA%\CoreVideoPro\launch.log` (WinUI) and `media-core.log` (core).
Support bundle (Diagnostics → "Export support bundle"): writes redacted JSON **and a
zip** to `%LOCALAPPDATA%\CoreVideoPro\support-bundles\` — the zip packs ~2MB
secret-filtered tails of launch/media-core/perf/vcam-serve logs + a CrashDumps
listing + manifest (`SupportBundleArchiveBuilder`/`SupportBundleLogRedactor`,
beta spec S2); missing logs become manifest skip notes, never a failed export.

MSIX signing (beta D2, 2026-07-18): `scripts/sign-native-msix.ps1 -Mode dev|production`.
Dev = self-signed, tolerates missing signtool (exit 0 + LOUD unsigned warning).
Production = Azure Trusted Signing (`COREVIDEO_SIGN_DLIB`+`COREVIDEO_SIGN_METADATA`)
or PFX/thumbprint env, RFC3161 timestamp + `verify /pa` + manifest-Publisher match
all REQUIRED — any gap hard-fails (never a silent unsigned artifact). `-DryRun`
prints the resolved plan; tests: `scripts/tests/test-sign-native-msix.ps1`. Full
env contract in the script header and `docs/beta-engineering-spec.md` §D2.

## Testing multi-participant WITHOUT a real meeting (important)

There is a **synthetic Zoom engine**: `native/zoom-engine/fake/fake-engine.cpp` →
`corevideo-zoom-engine-fake.exe`. It emits N participants + animated I420 + roster/
active-speaker churn over the real IPC. Build with `npm run build:native-dev` (or
directly with `cl.exe` via vcvars64 — the CMake VS-generator build can time out).

To drive the full WinUI app headlessly:
1. Stop any instance; copy the fake exe over **all three** `corevideo-zoom-engine.exe`
   paths (`native/build-dev/`, `native/build-dev/Release/`, and the WinUI `…/publish/`).
   **Back each up as `*.realbak` and RESTORE afterward** — the real engine is ~201728
   bytes, the fake ~87040.
2. `npm run app -- -StubOnly`, then drive Join via UI Automation:
   nav Button Name `"Zoom"` → Edit Name `"Zoom meeting URL or ID"` (ValuePattern.SetValue)
   → Button Name `"Join Zoom"`.
Caveat: fake participants populate the roster but only become live GPU tiles when
assigned to Show Inputs — so per-tile crash repros need that assignment.

## The crash class you WILL hit: CoreMessagingXP 0xc000027b

`CoreMessagingXP.dll +0x93b66`, exception `0xc000027b`, **no managed stack** — a native
WinUI fail-fast that bypasses managed handlers (so no first-chance logger catches it).
It is a churn/reentrancy fail-fast, NOT (usually) off-thread access (instrumented: the
off-thread guards never fired). Confirmed and suspected triggers:

- **Frame-rate rebuilds of x:Bound collections.** `OnSurfacesChanged` fires ~100/s; its
  coalesce did not cap the *rate*, so `RefreshSurfaceBindings` ran ~98/s rebuilding
  `MultiviewTiles` wholesale. FIXED by throttling to ~12.5/s (`RsbMinIntervalMs`).
- **Per-tile GPU swap chains.** One DXGI swap chain per multiview participant tile,
  created/reloaded on roster/active-speaker churn, fail-fasts. FIXED by the
  **core-composited single-texture multiview** (`docs/gpu-multiview-plan.md`): the core
  renders the whole grid into ONE keyed-mutex shared texture
  (`D3D11CompositorAdapter::renderMultiview`), presented by a single
  `ShowMultiviewHost` surface with XAML overlays (labels/tally/meters/clock) that
  rebuild only on structural change. The old per-tile CPU grid has been deleted.
- **ComboBox ItemsSource churn.** The Sources editors were keyed on participant `Health`
  (toggles constantly), rebuilding 10 source dropdowns per tick → blank flicker + churn.
  FIXED by keying the signature on the participant id-set only.
- **Window resize (mitigated by design, not soak-verified).** A crash reproduced right
  after a programmatic window resize. `Direct3D11InteropService` now creates the swap
  chain at the **source** size and never calls `ResizeBuffers` on panel resize — it only
  re-applies a matrix scale (`ApplyPanelTransform`), so the resize-vs-present race cannot
  occur by construction. No dedicated regression soak has confirmed it closed; treat any
  resize-adjacent fail-fast as this until the alpha soak passes.

Rules of thumb: never replace a bound collection at frame rate (sync in place / diff);
keep one stable swap chain per surface (program, preview, one multiview);
present with **skip-present** (only on a new keyed-mutex frame) — smooth-present crashes
~31s in.

## Live-meeting QA day (2026-08-09) — eight defects found in ONE real session

An afternoon of the owner operating a real 7-guest meeting surfaced more product
truth than a month of synthetic drills. Each fix carries its full story as a
comment at the code site; this is the index.

- **Zoom video froze ~2s after join — SHM regions cannot GROW on Windows**
  (`engine-ipc.h`, `engine-video.cpp`, `engine-share.cpp`): regions were sized to
  the FIRST ramp frame (256x144); a named section cannot grow while the core
  holds a read handle, so the 640x360→1080p ramp failed silently forever (the
  failure log was gated on frame_count==0). Regions are now allocated ONCE at
  capacity (1080p video / 4K share ≈ 12.4MB, Zoom's ceiling) and both sides log
  loudly on shm failure. NEVER size a shared mapping to the current frame.
- **Leaving a meeting killed the entire studio** (`SettingsViewModel.LeaveZoomAsync`):
  the leave path kill-treed the media core (engine-distrust-era sledgehammer), and
  the supervisor treated it as deliberate → no respawn → endless deferred syncs
  ("unstable" until app restart; core log ends the second the leave runs). A
  meeting is one SOURCE. `_bridge.Stop()` is the app-exit path ONLY. Proof:
  `node scripts/validate-leave-keeps-core.mjs` (join → leave → still rendering →
  rejoin on the same core).
- **Recording restart storm — start-recording-session is IDEMPOTENT per sessionId**
  (`MediaCore::startRecordingSession`): the command rides the REPEATING sync
  channel, and every delivery restarted the writer → with Magic Scene flipping
  scenes ~1/s a live meeting produced 465 one-second shards. Same-id repeat = the
  channel re-asserting state = no-op. The sessionId's ISO suffix is also SORTED
  (`MediaCoreCommandBuilder`) so a roster flap reordering the same selection
  cannot mint a "new" session mid-recording.
- **Zoom ISO audio isolation is REAL — proven against live Zoom**: 7 stems from a
  real meeting; only the talker carried signal, six were digital silence, zero
  pairwise correlation. The per-guest-stems product story holds.
- **…which convicted the meters: they FABRICATED levels** (`AudioDsp.h
  analyzeAudioParticipantFrame`): frames with no PCM got a level synthesized from
  a HASH (pre-real-audio leftover, untested) — seven strips pulsing identically
  while six stems were silence on disk. Meters now show measured PCM or explicit
  producer levels only; no evidence = silence.
- **THE FADER LAW (owner rule): no audio source reaches any bus without a strip.**
  Core: a routed source with no channel strip is DROPPED from the bus mix, loudly
  (`MediaCore` routed-source build; headless callers that sync no console keep
  unity). Shell: `zoom-mix` — the audible Zoom path — was EXPLICITLY excluded from
  getting a strip (`IsConcreteAudioMixSourceId`), which is why muting every fader
  left audio on master. It has a "Zoom program mix" fader now.
- **A throwing DispatcherQueue.TryEnqueue callback fail-fasts the process with NO
  managed log** (`UiDispatch.cs`): three live crashes decoded to ordinary NRE /
  ArgumentOutOfRange inside queued callbacks (stowed 0x80004003 / 0x8000000b at
  DeferInvokeCallback). ALL queued UI callbacks now route through `UiDispatch`
  (log-with-stack + survive). A raw `TryEnqueue` with a throwing body is a
  process-killer — never add one.
- **Sources kept reverting — it took THREE kills, one writer per report.**
  (1) auto-assign refilled operator-removed guests every sync
  (`ShowInputsCoordinator`): the fill pass now only places ids it has NEVER seen
  this meeting (real newcomers); flipping the auto-assign toggle explicitly
  reassigns everyone. (2) `EnsureAssignedSlotsForInShow` stuffed the first
  participant/first connected webcam into any in-show-but-unassigned slot every
  refresh — an unassigned slot now just leaves the show ("NEVER INVENT A
  SOURCE"). (3) the VESTIGIAL dual-capture selection
  (`StudioViewModel.ApplyDualCaptureSelection`) force-wrote the auto-picked
  primary/secondary capture devices (the local webcams) into ShowInputs[0]/[1]
  — slots 1-2 — on EVERY capture-fleet pass (device-watcher event, Inputs-tab
  visit, capture connect), with no UI bound to it at all, and the roster save
  then persisted the stomp; the slot write is deleted
  (`ShowInputAssignmentLawTests.TheDualCaptureSlotStufferStaysDead`). THE LAW:
  sources appear in slots by OPERATOR action or newcomer auto-assign ONLY.
  Enforcement: every `ShowInputSlot` setter logs `slot-write: slotN field
  old->new by=<reason>` with the ambient `ShowInputWriteScope` reason — an
  UNTRACKED slot-write in launch.log is a bug (wrap the writer in a scope). The
  roster also saves SYNCHRONOUSLY on every editor-observed change (the old save
  rode only the coalesced Low-priority refresh, so a crash lost the operator's
  pending change), and `LoadShowInputRoster` refuses a second load (persisted
  state restores ONLY at startup). Also: `DefaultMaxVideoSubscriptions` was 6, so
  the 7th+ camera-on guest was silently never subscribed — now 8 (the product's
  advertised feed count; the engine's downgrade ladder handles SDK refusals
  loudly). And `Selector.SelectedValue` must never be driven by x:Bind inside an
  ItemsRepeater template (`SourcesInputsPage` role ComboBox crash) — apply
  selection on Loaded, guarded.
- **Meters clipped when not fullscreen** (`AudioLevelMeter`): fixed-size segments
  (36×9px = 324px minimum) overflowed smaller windows, clipping the GREEN end.
  Segments now scale (spacing → size → count) and re-fit on resize.
- **Transport buttons had no `AutomationProperties.Name`** — screen readers and
  UIA (including our own tooling) could not find Record/Stream/VirtualCam. Named
  now; give every new interactive control an automation name.

## Other gotchas

- **Borders are MULTIVIEW-ONLY — they NEVER composite into program/preview
  (owner rule, 2026-07-31).** Borders exist to separate tiles in the multiview
  (which sets its own explicit accent/program tally borders in
  `buildMultiviewRenderPlan`). Route borders used to default to "accent" (studio
  green, thickness 2 → `computeBorderFraming` ≈ thickness/200 → ~11–19px at
  fullscreen 1080p) and composited INTO THE PROGRAM — the virtual camera,
  recordings, and streams all inherit the composed program, so every output showed
  a green outline ("webcam out green border" regression). Now enforced in layers:
  `buildRenderPlanForScene` hard-forces `borderStyle="none"`/thickness 0 on every
  route layer (program AND preview bus), every default is "none", the Sources
  layer editor has no border controls, and `ScenePersistenceService.FromPersisted`
  retires stale persisted styles to "none". Regression tests:
  `MediaCoreCommand.RouteBordersNeverCompositeIntoProgram` (core — explicit
  "accent" composites identically to "none") and
  `ScenePersistenceServiceTests.DefaultRouteBorderIsNone` (shell). Never render a
  visible adornment on the program path outside the multiview grid.
- **A ONE-SHOT COMMAND MUST BE RE-APPLIED ON EVERY CORE GENERATION (2026-08-08).**
  The core is respawned by the supervisor whenever it dies, *under a live shell*.
  Anything the shell sends once at launch is **silently lost** on that respawn, and
  the fresh core answers with its DEFAULT — which is usually a legal value, so
  nothing looks wrong. This shipped as "the multiviewer is broken":
  `configure-multiviewer` was sent only by `StartMediaCoreOnLaunchAsync`, so a
  respawned core sat on `multiviewLayoutMode_ = "grid"` while the shell still
  believed `pgmPvwTop`. The **PROGRAM and PREVIEW bus cells vanished off the top of
  the wall** and it degraded to a bare source grid. It presented as FIVE separate
  bugs — buses gone, layout wrong, tiles blank, tile-click-to-preview dead, preview
  layer editor dead — but click-to-preview and the editor were fine all along;
  with no PVW cell there was nowhere to show their result. The source roster
  survived because `set-multiview-layout` rides the frequent spine sync, which is
  what made it look like a layout bug rather than a lost command.
  **Fix pattern:** `MediaCoreSupervisor` fires `ProfileChanged` on every core
  generation (initial handshake AND respawn) — re-arm from
  `StudioViewModel.OnBridgeProfileChanged`, reusing the existing debounce rather
  than adding a second retry mechanism. **And make it observable:** `sessionState()`
  publishes a `multiviewer` node with the APPLIED config, unconditionally — a node
  that only appears once configured is absent in exactly the case worth detecting.
  Audit any other launch-time one-shot against this rule.
  Repro (this is the acceptance test): with a healthy wall up, `Stop-Process` the
  `corevideo-native.exe` and watch the wall after the supervisor respawns it.
  Headless oracle: `node scripts/validate-multiview.mjs [--sources N] [--mode M]`
  judges the published wall (PGM + PVW cells, N source tiles, 16:9 in-canvas
  non-overlapping rects, and that the core echoes the configured mode). It proves
  STRUCTURE, never pixels — the event carries a GPU handle, not a frame.
- The WinUI window often **opens minimized off-screen** (rect ≈ -32000,-32000). Restore
  gently with `ShowWindow(SW_RESTORE=9)`; do NOT aggressively maximize/move a
  SwapChainPanel window across monitors — it can kill the window (and resize can crash).
- 60fps needs `timeBeginPeriod(1)` (Windows 15.6ms timer granularity) + a frame-budget
  pace, both already in `JsonRpcServer.cpp`. The deadline accumulates from a FIXED
  ANCHOR with bounded catch-up (a relative `t0 + budget` deadline can only lose time —
  each overshoot becomes the next frame's start), and the post-timer spin tail is
  **200µs**: the old 500µs only existed to mask that drift, and re-measured on this rig
  it costs ~5s of core CPU per 53s wall for nothing. Never raise the guard to paper over
  a pacing bug.
- **Program recording muxes the NV12 program TAP, never `ProgramFrame::preview`**
  (fixed 2026-08-06). `preview` is a **320x180 UI thumbnail**; writing it into a
  writer opened at program dimensions put the entire show into a small corner of a
  black frame — and shipped that way for months (a 2026-07-13 recording: 8995
  frames, flat luma 4/255) because every validator checked stream presence and
  container alignment and **none ever looked at a pixel**. Windows has no full-res
  BGRA readback (that would be 8MB/frame); the full program exists only as NV12
  from `compositor->takeVcamNv12` — the same tap the vcam and RTMP consume — so
  MediaCore takes it ONCE per output tick, attaches it to `work.programFrame`
  BEFORE `encoder->submit`, and the senders inherit it via their copy. Gated by
  `RecordingSessionRequest::programNv12`, which MediaCore sets only when
  `ICompositor::suppliesProgramNv12()` AND the recording is exactly 1080p (the tap
  is a pinned 1080p scale-blit; feeding it to a 4K writer would letterbox — a
  non-1080p recording still takes the old path and is still wrong). macOS is
  unaffected: Metal publishes `programFullBgra` and AVFoundation already read it.
  **When touching the recording path, verify PIXELS** (mean luma of the output),
  not just that frames were written.
- **Zoom ingest runs a FRAME SYNCHRONIZER** (`ZoomEngineRuntime::frameSync_`, owner
  decision 2026-08-06) — the one-frame cushion every hardware switcher input has. A
  latest-wins slot cannot absorb a ~60Hz source and a ~60Hz render free-running against
  each other: ~1ms of jitter puts two frames in one render interval (one destroyed
  unseen) and none in the next (a repeat) — measured 6–10% each way. The cushion is
  built UP FRONT (prime to 2 queued, then serve 1 per fetch); a catch-up buffer that
  fills after the fact was measured and does NOT work, because the starved tick comes
  first. Result: 0.0% overwritten, 99% delivery, for a deliberate +16.7ms. Capped at 3
  deep — sustained overflow drops the OLDEST so latency never accumulates.
  `COREVIDEO_FRAME_SYNC=0` restores latest-wins and is the A/B control; keep it working.
  Note audio now leads video by one more frame in all outputs (within the 50ms G2
  budget; confirm on the clap test).
- **The perf drill is `scripts/mac-show-drill.py` and it runs on Windows** (despite the
  name — it gates SHARED core code, so it must run on every platform that ships it):
  `python scripts/mac-show-drill.py --seconds 40 --load 8` drives N synthetic 1080p60
  Zoom feeds through the real ingest path and gates sustained fps, dropped frames,
  frame DELIVERY, ingest→render latency percentiles, and coreMutex over-budget ratio.
  Defaults to `native/build-dev` + `.exe` here (`COREVIDEO_BUILD_DIR` /
  `COREVIDEO_FAKE_ENGINE_PATH` override). **Mean fps is not a health metric** — an 8ms
  ingest poll silently dropped ~22% of decoded frames while fps read a healthy 60
  (`docs/windows-perf-handoff.md` has the full before/after). Always confirm the fake
  engine actually delivered the rate you asked for (`COREVIDEO_FAKE_ENGINE_LOG`), and
  run `git status` before any measurement build — a stale tree answers a different
  question than the one you asked.
  **The drill now enforces that "did the harness source the load" check itself**
  (2026-08-07): delivery is (frames the compositor saw)/(frames we ASKED for), so a
  harness that under-produces reads as the CORE losing frames. It said "only 51% of
  decoded frames reached the compositor" on a macos-14 runner that sourced ~250 of
  480 frames/s; the same drill on a real box sources 479 f/s (1.49GB/s) and delivers
  101%. A >10% shortfall is now named as a HARNESS failure (still a failure — the run
  proved nothing). **The loaded step is therefore ADVISORY on CI and BLOCKING on real
  hardware**: sizing CI down to `--load 3` scored *worse* (45.2fps vs 59.3), so shared
  runners cannot gate perf at any load. Run `--load 8` locally before shipping perf work
  — that is the real gate.
- I420→RGB is a GPU HLSL shader in `D3D11CompositorAdapter.cpp`
  (`kCompositorYuvPixelShader`, BT.709 full-range). Zoom frames carry I420
  (`hasI420()`), NOT BGRA — any frame merge/match must check `hasI420()` too or Zoom
  renders blank (see the `renderSyntheticTick` engine-roster merge).
- Audio/output no longer rides the render lock — **Phase 2 shipped**: a dedicated
  ~50Hz worker (`JsonRpcServer` `audioOutputThread`) runs
  `MediaCore::renderAudioOutputTick` with a strict two-lock discipline: `coreMutex`
  briefly for gather/publish, `audioOutputMutex_` for the long DSP/device/network span,
  NEVER both nested on the worker. The render thread is video-only
  (`renderDisplayTick`), and an empty `media-core-sync` poll returns the published
  snapshot without a tick. When touching audio/output control-plane commands, keep the
  `coreMutex` → `audioOutputMutex_` lock ORDER (see `docs/phase2-threading-plan.md`);
  a single missed `audioOutputMutex_` guard is a data race. Engine pipe writes go
  through `ZoomEngineRuntime`'s outbound queue + dedicated sender thread (increment 3)
  — never call `process_->sendLine` directly. Full lock order:
  `coreMutex` → `audioOutputMutex_`, and `coreMutex` → `ZoomEngineRuntime::mutex_` →
  `::sendMutex_` (never reversed). `coreMutex` holds are budgeted sub-ms outside
  sanctioned sites — `core/LockHoldGuardrail` warns (rate-capped) on violations.

## Secrets at rest + OAuth return URI (beta S4, 2026-07-18)

- **Credentials at rest use DPAPI** via `DpapiSecretProtector` (WinUI, CurrentUser
  scope, `"dpapi:"+base64` field-level format): Zoom OAuth tokens
  (`FileZoomTokenStore` encrypt/decrypt delegates) and the RTMP stream key / SRT
  passphrase in `production-output-preferences.json` (prefs schema **v4**). Legacy
  plaintext files load fine and re-save encrypted on first load (never lose a working
  token). Any NEW persisted credential must ride the same delegates — never write a
  secret plaintext, and give every new secret-bearing bundle field a redaction test
  (`SupportBundleExportTests` is the template).
- **The OAuth app-return URI is `corevideo://oauth/callback` and is broker-pinned**:
  the deployed broker (`corevideo.iamfatness.us`, `site-worker.js handleOauthStart` in
  the external CoreVideo repo) 400-rejects any other `return_uri`. `corevideopro` is a
  legacy protocol alias only. Don't change the scheme without updating the broker
  allowlist first; `ZoomOAuthManifestTests`/`ZoomOAuthProtocolTests` pin it.

## Engine teardown order (the ZoomISO deadlock class — G4, 2026-07-18)

The reference product (ZoomISO) froze in production because teardown destroyed
renderers before stopping raw data with a callback in flight. Hard rules:

- **Exit order in the engine:** `Leave()` → `meeting_event.stop_raw_media("shutdown")`
  (raw-media off + unsubscribe_all across video/share/audio) → bounded message-pump
  drain (~250ms, never unbounded) → `share_engine.detach()`/audio shutdown →
  `CleanUPSDK()`. Renderer destructors must NEVER run after `CleanUPSDK()` —
  `EngineVideo` is a stack local in `main()`, so the explicit stop is what
  guarantees that.
- **Callback vs teardown must serialize.** `~ParticipantSubscription` sets
  `m_stopping` first, drains `m_targets_mtx` (acquire+release), THEN
  `unSubscribe()`/`destroyRenderer()` — do not hold a mutex across those SDK
  calls (they may wait on a callback that takes the same mutex). EngineShare's
  single-mutex callback/teardown pattern is the other accepted shape.
- **EngineVideo's subscription maps are guarded by `m_mtx`**, but
  `ParticipantSubscription` build/destroy makes SDK calls
  (`createRenderer`/`destroyRenderer`) and so runs OUTSIDE the map lock — move
  unique_ptrs out of the map under the lock, construct/destroy after release.
  Lock order: `m_mtx` → `m_targets_mtx` (leaf, never reversed).
- **Shell: stop off the UI thread.** `_bridge.Stop()` is a kill-tree +
  `WaitForExit(1500)` under the supervisor gate — it always rides `Task.Run`
  (both leave-meeting in `SettingsViewModel` and app-exit in `MainWindow`).
- **Never delete the vcam SHM file in `stop()`** — same hard rule as the
  virtual-camera section below; stop only unmaps/closes handles, the writer
  re-opens IN PLACE on the next start.

## Virtual camera (program feed → a webcam for Zoom/Teams/OBS)

The program appears system-wide as **"CoreVideo Pro Camera"** at native **1080p60**.
It is an out-of-process, user-mode COM Media Foundation source DLL
(`native/virtualcam-dll/` → `corevideo-virtualcam.dll`, CLSID
`{8B4B2C9E-2C4A-4E1D-9C7A-CDEF01234567}`) that the Windows **Frame Server** loads on
demand; the core registers it as a virtual camera via `MFCreateVirtualCamera`.

Pipeline: **core → cross-session shared memory → DLL → Frame Server → app**.

- **Cross-session shared memory is the whole trick.** The core publishes from the user's
  **session 1**, but the Frame Server serves the camera from the **session-0** `FrameServer`
  svchost — so a `Local\`-named mapping is a *different* object in each session and the DLL
  only ever saw the standby slate. Non-elevated processes can't create a `Global\` object
  (`SeCreateGlobalPrivilege`). **Fix = a file-backed mapping** at
  `%ProgramData%\CoreVideoPro\vcam-frame.shm` with a permissive DACL
  (`D:(A;;FRFW;;;WD)(A;;FR;;;AC)` — Everyone + ALL APPLICATION PACKAGES,
  `FILE_ATTRIBUTE_TEMPORARY` so it stays in cache). Same path in every session → the OS
  keeps it coherent. See `openVirtualCameraShmFile`/`mapVirtualCameraShmView` in
  `native/src/modules/VirtualCameraShm.h`; used by the publisher (writer), the DLL's
  `SharedFrameReader` (reader), and the round-trip test. Layout: 32-byte header
  (magic `0x43564643`, then seqlock `seq`/`w`/`h`/`fps`/`byteLen`/`frameNumber` as u64)
  followed by an NV12 payload; the writer uses a seqlock, the reader retries on an odd seq.
- **No flashing:** the DLL caches `lastGood_` and re-serves it on a transient read miss;
  it only falls back to the slate after ~30 missed frames (`MediaStream.cpp`).
- **THE SOURCE PACES DELIVERY (2026-07-12).** The pipeline requests the next sample the
  moment the previous completes — completing `RequestSample` immediately free-runs the
  serve chain at CPU speed (measured ~2000 samples/s = ~6GB/s of 3MB copies through the
  Frame Server + every consumer; Zoom's video process burned 8+ cores and system audio
  glitched whenever the camera was consumed). `MediaStream::RequestSample` now waits
  until the next frame is DUE (high-res waitable timer; plain Sleep quantizes to ~40fps).
  Verify cadence in `%ProgramData%\CoreVideoPro\vcam-serve.log` (Fill lines ≈ 1/s = 60/s).
- **NEVER delete the SHM file** (`openVirtualCameraShmFile`): readers hold the file
  object via FILE_SHARE_DELETE; delete+recreate orphans them on the unlinked file and
  they degrade to frozen frames / the slate forever (program/slate strobing when a stale
  and a fresh instance interleave). The writer opens IN PLACE and re-asserts the DACL
  (`SetKernelObjectSecurity`); the reader self-heals by re-opening by path after ~1s of
  frozen seq (`SharedFrameReader::kReopenAfterUnchangedReads`).
- **Serve diagnostics:** the DLL logs to `%ProgramData%\CoreVideoPro\vcam-serve.log`
  (pre-created by the publisher with a permissive DACL — locked-down Frame Server
  workers cannot write `C:\Windows\Temp`, which left the serving side unobservable).
- **No latency drift:** the DLL stamps each sample with `MFGetSystemTime()` (a live source),
  never an accumulating `nextPts_ += frameDuration_` counter.
- **Dims must match.** The DLL media type is **fixed 1920×1080@60** (`MediaSource.h`), so
  `MediaCore::syncVirtualCamera` HARD-PINS 1920×1080@60 and ignores the shell's command
  w/h/fps — a mismatch makes the DLL reject the frame → slate.
- **Off-thread readback (why it's ~free).** Reading a 4K program back on the render thread
  froze Take/preview (~20ms under `coreMutex`); on the audio worker it starved audio. The
  fix: on the render tick the compositor does a cheap GPU **scale-blit** of the program
  into a *dedicated* 1080p keyed-mutex shared texture (`exportVcamSharedTexture`,
  fullscreen-triangle identity draw — do NOT reuse the program `sharedTexture_`, WinUI
  already holds its keyed mutex and a third consumer deadlocks). A **second D3D device** on
  its own thread (`vcamTapLoop`) does AcquireSync/CopyResource→staging/Map/NV12-convert, and
  the output worker just does a cheap NV12 copy (`takeVcamNv12`). Net render
  cost ≈ 1ms. Rule: GPU→GPU `CopyResource` is microseconds; GPU→CPU-staging map+read is
  ~8–12ms and MUST live on a dedicated device/thread, never under `coreMutex` or the audio
  worker.
- **THE TAP THREAD PUBLISHES — never the output worker (2026-08-07).** The vcam used to be
  published from the ~50Hz audio/output worker, whose 20ms period is an AUDIO constant
  (960 samples at 48k). Gating video on it capped a 60fps program at **50fps** and added up
  to 20ms of quantisation to a path whose entire budget is one 16.7ms frame — measured:
  render 59.7fps, output worker 49.7Hz, **vcam published 50.0fps**. It publishes through
  `ICompositor::setVcamFrameSink` on the tap thread now (**59.9fps** measured, matching the
  DLL's declared 60). `MediaCore` must NOT also publish when
  `compositor->publishesVcamFrames()` or every frame goes out twice, and `~MediaCore` MUST
  clear the sink — `modules_` is declared before `virtualCamera_`, so the publisher dies
  first while the tap thread is still running. Note the OLD claim here ("the last ~10fps is
  the scalar `convertBgraToNv12`") was doubly stale: the GPU convert had already shipped,
  and the real cap was the worker cadence. Verify with
  `node scripts/measure-program-out-latency.mjs`, which reads the same seqlock header the
  DLL reads and attributes the published rate to a stage.
- **PROGRAM VIDEO HAS ITS OWN 60Hz TICK (2026-08-07).** `encoder->submit` used to run on the
  ~50Hz audio worker, so recordings muxed **49.9fps** of a 60fps program — measured properly
  with ffprobe on identical 25s content: **1251 frames before, 1495 after (59.7fps)**.
  `JsonRpcServer` now runs a `videoOutputThread` at 60Hz driving
  `MediaCore::renderVideoOutputTick`, and the audio worker submits **audio only** (guarded by
  `videoOutputTickRunning_`, so direct/unit-test callers keep the old synchronous path).
  Lock order is unchanged and MUST stay so: `coreMutex` (brief snapshot of `lastProgramFrame_`)
  → `audioOutputMutex_` (encoder), never both at once, never reversed — the two workers
  serialise on `audioOutputMutex_`, which the audio side holds only ~13% of the time
  (`work=2.6ms` per 20ms tick). Do NOT instead raise the audio worker to 60Hz: that breaks
  the 960-sample block contract (spec 4.2) its pacer exists to hold.
  **`takeVcamNv12` yields each tap generation exactly ONCE**, so only the video tick may take
  it; it leaves the newest frame in `latestProgramNv12_` and the audio worker reads that for
  the senders. Two callers would starve each other.
  Measured end state: render 59.9fps, video tick 59.8/s, audio worker 50.0/s, vcam 60.0fps.
- **THE VIDEO TICK IS SIGNALLED, NOT PACED (2026-08-08) — three designs were measured and
  only the third is correct.** It waits on `videoOutCv_` until the render thread publishes a
  new program frame (bounded 20ms so it can still deliver a sender stop when the program is
  idle), so the wait IS the pacing.
  1. **60Hz pacer — WRONG, and dangerously plausible.** A 60Hz sampler against a 60Hz
     producer is the frame-pairing problem the Zoom synchroniser exists to fix: it muxed
     **51.7fps** of a 60fps program. The same build on another run read 59.7fps, because it
     depends on the phase the two threads start in — so a single green measurement proves
     nothing here.
  2. **120Hz pacer — fixes the aliasing, breaks the show.** Sampling above Nyquist works,
     but the extra `coreMutex` acquisitions dropped the 8x1080p60 drill to **57.4fps** with a
     **141ms** command p99.
  3. **Condition variable — correct.** One wakeup per real frame: 59.9fps recorded (three
     consecutive runs), drill 60.0fps at 4.3ms hold, command p99 **47.4ms** (BETTER than the
     51.2ms baseline).
  **NEVER notify under `coreMutex`.** The first CV attempt signalled inside the render lock,
  waking a thread that instantly blocked on the lock still held — command p99 51ms → 107ms.
  `MediaCore::notifyProgramFramePublished()` is called by `JsonRpcServer` AFTER the lock
  scope closes, and must stay there.
- **Counters that count SUBMITS are not frame rates.** `recording.proof.programFrameCount`
  counts submits, so it read ~50/s and looked like the muxed rate; it also read 911 on a
  30fps SRT source whose file held 498 frames. When judging a recording's rate, count frames
  in the ARTIFACT (`ffprobe -count_frames`) over its duration — same discipline as "verify
  PIXELS, not stream presence".
- **THE SENDERS ARE SPLIT TOO (2026-08-08): video on the 60Hz tick, audio on the audio
  worker.** FFmpeg takes program video and program audio through **two separate inputs**
  (a rawvideo pipe and a PCM pipe), so they never had to arrive in one call — but
  `sync()` carried both, which pinned the whole stream to the ~50Hz worker. Now
  `renderVideoOutputTick` calls `sync()` (video + destinations + settings) and the audio
  worker calls the new `IOutputSender::submitAudio`. Measured: sender fed **50.0fps
  before, 60.0fps after**.
  Three things this required, each a trap on its own:
  1. **A LAYOUT DECLARES AUDIO — the sender must NOT wait for PCM to learn it exists.**
     This is the one that cost a full debugging round. The FFmpeg arg list bakes in the
     audio input, so if the first `sync()` carries no PCM the process starts with
     `anullsrc`; when audio then arrives it must RESTART — and **an SRT listener accepts
     ONE caller**, so the reconnect is refused (`Connection to srt://... failed: I/O
     error`) and the stream never recovers. It was intermittent because it depended on
     whether the first PCM beat the first sync. `sync()` now latches the layout from
     `audioChannels`/`audioSampleRate` ALONE (`haveRealAudio_`, sticky, never cleared by a
     video-only call), and `renderVideoOutputTick` passes the layout whenever
     `audioRoutingSends_` is non-empty. **Read FFmpeg's own stderr log
     (`ffmpegStderrPath_`, a temp file) before theorising about the sender** — it named
     this in one line after an hour of guessing.
  2. **`Kind::Audio` is never dropped** in `AsyncOutputSender` — video is state (newest
     wins), audio is a timeline. Queued audio MERGES into the newest pending audio item,
     capped at 5s, and never clobbers the session snapshot.
  3. **The video tick must run one tick past the last destination** (`senderSyncActive_`):
     senders are STOPPED by a `sync()` carrying no destinations, so returning early the
     moment outputs clear would strand a live stream running.
  Direct/unit-test callers (no video tick) keep the original single-call path behind
  `videoOutputTickRunning_`.
- **Gate the sender's cadence on its BEST interval, not the median.** The defect is a
  STRUCTURAL cap — video fed from the ~50Hz audio worker can never exceed ~50 on any
  interval (main measures 49.8 median / 50.2 best). A busy machine makes
  `AsyncOutputSender` coalesce and dip (46–53fps observed mid-build), which a
  median-based gate reports as the same failure. The peak separates "capped" from
  "loaded". `validate-srt-output.mjs` also needs a listener head start before the core
  calls — and **never probe the port with a UDP bind to test readiness**: SRT is UDP, so
  the probe steals the port from the listener it is waiting for and turns an intermittent
  race into a reliable failure (tried it; it made things worse).
- **A STREAM'S CONTAINER FPS CANNOT PROVE ITS CADENCE.** FFmpeg pads duplicates up to its
  declared `-r`, so a sender fed at 50fps still emits a stream that ffprobe reads as
  **59.9fps** — identical to a healthy one. The defect is only visible in the sender's OWN
  accepted-frame counter (`framesSent`: ~250 per 5s interval at 50Hz, ~301 at 60Hz), which
  is what `validate-srt-output.mjs` now gates. Same family as the recording counter that
  counted submits: **measure the thing, not a proxy that survives the bug.**
- **Enable it:** control API `POST http://127.0.0.1:8011/invoke
  {"action":"transport.virtualcam.set","args":[true]}` (or the transport toggle in the UI).
- **Verify the feed:** read the 32-byte header of the ProgramData file; `frameNumber`
  delta/sec = the publish fps.

**Rig ops for the DLL (READ before rebuilding it):**
1. Registration is HKCU (no admin): `scripts/register-virtualcam.ps1`.
2. **Rebuilding the DLL needs the app stopped AND the Frame Server restarted elevated** — it
   holds an image-section handle to the registered DLL, so the relink fails with `LNK1104`
   even though `tasklist /m` shows no holder. `Start-Process powershell -Verb RunAs
   -ArgumentList 'Restart-Service FrameServer -Force'` (owner approves the UAC).
3. Build target: `cmake --build native\build-dev --config Release --target
   corevideo-virtualcam corevideo-native corevideo-native-tests`.
4. `native/virtualcam-dll/VcamLog.h` is gated serve-tracing for debugging the DLL side.

## Zoom capture on/off (engine raw-media stop — 2026-07-19)

Capture-off must stop raw media IN THE ENGINE, not just our spine payloads:
the shell's Capture toggle OFF sends `zoom-stop-capture` (in addition to
`ConfigureZoomSpineSync(null)`) → core `MediaCore::stopZoomCapture` →
`ZoomEngineRuntime::stopCapture` enqueues `stop_media` on the sender thread
(never a direct pipe write) → the engine's command loop runs
`stop_raw_media`: `StopRawRecording()` (this is what clears Zoom's
participant-facing recording indicator) + `unsubscribe_all` across
video/share/audio. Without it the recording banner stayed up and frames kept
flowing after the button went red. `stopCapture` also clears the
`sentSubscriptions_` dedup + `mediaStarted_` so Capture ON re-arms through the
EXISTING path (spine `startCapture:true` → `ensureMediaStartedLocked` →
`start_media` → engine `start_raw_media` re-request + resubscribe). The engine
emits a first-class `raw_media_status {active}` event on every start/stop; the
core mirrors it as `rawMediaActive` in the zoom snapshots and the shell's
Capture status line reflects that engine-reported truth ("Capture stopped —
Zoom recording indicator cleared"), polling briefly until confirmed — never
claiming stopped on hope. (This section belongs with the engine-teardown rules
from PR #302 once that lands.)

## Browser sources (BR-1, 2026-07-13 — render-only URL sources)

`docs/capture-sources-spec.md` §4 status block has the full shape. The short version:

- **One `corevideo-browser-host.exe` per source** (`native/browser-host/`, the
  zoom-engine isolation pattern — page content never runs in the core). It renders the
  URL in a windowed WebView2 controller inside a borderless WS_EX_TOOLWINDOW positioned
  OFF the virtual desktop, **WGC-self-captures its own window**, and publishes BGRA into
  a `Local\CoreVideoPro.browser.<pid>.<n>` seqlock SHM (`BrowserSourceShm.h` — same
  layout as the WinUI capture bridge). **Real alpha survives** (rig-verified: transparent
  page regions arrive premultiplied alpha=0), so graphics tools key over program.
- **Two non-obvious host requirements:** Chromium marks offscreen windows occluded and
  throttles rAF to ~1fps — the host passes
  `--disable-features=CalculateNativeWinOcclusion` (+ background-throttling off), which
  is what makes offscreen rendering sustain ~28fps; and the window must be per-monitor
  DPI-aware with `RasterizationScale` pinned to 1.0 or CSS pixels ≠ frame pixels.
- **Core side:** `BrowserSourceHostAdapter` (owned directly by MediaCore, not in
  ModuleSet) — commands `browser-add {url,width,height,fps}` / `browser-remove` /
  `browser-reload {browserId}`; frames keyed `capture:browser:<n>` merge into the
  capture stream, sources enumerate as capture devices (vendor "browser"), health in the
  snapshot `browserSources` node. Supervision mirrors `CaptureReaderStallPolicy`:
  host death → LOUD stderr + last frame held 2 s → slate, respawn with 5→10→20→40→60 s
  backoff, **give up after 5 consecutive failures** (operator reload resets). Spawns run
  on the adapter's supervisor thread, never under `coreMutex`. URLs are validated (no
  quotes/whitespace/control chars; http(s)/file/data only) because they ride a
  CreateProcessA command line. Host stdin is the control pipe: `reload\n`, EOF = quit
  (no orphan hosts).
- **Shell:** Sources tab "Add browser source" (URL + preset), Browser group in the
  unified picker (`ShowInputKind.Browser` is capture-class everywhere), control API
  `browser.add` / `browser.remove` / `browser.reload`.
- **WebView2 SDK is VENDORED** at `third_party/webview2/` (NuGet 1.0.3800.47, BSD-style
  license, static loader — provenance in its README). The evergreen **Runtime** is
  probed at host startup and missing-runtime fails loudly (exit 3).
- **Self-test / render proof without the app:** run the host standalone —
  `corevideo-browser-host.exe --url <url> --width 640 --height 360 --fps 30
  --dump-bmp out.bmp --dump-after-ms 8000 --exit-after-ms 10000` prints machine-checkable
  pixel stats (mean BGRA + samples) and a capture-fps line every 5 s.
- **Known BR-1 limits:** per-frame CPU copy on the render tick (same cost as one bridge
  capture device; BR-1.5 = keyed-mutex shared texture), no page audio (BR-3), no
  interactivity/zoom/custom CSS (BR-2), navigation is unrestricted (popups/downloads are
  blocked). Static pages deliver ~0 fps by design (WGC fires on change; the core
  re-serves the held frame).
- **Build gotcha honored:** the new exe is in BOTH the cmake `--target` list AND the
  staging list in `scripts/build-native-dev.ps1`. Same-change fix: that script no longer
  aborts after a FRESH zoom-SDK stage (`$LASTEXITCODE` was null → treated as failure).

## SRT ingest (contribution feeds IN — video + embedded audio, 2026-08-07)

SRT is required in BOTH directions for a pro AV product; delivery shipped first
(`SrtFfmpegArgs.h`), this is the INGEST half. A remote guest/encoder pushes an
MPEG-TS/SRT stream at us and it becomes an ordinary capture source.

- **Shape:** one **ffmpeg decoder subprocess per channel**
  (`modules/SrtIngestCaptureAdapter.cpp`), never libsrt in the core. Video comes back
  as raw **BGRA on stdout** at the channel's configured size/rate and merges into
  `videoFrames` keyed `capture:<deviceId>` — so scenes, multiview, ISO, recording and
  every sender treat it exactly like a camera. Decoders run under a **job object**
  (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`) so a core crash can't orphan an ffmpeg still
  holding the SRT port.
- **Embedded audio is a SECOND output on the same ffmpeg** — `-map 0:a:0? -vn -f f32le
  -ar 48000 -ac 2` into a **Windows named pipe** the adapter serves
  (`\\.\pipe\corevideo-srt-ingest-audio-<pid>-<n>`; POSIX hands the child an inherited
  fd and uses `pipe:3` — no FIFO file, **not verified on hardware**), drained by a reader thread into
  a ~1s cap (drop-oldest) and emitted from `pollAudioFrames` keyed
  **`capture:<deviceId>` — the same id as the video**, which is what makes it land in the
  existing routing/metering/ISO paths with no special-casing. A contribution feed carries
  its guest's audio inside the transport with no OS device to pair, so it cannot use the
  WASAPI capture-audio path.
- **NO SHELL, EVER — the decoder is an argv VECTOR** (`buildSrtIngestArgv` in
  `SrtFfmpegArgs.h`, POSIX `execvp` / Windows quoted `lpCommandLine` with
  `lpApplicationName` pinned). `buildSrtUrl` deliberately tolerates a **pasted**
  `srt://host:port` because pasting a remote contributor's connection string is the
  intended way to add an ingest — so the URL is attacker-influenced by design. It must
  stay exactly one argument; never rebuild this as a command string.
- **`-y` IS LOAD-BEARING.** FFmpeg sees the audio pipe as an existing FILE and
  interactively prompts `Overwrite? [y/N]`, then EXITS — killing the whole decoder and
  taking **video** down with it. The symptom is "SRT ingest stopped working entirely"
  when you touch the audio output. Never drop `-y` from the ingest command.
- **THE WRAPPER LAW (this bit twice now).** `WinUiCaptureDeviceAdapter` wraps the whole
  capture composite, and `ICaptureDevice`'s defaults are permissive — inheriting the
  default `pollAudioFrames` returned `{}` and SILENTLY swallowed every ingested audio
  frame while video flowed perfectly. Identical shape to the old 1-arg `connect()` bug
  that caused pink tiles. **Any new `ICaptureDevice` method must be forwarded in
  `WinUiCaptureDeviceAdapter`** — the shell bridge carries no audio, but the devices it
  wraps do.
- **Proof:** `node scripts/validate-srt-ingest.mjs [--seconds N] [--port N] [--keep]`
  publishes `testsrc` + a 440Hz `sine` over real SRT and judges **decoded pixels and
  decoded audio in the program recording** — mean luma and audio peak — plus the core's
  own muxer proof counts. It judges output, not status strings, because the adapter this
  replaced counted bytes and threw the packets away: it reported "receiving" while
  emitting frames with NO PIXELS (correction published in `docs/spine-status-2026-08-06.md`).
- **Harness gotcha worth keeping:** `stop-recording-session` returns BEFORE the async
  encoder sink writes the MP4 **moov atom**, and file size stabilises well before the moov
  lands — so a size-based wait reads an unfinalized file that decodes as **zero frames**,
  which looks exactly like a dead feed. Wait until **ffprobe** can read a duration, with
  the core still alive, before killing it.

## Performance profiling (operator lag/stutter/crash)

The right tools, cheapest first — a full evidence trail lives in
`docs/operator-performance-plan.md` and `docs/present-stutter-fix-spec.md`.

- **`dotnet-trace` — no admin, in-process, USE THIS FIRST for the shell.**
  `dotnet tool install -g dotnet-trace`; `dotnet-trace collect -p <pid>
  --profile dotnet-sampled-thread-time -o x.nettrace`; `dotnet-trace report x.nettrace
  topN -n 20`. (`--profile cpu-sampling` is Linux-collect only — don't use it here.)
- **PresentMon 2.5.1 — needs elevation (UAC), measures on-screen frame delivery.**
  `PresentMon --process_name CoreVideoPro.WinUI.exe --timed 20 --output_file x.csv`.
  Read `MsBetweenDisplayChange` (`NA` = frame never displayed), and
  `MsCPUBusy` vs `MsCPUWait` (busy = compute/UI-thread; wait = GC-suspend/IO).
- **`dotnet-gcdump`** for the managed heap: `dotnet-gcdump collect -p <pid>`, open the
  `.gcdump` in PerfView/VS for the retained graph (the CLI `report` under-accounts).
- **The stutter only reproduces under LOAD** — use the fake engine (below) to synthesize
  participants/sources; a fresh idle StubOnly launch has a cheap apply and won't repro.

**Diagnosing NATIVE crashes (make the next one post-mortemable).** Two things must be in
place or a `corevideo-native.exe` dump is unreadable:
1. **PDBs.** Release now emits symbols (`native/CMakeLists.txt` MSVC `/Zi /DEBUG`), and the
   build/launch scripts stage each `.pdb` beside its binary. Analyze with
   `cdb -y "native\build-dev;srv*https://msdl.microsoft.com/download/symbols"
   -z "%LOCALAPPDATA%\CrashDumps\corevideo-native.exe.<pid>.dmp" -c "!analyze -v; kb; q"`.
   **Caveat: a matching PDB only exists for the CURRENT build** — analyze a dump BEFORE
   rebuilding the core, or the offsets stop resolving (this is exactly what lost the
   2026-07-10 08:13 startup crash).
2. **Full dumps.** Run `scripts/setup-crash-dumps.ps1` once (elevated — writes HKLM WER
   `LocalDumps`) to get `DumpType=2` full-memory dumps instead of the default registers+
   stack minidump. Dumps land in `%LOCALAPPDATA%\CrashDumps`.

**Beta crash pipeline (S1, 2026-07-18, `docs/beta-engineering-spec.md` §S1).** On launch
the shell scans `%LOCALAPPDATA%\CrashDumps` for our exes' dumps newer than the offer-once
watermark (`%LOCALAPPDATA%\CoreVideoPro\crash-watermark.json`) and shows a one-shot
consent InfoBar (never auto-sends). Send = zip (dump + ~2MB log tails + redacted support
bundle + manifest.txt, ~24MB cap) → POST `application/zip` to the telemetry-ingest
worker's `/v1/crashes` with `X-CoreVideo-*` metadata headers; the zip stays under
`support-bundles\` either way. Enabled only when `COREVIDEO_TELEMETRY_ENDPOINT` +
`COREVIDEO_TELEMETRY_API_KEY` are set (empty default = quietly disabled). Pieces:
`CrashDumpScanner` / `CrashReportWatermarkStore` / `CrashReportArchiveBuilder` /
`CrashReportUploader` (MediaCore, unit-tested) + `CrashReportCoordinator` +
`StudioWorkspace.BeginCrashReportScan` (WinUI).

**Beta opt-in telemetry (S3, 2026-07-23, `docs/beta-engineering-spec.md` §S3).**
A Settings-tab toggle (default OFF) sends "is beta healthy" events to the
telemetry-ingest worker's `/v1/events` on app close (session-end, fire-and-forget
at the top of `ShutdownAsync` — never blocks the close) and a daily heartbeat.
Reuses S1's config (`COREVIDEO_TELEMETRY_ENDPOINT`/`COREVIDEO_TELEMETRY_API_KEY`,
empty = disabled) and Bearer/202 contract. Consent lives in a STANDALONE flag file
`telemetry-consent.json` (NOT prefs — avoids the prefs-version race), default OFF.
Payload is COUNTS/KINDS ONLY (`TelemetryPayloadBuilder`, sourced from
`_bridge.LastSnapshot`, never StudioViewModel): version + sessionLengthSeconds +
outputConfigShape {recording/streaming/vcam bools, iso/capture/participant counts} +
crashCountSinceLastSend (from the S1 crash-watermark) + machineClass (the SAME
`win-x64-cpuN-ramNgb` string as S1 via shared `MachineClassProbe`) + banded machine
{cpuCores,ramBand,gpuTier}. **NEVER a secret/endpoint/path/name** — the no-leak unit
test (`Serialize_NeverLeaksAnySecretOrEndpoint`) seeds a snapshot full of stream
keys/URLs/paths and asserts none appear. Settings "Preview what's sent" shows the
exact JSON (works with consent OFF), and every send logs the JSON locally first
(§7 inspectable-before-egress). Pieces: `TelemetryConsentStore`/`MachineClassProbe`/
`TelemetryEvent`/`TelemetryEventClient` (MediaCore) + `TelemetryEventService`/
`GpuTierProbe` (WinUI). Any new telemetry field MUST be a count/kind and get a
no-leak assertion.

**Capture reader stability (the frozen-webcam / restart-storm class).** A stalled
MediaCapture reader (`CaptureDeviceFrameReaderService`) used to restart on a fixed ~5s
cadence forever when it couldn't recover (e.g. an Elgato Game Capture whose HDMI signal
dropped → `reader.StartAsync` returns `OutputFormatNotSupported`; 515 restarts logged in a
day). Now `CaptureReaderStallPolicy` applies exponential backoff (5→10→20→40→60s) and
**gives up after 5 consecutive failed restarts**, leaving the last frame frozen and asking
for a manual reconnect — no perpetual churn (that churn can trip the `CoreMessagingXP`
fail-fast on a long show). The counter resets the instant a real frame lands. Separately,
when native UVC (`COREVIDEO_NATIVE_UVC=1`) claims a device, the shell now stops any managed
bridge reader for that same device so the two never run concurrently.

**Native-UVC no-first-frame watchdog + confirm-before-commit fallback (2026-07-23).**
The native MF adapter's `connect()` flips a device to `connectionState:"connected"` the
instant the reader thread STARTS — before any frame. A single-consumer capture card
(Elgato Game Capture / HD60 S+) that another app (Zoom, Camera Hub, OBS) holds, or an HDMI
input with no signal, OPENS and NEGOTIATES fine, then delivers zero samples: open+negotiate
succeeded so no stall policy fired, and it sat forever on a placeholder tile (the compositor
logging `capture:<id> has NO matching frame` every 5s). Two-part fix, mirroring the
`CaptureReaderStallPolicy` shape: (1) **core watchdog** — `UvcCaptureSession::readLoop`
gives a negotiated device `kUvcNoFirstFrameTimeoutMs` (4s) to produce its first frame; past
that it fails LOUD (`uvcNoFirstFrameWarning` names the device + likely cause), ends the read
loop and RELEASES the MF device (frees the single-consumer card). The check is lock-free off
the `loggedFirstFrame` flag and fires whenever `ReadSample` returns (stream tick / gap — the
shape a no-signal card presents); `frameId>0` is never a stall. The adapter also gained a
real `disconnect()` override (was a no-op) that resets the session. (2) **shell confirm-
before-commit** — `TryConnectNativeUvcCaptureAsync` no longer commits (stops the bridge) on
the connect response alone; it polls `ListNativeCaptureDevicesAsync` for up to
`NativeUvcCapturePolicy.FirstFrameTimeoutMs` (6s > the 4s core watchdog so the core marks it
"error" first) until `signalPresent` (→ commit native) or the device errors/vanishes/times
out (→ `DisconnectNativeCaptureDeviceAsync` + return false → the existing WinUI MediaCapture
bridge fallback, the robust default per this file). Pure decisions: `uvcNoFirstFrameTimedOut`
(core, `UvcCaptureSupportTest`) and `NativeUvcCapturePolicy.EvaluateFirstFrame`/`FindDevice`
(shell, `NativeUvcCapturePolicyTests`). Net: a camera native UVC can't pull frames from now
FAILS LOUD and FALLS BACK, never a silent placeholder forever. Honest caveat: the watchdog
relies on `ReadSample` returning periodically (the common no-signal case); a driver that
blocks `ReadSample` forever with no return can't be interrupted from a synchronous reader
(async callback = the WgcSession free-threaded-callback crash class we forbid). NOT a
regression from the ISO merge (#316/#318/#320) — the reader path was untouched (last change
#275); the failure is device contention/starvation.

**Bridge capture allocation churn (the "video slow down").** The managed MediaCapture
bridge used to allocate a fresh ~8MB BGRA `byte[]` **per frame** in
`CaptureDeviceFrameReaderService.CopyBgraBytes`. Across two 60fps cameras that is ~0.7GB/s
of garbage → the WinUI heap grew to **5.2GB** and a core sat in GC, and the GC pauses stall
the UI thread → the operator preview visibly slows (and eventually OOMs). Fixed with a
per-`CaptureSession` ring of 4 reused buffers (`RentFrameBuffer`) — `OnFrameArrived` is
single-flighted so the ring advances on one thread, and depth 4 exceeds the buffers live at
once (SHM write is synchronous; the preview holds only the latest surface state, flushed to
the UI within ~16ms ≪ the ~66ms 4-frame reuse interval). Result: working set **5210MB →
~350MB flat**, 0 dropped frames. The residual ~1.5 cores of bridge CPU (the per-frame
convert + copy + SHM write) is inherent to the managed path; the full elimination is native
UVC once its display gap is closed.

**What the profiling proved (2026-07-10): the lag is the WinUI shell, not the core/GPU.**
On an RTX 4090 the core renders the 4K program + multiview in ~6.6ms (60fps) and the GPU is
near-idle. `dotnet-trace` under synthetic 4-participant load ranked
`CaptureDeviceFrameReaderService.OnFrameArrived` at **53%** →
`CaptureDeviceSharedMemoryWriter.Write` → `SafeBuffer.WriteSpan` **35.7% exclusive**: the
WinUI MediaCapture bridge copies **every** webcam frame through managed memory (~180MB/s
@1080p) = ~50% CPU + ~50MB/s heap churn → 2–3GB working set → crash → UI-thread starvation.
**Native UVC capture (`COREVIDEO_NATIVE_UVC=1`) eliminates it** — verified working set
~265MB (vs 5210MB on the bridge), CPU near-idle, 0 drops. The old "pink tiles" display gap
is **FIXED** (2026-07-10): it was a frame-key mismatch — the multiview layer looks up a
capture tile by `capture:<shell captureDeviceId>` but native frames were keyed
`capture:<core MF id>`, because the outer `WinUiCaptureDeviceAdapter` overrode only the
1-arg `connect(deviceId)` so the `ICaptureDevice` default dropped the shell's `outputSourceId`
before it reached the UVC adapter. Fixed by forwarding the 2-arg
`connect(deviceId, outputSourceId)`. Enabling native UVC also surfaced (and the full-dump+PDB
tooling pinned) a **separate WGC screen-capture crash** — `WgcSession::onFrame` deref'd a
torn-down D3D `context_` because `stop()` revoked the FrameArrived handler without draining an
in-flight callback on the free-threaded pool thread; fixed with a `frameMutex_` held across
`onFrame` + drained in `stop()` + a `~WgcSession(){ stop(); }`. Native UVC is still opt-in
(default OFF): the WinUI MediaCapture **bridge is the robust default** (the shell owns both
id sides, so it cannot go pink; memory-stable since the buffer-reuse fix). Native UVC is the
faster-but-more-delicate opt-in (two id spaces that must agree). A secondary
snapshot-apply churn fix also shipped in `StudioViewModel.ApplyLiveParticipants`
(order-independent, structural-only signature).

**System-audio citizenship (vcam glitching OTHER apps' audio, 2026-07-11/12).** With the
virtual camera consumed by Zoom, other apps' audio (browser) glitched; OBS's vcam on the
same rig was clean → our serve chain. Fixes shipped: (1) render pacer no longer spins the
last 1.5ms of every frame — high-res waitable timer + 500µs tail (200µs measured 58.7fps;
timer wakes ~300-400µs late); (2) the vcam tap thread runs BELOW_NORMAL (it does the most
bus-hostile work in the app); (3) WASAPI monitor thread uses MMCSS "Pro Audio" instead of
raw TIME_CRITICAL (`avrt.h` must be included AFTER `windows.h`); (4) **GPU BGRA→NV12** in
the tap — two pixel shaders on the tap's own device (R8 luma + R8G8 half-res chroma; BT.601
studio-swing matched to `convertBgraToNv12`), readback 8MB→3MB/frame, scalar convert gone;
(5) the Frame Server DLL reader caps torn retries 8→2 and skips the copy when no new frame
was published. Rules distilled: never spin in hot loops (waitable timer + tiny tail); raw
TIME_CRITICAL is forbidden — MMCSS class it; GPU→CPU readbacks are uncached/WC — minimize
bytes and convert on the GPU first (the OBS lesson: learn from OBS architecture, never copy
its GPL code).

**Loud-failure guardrail (no more silent pink).** The compositor
(`D3D11CompositorAdapter::warnUnmatchedCaptureLayer`) now logs — rate-limited 5s/key —
whenever a `capture:` render-plan layer resolves to NO matching frame (the pink condition),
dumping the layer key AND the available capture-frame keys. A key mismatch on either path is
now a 10-second diagnosis instead of a multi-session hunt. Fires only during the startup gap
before first frames, then silent. Companion audit: `WgcSession` was the ONLY free-threaded OS
callback in the capture layer — `UvcCaptureSession` owns its pull thread and signal+joins in
its destructor — so the WGC teardown-drain fix closed that crash class everywhere.

## ISO recording — ISO-1 (per-source Zoom VIDEO ISO, 2026-07-20)

`docs/iso-record-spec.md` is the source of truth; ISO-1 ships the video slice for
Zoom participants (audio stems = ISO-2, capture sources = ISO-3, UI/pre-flight =
ISO-4). What landed:

- **The encoder boundary is widened, not rebuilt.** The MF sink already held
  `Mp4Writer program_` + N ISO writers on ONE shared `RecordingPtsClock`. ISO-1
  stops feeding ISO writers the composed program frame and instead carries each
  source's OWN video across `IEncoderSink::submitIsoVideo(vector<IsoSourceVideoFrame>)`
  (`Interfaces.h`). The frames are **zero-copy** — `VideoFrame` holds `shared_ptr`
  I420/BGRA payloads, so the whole hop (render gather under `coreMutex` →
  `latestIsoSourceFrames_` → `gatherAudioOutputWork` `work.isoSources` → async
  sink) copies refs, never pixels. Any I420→NV12 interleave happens on the
  **AsyncEncoderSink writer thread** (`i420ToNv12` in `MediaFoundationEncoderAdapter.cpp`),
  never under a lock or the audio worker. The convert law holds.
- **NV12 input path on `Mp4Writer`** (`VideoInput::Nv12`): Zoom I420 needs no CPU
  color-convert — the writer opens LAZILY at the source's FIRST frame, sized to
  that frame's native dims (no scaling), and picks NV12 input for `zoom:` sources
  / RGB32(BGRA) for capture (ISO-3). Program keeps its BGRA path untouched.
- **Per-`(sourceId,frameId)` PTS dedup** on the SAME epoch (`RecordingPtsClock::videoPtsForSource`):
  the audio worker re-submits every selected source's latest frame each tick, and
  each source advances on its own Zoom frameId, so a per-source last-frameId map
  (one shared epoch) muxes each real frame once. Proven headless: two ISO guests
  recorded **different** frame counts (402 vs 375 over 12s) — real per-source
  video, not the program proxy, and deduped well below the ~50/s resubmit rate.
- **Folder scheme + manifest (spec §5):** per-session subfolder
  `<prefix>-<yyyymmdd-hhmmss>/` with `Program.mp4` + `ISO-NN-<SafeName>.mp4`
  (roster/display name, sanitized, selection order) + `manifest.json`
  ({sessionId, epochMs, entries[{sourceId,name,path,kind}]}). `sanitizeForFilename`
  in the core mirrors `sanitizeIsoName` in `src/engine/isoRecording.ts` (the older
  planner was reconciled to this scheme — `ISO-NN-*.mp4`, no more `track-NN-*.mov`).
- **Command surface:** `isoParticipantIds` generalized → `isoSourceIds` accepting
  `zoom:<pid>` (capture ids arrive in ISO-3), with back-compat parse (a bare id =
  `zoom:<id>`) across all THREE mirrors in lockstep: `Protocol.h` (capability),
  `native-core/src/protocol.ts` (types), and the core parser
  (`MediaCore::readIsoSourceIds`/`normalizeIsoSourceId`). `src/engine/isoRecording.ts`
  reconciled. A "Program only ↔ Program + ISOs" switch is a payload flag; per-source
  selection is `isoSourceIds` (UI wiring is ISO-4).
- **Loud, never silent (spec §4/§7):** the silent `%TEMP%` fallback is KILLED for
  ISO — a bad/uncreatable target or session subfolder → `recording.warning` +
  ISO refused, program still records (priority-1). Per-ISO-writer open/write
  failures fold into `recording.warning` with the source name AND surface per
  stream in `recording.streams[]` ({sourceId, displayName, path, kind:"iso",
  framesWritten, warning, trackOpen}). A video-only-broken ISO is as loud as
  #286 made a video-only program. Each ISO writer finalizes independently on stop
  (its own moov, no 0-byte tails).
- **INVARIANTS honored:** lock order `coreMutex → audioOutputMutex_ → …`
  unchanged; ISO gather is under `coreMutex` (zero-copy refs), encode under the
  async sink; ISO frames drop-to-latest under disk pressure (video budget in
  `AsyncEncoderSink`), NEVER program A/V. **PROGRAM IS NEVER REGRESSED** — proven
  both ways: `EncoderRecordingSession.MediaFoundationIsoWritersProduceIndependentPlayableFiles`
  (program A+V green with 2 ISO writers present) and
  `validate-record-audio.mjs` (program A+V unchanged with ISO disabled).
- **Tests:** `EncoderRecordingSessionTest.cpp` (RecordingPtsClock per-source
  dedup + monotonic; real-MF N-writer open/reset #286 shape, NV12 playable,
  independent finalize, bad-folder-loud) + headless
  `node scripts/validate-iso-record.mjs` (fake engine, ISO on 2 → 2 ISO mp4s with
  h264 video, deduped). ISO-2 extends it into A+V + clap alignment.

## ISO recording — ISO-2 (per-source AUDIO stems muxed into the ISO MP4s, 2026-07-20)

ISO-2 completes the **Demo E** shape: each Zoom-participant ISO is now a
self-contained **A+V** MP4 (its own video from ISO-1 **and** its own raw-stem
audio), time-aligned to program. Stacked on ISO-1 (`submitIsoVideo` boundary,
per-source `Mp4Writer` map, folder scheme). What landed:

- **Raw-stem tap = PRE-DSP, PRE-MIX** (owner decision-3). The stem is
  `work.audioFrames[i].pcm` — each source's isolated PCM, resampled to the 48k bus
  rate at gather but tapped BEFORE the channel-strip DSP and the bus mix. Proof
  it's pre-DSP: `RoutedAudioSource.pcm` is a `const` pointer into these buffers and
  `mixRoutedBuses` runs the gate/EQ/comp/inserts on COPIES — the source buffers are
  never mutated (`MediaCore.cpp` runAudioOutputWork, just after the program
  `submitAudio`). Do NOT move the tap after `mixRoutedBuses`; that would be the
  post-DSP signal (the option the owner explicitly did NOT choose).
- **`IEncoderSink::submitIsoAudio(vector<IsoSourceAudio>)`** (`Interfaces.h`), a
  separate boundary paired with `submitIsoVideo`. Submitted **every tick for EVERY
  selected source**: a source with PCM this tick muxes it; a source Zoom gated
  silent this tick rides an **empty** entry (frameCount==0). Rides its own
  `AsyncEncoderSink` `Kind::IsoAudio` with the audio budget but SEPARATE
  drop-to-latest accounting, so a slow disk drops ISO audio to silence-filled gaps
  and can NEVER evict a program-audio packet (program is priority-1, spec §9).
- **Silence-fill (spec §2c), the correctness core.** `RecordingPtsClock::isoAudioAdvance`
  anchors every stem to the ONE shared epoch (t=0 == program start): the expected
  sample position at wall time `now` is `(now-epoch)` worth of samples, so a buffer
  emits exactly enough leading silence to reach that position, then the real
  samples. A guest silent for K ticks (empty submits) advances by silence alone and
  lands the next real burst at the correct, program-aligned position — never a
  drift EARLIER of program. A dropped ISO-audio tick simply becomes silence in the
  stem (the next tick's wall-anchored fill covers it), timeline intact. The sink
  chunks long leading silence (`Mp4Writer::writeAudioSilence`, 0.1s blocks) so a
  guest who talks minutes in never emits one giant sample.
- **#286 up-front audio stream, per ISO writer.** The ISO writer opens LAZILY at
  its first video frame; the AAC stream is added THERE — `open()` →
  `ensureAudioStream(2, 48000, …)` → `beginWriting()` — never after BeginWriting
  (0xC00D36B2). `Mp4Writer::open()` already resets `audioConfigured_`, so a REUSED
  ISO writer across the double `start()` re-adds its stream cleanly (regression
  test proves a reused ISO writer keeps its audio track). ISO AAC is uniformly 48k
  **stereo**; mono Zoom `isolate_audio` stems are up-mixed L=R in `submitIsoAudio`.
- **Snapshot + manifest:** `recording.streams[]` ISO nodes now carry
  `audioSamples` (silence+real) and `hasAudio` (= `audioSamples > 0`);
  `manifest.json` marks every entry `"hasAudio": true`. A track-less ISO where
  audio was expected folds into `recording.warning` (as loud as #286 made a
  video-only program).
- **Tests:** `RecordingPtsClock.IsoAudioSilenceFillKeepsGappedStemAligned` (the key
  gapped-stem test — silent K ticks then resume lands at the right sample
  position) + `IsoAudioLateStartSilenceFillsFromEpoch`; real-MF
  `EncoderRecordingSession.MediaFoundationIsoWritersMuxOwnAudioStems` (2 ISO writers
  with DIFFERENT audio, #286 reused-writer audio-track reset, **program A+V not
  regressed with ISO audio enabled**); and `scripts/validate-iso-record.mjs`
  extended to the **Demo E leg** — ffprobe each ISO has h264 video AND aac audio,
  head-clap alignment (ISO audio start vs program audio start on the shared epoch)
  measured **0.0 ms** (budget 50 ms). Fake tone engine gives distinct
  per-participant sines (220Hz + pid%8·110), so the two ISO stems carry different
  content (956685 vs 969374 samples over 20s), not the program mix.

## ISO recording — ISO-3 (UVC/capture sources, 2026-07-21)

ISO-3 broadens ISO to **capture-class** sources (`capture:<id>` — UVC cameras,
screen/window capture, browser sources). Most of the machinery was already
capture-generic in ISO-1/2 — the delta is small and surgical:

- **Capture VIDEO rides ISO-1's BGRA writer path, no new code.** Capture frames
  merge into `videoFrames` keyed `capture:<id>` (`capture:browser:<n>` for
  browser) at the render gather, and ISO-1's `latestIsoSourceFrames_` snapshot
  already keys ANY `<scheme>:<id>` frame and skips only `media:`. So a capture
  frame flows to `submitIsoVideo`, which already branches `frame.hasI420() ?
  NV12(Zoom) : RGB32(BGRA)` — capture is BGRA, so it takes the RGB32 path (spec
  §2b, "the writer picks input type per source at open"). Per-`(sourceId,frameId)`
  dedup is scheme-agnostic; all three capture paths (WinUI bridge / native UVC /
  browser host) carry advancing `frameId`, so it holds.
- **Capture AUDIO pairing — THE decision (owner rule confirmed against the
  codebase).** A capture VIDEO source and its audio can be SEPARATE devices. The
  codebase pairs them via `sync-capture-audio-sources`: a `CaptureAudioSourceInput`
  has a `captureDeviceId` (the VIDEO device) + an optional `audioDeviceId`, and
  `WasapiAudioCaptureSourceAdapter::participantIdForSource` keys the PCM
  `capture:<captureDeviceId>` — the SAME id as the video. So paired capture audio
  muxes into the same ISO writer AUTOMATICALLY (ISO-2's `work.audioFrames` tap,
  same sourceId match). **Rule: a capture ISO carries audio IFF the operator paired
  an audio input to that capture device (Elgato-class embedded audio / a mic
  assigned to the camera). A pure camera (no paired audio) → VIDEO-ONLY ISO — no
  all-silence AAC track, no fabricated stem.** Implemented via
  `IsoSourceSelection.hasAudio` (`MediaCore::isoSourceHasAudio`: zoom→always,
  capture→matched real pairing in `captureAudioSources_`, browser→false); the ISO
  writer skips `ensureAudioStream` at lazy-open when `hasAudio==false`, so
  `submitIsoAudio` naturally skips it (`audioConfigured()` stays false). Snapshot
  `hasAudio`/`audioSamples` and `manifest.json` reflect the per-source decision.
- **Display names:** `resolveIsoDisplayName` resolves `capture:<id>` to the
  enumerated device name (`CaptureDeviceInfo.name`, match by id/`nativeDeviceId`),
  a browser source's URL, or the paired audio device name — so post sees
  `ISO-NN-<CameraName>.mp4`, falling back to the id tail (loud, never fabricated).
- **Command/snapshot parity (3 mirrors):** `isoSourceIds` already accepted
  `capture:<id>` (ISO-1 generalized `normalizeIsoSourceId`); the snapshot now also
  emits the canonical `isoSourceIds` list alongside `isoParticipantIds`
  (`canonicalIsoSourceIds`); `src/engine/isoRecording.ts` planner gains a
  `capture` `IsoTrackSource` (+`captureSources` option, `capture:<id>` track ids,
  participant-tier bitrate). `Protocol.h` (`iso-recording` capability + the
  scheme-qualified reader) needed no change.
- **Capture-stall interaction (CaptureReaderStallPolicy):** a stalled capture
  source either holds its last frame (same `frameId` → dedup muxes once, no churn)
  or stops appearing in `videoFrames` (its writer simply stops advancing and
  finalizes gracefully at stop) — never a churn/spam loop on the ISO writer. Loud
  in `recording.warning` only on a real writer failure.
- **Tests:** `MediaFoundationCaptureBgraIsoMixedWithZoomNv12` (capture BGRA +
  zoom NV12 in ONE session, both playable, **program A+V green with capture ISO**,
  paired capture audio muxed), `MediaFoundationVideoOnlyCaptureIsoHasNoAudioTrack`
  (a pure camera → `audioSampleCount==0`, no all-silence track),
  `MediaCoreResolvesCaptureIsoDisplayNamesAndAudioPairing` (display name from
  enumerate + the paired/unpaired hasAudio decision),
  `RecordingPtsClock.IsoVideoDedupsCaptureSourceIndependentlyOfZoom`; TS planner
  tests for the `capture` source. **Harness gap (honest):** the fake zoom engine
  is Zoom-only, so capture ISO has no headless E2E — it is covered by the real-MF
  unit tests + synthetic capture frames above, and is **rig-verified only** for a
  live camera. `validate-iso-record.mjs` (Zoom) still PASSES (2 ISO A+V streams,
  clap 0.0 ms) — proof ISO-1/2 is not regressed.

## ISO recording — ISO-4 (disk pre-flight + support-bundle health + Show-mode UI, 2026-07-21)

ISO-4 is the operator-facing polish; it adds NO new media protocol beyond the
`isoSourceIds` selection ISO-1/2/3 already defined, and — critically — **program
recording is never regressed**: the new "Program + ISOs" switch DEFAULTS OFF, so a
fresh install records program-only exactly like the pre-ISO product (no ISO writers
arm). Four pieces:

- **Disk pre-flight (spec §6) is SHELL-SIDE by design.** `IsoDiskPreflight.Evaluate`
  (`CoreVideoPro.MediaCore/Services/IsoDiskPreflight.cs`, pure/unit-tested) ports the
  TS `isoRecording.ts`/`diskSpace.ts` math: combined rate = program bitrate + N×6.192
  Mbps (1080p video + one raw-stem AAC), vs free bytes on the target volume
  (`DriveInfo`). Runs at the top of `StudioViewModel.ToggleRecordingAsync` BEFORE
  arming: **Insufficient** (< 5 min headroom) hard-blocks the start with a loud
  `OutputStatus`; **Low** (< 30 min planning window) sets a persistent
  `RecordingDiskWarning` (surfaced in the record flyout, survives the "start
  requested" status) but proceeds; an unmeasurable volume never blocks (warn-not-
  silent). No core/protocol/snapshot field — the shell already owns the folder,
  program bitrate, and ISO selection, so core-side would need a needless 3-mirror
  protocol change.
- **Support-bundle ISO health (spec §6, DoD).** `NativeMediaCoreRecordingStream` (wire)
  + `SupportBundleMediaCoreRecordingStream` (model) gained `SourceId/DisplayName/Path/
  AudioSamples/HasAudio` (camelCase deserialize auto-populates the ISO-1/2/3 snapshot
  fields that were previously dropped). `SupportBundleBuilder` maps them + adds an "ISO
  recordings: N stream(s)…" triage block listing each ISO's path + encode health.
  Paths are NOT secrets and are emitted verbatim; redaction stays green (no new field
  carries a key/token) — `SupportBundleBuilderTests.Build_ListsIsoStreamPathsAndEncodeHealth`.
- **Show-mode UI (spec §7, N1).** A transport-level **"Program only" ↔ "Program +
  ISOs"** ToggleSwitch in the record-output flyout (`StudioWorkspace.xaml`) bound to
  `IsoRecordingEnabled`; a per-source **"ISO" checkbox** on each eligible row in
  Sources → Inputs (`SourcesInputsPage.xaml`, `ShowInputSlotViewModel.IsoEnabled`/
  `ShowIsoToggle` — Zoom guests + capture devices only, media excluded); and an **ISO
  health readout** ("Program + N ISOs" + first per-stream warning) that reuses the
  recording-warning surface. **0xc000027b-safe:** the toggle rides the EXISTING
  signature-gated `ShowInputEditors` collection (never a new snapshot-rate bound
  collection; re-projected in place via `ApplyIsoSelectionToEditors` under the same
  id-set signature as `RefreshShowInputEditors`); ISO health strings are scalar props
  notified per snapshot apply (the `WorkspaceCompGrLevel` pattern), UI mutations via
  `RunOnUiThread`.
- **The pure selection logic is EXTRACTED and tested.** `IsoSourceSelectionResolver`
  (MediaCore) turns (enabled, selected set, eligible-present roster) → ordered
  `isoSourceIds` (OFF → empty; drops departed sources; deduped; capped at 8) — so the
  logic trapped in `StudioViewModel` (`BuildIsoSourceTargets`) is unit-tested without
  the VM. The command builder now emits canonical `isoSourceIds` (`zoom:<pid>`/
  `capture:<id>`) on all three recording payloads (the core prefers it over legacy
  `isoParticipantIds`; `SyntheticMediaCore` mirrors the preference).
- **Persistence: prefs schema v8.** `ProductionOutputPreferences.IsoRecordingEnabled` +
  `IsoRecordingSourceIds` persist the switch + selection; restore rides the O1/vcam
  BACKING-FIELD pattern (a setter would sync a core that isn't up), re-projected onto
  the editors on first `RefreshShowInputEditors`. v7→v8 migrates to program-only
  defaults. (v7 was the true current version — the "v6" in the B2 notes was stale; v7
  added VstInsertStates. v9 (2026-08-10) persists the Zoom→program audio topology
  (ZoomAudioMode: "programMix"/"perGuestIso"); absent = programMix, and an
  unrecognized value falls back to programMix rather than guessing ISO.)

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
