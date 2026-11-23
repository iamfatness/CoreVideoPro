# macOS audio parity, Phase 4 (A7 processing workspace, A8 system audio, A9 guest lip sync): implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the Mac Audio tab the Windows 1.0 processing workspace (ordered channel insert chains, bus, aux and master chains, true response curves, the Windows SHOW and SETUP layouts), system audio through a Core Audio process tap, and guest lip sync.

**Architecture:** A7 and A9 are shell-only. They send existing core commands (`sync-participant-audio-mix`, `sync-audio-routing-matrix` with `busPluginInserts`, `set-zoom-guest-av-sync-offset`) through pure Swift policies, and every new piece of state rides the full-state commands that `reassertAudioConsole()` already re-sends on each core generation. A8 starts with a two-day spike on the signed bundle. A native process-tap source then goes behind a platform seam in `CoreAudioCaptureSourceAdapter`; the tap is built off `coreMutex` and rebuilt on device, sample-rate and process changes. The shell adds a "System audio" entry to the existing Local audio source card.

**Tech stack:** SwiftUI shell (`mac-shell`, harness `COREVIDEO_SHELL_TESTS=1`), C++20 and Objective-C++ core (`native/`, GoogleTest `corevideo-native-tests`), CoreAudio (`AudioHardwareCreateProcessTap`, private aggregate device, AUHAL).

**Spec:** `docs/superpowers/specs/2026-11-15-mac-audio-parity-design.md` §2.6, §2.7, §4.3 (system audio), §4.7, §4.8, §6, §7. Master plan rows A7, A8, A9 (`2026-11-15-mac-parity-master-plan.md` Phase 4). Rules: `docs/superpowers/plan-authoring-rules.md` (mandatory; every test names the mutation that must turn it red).

**Base:** `origin/main` at `e117a8fa`. Branch `mac-audio-phase4`. Path shorthand used below: `M/` = `mac-shell/Sources/CoreVideoProShell/`, `W/` = `native-shell/CoreVideoPro.WinUI/`, `N/` = `native/src/`, `SVM` = `W/ViewModels/StudioViewModel.cs`, `XAML` = `W/Views/AudioPage.xaml`.

---

## 0. What has already landed (do not redo)

Verified in `git log origin/main` and the outcomes docs:

| Item | Where | State |
|---|---|---|
| A1 Zoom audio mode, doubling guard, METER ONLY | `c6e8911a`, `M/AudioConsolePolicy.swift:79-88,196-223` | done |
| A2/A3 21-field rack, presets, A/B, persistence | Phase 2 outcomes `a1932369`..`f096adde` | done |
| A4 guide meters, stereo rail, GR meter, ZOOM MUTED | `1bb8e8b8`, `d306435c` | done |
| A5 local audio source (input device) | `7b7e93c9`, review fix `a9da9eb8` (sent on first connect) | done |
| A6 15 buses, custom buses, bus sends, LISTEN | `bd33a3a9`, `c0f9c950` | done |
| **A9 capture audio-sync nudge** | Z2 `33da4c4a`, `cd8b64fd`; `M/CaptureAudioSync.swift` | **done**: ±20 ms, clamp ±500, camera rows on Sources (Windows `SourcesInputsPage.xaml:121-134`), session-only (owner confirmed), both core copies, snapshot reconcile |

**What is left of A9: only guest lip sync.** No `avSync`/`GuestAvSync` symbol exists anywhere in `M/`. The spec's §4.8 text for the nudge (pairing row, persisted) is superseded by Z2, which matches Windows.

## 1. Facts this plan rests on (each read in code on 2026-11-22)

