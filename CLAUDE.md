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

Logs: `%LOCALAPPDATA%\CoreVideoPro\launch.log` (WinUI) and `media-core.log` (core).

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

## Other gotchas

- The WinUI window often **opens minimized off-screen** (rect ≈ -32000,-32000). Restore
  gently with `ShowWindow(SW_RESTORE=9)`; do NOT aggressively maximize/move a
  SwapChainPanel window across monitors — it can kill the window (and resize can crash).
- 60fps needs `timeBeginPeriod(1)` (Windows 15.6ms timer granularity) + a frame-budget
  pace, both already in `JsonRpcServer.cpp`.
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
  the output worker just does a cheap NV12 copy (`takeVcamNv12`) + `publishNv12`. Net render
  cost ≈ 1ms. Rule: GPU→GPU `CopyResource` is microseconds; GPU→CPU-staging map+read is
  ~8–12ms and MUST live on a dedicated device/thread, never under `coreMutex` or the audio
  worker. Current: publish ~50fps 1080p; the last ~10fps to a true 60 is the scalar
  `convertBgraToNv12` (~15ms) — SIMD it or convert to NV12 on the GPU (half the readback).
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

**Loud-failure guardrail (no more silent pink).** The compositor
(`D3D11CompositorAdapter::warnUnmatchedCaptureLayer`) now logs — rate-limited 5s/key —
whenever a `capture:` render-plan layer resolves to NO matching frame (the pink condition),
dumping the layer key AND the available capture-frame keys. A key mismatch on either path is
now a 10-second diagnosis instead of a multi-session hunt. Fires only during the startup gap
before first frames, then silent. Companion audit: `WgcSession` was the ONLY free-threaded OS
callback in the capture layer — `UvcCaptureSession` owns its pull thread and signal+joins in
its destructor — so the WGC teardown-drain fix closed that crash class everywhere.

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

**Mastering chain M1** (docs/mastering-chain-spec.md): AudioMastering.h on the master
bus (LUFS ride + glue + ceiling; mastering{} params on the audio sync command; ride dB
is snapshot telemetry). Owner topology question (master == pgm-l/r?) open in spec 0.
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

**Scenes redesign S1–S3a + R1 shipped** (`docs/scenes-tab-redesign.md`): layer
delete/reorder/opacity, non-destructive presets + undo, duplicate/no-clobber save,
custom scenes persist across restarts, live-scene DRAFT editing (program untouched until
Update), numeric rect fields + snap guides + arrow-key nudge, and **production roles**
(session-only assignment on the Inputs tab; role-targeted routes resolve at sync time;
the assigned role rides the participant wire to the core director). Remaining: S3b
(aspect-lock, edge handles, selection sync), S4 polish, role templates/automation (R2).

In progress / next: (1) the **alpha validation pass** on the Windows rig — every
checkbox in `docs/alpha-plan.md` Tracks A–F is still unchecked, including the ≥10-min
audio-glitch-freedom soak that Phase 2 shipped without; (2) **real device
capture** — native Media Foundation UVC capture is CODE-COMPLETE behind
`COREVIDEO_WITH_UVC` (ON in `build-native-dev.ps1`; shell opt-in
`COREVIDEO_NATIVE_UVC=1`, WinUI shm bridge remains fallback) but **not yet
rig-validated with a live camera**; DeckLink/AJA frame delivery still to do
(see `docs/native-production-completion-plan.md` Items 1–2); (3) the **audio
overhaul** (`docs/audio-overhaul-spec.md`) — 4.1/4.2 landed (Zoom audio is REAL:
ISO PCM ingest + routing + 50Hz pacer + underrun telemetry); next 4.3 recording
A/V clock, 4.4 mixer completion + meter ballistics, **4.4b Audio tab overhaul**
(owner-reported: doesn't reflect reality, routing matrix unusable, device
selection broken, full UX redesign), 4.5 plugin host; (4) **Scenes tab overhaul**
(owner-reported 2026-07-03): scene BUILDING doesn't work well (creating/editing
layouts — sources, positions) and the tab needs a full layout/UX redesign, not
spot fixes.
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
