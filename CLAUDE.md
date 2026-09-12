# CLAUDE.md — working guide for CoreVideo Pro

Operational notes for working in this repo. Product/positioning lives in `README.md`
and `COREVIDEO_PRO_PRODUCT_SPEC.md`; this file is the "how to build, run, and not break
it" guide.

## North star (non-negotiable)

Low-latency **and** high-quality A/V is the entire product. It must beat vMix, Ecamm,
and mimoLive. All Zoom is 1080p and up to 60fps — never downgrade quality to dodge a
performance problem; fix the pipeline. CPU per-pixel work does not scale to 8 Zoom + 2
capture @1080p60 — the compositor path must stay on the GPU.

### Frame-delivery acceptance (user requirement, 2026-09-06)

60 fps is a per-frame output delivery requirement, not an average-FPS target.
The approved Program buffer is selectable at 2 or 3 frames, default 3, applied
after app restart. At 60 Hz this adds 33.333 or 50 ms of buffering, with matching
Program audio delay. A render overrun absorbed by the buffer is diagnostic, not
an output failure. Any underrun or missed scheduled output deadline fails
performance acceptance for the tested configuration. The PR #407
operator soak passed functional checks, but its reported late intervals and
42 ms worst interval do NOT pass this performance requirement. Never waive a
frame-rate failure because the average or median is 60 fps. Preserve resolution,
quality, audio continuity, and enabled outputs while fixing the pipeline.

Measure render cost, GPU readiness by the scheduled presentation deadline, and each enabled output's
delivery separately. CPU submissions or advancing frame counters alone do not
prove presentation; duplicated/padded output must not conceal missed rendering.
Report missing evidence as unverified, and record exact tested hardware, workload,
duration, and failures. A finite soak cannot establish an unlimited guarantee.

### Test interaction preference (user instruction, 2026-09-07)

Use the local control API and headless test processes to manipulate CoreVideo
while the user is using the PC. Do not take over the desktop with Computer Use
unless the user explicitly requests it again. Existing authorization for tests,
spikes, and soaks in the designated test meeting remains in effect.

## What to work on: `docs/BACKLOG.md` (owner-approved 2026-09-10)

The single ranked list. Pick the top unblocked item there, not the latest incident; a new
defect found mid-task gets a GitHub issue (label `backlog`) and a row in its tier, and the
owner re-ranks. `docs/beta-plan.md` and `docs/FOCUS_PLAN.md` are superseded for ordering
and status. A #419 architecture foundation lands on `main` only together with its first
real consumer, never as an unwired island.

## What this app is

Three processes, not a web app (plus an optional fourth, the OHG show engine host — see
its section below):

- **WinUI 3 (.NET 9) shell** — `native-shell/CoreVideoPro.WinUI/` — the operator console
  (the product). It owns no real-time media; it sends commands and renders shared textures.
- **C++ media core** — `native/` → `corevideo-native.exe` — real-time pixels/PCM:
  D3D11 compositor, audio mixer, recorder, output senders.
- **Zoom engine subprocess** — `native/zoom-engine/` → `corevideo-zoom-engine.exe` —
  speaks the Zoom Meeting SDK, writes raw **I420** frames to shared memory.

IPC: JSON-line commands/snapshots over child stdin/stdout pipes; video as keyed-mutex **DXGI shared
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
owning the Engine/Take/Record/Stream async command bodies, in-flight guards, #286 rollback
(scenes + the media selection the Take moved, T1.3),
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

## Cutting a Windows beta (local, unsigned) — the runbook

Betas are built LOCALLY and published as GitHub PRE-releases tagged `beta-YYYY-MM-DD-<sha7>`.
The tag-driven `release.yml` (`v*`) needs signing secrets that do not exist yet, so it is not the
beta path. Steps:

1. **Worktree.** Make a fresh DETACHED worktree at the main commit, then copy
   `native-core/zoom-runtime/windows` in.
2. **Native core.** `npm run build:native-dev` with `ZOOM_SDK_DIR` set. Never hand-write cmake:
   that once silently built a software "Stub" core. Confirm `COREVIDEO_WITH_D3D11:BOOL=ON`. Run
   `native/build-dev/corevideo-native-tests.exe`.
3. **Shell.** Publish from a CLEAN tree:
   `dotnet publish native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj -c Release -r win-x64
   --self-contained true -p:WindowsAppSDKSelfContained=true -p:Platform=x64 -o <publish>`.
4. **Package and install.**
   - `scripts/package-alpha.ps1 -ReleaseId … -PublishDirectory … -NativeBuildDirectory …`
   - `scripts/package-alpha-installer.ps1 -Archive <zip> -VcRedist <VS-bundled vc_redist.x64.exe>`.
     The copy in Downloads is too old.
   - `scripts/alpha/Test-AlphaPackage.ps1` and `Test-AlphaInstaller.ps1`, which does a real silent
     install, runs the startup probe, then uninstalls.
5. **Publish.** `gh release create <tag> --prerelease --target <sha>` with the six assets: the zip,
   the Setup .exe, their `.sha256` files, their manifests, and the top-level manifest.

**THE STALE-PRI TRAP (caught before publish on beta-2026-09-10-3a3bedf).** A publish run WITHOUT
`-p:WindowsAppSDKSelfContained=true` leaves an app-only `CoreVideoPro.WinUI.pri` (~177 KB), and a
later flagged publish REUSES it. The installed app then dies at launch with `XamlParseException:
Cannot locate resource from 'ms-appx:///Microsoft.UI.Xaml/Themes/themeresources.xaml'`. The file
count is identical, so only these two gates catch it, and both must pass before packaging:
- the `.pri` is ~2.36 MB;
- `CoreVideoPro.WinUI.exe --verify-runtime <json>` on the publish exits 0.

**Before and after, live.** `python scripts/qa/live-check-sources-audio.py [seconds]` reads ONLY the
control API, sends no input, and checks four things in a real meeting:
- wall guests have video;
- camera-off participants hold no video feed;
- the app muted no guest;
- talking causes no subscription churn, meters are live, and the snapshot is fresh.

Run it on the previous build first, then on the new one.

## Observing a RUNNING core: `GET /snapshot` (2026-09-09)

`GET http://127.0.0.1:8011/snapshot` serves **the core's own sessionState JSON**, verbatim,
from the snapshot the shell already holds. It exists because `ControlState` forwards only a
handful of hand-picked `Native*` fields and the typed `NativeMediaCoreStateSnapshot` binds only
what the shell consumes — encoder evidence, real-time worker evidence, the program buffer,
tiles, multiviewer, browser sources and ~40 other nodes were parsed and dropped. A qualification
judge can now watch the core the operator is actually running instead of spawning its own.

- **How it flows.** `CoreProtocolParser` / `MediaCoreSupervisor` tag every parsed snapshot with
  `RawJson` + `RawReceivedUtc` (`[JsonIgnore]`, in-process only). **Both** sync paths must tag:
  a real core answers with a WIRE state that is mapped onto a *synthesized* base
  (`NativeMediaCoreStateMapper`), so tagging only `TryParseSyncSnapshot` leaves the live path
  with no raw at all — that is exactly the bug this endpoint was first caught by.
  `StudioControlSurface` (an `INativeSnapshotObserver`) reads the reference the bridge already
  publishes: no core round-trip, no UI marshal, no render-path lock.
- **Envelope.** Always states `available`, `receivedUtc` (shell receipt), `servedUtc`, `ageMs`
  and `stale` (>2s). Absent / synthesized / unparseable snapshots answer `available:false` with
  a `reasonCode`, never something that reads as current.
- **Auth.** Same rules as every other route — loopback needs none, a LAN bind (`"+"`) refuses to
  start without `COREVIDEO_CONTROL_TOKEN`. No new unauthenticated surface.
- **Redaction** (`CoreSnapshotObserver`, tests in `CoreSnapshotObserverTests`): applied at the
  observation boundary, not per tick. It **walks the JSON** and filters each string value through
  `SupportBundleLogRedactor` + drops secret-NAMED values. Do not run that redactor over the
  document as text: its rtmp rule is greedy over non-whitespace, so one URL in a `lastError`
  eats the closing quote and the properties after it. Name matching is by suffix
  (`…Key/Token/Secret/Password/Passphrase/Jwt/Zak`) so `keyPhase`/`keyer`/`keyPosition` survive.
  Audit result: the snapshot carries **no** stream keys or passphrases (destination settings are
  inputs; the RTMP/SRT adapters already publish `redactedEndpoint`), but it does carry adapter
  free text that could quote one, operator browser-source URLs, and recording paths — paths are
  deliberately kept, matching the support bundle's "ISO paths are not secrets".