**Core (shared, unchanged by A7/A9):**
- Channel inserts run **in list order**: `applyChannelInsertChain` loops `inserts` in order (`N/modules/AudioDsp.h:1586-1700`). Gate, high-pass, 8-band EQ (`band1Db..band8Db`, `highpassHz`), compressor (`thresholdDb, ratio, attackMs, releaseMs, kneeDb, makeupDb`) and limiter (`ceilingDb` -12..-0.1, default -1) all read `insertSettings[name]`. This settles the spec's §4.7 UNVERIFIED item: order is honoured.
- Bus inserts also run in list order (`applyOrderedBusInserts`, `N/core/MediaCore.cpp:7201-7213`; master's chain runs before mastering at `:7246`). **But `applyBusInsertChain` (`AudioDsp.h:1319-1342`) processes only `limiter` (fixed -1 dBFS) and `compressor` (fixed -18 dBFS, 4:1, a sample-wise curve with no settings). EQ and gate on a bus pass audio through unchanged.** This is true on Windows too.
- The bus chain is taken from any send to that bus that carries a non-empty `busPluginInserts` (`MediaCore.cpp:7195-7200`). The snapshot echoes it: `audioRoutingMatrix.sends[].busPluginInserts` and `audioRoutingMatrix.busProcessing[] {busId, pluginInserts}` (`N/core/CoreStateCapture.cpp:1653-1690`).
- Guest lip sync: `set-zoom-guest-av-sync-offset {payload:{participantId, offsetMs}}` rejects an empty id, `zoom-mix`, or an id containing `:`, and clamps the offset to ±200 (`N/rpc/JsonRpcServer.cpp:264-276`). The reply is `{type:"zoom-guest-av-sync-offset", setting:{participantId, offsetMs, speakerEpoch, revision}}` (`MediaCore.cpp:561-575`). The snapshot carries `zoomGuestAvSync[] {participantId, offsetMs}` and `zoomGuestAvSyncRevision` (`CoreStateCapture.cpp:724-744`). The core clears every offset on a new speaker epoch (`MediaCore.cpp:549-559`) and retires an offset whose id left the roster (`:5651-5668`). The code is platform-neutral, and `GuestAvSyncTest` is in the Mac test build (`native/CMakeLists.txt:1049`).
- `sync-capture-audio-sources` is applied inside `applyCommands` under `coreMutex` (`MediaCore.cpp:1168-1181`, `:1354`, `:3437-3488`). `configure()` therefore runs under `coreMutex`.

**Windows (the oracle):**
- SHOW workspace: `XAML:977-1308`, a Border with `Height="255"`, `Padding="10,8"` and columns `190 | * | * | 1.4*` (`ColumnSpacing="10"`). RACK column: CHANNEL label, channel name, the guest lip-sync button and flyout (`XAML:1005-1035`), the slot list (`SelectedChannelInsertSlots`), and `＋ Add processing`. Then GATE (curve plus Threshold/Range, then Attack/Hold/Release), COMP (GR meter and readout in the header, curve, Threshold/Ratio, then Attack/Release/Knee/Makeup), and EQ (curve plus LOW CUT and eight vertical band faders at 63..8k, height 86). Every cell dims to 0.45 with no channel selected (`SVM:8863-8866`). The curve stroke is `#3FD2A8` when engaged and `#46545C` at 0.55 opacity when not (`W/Controls/DspResponseCurve.xaml.cs:60-72`).
- SHOW is a fixed viewport that never scrolls (`XAML:31-36`, `:914-917`). The strip bank is a horizontal ScrollViewer of 120-wide strips, and the master rail is pinned at 150 wide (`XAML:1309-1491`).
- SETUP is a scrolling page in two columns: `340 | * (MaxWidth 820)`, `ColumnSpacing 12` (`XAML:37-49`). The header row spans both columns. **Left:** "Devices & monitoring" with the Monitor output card and the Local audio source card (`XAML:102-181`). **Right:** "Master processor", the rack and meters (`XAML:187-627`), a divider, then "Inserts & plug-ins": Processing target picker, target label and detail, a collapsed "Advanced · VST3 plug-in browser" Expander, Audio gain / Manual gain / Output level readouts, the SIGNAL FLOW slot list, a 2×2 grid of Add EQ / Add compressor / Add VST3 plug-in / Clear chain, a note, and the selected-channel Mute button (`XAML:630-797`).
- Processing targets: `channel:<id>` for every strip, then `bus:master`, then every other bus in header order; the default is `bus:master` (`SVM:508`, `:11506-11603`). Kind and detail strings are at `SVM:11605-11648`. Selecting a strip selects its channel target (`SVM:4688-4713`). The bus chains live in a non-persisted dictionary (`SVM:59`) and are attached to **every** send of that bus, **after** the defaults are synthesised (`SVM:9221-9229`). Add/remove/clear and their status strings are at `SVM:5570-5627` and `:2618-2632`. Built-in names are `Noise Gate`, `Built-in EQ`, `Compressor`, `Limiter` (`SVM:2250`); normalisation is at `SVM:5550-5557`. Slot labels and states are at `SVM:2279-2331` (channel, `"1"`) and `:2655-2699` (target, `"01"`).
- `DspCurveMath` (`W/Services/DspCurveMath.cs`) matches the core's biquads and gain computer (`AudioDsp.h:1408-1435`, `:1548-1567`). **Windows has no test for it.**
- Guest lip sync: `W/ViewModels/GuestAvSyncController.cs`, tests `W.Tests/GuestAvSyncControllerTests.cs`, wiring `W/ViewModels/StudioViewModel.GuestAvSync.cs`, view handlers `W/Views/AudioPage.xaml.cs:16-48`.

**Mac today (the gaps A7 closes):**
- `ChannelInserts` stores three enable flags, and `activeNames` always emits gate→EQ→comp (`M/AppModel.swift:194-253`). There is no Limiter and no order. A VST is appended last and only while the host is live (`M/VstInserts.swift:240-247`).
- Both routing paths send `"busPluginInserts": []`: `AudioConsolePolicy.baseRoutingSends` (`M/AudioConsolePolicy.swift:220-222`) and the legacy tail of `pushAudioRouting` (`M/AppModel.swift:4626-4648`).
- `selectedStripId` is view `@State` (`M/AudioConsole.swift:32`), so the model has no selection to drive a processing target or the lip-sync controller.
- The curves are hand-drawn approximations (`M/AudioConsole.swift:202-260`).
- The channel chip shows the hard-coded text "Master" (`M/AudioConsole.swift:129`). Windows shows the primary routed bus (`SVM:11650-11661`).
- **Layout:** the whole Audio tab sits inside `TabPage`'s `ScrollView` (`M/ShellApp.swift:1287-1294`) and is capped at 1000 pt (`M/TabPageLayout.swift:21`). SHOW scrolls, and the strips are a plain `HStack` that clips past the window edge (`M/AudioConsole.swift:151-158`). **SETUP is one stacked `VStack`** of guide meters, rack, local source card and monitor controls (`M/AudioConsole.swift:77-111`), where Windows has two columns. A Mac-only `PLUG-INS` pill holds the VST browser; Windows keeps it as a collapsed Expander inside SETUP.

## 2. Boundary audit: Windows against Mac

| Boundary | Windows | Mac | Consequence in this plan |
|---|---|---|---|
| System audio capture | WASAPI loopback of one render endpoint (`AUDCLNT_STREAMFLAGS_LOOPBACK`, `N/modules/WasapiAudioCaptureSourceAdapter.cpp:287`). No permission prompt. Captures everything on that endpoint, **including our own monitor** (hence the endpoint-equality feedback guard, `MediaCore.cpp:7388-7410`). | `AudioHardwareCreateProcessTap` (macOS **14.2+**, `CoreAudio/AudioHardwareTapping.h`, **Objective-C only**: `#ifdef __OBJC__`) plus a private aggregate device. TCC "audio capture" prompt, `NSAudioCaptureUsageDescription`. A global tap captures every process except those excluded, independent of the output device. | The tap code is a `.mm` file. Excluding our processes is the only feedback protection: the endpoint guard can never match a private aggregate UID. A test pins that the guard stays false and the core is excluded. |
| Idle and denied states | Nothing playing means WASAPI delivers **no packets**: `framesReceived == 0`, which drives "loopback idle" (`SVM:8576-8580`). | A tap aggregate delivers **continuous exact zeros** when nothing plays, and (to be confirmed by the spike) also when TCC is denied. There is no public preflight for the audio-capture TCC service. | The Windows "loopback idle" state cannot occur. The core adds `captureDigitalSilence` (frames received, no non-zero sample since open). The shell shows one combined hint: nothing is playing, or permission is missing. No private SPI (OD-8). |
| Process identity | n/a | Exclusion works by audio **process object**, which exists only once a process has connected to coreaudiod. The Zoom engine sits behind a child guard (`N/modules/PosixChildProcess.h:74`), so the core does not hold its pid. `CATapDescription.bundleIDs` and `processRestoreEnabled` exist only on **macOS 26+**. The macOS 27.2 SDK has no tap API changes against 26.5 (header diff, 2026-11-22). | Exclude by process object: our own pid, the parent (shell) pid, and every process object whose `kAudioProcessPropertyBundleID` starts with `us.iamfatness.corevideopro`. On 26+, also set `bundleIDs`. Rebuild (or update in place, if the spike proves the description is settable) when the process list changes. |
| TCC attribution | n/a | The core is a bare executable `Contents/Resources/corevideo-native`, spawned by the shell with `Process()` (`M/MediaCoreBridge.swift:168-169`). Its signing identifier is `us.iamfatness.corevideopro.core` and it has no Info.plist (`scripts/make-macos-shell-bundle.sh:239-243`). The responsible process should be the shell. | Spike Q1/Q2: does the prompt name CoreVideo Pro, and is the usage string read from the shell's Info.plist? The spec's fallback ("create the tap in the shell, pass the aggregate UID to the core") **does not work with a private tap or aggregate**, which is visible only to its creator. It would need a public aggregate that shows in every app's device menu (Zoom's too) and leaks if the shell crashes. |
| Entitlements | n/a | The core already has `device.audio-input` and `disable-library-validation` (`scripts/macos-core.entitlements`). No entitlement is known to gate taps. | Spike Q3 confirms nothing new is needed under the hardened runtime and notarization. |
| Sample rate and clock | The WASAPI mix format is fixed per endpoint stream. The endpoint is pinned and the adapter never reopens on a default-device change (no `IMMNotificationClient` in the adapter). | The aggregate's rate is its main subdevice's rate: 44.1, 48 or 96 kHz, and lower for Bluetooth in hands-free mode. The adapter reads `kAudioDevicePropertyNominalSampleRate` **once** at open (`N/modules/CoreAudioCaptureSourceAdapter.cpp:303-309`) and AUHAL input does no rate conversion. The core resamples by `AudioFrame::sampleRate` at nominal rate with no drift correction (`MediaCore.cpp:6862-6872`, `AudioDsp.h:914-933`); drift is absorbed by the 1 s shed (`CoreAudioCaptureSourceAdapter.cpp:403-408`). | The tap source listens for rate and default-output changes and rebuilds, with debounce on an injected clock. Mislabelled frames would be a pitch shift. Drift handling is the same class as every existing CoreAudio input and is not changed here. |
| Hot-plug | AVCaptureDevice notifications re-list inputs (A5 review #2, `M/AudioInputs.swift:51-60`). | A tap is device-independent, but its aggregate's clock device is not. coreaudiod restarts (`kAudioHardwarePropertyServiceRestarted`) invalidate every object. | Rebuild on `DefaultOutputChanged`, `SampleRateChanged`, `ServiceRestarted`. A private aggregate dies with the core process, and the spike checks nothing leaks after `kill -9`. |
| Threading | WASAPI open runs on the command path too. | Tap and aggregate creation can block in coreaudiod for tens to hundreds of ms. Doing it inside `configure()` would hold `coreMutex` and stall render (the `[cmd] … ≥10ms` log, `MediaCore.cpp:1176-1180`). | Tap builds run on a dedicated `SystemAudioTapWorker` thread. `configure()` only posts. A test proves `configure()` returns while the fake platform's build is blocked on a latch. |
| Bus insert DSP | EQ and gate on a bus are dead buttons (pass-through) and the slot still reads `LIVE` (`SVM:2675-2690`). | Same shared core. | OD-1: the Mac dims "Add EQ" for bus targets and gives the reason (honest-reporting). Its slot state is `PASS`, never `LIVE`. |
| Insert-slot flyout | The EQ flyout writes `presenceHz` and `presenceDb` (`W/Views/AudioPage.xaml.cs:148-156`), keys the core no longer reads (8-band since C6b). These are dead sliders on Windows. | n/a | Not ported. The Mac EQ slot flyout shows High-pass only plus "Adjust the bands in the EQ cell." Filed as a Windows issue. |
| Guest lip-sync target | `SelectedZoomId` accepts any id with no `:` other than `zoom-mix` (`GuestAvSyncController.cs:43-50`), so `local-machine-audio` and `media` pass. The core accepts them and then silently retires them at the next roster pass. | `AudioConsolePolicy.isZoomParticipantChannel` already excludes `local-machine-audio` and `media` (`M/AudioConsolePolicy.swift:79-82`). | OD-4: the Mac uses `isZoomParticipantChannel` **and** membership in `zoomAudioSourceIds`. Filed as a Windows issue. |

## Global constraints

- Shell commands are full-state. Never send a partial list. Every new piece of state reaches a respawned core through `reassertAudioConsole()` (mix, then routing base, then capture audio, in that order: `M/AppModel.swift:5415-5444`).
- A saved choice must reach the **first** reassert of a launch with no dependency on a snapshot or a device enumeration (the Phase 2 lesson, `a9da9eb8`).
- No new work under `coreMutex` or on the audio worker's lock path (`docs/reference/gotchas.md` audio two-lock rule). A8's tap build runs on its own thread.
- Honest reporting (`docs/reference/honest-reporting.md`): a control that cannot work is dimmed and gives its reason. A state the core did not report reads a dash or "unknown".
- Copy that is ported from Windows stays verbatim, with its `SVM:`/`XAML:` line cited in a code comment.
- Layout numbers come from the XAML: 255, 190/\*/\*/1.4\*, 120, 150, 340/820, 12, 10. Checked by pixels from `COREVIDEO_SHELL_TAB_SNAPSHOT` at 1360 and 1920, plus the 900-wide pop-out.
- Clocks are injected wherever a test depends on time (`timerSleepForTesting` in the shell; a `std::function<int64_t()>` in native).
- Any audio performance gate runs on a **quiet machine** (no other agent building, no Zoom, no browser playing) and records the hardware, the macOS version and the run length.
- Native builds use the Xcode 26.6 toolchain workaround from the quick-wins outcomes when the default `MacOSX27.2.sdk` linker fails: `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` and `CMAKE_OSX_SYSROOT=.../MacOSX26.5.sdk`.
- Each task ends with an **independent review**: a fresh reviewer gets the diff and the task's mutation list, not the implementer's conclusions. Every finding is checked in code before it is fixed.

## Review focus

The five failure modes most likely to reach an operator that no single task's happy path exercises. Each has a test in the task named.

1. **A core respawn mid-show with System audio selected.** The saved tap source must be in the first reassert batch even though "system-audio" is not an AVCaptureDevice and no snapshot has arrived yet. Test: Task 11 `a8-first-connect-sends-system-audio`.
2. **The output device changes mid-show** (headphones plugged in, AirPods switching rate). The tap rebuilds and frames carry the new rate; no pitch shift and no frozen source. Tests: Task 10 `ProcessTap.RateChangeRebuildsAndRelabels` and `DefaultOutputChangeRebuildsOnceAfterQuiet`; device proof in Task 12.
3. **Two quick lip-sync nudges before the first ack.** The final offset must be +20, not +10. A core respawn resets the revision memo, so the new core's snapshots are adopted. Tests: Task 7 `a9-nudges-serialise` and `a9-new-core-generation-resets-memo`.
4. **Removing a custom bus that carries an insert chain, or is the selected processing target.** The target falls back to `bus:master`, the chain is dropped, and no stale `busPluginInserts` go out. Test: Task 3 `a7-removed-bus-drops-chain-and-target`.
5. **A master chain on a show whose sources use only implicit default routes.** The chain must be on every default master send in both routing paths, and on the respawn batch. Tests: Task 3 `a7-master-chain-on-every-default-send` and `a7-legacy-route-path-keeps-bus-chains`.

---

## Task list

| # | Task | Slice | Lane | Size | Depends on |
|---|---|---|---|---|---|
| 1 | Response-curve maths and view (Windows vectors) | A7 | shell | S | none |
| 2 | Ordered channel insert chain, Limiter, slot model | A7 | shell | M | none |
| 3 | Processing targets, bus and master chains on the wire | A7 | shell | M | 2 |
| 4 | SHOW layout: fixed viewport, Windows workspace, strip bank | A7 | shell | L | 1, 2, 3 |
| 5 | SETUP layout: two columns, Inserts & plug-ins, VST browser folded in | A7 | shell | M | 3 |
| 6 | A7 evidence: read-back, audio proof, quiet-machine performance | A7 | shell+native tooling | S | 4, 5 |
| 7 | Guest lip sync (controller, wire, snapshot, flyout) | A9 | shell | M | 4 (placement only; the controller can start in parallel) |
| 8 | A9 evidence: clap skew moves by the offset | A9 | tooling | S | 7 |
| 9 | **A8 spike** on the signed bundle (timebox 2 days) | A8 | native | M | none (start on day 1) |
| 10 | Native system-audio tap source | A8 | native | L | 9 |
| 11 | Shell: System audio entry, status, hint, Info.plist | A8 | shell | M | 10 (wire), 5 (card placement) |
| 12 | A8 evidence on the installed, signed build | A8 | native+owner | M | 10, 11 |

Lanes: the native lane runs 9 → 10 → 12. Shell lane B runs 1 → 2 → 3 → 4/5 → 6 → 7 → 8 → 11. Task 9 starts on day 1, in parallel with Task 1.

---

### Task 1: Response-curve maths and view

**Files:**
- Create: `M/DspCurveMath.swift` (pure maths plus the `DspResponseCurve` view)
- Create: `M/ShellTestsAudioPhase4.swift` (registers `audioPhase4Cases`)
- Modify: `M/ShellTests.swift:4139` (append `+ audioPhase4Cases`)

**Interfaces:**
- Produces:
  - `struct DspPoint: Equatable { let x: Double; let y: Double }`
  - `enum DspCurveMath { static let minFrequencyHz = 20.0, maxFrequencyHz = 20000.0, minDb = -24.0, maxDb = 12.0; static let eqBandHz: [Double] = [63,125,250,500,1000,2000,4000,8000]; static func eqResponse(highpassHz: Double, bandGainsDb: [Double], pointCount: Int = 64, sampleRate: Double = 48000) -> [DspPoint]; static func compressorTransfer(thresholdDb: Double, ratio: Double, kneeDb: Double = 0, pointCount: Int = 48) -> [DspPoint]; static func gateTransfer(thresholdDb: Double, rangeDb: Double = -80, pointCount: Int = 48) -> [DspPoint] }`
  - `struct DspResponseCurve: View { enum Kind { case eq, comp, gate }; let kind: Kind; let p1: Double; let p2: Double; let p3: Double; let bands: [Double]; let isEngaged: Bool }`

A line-for-line port of `W/Services/DspCurveMath.cs`, including the high-pass clamp 20..400, band clamp ±12, the skip rule `|gain| < 0.05`, peaking Q 1.4, ratio floored at 1, the knee quadratic, and the gate floor at -80. The view draws the reference line (the 0 dB horizontal for EQ, the unity diagonal for comp and gate) and strokes `#3FD2A8` when engaged, `#46545C` at 0.55 opacity when bypassed (`DspResponseCurve.xaml.cs:60-72,128-152`). y is flipped: `(1 - y) * height`.

**Fixture provenance.** Windows has no `DspCurveMath` test. The vectors below are the output of the Windows source file itself, compiled unmodified on this Mac on 2026-11-22: `~/.dotnet/dotnet run` on a scratch `net9.0` console project that `<Compile Include>`s `W/Services/DspCurveMath.cs`. The task re-runs the same project and pastes its output into the test, so the fixture is regenerated from Windows code and not hand-copied. Invariants: EQ has 64 points and comp/gate have 48; `x == i/(n-1)`; tolerance `1e-9`.

- [ ] **Step 1: Regenerate the vectors from the Windows file.** Create `<scratchpad>/dspvec/dspvec.csproj` (`net9.0`, `<Compile Include="…/W/Services/DspCurveMath.cs"/>`) and a `Program.cs` that prints `(x, y)` with `ToString("R")` for the cases below. Expected output (2026-11-22 run):

```
eq(90, flat)            [10]=(0.15873015873015872, 0.4483948696574838) [20]=(0.31746031746031744, 0.6592303079543049) [63]=(1, 0.6666666666543891)
eq(20, band5 -6)        [0]=(0, 0.5830082106640145) [32]=(0.5079365079365079, 0.5953287920957033) [40]=(0.6349206349206349, 0.6086025552198154)
eq(400, band8 +12)      [0]=(0, 0) [40]=(0.6349206349206349, 0.6741139415929801) [52]=(0.8253968253968254, 0.8401268106052969)
eq(20, band1 0.04)      [10]=(0.15873015873015872, 0.6651732863528543)   (0.04 dB is skipped: equals a flat EQ at 20 Hz)
comp(-18, 4, knee 0)    [33]=(0.7021276595744681, 0.700531914893617) [40]=(0.851063829787234, 0.7377659574468086) [47]=(1, 0.775)
comp(-18, 4, knee 6)    [32]=(0.6808510638297872, 0.6772818583069262) [33]=(0.7021276595744681, 0.6919378112267994) [34]=(0.723404255319149, 0.7031985626980535)
comp(-18, 0.5)          [47]=(1, 1)                                       (ratio floored at 1)
gate(-40, -20)          [20]=(0.425531914893617, 0.17553191489361702) [23]=(0.48936170212765956, 0.23936170212765956) [24]=(0.5106382978723404, 0.5106382978723404)
gate(-12, -6)           [30]=(0.6382978723404256, 0.5632978723404255) [41]=(0.8723404255319149, 0.8723404255319149)
```

- [ ] **Step 2: Write the failing tests** in `ShellTestsAudioPhase4.swift`:
  - `a7-curve-eq-windows-vectors`: the four EQ rows. *Mutations:* Q 1.0 instead of 1.4 (red at `[32]`); drop the skip rule (red on the 0.04 row); no high-pass clamp (red at `eq(400…)[40]`).
  - `a7-curve-comp-windows-vectors`: the three comp rows. *Mutations:* hard knee only (red at `[33]` with knee 6); no ratio floor (red at `[47]`).
  - `a7-curve-gate-windows-vectors`: the gate rows. *Mutation:* `<=` instead of `<` at the threshold (red at `[24]`).
  - `a7-curve-point-counts`: `eqResponse(...).count == 64`, `compressorTransfer(...).count == 48`, and `x` of the last point `== 1`.
- [ ] **Step 3: Run and verify red.** `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 COREVIDEO_SHELL_TESTS_FILTER=a7-curve ./.build/release/CoreVideoProShell`. Expected: a compile failure (no `DspCurveMath`).
- [ ] **Step 4: Implement** `DspCurveMath` and `DspResponseCurve` in `M/DspCurveMath.swift`.
- [ ] **Step 5: Run and verify green**, then run each named mutation and confirm it turns red. Record the results.
- [ ] **Step 6: Commit** `feat(mac-audio): DspCurveMath, a port of the Windows curve maths, pinned to vectors from the Windows file`.
- [ ] **Step 7: Independent review** (diff plus the mutation list).

### Task 2: Ordered channel insert chain, Limiter, slot model

**Files:**
- Create: `M/AudioInsertChain.swift` (`InsertChainPolicy`, `InsertSlot`)
- Modify: `M/AppModel.swift:194-253` (`ChannelInserts`), `:5517-5520` (`editInserts`), `:5546-5560` (`audioMixCommand`)
- Modify: `M/VstInserts.swift:240-256` (`pluginInsertNames`, `setVstChannelSelection`)
- Modify: `M/AppModel.swift:848` and the prefs restore (`vstChannelSelections` compatibility)
- Test: `M/ShellTestsAudioPhase4.swift`

**Interfaces:**
- Produces:
  - `ChannelInserts.chain: [String]`, the ordered canonical names (Windows `PluginInserts`). `gateEnabled`, `eqEnabled` and `compEnabled` become **get-only** computed properties (`chain` contains the name, case-insensitive). New `limiterCeilingDb = -1.0` (clamp -12...-0.1). `static let limiterName = "Limiter"`, `static let builtInNames = [gateName, eqName, compName, limiterName]` (Windows `SVM:2250`).
  - `mutating func toggle(_ name: String)`: remove it if present, else append (Windows `ToggleSelectedRackInsert`, `SVM:5496-5521`).
  - `mutating func add(_ raw: String) -> InsertChainPolicy.AddOutcome`, where `enum AddOutcome { case added(String), duplicate(String) }`. Uses `normalize`; a duplicate is refused case-insensitively (`SVM:5531-5537`).
  - `mutating func remove(_ name: String) -> Bool` and `mutating func clear()`.
  - `var settingsJson: JSONObject`: settings for **every built-in present in `chain`**, keyed by its chain name (gate keys, `highpassHz` plus `band1Db..band8Db`, compressor keys, limiter `ceilingDb`).
  - `enum InsertChainPolicy { static func normalize(_ raw: String) -> String` (Windows `NormalizeAudioInsertName`: `eq`/`built-in eq` → `Built-in EQ`, `compressor` → `Compressor`, `vst`/`vst3`/`vst3 bridge` → `VST3 Bridge Slot`, else trimmed)`; static func isBuiltIn(_:) -> Bool` (`SVM:2252-2258`)`; static func isVst(_:) -> Bool` (`vst:` prefix, case-insensitive, or contains `VST3`)`; static func slots(chain: [String], onBus: Bool, vstLive: Bool, vstStatusLine: String, labelStyle: LabelStyle) -> [InsertSlot] }`, with `enum LabelStyle { case channel /* "1" */, target /* "01" */ }`.
  - `struct InsertSlot: Equatable, Identifiable { let name: String; let slotLabel: String; let isBuiltIn: Bool; let statusLabel: String; let tooltip: String; var id: String { name } }`. Status: a built-in that processes in this position reads `LIVE` with "Processing active."; `onBus` and a gate/EQ/high-pass name reads `PASS` with "Built-in <name> does not process on a bus; audio passes through unchanged."; a VST reads `BYPASS` with `vstStatusLine` when `!vstLive`. (Windows `START`/`LIVE` apply only to a live host, which the Mac does not have before V2.)
  - `AppModel.pluginInsertNames(for:) -> [String]` = `chain`, dropping VST names unless `vstInsertsLive`.
- Consumes: `VstHostState`/`vstInsertsLive`/`vstStatusLine` (`M/VstInserts.swift:71-160`).

Prefs compatibility: `ShellPrefs.vstChannelSelections` stays. On restore, a saved selection is **appended** to that strip's chain. On save, the last VST name in each chain is written. Built-in chains are not persisted (Windows does not persist them). They reach a respawned core through the mix command in `reassertAudioConsole()`.

- [ ] **Step 1: Failing tests:**
  - `a7-chain-order-is-toggle-order`: toggle EQ, then Gate, then Compressor; `audioMixCommand()["channels"][0]["pluginInserts"] == ["Built-in EQ","Noise Gate","Compressor"]`. *Mutation:* keep today's fixed `activeNames` order (red).
  - `a7-chain-limiter-processes-with-ceiling`: add `"Limiter"` with ceiling -3; the wire has `insertSettings["Limiter"]["ceilingDb"] == -3` and the Limiter name. *Mutation:* omit the limiter from `settingsJson` (red).
  - `a7-chain-duplicate-refused`: `add("eq")` twice; the second returns `.duplicate("Built-in EQ")` and the chain count is 1. *Mutation:* compare case-sensitively (red with `add("built-in eq")`).
  - `a7-chain-normalize-is-windows`: table `eq→Built-in EQ`, `Built-In EQ→Built-in EQ`, `compressor→Compressor`, `vst3→VST3 Bridge Slot`, `  Reverb ` → `Reverb`.
  - `a7-chain-vst-gated-on-host`: chain `["Compressor","vst:Pro-Q 3"]` with `vstInsertsLive == false` gives a wire of `["Compressor"]`, and the slot for the VST reads `BYPASS` with tooltip == `vstStatusLine`. *Mutation:* send the VST unconditionally (red).
  - `a7-chain-slots-labels`: channel style labels `"1","2"`, target style `"01","02"`; on a bus, `Built-in EQ` reads `PASS` and `Compressor` reads `LIVE`. *Mutation:* `LIVE` for every built-in (red on the bus EQ row; this guards OD-1).
  - `a7-chain-reasserted-on-new-core`: set a chain, call `onConnected()` with `audioReassertObserver` set; the first command's channel carries the chain in order. (Confirmatory: the mix command was already in the reassert batch; this pins that the chain did not move off it.)
  - `a7-chain-prefs-compat`: prefs with `vstChannelSelections = ["101": "vst:Test"]` restore to the chain `["vst:Test"]`; the save writes it back.
- [ ] **Step 2: Run and verify red** (`COREVIDEO_SHELL_TESTS_FILTER=a7-chain`).
- [ ] **Step 3: Implement.** Keep `ChannelInserts`' parameter fields and defaults unchanged (they are the core defaults). Existing callers of `$0.gateEnabled.toggle()` move to `toggle(ChannelInserts.gateName)`.
- [ ] **Step 4: Verify green, plus every Phase 2 and quick-wins audio case** (`COREVIDEO_SHELL_TESTS_FILTER=audio` and `quickwins`). Run the mutations.
- [ ] **Step 5: Commit** `feat(mac-audio): ordered channel insert chain with Limiter (Windows PluginInserts order)`.
- [ ] **Step 6: Independent review.**

### Task 3: Processing targets, bus and master chains on the wire

**Files:**
- Modify: `M/AudioInsertChain.swift` (add `AudioProcessingTarget`, `AudioProcessingTargetPolicy`)
- Modify: `M/AudioConsolePolicy.swift:196-223` (`baseRoutingSends` gains `busInserts`)
- Modify: `M/AppModel.swift:4511-4533` (`routingBaseCommand`), `:4626-4648` (legacy `pushAudioRouting` tail), `:4385-4395` (bus removal), selection state
- Test: `M/ShellTestsAudioPhase4.swift`

**Interfaces:**
- Produces:
  - `struct AudioProcessingTarget: Identifiable, Equatable { let id: String; let label: String; let kind: String; let detail: String; let insertLabel: String }`
  - `enum AudioProcessingTargetPolicy { static let defaultTargetId = "bus:master"; static func channelId(_ stripId: String) -> String /* "channel:<id>" */; static func busId(_ busId: String) -> String /* "bus:<id>" */; static func parse(_ id: String) -> (channel: String?, bus: String?); static func kind(busId: String) -> String; static func detail(busId: String) -> String; static func insertLabel(_ chain: [String]) -> String; static func targets(strips: [(id: String, label: String, primaryBus: String)], buses: [AudioBus], channelChains: [String: [String]], busChains: [String: [String]]) -> [AudioProcessingTarget] }`. Kind and detail strings are verbatim from `SVM:11605-11648` (`"Master bus"`, `"Aux bus"`, `"ISO bus"`, `"Custom bus"`, `"Bus"`; `"Final program mix processing before output."` …). The channel detail is `"Channel - <primary bus label>"`. Order: channels in strip order, then `bus:master`, then the remaining buses in `AudioBusPolicy` order. `insertLabel` is `"No inserts"` or the names joined with `" + "`.
  - `AppModel`: `@Published var selectedAudioStripId: String?` (moved from view `@State`), `@Published var selectedProcessingTargetId = "bus:master"`, `@Published var busInserts: [String: [String]] = [:]` (session state, **not persisted**, as on Windows).
  - `func selectAudioStrip(_ id: String)` sets both (Windows `SelectParticipant`, `SVM:4688-4693`). `var processingTargets: [AudioProcessingTarget]`. `func addProcessingInsert(_ raw: String)`, `func removeProcessingInsert(_ name: String)`, `func clearProcessingInserts()`. These route to the channel chain (Task 2) or `busInserts`, set `takeStatus` to the Windows strings (`"<n> is already on <target>"`, `"<n> added to <target>"`, `"Removed <n> from <target>"`, `"Insert chain cleared for <target>"`), then `scheduleMixSync()` (channel) or `pushRoutingBase()` (bus).
  - `func primaryBusLabel(_ stripId: String) -> String`: the first bus in `buses` order where `audioSends[stripId][bus] != nil`, its label, else `"MASTER"` (`SVM:11650-11661`).
  - `AudioConsolePolicy.baseRoutingSends(…, busInserts: [String: [String]] = [:])`: every send's `busPluginInserts = busInserts[busId] ?? []`, applied **after** the defaults and the doubling guard (`SVM:9221-9229`).
  - `func busChainFromSnapshot(_ matrix: JSONObject) -> [String: [String]]` reads `busProcessing[]` (for the drill and diagnostics only; the shell never adopts it as intent).
- Consumes: Task 2's `ChannelInserts.add/remove/clear`, `InsertChainPolicy.normalize`.

Bus removal (`removeCustomBus`) also drops `busInserts[id]`, and when the removed bus is the selected target, falls back to `bus:master`. A strip that disappears while it is the selected target also falls back to `bus:master` (Windows `RefreshAudioProcessingTargets`, `SVM:11591-11596`, falls back to the first target; the Mac uses `bus:master`, the documented default, because the first target moves with roster churn. Stated in the code comment).

Bus targets: `addProcessingInsert("Built-in EQ")` on a `bus:` target is **refused** with `takeStatus = "Built-in EQ does not process on a bus yet; add a compressor or limiter."` (OD-1 default). The SETUP button is dimmed with the same text (Task 5).

- [ ] **Step 1: Failing tests.** Fixture invariant: strips `101`, `102`; default sends exactly `AudioConsolePolicy.defaultSendBuses`; no operator edits unless stated.
  - `a7-master-chain-on-every-default-send`: `busInserts = ["master": ["Compressor","Limiter"]]`; every send in `routingBaseCommand()["sends"]` with `busId == "master"` (both strips, 2 sends) has `busPluginInserts == ["Compressor","Limiter"]`, and every other send has `[]`. *Mutations:* attach only to operator edits (red: defaults lack it); attach before the defaults are synthesised (red).
  - `a7-chain-survives-doubling-guard`: ProgramMix with `meetingAudio: true` and a master chain; the `zoom-mix→master` send carries the chain.
  - `a7-legacy-route-path-keeps-bus-chains`: drive the `pushAudioRouting` tail (core without an authority epoch: `captureRequestForTesting` returns a snapshot with no `audioRouteAuthorityEpoch`) and assert the master sends carry the chain. *Mutation:* leave `[]` in the tail (red).
  - `a7-bus-chain-change-pushes-matrix`: adding a Limiter to `bus:aux-1` with no routing change makes `routingBaseSendObserver` see one new matrix (the memo compares whole send objects, `M/AppModel.swift:4442-4445`). *Mutation:* strip `busPluginInserts` from the memo copy (red).
  - `a7-removed-bus-drops-chain-and-target`: add `bus-01`, select `bus:bus-01`, add a Compressor, remove the bus; the target is `bus:master`, `busInserts["bus-01"] == nil`, and no send in the next matrix names `bus-01`.
  - `a7-targets-order-and-strings`: `processingTargets.map(\.id)` == `["channel:101","channel:102","bus:master","bus:pgm-l","bus:pgm-r","bus:iso-1",…,"bus:aux-2"]`; the kind and detail of `bus:aux-1` are `"Aux bus"` / `"Aux send processing."`; an empty chain's `insertLabel` is `"No inserts"`. *Mutation:* master after the other buses (red).
  - `a7-select-strip-selects-channel-target`: `selectAudioStrip("102")` gives `selectedProcessingTargetId == "channel:102"`; `addProcessingInsert("compressor")` puts `Compressor` in strip 102's chain and the status reads `"Compressor added to <label>"`.
  - `a7-bus-eq-refused-with-reason`: `bus:master` plus `addProcessingInsert("eq")` leaves the chain unchanged and the status is the OD-1 sentence. *Mutation:* allow it (red).
  - `a7-bus-chain-reasserted-on-new-core`: `onConnected()` with `audioReassertObserver` set; the routing command in the batch has the master chain on master sends.
  - `a7-primary-bus-label`: strip 101 routed to master gives `"MASTER"`; with master removed by an operator edit and `aux-1` routed, it gives `"AUX 1"`.
- [ ] **Step 2: Verify red.**
- [ ] **Step 3: Implement.** Move `selectedStripId` out of `AudioPane` into the model (the pop-out and the main window share the selection, as on Windows where one view model backs both).
- [ ] **Step 4: Verify green, plus the full audio, quick-wins and control filters** (the `audio.route.set` tests read `routingBaseCommand`). Run the mutations.
- [ ] **Step 5: Commit** `feat(mac-audio): processing targets; bus and master insert chains ride every send (Windows SVM:9221-9229)`.
- [ ] **Step 6: Independent review.**

### Task 4: SHOW layout, a fixed viewport with the Windows workspace

**Files:**
- Create: `M/AudioWorkspace.swift` (the workspace extracted from `M/AudioConsole.swift:164-335`)
- Modify: `M/AudioConsole.swift` (`AudioPane` SHOW, `ConsoleStrip`, `MasterLufsRail`)
- Modify: `M/ShellApp.swift:1287-1294` (`TabPage`: no outer `ScrollView` for `.audio`), `M/TabPageLayout.swift:21` (`.audio` → `.infinity`)
- Modify: `M/TabPageLayout.swift` `TabSnapshotHarness.render` (an optional `COREVIDEO_SHELL_TAB_SNAPSHOT_HEIGHT`, used when set, so a fixed-viewport SHOW renders at a real window height and not its fitting height)
- Test: `M/ShellTestsAudioPhase4.swift`

**Interfaces:**
- Consumes: `DspResponseCurve` (Task 1); `ChannelInserts.chain/toggle`, `InsertChainPolicy.slots` (Task 2); `selectAudioStrip`, `primaryBusLabel`, `addProcessingInsert` (Task 3).
- Produces: `struct ProcessingWorkspace: View` (stripId), `struct InsertSlotList: View { let slots: [InsertSlot]; let onRemove: (String) -> Void; let flyout: (InsertSlot) -> AnyView }` (reused by Task 5), `enum AudioLayout { static let workspaceHeight: CGFloat = 255; static let rackWidth: CGFloat = 190; static let stripWidth: CGFloat = 120; static let railWidth: CGFloat = 150; static let setupLeftWidth: CGFloat = 340; static let setupRightMaxWidth: CGFloat = 820; static let columnSpacing: CGFloat = 10; static let setupColumnSpacing: CGFloat = 12 }`.
- Produces: `struct InsertParamSpec: Equatable { let label: String; let key: String; let min: Double; let max: Double; let step: Double; let defaultValue: Double }` and `InsertChainPolicy.paramSpecs(_ name: String) -> [InsertParamSpec]`. Gate: `("Threshold (dBFS)","thresholdDb",-80,-12,1,-48)`, `("Release (ms)","releaseMs",20,500,5,120)`. High-pass: `("High-pass (Hz)","highpassHz",20,400,5,90)`. EQ: the high-pass row only (presence rows dropped: dead on Windows). Compressor: `("Threshold (dBFS)","thresholdDb",-40,-6,1,-18)`, `("Ratio","ratio",1,20,0.5,4)`. Limiter: `("Ceiling (dBFS)","ceilingDb",-12,-0.1,0.1,-1)`. All from `W/Views/AudioPage.xaml.cs:137-166`.

**Layout (exact):**
- `AudioPane` in SHOW is a `VStack(spacing: 8)` inside a panel with padding 10, filling the tab's height and width (no width cap). Rows:
  1. The header (existing pills, toggle and pop-out), height 34.
  2. The channel chip row: a horizontal `ScrollView` with indicators hidden. Each chip is 88...170 wide and shows the name (11 pt semibold, one line, truncated) above `primaryBusLabel` (8 pt). Tapping a chip calls `selectAudioStrip`. Replaces the hard-coded "Master".
  3. The workspace: height 255, padding (h 10, v 8), `HStack(spacing: 10)` with the rack at **190** fixed, then GATE, COMP and EQ sharing the rest in a **1 : 1 : 1.4** ratio (a `GeometryReader` splits the width; tested below as a pure `AudioLayout.cellWidths(total:) -> (gate: CGFloat, comp: CGFloat, eq: CGFloat)`).
     - **Rack** (top to bottom): "CHANNEL" (8 pt, tracked), the channel name (14 pt bold, `#A7E8F2`), the guest lip-sync button placeholder (filled by Task 7; until then nothing renders), the `InsertSlotList` in a vertical `ScrollView` (style `.channel`), and a `Menu("＋ Add processing")` whose sections are the disabled header "BUILT-IN — processes live", the four built-ins, a divider, the disabled header "VST3 PLUG-INS", then each scanned plug-in (disabled with `vstStatusLine` while `!vstInsertsLive`). Clicking a slot opens a popover 240 wide holding the slot name, its `paramSpecs` sliders (a VST with no params shows the Windows text at `AudioPage.xaml.cs:219-227`), and "Remove from chain".
     - **GATE** cell: header "GATE" with an IN toggle (`toggle(gateName)`); `DspResponseCurve(.gate, p1: threshold, p2: range)`; Threshold (-80..-12) and Range (-80..-6); then Attack (0.5..50), Hold (0..500) and Release (20..500). The existing ranges at `M/AudioConsole.swift:211-218` are kept.
     - **COMP** cell: header "COMP", the GR meter (16 segments, height 8) and the GR label (`"GR −x.x dB"` when > 0.05, else `"GR —"`, `SVM:2401-2407`), and IN; `DspResponseCurve(.comp, p1: threshold, p2: ratio, p3: knee)`; Threshold and Ratio; then Attack, Release, Knee and Makeup.
     - **EQ** cell: header "EQ" and IN; `DspResponseCurve(.eq, p1: highpass, bands)`; then a row of height 86: "LOW CUT" slider (20..400, width share 1.6) and eight vertical band faders (-12..12, step 0.5, fader height 62) labelled `63 125 250 500 1k 2k 4k 8k`.
     - With no strip selected, every cell's opacity is 0.45 and hit-testing is off (`SVM:8866`). The curve, not the cell, shows engaged or bypassed (cells no longer dim to 0.55 when off).
  4. The strip bank fills the remaining height: `HStack(spacing: 8)` of a horizontal `ScrollView` of strips (`spacing 4`) and `MasterLufsRail` pinned at **150** wide. `ConsoleStrip` is **120** wide; its fader and meter stretch vertically (no fixed 150 pt height). It gains the PAN readout (`"C"`, `"L 0.25"`, `"R 0.50"` from `SVM:11452-11456`), the gain label with a sign (`"+3.0 dB"`, `SVM:11451`), the meter tooltip (`"Muted in mix"` / `"Source muted"` / `"Live"`, `SVM:11464`), and the name chip as a button that calls `selectAudioStrip`.
- `TabPage` renders `.audio` without its `ScrollView`. SETUP brings its own (Task 5) and ROUTING keeps its existing internal scrolling.

- [ ] **Step 1: Failing tests:**
  - `a7-layout-cell-widths`: `AudioLayout.cellWidths(total: 1000)` with rack 190 and three gaps of 10 gives `gate == comp` and `eq / gate == 1.4` within 0.01, and `gate + comp + eq == 1000 - 190 - 30`. *Mutation:* equal thirds (red).
  - `a7-pan-gain-tooltip-strings`: pan `0.004 → "C"`, `-0.25 → "L 0.25"`, `0.5 → "R 0.50"`; gain `3 → "+3.0 dB"`, `-2.5 → "-2.5 dB"`; tooltip muted → `"Muted in mix"`, Zoom-muted → `"Source muted"`.
  - `a7-param-specs-are-windows`: the table above, and EQ has exactly one spec (`highpassHz`). *Mutation:* port the presence rows (red on the count).
  - `a7-show-not-in-page-scroll`: `TabPage.wrapsInScrollView(.audio) == false`, `true` for `.settings` (a small pure helper the view uses). *Mutation:* revert the `TabPage` change (red).
- [ ] **Step 2: Verify red. Step 3: Implement.**
- [ ] **Step 4: Pixels.** `COREVIDEO_SHELL_TAB_SNAPSHOT=<scratch> COREVIDEO_SHELL_TAB_LIST=audio COREVIDEO_SHELL_AUDIO_SURFACE=SHOW COREVIDEO_SHELL_TAB_SNAPSHOT_HEIGHT=820 ./.build/release/CoreVideoProShell`. Extend `seedAudio` (`M/TabPageLayout.swift:122-135`) with eight more strips (so the bank must scroll), a chain `["Built-in EQ","Compressor","Limiter"]` on 101, and 101 selected. Review both widths: the workspace is 255 tall, the rack is 190, EQ is visibly the widest cell, the rail sits at the right edge with strips scrolling under it, and nothing is clipped. Repeat at width 900 (the pop-out minimum, `M/ShellApp.swift:170`). **If the 900 render clips a cell's controls, raise the pop-out `minWidth` to 1080 (the spec's pop-out size) and state it in the commit.**
- [ ] **Step 5: Verify green, then the full shell suite** (`COREVIDEO_SHELL_TESTS=1`, all cases), `COREVIDEO_SHELL_SELFCHECK=1`, `bash scripts/lint-mac-design.sh`.
- [ ] **Step 6: Commit** `feat(mac-audio): SHOW matches Windows XAML (fixed viewport, 190|1|1|1.4 workspace, scrolling strip bank, pinned rail)`.
- [ ] **Step 7: Independent review** (diff plus the PNGs plus the XAML line ranges).

