# Audio parity on macOS: topology, local sources, mastering, buses, meters, processing, VST3

Status: DRAFT design for owner review, 2026-11-15.
Covers gap rows **2.9** and **2.12** of `docs/superpowers/audits/2026-11-15-mac-ui-parity-consolidated.md`,
the audio half of **2.13** (the capture audio-sync nudge), and every row of
`docs/superpowers/audits/2026-11-15-mac-ui-parity-audio.md` that is still open after the Tier 0
audio fixes (0.1 to 0.5, 0.12) that are already on this branch. Roadmap: `docs/superpowers/plans/2026-11-14-mac-parity-1.0-roadmap.md`
(no audio milestone exists there yet; this spec proposes one, "M-Audio", in lane S with one late lane E slice).

Path shorthand: `W/` = `native-shell/CoreVideoPro.WinUI/`, `M/` = `mac-shell/Sources/CoreVideoProShell/`,
`N/` = `native/src/`, `SVM` = `W/ViewModels/StudioViewModel.cs`. Every `M/` and `N/` line below was
re-read in this worktree on 2026-11-15. `AudioPage.xaml` lines marked (audit) are carried from the
row-level audit and were not re-opened.

## 1. Why

A Mac operator can now hear, meter and mix a Zoom meeting, but still cannot do the following,
all of which the Windows 1.0 console does:

- Choose between per-guest ISO stems and Zoom's own program mix. The Mac is fixed in the per-guest
  topology, so an A1 who wants Zoom's echo-cancelled continuous mix on air has to hand-build it in
  the routing grid on every launch.
- Put the host's own microphone, or the Mac's system audio, on the console without first pairing it
  to a capture card slot.
- Keep a mastering chain. The rack resets to bypass on every relaunch, and half of its controls
  (low-pass, the glue compressor's ratio/attack/release/makeup/multiband, the limiter toggle,
  presets, A/B) do not exist.
- Check delivery loudness. There is no integrated LUFS or true-peak guide meter, and the console's
  master rail draws the same mono level twice.
- Use ISO buses 2 to 8, the two aux buses, custom buses, bus sends, or bus LISTEN.
- See or reorder an insert chain, process a bus, read compressor gain reduction, tell "muted in
  Zoom" from "muted by me", or trim a guest's lip sync.
- Load any VST3 plug-in. The Mac can list installed plug-ins and nothing more.

The A1 notices all of these in the first ten minutes of a show. The mastering reset is the worst:
it is silent, and it changes what goes to air.

## 2. Windows behaviour to match

### 2.1 Zoom audio topology
- Two modes, `PerGuestIso` (product default) and `ProgramMix` (`W/Models/ProductionModels.cs:230-236`,
  `SVM:928-934`). `SetZoomAudioMode` sets a status line that names the topology (`SVM:936-947`).
- The mode only changes which **default sends are synthesised on every sync**; it never writes the
  matrix (`SVM:9625-9690`, `EnsureDefaultZoomAudioRoutingSends`). PerGuestIso: each guest id gets
  master, pgm-l, pgm-r, stream, mon at 0 dB, and any `zoom-mix` send to those five buses is removed
  (the doubling guard: stems plus the mix double every voice at about 110 ms skew). ProgramMix:
  `zoom-mix` gets the five sends and stems get none, but a stem send the operator routed
  deliberately survives.
- Routing rows mirror it: stems are `DefaultUnrouted` in ProgramMix, `zoom-mix` is `DefaultUnrouted`
  in PerGuestIso (`SVM:4555-4590`).
- Persisted as a string, `"programMix"` / `"perGuestIso"`; anything unreadable parses to
  `PerGuestIso` (`W/Services/ZoomAudioModePreference.cs:18-38`,
  `W/Services/ProductionOutputPreferencesStore.cs:205-207`). Restore says the topology out loud
  (`SVM:12398-12410`).
- Tests: `ZoomAudioModePreferenceTests` (five facts), `StudioViewModelAudioStatusTests`
  `EnsureDefaultZoomAudioRoutingSends_RoutesTheMeetingMixNotIsoParticipants` (`:2590`) and
  `..._PerGuestIsoRoutesStemsAndDropsTheMixFromProgram` (`:2615`).
- Control action `audio.zoomMode.set` (`W/Services/StudioControlSurface.cs:638-652`).

### 2.2 Local audio source
- One optional source with the fixed id `local-machine-audio`: an enable toggle and a device picker
  that lists inputs and loopback endpoints (`SVM:3580-3606`, `AudioPage.xaml:158-176` (audit)).
  The default device prefers a loopback endpoint, then an input
  (`W/Services/AudioCaptureDeviceDiscoveryService.cs:60-75`).
- It rides `sync-capture-audio-sources` as one more entry with `captureDeviceId:
  "local-machine-audio"` (`SVM:9193-9207`), gets a routing row (`SVM:4559-4564`) and the capture
  default sends (`SVM:9209-9212`).
- Status and recommendation text are derived from the measured source row, including "loopback
  idle - play audio through this output or choose an input" (`SVM:8456-8580`).
- Persisted: `LocalAudioSourceEnabled`, `SelectedLocalAudioCaptureDeviceId`
  (`ProductionOutputPreferencesStore.cs:117-118`). Test: `AudioCaptureDeviceDiscoveryServiceTests`.

### 2.3 Mastering rack
- 21 persisted fields (`ProductionOutputPreferencesStore.cs:240-262`): enabled, target index
  (-14 / -16 / -23), glue amount, ceiling, max ride, input gain, high-pass, low-pass, low shelf,
  presence, high shelf, width, limiter enabled, glue ratio / attack / release / makeup, glue
  multiband and its three band trims.