- **Per-layer geometry does not exist on the wire.** `RenderedProgramSources.h` publishes exactly
  `layerId/sourceId/participantId/kind`; rect / fit / opacity / fill colour live on
  `CompositorRenderPlanLayer` inside the core and are never serialized. `ControlProgramVideoSource`
  adds `order` (the index in the core's already-sorted publish order). Only the `tiles` node
  carries a rect per member. Adding real geometry is a **core** change.

## Testing multi-participant WITHOUT a real meeting (important)

### Current test-meeting authorization (2026-09-06)

The user explicitly authorized any automatic test, spike, or soak in the current
test meeting, including exercising the HTTP control API (`127.0.0.1:8011`), scene
changes, Take, graphics, automation, and restarting/rejoining for validation.
Do not ask again before running these tests. Validate observed running behavior,
not only unit tests, and verify the executable path actually contains the fix.
This permission applies to this test meeting, not unrelated future live shows.

### Synthetic meeting setup

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
- **The post-Exit dispatcher drain on a normal close (T1.7, #457, 2026-09-10).** Here the
  stack is `DispatcherQueue::DeferInvokeCallback` under
  `DispatcherQueueController::ShutdownQueue` under `FrameworkApplication::StartDesktop`, on the
  UI thread, AFTER `shutdown: resources released`. `Application.Current.Exit()` handed the
  process back to XAML. Its shutdown drain then ran a leftover work item against torn-down XAML,
  the item returned a failure HRESULT, and CoreMessaging fail-fasted. The failing item was a
  `DispatcherQueueTimer::TimerCallback` (stowed E_UNEXPECTED) in one dump and a non-managed
  callback (stowed E_ABORT) in the other. It hit 2 of 10 graceful closes, both after long
  in-meeting sessions. Nothing aired, but each one costs a 1.1 GB dump, a WER APPCRASH, and a
  false crash prompt on the next launch. FIXED: after a CLEAN shutdown, `MainWindow.ShutdownAsync`
  calls `ShutdownCompletion.Complete`. It stops the view model's leftover timers, writes the last
  log line, and calls `TerminateProcess` on its own process. It never calls
  `Application.Current.Exit()`. It uses TerminateProcess, not `Environment.Exit`, because
  `ExitProcess` would still run `DLL_PROCESS_DETACH` in Microsoft.UI.Xaml and CoreMessagingXP.
  The logs are synchronous, and no ProcessExit handlers exist. A failed or timed-out cleanup
  keeps the `ApplicationLifecycle.ForceExit` fallback. `PrepareForShutdown` also stops the view
  model's DispatcherQueueTimers (defence in depth). **Rule: never hand a torn-down shell back to
  WinUI's shutdown drain.** Unit tests (`ShutdownCompletionTests`) pin the order and the gate.
  (T1.8 put a close guard in FRONT of this path — see "Engine teardown order": closing while
  recording/streaming asks first and finishes the files before `ShutdownAsync` starts.)
  The proof is a scripted close-cycle loop on the real app: zero new
  `CoreVideoPro.WinUI.exe.*.dmp` and zero Application Error 1000 events.

Rules of thumb: never replace a bound collection at frame rate (sync in place / diff);
keep one stable swap chain per surface (program, preview, one multiview);
present with **skip-present** (only on a new keyed-mutex frame) — smooth-present crashes
~31s in.

## D3D device loss is RECOVERED, by generation (beta slice, 2026-09-09)

The shared device (`Direct3D11InteropService.s_sharedDevice`) can die mid-show — a TDR, a
driver upgrade, a hardware fault. It used to die **permanently**: the present threw,
one host dropped to CPU fallback, and nothing ever cleared `s_sharedDevice`, so
`EnsureDevice` kept returning true for a dead device and EVERY surface stayed on CPU
until the app restarted. Nothing in the tree called `GetDeviceRemovedReason`, so the log
never named the cause. On a tester's machine that is a silently degraded show we cannot
diagnose. Now:

- **Classification is pure and tested** (`Services/DeviceLossPolicy.cs`,
  `DeviceLossPolicyTests`): only `DXGI_ERROR_DEVICE_REMOVED`/`DEVICE_RESET` — or a
  negative `GetDeviceRemovedReason` — retire the device. Everything else
  (`WAS_STILL_DRAWING`, occlusion, a resource-pressure create failure, a stale shared
  handle) keeps the existing per-handle invalidation path. `PresentationAttempt` is
  unchanged.
- **Retirement is a GENERATION bump, never ad-hoc field clearing.** `RetireDevice` is a
  no-op for an already-retired generation, so a late callback from the dead device cannot
  resurrect anything. It disposes the whole `HandleIngest` map and drops the device/context
  RCWs — it does NOT dispose them, because other hosts' swap chains still hold native refs.
  Each host rebuilds its swap chain when it adopts the new generation (a CREATE — **still
  never `ResizeBuffers`**), and clears `_invalidHandles`, since a handle blacklisted against
  the dead device is usually fine against the new one.
- **Recovery is automatic and BOUNDED.** No restart needed: the next
  `CompositionTarget.Rendering` tick past the backoff deadline recreates the device and GPU
  presentation resumes. `DeviceLossPolicy.DeviceRecoveryPolicy` is the same shape as
  `ShowEngineRestartPolicy`/`MediaCoreSupervisor`/`BrowserHostRestartPolicy`/
  `PluginHostRespawnPolicy` — 250ms→1s→2s→5s→10s→30s, **give up after 5 consecutive
  failures**, 60s of healthy presenting resets the budget. Recreation never runs inline on
  the failing frame (that frame just drops to CPU), so the UI thread never eats a
  device-create stall on the same tick it already lost.
- **What a tester's log shows** (launch.log, which the support bundle already collects):
  `d3d: DEVICE LOST context=… generation=N->N+1 removedReason=0x887A0006 (DEVICE_HUNG …)
  totalLosses=K`, then `d3d: device recovery attempt K scheduled in Nms`, then either
  `d3d: DEVICE RECOVERED generation=… recreates=… losses=…` or
  `d3d: DEVICE RECOVERY ABANDONED after 5 consecutive failures …`. The per-vsync "no device"
  line is throttled to 5s so it can't roll the diagnosis out of the bundle.
- **Unproven, honestly:** no real TDR was provoked (deliberately). The GPU-side ordering is
  reasoned + reviewed, not executed; only the classify-and-decide half is test-covered.

## Fault-injection seams (beta slice, 2026-09-09) — how to prove stability work

Gates G2 and G3 in `docs/production-realtime-execution-plan.md` (on the PR #419 branch) are written in terms of
injected faults, and until now nothing in this tree could inject anything, so neither
could be attempted. There are now three seams. **All three are test-only, and the guard
is structural, not conditional compilation** — `corevideo-native-tests` links the same
`corevideo_native` library the product does, so a compile-time gate would delete the seam
from the tests too. The guarantee is the same one
`MediaCore::setStillImageDecoderForTest` already relies on: no env var, no command, no
config key, no wire field reaches any of them, and nothing outside `native/tests/` or
`CoreVideoPro.WinUI.Tests` calls them.

- **Forced device loss + blocked present** (shell):
  `native-shell/CoreVideoPro.WinUI/Services/PresentationFaultInjection.cs`. `internal`
  (the WinUI assembly's only `InternalsVisibleTo` is the test project) AND arming refuses
  unless `CoreVideoPro.WinUI.Tests` is loaded in the process — a positive check that fails
  closed. The present hot path reads exactly one static bool. Arming returns an
  `IDisposable` that disarms, and a blocked present ALWAYS carries a hard timeout: a seam
  that can wedge the process it exists to diagnose is not a diagnostic.
- **Blocked monitor render** (core): `native/src/compositor/CompositorFaultInjection.h`,
  consulted at the top of `D3D11Compositor::renderMultiview` / `renderPreview`. One
  relaxed atomic bool per monitor pass; the stall is a plain function pointer, so arming
  allocates nothing and null is the disarmed state.

**What the seams proved, and what they did not:**

- **Device-loss recovery survives a real injected loss.** `PresentationFaultInjectionTests`
  drives the real `Direct3D11InteropService` against a real D3D11 device: the loss is
  classified, the generation is retired, the bounded ladder schedules, a real
  `D3D11CreateDevice` runs at the 250ms rung (measured 266ms), the host adopts the new
  generation, its per-generation handle blacklist is cleared, and the loss report is
  accurate. A second host observing the same loss does NOT spend a second ladder rung.
  **Still unproven:** the swap-chain rebuild and on-screen GPU presentation — a
  `SwapChainPanel` is a XAML object and the test host runs with the Windows App SDK
  bootstrap disabled, so no panel can exist there. That last hop needs the app.
- **A blocked present still blocks UI control** — presents run on the UI thread from
  `CompositionTarget.Rendering`, so the property G2 wants is FALSE today, and no test can
  make it come out otherwise while that is the shape. What is proven is that the
  DIAGNOSIS is independent: `PresentationStageWatchdog` names the stalled stage from its
  own thread while the present is still stuck. `BlockedPresentFaultTests` pins the
  structural gap, so moving presents off the UI thread fails a test that says to come back
  and prove the real property.
- **Program is NOT isolated from monitor rendering.** `MonitorRenderFaultInjectionTest`
  measured it on this rig (RTX 4090, 1080p Program + 720p Preview, 3-frame buffer): with
  no fault, 121 produced / 124 delivered / 4 underruns per 2s; with a sustained 25ms
  Preview stall (unmitigated, at the product's 1ms timer resolution), 77 / 77 / 51 —
  **a Preview compositor overrunning by ~1.5 frame periods costs Program ~36% of its
  frames** (≈ 1 − 16.7/25.7, the slots a 25.7ms pass leaves). The originally published
  "65 / 65 / 64 — HALF its frames" was measured at the default ~15.6ms timer tick, where
  the seam's "25ms" stall actually slept ~31ms. Program `render()`, `renderMultiview()` and
  `renderPreview()` share one render thread and one D3D immediate context. A one-off stall
  costs only the slots it spans and Program recovers on its own. **The program buffer does
  not help here** — it protects delivery timing for frames that were produced, and these
  frames were never rendered. The monitor-compositor split in
  `docs/production-realtime-completion-plan.md` (on the PR #419 branch) is what would give G2 its property; the
  sustained-stall case must be INVERTED when that lands, not deleted.
  **Beta MITIGATION shipped (T1.4 / #431, 2026-09-10): monitor load-shedding.** Program
  always renders; under sustained overload the MONITOR passes give way.
  `core/MonitorShedPolicy.h` (pure, `MonitorShedPolicyTest.cpp`) is fed once per DISPLAY
  tick in `MediaCore::renderSyntheticTick` (videoOnly only — synthetic full ticks never
  feed it, so ordinary unit tests stay timing-free) with the tick's non-monitor render
  cost, the last measured cost of one multiview+preview cycle (each refreshed only when
  that pass runs — and only when that pass is SHEDDABLE: a compositor that exports no
  handle is forced every tick, so its cost is fed as 0), and the budget `1s / outputFps_`. It projects the per-tick load at
  divisor d as `program + monitor/d` and returns a monitor cadence divisor 1/2/3 that
  multiplies `kMultiviewTickDivisor` (still 1 = the healthy cadence) and applies to BOTH
  the multiview pass (phase 0) and the preview pass (phase 1 — staggered so a shed cycle
  never stacks both). Constants: `kEnterAfterOverBudgetTicks = 3` (one or two slow ticks
  are a shader compile; the buffer rides them out), `kShedAboveUtilisationPercent = 90`
  (NOT 100 — jitter headroom: under shedding the tick that runs the monitor pass
  finishes late, and the cheap ticks after it must absorb that plus scheduler / pacer /
  GPU jitter; a lost Program slot costs far more than shedding a level early),
  `kRecoverAfterHealthyTicks = 60` (1s, 20x slower than entry — late
  recovery costs monitor smoothness, early recovery costs Program),
  `kRecoveryHeadroomPercent = 75` (recover only when the next LOWER divisor fits in 75%:
  the 75–90% band is the anti-flap hysteresis), `kMaxDivisor = 3`. It steps one level at
  a time both ways. CPU-deadline misses are deliberately NOT an input (a shed monitor
  tick finishing late is expected and absorbed by the buffer; feeding it back would pin
  every recoverable overload at 3). On a shed tick the cached preview texture/dims are
  REPUBLISHED exactly like the multiview cache (`lastPreviewTexture_`), and the first
  tick, a structural change (`*StructureEmitted_ = false`) or an empty cache still force
  the pass. No locks, no allocation, up to seven clock reads per tick (tick start/end,
  monitor start/end, plus the end of the multiview pass and the start/end of the preview
  pass when they run). **A Program- or GPU-bound overload sheds monitors too, by design:**
  the policy sees only CPU-submission time, and D3D submission is async, so GPU cost
  queued by a monitor pass often surfaces inside Program's NEXT call — shedding monitors
  is the one lever this thread has, and it can only give Program time back.
  Observability: `sessionState().realtimeEvidence.monitorShed {divisor, level,
  enteredCount, shedTicks, lastReason, lastTransitionProgramMs,
  lastTransitionMonitorCycleMs, lastTransitionBudgetMs}` (published unconditionally;
  `lastReason` = none | over-budget | recovered; the `lastTransition*` fields are the
  observation that caused the last divisor change, so a monitor-bound shed is
  distinguishable from a Program/GPU-bound one) and ONE `[monitor-shed] enter|step-up|step-down|exit …` line per state
  change via `nativeLogf`. **Measured (same rig, 1ms timer, one run, 2s windows):**
  raw compositor, no shed — baseline 121/124/4, sustained 25ms 77/77/51; through
  MediaCore with the shed — baseline 121/124/4, sustained 25ms **119/119/9** at divisor 2,
  back to divisor 1 within 3s of the stall ending (182/182/6 per 3s). **What it does NOT
  give:** isolation. Program is protected by cadence: the entry ticks are paid in full,
  and a single monitor pass longer than the slack a 1/3 cadence leaves (~2 frame periods)
  still costs Program slots. `ASustainedMonitorStallIsShedAndProgramKeepsItsRate` asserts
  both halves — the raw leg still delivers <75% of baseline with underruns up by >30,
  a bound tied to the stall (~35% expected loss), and that is the assertion to INVERT
  when the split lands; the shed leg keeps >=90% — and the timing tests now run under
  `timeBeginPeriod(1)` like the product's render thread: at the default ~15.6ms tick the
  seam's "25ms" stall really slept ~31ms (that is what the 65/121 above measured) and
  the harness's own slot sleeps overshot by up to a frame.

## One destination failing cannot take the show down (beta slice, PR19 — output supervisor)

Each network destination now has a supervisor with **a generation, a health signal and a
bounded restart policy**. It is a decorator that sits OUTSIDE the per-protocol async writer:

```
CompositeOutputSender -> SupervisedOutputSender -> AsyncOutputSender -> RTMP / SRT / NDI adapter
```

**Outside, not inside — that is the entire isolation argument.** Every call the supervisor
makes lands on `AsyncOutputSender` (enqueue-and-return; `session()` is a cached snapshot),
so a wedged FFmpeg pipe or a blocked libNDI send cannot block the supervisor, the control
path, or a sibling destination. Restarts run on the supervisor's own thread — never the
render tick, never under `coreMutex`, the same reason `BrowserSourceHostAdapter` spawns on
its own supervisor thread.

- **The ladder is the HOUSE ladder** (`modules/OutputDestinationSupervisorPolicy.h`), adopted
  verbatim from `BrowserHostRestartPolicy` / `PluginHostRespawnPolicy` / `ShowEngineRestartPolicy`
  / `MediaCoreSupervisor`: 5→10→20→40→60 s, **give up after 5 consecutive failures**, reset the
  budget after a healthy RUN, and an operator reset (`recover`) always clears give-up.
- **HEALTHY = accepted units advancing, re-evaluated at read time.** `framesSent +
  audioFramesSent` is the only number that can only move when the transport actually took
  bytes from us. A launched FFmpeg child, a non-null `NDIlib_send_create`, or a status string
  reading "live" are all LAUNCHES, and each has been observed to survive the destination
  dying. Rule 7, at the exact point it was ignored.
- **Three budgets, three questions, and they are deliberately different numbers.**
  1000 ms = "is it producing right now?" (the SAME declared value as
  `core::kProducingProgressStaleMs` / the qualification judge's `encoderQueueAgeMs`, so health
  and the truthful lifecycle cannot drift apart); 5000 ms = "is it coming back on its own?"
  — restarting an encoder over a 1.2 s hiccup costs a reconnect and a keyframe for nothing;
  30 s = "has it earned its budget back?"; 15 s = "did it ever come up at all?".
- **Generations are checked by OBJECT IDENTITY, not a counter** (`DestinationEventStamp`
  carries a `weak_ptr` to the run token). `ShowEngineSupervisor`'s stated reason applies
  exactly: a number-only guard reds nothing when the child object is swapped, and a retired
  FFmpeg child's last snapshot would otherwise vouch for the generation that replaced it.
- **A terminal failure BYPASSES the ladder.** The show engine's exit 78 is the model: an
  unreachable endpoint is retryable, an inadmissible configuration is not.
  `isTerminalResultCode` names them (`endpoint-missing`, `rtmp-settings-{missing,invalid}`,
  `source-name-invalid`, `runtime-missing`, `ffmpeg-missing`, `*-output-unavailable`), and an
  **unknown code is RETRYABLE** — guessing "terminal" would silently stop protecting a
  destination, which is the failure this exists to prevent.
- **Give-up is LOUD, never a private opinion.** The supervisor rewrites the published sender
  record to `status: failed` / `lastResultCode: "supervisor-gave-up"` with its reason, so
  `core::SenderLifecyclePolicy` (which reads `status`/`destinationHealth`) reports it as
  failed everywhere downstream. There is no second status machine.
- **It reaches a support bundle.** `outputSenders.senders[].supervisor` carries generation,
  healthy, gaveUp, consecutiveFailures, restarts, nextAttemptInMs, lastProgressAgeMs,
  acceptedUnits, staleEventsRejected, malformedObservations, failureClass, reason — and
  `/snapshot` serves `sessionState` verbatim, so it is visible immediately.
- **NDI's residual risk is PUBLISHED, not fixed.** `destinationIsolationTraits()` is the one
  place that says which destinations are actually contained: RTMP/SRT are out-of-process
  FFmpeg children in a job object and `interrupt()` ends them; **NDI is in-process** (runtime
  `Processing.NDI.Lib.x64.dll`, `send_send_video_v2` on the writer thread with `clock_video`
  set) and **implements no `interrupt()` at all**, so a wedged libNDI send can be detected,
  published and escalated but NOT released — the writer is detached after
  `AsyncOutputSender`'s 2 s grace and leaks until exit. Moving NDI out of process is PR 20/28.
  An UNKNOWN destination is assumed unprotected; never claim isolation that was not established.

Tests: `native/tests/OutputDestinationSupervisorTest.cpp` — the ladder, the health rule, and
a deterministic in-process fault host covering hang, crash, malformed reply, IPC disconnect,
stale completion, restart, terminal failure, and off-caller-thread action.

**Build gotcha this shipped with:** widening `OutputSender` (in `Interfaces.h`, included
almost everywhere) while another agent was building in the same `native/build-dev` left a
test TU compiled against the OLD struct. The symptom was NOT a link error — it was
`FAST_FAIL_STACK_COOKIE_CHECK_FAILURE (0xC0000409)` inside a test that stack-constructs
`MediaCore`, at a different point on each run, which reads exactly like a heap race in
someone else's code. **After changing a widely-included struct, rebuild with `--clean-first`
before believing any crash you see.**

## Destination lifecycle is TRUTHFUL, and Stop does not claim completion (beta slice, PR22)

Every output destination — the recording writer and each RTMP/SRT/NDI sender — reports
one contract state machine, and it is decided from evidence, never from a request:

```
requested -> preparing -> producing -> stopping -> finalizing -> completed | failed | interrupted
```

Three things changed, each of which was a lie an operator could read:

- **Stop used to report success before the work was done.** `MediaCore::stopRecordingSession`
  assigned `recordingStatus_ = "stopped"` **immediately**, before it had even called
  `encoder->stopRecording()`. The RPC returned there — so the operator was told the
  recording had finished while the FIFO barrier was still draining and the moov atom had
  not been written. The truth lived only in `recording.lifecycle`, and the two fields
  disagreed for the whole finalize window. Stop now reports that stopping has **begun**
  (`status: "stopping"`, `writerStatus: "finalizing"`), and the terminal state arrives from
  the writer thread when the barrier has drained and finalization has actually succeeded or
  failed. **The RPC still does not block** — the asynchronous finalization was always
  correct, and `scripts/validate-recording-finalization.mjs` proves it completes with the
  core alive. Only the lying field was fixed.
- **`recording.status` / `recording.writerStatus` are now PROJECTIONS of the lifecycle**
  (`core::publishedRecordingStatus` / `publishedRecordingWriterStatus`, pure and tested), so
  they cannot contradict it again. `recordingStatus_` survives as the internal desired-state
  gate that the render gather and the idempotent-start dedup read — it is not, and never
  was, evidence of what the writer is doing. New values: `stopping`, `starting`,
  `interrupted`, `idle` (and `opening`/`finalizing`/`stalled` on the writer side).
- **`producing` REQUIRES FRESH PROGRESS.** The old `live` was latched the moment a request
  produced its first frame and was never re-examined, so a wedged writer reported healthy
  for the rest of the show. `AsyncEncoderSink::session()` now re-decides the active state
  against the clock at **READ** time — which is the only thing that works, because a writer
  blocked inside the wrapped sink applies no further items and therefore publishes no
  further snapshots. A destination whose last observed progress is older than the budget
  decays to `interrupted`, and returns to `producing` when real progress resumes.
  **The budget is not a new number:** it is
  `scripts/qa/runtime-snapshot-qualification.mjs` `DEFAULT_RUNTIME_POLICY.encoderQueueAgeMs`
  (1000 ms), reused so there is one declared definition of "the encoder has stopped moving".

**Senders have a lifecycle now.** `OutputSender.lifecycle` (snapshot
`outputSenders.senders[].lifecycle`) gives every destination a state and a terminal
outcome. It is computed centrally in `MediaCore::evaluateSenderLifecycle` from the pure
`core::SenderLifecyclePolicy`, so RTMP/SRT/NDI keep exactly ONE status machine each instead
of gaining a second. Two traps encoded there: **`lastError` is sticky history, not current
state** (a genuinely streaming SRT sender still carries its first-tick "waiting for composed
BGRA program pixels" — treating that as failure reports every live stream as broken; failure
is `status`/`destinationHealth`), and freshness is sampled between snapshot reads, so a dead
sender decays within one poll interval of the budget, not instantly.

**The decision logic is pure and testable without a writer** — `native/src/core/OutputLifecyclePolicy.h`,
the `CaptureReaderStallPolicy`/`NativeUvcCapturePolicy` shape, covered by
`native/tests/OutputLifecyclePolicyTest.cpp` plus two integration tests in
`AsyncEncoderSinkTest.cpp`: a writer wedged inside the wrapped sink decays out of
`producing` and recovers, and Stop never claims completion until finalize returns.

**It reaches a support bundle**, which is the point — `SupportBundleBuilder` projects both
the recording lifecycle and every sender lifecycle (redaction-safe: `Error` rides the same
endpoint filter), and triage names a failed/interrupted destination, a bundle exported
during the finalize window, and a stream that ended without sending media.

**Contract:** `starting`/`live` remain in the `OutputLifecycle` enum as the RETIRED names so
a newer consumer can read an older core; new producers must not emit them. Absent lifecycle
means UNKNOWN, never healthy. Vocabulary and rules: `contracts/README.md`.

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
  **Media clips are governed by the shell's `"media"` strip and sends via a CORE
  ALIAS (T1.6 / #455, `core/AudioControlSourcePolicy.h`).** Since #408 the decoder
  labels each clip's PCM `media:<assetId>`, but the shell has ONE "Media playback"
  row/strip/send set keyed `"media"`; exact-id matching made the FADER LAW drop every
  clip and no send reached a bus, so media audio was silent on master, stream and
  every recording while the shell looked fine. The routed-source build now
  PRE-SUMS every `media:*` clip without its own strip/send into ONE routed source
  keyed `"media"` (worker-owned scratch, no per-tick allocation), so the Media strip's
  gate/compressor/inserts/VST run ONCE on the combined signal (a VST insert must never
  be exchanged once per clip against one host instance), the `"media"` sends route it,
  and the strip meters/GR-meters the sum. Clips keep their own ids in the mixer
  session. An exact `media:<assetId>` strip or send keeps that clip separate; a clip
  with its own send rows does NOT inherit the generic row's other cells. Do NOT fix it by sending
  per-clip ids from the shell: the routing grid un-routes cells the core did not
  echo, so the Media row would switch itself off ~2 s later. Proof:
  `MediaCoreCommand.SceneMediaAudioReachesMasterThroughTheShellMediaStrip` and
  `node scripts/validate-record-audio.mjs --media` (real MF decoder, AAC 440 Hz clip,
  judges the recording's decoded audio). The FADER LAW line now says "unrouted
  source (no sends)" for a strip-less source nothing routes (perGuestIso's zoom-mix).
- **THE A1's MUTE IS ONLY SET BY THE A1 (#481).** A live meeting caught CoreVideo
  muting Courtney, Guy and CJ on its own while the console showed nobody muted: a
  new audio channel's `Muted` was seeded from the core's EFFECTIVE mute
  (`nativeChannel.Muted`, which folds in the Zoom mute), and then `prior?.Muted`
  latched that forever — a guest who was Zoom-muted the instant their channel
  first appeared stayed muted on every bus after they unmuted in Zoom. Fix: a new
  channel's `Muted` starts `false`, full stop, via the pure
  `StudioViewModel.ResolveMergedChannelMute(prior)`; the Zoom mute lives only in
  `SourceMuted` and is never adopted into `Muted`. It is still ORed into the
  EFFECTIVE mute sent to the core (`ResolveEffectiveAudioMute`) — harmless, since
  Zoom sends no audio while muted — but that gating is recomputed fresh every
  wire build, never latched into the A1's state. The core also now publishes
  PRE-MUTE `inputRmsDbfs`/`inputPeakDbfs` per channel (measured before mute/
  fader) so a muted, talking guest still shows on the meter (dimmed) instead of
  reading silence — the OUTPUT `rmsDbfs`/`peakDbfs` stay exactly as documented
  above. **Round 1 review correction: test the WHOLE merge, not just the leaf.**
  `ResolveMergedChannelMute(prior)` alone takes no native channel, so a
  regression that put `?? nativeChannel.Muted` back at the call site could not
  fail any test built only against that function. The real call site is now
  `StudioViewModel.MergeNativeAudioChannel(nativeChannel, prior, sourceMuted)` —
  the WHOLE native-channel→`ParticipantAudioMix` merge, extracted as one pure
  static — and the tests drive it through the live two-rebuild sequence (arrives
  Zoom-muted with no prior → live; guest unmutes in Zoom before the core's next
  wire echoes it → still live). Also fixed there: `SourceMuted` must never carry
  forward from `prior` on a roster miss (a missing roster entry is not evidence
  of a Zoom mute) — the caller resolves it fresh every rebuild and passes
  `false` on a miss, never `prior?.SourceMuted`. **Any new derived-state merge in
  this codebase should default to the pure-function-over-the-whole-decision
  shape, not a leaf function that omits the variable the regression would
  restore** — a test that cannot construct the regressed expression cannot catch
  it.
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

- **AN EMPTY RENDER PLAN IS NOT "DRAW NOTHING" (2026-08-15, CoreVideo Tiles T1).**
  All THREE compositors — `D3D11CompositorAdapter::resolveLayers`,
  `ProgramFramePreview`'s `buildProgramFramePreview`, and
  `MetalCompositorAdapter::resolveLayers` — carry their own `renderPlan.layers.empty()`
  fallback that improvises **one full-canvas grid cell per DECODED FRAME**. So any
  scene path that legitimately produces zero layers puts a grid of whatever the core
  happens to be decoding onto PROGRAM — sources that are not in the scene at all —
  and PROGRAM is inherited by the virtual camera, every recording and every stream.
  The Tiles wall hit this exactly: `buildRenderPlanForScene` suppresses the legacy
  full-canvas fallback whenever a wall is active, and the wall's background layer was
  emitted *inside* the `!admitted.empty()` gate — so a wall whose members were all
  stale (and, transiently, EVERY Tiles take before first frames land) shipped an empty
  plan. **Rule: any code path that owns a scene's video layers must always emit at
  least one layer.** The wall's background `push_back` now sits above the admission
  gate; regression test `TilesRenderPlan.AnAllStaleWallStillEmitsItsBackground`
  asserts the plan is non-empty, not just that the tiles are absent.
  **And the gate deciding "is a wall active" is `wall.present` ALONE — never
  `present && !members.empty()`.** `TilesLayerPayloadBuilder.Build` sends
  `members: []` whenever every guest is video-off or the roster is momentarily empty
  (an ORDINARY meeting state), so a members-aware gate re-opened the identical
  on-air hole one level up: no background AND no fallback suppression, i.e. an
  improvised grid on PROGRAM the moment all cameras went off. A configured wall
  with nobody live shows its BACKGROUND. The same `.present` gate is used by the
  `lastRenderPlan_` cache and the snapshot `tiles` node so they can never disagree
  (and so the all-cameras-off state stays OBSERVABLE — a node that vanishes in the
  case worth detecting is the multiviewer mistake again). The wall also **counts as
  a layer in `hasPreviewScene()`**: that tally was routes + background + overlays
  only, so a Tiles preview scene with no media background and no overlay scored
  ZERO, the third composite never ran, the preview shared-texture handle was
  cleared, and the operator's preview monitor silently fell back to the
  single-source path — never showing the wall it was about to take. Tests:
  `AMemberLessWallStillOwnsTheSceneAndEmitsItsBackground`,
  `APreviewSceneCarryingOnlyAWallStillComposites`.
  Related, same family: **routes and the wall share ONE order namespace** (tiles-bg at
  `wall.order`, tile #i at `wall.order + 1 + i`), so a surviving gallery route at order
  2 composites between tiles. The shell keeps that impossible **by construction** —
  `StudioViewModel.BuildProductionSyncContext` serializes an EMPTY route list for a
  `DynamicGallery` scene, at the point the wire is built, never relying on the
  coalesced UI reconcile pass (`ReconcileDynamicGalleryRoutes`) having run. The
  core still ACCEPTS routes+wall from any producer, so both scene parse sites now
  push a deduped `sceneValidationWarnings_` entry when they arrive together —
  audible drift, not latent. (Dedupe matters: `applyPreviewScene` rides the
  REPEATING spine sync and does NOT clear that vector, so an unconditional push
  there grows without bound on a scene-flip loop.)
  And **Metal has no `hasFillColor` branch** — the wall background renders as a
  dark-grey slab on macOS; named in a comment at the site, owned by
  `docs/corevideo-tiles-iso-scaling-plan.md` implementation slice 3 (Metal parity).

- **A TILES WALL TAKEN FROM PREVIEW IS CUT TO, NEVER REDRAWN (owner report,
  live show 2026-09-09).** "I am ok if panelists leave and join the video but
  what I can't have is a total rerender from what is in preview to program like
  it is loading for the first time." The wall key is `sceneId + ":" + layerId`
  and the layer id is derived from the scene id, so the SAME gallery has the
  SAME key on both buses — `MediaCore` holds two animation objects
  (`programTilesAnimation_` / `previewTilesAnimation_`) and the program one used
  to reset its animator the moment the key it had never held arrived. Two
  corrections, both in `compositor/TilesPlanAnimation.h`:
  `adoptSettledFrom()` MOVES a settled wall's state from preview to program on
  the take tick (exact key match + every sampled tile `atRest` only; the source
  is reset, never aliased, so the next wall cued in preview starts clean), and
  `advance()` no longer samples an EMPTY target set for a wall that is still
  present and has already drawn tiles. That second one is what actually produced
  the reported replay: an all-stale beat (`kTilesStaleFrameMs`, an ordinary
  state — see the empty-plan rule above) erased every retained tile AND consumed
  the animator's adoption, so the instant frames returned the whole wall faded in
  from alpha 0. A COLD wall's first tick is untouched, so a wall that was never
  in preview behaves exactly as before. Not a contributor, measured: preview and
  program share one device and one `sourceTextures_` cache keyed by
  `participantId` (`D3D11CompositorAdapter`), so tile textures are already warm
  across a take. Tests: `TilesRenderPlan.AWallSettledInPreviewIsAlreadySettledOnItsFirstProgramFrame`,
  `AWallTakenWhileItsFramesLapseIsStillCutToNotRedrawn` (fails without the fix),
  `AWallThatWasNeverInPreviewIsHandedNothing`, plus three `TilesAnimator.*`
  hand-off unit tests.
  **The wall's LIVE BACKGROUND had the same defect and needed a different fix
  (same show, follow-up report: "Tiles background still refreshing on cut to
  program, that should be seamless").** The wall emits TWO background layers:
  `tiles-bg:<layerId>`, a sourceless solid, emitted unconditionally above the
  admission gate (safe — it depends only on `!sceneBackground.enabled`, which is
  parsed from the scene payload and cannot move across a take); and
  `tiles-source-bg:<layerId>`, the live background FEED, which rode
  `admitTilesMembers` — the SAME 1500 ms `kTilesStaleFrameMs` gate the tiles go
  through. So one beat of the background source's frameId not advancing dropped
  the layer entirely, program fell through to the solid colour or the scene
  background, and the picture popped back when frames resumed. The gate is now
  `compositor::tilesBackgroundSourceIsDrawable`, which asks the question that
  applies to a BACKDROP instead: is a real-content frame for this source in this
  tick's gather (`TilesMemberFrameAge::hasFrame`)? A stale TILE is still refused
  — it occupies a slot, and holding it seats a dead guest — but a stale
  BACKGROUND competes with nothing, and a backdrop frozen for a beat is
  indistinguishable from a live one where its absence is a full-frame colour
  change on air. `kTilesStaleFrameMs` is deliberately NOT widened: it is shared
  with tile admission and moving it changes wall membership for every source.
  The hold is EVIDENCE, not memory — there is no retained layer and no per-bus
  state, so a source that never arrived or has departed keeps today's behaviour
  exactly and the two buses cannot contaminate each other through it. That
  matters concretely: a `participant-video` layer whose sourceId resolves to no
  frame renders a solid `colorFromParticipantId()` slab OVER the wall background
  (`resolveLayers`), which is worse than the pop; and the layer always carries a
  non-empty participantId, so `RouteSourcePolicy`'s positional-fallback hazard
  stays unreachable. Tests: `TilesRenderPlan.AWallsLiveBackgroundSurvivesATakeAcrossAStaleBeat`
  (the live wall harness, using the new `LiveWallCaptureDevice::freeze()` — a
  frozen feed still DELIVERS with a held frameId; `pause()` is the harsher
  no-frame case) and `AStaleBackgroundIsHeldButAnAbsentOneIsNeverFabricated`.
  Both fail without the gate change.
  **A SECOND, ENGINE-SIDE CONTRIBUTOR EXISTED; IT IS FIXED (#478, 2026-09-11,
  plus fix rounds 1 and 2).** It was: `ZoomMediaSpinePayloadBuilder` ordered video
  candidates active-speaker, program routes, preview routes, then EVERY participant
  in roster order, capped at `maxVideoSubscriptions`; and the core picked resolution
  as `purpose == "active-speaker" ? 1080P : 720P`, with resolution in the dedup key.
  So every speaker change rebuilt two engine renderers, a Tiles scene's EMPTY route
  list reordered the list on a take, and camera-OFF early joiners took the cap ahead
  of a late wall guest (live, 12-person meeting: Alexander, slot 2,
  `subscribed:false`, generation 14, froze whenever he left Preview/Program; three
  camera-off non-wall guests held live subscriptions; `totalChurn` 53->59 in 20 s
  with the owner seeing Tiles "flashing"). Now:
  **(1) ONLY SOURCES ARE SUBSCRIBED (owner rule: "Why are you grabbing sources I
  don't have routed to the multiviewer?").** `MediaCore/Services/ZoomSourceSetPolicy.cs`
  is the ONE pure decision of who is a source, in budget order, PROGRAM FIRST:
  Program routes -> Program Tiles members (+ Zoom wall background) -> Preview routes
  -> Preview Tiles members -> in-show wall slots in slot order -> ISO-armed guests
  (only while "Program + ISOs" is on) -> sticky Tiles audio members. What is on air
  is never disturbed by a cue: an off-air Preview look can never take video (or the
  1080P grant, which the core makes in this same order) from a Program source; a cued
  guest the budget leaves out is named on the multiview PVW cell instead. (Round 1
  briefly ranked Preview routes above Program Tiles; the re-review withdrew it — it
  spent on-air pixels on an off-air cue.) No roster fill, anywhere. VIDEO =
  camera-on sources, capped at 10; AUDIO (`participant-audio`, purpose "mix") = every
  source including camera-off ones, uncapped (owner ruling "sources only": a
  non-source is NOT subscribed and is inaudible in Program, the hardware-switcher
  model); `meeting-audio` (the programMix path) is unchanged. **Tiles audio is
  sticky** (`TilesAudioSourceLatch`), keyed by SCENE: a Tiles member stays an audio
  source while their scene is on EITHER bus, so a panelist who turns their camera off
  (the membership policy drops them) keeps talking on air — including across the Take
  that swaps their gallery from Preview to Program. It forgets a scene on neither bus,
  a participant who leaves, and EVERYTHING on a KNOWN "not in a meeting" or on Engine
  off — a tick whose meeting state is UNKNOWN (no/synthesized snapshot) never clears it (Zoom
  reuses per-meeting user ids; a latch that outlived the meeting would make a different
  person audible). A never-on-camera participant is never a member, so never audible.
  `StudioViewModel.BuildSpinePayload` only plumbs the Tiles layers, ISO ids and the
  latch in, and maps the live roster's `VideoOn` into health (it used to pass raw
  NetworkQuality, so every camera-off guest read as video-on).
  **(2) THE SPEAKER DIRECTOR FOLLOWS ONLY SOURCES** (`ZoomActiveSpeakerDirector::setSourceFilter`,
  fed from the payload's `sourceParticipantIds`). Without this, sources-only
  DEADLOCKED speaker-following: the director only promotes a challenger with fresh
  frames, a non-source never has a subscription, and the meeting's first talker
  (vacancy fill, no freshness check) held the directed slot for the whole show. A
  non-source who talks is ignored for direction; an incumbent that stops being a
  source is released. A follow-speaker (`active-speaker` mode) route adds nobody,
  moves nobody's budget position and GRANTS NO PURPOSE (round 2, N1: round 1 gave the
  speaker the bus's 1080P purpose, which rebuilt two renderers — often on-air Tiles
  tiles — on every speaker change, i.e. #478's flashing driven by talk again). **Known
  limitation: a follow-speaker shot is 720P** (the speaker's own wall/Tiles/ISO tier)
  until an in-place resolution change is proven on a live renderer. **The core
  RENDERS a follow route as the directed speaker, FRAME-VALIDATED**
  (`RouteSourcePolicy` `directedSpeakerParticipantId`, `MediaCore::followSpeakerForRoutes`,
  pure `core/FollowSpeakerHold.h`): the directed speaker if they have a content frame
  THIS tick, else the most recent previously directed speaker who does, else NOBODY —
  the layer renders EMPTY (a transparent fill, opacity 0). Binding a frameless id
  painted the `colorFromParticipantId` slab on Program (the director keeps a departed
  incumbent 60 s; a speaker dropped from the sources mid-talk loses their video in the
  same tick), and an unbound layer paints grey. The history is scoped to one meeting
  by `ZoomEngineRuntime::speakerEpoch()` (moves on join, leave, Engine off and every
  new engine process), so a reused Zoom id from the last meeting is never bound. It
  used to take the positional fallback `videoFrames[routeIndex]` (uuid order), so a
  speaker scene showed the lowest-pid source. #480 removed that fallback for every
  mode: a route with no source id renders BLANK (transparent fill), including an
  emptied OHG box.
  **(3) RESOLUTION IS A STABLE TIER, CAPPED, NO RATCHET.**
  `native/src/modules/ZoomSubscriptionResolutionPolicy.h`: FIXED bus routes (purpose
  program/preview) and screen share at 1080P; Tiles, wall, ISO and a follow route's
  speaker at 720P. At most
  `kMaxConcurrentFullResolutionCameras` (4) cameras at 1080P, granted in payload order
  (Program routes first, then Program Tiles, then Preview routes); the rest get 720P
  and `zoomSubscriptionChurn.fullResolutionDemoted`
  counts them. The number comes from commit bd3caf29: SIX concurrent 1080P raw
  subscriptions crashed the SDK subprocess (0xc000000d), and everything since shipped
  with ONE camera at 1080P. A guest who leaves the buses DROPS BACK to 720P: the engine
  used to ignore a lower request (`video_subscribe_noop_existing`), which over a show
  ratcheted every rotated guest to 1080P; it now rebuilds when the source is the
  renderer's only target (`zoom-engine/shared/engine-resolution-policy.h`). In-place
  `setRawDataResolution` was REJECTED: the SDK header only declares it
  (`h/rawdata/rawdata_renderer_interface.h:50`) and the engine only ever calls it
  before `subscribe()` (`engine-video.cpp:70`). **The unavoidable on-air
  re-subscribe (a cue raises, leaving a bus drops — never who is talking) is HIDDEN by
  holding the last frame:**
  `ZoomEngineRuntime::latestDecodedFrames_` is erased only when a source is RETIRED,
  so across a same-uuid re-subscribe the compositor keeps drawing the last decoded
  frame. A route layer holds it until the rebuilt renderer delivers (no staleness
  gate on routes — same as any stalled feed); a Tiles tile holds it for
  `kTilesStaleFrameMs` (1500 ms) and is then dropped from the wall until frames
  return. Pinned by `ZoomEngineRuntime.AResolutionReSubscribeKeepsTheSourcesLastFrameForTheCompositor`.
  **(4) LOUD, BUT MULTIVIEW-ONLY.** A camera-on source the cap leaves out goes in the
  payload's `videoSubscriptionShortfall` + `warnings`, and its multiview tile label
  reads "<name> · no video: subscription limit (10)". A BUS source with no wall tile
  (a cued Preview guest) is named on the PGM / PVW cell instead: the multiview layout
  carries `programNotice` / `previewNotice`, the core appends it to the cell label and
  the overlay draws "PREVIEW · no video: <names> (subscription limit 10)" (the tile LABEL
  is part of `VideoSurfaceCoordinator.MultiviewLayoutSignature`, so a label-only change
  reaches the overlay; the production sync's plain layout still blinks it for up to one
  spine tick after a user action). The
  multiview is the ONLY place a tile carries text: Program/Preview/Tiles are composited
  pixels. The
  subscription UUID is still `participant-video-<pid>-camera` (purpose excluded).
  Tests: `ZoomMediaSpinePayloadBuilderTests` (`LiveCase478_*`, the follow-route,
  speaker-flip-changes-nothing, at-the-cap, Program-first, sticky and breakout cases),
  `TilesAudioSourceLatchTests` (incl. the swap Take and the reused-id rejoin),
  `MagicSceneCoordinatorTests.AutomationCuesPreviewAtTheStartOfTheHold…`,
  `MultiviewOverlayFormattingTests.ResolveLabel_BusCellCarries…`; native
  `FollowSpeakerHold.*`, `TilesRenderPlan.AFollowSpeakerRouteWhoseSpeakerHasNoFrameRendersEmptyNotASlab`,
  `LeavingTheMeetingForgetsTheFollowRoutesHeldSpeaker`,
  `MediaCoreMultiview.TheBusCellsCarryTheShellsSubscriptionLimitNotice`;
  native `ZoomEngineRuntime.AnActiveSpeakerFlipCausesNoTeardown`,
  `ACueRaisesOnceTheTakeSendsNothingAndLeavingTheBusDropsBack`,
  `TheFullResolutionCapDemotesTheRoutesPastItAndSaysSo`,
  `ANonSourceWhoTalksFirstIsReleasedAndNeverDirected`,
  `ZoomEngineRuntimeState.DirectsOnlyAmongSourcesAndReleasesANonSourceIncumbent`,
  `TilesRenderPlan.AFollowSpeakerRouteShowsTheDirectedSpeakerNotTheFirstFrame`,
  `RouteSourcePolicy.AFollowSpeakerRouteBindsTheDirectedSpeakerNeverAPositionalSource`,
  `EngineResolutionPolicy.*`.
  **Costs, by the owner's rule (expected, not defects):** an unrouted guest has NO
  frames and NO audio anywhere — the Show Input / source pickers and the scene canvas
  editor show "Waiting" for them (roster thumbnails come only from subscribed
  streams), a non-source's mixer strip reads `waiting-for-pcm` (the mixer still lists
  every participant; wiring it to `ZoomSourceSetPolicy` is a follow-up after #481),
  and a scene built only on a follow-speaker route with nobody on the wall has no
  speaker to follow. Magic Scene now cues Preview at the START of its hold so the
  target warms before the auto-Take (a direct operator cut to a non-source still
  subscribes cold).

- **The scene canvas editor cannot show GPU video — DIAGNOSED 2026-08-15, NOT FIXED
  (a redesign is being specced separately; do not patch this ad hoc).** Owner report:
  "layer boxes show live video inconsistently". `VideoSurfaceHost` attaches a
  SwapChainPanel (and hooks its per-vsync present) ONLY when the surface key is
  `program`/`preview`/`multiview` or the kind is Program/Preview
  (`VideoSurfacePresentationRules.UsesGpuSharedTexture`). `StudioViewModel.ResolveLayerSurface`
  hands each layer a surface rewritten to key `scene-layer-N:<tileKey>` + kind
  **Multiview** — matching no clause — so `OnLoaded` early-returns and the core's
  per-source keyed-mutex export (`D3D11CompositorAdapter::exportParticipantTextures`,
  built expressly for this intermittent consumer) is DROPPED. The PREVIEW monitor works
  off the SAME tile only because `ResolvePreviewPrimarySurface` rewrites it to key
  `preview`/kind Preview. Net: editor layers render CPU BGRA only, so —
  media assets: always live (own player); Zoom guests: the 640x360 thumbnail at ~2/s
  (`kThumbnailEmitIntervalMs = 500`) and nothing while capture is unsubscribed;
  managed-bridge UVC cameras: smooth; **native-UVC / screen (WGC) / browser / SRT-ingest:
  placeholder forever** (only `CaptureDeviceFrameReaderService` fills
  `CaptureDeviceSurfaces`). Do NOT "fix" it by whitelisting `scene-layer` keys — that is
  N per-layer swap chains, the retired 0xc000027b pattern, and the per-source export is
  single-consumer keyed-mutex already claimed by the preview host. Characterization
  tests: `SceneCanvasLayerSurfaceTests`.
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
  **The fake engine delivers ONE video stream per participant** (2026-09-09): it used
  to keep its `participant-video-<id>-auto` stand-in alive alongside the app's explicit
  `participant-video-<id>-camera` subscribe, so 3 participants ran 6 targets and every
  participant got 2 x `COREVIDEO_FAKE_ENGINE_FPS` interleaved into one core slot
  (`latestDecodedFrames_[participantId]`) — every fps number from the rig was
  uninterpretable. An explicit subscribe now retires the auto target, and the
  achieved-rate line states target count, participant count and per-participant fps
  next to the configured source rate, so a 6-target log can never again be read as a
  3-participant rate. Every fake-engine harness must PIN `COREVIDEO_FAKE_ENGINE_FPS`
  (`mac-show-drill.py`, `qa/collect-runtime-snapshots.mjs`, `validate-iso-record.mjs`)
  and print it — an unpinned run silently measures at the default 30.
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
  **The recorded-rate gate NAMES EVIDENCE, it does not assert a cause (2026-09-09).**
  The drill used to hard-code "encoder->submit rides the ~50Hz audio worker" on that
  failure. For Program video that is stale — the submit moved to the signalled video
  tick and the audio-worker submit is guarded by `videoOutputTickRunning_`, which
  `JsonRpcServer.cpp` sets true unconditionally in any real run — so the message was
  pointing every reader at the wrong stage. It now prints a "Recording-window stage
  rates" block on BOTH paths (compositor render slots/s, video-out tick/s, audio
  worker tick/s, encoder programVideoWritten/s, render skipped/deadline misses,
  encoder droppedVideo, program-buffer underruns/gpuNotReady, the recording proof's
  `encoderQueueDroppedVideoFrames`, and the last full `[render]` window), sampled as
  deltas from `realtimeEvidence`/`encoderEvidence` at both ends of the record window.
  A compositor rate below 60 means the machine never produced 60; a compositor at 60
  with a lower video-out/encoder/muxed rate means the loss is downstream.
  `MIN_RECORDED_FPS_RATIO` is unchanged.
- **`MonitorRenderFaultInjection.*` are TIMING MEASUREMENTS on a real GPU, not unit
  tests.** They drive the real compositor on the real 60Hz production timeline and every
  assertion is relative to an unfaulted baseline window measured moments earlier. That
  baseline is a PRECONDITION, so `measureSettledBaseline` retries up to four windows
  before giving up — but sustained contention (another build, a soak, a second test run)
  can starve every attempt, and then the test fails for machine load rather than for the
  property under test. Observed failing this way while an A/B soak had the box.
  **Run them on a quiet machine**, and exclude them with
  `corevideo-native-tests.exe --gtest_filter=-MonitorRenderFaultInjection.*` when the box
  is busy. Same posture `mac-show-drill.py` already carries: a shared or loaded machine
  cannot gate a timing property at any threshold. Do NOT "fix" a load failure by widening
  the margin — that trades a flaky test for one that asserts nothing.
- **The Wave 0 snapshot judge finally has a producer:**
  `node scripts/qa/collect-runtime-snapshots.mjs --out capture.json [--seconds N]
  [--interval-ms N] [--load N] [--recording]` runs its own core over stdio, samples
  bare `{"type":"snapshot"}` on a DECLARED interval, and writes the
  `{samples[], expectedWorkers, recordingExpected, policy}` envelope that
  `production-qualification.mjs --runtime-snapshots` consumes. Full contract and the
  two deliberate refusals (no fabricated `nativeNowMs`; never a quiet empty envelope)
  are in `docs/qualification/WAVE-0.md`.
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

## A Take is traceable, and "what Program rendered" no longer lies (2026-09-10)

Three things, all core-side, all born from the same live show. The first is a
correctness fix; the other two are instruments, deliberately built before any
further fix, because three of that night's wrong conclusions came from a
measurement rather than from the product.

- **THE RENDERED SCENE ID WAS STUCK, NOT LAGGING.** `programFrame.sceneId`
  (snapshot) sat on the pre-take scene for 15+ seconds while Program was
  demonstrably compositing the new one. Root cause, in `MediaCore::renderTick`'s
  buffered branch: it attributed the snapshot from
  `ICompositor::latestDeliveredProgramFrame`, which is a **PEEK** — the program
  buffer hands back the same delivered frame on every call until its delivery
  thread advances `latest_`, and `D3DProgramBuffer` deliberately refuses to
  advance it when the export it is paired with was busy, and CLEARS it when a
  packet expires. So "a frame came back" was never evidence Program moved, and
  when nothing came back there was **no else branch at all** — the attribution
  simply stopped being written and the old scene stood forever. The delivery
  SEQUENCE is now what says a new frame reached air, and the rest is the pure
  `core/RenderedSceneAttributionPolicy.h` (`OutputLifecyclePolicy` shape):
  Follow a new delivery with plan evidence, Hold through <=12 ticks (200ms) of
  delivery jitter, then Forget. `programFrame.sceneIdAttribution` publishes
  `live`/`holding`/`unknown` unconditionally alongside
  `sceneIdAttributionTicks` and `deliverySequence`, so the field can never again
  assert a scene nothing confirmed. **Rule: a peek is not an observation** — if a
  reader republishes the same value, key your freshness on a sequence the
  producer advances, not on the call succeeding.
- **ONE STRUCTURED RECORD PER TAKE**, per operator action and never per frame, so
  it is on by default without flooding the bounded log. Armed in `loadSceneGraph`
  when the scene id actually changes (Take is a client-side scene swap that sends
  ONE sync, so that IS the take on this wire) and completed on the first program
  render tick after it — the only place the "after" half exists. Carries scene id
  and renderPlanId on both sides, the layer ids on both sides, the wall keys,
  whether `TilesPlanAnimation::adoptSettledFrom` **adopted or reset**, whether the
  wall's live background (`tiles-source-bg:`) made the first program frame, and
  the subscription-churn delta across the take. `core/TakeRecordPolicy.h` turns
  those into the one-word answer to "did the wall rebuild or cut" — and it will
  NOT certify a clean cut when the background dropped or a subscription churned
  in the same tick, because both look identical on air. Lands as a `[take]` line
  in the bounded process log (which the support bundle already collects) and as
  a bounded 8-deep `takeRecords` node in `sessionState`. The outgoing plan is
  built once on the command thread; the render tick pays only a layer-id copy.
- **SUBSCRIPTION CHURN IS MEASURED PER SOURCE.** `ZoomEngineRuntime` keeps a
  ledger keyed by sourceUuid — a `generation` that increments on every real
  (re)subscribe or teardown, a cumulative `churn` count, and the REASON
  (`resolution-change` / `cap-eviction` / `unrouted` / `departure` / `resubscribe`),
  decided by the pure `modules/ZoomSubscriptionChurnPolicy.h`. Published unconditionally as
  `sessionState().zoomSubscriptionChurn` (engine:false with empty arrays when there is
  no engine — the multiviewer-node rule). Two things it was built to catch:
  resolution is part of the subscription key, so a raised resolution is a genuine
  engine-side renderer teardown; and a source dropped from the requested set is
  unsubscribed outright. **The ledger deliberately SURVIVES the unsubscribe** — a
  record erased with the subscription cannot answer the question it exists for —
  and is cleared only where `sentSubscriptions_` is (leave / rejoin / a new engine
  process). **The instrument did its job and the churn is now FIXED (#478,
  2026-09-11)**: on a real 12-person show it fired several times a minute
  (`totalChurn` 53->59 in 20 s). See "A SECOND, ENGINE-SIDE CONTRIBUTOR" above:
  resolution is a stable tier (an active-speaker flip sends nothing and moves no
  generation, follow-speaker route or not), and the shell subscribes
  only sources. A `resolution-change` now means a guest was cued onto or left a bus
  (both directions: there is no ratchet). Retire reasons are split so the ledger
  cannot blame the cap for ordinary events: `cap-eviction` ONLY when the shell's
  `videoSubscriptionShortfall` names that participant, `video-off` when the engine
  roster says their camera went off, else `unrouted` (an operator un-route); the
  node carries `lastCapEvictions` / `lastVideoOff` / `lastUnrouted`, plus
  `fullResolutionCap` / `fullResolutionDemoted` for the 1080P cap.
- **PER-SOURCE CONTINUITY IS PART OF THE VERDICT (slice 1 of the persistent-sources
  redesign, 2026-09-10).** The wall-only verdict above missed the owner's actual
  case: a Tiles gallery whose foreground AND media background are on both Preview
  and Program re-rendered on every cut, and the take record still said `cut`,
  because nothing was watching the SOURCES a shared scene depends on.
  `core/SourceContinuityLedger.h` observes every render tick over `videoFrames`
  (keyed by `frame.participantId`) and bumps a per-source `generation` whenever a
  frameId regresses (a decoder reopened) or a source reappears after
  `absentTicksBeforeRestart` (30 ticks, 500 ms at 60 Hz) — a cold start. Only
  frames with CONTENT count (`hasPixels() || hasI420()`): a metadata-only Zoom
  roster frame is neither observed by the ledger nor accepted as "had a frame".
  The take record now carries `fromSourceIds` (the frame keys — participantId,
  else sourceId — of the outgoing Program plan UNION the outgoing Preview plan,
  i.e. the before-set), `sources[]` ({sourceId, generationBefore, generationAfter,
  frameIdBefore, frameIdAfter, restarted}) for every source in that before-set
  that is also in the incoming plan, `restartedSources` + the boolean
  `sharedSourceRestarted`, and `missingSources` + the boolean `sourceMissing` (a
  source the take brought on air with no frame on its first program tick, judged
  only when a frame was EXPECTED — a media layer, or a source that was running
  within the ledger's own restart window when the take was armed). Any restart or
  missing source forces `verdict=rebuilt`, even when the wall itself cut cleanly.
  **A HELD frame counts as continuous.** A source slower than the render rate
  (a 30 fps guest, a still, a paused poster) re-presents the same frameId for
  several ticks; the ledger treats that as the same generation, not a stall. This
  deliberately differs from spec §4.1's "each kept advancing frameId" — requiring
  an advance on the take tick would call every slow source a rebuild. Only a
  regression or a >30-tick absence is a restart.
  **Known low-probability race (documented, not fixed):** the before-set is read
  when the Take's `load-scene-graph` arms the record. If a repeating spine sync
  applies the swapped Preview (the outgoing scene) BEFORE that `load-scene-graph`
  lands, the "outgoing Preview plan" is already the new one and the before-union
  can miss incoming sources that were on the old Preview — so a source can be
  judged missing/unshared rather than continuous. The shell sends both in one
  sync, so this needs an interleaved spine tick. Extends the existing rule "a peek is not an
  observation" one step further: **a counter that only counts submits is not
  continuity** — the ledger has to watch frameId actually advance across the
  take, not just that something arrived.

Tests: `native/tests/RenderedSceneAttributionTest.cpp` (the attribution defect
red/green, the policies, and the take record end to end),
`native/tests/SourceContinuityLedgerTest.cpp`, and
`ZoomEngineRuntime.SubscriptionChurnNamesResolutionChangesAndTeardowns`.

## Media is a persistent source (slice 1, 2026-09-10)

Slice 1 of `docs/superpowers/specs/2026-09-10-persistent-sources-design.md`: a
media asset is now one decoder with one clock, not one per bus.

- **Same source id on both buses.** `buildPreviewCompositorRenderPlan` no longer
  renames every Preview media layer to `preview:<id>` — Program and Preview
  address the SAME source: `media:<assetId>` for a routed asset or still,
  `background:<assetId>` for a scene background. So `OwnedMediaFrameSource` runs
  one decoder (and `StillMediaFrameCache` one entry) for a background or still
  shared by both scenes, and a Take cannot restart it. **The one exception:** a
  paused, non-still `media-video` layer (a clip cue poster) still gets `preview:` —
  its paused poster frame is a different playback position from Program's rolling
  copy, and the two must never replace each other in the frame set.
- **Route stills never reach a decoder.** A still on both buses arrives playing
  on Program and paused on Preview; `OwnedMediaFrameSource::requests()` skips
  `media-video` stills (`isStillImageMediaAsset`, the MF adapter's own filter),
  so they are served only by `StillMediaFrameCache` — no dead decoder threads, no
  false "two playback identities" warning. Background stills keep the decoder path.
- **The route wire carries the loop flag** (`mediaAssetLoop`, from
  `MediaRoutePlaybackService.IsLoopingAsset`), parsed at BOTH scene parse sites
  onto `SceneRouteState` and the layer — without it a looping route asset played
  once and froze. The preview-scene dedup signature includes the media path,
  playback key, playing and loop flags, so a change to any of them is applied.
- **Loop vs clip go-live policy** (shell, `MediaRoutePlaybackService` /
  `TransportCoordinator` / `StudioViewModel`). A looping background asset (kind
  "background") plays under key `media:<id>` on both buses, identical to Program,
  matching §2 of the spec ("nothing" on go-live for loops). A clip (non-loop)
  plays under `media:<id>:live:<n>` — paused on first frame while only in
  Preview, rolling from 0 with audio on only when it actually GOES LIVE. `n` is
  the `MediaGoLiveLedger` generation for that asset, which advances only when
  the asset enters Program for the first time — never on every Take, and never
  from Pause/Play on an already-live clip (pause is a clock state, below) — so a
  clip already playing on Program stays rolling across an unrelated cut. `ChooseAssetToPromote` /
  `ITransportHost.RecordProgramMediaGoLive` runs promotion only for the assets
  that actually went live, and never for a still (`SupportsPlayback` filter).
  **Operator pause is PER-ASSET state, not "is it the selection":**
  `MediaGoLiveLedger` keeps an operator-paused set — pausing a Program-routed clip
  adds it, playing it removes it, the clip GOING LIVE clears it — and
  `ShouldPlaySceneMediaRoute` / `ResolveSceneRoutePlayback` read that set (loops
  always play). Promotion only moves the selection, so a paused clip that stays
  on Program stays paused when another asset goes live. Selecting a Program clip
  in the bin reports its real state instead of pausing it.
- **PAUSE IS A CLOCK STATE, NOT A NEW DECODER (T1.2, 2026-09-10).** `playing` used
  to be part of the decoder's identity in two places — the owned source's request
  key and the MF adapter's playback identity — so promoting another asset (or a
  bin-row tap) that flipped a clip's `playing` flag opened a fresh decoder: Pause
  cut to the clip's first frame, Play restarted it from 0. **Both identities now
  EXCLUDE `playing`.** `MediaPlaybackTimeline::configure` only resets (new epoch,
  `++generation`) on an identity change; a playing→paused transition freezes
  elapsed time at its current value, and paused→playing resumes from exactly
  that value (the epoch shifts by the paused duration). A paused clip HOLDS its
  on-air frame (same `frameId`, no read) rather than showing a poster; audio
  emits nothing while paused (not even silence) and resumes at the clock
  position — `MediaAudioWindows` keeps 200 ms of decoded-ahead history so the
  windows that would otherwise become a silent hole at the pause point are
  replayed instead of dropped, and resume re-times the already-prepared frames
  by the paused duration rather than snapping them to "now". The FFmpeg fallback
  path (ProRes, which cannot pause a running process in place) is stopped on
  Pause and restarted AT THE FROZEN CLOCK POSITION on Play — never from the top
  — with frame ids kept rising; a restart that fails keeps the resume pending
  and retries at the clock position on a bounded ladder (250 ms, 500 ms, 1 s,
  2 s, give up after 5 attempts), holding the paused frame and warning every
  poll, and a fresh operator Pause re-arms a resume that gave up (the FFmpeg
  tests self-skip with a `[ SKIPPED ]` stderr line, not a silent pass, when
  `C:\ffmpeg\bin` is absent from the build machine). In
  `MediaCore::renderSyntheticTick`, Program's media layers are gathered BEFORE
  Preview's are appended — Program-first ordering is what keeps Program
  authoritative when a shared source id (same clip on both buses) arrives
  paused on Preview: the Program request always wins the one shared clock
  (`OwnedMediaFrameSource::requests()` lets the FIRST request for a key win).
  **Restart from the top happens ONLY via the go-live generation**
  (`media:<id>:live:<n>` advancing) — never from Pause, Play or a bin-row tap.
  The Preview cue poster exception above (a paused, non-still `media-video`
  layer keeps its own `preview:` key) is unchanged by this — it is a genuinely
  different playback position from Program's rolling copy, not the same clip's
  pause/resume.
  **The shell surfaces the real on-air state, not "is it the selection"**
  (`MediaRoutePlaybackService.IsPlayingOnAir` / `ResolveTap`): the media bin
  row shows a rolling Program clip as playing even when it is not the current
  selection (`ApplyMediaSelection` calls `IsMediaAssetPlaying` per row), and
  tapping that row pauses it via the ledger (`RecordPause`/`RecordPlay`) —
  `PlayMediaAsset` no longer restarts anything, and `MediaGoLiveLedger` no
  longer even HAS a `RecordRestart` method (deleted as dead code once its one
  caller was removed). **The transport toggle (`MediaPlaybackButtonLabel`,
  "Pause Program"/"Resume Program"/"Audition") still only reflects the
  SELECTED asset** — `FormatMediaPlaybackActionLabel` reads
  `SelectedMediaAssetPlaying`, not the tapped bin row's id, so it does not (yet)
  show an unselected rolling clip's state; only the bin row does. A tap on a
  looping asset (kind `background`) is always just a selection
  (`MediaTapAction.Select`) — a loop is always playing and has no useful ledger
  pause state. `TransportCoordinator.TakeAsync` (and
  `StudioViewModel.UpdateScene`) also calls
  `ITransportHost.RefreshMediaBinPlaybackIndicators` so a clip that LEAVES
  Program on a Take stops reading "playing" — `PromoteProgramMediaRouteToPlayback`
  only rebuilds the bin when something ENTERED — and it clears
  `SelectedMediaAssetPlaying` when the SELECTED clip itself is the one that
  left, so the transport toggle cannot keep reading "Pause Program" for a clip
  no longer on air. **It is called CONDITIONALLY, not on every Take** (folded
  in from a review pass): only when the Program media SET actually changed
  (`StudioViewModel.BuildProgramMediaRouteSignature` before vs after) AND
  `PromoteProgramMediaRouteToPlayback` did NOT already run (that call's own
  `ApplyMediaSelection` rebuild already gives every row — not just the
  promoted one — its current on-air state, so a second rebuild in the same
  Take would be pure waste). This is what keeps an automated Magic Scene Take
  between two non-media scenes from rebuilding `MediaBinGroups` on every cut.
  **A ROLLED-BACK TAKE RESTORES THE MEDIA SELECTION TOO (T1.3, #430).** The #286
  rollback (`CaptureTakeRollback`) only ever put the SCENES back. A failed Take
  kept the selection Promote had moved to a clip that went live. That clip was
  still marked playing and kept auditioning locally with audio, and the status
  said it was on Program. Worse, a Program clip X that LEFT on the failed Take
  had its playing flag cleared. The rollback put X back on air, still rolling,
  yet the toggle read "Resume Program", and pressing it paused X ON AIR.
  `TransportCoordinator.TakeAsync` now captures the selection
  (`ITransportHost.CaptureMediaSelection`) before and after the local mutations.
  On a SUCCESSFUL scene rollback the pure `TakeMediaSelectionRollback.Resolve`
  decides what stands: the pre-Take selection, unless the operator moved it
  while the sync was pending (their choice is kept, like the scene rollback's
  newer-edits rule); and for a clip on the restored Program, the playing flag
  and status come from the real on-air state (`IsPlayingOnAir` over the paused
  set), never from the saved flag. One more case: if the operator picked a clip
  on the ATTEMPTED Program and the rollback takes it off air, it reads "<name> left
  Program" and is not playing. `RequestTakeReconciliation` runs right after the scene
  rollback, before `RestoreMediaSelectionAfterRollback`, so a throwing restore
  cannot skip it. The restore then rebuilds the bin ONCE. A refused rollback
  restores nothing. The go-live
  ledger and the paused set are still deliberately NOT rewound. Tests:
  `TransportCoordinatorTests.Take_Rollback*` and
  `Take_RefusedRollbackLeavesTheSelectionAlone`.
  Tests: `native/tests/MediaPlaybackTimelineTest.cpp`
  (`MediaPlaybackTimeline.PauseFreezesElapsedAndResumeContinues`,
  `OwnedMediaFrameSource.PauseAndResumeKeepOneDecoder` /
  `PauseHoldsTheOnAirFrame` / `NoAudioWhilePausedAndAudioResumes`),
  `native/tests/MediaCoreCommandTest.cpp`
  (`AFailedFfmpegResumeRetriesAtTheClockPositionNeverFromTheTop`), and
  `MediaRoutePlaybackServiceTests` (`ResolveTap_*`, `IsPlayingOnAir_*`,
  `SelectedAssetLeftProgram_*`) / `TransportCoordinatorTests`
  (`Take_RefreshesTheMediaBinWhenAClipLeavesProgramWithNothingGoingLive`,
  `Take_DoesNotDoubleRefreshWhenPromoteAlreadyRebuiltTheBin`,
  `Take_DoesNotRefreshTheMediaBinWhenTheProgramMediaSetIsUnchanged`) on the
  shell side.
- **A CUED CLIP HANDS ITS WARM DECODER TO PROGRAM (T1.11 / #449, 2026-09-12).**
  The Preview cue poster (`preview:media:<id>`) and the rolling Program source
  (`media:<id>`) are two decoders, because a clip changes identity TWICE on
  go-live: the `preview:` namespace collapses, and `MediaGoLiveLedger` advances
  the generation baked into the playback key (`media:<id>:live:<n>` ->
  `:live:<n+1>`). So the arriving request matched no entry, a cold decoder
  opened, and for the ticks before its first frame `resolveLayers` painted
  `colorFromParticipantId` over PROGRAM — the placeholder flash. Now
  `OwnedMediaFrameSource::adoptCuedDecoders` RE-KEYS the cue's entry onto the
  live request instead of retiring it. `Entry` is a `shared_ptr` whose worker
  holds its own reference, so the hand-over is a map re-key: the decoder and its
  held poster never notice. **This is not an exception to the go-live contract,
  it IS the contract** — the cue poster sits paused at frame 0
  (`MediaVideoPresentation::hold` shows the first prepared frame and never
  advances), so resuming it is exactly "roll from 0, audio on".
  **The decision is pure** (`modules/MediaCueHandoff.h`, the
  `CaptureReaderStallPolicy`/`TakeRecordPolicy` shape) and every condition is
  required: same asset id AND same path (a repointed bin row holds the old
  file's pictures), same loop flag, the retiring `sourceId` is exactly
  `"preview:" + arriving.sourceId`, the arriving generation is exactly the
  retiring one +1, and — the load-bearing one — **the cue NEVER ROLLED**
  (`Entry::everPlayed`). A decoder that has played is at an arbitrary position,
  and adopting it would put a clip on air mid-roll while the take record still
  read `cut`. Two candidates for one arrival is refused loudly and cold-starts:
  never guess which cue is the predecessor.
  **Three traps, each found by a test that failed first:**
  1. **It runs on the REQUEST path (`selectVideo` / `pollMediaAudioFrames`),
     never in `manage()`.** That is the difference between one flashed frame and
     none — the request set changes on the take tick, but `manage()` is a
     separate thread on a 2 ms wait, so an adoption deferred to it lands a tick
     late. Adoption starts no thread and does no I/O, which is what makes it
     safe on the caller's path where creating a worker would not be.
  2. **The queued frames are DROPPED (`MediaVideoPresentation::dropQueued`),
     `current_` is kept.** They were scheduled against the cue's paused epoch
     and can never come due on the go-live clock, so keeping them freezes the
     clip on its poster forever. The held poster is what covers the refill.
  3. **The OWNER names the source, not the decoder.** Every decoder stamps
     `participantId` from the layer it was handed, so `selectVideo` re-stamping
     it is normally a no-op — but an adopted poster was decoded under the
     `preview:` id, and the compositor looks a media layer up by the LIVE source
     id. Without the re-stamp the hand-off delivered a frame nothing could
     match and Program painted the placeholder anyway.
  The take record now reads `missingSources=[]` for this case because the cold
  start stopped happening — **the judge was not touched, and must not be**.
  Tests: `MediaCueHandoffTest.cpp` (every refusal),
  `OwnedMediaFrameSource.ACuedClipHandsItsWarmDecoderToProgram` /
  `ACueThatAlreadyRolledIsNeverHandedOver` /
  `AnAdoptedCueRollsInsteadOfFreezingOnItsPoster` / `AnAdoptedCueTurnsItsAudioOn`,
  and `ProgramPixelContinuity.ACuedClipTakenToProgramNeverShowsThePlaceholder`
  (the end-to-end pixel proof, over a REAL `OwnedMediaFrameSource`).
  **Still cold-starts, honestly:** a clip cut to Program that was never cued in
  Preview has no warm decoder to adopt. That is step 1 of #449 (hold the
  outgoing picture until the first real frame), not done here.
- **The 16-decoder cap warning names the refused source** (`OwnedMediaFrameSource`)
  instead of just stating the count, and a loud, once-per-id `[media-playback]`
  warning fires when two different playback identities request one source id —
  now a real risk once media sources outlive buses (spec §5 risk called out
  up front).

Tests: `native/tests/MediaCoreCommandTest.cpp` (media identity across buses),
`native/tests/MediaPlaybackTimelineTest.cpp` (`OwnedMediaFrameSource.*`),
`native/tests/ProgramPixelContinuityTest.cpp` (dark 0x10 fill placed outside the
placeholder colour range — fails against the old per-bus rename),
`MediaRoutePlaybackServiceTests` (incl. the per-asset pause rules) /
`TransportCoordinatorTests` / `MediaCoreCommandBuilderTests.SerializesTheRouteLoopFlagNextToPlaying`
(shell), `MediaCoreCommand.ARouteLoopFlagReachesTheMediaSourceOnBothBuses`,
`TakeRecord.AMetadataOnlyZoomFrameDoesNotCountAsHavingAFrame`, and
`scripts/qa/take-verdict-judge.test.mjs`. Not yet run: `scripts/qa/live-meeting-soak.mjs
--takes N [--background <file>]` against a real meeting. Its Takes mirror the
shell's Take (one sync: `load-scene-graph` incoming + `set-preview-scene` outgoing),
both scenes carry the same Tiles wall over the live Zoom members (plus the same
media background with `--background`), and Program is primed to scene B before the
floor is read so N takes score N records. **Remaining limits:** cuts only (no fade
Takes); no clip or still routes, so the go-live cold start above is not exercised;
without `--background` it proves Zoom/wall continuity only; no pixel probe across
the take (the record is the only judge); and the synthesized scenes are not the
shell's own scene payloads.

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
- **Shell: stop off the UI thread.** Every core stop rides `Task.Run`. Leave-meeting
  (`SettingsViewModel`), respawn and the post-failure `ForceStopMediaCoreAsync` use
  `_bridge.Stop()`: a kill-tree + `WaitForExit(1500)` under the supervisor gate. App exit
  (`StudioViewModel.DisposeAsync`) uses `_bridge.StopForAppExit(2 s)` instead (T1.8, below):
  worst case 2 s grace + 1.5 s kill wait = 3.5 s inside the 5 s `ShutdownTimeout`.
- **App close never cuts a recording off unasked (T1.8, #461, 2026-09-10; fix round 1).** Closing
  used to kill-tree the core with no stop sent, so the Program moov atom, every ISO writer and any
  RTMP/SRT egress died mid-write (unplayable show file). Now, in order:
  1. `MainWindow.OnAppWindowClosing` hands the request to `CloseGuardFlow` (constructible, tested
     in `CloseGuardFlowTests`), which asks `CloseGuardPolicy` (pure, tested). Recording or
     streaming live — the shell flags, the core lifecycle (`stopping`/`finalizing` COUNT as live),
     or `recording.active` on a core with no lifecycle; the virtual camera alone never asks — opens
     a ContentDialog "Stop outputs and close?" with **Stop and close** / **Keep running** (the
     default). The window is restored/activated first and the dialog opens after the Closing
     callback returns. A second close while it is open, or while outputs finish, is ignored (and
     brings the window forward) — never a force-exit. **If the dialog cannot be shown (no XamlRoot,
     another ContentDialog open, anything) while outputs are live, the flow takes the Stop-and-close
     path — never the old kill path.** Only a failure of the stop itself falls through to a plain
     shutdown.
  2. **Stop and close** → `OutputShutdownCoordinator`: sends the EXISTING transport stops
     (`SetRecordingAsync(false)` / `SetStreamingAsync(false)`, not awaited — they can hold for
     30 s on a busy core) and refuses new Record/Stream starts while it runs. It then waits for
     evidence tied to THIS stop, read from the bridge's `LastSnapshot` (never its own syncs, which
     would compete with the stop for the single sync slot):
     **freshness** — only a snapshot whose `RawReceivedUtc` is later than the last moment the stop
     was still pending (intent set or a Record/Stream command in flight — a stop sent while a start
     is in flight is silently ignored by the transport, so it is re-sent once a second until it
     lands); **session binding** — the `OutputStopBaseline` captured at the close request names the
     recording lifecycle session and the senders that were live, and only THEIR terminal states
     count (a stale pre-stop `completed`, another session's state, an old failure, or an ABSENT
     node never read as finished); **core generation** — the supervisor restart count; a change means
     a respawned core answered, so the wait ends at once as `CoreRestarted` ("the recording was
     interrupted with the old core and nothing more can be saved"), and a core that is gone ends it
     as `CoreUnavailable`. The generation baseline is re-armed when the stop is actually SENT: a
     core that respawned while the dialog was open is logged as having lost the old files, and
     whatever the NEW core is doing is still stopped and waited for (fix round 2). Bounded at
     15 s; on timeout it logs `shutdown: outputs did not finish within 15s — closing anyway` and
     proceeds. Only THEN does the unchanged `ShutdownAsync` run, so the 15 s is outside the
     5 s / 6 s watchdog budget.
     **STREAMS ARE STOPPED BEFORE RECORDING, and the order is load-bearing** (fix round 2). Each
     transport stop builds its sync payload inline from the current flags. A recording stop sent
     while Streaming is still desired carries `start-program-output{rtmp…}`, which the core
     treats as unowned (`recordingStatus_` is "stopping") and answers with `encoder->start`: the
     sink generation bumps, the recording lifecycle is ERASED, and the finalized file never
     reports `completed` — so record+stream always ran to the 15 s bound. Pinned by
     `OutputShutdownCoordinatorTests.StreamsAreStoppedBeforeRecording…` and
     `RecordAndStreamTogetherFinishesInsteadOfTimingOut` (both fail with the order swapped). The
     core defect itself (a non-recording Start erasing a finalizing recording's lifecycle — it
     also leaves `recording.status` "stopping" whenever an operator stops Record while Stream
     stays up) is filed separately and NOT fixed here. Two more evidence rules: `interrupted` is
     NOT terminal (the core's `isTerminal` is completed|failed; a stalled writer is still open),
     and a sender only counts as finished when its ADAPTER reads `stopped`/`failed` — the core
     reports `idle`, or re-serves an older run's terminal state, while the adapter is still live.
  3. **App-exit core stop only** (`StudioViewModel.DisposeAsync` → `MediaCoreBridgeService.StopForAppExit`):
     snapshot the core's descendants (`ProcessTreeSnapshot`, pid + start time), close stdin — the
     core's quit signal: JsonRpcServer's reader hits EOF, the loop breaks, all workers join, `main`
     returns and MediaCore's destructors run — wait up to `ShutdownBudget.CoreExitGrace` (2 s) for
     it to exit, THEN the old kill-tree. While it exits the supervisor keeps DRAINING the retired
     core's stdout (`_drainOnlyProcess`), because the core's writer thread flushes before it can
     join — stop reading and a full pipe wedges the exit (`MediaCoreAppExitStopTests` fails with
     the drain removed). After a clean exit it waits ≤250 ms for stderr EOF (the core's last log
     lines) and kills any recorded descendant still alive (the tree kill can no longer reach them;
     that test fails with the sweep removed). A plain `Stop()` that lands while a grace is still
     running (the shutdown-timeout fallback) kills the retiring core instead of orphaning it.
     `ShutdownBudgetTests` pins the budget; the disposal after the core stop is logged with its
     duration. Leave-meeting, respawn and the post-failure `ForceStopMediaCoreAsync` keep the
     immediate kill.
  **Remaining gaps:** the Zoom engine still does not get the Leave → `stop_raw_media` order on app
  close — `~ZoomEngineRuntime` terminates it (same as the kill-tree did). And NONE of this has run
  against the real app yet: the pre-merge live checklist is in the T1.8 fix-round-1 report (idle
  close, Keep running via mouse/Esc/Enter, Stop and close + ffprobe of Program and every ISO,
  repeated closes, minimized close, a stream stop, zero new WinUI dumps).
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

## OHG show engine host (Plan 7a, 2026-09-07)

The OHG show engine (`show-engine/`, TypeScript) runs **as a fourth, optional child process** and
drives the shell over the existing control surface. It is not media — it is show *direction*
(seating, looks, hands queue, gallery) that issues commands the shell applies.

**What runs where.** `CoreVideoPro.ShowEngine.ShowEngineSupervisor` spawns
`node show-engine/dist/host/main.js --config <path> --generation <n>` and speaks JSON lines over
stdin/stdout (the same shape as the core/engine pipes). `ShowEngineBridge` implements
`IControlActionProvider`, so the engine's 28 `ohg.*` actions are **merged into `ControlCatalog`**
at runtime — `ControlActionRegistry` stays a closed compile-time list and is NOT made mutable; the
catalog composes static + provider. State comes back two ways: the raw `ShowSnapshot` under
`ControlState.Ohg` (`/state` → `ohg`, camelCase straight from TS, never mirrored into a C# type)
and a **flattened** `ControlState.OhgFields` map (`ohg/slot/3/tally` → one scalar) that OSC and the
Companion module read without walking the nested object. `ohg.*` actions are **loopback-only over
OSC** by default (`OscExposure.LoopbackOnly`) — a LAN sender is refused with a message naming
`COREVIDEO_OSC_OHG_LAN=1`, which is the only way to open them up. OSC carries no auth token, so
opening them puts on-air actions on the LAN unauthenticated; that is the whole reason for the gate.

**Configure it:** `%LOCALAPPDATA%\CoreVideoPro\ohg-show-config.json` (`ShowConfigStore`). `engine`
is opaque to the shell (validated engine-side); `shell` is ours:
- `driveHost` (**default false = shadow mode**) — host commands are logged to
  `ohg/shadow/lastCommand` and the log, and NEVER applied. Ship a new show config in shadow first.
- `presets` — the four fixed scene ids the engine cues by name: `solo`, `activeSpeaker`, `black`,
  `gallery`. Unconfigured = null = refused **at use time**, loudly, not at load.
- Route ids are naming, not config: `ohg-box-<n>` per look box, plus `ohg-host` / `ohg-reader`
  for the two chairs (a chair route is only written when that chair is seated).
- `engine.capacity` **must be 10** and must equal the host capacity — a mismatch is a loud config
  refusal, not a clamp. A config with **no `version`** is refused as unsupported (never assumed v1).
- `tallyUrl` is parsed and reserved; nothing posts to it in 7a.

**Edit it in the app (Plan 7b Task 10):** Settings -> **OHG show** edits the same
`ShowConfigStore` document (integrations, Mukana polling, looks, the four preset scenes,
`driveHost`, default transition, tally URL). **Save** validates against the app's CURRENT
scene ids, writes the effective config, hot-swaps the `OhgHostAdapter`
(`StudioControlSurface.ReplaceOhgAdapter` -> `OhgAdapterSlot`), rebuilds `StudioViewModel.OhgShow`,
then restarts the engine — in that ORDER, which is pinned by the pure
`OhgConfigApplySteps.Order(engineRunning)` (restarting before materializing boots the engine on
the PREVIOUS config; swapping the adapter after the restart cues the wrong scene on air). With no
engine this launch it validates + writes only and the page says to restart the app. **The adapter
is read at APPLY time, never at enqueue time** — a host command queued behind an awaiting take
must use the adapter that is current when it RUNS, because the engine has by then been restarted
onto the new config. `OhgConfigEditModel` is deliberately NOT observable, so the section binds
`[ObservableProperty]` mirrors on `OhgSettingsViewModel` that write through to it; the intervals
and per-look box count are `double` because `NumberBox.Value` is a double and x:Bind will not
narrow it back. Every ComboBox in the section is filled and selected in guarded code-behind (never
x:Bind selection), and every interactive element is named `OHG settings ...`.

**OHG Show tab (Plan 7b).** Open it from the Produce nav group — the **"OHG Show"** button
(nav key `ohgshow`, `StudioTab.OhgShow`) — for the status strip, panelist board, program, gallery
and GFX/data panels. Four rules govern anything you change there:

- **The panels are THIN RENDERERS.** `OhgShowViewModel` ingests the engine snapshot on the UI
  thread and projects it (`OhgSnapshotView`, pure); rows are **diff-updated in place** via
  `ObservableCollectionSync` and scalars are `[ObservableProperty]`. So a `PropertyChanged` storm
  while this tab is up is a **snapshot-rate bug** (the engine publishing too often, or a projection
  that mints new values from equal input) — **not** a UI bug. Fix the rate or the projection; never
  add a UI-side throttle on top.
- **Every button is an `ohg.*` invoke through `IOhgActionInvoker`.** The page and its view models
  reach the engine ONLY through that seam (typed `OhgActionArgs` builders) — no direct bridge or
  supervisor calls, which is what makes every control testable without a running child process.
- **0xc000027b discipline, deliberately:** diff-updated rows (never a bound collection replaced at
  snapshot rate), every code-behind handler — **including DependencyProperty callbacks** — wrapped
  in `Guarded(...)` so a throwing callback logs instead of fail-fasting the process, and ComboBox
  selection applied in guarded code-behind rather than x:Bind. There is **no exception**: both
  `OhgShowPageContentTests.Page_GuardsEveryUiCallback` and its settings-window twin now read each
  handler's BODY and fail unless it routes through `Guarded(...)` (naming the handler was not
  enough — `OnRoleComboLoaded` was hooked from the XAML and delegated to a method with its own
  try/catch, which is a different guarantee and invisible at the handler).
- **The page's `ViewModel` DependencyProperty is assigned AFTER construction, so `Mode=OneTime` is
  forbidden on any `ViewModel.` path — commands included.** A OneTime binding evaluates against a
  null root and never re-evaluates: the "Set up OHG" button was inert and the Gallery note empty.
  Pinned by `Page_NeverBindsALateBoundViewModelPathOneTime`.
- **A settings picker may only offer values the ENGINE accepts.** `optionalPlateTone` and its
  siblings (`show-engine/src/config.ts`) THROW on anything outside their enum, and a rejected
  config is exit 78 — terminal, no respawn. The plate-tone picker shipped `warm`/`cool`, which no
  engine build has ever accepted. The one copy of the three enums is `OhgLookChoices`
  (`Services/OhgConfigEditModel.cs`); the pickers and `OhgSettingsViewModel.Validate()` both read
  it, and `OhgSettingsChoicesTests` pins it against `contracts.ts`. Validate also requires a
  non-blank look `label` — `ToConfig()` would otherwise emit `label:""`, which `requireString`
  refuses at parse time.
- **The seat button is one gesture with two meanings.** It selects the seat AND runs the assign
  command. A successful assign therefore CLEARS `SelectedParticipantId`, and a tap with nothing
  selected only selects the seat (silently) — without that, tapping a second seat merely to look at
  it fired `ohg.panelist.replace` on air with the previously selected guest.
- **Saving the OHG config hot-restarts the engine and REBUILDS the page view model** (the order is
  pinned by `OhgConfigApplySteps.Order`, above). The one exception is first-time setup with no
  engine running this launch: that writes the config and asks for an **app restart**.
- **The importer is HONEST by contract** (Settings -> OHG show -> import from the legacy Isadora
  files): it reports `Found`/`NotFound` per probe in plain words, **never guesses** a value it
  could not read, and **never changes capacity** (a legacy `videoPins` that disagrees is reported,
  not applied).

The **first-launch checklist** the owner runs on first open lives in
`docs/superpowers/plans/2026-09-07-show-engine-winui-workspace-outcomes.md`.

**Env vars:** `COREVIDEO_NODE_EXE` + `COREVIDEO_SHOW_ENGINE_DIR` (BOTH or neither — one alone is
ignored) select a dev/override host; otherwise `<app>\node\node.exe` + `<app>\show-engine\` (packaged,
staged by `scripts/sync-node-runtime-to-app.ps1`), then `node` on PATH + `<repo>\show-engine\` (dev).
`COREVIDEO_OSC_OHG_LAN=1` exposes `ohg.*` to LAN OSC. Node **>= 24** is required.

**Exit codes** (the host owns them; the supervisor reads them): `64` usage (bad argv), `78` config
rejected — **terminal, no backoff, no respawn**, because a bad config will be bad again — `70`
anything else (restartable). Restarts escalate 1→2→4→8→16 s, then Failed after the 5th consecutive
failure (the 30 s rung in `ShowEngineRestartPolicy.Delays` is reachable only with a raised budget);
a 60 s healthy run resets the budget. `--conformance` runs the exported host conformance
suite in-process and exits 0 iff every case passed (this is what the xUnit integration test drives).

**Logs:** `%LOCALAPPDATA%\CoreVideoPro\show-engine.log` (the engine's own `log` events, and the
supervisor's) and `launch.log` `ohg:` lines for startup/resolution/teardown (`ohg: show engine
starting (dev|packaged|env) node=… entry=… driveHost=…`).

**Test it:** `npm run typecheck:show-engine`, `npm run test:show-engine` (vitest),
`npm run smoke:show-engine-host` (spawns the real host, asserts handshake + 28 actions),
`dotnet test native-shell/CoreVideoPro.ShowEngine.Tests`, and — the one that actually proves the
seam — `AdapterConformanceTests` in `CoreVideoPro.WinUI.Tests`, which **spawns node** and drives
every conformance case through the real `OhgHostAdapter` to a golden facade sequence. (It lives in
WinUI.Tests, not ShowEngine.Tests, because ShowEngine cannot reference WinUI.) Operator drill:
`node scripts/validate-show-engine.mjs --base http://127.0.0.1:8011` against a running app;
its judgement logic is unit-tested offline by `npm run test:show-engine-drill-judge`.

**Three gotchas learned the hard way:**

- **Every supervisor event is guarded by generation, and a dead child is drained to EOF.** A
  respawn means responses, snapshots and host commands from the OLD child can still be in flight;
  each is checked against the current child (by reference, not just a generation number — a
  number-only guard reds nothing when the child object is swapped) and dropped. The exception is
  **log** lines: the dying child's last words are the whole reason we drain its stdout to EOF, so
  they are **tagged** `[gen N, exited]` rather than dropped. Logs are inert text; state is not.
- **A route to an unassigned slot MUST clear `ParticipantId`.** Writing only the slot number left
  the previous guest's participant id on the route — so cueing a look with an empty box put the
  PREVIOUS guest on air. `OhgRouteSlotWriter` clears participant/role/spotlight on every route it
  writes; the test is the contract.
- **An empty route renders BLANK (#480).** `RouteSourcePolicy` never inherits
  `videoFrames[routeIndex]`. A `fixed`/`none` route with no participant/capture/media
  id (and a follow-speaker route with nobody directed) binds nothing; the compositor
  paints a transparent fill instead of the default grey or a random guest. The shell
  may only change a scene layer's source on an operator gesture — a ComboBox list
  refresh that lands on the blank placeholder is ignored (`LayerSourceSelectionPolicy`,
  log `by=operator` / `by=refresh-ignored`).

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

**THE POLL CAN DIE SILENTLY IF START/STOP RACE — they are atomic now (2026-09-10, live on
beta-2026-09-10-5a24225).** Owner: "Audio meters aren't showing live data… If I slide a channel it
updates." `/snapshot` aged to 130 s+ while the core rendered at 58 fps. Evidence chain, all from
the running shell with no input touched: `dotnet-trace` showed no poll work at all (only the spine
sync and command replies), a 10 s exception trace showed ZERO exceptions (so polls were not failing,
they were not happening), and a `dotnet-dump` + `dumpasync` showed NO pending poll. Reading the
bridge object out of the dump settled it: the one live poll timer was armed with generation **17**
while `SingleFlightTimerWork` was at **20**, so every tick returned at the generation check. Cause:
`EnsureMediaCoreRunningAsync` is built to be called by several startup edits at once, each reaches
`StartAsync` → `StartPolling`, and start/stop were three unsynchronised steps (reset, dispose,
assign) — a stop reset the generation and disposed the newest timer, then an older start still in
flight installed ITS timer. Only operator commands (a fader, a source pick) carried fresh state
after that, for the whole session. T1.5's launch retries made the interleaving far more likely.
`StartPolling`/`StopPolling` now run under one leaf lock (`_pollTimerGate`), so the installed timer
always carries the current generation. Test: `MediaCoreBridgePollTimerRaceTests` — 8 threads x
1000 rounds of concurrent start/stop; it fails 3/3 runs with the lock removed. **Diagnostic
lesson:** when a periodic loop "stops", check its generation guard against its timer in a dump
before theorising — a no-op tick leaves no trace in logs, CPU samples, or exceptions.

**Engine off does NOT stop the shell polling the core (T1.5, #432).** The
bridge's 250 ms poll (`MediaCoreBridgeService.PollLoopAsync`) requests the core
snapshot on EVERY tick, with Engine on or off. While capture is off in a meeting
it ALSO refreshes the Zoom roster, because the spine sync that normally carries
the roster is not running. It used to do only the roster refresh there, and
`ZoomCaptureSnapshotMerger` carried the old core fields forward. So after a join
with Engine off, `/snapshot` aged and `nativeProgramFrameCount` froze, and so did
everything bound to `LastSnapshot` (meters, output health, program buffer). In one
session that lasted 67 minutes. Operators read it as a core wedge, but Program
had rendered at 60 Hz the whole time. The decision is `MediaCorePollPolicy`: core
first, then roster, each best-effort on its own. An empty `media-core-sync` runs
no tick, but it is not free: the core takes `coreMutex` and builds
`sessionState()`, and the shell's single sync slot is held for the round trip.
This is the same per-poll cost as with Engine on. Test: `MediaCoreBridgePollTests`
(a node fake core; it asserts the poll cadence and a fresh `RawReceivedUtc` with
Engine off).
**Consequence, and a rule: A SKIPPED SINGLE SEND MUST RE-ARM ITSELF.** With the
poll now running while Engine is off, a single-send sync can collide with it and
be refused with `MediaCoreSyncInFlightException`, meaning it was NOT delivered.
Three paths used to swallow that because "the periodic sync reapplies". With
Engine off nothing does: there is no spine sync and the poll is empty. A
Preview-scene pick could be lost, and the operator could then Take a scene the
core never composited in Preview. The three paths now re-arm: the Preview-scene
sync (`QueueProductionSyncRetry`), the multiview layout (its own debounce), and
the Engine-On production sync. The spine carries only the Preview scene, so the
Program sync is not repeated either. They use `SingleSendBackpressure.RunAsync`
(`SingleSendBackpressureTests`, plus
`TransportCoordinatorTests.ToggleEngine_ASyncSkippedForBackpressureIsReArmedNotAssumed`).
Never swallow `MediaCoreSyncInFlightException` on the assumption that someone
else will resend. Same report, second half:
a launch sync that collided with that poll used to leave EngineStatus reading
"Media core unavailable - media-core sync in flight; skipped for backpressure"
until Engine On. `MediaCoreLaunchStatusPolicy` now treats a skipped launch sync
as backpressure: the core is reported ready and the retry worker delivers the
sync. Only a real failure reads "unavailable" (`MediaCoreLaunchStatusPolicyTests`).

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
  holding the SRT port. **The OUTPUT senders do the same, in their OWN job**
  (`outputJobObject`/`adoptOutputChild` in `modules/RtmpOutputSenderAdapter.cpp`,
  shared by RTMP and SRT egress plus the encoder-availability probe): an orphaned
  EGRESS ffmpeg keeps PUBLISHING to a live destination after the app is gone, which
  is worse than a held port. Separate jobs because ingest's is a file-local static in
  another TU, and because outputs may later need group-killing without touching
  ingest. Assignment is BEST EFFORT — a failure logs loudly and the stream still
  starts. The job never kills anything on its own (the handle is a leaked
  process-lifetime static), so `stopFfmpegProcess()` restarts are unaffected; the
  replacement child is simply assigned to the same job. **Any new long-lived child
  process spawned by the core must be adopted into a KILL_ON_JOB_CLOSE job.**
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

## Encoder capacity is PROBED, and the software spill is LOUD (beta slice, 2026-09-09)

ISO encoder placement used to be planned against a hard-coded literal —
`hardwareSessionLimit = 8, reserved = 1, hardware = true, software = true` — passed
straight into `planIsoEncoders`. Every machine was told it had eight hardware encode
sessions, and an over-subscribed one **spilled to the CPU software MFT in silence**:
the only record was a `fallbackReason` in a manifest nobody opens. Beta testers have
GPUs we have never seen, so that is exactly the unwitnessed failure this slice exists
to remove.

- **`modules/EncoderCapacityProbe`** replaces the literal. Per `(codec, width, height,
  fps)` it names the DXGI adapter (description/vendor/device/LUID — a support bundle
  now says which GPU), finds the hardware encoder MFT, and counts how many independent
  sessions it can CREATE at that exact size and rate, taking each one to
  `MFT_MESSAGE_NOTIFY_BEGIN_STREAMING` (where NVENC's driver-side limit is actually
  enforced), plus whether an OS software H.264 MFT exists.
- **The number is a CEILING, and says so** (`ceilingIsCreationProofOnly`).
  `production-realtime-architecture.md:136` is explicit that hardware-session creation
  is not proof of sustainable capacity, and this probe establishes creation and nothing
  more. Never promote it to a guarantee without measuring sustained throughput.
- **Two traps found on the rig, both now fixed in the probe, both would have produced a
  confidently wrong answer:** a hardware encoder MFT is an ASYNC MFT and refuses
  `SetInputType` with `MF_E_TRANSFORM_ASYNC_LOCKED` (0xC00D6D77) until
  `MF_TRANSFORM_ASYNC_UNLOCK` is set — without it an RTX 4090 reported "no hardware
  encoder"; and two probes running concurrently (the sink's default-profile prewarm and
  the `configureRecording` prewarm) cannibalise each other's sessions, so probes are
  **serialised process-wide**. Relatedly, "an MFT exists but not one session could be
  created" is reported as an INCONCLUSIVE probe, never as "no hardware" — that shape is
  contention far more often than incapability.
- **Never on a hot path.** `lookup()` is a leaf-mutex map read and returns immediately;
  a miss reports `pending` and kicks a detached background probe (the `startPluginHostScan`
  / `StillMediaFrameCache` law). Prewarm happens at sink construction (default profile)
  and at `configureRecording` (the real one), rate-limited to once a minute per workload
  and **suppressed entirely while a recording is live** — the probe transiently occupies
  encoder sessions and must never compete with a show. Cached per workload; the whole
  cache is dropped when the DXGI adapter LUID changes (eGPU, driver reinstall,
  switchable graphics). The singleton is deliberately leaked so a detached probe cannot
  publish into a destroyed object at process exit.
- **`modules/IsoEncoderAdmission` decides admit / warn / refuse** and is pure and
  unit-tested (`IsoEncoderAdmissionTest.cpp`), in the `CaptureReaderStallPolicy` /
  `DeviceLossPolicy` shape. Tracks with nowhere to go, or a spill bigger than the
  machine's software budget, **refuse ISO before the show** — program still records,
  same priority-1 treatment as the unwritable-folder refusal — with an ACTIONABLE
  message naming how many ISO sources this machine is good for. A spill within budget
  arms but rides `recording.warning`.
- **THE TESTER RULE: we never refuse a show on an assumption.** If the probe is pending,
  failed, disabled or unavailable, the capacity falls back to `assumedIsoEncoderCapacity`
  — byte-for-byte the old literal — and the verdict may warn but may NOT refuse. Nobody's
  show gets blocked because we could not read their driver. `COREVIDEO_ENCODER_PROBE=0`
  turns probing off entirely; `COREVIDEO_ENCODER_PROBE_MAX_SESSIONS` raises the count cap
  (default 8).
- **Diagnosability:** the probe summary and the admission code go into the session
  `manifest.json` (`encoderCapacity`, `isoAdmission`) and to `[encoder-probe]` /
  `[recording] iso-admission` log lines.
- **Measured here (RTX 4090, 28 logical CPUs):** `hw=yes sessions<=8 (creation-proof
  only) (probe cap reached; true ceiling may be higher) mft="NVIDIA H.264 Encoder MFT"
  sw=yes`, ~0.5-1.0 s per workload on a background thread. **One machine proves the probe
  RUNS, not that it is correct everywhere** — nothing here has been seen on an Intel or
  AMD integrated GPU, and no over-subscribed machine has been observed refusing a real
  show (the refusal is covered by unit tests only).
- **Tests that arm a real recording must pin the capacity** with
  `corevideo::testing::ForcedEncoderCapacity` (`tests/EncoderCapacityProbeTestSupport.h`)
  — otherwise they race an asynchronous, GPU-dependent probe.

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

## ISO video is ARRIVAL-DRIVEN, and its loss counters are SPLIT (2026-09-09)

Three linked corrections to the ISO video path. Read the Program video-tick section
above first — this is the same lesson, applied where it had not been carried across.

- **ISO submission is signalled by ISO FRAME ARRIVAL, never by Program cadence.**
  ISO used to be submitted inside `renderVideoOutputTick` gated on `programSubmitted`,
  which made a Program-paced ~60Hz sampler read the render thread's independently
  published ~60Hz ISO set. Two free-running 60Hz clocks beat: 15-22% of submissions
  were rejected as duplicate `(sourceId, frameId)` by `AsyncEncoderSink`, and the
  result was **NON-MONOTONIC** — a faster source wrote FEWER stem frames. The render
  gather now APPENDS each newly-seen `(sourceId, frameId)` to an accumulating queue
  (`pendingIsoVideoQueue_`, per-source dedup, per-source pending cap
  `kMaxPendingIsoFramesPerSource = 4`) and bumps `isoVideoPublishSeq_`;
  `MediaCore::renderIsoVideoTick` — its own `isoVideoThread` in `JsonRpcServer` —
  waits on that signal and DRAINS EVERYTHING pending. **It does not sample, it
  drains**: a late tick costs latency, never frames. Measured with the fake engine
  (`validate-iso-record.mjs --source-fps N`, 20s, 2 ISO sources): 30 -> 29.8fps both
  before and after; 60 -> **58.1-58.8 before, 59.3-59.5 after**; 120 -> **56.8-58.3
  before, 59.4-59.5 after**. Rules it keeps: `isoVideoQueueMutex_` is a LEAF (taken
  under `coreMutex` for shared_ptr ref copies only — no pixel work, no I/O) and never
  reaches back for `coreMutex`/`audioOutputMutex_`; the worker touches neither; and
  the async sink's writer already gives Program items weighted priority over ISO, so
  an ISO burst cannot displace Program work. **ISO frames are stamped at GATHER, not
  at submit** — stamping at submit collapses a whole drain onto one instant, which
  `RecordingPtsClock` then de-collides into a 100ns clump.
  Remaining cap, honestly: the render gather still samples each source
  latest-per-tick, so a source above the render rate is capped at ~60 distinct ISO
  frames/s (monotonic, but not 1:1). Making that lossless means changing
  `ZoomEngineRuntime`'s per-participant latest-frame slot, not this path.
- **A one-line change with teeth: the sink's per-source ISO coalesce now fires ONLY
  at the cap.** It used to erase a source's older pending item unconditionally, which
  is a silent fidelity ceiling the moment a producer legitimately hands the sink two
  distinct frames for one source in quick succession — exactly what an arrival-driven
  drain does when it catches up. Its stated purpose (stop a fast participant evicting
  every slower guest when the GLOBAL cap bites) is preserved by gating it on that cap.
- **Video startup drops are counted apart from steady-state loss** — the concept audio
  has had since `recordingStartupDroppedAudioPackets`. The recording writer's Media
  Foundation open is SYNCHRONOUS and applies as a FIFO item on the writer thread
  (95-250ms), while the producer keeps submitting at 60Hz because `recording.status`
  already reads "recording". 7-12 frames are shed there. **No frame is missing from
  the file** — the head of the show is clipped — but they landed in the same
  `droppedVideo` the Wave 0 judge is fail-closed on, so a clean run reported `failed`.
  `startupDroppedVideo` (evidence) / `recordingStartupDroppedVideoFrames` (recording
  proof) now carry them, and **the window ends at the writer's first committed video
  frame (or failure), NOT when Start was applied** — the first WriteSample calls into
  a freshly opened MF sink are slow too, and closing the window at Start left ~7 of 13
  drops still poisoning the steady-state counter (measured: judge still `failed`;
  after: `droppedVideo` flat 0 for the whole run, judge clean). NOTHING IS HIDDEN —
  `runtime-snapshot-qualification.mjs` tracks it as a non-loss counter plus an
  observation, `validate-recording-finalization.mjs` reports it, and no threshold in
  either judge was weakened. Known remaining: each ISO writer performs its OWN lazy
  synchronous open at its first frame, and the items shed there still land in
  `droppedVideo` (bounded, one-time, before the first sample, so the delta-based judge
  does not trip on it).
- **ISO fidelity is measurable now.** `framesWritten` on an ISO stream is an APPEND
  count and cannot tell a distinct picture from a repeat — which made any change to the
  ISO cadence unverifiable. `encoderEvidence.isoVideoBySource` carries the whole chain
  per source: `queued` / `heldFrameSuppressed` / `queueOverflowed` (arrival side, from
  the render gather's queue) and `submitted` / `duplicateRejected` / `dropped` /
  `written` (sink side). Live at 60fps after the fix: `queued == submitted` exactly,
  `heldFrameSuppressed = 1`, `duplicateRejected = 0` — i.e. the sink-side dedup that
  was rejecting 15-22% of submissions now rejects nothing, because the repeats are
  suppressed where they are actually observed.

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
  added VstInsertStates.) v9 (2026-08-10) persists the Zoom→program audio topology
  (ZoomAudioMode: "programMix"/"perGuestIso"); absent = programMix, and an
  unrecognized value falls back to programMix rather than guessing ISO.

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