### Task 5: SETUP layout in two columns, with Inserts & plug-ins and the VST browser folded in

**Files:**
- Create: `M/AudioSetup.swift` (`AudioSetupSurface`)
- Modify: `M/AudioConsole.swift:40-111` (pills: remove `PLUG-INS`; SETUP → `AudioSetupSurface()`), `M/VstInserts.swift:339-470` (`VstInsertsSurface` becomes the disclosure content; no behaviour change), `M/LocalAudioSource.swift` (card placement only)
- Modify: `M/AudioConsole.swift:26-30` (`initialSurface` accepts only `SHOW|ROUTING|SETUP`; `PLUG-INS` maps to `SETUP` with the disclosure open, so existing snapshot scripts keep working)
- Test: `M/ShellTestsAudioPhase4.swift`

**Interfaces:**
- Consumes: Task 3 (`processingTargets`, `selectedProcessingTargetId`, add/remove/clear), Task 4 (`InsertSlotList`, `AudioLayout`).
- Produces: `struct AudioSetupSurface: View`, and `AudioSetupPolicy.addButtons(target: String, vstLive: Bool, vstStatusLine: String) -> [(title: String, enabled: Bool, reason: String?)]`, which returns Add EQ (disabled on `bus:` targets with the OD-1 sentence), Add compressor, Add VST3 plug-in (disabled with `vstStatusLine` while not live) and Clear chain (disabled when the chain is empty).

