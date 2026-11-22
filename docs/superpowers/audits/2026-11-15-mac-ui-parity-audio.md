# Mac UI parity audit: Audio (2026-11-15)

Read-only audit of the macOS shell (`mac-shell/Sources/CoreVideoProShell/`) against the
Windows operator console (`native-shell/CoreVideoPro.WinUI/`) for audio. Covers the Audio
tab, the pop-out Audio mixer, VST inserts, routing and sends, buses, monitoring, metering,
loudness and mastering. Worktree: `.claude/worktrees/mac-zoom-signin`, the build the owner
has installed. No app was launched. I checked every Mac claim in source, and checked the
core (`native/src/...`) wherever a gap could be a platform blocker rather than shell work.

Paths are shortened: `W/` = `native-shell/CoreVideoPro.WinUI/`, `M/` =
`mac-shell/Sources/CoreVideoProShell/`, `N/` = `native/src/`. `AudioPage.xaml` is `W/Views/AudioPage.xaml`.
`SVM` = `W/ViewModels/StudioViewModel.cs`.

**Status key:**
- **Same**: the behaviour matches.
- **Partial**: present but reduced.
- **Different**: present, but behaves differently in a way that matters.
- **Missing**: absent.

**Core column:**
- **shell**: the core already supports it on macOS.
- **native**: needs core or platform work first.
- **n/a**: no core involvement.

---

## 1. Correctness rules (CLAUDE.md / `docs/reference/live-meeting-qa-2026-08-09.md`)

| Rule | Windows | Mac verdict | Evidence |
|---|---|---|---|
| **FADER LAW**: no source reaches a bus without a strip. | The shell builds the strip set itself. It covers every expected source (`SVM:8829-8838`, `BuildExpectedAudioSourceIds`), and sources with no PCM yet get `waiting-for-pcm` strips (`SVM:8941-8962`). A Zoom-mix strip always exists. | **VIOLATED IN PRACTICE.** The Mac strip set is whatever the core echoes back (`M/AppModel.swift:2928-2952`). Before any channel sync, the core reports native-mixer participants (`N/core/CoreStateCapture.cpp:1291-1310`). After the first sync, it reports *only the channels the shell sent* (`CoreStateCapture.cpp:1436`). The Mac sends only the strips it already has (`M/AppModel.swift:2983-2996`, and it bails out if there are none). So a source that starts carrying PCM **after the operator's first fader, mute or insert edit never gets a strip**. Once channels exist, the core drops any strip-less source from the bus mix (`N/core/MediaCore.cpp:6939-6955`). Result: a late-joining guest, a late capture card or a newly played media clip is inaudible and invisible on the console. | see cells |
| **"media" strip alias** (one `"media"` strip and send set governs every `media:*` clip). | `"media"` is always a routing source and a strip (`SVM:4569`, `SVM:4619`). | **MISSING.** No Mac code creates a `"media"` strip or send. Audio routing sources are the strips (`M/AudioConsole.swift:453-457`). The only `"media"` row on the Mac is the *video* routing row (`M/AppModel.swift:2510`). A clip is governed only if the core happens to echo it under its exact `media:<assetId>` id before the first sync. After that, the clip falls under the FADER LAW gap above. | `grep '"media"' M/*.swift` |
| **The A1's mute is set only by the A1** (#481). | `Muted` is never seeded from the core echo: `ResolveMergedChannelMute(prior) => prior?.Muted ?? false` and `MergeNativeAudioChannel` (`SVM:8883`, `SVM:8911-8939`). The Zoom mute lives only in `SourceMuted`. | **PARTIAL.** The Mac never sends a Zoom mute to the core, so a Zoom mute cannot latch into the A1's mute, which meets the letter of the rule. But on every snapshot the Mac re-adopts the core's echoed `muted` for any strip not edited in the last 2 s (`M/AppModel.swift:2946`). That is the "seed from `nativeChannel.Muted`" pattern #481 banned. The concrete failure: after a **core restart** the core's channel list is empty and its echo reads `false`, so the Mac takes "unmuted" and the A1's mutes are silently gone. Nothing re-sends the mix on reconnect (`M/AppModel.swift:780-791` re-sends the monitor, grade and capture audio, but not the mix or routing). | `AppModel.swift:2946`, `:780-791` |
| **Pre-mute input meters** (a muted, talking guest still shows on a dimmed meter). | `AudioMeterScale.ResolveChannelMeterLevel` uses input true peak when effectively muted (`W/.../AudioMeterScale.cs:35-36`), with `ShowLevelWhileMuted` (`AudioPage.xaml:1414-1415`, `W/Controls/AudioLevelMeter.xaml.cs:50-61`). | **MISSING.** The Mac meter is `SegmentedMeter(level: strip.muted ? 0 : strip.outputLevel)` (`M/AudioConsole.swift:354`). The core's `outputLevel` is already 0 when muted (`CoreStateCapture.cpp:1450`). The Mac never reads `inputPeakDbfs`/`inputRmsDbfs`, which the core publishes (`CoreStateCapture.cpp:1510-1519`): zero matches in `M/`. A muted strip always reads silence. | `AudioConsole.swift:354` |