- Persisted as current, compare A, compare B, the active compare slot, and user presets
  (`:225-232`). Built-in starting points live in `W/Models/MasteringPresetCatalog.cs`; user presets
  in `W/Models/MasteringPresetLibrary.cs`. Tests: `MasteringPresetCatalogTests`,
  `MasteringPresetLibraryTests`.
- The core reads every one of those keys from the `mastering` object of
  `sync-participant-audio-mix` (`N/core/MediaCore.cpp:2944-2965`), and `limiterEnabled` from the
  command itself (`:2938`). Core proof: `native/tests/AudioMasteringTest.cpp`.

### 2.4 Buses
- 15 fixed buses: master, pgm-l, pgm-r, iso-1..iso-8, mon, stream, aux-1, aux-2
  (`W/ViewModels/AudioRoutingMatrixViewModel.cs:84-101`); ISO columns are exclusive (`:432-444`).
- Removable buses (aux and custom `bus-*`) can send to master / mon / stream with a gain, and any
  bus can be auditioned with LISTEN, exclusive (`:476-499`). Test: `RoutingMatrixViewModelTests`.
- Core: `isAudioRoutingBus` accepts the 15 plus any `bus-*` (`MediaCore.cpp:3228-3236`); `busSends`
  entries are `{fromBusId, toBusId, gainDb}`, accepted only from `aux-*` / `bus-*` to master,
  pgm-l, pgm-r, stream, mon (`:3362-3383`); `monitorBusId` selects the LISTEN bus (`:3385-3388`).

### 2.5 Meters
- Integrated LUFS and true-peak guide meters with target and ceiling markers
  (`AudioPage.xaml:359-375`, `W/Controls/MasteringGuideMeter.xaml.cs`), fed by
  `audioMixSession.masterMeter.integratedLufs` / `truePeakDbfs` (`SVM:1313-1325`).
- Master L and R on the rail (`AudioPage.xaml:1466`), from the pgm-l / pgm-r bus taps.
- Strip meters on a calibrated dBFS scale with pre-mute input level while muted
  (`W/Models/AudioMeterScale.cs`; tests `AudioMeterScaleTests`, `AudioMeterBallisticsTests`).
- Compressor gain-reduction meter and the ZOOM MUTED badge (`AudioPage.xaml:1397-1398`).
- Core: `masterMeterState` publishes `momentaryLufs`, `shortTermLufs`, `integratedLufs`,
  `truePeakDbfs`, `windowMs` (`N/core/CoreStateCapture.cpp:2633-2641`); per-channel
  `inputPeakDbfs` and `gainReductionDb` (`:1517-1525`); per-bus `busTaps` with `peakDbfs`
  (`MediaCore.cpp:6538-6544`). Core tests: `AudioDspTest` `IntegratedLoudness*`, `TruePeak*`.