**Layout (exact, `XAML:37-797`):**
- A `ScrollView` (vertical) wraps an `HStack(alignment: .top, spacing: 12)`. The header row already sits above the surface switch, as on Windows, where it spans both columns.
- **Left column, width 340:** the title "Devices & monitoring", then two cards (corner 6, padding 12): **Monitor output** (title 12 semibold, status 10 pt muted, the Device picker (`MonitorDevicePicker`), a full-width "Monitor" toggle, and the volume slider with a 46-wide percentage label) and **Local audio source** (the existing `LocalAudioSourceCard`, restyled to the card: title, status, the "Input / loopback" picker, the "Source" toggle). The feedback-risk and monitor-control notices stay under the monitor card.
- **Right column, flexible, max 820:** "Master processor" with the summary "Program L/R · processing order" and the existing MASTER IN toggle; the existing `MasteringGuideMeters` and `MasteringRack` content (unchanged); a 1 pt divider; then **"Inserts & plug-ins"** with the summary `"Choose the exact channel or bus, then build its insert chain in processing order."` (verbatim, `XAML:632-633`), a Processing target `Picker` over `processingTargets` (label = target label), the target label (16 semibold), the target detail, a `DisclosureGroup("Advanced · VST3 plug-in browser")` that is **collapsed by default** and holds `VstInsertsSurface`, three readout boxes (Audio gain, Manual gain, Output level, for the selected strip: `"—"` when the target is a bus), a "SIGNAL FLOW · TOP TO BOTTOM" box holding `InsertSlotList` (style `.target`) and the target's `insertLabel`, a 2×2 grid of `addButtons`, the note `"Select a VST3 plug-in above to open its controls."`, and the Mute button (`"Mute in mix"` / `"Unmute in mix"`, `SVM:2229-2230`; hidden for bus targets).