---

## 2. Feature table

| # | Feature / control | Windows behaviour (file:line) | Mac | Mac evidence | Impact | Size | Core |
|---|---|---|---|---|---|---|---|
| **Surface and window** |
| 1 | Mode pills | SHOW / ROUTING / SETUP (`AudioPage.xaml:66-77`, `:934-942`) | Same (plus a PLUG-INS pill) | `M/AudioConsole.swift:32-40`, `:50-88` | Low: navigation matches | S | n/a |
| 2 | Pop-out mixer hosts the same console | `W/Views/AudioMixerWindow.xaml:19`, size 1080x700 (`.xaml.cs:17-18`) | Same | `M/ShellApp.swift:151-152`, `AudioConsole.swift:44-48` (hidden inside the pop-out) | Low | S | n/a |
| 3 | Zoom audio topology toggle (Per-guest ISO vs Zoom program mix, persisted) | `AudioPage.xaml:84-88`, `SVM:932-944`, prefs v9/v10 | Missing | no `perGuest`/`zoomAudioMode` anywhere in `M/` | **High**: the operator can't choose between per-guest stems and the Zoom mix, and the Mac never stages the matching routing (ISO stems unrouted or routed) | M | shell (routing and strip policy are shell-side on Windows) |
| **Strip set** |
| 4 | Strip set from expected sources, with `waiting-for-pcm` strips | `SVM:8829-8838`, `:8941-8962` | Different (broken) | `M/AppModel.swift:2928-2952`; core echo rule at `CoreStateCapture.cpp:1291` vs `:1436` | **High**: late sources get no strip, and the FADER LAW then drops them (see §1) | M | shell |
| 5 | `"media"` strip and routing row | `SVM:4569`, `:4619` | Missing | §1 | **High**: media clip audio is ungoverned or silent | S | shell (core alias exists: `core/AudioControlSourcePolicy.h`) |
| 6 | Friendly source names on strips and routing rows | `SVM:11336-11346` (roster name, `ResolveAudioSourceDisplayName`) | Partial | `AudioConsole.swift:137-142` maps capture slots only. Routing shows raw ids apart from zoom-mix and local (`:453-457`), so Zoom ISO strips show `zoom:<pid>`-style ids | Med: guests are hard to identify while mixing live | S | n/a |
| 7 | Channel chips show the primary bus label | `AudioPage.xaml:953-975` (`BusLabel`) | Different | `AudioConsole.swift:106` hard-codes "Master" on every chip | Low: misleading but cosmetic | S | n/a |
| **Fader strip** |
| 8 | Pan slider with readout (C / L 0.50 / R 0.50) | `AudioPage.xaml:1339-1351` | Partial | `AudioConsole.swift:318-323`: slider only, no readout | Low | S | n/a |
| 9 | MUTE toggle | `AudioPage.xaml:1357-1369` | Same | `AudioConsole.swift:325-333` | n/a | n/a | n/a |
| 10 | Solo toggle | `AudioPage.xaml:1370-1383` | Same | `AudioConsole.swift:334-342`; core `MediaCore.cpp:2994`, `:6900` | n/a | n/a | n/a |
| 11 | Gain readout | `AudioPage.xaml:1389-1390` ("+x.x dB") | Same (no "+" sign) | `AudioConsole.swift:344-345` | Low | S | n/a |
| 12 | **ZOOM MUTED** badge on the strip | `AudioPage.xaml:1393-1398` (#481) | Missing | The roster has `muted` (`M/AppModel.swift:287`, `:311`) but the console never uses it | **Med**: the A1 can't tell "muted in Zoom" from "my console mute" | S | n/a |
| 13 | Fader, -24 to +24 dB | `AudioPage.xaml:1405-1410` | Same | `AudioConsole.swift:347-353`; clamp at `AppModel.swift:2990` | n/a | n/a | n/a |
| 14 | Strip meter: dBFS scale, 36 segments, driven by output true peak | `AudioPage.xaml:1412-1422`, `AudioMeterScale.cs:35` | Different | `AudioConsole.swift:354`, `:423-444`: 24 segments, no scale, fed by `outputLevel`, the core's 0-100 heuristic (`CoreStateCapture.cpp:1450` = inputLevel + gainDb×4), not measured dBFS | Med: the meter can't be read against a dB scale | S | shell (`peakDbfs` is published) |
| 15 | Pre-mute input meter, dimmed while muted | see §1 | Missing | `AudioConsole.swift:354` | **High**: a muted, talking guest reads as silence (the #481 regression class) | S | shell |
| 16 | Meter tooltip (mute-state label) | `AudioPage.xaml:1422` | Missing | none | Low | S | n/a |
| 17 | Name chip selects the channel; tooltip shows the subtitle | `AudioPage.xaml:1426-1436` | Partial | `AudioConsole.swift:357-361`: plain text, not a button (the chips row at `:98-118` selects) | Low | S | n/a |
| 18 | 2 s local-edit quiet period | `SVM` quiet period | Same | `M/AppModel.swift:2925-2940` | n/a | n/a | n/a |
| **Processing workspace** |
| 19 | GATE cell: IN toggle and 5 sliders, ranges match the core | `AudioPage.xaml:1101-1137` | Same | `AudioConsole.swift:176-196`; names and keys `AppModel.swift:177-238` | n/a | n/a | n/a |
| 20 | COMP cell: IN toggle and 6 sliders | `AudioPage.xaml:1160-1209` | Same | `AudioConsole.swift:197-220` | n/a | n/a | n/a |
| 21 | Compressor gain-reduction meter | `AudioPage.xaml:1163` | Missing | The core publishes `gainReductionDb` (`CoreStateCapture.cpp:1522-1525`); the Mac never reads it | Med: no feedback on whether the comp is working | S | shell |
| 22 | EQ cell: low cut and 8 bands | `AudioPage.xaml:1225-1290` | Same | `AudioConsole.swift:221-257` | n/a | n/a | n/a |
| 23 | Response curves drawn from the chain's own maths | `DspResponseCurve` (`AudioPage.xaml:1108`, `:1175`, `:1232`) | Partial | `AudioConsole.swift:178-186`, `:199-209`, `:223-234`: hand-made approximations (the gate knee is an arbitrary formula; the EQ is a polyline through band gains with no low-cut slope) | Low: the "what you see is what processes" promise is weakened | M | n/a |
| 24 | Workspace dims when no channel is selected | `SVM` `WorkspaceEnabledOpacity` | Same (hidden instead) | `AudioConsole.swift:124-126` | n/a | n/a | n/a |
| 25 | Insert chain list (slot order, status, latency badge, remove ×) | `AudioPage.xaml:712-754`, `:1040-1075` | Missing | Fixed gate, then EQ, then comp, plus one VST (`AppModel.swift:203-209`, `VstInserts.swift:242-248`); no chain view | Med: the chain can't be seen or reordered | M | shell |
| 26 | "+ Add processing" menu (built-in or VST3 per channel) | `AudioPage.xaml:1081-1085`, `.xaml.cs:326-383` | Partial | Built-ins through the IN badges. The VST picker is disabled unless the host is live (`VstInserts.swift:596-620`) | Med | n/a | see 37 |
| 27 | Processing target picker: channel, bus, aux or master | `AudioPage.xaml:635`, `SVM:2634-2648`, `:5570-5700` | Missing | Workspace edits the selected strip only; bus inserts are always `[]` (`AppModel.swift:2773`) | Med: no bus or aux processing | M | shell (core reads `busPluginInserts`: `MediaCore.cpp:6845-6851`) |
| 28 | Add EQ, Add compressor, Clear chain (SETUP) | `AudioPage.xaml:769-788` | Partial | IN toggles cover add and remove; there is no Clear | Low | S | n/a |
| 29 | Selected-channel readouts (audio gain, manual gain, output level) and mute button in SETUP | `AudioPage.xaml:692-708`, `:794-796` | Missing | none | Low: the information is on the strip anyway | S | n/a |
| 30 | Guest lip-sync offset flyout (−10 / +10 / apply / reset) | `AudioPage.xaml:1005-1027`, `.xaml.cs:16-48` | Missing | no `avSync` in `M/` | Med: no lip-sync trim for Zoom guests | M | not checked; Windows routes it through `_bridge` (`StudioViewModel.GuestAvSync.cs:10`) |
| **VST3** |
| 31 | VST3 browser: rescan and search | `AudioPage.xaml:647-690` | Same | `VstInsertsSurface` (`M/VstInserts.swift:339-466`), filter at `:343-349`, rescan at `:381-386` | n/a | n/a | n/a |
| 32 | Probe verdict per plug-in | `AudioPage.xaml:684` (`ProbeLabel`) | Different (on purpose) | `VstInserts.swift:482-492` shows "NOT VERIFIED", because `--probe` is Win32-only (`N/../plugin-host/plugin-host.cpp:871-874`) | Low | n/a | native |
| 33 | Scan roots | Windows Common Files VST3 roots | Partial | Scans one root only (`VstInserts.swift:110-128`); the user root is skipped unless the env override is set | Low | S | native (the core accepts a single override path) |
| 34 | Load a VST into a channel and process audio | `.xaml.cs:377-403`, `SVM:5601-5611` | Missing | Gated on `pluginHost.serve.running` (`VstInserts.swift:134-145`). `PluginHostClient` is a non-Win32 stub (`N/modules/PluginHostClient.h:411-437`). `--serve`/`--process` are Win32-only (`plugin-host.cpp:854-894`) | **High** for operators who rely on plug-ins; none work | XL | **native** |
| 35 | Open the plug-in editor | `.xaml.cs:405-419`, `SVM:2572` | Missing (button disabled) | `VstInserts.swift:493-501` | High (same root cause) | XL | native |
| 36 | Generic parameter sliders | Windows insert-slot flyout (`.xaml.cs:168-250`) | Wired but never shown | `VstParamRack` (`VstInserts.swift:523-585`) only renders when the host publishes params | n/a | n/a | native |
| 37 | Persist VST state blobs | prefs `VstInsertStates` (`W/Services/ProductionOutputPreferencesStore.cs:188-193`) | Missing | Only the selection name persists (`AppModel.swift:513`, `:557`) | Med | M | native (needs `--state-roundtrip`) |
| 38 | Latency badge and compensation | `AudioPage.xaml:741`, `:1071` | Partial | Banner shows `+x ms` when live (`VstInserts.swift:414-418`) | Low | n/a | native |
| **Routing and buses** |
| 39 | Crosspoint grid; every cell has a level | `W/Controls/AudioRoutingMatrixPanel.xaml:77-113` | Same | `AudioConsole.swift:473-512` | n/a | n/a | n/a |
| 40 | Selected route: gain and Remove | `AudioRoutingMatrixPanel.xaml:132-148` | Same | `AudioConsole.swift:513-536`; range -60 to +10 (`AppModel.swift:2715-2722`) | n/a | n/a | n/a |
| 41 | Revisioned route control with notices | `W/ViewModels/StudioViewModel.AudioRouteControl.cs`, `AudioRoutingMatrixPanel.xaml:152` | Same | `M/AudioRouteControlProjection.swift:1-139`, `AudioConsole.swift:465-468` | n/a | n/a | n/a |
| 42 | ISO bus exclusivity | `NormalizeIsolatedAudioBuses` (`AudioRoutingMatrixViewModel.cs:432-444`) | Same | `AppModel.swift:2693-2699` | n/a | n/a | n/a |
| 43 | Bus set: 15 buses (master, pgm L/R, iso-1 to iso-8, mon, stream, aux-1/2) | `W/ViewModels/AudioRoutingMatrixViewModel.cs:84-101` | Partial | 6 buses: master, pgm-l, pgm-r, stream, mon, iso-1 (`AppModel.swift:2674`) | **High**: ISO 2-8 audio can't be routed, so multi-guest ISO recordings get no per-guest audio beyond the first | S | shell (core accepts all 15 plus `bus-*`: `MediaCore.cpp:3207-3215`) |
| 44 | Default sends for new sources (master, pgm, stream, mon; Zoom ISO left unrouted) | `AudioRoutingMatrixViewModel.cs:395-409`, `SVM:9209-9217` (`EnsureDefault*AudioRoutingSends`), hint at `AudioPage.xaml:911` | Missing | Sends exist only after an operator click (`AppModel.swift:2686-2705`); the empty list is never pushed (`:2780`) | **High**: a fresh Mac console routes nothing. The core sets `expectsAudio = !audioRoutingSends_.empty()` (`MediaCore.cpp:1519`), so check at runtime whether recording and stream audio work before any manual routing | M | shell |
| 45 | Add, rename and delete buses | `AudioPage.xaml:834-835`, `:865`, `:897-901`; panel `:56-68` | Missing | none | Med | M | shell (core accepts `bus-*`) |
| 46 | Bus LISTEN (PFL to headphones) | `AudioPage.xaml:869-872`, `AudioRoutingMatrixViewModel.cs:480-499` | Missing | `monitorBusId: ""` (`AppModel.swift:2785`) | Med: no audition of a bus | S | shell (`MediaCore.cpp:7256-7263`) |
| 47 | Bus sends to MASTER / MON / STREAM, plus bus output gain | `AudioPage.xaml:877-896` | Missing | `busSends: []` (`AppModel.swift:2785`) | Med: no subgroup or aux workflow | M | shell (`MediaCore.cpp:3336-3352`) |
| 48 | Clear all sends for a source | `AudioRoutingMatrixPanel.xaml:91-96` | Missing | none | Low | S | n/a |
| 49 | Routed-count summary per bus | `AudioPage.xaml:868` | Missing | none | Low | S | n/a |
| 50 | Mix and routing re-sent after a core restart | Windows rebuilds the full wire on every sync (`SVM` `BuildSpinePayload`, `:11320`) | Missing | `onConnected` (`AppModel.swift:780-791`) doesn't call `scheduleMixSync()` or re-push routing. Mutes, gains, inserts and mastering are lost in the core while the Mac UI still shows them | **High**: after a restart the console no longer matches what is on air | S | shell |
| **Monitoring** |
| 51 | Monitor enable and volume (SETUP) | `AudioPage.xaml:128-146` | Same | `AudioConsole.swift:60-75`; revisioned (`M/MonitorControlProjection.swift`) | n/a | n/a | n/a |
| 52 | Monitor output device picker | `AudioPage.xaml:121-125` | Missing | `MonitorControlProjection.swift:88-89` hard-codes `deviceId: ""`, "System default output" | **Med-High**: the operator can't put headphones on a separate interface | S | shell (CoreAudio resolves by UID: `N/modules/CoreAudioMonitorOutputAdapter.cpp:220-242`) |
| 53 | Monitor status text | `AudioPage.xaml:115` | Same | `AudioConsole.swift:71-74` | n/a | n/a | n/a |
| 54 | Monitor toggle and volume on the master rail | `AudioPage.xaml:1476-1484` | Same | `AudioConsole.swift:398-409` | n/a | n/a | n/a |
| 55 | Monitor persisted across restarts | prefs `:119-121` (enabled, device, volume) | Partial | `AppModel.swift:484-485`, `:560-561`: enabled and volume only (there is no device setting) | Low | S | n/a |
| **Local audio** |
| 56 | Local audio source (input or loopback picker, Source toggle) | `AudioPage.xaml:158-176`, `SVM:3580-3606` | Missing | none. `AudioConsole.swift:139` labels a `local-machine-audio` strip but nothing can create one | **High**: no host mic or system audio on the console without a capture-card pairing | M | shell (`N/modules/CoreAudioCaptureSourceAdapter.cpp:97-104`; loopback needs a BlackHole-class device) |
| 57 | Per-capture microphone pairing (Sources tab) | Windows Sources tab "Pair a microphone" | Same | `M/AudioInputs.swift:50-108` | n/a | n/a | n/a |
| **Master processor (SETUP)** |
| 58 | MASTER IN / Bypass | `AudioPage.xaml:203-204` | Same | `AudioConsole.swift:552-560` | n/a | n/a | n/a |
| 59 | A/B compare slots | `AudioPage.xaml:221-233`, `SVM:3242` | Missing | none | Med | M | n/a |
| 60 | Starting-point presets (-14 / -16 / -23 / neutral) | `AudioPage.xaml:234-249`, `SVM:3271` | Missing | none | Med: no quick way to hit a delivery spec | S | n/a |
| 61 | Reset stage menu | `AudioPage.xaml:254-277`, `SVM:3284` | Missing | none | Low | S | n/a |
| 62 | User presets: save, apply, rename, delete | `AudioPage.xaml:299-329`, `SVM:3373` | Missing | none | Med | M | n/a |
| 63 | Rack persists across restarts | `AudioPage.xaml:283`, prefs `:228-230` | Missing | `mastering` isn't in ShellPrefs (`AppModel.swift:484-513`, `:2678`) and isn't re-sent on connect | **High**: the master chain silently resets to bypass on relaunch | S | n/a |
| 64 | Integrated LUFS guide meter with target marker | `AudioPage.xaml:351-363` (`W/Controls/MasteringGuideMeter`) | Missing | The core publishes `masterMeter.integratedLufs` (`CoreStateCapture.cpp`, `masterMeterState`); the Mac reads only `shortTermLufs` (`AppModel.swift:946`) | **Med-High**: can't check delivery loudness | S | shell |
| 65 | True-peak guide meter with ceiling marker | `AudioPage.xaml:367-379` | Partial | Number only, in the footer (`M/StudioFooter.swift:843`) | Med | S | shell |
| 66 | Master L/R meters in SETUP | `AudioPage.xaml:387-400` | Missing | none in SETUP | Low | S | n/a |
| 67 | Tone curve preview | `AudioPage.xaml:431` | Missing | none | Low | M | n/a |
| 68 | INPUT trim, ±12 dB | `AudioPage.xaml:476` | Same | `AudioConsole.swift:568-573` | n/a | n/a | n/a |
| 69 | FILTER: high-pass 0-300 Hz and low-pass 0-20 kHz | `AudioPage.xaml:487-491` | Partial | High-pass 0-200 only; `MasteringParams` has no `lowPassHz` (`AppModel.swift:242-266`, `AudioConsole.swift:574-579`). The stage dims at `highPassHz > 20`, which can show a dim (no-op) stage while 1-20 Hz is applied (honesty rule) | Low | S | shell (core reads `lowPassHz`: `MediaCore.cpp:2936`) |
| 70 | TONE: low, presence, high at ±12 dB | `AudioPage.xaml:502-510` | Partial | ±6 dB (`AudioConsole.swift:580-594`) | Low | S | n/a |
| 71 | RIDE: target picker (-14 / -16 / -23) and max ride | `AudioPage.xaml:521-533` | Different | Continuous target slider, -24 to -9 (`AudioConsole.swift:595-604`) | Low | S | n/a |
| 72 | GLUE: amount, ratio, makeup, attack, release, 3-band multiband | `AudioPage.xaml:543-592` | Partial | Amount only (`AudioConsole.swift:605-610`) | Med | M | shell (core reads `glue*`: `MediaCore.cpp:2943-2950`) |
| 73 | WIDTH | `AudioPage.xaml:605` | Same | `AudioConsole.swift:611-616` | n/a | n/a | n/a |
| 74 | CEILING: master protection toggle and ceiling -6 to -0.5 | `AudioPage.xaml:615-620` (`MasterLimiterEnabled`) | Partial | Ceiling -6 to -0.1; no protection toggle, `limiterEnabled: true` is hard-coded (`AppModel.swift:3001`) | Low | S | shell |
| 75 | Ride readout | Windows `MasterLimiterSummary` | Same | `AudioConsole.swift:562-565` | n/a | n/a | n/a |
| **Master rail and metering** |
| 76 | Master LUFS number | `AudioPage.xaml:1457-1458` (`loudnessLufs`, `W/ViewModels/TransportViewModel.cs:259-261`) | Different | `shortTermLufs` (`AudioConsole.swift:372-382`) | Low: a different metric under the same label | S | n/a |
| 77 | Master L/R meters on the rail | `AudioPage.xaml:1464-1472` (L and R) | Partial | Both bars show mono `masterLevel` (`AudioConsole.swift:395-396`) even though `masterLevelLeft/Right` exist (`AppModel.swift:341-342`) | Med: can't see stereo imbalance on the console | S | n/a |
| 78 | Limiter activity on the rail | `AudioPage.xaml:1459` (`SVM:1288`) | Partial | The rail shows Live / Standing by / meter warning (`AudioConsole.swift:385-393`); limiter state is footer-only (`StudioFooter.swift:61-63`, `:846`) | Low | S | n/a |
| 79 | Meter staleness warning (#870) | `SVM:1217` (`AudioObservationPolicy.MeterWarning`) | Same (plus a poll allowance) | `M/AudioObservationPolicy.swift:39-68`, `AudioConsole.swift:385-388` | n/a | n/a | n/a |
| **Effective mute** |
| 80 | Effective mute sent to the core includes the Zoom mute (`ResolveEffectiveAudioMute`) | `SVM` (#481) | Different (harmless) | `AppModel.swift:2988` sends `strip.muted` only. Zoom sends no audio while a guest is muted, so this is acceptable | Low | n/a | n/a |
| **Control API** |
| 81 | `audio.*` control actions (zoomMode, monitor, route, masterLimiter, mastering, vst.scan) | `W/Services/StudioControlSurface.cs:86-87`, `:638-662` | Missing | `M/ControlSurface.swift` dispatch has no `audio.*` case | Med: Companion and HTTP can't drive audio on the Mac (overlaps the control-API audit) | M | shell |

### Status counts (81 rows)

| Status | Rows | Count |
|---|---|---|
| Same | 1, 2, 9, 10, 11, 13, 18, 19, 20, 22, 24, 31, 39, 40, 41, 42, 51, 53, 54, 57, 58, 68, 73, 75, 79 | **25** |
| Partial | 6, 8, 17, 23, 26, 28, 33, 36, 38, 43, 55, 65, 69, 70, 72, 74, 77, 78 | **18** |
| Different | 4, 7, 14, 32, 71, 76, 80 | **7** |
| Missing | 3, 5, 12, 15, 16, 21, 25, 27, 29, 30, 34, 35, 37, 44, 45, 46, 47, 48, 49, 50, 52, 56, 59, 60, 61, 62, 63, 64, 66, 67, 81 | **31** |

Core needed: only the VST rows (32-38) are blocked natively; that is effectively one blocker, the
Win32-only plug-in host transport. Every other gap is shell-only work against a core that already
supports it on macOS.

---

## 3. Top 10 gaps by operator impact

1. **Strip set comes from the core echo (rows 4, 5).** A source that arrives after the first console edit never gets a strip, so the FADER LAW drops it. The `"media"` alias strip and row are missing too. Late guests and media clips are silent and missing from the console. Shell, M.
2. **No default routing (row 44).** A fresh Mac console stages no master / pgm / stream / mon sends, and the core gates `expectsAudio` on sends existing. Shell, M (check record and stream audio at runtime first).
3. **Mix and routing not re-sent after a core restart (row 50), and the A1-mute echo (§1).** Mutes, gains, inserts and mastering vanish in the core while the UI still shows them, and the echo then rewrites the A1's mutes to unmuted. Shell, S.
4. **Pre-mute input meters missing (row 15).** A muted, talking guest reads as dead. This is the #481 correctness rule. Shell, S.
5. **Zoom audio topology toggle missing (row 3).** There is no per-guest ISO or Zoom program mix choice, and no matching routing policy. Shell, M.
6. **Local audio source missing (row 56).** No host mic or system loopback on the console. Shell, M.
7. **Mastering rack not persisted or re-sent (row 63).** The master chain silently falls back to bypass after a relaunch or core restart. Shell, S.
8. **VST3 inserts can't load, process or open an editor (rows 34-35).** Every plug-in is dead on macOS. Native, XL (Win32-only plug-in host transport).
9. **ISO buses 2-8 and aux buses absent (row 43).** Per-guest ISO audio is limited to one stem. Shell, S.
10. **No integrated LUFS or true-peak guide meters, and the master rail is mono (rows 64, 65, 77).** The operator can't confirm delivery loudness or stereo balance on the Audio tab. Shell, S.

Next in line: monitor device picker (52), ZOOM MUTED badge (12), compressor GR meter (21),
bus LISTEN, sends and management (45-47), A/B compare and presets (59-62).

---

## 4. Things the Mac has that Windows doesn't

- **Monitor feedback-risk warning.** The Mac shows "Monitor output matches the loopback device — feedback risk" (`M/AudioConsole.swift:80-83`, from core `monitorFeedbackRisk`). Windows never shows it: there is no `FeedbackRisk` in `W/`.
- **A dedicated PLUG-INS surface** with an honest host-capability chip (HOST ABSENT / SCAN ONLY / HOST LIVE), host respawn-gave-up and editor-error callouts (`M/VstInserts.swift:351-403`), and probe badges suppressed where a probe can't run.
- **A meter-age poll allowance.** The staleness policy subtracts one poll period so a healthy polled shell never flashes "meters aging" (`M/AudioObservationPolicy.swift:9-15`, `:46-49`). This is Mac-specific because only the Mac polls.
- **A consistent monitor volume scale.** Both Mac sliders use 0-1 (`AudioConsole.swift:65-69`, `:403-406`). On Windows, the master-rail monitor slider is `Minimum="0" Maximum="100"` (`AudioPage.xaml:1479-1481`), bound to `AudioMonitorVolume`, which `NormalizeAudioMonitorVolume` clamps to 0-1 (`SVM:9956-9957`). The SETUP slider is 0-1 (`AudioPage.xaml:136-139`). Likely a Windows bug: any rail value above 1 clamps to 100%. Not checked at runtime.

## 5. Notes and caveats

- Row 44's severity depends on how the core behaves with zero routing sends (the legacy DevSafe sum vs `expectsAudio=false`). Check record and stream audio on a fresh Mac launch before sizing the fix.
- §1's FADER LAW finding follows from `CoreStateCapture.cpp:1291` vs `:1436` together with Mac sync behaviour. Confirm with a live join: touch a fader, then admit a second guest and look for the `[audio] FADER LAW` or `unrouted` log line.
- Guest lip sync (row 30): I didn't check core support on macOS.