### 2.6 Processing workspace
- Insert chain list per target, "+ Add processing", a target picker (channel, bus, aux, master),
  Clear chain (`AudioPage.xaml:635`, `:712-754`, `:1040-1085` (audit); `SVM:9185`, `:9224-9229`
  attach each bus's insert chain to every send as `busPluginInserts`).
- Response curves drawn from the chain's own maths (`DspResponseCurve`, audit row 23).

### 2.7 Sync trims
- Guest lip sync: per-guest offset, plus/minus 10 ms steps, clamp plus/minus 200, only while Zoom
  is live, the topology is PerGuestIso and a Zoom guest is selected
  (`W/ViewModels/GuestAvSyncController.cs:51-116`; test `GuestAvSyncControllerTests`). Wire:
  `set-zoom-guest-av-sync-offset {participantId, offsetMs}` (`N/rpc/JsonRpcServer.cpp:264-275`);
  snapshot `zoomGuestAvSync[]` and `zoomGuestAvSyncRevision` (`CoreStateCapture.cpp:721-741`).
  The core clears offsets on a new speaker epoch (`MediaCore.cpp:550-559`).
- Capture audio nudge: plus/minus 20 ms and reset, clamp plus/minus 500, per capture device, on
  the Sources page (`SVM:7135-7172`, `W/Views/SourcesInputsPage.xaml:110-132`). Wire:
  `set-capture-audio-sync-offset {deviceId, offsetMs}` (`JsonRpcServer.cpp:255-262`). The offset is
  also a field of each `sync-capture-audio-sources` entry (`MediaCore.cpp:3456-3459`).

### 2.8 VST3
- Out-of-process host `corevideo-plugin-host` with `--scan`, `--probe`, `--process`,
  `--state-roundtrip`, `--serve` (`native/plugin-host/plugin-host.cpp:1-22`, `:852-896`).
- **No VST3 SDK and no JUCE.** The COM ABI is declared raw in `native/plugin-host/vst-abi.h`
  ("GPLv3 - house rule", `plugin-host.cpp:11-15`).
- Transport: one shared-memory block plus six named events per instance
  (`native/plugin-host/host-transport.h:1-13`, `:181-190`), magic `CVP3`; a missed deadline is a
  bypass for that tick.
- Respawn ladder 5, 10, 20, 40, 60 s, gives up after five failed retries, a run of 30 s or more
  is healthy (`N/modules/PluginHostRespawnPolicy.h`; test `PluginHostRespawnPolicyTest`).
- Commands `scan-vst-plugins`, `open-vst-editor`, `set-vst-param`, `set-vst-state`, `get-vst-state`
  (`N/core/Protocol.h:98-101`, `:175-179`). State blobs persist in `VstInsertStates`
  (`ProductionOutputPreferencesStore.cs:193`).
- Tests: `VstHostAbiTest` (16 tests, fake factory, no plug-in needed), `VstLatencyCompensationTest`,
  `MediaCoreCommandTest` `PluginHostTransport.ExchangesBlocksWithTheRealHostAndBypassesOnDeath`.
- Rule (`docs/reference/live-meeting-qa-2026-08-09.md:44-56`): the `"media"` strip's inserts run
  once on the pre-summed clip signal; a VST insert must never be exchanged once per clip.

## 3. Mac today

### 3.1 Done on this branch (do not re-specify)
- Zoom audio subscriptions: `meeting-audio` plus `participant-audio` per source
  (`M/AppModel.swift:2402-2455`).
- Strip set from the shell's own model, `"media"` strip included, echo used for measurements only
  (`M/AudioConsolePolicy.swift:48-122`); the A1's mute is never adopted from the echo.
- Default routing in the PerGuestIso shape, operator edits carried in the base matrix
  (`AudioConsolePolicy.swift:147-164`, `AppModel.swift:3664-3675`).
- Whole-console re-assert on every core generation, mix first (`AppModel.swift:4028-4058`, called
  from `onConnected` at `:1481`).
- Pre-mute dimmed meters (`AudioConsolePolicy.swift:166-188`, `M/AudioConsole.swift:351`).
- Monitor output device by CoreAudio UID, persisted (`M/MonitorOutputDevice.swift`,
  `M/ShellPrefs.swift:166-167`).
- Friendly source names (`AppModel.swift:3990-4002`); audit row 6 is closed.

### 3.2 Still open (verified)
| Area | State | Evidence |
|---|---|---|
| Zoom audio mode | Missing. The base matrix is hard-wired PerGuestIso. | no `zoomAudioMode` in `M/`; `AudioConsolePolicy.swift:126-137` |
| Local audio source | Missing. Only the label exists. | `AppModel.swift:3994`; `M/AudioInputs.swift:82-98` sends capture slots only |
| Mastering | 11 of 21 fields, not persisted, limiter hard-coded on. | `AppModel.swift:248-266`, `:4170`; no mastering key in `M/ShellPrefs.swift` |
| Buses | Six buses, `busSends: []`, `monitorBusId: ""`, `busPluginInserts: []`. | `AppModel.swift:3598`, `:3673-3674`; `AudioConsolePolicy.swift:161` |
| Meters | Console rail draws mono `masterLevel` twice and labels short-term LUFS as "LUFS"; integrated LUFS never read; no GR meter; no ZOOM MUTED badge. | `M/AudioConsole.swift:366-393`; `AppModel.swift:1659-1662` |
| Processing | Fixed gate, EQ, comp plus one VST per channel; no chain list, no bus target, hand-drawn curves. | `AppModel.swift:178-240`; `M/VstInserts.swift` |
| Sync trims | Missing in the shell. | no `avSync` / `audioSyncOffset` in `M/` |
| VST3 | Scan only. | `M/VstInserts.swift:1-40` |
| `audio.*` control actions | Missing. | no `audio.` in `M/ControlApi.swift` or `M/ControlSurface.swift` |

The Studio footer already has stereo L/R from the bus taps (`M/StudioFooter.swift:76-90`,
`:911-912`); only the Audio console rail is mono.

### 3.3 What the shared core supports on macOS
Already working on macOS, shell work only: every mastering key, all 15 buses and `bus-*`, bus
sends, LISTEN, `busPluginInserts` for built-in inserts, `masterMeter`, `busTaps`, `inputPeakDbfs`,
`gainReductionDb`, guest lip sync (platform-neutral core code), the capture audio offset
(`N/modules/AvfCaptureDeviceAdapter.mm:184-192`; applied to frame timestamps at
`N/modules/CoreAudioCaptureSourceAdapter.cpp:371`), and a `local-machine-audio` source on any
CoreAudio input device (`CoreAudioCaptureSourceAdapter.cpp:96-101`).

Windows-only today, native work needed:
- **System-audio loopback.** The CoreAudio adapter refuses a loopback source on a default sentinel
  and asks for a BlackHole-class device (`CoreAudioCaptureSourceAdapter.cpp:17-22`, `:227-233`).
  `N/modules/SckScreenCaptureAdapter.mm` captures no audio.
- **VST3 load, probe, process, state, serve, editor.** All inside `#ifdef _WIN32`
  (`plugin-host.cpp:153-430`, `:448-850`, `:854-896`); `PluginHostClient` is a stub off Windows
  (`N/modules/PluginHostClient.h:411-437`); the editor needs an HWND
  (`native/plugin-host/vst-processor.h:834-1003`); `resolvePluginHostExecutablePath` has no
  sibling lookup off Windows (`MediaCore.cpp:7906-7924`); scan roots are Windows environment
  variables plus one override (`plugin-host.cpp:138-150`). The bundled host is signed with **no
  entitlements** because it "only walks the filesystem" (`scripts/make-macos-shell-bundle.sh:213-218`).

## 4. Design

### 4.1 Principles
1. The shell owns intent; the echo supplies measurements. Every new control follows the
   `AudioConsolePolicy` shape: a pure function over the whole decision, tested by constructing the
   regression.
2. Full-state commands stay full-state. Nothing new is sent as a partial list, and everything new
   is added to `reassertAudioConsole()` so a respawned core gets it back in the same request,
   mix first.
3. No new work on the audio worker's lock path. The two-lock discipline
   (`docs/reference/gotchas.md:515-527`: `coreMutex` briefly for gather and publish,
   `audioOutputMutex_` for DSP and devices, never nested on the worker) is untouched by slices
   A1 to A7 because they only send existing commands. A8 and A9 add native code and state their
   threading below.
4. Honest reporting (`docs/reference/honest-reporting.md`): a control that cannot work is dimmed
   with the reason; a meter with no evidence reads a dash.

### 4.2 Zoom audio mode (A1)
- New `enum ZoomAudioMode { programMix, perGuestIso }` in `AudioConsolePolicy`, with `parse` and
  `format` that are a line-for-line port of `ZoomAudioModePreference` (unknown parses to
  `perGuestIso`).
- `AudioConsolePolicy.baseRoutingSends` gains a `zoomAudioMode` parameter. PerGuestIso keeps
  today's output and additionally drops operator `zoom-mix` sends to the five default buses (the
  Windows doubling guard, which the Mac does not apply today). ProgramMix: `zoom-mix` gets the
  five sends when `meetingAudio` is true, stems get none, and operator stem edits survive.