- [ ] **Step 1: Failing tests:**
  - `a7-setup-add-buttons`: on `bus:master`, Add EQ is disabled with the OD-1 sentence and Add compressor is enabled; on `channel:101`, all are enabled except Add VST3 (`vstStatusLine`). Clear chain is disabled on an empty chain. *Mutation:* enable Add EQ on buses (red).
  - `a7-surface-pills`: `AudioPane.surfaces == ["SHOW","ROUTING","SETUP"]`, and `initialSurface` for an env of `PLUG-INS` resolves to `SETUP`. *Mutation:* keep `PLUG-INS` (red).
  - `a7-setup-target-picker-drives-list`: selecting `bus:aux-1` and adding a Compressor gives the SIGNAL FLOW slots `[("01","Compressor","LIVE")]` and `insertLabel == "Compressor"`.
- [ ] **Step 2: Verify red. Step 3: Implement.**
- [ ] **Step 4: Pixels** at 1360, 1920 and 900 with `COREVIDEO_SHELL_AUDIO_SURFACE=SETUP` (seed: master Compressor+Limiter, `bus:master` selected). Check: two columns, left exactly 340, right capped at 820 at 1920, the VST browser collapsed, no stacked single column. Also check `PLUG-INS` gives SETUP with the disclosure open.
- [ ] **Step 5: Full shell suite, self-check, design lint.**
- [ ] **Step 6: Commit** `feat(mac-audio): SETUP is Windows' two columns; Inserts & plug-ins; VST browser folded into SETUP`.
- [ ] **Step 7: Independent review.**

### Task 6: A7 evidence (read-back, audio proof, quiet-machine performance)

**Files:**
- Modify: `scripts/mac-show-drill.py` (a new `--bus-inserts` flag)
- Modify: `docs/owner-acceptance/mac-audio-phase4.md` (create)

**Interfaces:**
- `--bus-inserts "master=Compressor,Limiter;aux-1=Compressor"` makes the drill send one `sync-audio-routing-matrix` whose `media→master` send (and `media→aux-1`, if listed) carries those `busPluginInserts`. It requires `--media` (fail fast otherwise: without a PCM source the chain never runs, and the gate would be vacuous).

- [ ] **Step 1: Anti-vacuity guards in the drill.** After the matrix, assert `audioRoutingMatrix.busProcessing[busId=="master"].pluginInserts == ["Compressor","Limiter"]` **and** that `busTaps` has `master` with `peakDbfs > -60` (real samples flowed through the chain). Fail with a named reason otherwise. *Mutation:* send the matrix without `busPluginInserts`; the drill must fail on the read-back.
- [ ] **Step 2: Quiet-machine performance gate.** Run `python3 scripts/mac-show-drill.py --load 8 --media --seconds 120` (baseline), then the same with `--bus-inserts "master=Compressor,Limiter;aux-1=Compressor"`. Both must pass the drill's existing per-frame delivery verdicts. From `realtimeEvidence.audio`: `audioLostSamples` delta == 0, `pacerReanchors` delta == 0, and `workMaximumNs` with chains ≤ baseline + 2 ms. Record the hardware, macOS, run length and both tables in the outcomes doc.
- [ ] **Step 3: Audio proof (core, recorded).** With the drill core: record 10 s of program with `--media` and a -6 dBFS 1 kHz tone clip, once without and once with `master=Compressor`. Measure the peak with the bundled ffmpeg (`-af astats`). Expected: the peak with the chain is ≈ -15 dBFS (-18 + 12/4, the fixed bus compressor at `AudioDsp.h:516-537`) ± 1 dB, against ≈ -6 dBFS without it. **This replaces the spec's "master EQ cut in the spectrum" proof, which cannot pass: bus EQ is pass-through in the shared core.**
- [ ] **Step 4: Write the owner steps** (`docs/owner-acceptance/mac-audio-phase4.md` §A7): add a Compressor on a guest channel and see GR move; add a Limiter on master and hear the ceiling; Add EQ is dimmed on the master target with the reason; the workspace and SETUP layouts match the Windows screenshots.
- [ ] **Step 5: Commit** `test(mac-audio): drill --bus-inserts with read-back and taps; A7 audio and performance evidence`.
- [ ] **Step 6: Independent review.**

### Task 7: Guest lip sync

**Files:**
- Create: `M/GuestAvSync.swift` (`GuestAvSyncController`, the flyout view)
- Modify: `M/CoreCommands.swift` (add `setZoomGuestAvSyncOffset`), `M/AppModel.swift` (`applySnapshot` decode, `onConnected`, meeting-state hook), `M/AudioWorkspace.swift` (the rack button)
- Test: `M/ShellTestsAudioPhase4.swift`

**Interfaces:**
- Produces:
  - `CoreCommands.setZoomGuestAvSyncOffset(participantId: String, offsetMs: Int) -> JSONObject` = `["type": "set-zoom-guest-av-sync-offset", "payload": ["participantId": …, "offsetMs": …]]`.
  - `struct GuestAvSyncAck: Equatable { let participantId: String; let offsetMs: Int; let revision: Int64 }` decoded from `response["setting"]`.
  - `@MainActor final class GuestAvSyncController: ObservableObject { init(send: @escaping (String, Int) async throws -> GuestAvSyncAck, selectedId: @escaping () -> String?, perGuestIso: @escaping () -> Bool, zoomLive: @escaping () -> Bool, isZoomGuest: @escaping (String) -> Bool); var canAdjust: Bool; var offsetMs: Int; var label: String; var explanation: String; @Published private(set) var status: String; func refresh(); func clearSession(); func coreGenerationChanged(); func applySnapshot(revision: Int64, settings: [(String, Int)]); func nudgeEarlier() async; func nudgeLater() async; func reset() async; func set(_ ms: Int) async }`
  - `AppModel.guestAvSync: GuestAvSyncController`, wired with `selectedId = { selectedAudioStripId }`, `perGuestIso = { zoomAudioMode == .perGuestIso }`, `zoomLive = { meetingState == "in_meeting" }`, `isZoomGuest = { AudioConsolePolicy.isZoomParticipantChannel($0) && zoomAudioSourceIds.contains($0) }` (OD-4).