- Subscriptions do not change with the mode. Stems stay subscribed in ProgramMix: the strips still
  meter, and ISO recording takes stems from source PCM, not from buses (`MediaCore.cpp:7582-7606`).
- Mute semantics differ by mode and the UI must say so. In ProgramMix a guest strip's mute does
  not silence that guest on program, because the guest is inside `zoom-mix`. The strip header
  shows "meter only" in that mode, matching the Windows status copy.
- UI: one toggle in the SETUP header, "Per-guest ISO audio", with the two Windows status sentences.
- Persistence: new optional `ShellPrefs.zoomAudioMode: String?`. No migration is needed; absent
  means `perGuestIso`.
- Flip sequence: change mode, `pushRoutingBase()`. No mix resync is needed. The flip is one
  full-state matrix, so there is no window with both paths routed or neither.
- Model entry point `setZoomAudioMode(_:)` is what the control surface's `audio.zoomMode.set`
  will call, and `zoomAudioMode` is added to the control state the Mac publishes.

### 4.3 Local audio source (A5 microphone, A8 system audio)
**Microphone or interface input (shell only).** A SETUP card "Local audio source" with an enable
toggle and a device picker backed by the existing `AudioInputs` list. When enabled and resolved,
`captureAudioSourcesCommand()` appends
`{captureDeviceId: "local-machine-audio", audioDeviceId: <UID>, audioDeviceName, audioSourceKind:
"coreaudio-input", nativeAudioDeviceId: <UID>}`; `expectedSourceIds` and `baseRoutingSends` treat
`local-machine-audio` as a capture source (strip plus the five default sends). Status text is
derived from the `captureAudioSources[]` row the core already publishes (`captureStreaming`,
`captureFramesReceived`, `captureLastFrameAgeMs`, `signalPresent`, `peakDbfs`;
`CoreStateCapture.cpp:1795-1821`). Persist `localAudioEnabled`, `localAudioDeviceId`,
`localAudioDeviceName`, `localAudioKind`. A saved device that is not connected keeps the choice
and says so, as the monitor picker does.

**System audio.** macOS offers three routes:

| Route | OS floor | Permission | Verdict |
|---|---|---|---|
| Virtual device (BlackHole-class) chosen as an input | 13 | microphone | Works today through the core. Keep as the universal fallback; the picker lists such devices and marks them "loopback". |
| Core Audio process tap (`AudioHardwareCreateProcessTap` on a private aggregate device) | 14.2 | audio capture (`NSAudioCaptureUsageDescription`), no screen-recording prompt | **Recommended primary.** The tap appears as a device, so the existing AUHAL reader consumes it; the tap can exclude our own processes. |
| ScreenCaptureKit audio (`SCStream`, `capturesAudio`) | 13 | Screen Recording, with the recurring system reminder on recent macOS | Rejected as the default: a heavier permission for an audio-only need, and a second clock domain to rate-match. |

Design for the tap (native, new):
- New `audioSourceKind` value **`coreaudio-tap-loopback`** (new; contains "loopback", so the
  monitor feedback guard at `MediaCore.cpp:6722` still classifies it) and a new sentinel
  `audioDeviceId` **`system-audio`** (new). `CoreAudioCaptureSourceAdapter` creates a global
  stereo tap that excludes the core's own pid and the Zoom engine's pid, wraps it in a private
  aggregate device, and hands that device to the existing AUHAL path. Teardown destroys both.
- Excluding our own pid is a correctness requirement: the core plays the monitor bus, so an
  unfiltered tap would feed the monitor back into every bus the local source is sent to.
- Threading: tap and aggregate creation happen in `configure()`, which already runs off the audio
  worker; the render callback path is unchanged.
- Capability is reported, never assumed: `list-capture-devices` (or the capture-audio state; the
  plan picks one) gains a boolean **`systemAudioTapSupported`** (new). On macOS 13.0 to 14.1 the
  picker shows "System audio needs macOS 14.2 or a loopback device such as BlackHole".
- Degraded states the operator sees: permission denied ("Allow CoreVideo Pro to record system
  audio in System Settings > Privacy & Security"), tap created but silent (the Windows "loopback
  idle" sentence), and output-device change (the adapter rebuilds the tap; the strip reads
  `waiting-for-pcm` meanwhile).
- UNVERIFIED: which process TCC attributes the audio-capture grant to when the helper core (not
  the app) creates the tap, and whether the usage string must be in the shell's Info.plist, the
  core's embedded plist, or both. Slice A8 starts with a spike on the signed bundle.