Behaviour is a port of `GuestAvSyncController.cs`:
- Steps of ±10 and a clamp of ±200 (on the request and on snapshot values).
- Label: `"Guest sync unavailable"`, `"Audio +N ms later"`, `"Video N ms later"`, `"Lip sync 0 ms"`.
- Explanation: the four Windows sentences verbatim (`:56-61`).
- Status: `"<id>: audio delayed N ms"`, `"<id>: video delayed N ms"`, `"<id>: lip sync reset"`, `"Lip sync not applied: <error>"`.
- A session generation drops a late ack after `clearSession`. Revision gating: `applySnapshot` ignores `revision < lastRevision`, and an ack below `lastRevision` is ignored.
- **Sends are serialised:** each request is chained on the previous `Task`, so a second nudge computes from the first ack (Windows `SemaphoreSlim`).

Mac-specific lifecycle, which the "re-apply after core restart" rule decides here explicitly:
- `clearSession()` when `meetingState` leaves `in_meeting` (Windows `ZoomStatus` hook).
- `coreGenerationChanged()` from `onConnected()`: **clears applied offsets and resets `lastRevision` to -1. It does not re-send.** Reason: the core keys an offset to a participant incarnation and clears them on a new speaker epoch (`MediaCore.cpp:549-559`, `:5651-5668`). A respawned core rejoins as a new incarnation, so re-applying would push an old guest's trim onto whoever holds the id now. Without the reset, the new core's revision restarting at 0 would be ignored forever.
- `applySnapshot` is fed from `applySnapshot(_:)` when `zoomGuestAvSyncRevision` is present.

**UI:** in the rack column under the channel name, a full-width small button labelled `guestAvSync.label`, disabled unless `canAdjust`, with tooltip `explanation`. It opens a popover 290 wide: "GUEST LIP SYNC" (11 bold), the explanation, an "Offset (ms)" stepper field (-200...200, step 10), "Apply offset", a row of "−10" / "+10" / "Reset to 0", and the status (`XAML:1005-1035`).

- [ ] **Step 1: Failing tests** (fixture invariant: selected ids are bare numeric strings present in `zoomAudioSourceIds`; acks echo the requested offset with a revision incremented by the fake):
  - `a9-isolated-clamped-reset`: the Windows `SelectedGuestNudgesAreIsolatedClampedAndReset`, transcribed: sends == `[("101",-10),("202",200),("101",0)]`, and the labels contain `"Video 10 ms"` and `"Audio +200 ms"`. *Mutation:* clamp to 250 (red).
  - `a9-program-mix-and-snapshot-repair`: the Windows `ProgramMixCannotPretendToApplyGuestTrimAndSnapshotRepairsState`, transcribed: 0 sends in ProgramMix, the explanation contains `"program mix"`, a snapshot gives -70, a nudge gives -60, a stale snapshot (revision 1 after ack revision 2) leaves -60, and `clearSession` gives 0. *Mutation:* accept `revision <= lastRevision` snapshots (red).
  - `a9-nudges-serialise`: the fake's first send is held until the test releases it; call `nudgeLater()` twice without awaiting the first; the second send's offset is 20, the final `offsetMs` is 20, and the sends are `[("101",10),("101",20)]`. *Mutation:* no chaining (red: the second send is 10).
  - `a9-new-core-generation-resets-memo`: after an ack at revision 7, `coreGenerationChanged()` then `applySnapshot(revision: 0, [("101",30)])` gives `offsetMs == 30`, and **no send happened** on the generation change. *Mutations:* keep `lastRevision` (red: 0 is ignored); re-send on generation (red: the send count).
  - `a9-not-a-zoom-guest`: selecting `local-machine-audio`, `media`, `zoom-mix` or `capture:cam` each gives `canAdjust == false` and no send (OD-4). *Mutation:* use the Windows predicate only (red on `local-machine-audio`).
  - `a9-wire-shape`: the command sent through `captureRequestForTesting` equals `["type":"set-zoom-guest-av-sync-offset","payload":["participantId":"101","offsetMs":10]]`, and a reply `["setting": ["participantId":"101","offsetMs":10,"revision":3,"speakerEpoch":1]]` decodes.
  - `a9-meeting-end-clears`: `meetingState` goes from `"in_meeting"` to `"ended"` and `offsetMs` becomes 0.
- [ ] **Step 2: Verify red. Step 3: Implement.**
- [ ] **Step 4: Pixels**: SHOW with 101 selected, the button enabled in the seed (PerGuestIso, in a meeting) and disabled with ProgramMix.
- [ ] **Step 5: Full suite. Step 6: Commit** `feat(mac-audio): guest lip sync (port of GuestAvSyncController; generation reset, Zoom guests only)`.
- [ ] **Step 7: Independent review.**

### Task 8: A9 evidence, where the clap skew moves by the offset

**Files:** Modify `scripts/validate-av-clap.mjs` (add `--guest-av-offset-ms <n>`).