### 4.4 Mastering rack (A2, A3)
- `MasteringParams` grows to the 21 Windows fields with the Windows defaults and ranges:
  `lowPassHz`, `limiterEnabled`, `glueRatio`, `glueAttackMs`, `glueReleaseMs`, `glueMakeupDb`,
  `glueMultiband`, `glueBandLowDb`, `glueBandMidDb`, `glueBandHighDb`, and a target that is one of
  -14 / -16 / -23 (a saved off-grid Mac value snaps to the nearest). `limiterEnabled` moves from
  the hard-coded `true` at `AppModel.swift:4170` into the params and stays a top-level key of
  `sync-participant-audio-mix`, where the core reads it.
- Range fixes: high-pass 0 to 300 Hz, tone plus/minus 12 dB, ceiling -6 to -0.5 dBFS. The FILTER
  stage's "engaged" light follows the value actually sent (the audit's honesty finding: today the
  stage dims below 20 Hz while 1 to 20 Hz is applied, `M/AudioConsole.swift:572`).
- Persistence (A2): `ShellPrefs.mastering: MasteringParams?` as Codable. It is restored into the
  model before the first `reassertAudioConsole()`, so the first mix command a core ever receives
  carries the saved rack. This is the fix for "silently resets to bypass".
- Presets and A/B (A3): a Swift port of `MasteringPresetCatalog` (built-ins, code-defined) and
  `MasteringPresetLibrary` (user presets: save, apply, rename, delete), plus compare slots A and B
  with the active slot. Persisted as `masteringCompareA`, `masteringCompareB`,
  `masteringCompareSlot`, `masteringUserPresets`. The two Windows test files are the oracle; the
  built-in values are transcribed from the catalog and pinned by a table test.
- The tone-curve preview (audit row 67) and Reset-stage menu ride with A3.

### 4.5 Meters (A4)
- Decode `masterMeter.integratedLufs` and `momentaryLufs` next to the two fields read today.
- SETUP gets two guide meters: integrated LUFS with a marker at the mastering target, and true
  peak with a marker at the ceiling. Below -119 they read a dash (Windows rule, `SVM:1319-1323`).
- The console's master rail uses `masterLevelLeft` / `masterLevelRight` (already in the model) and
  labels its number with what it is. Windows shows `loudnessLufs`, which the core defines as
  short-term falling back to momentary (`CoreStateCapture.cpp:1570`); the Mac adopts the same
  rule so both shells show the same number under the same label.
- Strip meters move to the calibrated dBFS scale with tick labels, driven by measured `peakDbfs`
  when published and the pre-mute `inputPeakDbfs` when muted. A Swift `AudioMeterScale` is tested
  against the same vectors as `AudioMeterScaleTests`.
- Compressor GR meter from per-channel `gainReductionDb`. ZOOM MUTED badge from the roster's
  `muted` flag: display only, never written into the strip's mute (#481).
- Integrated loudness has no reset command in the core that I could find (UNVERIFIED: only
  `programIntegratedMeter_` at `MediaCore.cpp:4305` was read). The Mac shows the value as
  published and does not invent a reset.
- Performance: meters are scalar `@Published` values updated once per poll; no collection is
  rebuilt per tick.

### 4.6 Buses (A6)
- `AppModel.routingBuses` becomes the 15 fixed buses in Windows order, plus operator-added custom
  buses with ids `bus-<n>` and editable labels. Aux and custom buses are removable; the rest are
  fixed.
- New model state: `busSends` (from, to, gainDb), `listenBusId`, `customBuses`. `routingBaseCommand()`
  stops sending `busSends: []` and `monitorBusId: ""` and sends the model's values. Because the
  command is full-state with a hold-last guard, bus sends and LISTEN always travel with the sends.
- LISTEN is exclusive and is transient: it is not persisted and is cleared on a core respawn, so
  a restart can never leave the monitor silently auditioning a bus.
- Grid additions: Clear-row action, routed count per bus, horizontal scrolling with the source
  column pinned (15 or more columns do not fit the pop-out's width).
- Persistence: custom bus ids and labels only (`ShellPrefs.audioCustomBuses`). Windows does not
  persist the matrix, bus sends or strip settings (no such members in
  `ProductionOutputPreferencesStore.cs`), so neither does the Mac.
- What the ISO buses are for: they are routing taps with meters (`busTaps`). ISO **recording**
  stems do not come from them (see Surprises). The UI copy must not promise otherwise.

### 4.7 Processing workspace (A7)
- A "processing target" picker: selected channel, any bus, aux, or master. Bus targets write a
  per-bus insert chain that `baseRoutingSends` attaches to every send of that bus as
  `busPluginInserts`, exactly as Windows does after synthesising defaults (`SVM:9224-9229`), so
  master processing stays live on implicit default routes.
- The insert chain becomes an ordered list (name, status, remove) rather than three fixed
  toggles. The core matches built-in inserts by lowercase substring (`AppModel.swift:178-182`), so
  order is expressed by the order of `pluginInserts`. UNVERIFIED: whether the core honours that
  order for built-ins or applies a fixed gate, EQ, compressor order; the plan's first task reads
  `applyChannelInserts` in `N/modules/AudioDsp.h` and, if the order is fixed, the Mac list is
  shown in that fixed order without drag handles.
- Response curves: replace the three hand-made approximations with Swift ports of the Windows
  `DspResponseCurve` maths, pinned by shared numeric vectors (frequency, expected dB) taken from
  the Windows tests where they exist and from the core's own filter coefficients otherwise.
- Clear chain; pan readout; meter tooltip; name chip as a button (audit rows 8, 16, 17, 28).

### 4.8 Sync trims (A9)
- Guest lip sync: a Swift `GuestAvSyncController` port (pure state machine: can-adjust rule,
  plus/minus 10 ms, clamp 200, revision-gated snapshot adoption, clear on leaving the meeting),
  a flyout on the selected Zoom strip, wire command `set-zoom-guest-av-sync-offset`. It depends on
  A1 for the PerGuestIso condition. Offsets are session state in the core and are not persisted.
- Capture audio nudge: minus 20 / plus 20 / reset beside each paired microphone in the existing
  pairing row (`M/AudioInputs.swift:50-108`), clamp 500. The value is stored on the slot,
  persisted with the pairing, sent as `audioSyncOffsetMs` in every `sync-capture-audio-sources`
  entry (so a respawned core gets it), and applied immediately with
  `set-capture-audio-sync-offset`. This is audio-owned; the Sources workstream only needs to leave
  the pairing row in place.

### 4.9 VST3 on macOS (V1 to V4, late)
**Where the Mac host stands.** It builds, ships in the bundle, and implements `--scan` only.
There is no SDK to port and no licence question to settle for VST3: the host speaks the ABI
directly, and `vst-abi.h` already compiles on macOS with an empty calling-convention macro
(`vst-abi.h:36-40`). `VstHostAbiTest` exercises the processor lifecycle with fake vtables and is
platform-neutral. What is missing is everything that touches the OS:

| Piece | Windows | macOS design |
|---|---|---|
| Module load | `LoadLibraryExA`, `InitDll`, `GetPluginFactory` (`plugin-host.cpp:215-232`) | `CFBundle` load of `<name>.vst3/Contents/MacOS/<name>`, `bundleEntry(CFBundleRef)`, `GetPluginFactory`, `bundleExit` on unload. An Intel-only bundle is reported as "Intel-only plug-in, not loadable on Apple Silicon" from the Mach-O header before any load. |
| Shared block | `CreateFileMappingA` | `shm_open` + `mmap`. Names must be 31 characters or fewer on macOS; the current prefixes (`corevideo-vsthost-shm-` plus instance) do not fit, so macOS uses a short form `/cvh-<8 hex>-<role>` (new), generated beside the existing name functions. |
| Events | Six auto-reset named events | Six named POSIX semaphores under the same short-name rule. The audio exchange waits with a deadline; macOS has no `sem_timedwait`, so the plan chooses between a bounded `sem_trywait` spin-then-sleep and Mach semaphores passed at spawn. Decided by measurement in V2, with the deadline-miss rate as the criterion. |
| Spawn and liveness | `CreateProcessA`, `WaitForSingleObject(process)` | `posix_spawn`, `waitpid(WNOHANG)`. Host path: sibling of the core executable when `COREVIDEO_PLUGIN_HOST_PATH` is unset. |
| Serve loop | `MsgWaitForMultipleObjects` on one STA thread | Audio exchange on a dedicated realtime-priority thread; the main thread runs an `NSApplication` run loop (accessory activation policy) that owns editor windows and drains param and state requests. |
| Editor | HWND child window | `NSWindow` whose content view is passed to `IPlugView::attached(view, "NSView")`; resize through `IPlugFrame`; close publishes the idle editor status exactly as on Windows. |
| Scan roots | Windows env vars | `/Library/Audio/Plug-Ins/VST3` and `~/Library/Audio/Plug-Ins/VST3` built in, plus the override. Closes audit row 33. |
| Signing | n/a | The host needs `com.apple.security.cs.disable-library-validation` (plug-ins are signed by other teams) and its own entitlements file. Some plug-ins also need unsigned executable memory or JIT; see Owner decisions. The current "no entitlements" comment in the bundle script becomes false and must be rewritten with the slice. |

Unchanged and reused: `HostAudioBlock` and magic `CVP3`, `PluginHostRespawnPolicy`, latency
compensation, the param ring, the 1 MiB state area, the core's bypass-on-deadline rule, and every
wire command and snapshot field. The Mac shell's VST surface is already gated on
`pluginHost.serve.running` (`M/VstInserts.swift:134-145`), so it lights up without a platform
check; the shell work is state-blob persistence (`vstInsertStates` in `ShellPrefs`, restored with
`set-vst-state` on every core generation) and the insert-slot placement inside the A7 chain list.

Threading: the audio worker's exchange is already synchronous with a deadline and takes no core
lock while waiting; the macOS client must keep that property (no allocation, no `coreMutex`, no
blocking beyond the deadline). The `"media"` strip rule holds unchanged: one exchange per strip
per tick on the pre-summed signal.

UNVERIFIED: that the struct layouts asserted for x64 Windows hold on arm64 macOS for every
structure (the static asserts compile today, which covers size and offsets, but not the VST3
packing pragma on macOS); V1 proves it against real plug-ins with `--process`.

**Audio Units.** Not required for parity and not recommended for 1.x. An AU host is a second
plug-in model (AUv2 component and AUv3 extension, different state format, different editor
embedding), its state blobs would not travel between a Windows and a Mac show, and the Windows
product has no AU concept to be at parity with. VST3 covers the plug-ins operators asked about
(the scan on this Mac already finds 200 or more, UAD and Waves among them, per
`M/VstInserts.swift:21-24`). Revisit only if operators name AU-only plug-ins.

### 4.10 Control surface
The Windows `audio.*` actions (`audio.zoomMode.set`, `audio.monitor.set`, `audio.monitor.volume`,
`audio.route.set`, `audio.masterLimiter.set`, `audio.mastering.set`, `audio.mastering.target`,
`audio.vst.scan`; `native-shell/CoreVideoPro.Control/ControlActionRegistry.cs:265-293`) are owned
by the control-surface workstream. This spec guarantees one model entry point per action
(`setZoomAudioMode`, the existing monitor and route control projections, `editMastering`,
`scanVstPlugins`) so that workstream adds dispatch only.

## 5. Slices

Order follows operator impact and keeps every slice wired to a visible control.

| Id | What lands | Lane | Size | Deps | Proof |
|---|---|---|---|---|---|
| A1 | Zoom audio mode toggle, mode-aware base routing with the doubling guard, "meter only" strip note, persistence, `setZoomAudioMode` | shell | M | none | Swift ports of the two `EnsureDefaultZoomAudioRoutingSends_*` tests and the five preference tests, each failing on today's code; live owner check with two guests |
| A2 | Mastering: 21 fields, correct ranges, limiter toggle, honest FILTER light, persisted and restored before the first mix command | shell | M | none | Unit test that the first `audioMixCommand()` after a prefs load carries the saved rack; relaunch drill reading `audioMixSession` back from the core; recorded-file loudness check |
| A3 | Mastering presets, user presets, A/B compare, reset stage, tone-curve preview | shell | M | A2 | Ports of `MasteringPresetCatalogTests` and `MasteringPresetLibraryTests`; table test pinning built-in values |
| A4 | Meters: integrated LUFS and true-peak guide meters, stereo rail, dBFS strip scale, GR meter, ZOOM MUTED badge | shell | M | A2 for the markers | `AudioMeterScale` vectors; snapshot-fixture decode test; `COREVIDEO_SHELL_SNAPSHOT` capture of the SETUP meters with a known tone; tone test below |
| A5 | Local audio source, microphone or interface input (and any virtual loopback device) | shell | S | none | Full-state command test (local entry present, absent when disabled, survives re-assert); live: speak into the mic, see the strip, hear it in a recording |
| A6 | Buses: 15 fixed, custom buses, bus sends, LISTEN, clear row, routed counts, scrolling grid | shell | M | none | Policy tests for exclusivity, removable-only bus sends and LISTEN exclusivity (ports of `RoutingMatrixViewModelTests`); core read-back of `audioRoutingMatrix`; live LISTEN on headphones |
| A7 | Processing workspace: target picker with bus, aux and master chains, ordered insert list, real response curves, clear chain, small strip polish | shell | L | A6 | Wire test that a master chain appears as `busPluginInserts` on every master send including defaults; curve vectors; audio proof: master EQ cut measured in a recording |
| A8 | System audio through a Core Audio process tap, capability flag, permission and idle states | native + shell | L | A5; spike first | Native test with the tap seam faked; on-device proof: play a 1 kHz tone from another app, measure it in the recorded master, and prove the monitor output is not captured |
| A9 | Sync trims: guest lip-sync flyout, capture audio nudge with persistence | shell | M | A1 (guest sync) | Port of `GuestAvSyncControllerTests`; command tests; `scripts/validate-av-clap.mjs`-style clap measurement before and after a +40 ms nudge |
| V1 | Plug-in host on macOS: module load, `--probe`, `--process`, `--state-roundtrip`, built-in scan roots, Intel-only detection, host entitlements | native | L | owner decision 1 | `VstHostAbiTest` green on macOS; `--process` on three real plug-ins changes the 440 Hz signal; probe verdicts shown in the existing Mac browser |
| V2 | POSIX transport, macOS `PluginHostClient`, `--serve` audio exchange, respawn and bypass | native | L | V1 | `PluginHostTransport.ExchangesBlocksWithTheRealHostAndBypassesOnDeath` enabled on macOS; kill-the-host drill; deadline-miss count over a 30-minute soak; a channel insert audibly processing in a recording |
| V3 | Editor windows, parameter publish and set, state get/set, shell persistence of state blobs | native + shell | L | V2 | Open, edit, close, relaunch: the state returns and the recording shows the same processing; editor-failed and gave-up callouts exercised |
| V4 | Audio Units host | native + shell | XL | owner decision 2 | not planned |

A1 to A7 and A9 need no native change and can ship in any order after their dependencies.

## 6. Acceptance and test strategy

- **Unit tests (Swift, in the `COREVIDEO_SHELL_TESTS` harness).** Each ported Windows ruling is
  quoted in its task with the Windows test that proves it, and the Swift test must fail on the
  current Mac behaviour first (roadmap rule 6). The policies in section 4 are pure functions so
  the regression can be constructed.
- **Shared vectors rather than re-derived behaviour.** Mode parse/format, default-send synthesis,
  meter scale, preset values and response curves are checked against the same input and output
  tables as the Windows tests. Where the control-surface workstream adds contract fixtures for
  `audio.*`, this spec's model entry points are exercised through them.
- **Core read-back.** For every command the shell now sends differently (mix with the full rack,
  matrix with bus sends and LISTEN, capture-audio with the local source and offsets), a harness
  test asserts the command, and a drill asserts the core's snapshot echoes it.
- **Audio is proven with audio.** `scripts/validate-record-audio.mjs` is the model: record, decode,
  measure. Required runs on the Mac: (a) ProgramMix against PerGuestIso with a synthetic two-talker
  meeting, checking that master carries each voice exactly once in both modes; (b) a -20 dBFS
  1 kHz tone through the local source, with the recorded master's integrated loudness within
  0.5 LU of the guide meter and the true peak within 0.5 dB; (c) a master EQ cut visible in the
  recording's spectrum; (d) the same for one VST3 insert in V2. UNVERIFIED: whether
  `validate-record-audio.mjs` runs unmodified against the Mac core; if not, its Mac port is part of
  the first slice that needs it.
- **Pixels.** The guide meters, the stereo rail and the bus grid are captured with
  `COREVIDEO_SHELL_SNAPSHOT` at the pop-out size (1080 by 700) and the full tab.
- **Drills.** `scripts/mac-show-drill.py` gains an audio pass: set a rack, kill the core, and
  assert that mastering, mode, local source, bus sends and offsets are back on the respawned
  core, and that LISTEN is not.
- **Soak.** V2 runs 30 minutes with a loaded plug-in while `mac-show-drill.py --load 8` runs;
  acceptance is no rise in audio underruns or missed program frames against a no-plug-in baseline.
- **Live owner checks.** A1 with real guests (mute behaviour in both modes); A5 and A8 on the
  owner's interface and with system audio from a browser; A6 LISTEN on headphones; A9 guest lip
  sync on a real guest; V3 with the owner's own plug-ins, including one that needs a licence dongle
  or manager.

## 7. Risks and unknowns

| Risk | Resolution |
|---|---|
| Process-tap permission is attributed to the wrong process, or the prompt never appears from a helper | Spike at the start of A8 on a signed bundle. Fallback: create the tap in the shell and pass the aggregate device UID to the core. |
| The tap's aggregate device changes clock when the output device changes | Rebuild on the device-change notification; measured in the A8 on-device proof by switching outputs mid-capture. |
| No timed semaphore wait on macOS makes the exchange deadline inexact | V2 measures both candidate mechanisms; the criterion is the deadline-miss rate and worker jitter, not elegance. |
| Plug-ins that need JIT or unsigned executable memory fail to load under the hardened runtime | V1 loads the owner's list and records which entitlement each needs; the result drives owner decision 3. |
| Copy-protected plug-ins (licence managers) refuse an unfamiliar host | Owner test in V3; reported honestly per plug-in through the existing probe verdict. |
| Plug-in editors assume they own the main thread or the app's menu | The host is a separate accessory app with its own run loop, which is the usual arrangement; verified with three vendors' editors in V3. |
| Built-in insert order may be fixed in the core | Read `applyChannelInserts` first in A7; show the true order either way. |
| ProgramMix makes strip mutes look effective when they are not | The "meter only" note in A1; owner check with real guests. |
| The 15-bus grid does not fit the pop-out | Pinned source column and horizontal scroll; snapshot at 1080 by 700. |
| A notarized host with library validation disabled is rejected | The Zoom engine already ships with that entitlement (roadmap section 5.3); V1 submits a test notarization early. |

## 8. Owner decisions needed

1. **Does 1.x parity on the Mac require VST3?** Recommendation: no for the first Mac release that
   claims parity on the rest of this spec; ship A1 to A9 first with the plug-in surface labelled
   "scan only", then V1 to V3 as the next audio release. V1 to V3 are about five to six weeks of
   native work and are independent of everything else here.
2. **Audio Units as well?** Recommendation: no. Not a parity item, and state would not travel
   between platforms.
3. **Plug-in host entitlements.** Recommendation: grant `disable-library-validation` only at
   first; add unsigned-executable-memory or JIT only if V1's survey shows plug-ins the owner
   actually uses need them, since each widens what a compromised plug-in can do (the host is
   already a separate process for that reason).
4. **System audio on macOS 13.0 to 14.1.** Recommendation: process tap on 14.2 and later, and a
   loopback device (BlackHole-class) with clear guidance below that. The alternative is a
   ScreenCaptureKit path that needs the Screen Recording permission on every supported version.
5. **Default topology.** Recommendation: keep `perGuestIso` as the Mac default, as Windows does
   since prefs v10.

## 9. Out of scope

- The `audio.*` control actions' dispatch, manifest and fixtures (control-surface workstream; this
  spec supplies the model entry points).