- [ ] **Step 1:** Add the flag. After the fake participant with the clap is live, send `set-zoom-guest-av-sync-offset` for that participant id. Fail if the reply's `offsetMs` differs.
- [ ] **Step 2:** On the Mac build (`--build-dir native/build-metal`, `COREVIDEO_FAKE_CLAP_MS` as in the script's header), run once with offset 0 and once with `+40`. Expected: `skew(+40) − skew(0) = −40 ms ± 17 ms` (video minus audio; delaying audio by 40 lowers the skew by 40; ±1 frame at 60 fps). If the script cannot run unmodified on the Mac core, port only what fails and say so in the outcomes doc. If it still cannot run, record the result as **unverified** and lean on the platform-neutral `GuestAvSyncTest` (confirmatory only).
- [ ] **Step 3:** Owner step: on a real guest, nudge until the lips match and confirm it is cleared when the guest rejoins.
- [ ] **Step 4: Commit** `test(av-clap): --guest-av-offset-ms; Mac guest lip-sync skew evidence`. **Step 5: Independent review.**

### Task 9: A8 spike on the signed bundle (timebox 2 days)

**Why a spike:** each of the following decides the native design, and none can be learned from headers or a simulator. The master plan rule applies: "system-level behaviour is proven on the installed build".

**Setup.** A throwaway Objective-C++ CLI, `tap-probe`, lives on branch `spike/process-tap` and is never merged. It is built with `clang++ -fobjc-arc -framework CoreAudio -framework AudioToolbox -framework Foundation` and signed exactly like the core (`--identifier us.iamfatness.corevideopro.tap-probe --entitlements scripts/macos-core.entitlements`, hardened runtime, Developer ID). It is placed at `CoreVideo Pro.app/Contents/Resources/tap-probe`. The shell spawns it the way it spawns the core (`Process()`, `M/MediaCoreBridge.swift:168`) through a spike-only env `COREVIDEO_SHELL_TAP_PROBE=<args>`. The probe creates a global stereo tap that excludes listed process objects, wraps it in a private aggregate (main subdevice = default output, tap drift compensation on), reads it with **both** AUHAL input (the existing adapter's path) and an `AudioDeviceIOProc`, and prints JSON lines: status codes, format, peak per 100 ms, a count of all-zero buffers, and every property-listener event.

**Questions, with what counts as an answer (record each verbatim in the outcomes doc, with OS build 27.2.1 (26B131) and the hardware):**

| # | Question | Answer needed |
|---|---|---|
| Q1 | Which app does the TCC prompt name when a shell-spawned child creates the tap? Is the grant keyed to the shell? | Prompt text (screenshot); `tccutil reset AudioCapture us.iamfatness.corevideopro.shell` resets it (or not). |
| Q2 | Where must `NSAudioCaptureUsageDescription` live? | Try: shell Info.plist only; neither (expect a crash or silent denial, record which). A bare executable cannot carry one unless it is embedded (`-sectcreate __TEXT __info_plist`); try that only if the shell plist is not enough. |
| Q3 | Is any entitlement needed under the hardened runtime, and does notarization accept it? | Run without `device.audio-input`; submit one notarization. |
| Q4 | When is the prompt shown: at `AudioHardwareCreateProcessTap`, at aggregate creation, or at the first IO start? What do denial and not-yet-answered look like? | The `OSStatus` of each call and the sample content (all-zero or not) in each state. **Decides** whether "denied" is detectable; the plan assumes it is not (silent zeros). |
| Q5 | Does AUHAL input on the private aggregate deliver the tap's stereo at the aggregate's nominal rate? | If not, Task 10 uses `AudioDeviceCreateIOProcID` instead of AUHAL. |
| Q6 | Process exclusion: do process objects exist for the core (bare executable), the engine `.app` and the shell? What `kAudioProcessPropertyBundleID` does each report? Does a process that starts audio **after** tap creation get tapped? Does setting `kAudioTapPropertyDescription` on a live tap apply a new exclusion list without rebuilding the aggregate? On macOS 26+, does `bundleIDs` plus `processRestoreEnabled` exclude a respawned engine? | A table per process. Decides "rebuild" versus "update in place". |
| Q7 | Does the Zoom engine produce output audio in a meeting (fake engine, then a real test meeting)? | `kAudioProcessPropertyIsRunningOutput` on the engine's process object. If yes, the engine must be excluded or every guest doubles. |
| Q8 | What happens on a default-output change (built-in to USB/AirPods), a nominal-rate change (48 → 44.1 kHz in Audio MIDI Setup), and `sudo killall coreaudiod`? | The listener events seen, whether the aggregate keeps delivering, and at what rate. |
| Q9 | Teardown: after `kill -9` of the probe, does any aggregate or tap remain (`system_profiler SPAudioDataType`, Audio MIDI Setup)? Is any aggregate visible to another app while the probe runs (check the Zoom.app microphone menu)? | Must be "none" and "not visible" for a private aggregate. |
| Q10 | Cost: wall time of create + start for tap and aggregate (n=20), and the effect of the tap's `muteBehavior` (must stay `CATapUnmuted`: the operator still hears the system). | p50/p95 ms. Sets the "starting" timeout in Task 10. |

**Exit criteria.** A short spike section in the phase outcomes doc that answers Q1-Q10 and updates the design decisions marked **(spike)** in Task 10. **If Q1/Q2 show the grant cannot be attributed to the shell from a child process,** stop and bring it to the owner with two options: create the tap in the core but launch the core through `NSWorkspace`/`open` as its own bundle, or create a public aggregate in the shell (visible to other apps; leaks on crash). Do not pick one alone.

- [ ] **Step 1:** Build and sign the probe; run Q1-Q10; save the logs and screenshots under `artifacts/a8-spike/` (gitignored, path listed in the outcomes doc).
- [ ] **Step 2:** Write the outcomes section; mark each Task 10 **(spike)** item confirmed or changed.
- [ ] **Step 3: Independent review** of the findings (a reviewer re-runs Q4 and Q6 from the logs).

### Task 10: Native system-audio tap source

**Files:**
- Create: `N/modules/ProcessTapPolicy.h` (platform-neutral: exclusions, rebuild decisions, strings, debounce)
- Create: `N/modules/CoreAudioProcessTap.h` (the `IProcessTapPlatform` seam), `N/modules/CoreAudioProcessTap.mm` (the real platform, `@available(macOS 14.2, *)`)
- Modify: `N/modules/CoreAudioCaptureSourceAdapter.cpp` (the `coreaudio-tap-loopback` kind; a factory overload taking the platform)
- Modify: `N/modules/Interfaces.h:1238-1245` (`virtual bool systemAudioTapSupported() const { return false; }`), `N/modules/Interfaces.h` (`CaptureAudioSourceMetrics::digitalSilence`)
- Modify: `N/core/CoreStateCapture.cpp:1747-1880` (`captureAudioSources.systemAudioTapSupported`, per-row `captureDigitalSilence`)
- Modify: `native/CMakeLists.txt` (the `.mm` under `APPLE AND COREVIDEO_WITH_COREAUDIO`, `-framework Foundation`; the new test file in every build)
- Test: create `native/tests/ProcessTapPolicyTest.cpp`; modify `native/tests/CoreAudioAdapterTest.cpp`

**Interfaces:**
- Produces (C++):

```cpp
// ProcessTapPolicy.h — no OS headers
namespace corevideo::modules::tap {
inline constexpr std::string_view kKind = "coreaudio-tap-loopback";
inline constexpr std::string_view kDeviceId = "system-audio";
inline constexpr std::string_view kOwnBundlePrefix = "us.iamfatness.corevideopro";
struct Process { uint32_t objectId; int32_t pid; std::string bundleId; };
struct Exclusions { std::vector<uint32_t> objectIds; std::vector<std::string> bundleIds; };
enum class Event { DefaultOutputChanged, SampleRateChanged, ProcessListChanged, ServiceRestarted };
Exclusions exclusionsFor(const std::vector<Process>&, int32_t selfPid, int32_t parentPid, bool bundleIdsSupported);
bool exclusionsChanged(const Exclusions& current, const Exclusions& next);   // order-insensitive
class Debouncer { public: Debouncer(std::function<int64_t()> nowMs, int64_t quietMs); void note(); bool due(); };
std::string unsupportedMessage();     // "System audio needs macOS 14.2 or later. Choose a loopback device such as BlackHole instead."
std::string startingMessage();        // "Starting system audio"
std::string buildFailedMessage(int32_t status);   // "System audio could not start (OSStatus <n>)."
}
// CoreAudioProcessTap.h
struct TapBuild { bool ok = false; std::string aggregateUid; int sampleRate = 0; int32_t status = 0; };
class IProcessTapPlatform {
 public:
  virtual ~IProcessTapPlatform() = default;
  virtual bool supported() const = 0;                       // runtime macOS >= 14.2
  virtual bool bundleIdExclusionSupported() const = 0;      // runtime macOS >= 26
  virtual std::vector<tap::Process> processes() = 0;
  virtual TapBuild build(const tap::Exclusions&) = 0;       // tap + private aggregate (+ drift compensation)
  virtual void destroy() = 0;
  virtual void setListener(std::function<void(tap::Event)>) = 0;
};
std::unique_ptr<IProcessTapPlatform> createCoreAudioProcessTapPlatform();   // nullptr off Apple/COREAUDIO
std::unique_ptr<IAudioCaptureSource> createCoreAudioCaptureSource(std::unique_ptr<IProcessTapPlatform> tap);
```

- Consumes: the existing AUHAL open path (`CoreAudioCaptureSourceAdapter.cpp:250-340`), or an IOProc reader if Q5 says AUHAL does not work **(spike)**.

**Behaviour:**
- A source with `audioSourceKind == kKind` and `audioDeviceId == kDeviceId` takes the tap path. Any other loopback kind keeps today's behaviour, including `LoopbackDefaultSentinelFailsWithVirtualDeviceExplanation`.
- `configure()` **never** calls the platform's `build`. It creates the source row (`streaming=false`, `warning=startingMessage()`) and posts a build to a single `SystemAudioTapWorker` thread owned by the adapter. The worker computes `exclusionsFor(processes(), getpid(), getppid(), bundleIdExclusionSupported())`, builds, then opens the reader at `TapBuild::sampleRate`. The row's endpoint fields become `endpointId = "system-audio"` and `endpointName = "System audio (all apps)"`.
- On `!supported()`: no worker. The row's `lastError` and `warning` are `unsupportedMessage()`.
- Listener events go to the worker. `SampleRateChanged`, `DefaultOutputChanged` and `ServiceRestarted` rebuild after the debouncer is `due()` (quiet 250 ms, injected clock). `ProcessListChanged` rebuilds (or updates in place, per Q6 **(spike)**) only when `exclusionsChanged(...)`. Frames from a superseded build are dropped by a build generation, never relabelled.
- `digitalSilence` is true while `framesReceived > 0` and no sample since this build was non-zero (checked in the callback; no clock).
- `stopAll()` and the destructor stop the reader **outside** the adapter mutex (the existing rule at `:418-433`), then `destroy()` on the worker, then join the worker.
- `systemAudioTapSupported()` returns `tap && tap->supported()`.

- [ ] **Step 1: Failing tests, `ProcessTapPolicyTest.cpp`** (every build, no OS):
  - `ProcessTapPolicy.ExcludesSelfParentAndOwnBundles`: processes `{1,self,""}, {2,parent,"us.iamfatness.corevideopro.shell"}, {3,999,"us.iamfatness.corevideopro.zoom-engine"}, {4,500,"com.google.Chrome"}, {5,501,"us.zoom.xos"}` give `objectIds == {1,2,3}` (Zoom.app **not** excluded, OD-5). With `bundleIdsSupported`, `bundleIds == {"us.iamfatness.corevideopro.shell","us.iamfatness.corevideopro.zoom-engine"}`. *Mutations:* drop the parent (red); a prefix match without the dot boundary (add `{6,7,"us.iamfatness.corevideoprofake"}` → must not be excluded) (red).
  - `ProcessTapPolicy.ExclusionsChangedIsOrderInsensitive`.
  - `ProcessTapPolicy.DebouncerWaitsForQuiet`: fake clock; note at 0, 100, 200; `due()` is false at 400, true at 451. *Mutation:* measure from the first note (red at 400).
- [ ] **Step 2: Failing tests, `CoreAudioAdapterTest.cpp`** (gated like the existing file), using `FakeTapPlatform` (records calls, a configurable `TapBuild`, a `std::latch` to hold `build`, fires listener events on demand). The fake reader is the platform's: `build` returns an `aggregateUid` naming a fake that the adapter's reader seam accepts. If Q5 keeps AUHAL, add a reader seam `ITapReader` so the test does not open real hardware.
  - `ProcessTap.ConfigureReturnsWhileBuildIsBlocked`: hold `build` on the latch; `configure({system-audio})` returns; `metrics()[0]` is `streaming=false` with `warning == "Starting system audio"`; release; within 2 s (polling on the fake clock's condition, not a sleep) the row is streaming. *Mutation:* build inside `configure` (the test deadlocks on the latch: run it with a 5 s gtest timeout and treat a timeout as red).
  - `ProcessTap.UnsupportedGivesTruthfulRow`: `supported()=false`; the row's `lastError == unsupportedMessage()` and `build` is never called.
  - `ProcessTap.RateChangeRebuildsAndRelabels`: build at 48000, deliver frames, fire `SampleRateChanged`, the next build returns 44100; after the debounce, every frame from the new build has `sampleRate == 44100` and **no** frame stamped 48000 arrives after the event. *Mutation:* relabel without rebuilding (red: a 44100-labelled frame from the old generation).
  - `ProcessTap.DefaultOutputChangeRebuildsOnceAfterQuiet`: three events 50 ms apart give exactly one rebuild.
  - `ProcessTap.EngineAppearingRebuildsExclusions`: `ProcessListChanged` with a new `us.iamfatness.corevideopro.zoom-engine` object gives one rebuild whose exclusions contain it; an unrelated new process gives none.
  - `ProcessTap.DigitalSilenceFlag`: all-zero frames give `digitalSilence=true`; one non-zero sample makes it false for that build.
  - `ProcessTap.OtherLoopbackKindsUnchanged`: `wasapi-loopback` with `default-render` still fails with the BlackHole explanation (the existing test, kept green).
  - `CoreStateCapture`-level (stub core with the fake adapter injected): `captureAudioSources.systemAudioTapSupported == true` and the row has `captureDigitalSilence`; **`monitorFeedbackRisk` stays false** with the tap source configured and the monitor on the default device. *Mutation:* report `endpointId` as the default output UID (red: the guard fires, proving that the guard and the exclusion are separate mechanisms).
- [ ] **Step 3: Verify red** (Metal build): `cmake -S native -B native/build-metal -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOREVIDEO_STUB=OFF -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DCOREVIDEO_WITH_METAL=ON -DCOREVIDEO_WITH_AVF_ENCODER=ON -DCOREVIDEO_WITH_COREAUDIO=ON && cmake --build native/build-metal -j 6 --target corevideo-native corevideo-native-tests && native/build-metal/corevideo-native-tests --gtest_filter='ProcessTap*:CoreAudioAdapters.*'`.
- [ ] **Step 4: Implement** policy, then platform, then adapter. `CoreAudioProcessTap.mm`: `CATapDescription initStereoGlobalTapButExcludeProcesses:` (plus `bundleIDs`/`processRestoreEnabled` under `@available(macOS 26.0, *)`), `privateTap = YES`, `muteBehavior = CATapUnmuted`; the aggregate dictionary with `kAudioAggregateDeviceIsPrivateKey = 1`, `kAudioAggregateDeviceTapListKey` = `[{kAudioSubTapUIDKey: tapUUID, kAudioSubTapDriftCompensationKey: 1}]`, `kAudioAggregateDeviceTapAutoStartKey = 1`, main subdevice = the default output UID **(spike Q5/Q8 may change the main-subdevice choice)**; listeners on `kAudioHardwarePropertyDefaultOutputDevice`, the aggregate's `kAudioDevicePropertyNominalSampleRate`, `kAudioHardwarePropertyProcessObjectList`, `kAudioHardwarePropertyServiceRestarted`, delivered to the worker queue (never the audio worker).
- [ ] **Step 5: Verify green.** Run the full Metal suite `-LE realtime`, the stub build (`cmake -S native -B native/build-stub -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build native/build-stub -j 4 && native/build-stub/corevideo-native-tests`; the policy tests must run there too), and `npm run contract:check` (new nested fields).
- [ ] **Step 6: Commit** `feat(native,mac): system audio through a Core Audio process tap, built off coreMutex, rebuilt on device/rate/process change`.
- [ ] **Step 7: Independent review**, including a reviewer pass over threading: no `coreMutex` or audio-worker lock is taken on the worker or listener paths.

### Task 11: Shell System audio entry, status, hint and Info.plist

**Files:**
- Modify: `M/LocalAudioSource.swift` (policy and card), `M/AppModel.swift` (snapshot decode of `systemAudioTapSupported`), `scripts/make-macos-shell-bundle.sh:130-134` (`NSAudioCaptureUsageDescription`, wording from the spike)
- Test: `M/ShellTestsAudioPhase4.swift`, plus the `ShellPrefs` self-check

**Interfaces:**
- Produces: `LocalAudioPolicy.systemAudioId = "system-audio"`, `systemAudioKind = "coreaudio-tap-loopback"`, `systemAudioName = "System audio (all apps)"`. `entry(_:devices:)` returns `["captureDeviceId":"local-machine-audio","audioDeviceId":"system-audio","audioDeviceName":"System audio (all apps)","audioSourceKind":"coreaudio-tap-loopback","nativeAudioDeviceId":"system-audio"]` when the setting is enabled with `deviceId == systemAudioId`, **independent of `audioInputs` and of the capability flag**. `AppModel.systemAudioTapSupported: Bool?` (`nil` until a snapshot reports it). `LocalAudioPolicy.status(...)` keeps the four Windows states and adds: when the row has `captureDigitalSilence == true` and the kind is the tap, `"No audio detected. If audio is playing, allow CoreVideo Pro in System Settings > Privacy & Security > <pane from spike Q1>."`; when the row's `lastError` is the unsupported message, that message.
- The picker's first item is "System audio (all apps)", disabled when `systemAudioTapSupported == false`, with the help text set to the unsupported message. Selecting it never auto-enables the source (OD-6). The default selection is unchanged: Windows prefers loopback (`W/Services/AudioCaptureDeviceDiscoveryService.cs:60-75`), but the Mac does not, because it would raise the TCC prompt unasked.
- Persisted through the existing `localAudioDeviceId`/`localAudioDeviceName`. No new prefs key.

- [ ] **Step 1: Failing tests:**
  - `a8-entry-shape`: enabled with `system-audio` gives the exact entry above with `audioInputs` empty. *Mutation:* require the device to be in `audioInputs` (red).
  - `a8-first-connect-sends-system-audio`: prefs restore `{enabled, "system-audio"}`, the `onConnected()` reassert batch (`audioReassertObserver`) has the tap entry in `sync-capture-audio-sources` before any snapshot, and `local-machine-audio` has a strip and five default sends in the routing command. *Mutations:* gate on `systemAudioTapSupported == true` (red: nil at first connect); gate on `audioInputs` (red).
  - `a8-status-hint-and-unsupported`: row fixtures (tap kind; `captureStreaming` true and false; `captureDigitalSilence` true and false; `lastError` unsupported) map to the expected strings, and every combination is listed (plan-authoring rule 3). An input-device row with `captureDigitalSilence` shows **no** permission hint. *Mutation:* show the hint for any kind (red).
  - `a8-picker-disabled-when-unsupported`: `systemAudioTapSupported=false` gives the item disabled with the help text; `nil` gives it enabled.
  - `a8-bundle-plist-has-usage`: a script test (`npm run test:scripts` suite) asserts `make-macos-shell-bundle.sh` writes `NSAudioCaptureUsageDescription`. *Mutation:* remove the key (red).
- [ ] **Step 2: Verify red. Step 3: Implement. Step 4: Pixels** of SETUP's left column with the System audio item selected and the hint shown (fixture row).
- [ ] **Step 5: Full suite, self-check, `npm run test:scripts`.**
- [ ] **Step 6: Commit** `feat(mac-audio): System audio as a local audio source; capability, hint, usage string`.
- [ ] **Step 7: Independent review.**

### Task 12: A8 evidence on the installed, signed build

Everything here runs on the **installed Developer ID build** from `make-macos-shell-bundle.sh` (with `COREVIDEO_APP_PROFILE`), not on a harness. The owner's app must not be live; use a test install, or follow the owner's restart permission in CLAUDE.md.

- [ ] **Step 1: Permission flow.** Run `tccutil reset AudioCapture us.iamfatness.corevideopro.shell`, then enable System audio. The prompt names CoreVideo Pro (screenshot). Deny: the card shows the hint within 3 s of streaming. Allow (System Settings): the card shows "Audio detected" while a browser plays.
- [ ] **Step 2: Level and pitch.** Play a -20 dBFS 1 kHz WAV with `afplay` (generated by a Python one-liner into scratch). Record 10 s of program with `local-machine-audio → master` only. Recorded peak = -20 ± 1 dBFS; FFT peak 1000 ± 5 Hz. Repeat after switching the default output between built-in speakers (48 kHz) and a 44.1 kHz device mid-recording: pitch still 1000 ± 5 Hz, `captureSampleRate` follows, and the gap is ≤ 500 ms.
- [ ] **Step 3: No feedback.** Monitor on (the default device), local source routed to master, a media clip tone on Program, **no other app playing**: the local source row shows `captureDigitalSilence == true` for 30 s, and master loudness does not climb. Then play the browser and see the local source signal present. This proves the core's monitor output is excluded.
- [ ] **Step 4: Teardown.** `kill -9` the core while capturing. After the respawn the source is back (Review Focus 1), and `system_profiler SPAudioDataType` lists no orphaned aggregate. While capturing, Zoom.app's microphone menu lists no CoreVideo aggregate.
- [ ] **Step 5: Quiet-machine performance.** Run `mac-show-drill.py --load 8 --seconds 120` with and without the tap source enabled (a new `--system-audio` flag that sends the tap entry and plays nothing). Pass criteria: the drill's per-frame verdicts pass, `realtimeEvidence.audio.audioLostSamples` delta 0, `pacerReanchors` delta 0, `workMaximumNs` ≤ baseline + 2 ms, and no `[cmd] 'sync-capture-audio-sources' ≥10ms` line in `media-core.log`.
- [ ] **Step 6:** Owner steps in `docs/owner-acceptance/mac-audio-phase4.md` §A8 (the browser into the program mix in a real show; check that Zoom's own audio is not doubled when the real Zoom.app is also running, per OD-5).
- [ ] **Step 7: Commit** the drill flag, the evidence tables and the outcomes doc. **Step 8: Independent review.**

---

## Owner decisions

Defaults are taken so work can proceed. Each is restated in the outcomes doc.

| # | Decision | Default taken | Why |
|---|---|---|---|
| OD-1 | Bus EQ and gate are pass-through in the shared core. What does the Mac offer on a bus target? | Dim "Add EQ" on bus targets with the reason; slot state `PASS`. | Honest reporting. The alternative is a shared native change: give `applyBusInsertChain` the channel DSP with per-bus settings. That fixes Windows too but touches Windows audio, so it gets its own issue. |
| OD-2 | Fold the Mac-only `PLUG-INS` pill into SETUP as Windows' collapsed browser? | Yes. | Matches the Windows layout. VST is scan-only on the Mac until V1-V3. |
| OD-3 | SHOW as a fixed viewport with no page scroll; Audio tab width uncapped (SETUP right column 820 max)? | Yes. | `XAML:31-36` gives the reason: the page scroll made the console jump on every snapshot. |
| OD-4 | Lip sync only on real Zoom guest strips (Windows also enables it on local/media strips, and the core then silently retires those offsets)? | Mac: Zoom guests only. File a Windows issue. | The Windows control looks applied and then reverts. |
| OD-5 | Which processes does the system-audio tap exclude? | All CoreVideo Pro processes (shell, core, Zoom engine, plug-in host) and nothing else. The operator's own Zoom.app **is** captured. | Excluding the core is a correctness need (monitor feedback). Excluding other apps is a product choice. |
| OD-6 | Auto-select System audio as the default local source (Windows defaults to loopback)? | No. | It would raise the TCC prompt unasked. |
| OD-7 | System audio below macOS 14.2 (app floor is 13.0)? | Entry disabled with BlackHole guidance (the spec's OD-4 confirmed). | No tap API below 14.2. |
| OD-8 | Use private TCC SPI to detect "denied"? | No. One combined hint (nothing playing, or not allowed). | Notarization and stability risk. |
| OD-9 | Windows bugs found here (dead EQ presence sliders in the slot flyout; lip sync on non-guest strips; bus EQ shows `LIVE`). | File three `backlog` issues with owner approval; do not fix them in this Mac plan. | Out of the Mac lane. |

## Top risks

1. **TCC attribution from the helper core** (Q1/Q2). If the grant cannot go to the shell, the design needs an owner decision on how the core is launched. This blocks Tasks 10-12, not A7/A9.
2. **"Denied" may be indistinguishable from "nothing playing"** (Q4). The operator gets a combined hint; a silent show with a missing grant is still possible. Mitigation: the hint appears whenever System audio has never carried a sample.
3. **Exclusion races.** A CoreVideo process that starts audio after the tap is built is captured until the rebuild. On macOS 14.2-25 the window is one debounce (250 ms) after `ProcessListChanged`. On 26+, `bundleIDs` closes it. Q6/Q7 size it.
4. **The bus compressor is a crude sample-wise curve with no settings** (`AudioDsp.h:516-537`), and on master it sits before mastering. Operators may expect the channel compressor's controls. The SETUP list shows that it is fixed (tooltip), and OD-1's issue covers the real fix.
5. **The layout rework touches the whole Audio tab** (SHOW, SETUP, the pop-out at 900 pt). Mitigation: pixels at three widths per task, a review against XAML line ranges, and the full shell suite on each commit.
6. **AUHAL on a tap aggregate** (Q5). If it does not work, the IOProc reader adds a second capture path to maintain.

## Where the master plan and spec were wrong

- **Spec §4.7 and §6(c), "master EQ cut visible in the recording's spectrum":** it cannot pass. `applyBusInsertChain` processes only `limiter` and `compressor` with fixed parameters; EQ and gate pass through on every bus, on both platforms. Task 6 proves the master chain with the fixed compressor instead.
- **Spec §4.7 UNVERIFIED insert order:** resolved. Channel and bus chains run in list order. The Mac's fixed gate→EQ→comp order is a divergence from Windows (append order), and Task 2 fixes it.
- **Spec §4.7 "curve vectors taken from the Windows tests":** none exist. The vectors come from compiling the Windows file on this Mac (Task 1).
- **Spec §4.8 and master plan A9 (M):** the capture nudge already shipped as Z2, on Sources camera rows and session-only, which matches Windows and was owner-confirmed. It is not on the pairing row and it is not persisted. A9 is guest lip sync only; resized to M including evidence.
- **Spec §4.3 "configure() already runs off the audio worker":** true, but it runs under `coreMutex` inside `applyCommands`. Building a tap there would stall render, so it must be asynchronous (Task 10).
- **Spec §4.3 "the tap can exclude our own processes":** only by audio process object. Those exist only for processes connected to coreaudiod. The engine's pid is hidden behind the child guard and changes on respawn, and bundle-ID exclusion is macOS 26+. The plan excludes by bundle-ID prefix over the process list and rebuilds on change.
- **Spec §4.3 fallback "create the tap in the shell and pass the aggregate UID to the core":** this fails with a private tap or aggregate, which is visible only to its creator. It needs a public aggregate that appears in every app's device menu and leaks on a crash.
- **Spec §4.3 "tap created but silent (the Windows 'loopback idle' sentence)":** that Windows state means "no packets". A tap delivers zeros, so the state never occurs, and denial probably looks the same. Replaced by `captureDigitalSilence` plus one hint.
- **The tap API is Objective-C only** (`AudioHardwareTapping.h` is `#ifdef __OBJC__`). The adapter is `.cpp`, so a new `.mm` is required. The spec did not say so.
- **The monitor feedback guard cannot protect a tap source:** it compares endpoint IDs, and a private aggregate's UID never equals the monitor endpoint. The exclusion list is the only protection, and Task 10 pins it with a test.
- **Layout:** the Mac SETUP is one stacked column (Windows has two), SHOW scrolls inside the page (Windows is a fixed viewport), and the channel chip hard-codes "Master". The spec's A7 row listed controls, not this layout.

## Self-review (done before handing over)

- Spec coverage: §2.6 → Tasks 2-5; §2.7 guest sync → Task 7 (nudge already done); §4.3 system audio → Tasks 9-12; §6 audio-with-audio → Tasks 6, 8, 12; §6 drills → Tasks 6, 12; §7 risks → the spike's Q list.
- Type names are used consistently across tasks: `ChannelInserts.chain`, `InsertChainPolicy`, `InsertSlot`, `AudioProcessingTarget(Policy)`, `selectAudioStrip`, `busInserts`, `GuestAvSyncController`, `tap::Exclusions`, `IProcessTapPlatform`, `captureDigitalSilence`, `systemAudioTapSupported`.
- Every assertion has a fixture-derived operand: Windows vectors, Windows strings, literal ids and counts. Confirmatory assertions are labelled (`a7-chain-reasserted-on-new-core`; the `GuestAvSyncTest` fallback in Task 8).
- Every new contract has a conforming fixture: the bus-chain read-back in the drill; `FakeTapPlatform` that honours the async build and fires events; a lip-sync fake whose acks carry revisions.