- Media clip decode and the audio it produces (row 2.1); the `"media"` strip and sends already
  exist and need no change.
- Recording and stream audio bitrate settings (row 2.8) and Zoom quality caps (the video half of
  row 2.13).
- Dedicated Zoom ISO feed (roadmap M8.4) and Comms talkback audio (M7).
- ASIO-style multi-channel interface routing, noise suppression, and anything Auto director.
- Persisting strip settings or the routing matrix across launches: Windows does not, so this is
  not parity work.
- The Windows monitor-volume slider scale discrepancy noted in the audit (a Windows item).

## 10. Surprises against the audits and roadmap

- **ISO recording audio does not use the ISO buses.** The audio audit (row 43) and gap row 2.9
  say that without ISO 2 to 8 "multi-guest ISO recordings get no per-guest audio beyond the
  first". The core builds each ISO stem from that source's own PCM by source id
  (`MediaCore.cpp:7582-7606`); the `iso-*` buses are routing taps with meters and have no
  recording consumer that I could find. Adding the columns is still parity work, but it is
  cosmetic for recording, not a data-loss fix.
- **The VST3 host uses neither JUCE nor the Steinberg SDK.** The brief asked for the SDK
  situation: there is none to resolve. The port is OS plumbing (bundle load, shared memory,
  semaphores, an AppKit editor window), not an SDK integration.
- **macOS shared-memory names.** The plug-in transport's names exceed the 31-character macOS
  limit that already bit the plug-in to engine frame transport; V2 must shorten them.
- **The Mac already defaults to the PerGuestIso routing shape**, so the missing toggle is "no way
  to choose ProgramMix", plus one missing guard (operator `zoom-mix` sends are not removed from
  the program buses in PerGuestIso, so a hand-routed mix can double voices today).
- **The roadmap has no audio milestone.** Audio parity is absent from M0 to M9; this spec's
  slices need a tracking issue and a rank in `docs/BACKLOG.md`.
- **The bundle script's claim that the plug-in host needs no entitlements** holds only while the
  host is scan-only; it becomes wrong at V1.
