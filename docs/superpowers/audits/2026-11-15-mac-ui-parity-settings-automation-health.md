# macOS UI parity audit: Automation, Settings, Health, Zoom tab, pop-outs, shortcuts, Studio workspace (2026-11-15)

Tree: worktree `mac-zoom-signin @ 060d6fa7`, which is the build the owner has installed on the Mac.
This is a read-only audit. I read every Mac claim in code. Nothing was launched.

Path prefixes used below:
- **W** = `native-shell/CoreVideoPro.WinUI/`
- **M** = `mac-shell/Sources/CoreVideoProShell/`
- **C** = `native-shell/CoreVideoPro.Control/`

Scope: everything named in the brief except the Studio footer (already matched in `3103b311`).

Not repeated here, only referenced:
- `2026-11-14-mac-parity-studio.md`: Tiles, Take/FTB internals, the scene catalog.
- `2026-11-14-mac-parity-behaviour.md`: recording, ISO, NDI and capture behaviour.
- `2026-11-14-mac-parity-social-comms.md`: Comms, telemetry and crash upload, the control surface design.
- `2026-11-14-mac-parity-graphics.md`: the graphics playlist.

Owner decisions this audit honours:
- Auto director, Set & Forget and Magic Scene are retired for 1.0.
- OHG is a deliberate non-port.
- The Mac has no uploader (local support bundle only).

Status key:
- **Same**: equivalent operator outcome.
- **Partial**: present, but missing a sub-behaviour.
- **Different**: the same need is met by another mechanism, or the same name does something else.
- **Missing**: not present.
- **N/A**: does not apply to macOS, or is a Windows-only orphan.

Native = native core or engine work needed. Shell = Swift-only.

Two facts change the picture since the 11-14 audits:
1. #1211 and later commits landed Space/Cmd+Return/Cmd+Shift+B (`M/TransportKeys.swift`), the control API (`M/ControlApi.swift`, `M/ControlHttpServer.swift`), the scene removal and rename rules (`M/AppModel.swift:1923-1997`), and multiview tile-click cueing (`M/AppModel.swift:2055-2066`). Several "Missing" rows from 11-14 are now Same.
2. Two Windows surfaces in scope are **orphans on Windows itself**:
   - The Automation tab has no nav button (`W/Views/StudioWorkspace.xaml:174-242`). It is reachable only through `ParseTab("automation")` at `W/ViewModels/StudioTabPlumbing.cs:35`.
   - `DiagnosticsWindow` (`W/Views/DiagnosticsWindow.xaml`) has no caller of `OpenDiagnosticsCommand` (`W/ViewModels/StudioViewModel.cs:4910`).
   - Health on Windows opens the **Zoom tab's** "Health and support" expander (`W/Views/StudioWorkspace.SceneNaming.cs:8-12`).

---

## A. Automation (incl. external automation)

| # | Feature | Windows | Mac status | Mac evidence | Impact | Size | Native/Shell |
|---|---|---|---|---|---|---|---|
| A1 | Automation page reachability | No nav button. Page host `W/Views/StudioWorkspace.xaml:745-748`. Only `ParseTab` at `W/ViewModels/StudioTabPlumbing.cs:35` reaches it | Same | Not in `StudioTab.navigation` (`M/AppModel.swift:30-33`). Hosted at `M/ShellApp.swift:1227` | Low: hidden on both platforms | — | — |
| A2 | "Manual production" explanation copy | `W/Views/AutomationPage.xaml:6-8` | Same | `M/ShellApp.swift:1859-1869`. Mac copy leaves out the Tiles / Stream Deck guidance | Low | S | Shell |
| A3 | "Fill free inputs when participants join" toggle (persisted, default on) | `W/Views/AutomationPage.xaml:9-10`; default `W/ViewModels/MagicScene/MagicSceneCoordinator.cs:458` | Missing | Auto-fill always runs: `M/AppModel.swift:1026-1040`. No pref field in `M/ShellPrefs.swift:86-122` | Low–Med: a Mac operator cannot stop guests landing in free inputs. On Windows the toggle sits on an unreachable page, so the practical gap is small | S | Shell |
| A4 | Retired scene automation (Auto/Set & Forget/Magic Scene) hard-off | Gated per `docs/reference/first-release-scene-control.md` | Same | `sceneAutomationAvailable = false` at `M/AppModel.swift:414`. `AutomationExtrasSection` (`M/AutomationExtras.swift:284-360`, "Run Magic Scene") is never mounted: dead code | Low (cleanup only) | S | Shell |
| A5 | Remote control actions (Companion / HTTP / WebSocket) | About 100 actions, `C/ControlActionRegistry.cs:45-248`: zoom.join/leave, transition.set, engine, virtualcam, NDI, view.setMode, input.\*, source.\*, scene.cueWithRoutes/route.set/restoreStarters, Tiles, graphics.\* | Partial | 1.0 subset of 9 actions (Take, FTB toggle/set, record/stream toggle/set, scene.select/list): `M/ControlApi.swift:14-19, 63-80`. Loopback by default, token required on LAN: `M/ControlHttpServer.swift:42, 61-83` | Med: Stream Deck pages built on Windows (transition, input assign, lower third, join/leave) fail with "Unknown action" | L | Mostly Shell (graphics.\* needs native graphics on Metal) |
| A6 | OSC transport | `C/Http/Osc/` | Missing | Only the OSC *address strings* appear in the manifest (`M/ControlApi.swift:92-97`). The transport is HTTP + WS only (`M/ControlHttpServer.swift:1-3`) | Low–Med: OSC-driven rigs (QLab, TouchOSC) cannot reach the Mac | M | Shell |

## B. Settings: Windows Production Settings window vs Mac Settings tab

| # | Feature | Windows | Mac status | Mac evidence | Impact | Size | Native/Shell |
|---|---|---|---|---|---|---|---|
| B1 | Surface and entry | Separate window with 7-section left nav (`W/Views/ProductionSettingsWindow.xaml:56-90`). Footer "More" deep-links to a section: `W/Views/StudioWorkspace.xaml:1004,1113`, `W/Views/ProductionSettingsWindow.xaml.cs:48`, `W/ViewModels/StudioViewModel.cs:4885-4898` | Different | A top-nav "Settings" tab (`M/AppModel.swift:30-33`), one scrolling page (`M/SettingsPane.swift:9-218`). Footer "More" switches to the tab with no section anchor (`M/StudioFooter.swift:534,567`) | Low | S | Shell |
| B2 | Program buffer frames (D3D11) | `W/Views/ProductionSettingsWindow.xaml:132-137` | N/A | Metal renderer, no D3D program buffer | — | — | — |
| B3 | Canvas size and FPS | `…ProductionSettingsWindow.xaml:144-151`. Persisted at `W/ViewModels/StudioViewModel.cs:12064-12067` | Partial | Pickers at `M/SettingsPane.swift:50-82`, applied through `M/AppModel.swift:1291-1300`. **Not persisted**: no canvas field in `M/ShellPrefs.swift:86-122`, and the defaults at `M/AppModel.swift:1202-1204` are 1920×1080@60 | **High**: a 4K or 30 fps show silently comes back as 1080p60 on the next launch | S | Shell |
| B4 | GPU-tier limit override, tier status and notice | `…ProductionSettingsWindow.xaml:158-166` | Missing | No equivalent. Health only shows `profile.maxProgramResolution` (`M/Diagnostics.swift:940-942`) | Low | M | Shell (the core already publishes the profile) |
| B5 | Independent stream and recording render profiles (size, fps, codec, bitrate, audio kbps) | `…:172-251` (profiles), `:339-384` (stream), `:692-735` (recording) | Missing | Stream and record both inherit the canvas (`M/AppModel.swift:1291-1300`). Recording quality is hard-coded `"high"` (`M/AppModel.swift:3245`). No recording bitrate, no audio kbps | Med: you cannot stream 720p while recording 1080p, a common bandwidth compromise | M | Shell + native check (does AVF honour per-output size?) |
| B6 | Streaming platform preset (YouTube/Twitch/… starting points) | `…:275-285` | Missing | — | Low | S | Shell |
| B7 | Stream bitrate | `…:299-305` | Partial | Slider 1–20 Mbps at `M/SettingsPane.swift:109-119`. Not persisted (`M/AppModel.swift:1205`, absent from ShellPrefs) | Low | S | Shell |
| B8 | Stream codec | `…:353` | Partial | H.264 (VideoToolbox) only: `M/AppModel.swift:1229-1231`, picker disabled at `M/SettingsPane.swift:120-132` | Low | M | Native (HEVC/AV1 encode) |
| B9 | Advanced encoder settings: hardware encoder, keyframe interval, rate control, H.264 profile, B-frames, Enhanced RTMP | `…:370-412` | Missing | Footer states the fixed recipe "1080p30 H.264 … 6 Mbps CBR · AAC 160 kbps" (`M/StudioFooter.swift:563`) | Low–Med: some CDNs require a 2 s keyframe interval or a fixed profile | M | Shell + core payload check |
| B10 | RTMP server URL and stream key | `…:433-443` | Same | `M/SettingsPane.swift:157-170`, footer `M/StudioFooter.swift:546-548`. Key is kept in the Keychain (`M/StreamKeychain.swift`). There is no RTMP/RTMPS picker; the URL scheme carries it | — | — | — |
| B11 | SRT destination (mode, host, port, latency, key length, stream id, passphrase) | `…:512-536` | Missing | Footer says "SRT and HLS destinations are Windows-only for now." (`M/StudioFooter.swift:559`) | Med | L | Native check (shared sender on macOS) + Shell |
| B12 | HLS push (HTTP PUT playlist URL) | `…:547-550` | Missing | Same footer line | Low | M | Native check + Shell |
| B13 | NDI output, program/group names, graphics key | `…:456-488` | Missing | No NDI on Mac (behaviour audit; no match in mac-shell) | Med | XL | Native |
| B14 | Local machine audio source (device + enable) | `…:570-590` | Different | Audio is paired per capture input in Sources (`M/ShellApp.swift:1740-1750`). There is no standalone "local mic into the mix" source | Low | M | Shell (+ native check) |
| B15 | Monitor output device picker | `…:596-600`. Persisted at `W/ViewModels/StudioViewModel.cs:12072` | Partial | System default output only: `M/SettingsPane.swift:177`, `M/AudioConsole.swift:60` ("Monitor (system default output)") | **Med**: headphones or an interface means changing the macOS default output, which also moves Zoom's and every other app's audio | M | Native (CoreAudio monitor adapter device id) + Shell |
| B16 | Monitor on/off and volume | `…:602-618` | Same | `M/SettingsPane.swift:176-192`. Persisted at `M/ShellPrefs.swift:90-91` | — | — | — |
| B17 | Master bus meters, LUFS, true peak, limiter toggle | `…:626-670` | Same (moved) | Master rack on the Audio tab (`M/AudioConsole.swift:551-565`). LUFS, true peak and limiter rows in Health (`M/Diagnostics.swift:1251-1281`) | Low | — | — |
| B18 | Recording format | `…:721` | Partial | MP4/MOV at `M/SettingsPane.swift:133-139`. Not persisted (`M/AppModel.swift:1232`) | Low | S | Shell |
| B19 | Recording filename prefix | `…:755` | Partial | `M/SettingsPane.swift:140-144`. Not persisted (`M/AppModel.swift:1233`) | Low | S | Shell |
| B20 | Recording target folder: Browse… / Open folder | `…:758-771` | Partial | Fixed `~/Movies/CoreVideoPro` (`M/AppModel.swift:3220-3224`, `M/SettingsPane.swift:12-15`). Only "Reveal" (`:145-156`) | **Med**: an operator cannot record to an external or fast drive | S | Shell |
| B21 | FFmpeg bin folder override | `…:785-806` | N/A | The Mac ships its own ffmpeg (RTMP via the core's ffmpeg sender, `M/AppModel.swift:2590`) | Low | — | — |
| B22 | Multiview layout mode | `…:830`; options at `W/ViewModels/StudioViewModel.cs:1883-1889` | Partial | `M/SettingsPane.swift:86-96`, plus the menu at `M/ShellApp.swift:764-777`. Not persisted (`M/AppModel.swift:767`) | Low | S | Shell |
| B23 | Multiview tile count (max = hardware tier) | `…:836-843` | Partial | Stepper fixed at 1…16 (`M/SettingsPane.swift:97-105`). Not persisted (`M/AppModel.swift:1234`) | Low | S | Shell |
| B24 | Multiview overlay toggles: labels, tally, audio meters, clock | `…:852-867` | Missing | Labels and tally always on (`M/ShellApp.swift:847-893`). Clock always in the header (`:762`). No meter overlay | Low | S | Shell |
| B25 | OHG show config section | `…:880-1132` | N/A | Deliberate non-port | — | — | — |

## C. Zoom tab (Windows `SettingsPage.xaml`, bound to the "Zoom" nav button)

| # | Feature | Windows | Mac status | Mac evidence | Impact | Size | Native/Shell |
|---|---|---|---|---|---|---|---|
| C1 | Meeting URL/ID, display name, Webinar | `W/Views/SettingsPage.xaml:58-68`. Locked while joining or in a meeting (`CanEditJoinFields`) | Partial | `M/ShellApp.swift:1337-1346`. Fields are never disabled | Low | S | Shell |
| C2 | Join status and meeting status line | `…SettingsPage.xaml:70-76` | Same | `M/ShellApp.swift:1312-1331, 1352-1357` (shows `zoomJoinStatus`) | — | — | — |
| C3 | Recent meetings | `…:79-90`: hidden when empty, summary rows, selecting fills the field and clears the selection | Partial | `M/ShellApp.swift:1358-1375`: always shown, raw strings, and the label shows the newest entry as if it were selected | Low | S | Shell |
| C4 | Sign in with Zoom / Sign out + OAuth status | `…:92-113` | Same | `M/ShellApp.swift:1377-1393` | — | — | — |
| C5 | Join / Leave + why-disabled hint | `…:115-125, 142-144` (`CanJoinZoom` + `JoinActionHint` tooltip and text) | Partial | `M/ShellApp.swift:1394-1411`: disabled only when the ID is empty, and the hint text is static | Low | S | Shell |
| C6 | "Join and end my other Zoom session" (#475) | `…:130-135`; `W/ViewModels/SettingsViewModel.cs:721` (`endOtherMeeting: true`) | Missing | Not in mac-shell. Only the Windows engine handles `end_other_meeting` (`native/zoom-engine/engine/main.cpp`); the macOS engine does not | **Med–High**: if the owner's own Zoom app is already in the meeting, the join fails and the Mac has no recovery path in the app | M | Native (macOS engine) + S Shell |
| C7 | Open in Zoom app | `…:136-140` | Same | `M/ShellApp.swift:1403-1405` | — | — | — |
| C8 | Zoom status: SDK readiness chip (ready/blocked) + guidance | `…:148-167`; `W/ViewModels/SettingsViewModel.cs:273-279` | Partial | Fixed text "Ready to join Zoom" / "In the meeting — N in room" (`M/ShellApp.swift:1418-1436`), regardless of SDK readiness. Readiness shows only in Health (`M/Diagnostics.swift:1174-1185`) | **Med**: a blocked SDK or bad credentials are not shown where the operator joins | S | Shell |
| C9 | Health and support (diagnostics embedded in the Zoom tab) | `…:169-172` | Different | Separate Health tab (`M/AppModel.swift:30-43`) | Low | — | — |
| C10 | Optional workspaces (OHG toggle) | `…:174-182` | N/A | Non-port | — | — | — |
| C11 | Anonymous telemetry opt-in + "Preview what's sent" | `…:187-223` | Missing | "deliberately no uploader and no telemetry" (`M/Diagnostics.swift:20`). Owner decision, social-comms audit §4 | Low (opt-in, default off) | M | Shell |

## D. Diagnostics / Health

| # | Feature | Windows | Mac status | Mac evidence | Impact | Size | Native/Shell |
|---|---|---|---|---|---|---|---|
| D1 | Entry points (header Health, footer Health) | `W/Views/StudioWorkspace.xaml:234, 1325` → Zoom tab expander (`W/Views/StudioWorkspace.SceneNaming.cs:8-12`) | Same | Health nav tab (`M/AppModel.swift:30-43`) and footer Health (`M/StudioFooter.swift:789`) | — | — | — |
| D2 | Standalone diagnostics window (1040×760) | `W/Views/DiagnosticsWindow.xaml`, `W/ViewModels/StudioViewModel.cs:4910-4923`; no caller | N/A | Orphan on Windows | — | — | — |
| D3 | Engine / native core / native audio runtime status | `W/Views/DiagnosticsView.xaml:32-44` | Same | Core section `M/Diagnostics.swift:929-955` (bridge, renderer, profile, binaries, core messages) | — | — | — |
| D4 | Audio observation (meter age) status | `W/Views/DiagnosticsView.xaml:43` (`AudioObservationStatus`) | Partial | The policy exists (`M/AudioObservationPolicy.swift`), but Health has no observation row (no "observation" in `M/Diagnostics.swift` UI) | Low | S | Shell |
| D5 | Export support bundle + status | `…DiagnosticsView.xaml:49-52, 284` | Same | `M/Diagnostics.swift:1348-1380, 1391`. Redacted, with Reveal | — | — | — |
| D6 | "Extra logs" (verbose diagnostics) toggle + status | `…:64-70` | Missing | No verbose toggle in mac-shell | **Med**: support cannot ask an operator to turn on verbose logging and reproduce | S–M | Shell + core log-level check |
| D7 | Zoom section | `…:89-97` | Same | `M/Diagnostics.swift:1174-1218` (more detail than Windows) | — | — | — |
| D8 | Program / output session | `…:100-108` | Same | `M/Diagnostics.swift:958-1117` (program frame, encoder, recording) | — | — | — |
| D9 | Stream + ffmpeg runtime | `…:111-119` | Same | `M/Diagnostics.swift:1119-1172` (sender rows and warnings) | — | — | — |
| D10 | Audio meters: source summary, bus taps, validation checklist, first issue, full-chain validation | `…:122-140` | Partial | `M/Diagnostics.swift:1251-1281` has mix, LUFS, peak, limiter and strips, but no validation checklist or first-issue line | Low–Med | M | Shell |
| D11 | Monitor + control recovery summary | `…:143-153` | Partial | Monitor row and feedback risk at `M/Diagnostics.swift:1268-1272`. No recovery summary | Low | S | Shell |
| D12 | Local audio / capture audio signal | `…:156-166` | Missing | — | Low | S | Shell |
| D13 | Native lower third status + timing | `…:169-177` | Missing | — | Low | S | Shell |
| D14 | Media playback status | `…:180-195` | Missing | — | Low | S | Shell |
| D15 | Capture fleet + per-feed health rows | `…:198-238` | Partial | Capture devices at `M/Diagnostics.swift:1220-1249`. Zoom feed health is on Sources (`M/ShellApp.swift:1487`), not in Health | Low | S | Shell |
| D16 | Runtime readout (recovery, warnings, senders, events) | `…:241-280` | Same | Warnings `M/Diagnostics.swift:1431`, senders `:1138-1170` | — | — | — |
| D17 | Crash-report consent bar + "Send report" | `W/Views/StudioWorkspace.xaml:1337-1354` | Missing | The bundle lists crash files only (`M/Diagnostics.swift:20, 422-425`). Owner decision | Low | M | Shell |

## E. Pop-out windows

| # | Feature | Windows | Mac status | Mac evidence | Impact | Size | Native/Shell |
|---|---|---|---|---|---|---|---|
| E1 | Multiviewer pop-out window (single instance) | `W/Views/MultiviewPopoutWindow.xaml:1-49` | Same | `Window("Multiviewer")` at `M/ShellApp.swift:124-131` | — | — | — |
| E2 | Pop-out triggers in the multiview header (Pop out, P/P out) | `W/Views/StudioWorkspace.xaml:435-442, 469-479` | Same | `M/ShellApp.swift:792-797` | — | — | — |
| E3 | Pop in + main-window "popped out" placeholder | `W/Views/MultiviewPopoutWindow.xaml:32`; `W/Views/StudioWorkspace.xaml:522-534` | Different | The main multiview stays live (IOSurface has several consumers). No "Pop in" button; closing the window is the pop-in | Low (arguably better) | — | — |
| E4 | Pop-out tile interactions (click cues Preview, drag swaps inputs) | `W/Views/MultiviewPopoutWindow.xaml:39-40` | Partial | Click works: shared `MultiviewerCanvas` (`M/ShellApp.swift:812-818`). No drag-swap (see G21) | Low | M | Shell |
| E5 | Pop-out overlay toggles (labels, tally, meters, clock) | `W/Views/MultiviewPopoutWindow.xaml:41-46` | Partial | Clock always on, no meters (see B24) | Low | S | Shell |
| E6 | Pop-out header content | Title + input count + Pop in only (`W/Views/MultiviewPopoutWindow.xaml:21-33`) | Different | The pop-out reuses the full console header: layout menu, "Scene builder", "Room" and "Grade" chips (`M/ShellApp.swift:755-797`). "Scene builder" and "Room" switch the *main* window's tab, which may be on another screen | Low | S | Shell |
| E7 | Program/Preview pop-out (PVW left, PGM right, 16:9) | `W/Views/ProgramPreviewPopoutWindow.xaml:30-75` | Same | `M/ShellApp.swift:133-139, 217-250`. Adds a clock and a "waiting for … pixels" placeholder. The PGM tally is red where Windows uses amber | — | — | — |
| E8 | Resizable / maximisable chrome | `W/Views/MultiviewPopoutWindow.xaml.cs:62-79` | Same | Native macOS windows with full screen | — | — | — |

## F. Menu bar and keyboard shortcuts

| # | Feature | Windows | Mac status | Mac evidence | Impact | Size | Native/Shell |
|---|---|---|---|---|---|---|---|
| F1 | Space = Take (beats a focused button; ignored in text entry; consumed when Take is unavailable; first press only; Studio tab only) | `W/Views/StudioWorkspace.TransportHotkeys.cs:43-73` | Same | `M/TransportKeys.swift:40-73, 97-125`; `M/AppModel.swift:2282-2310`. Main window only; quit prompt takes priority | — | — | — |
| F2 | Ctrl+Enter = Take | `…TransportHotkeys.cs:21` | Same | Cmd+Return (owner call): `M/TransportKeys.swift:50-52` | — | — | — |
| F3 | Ctrl+Shift+B = FTB | `…TransportHotkeys.cs:22` | Same | Cmd+Shift+B: `M/TransportKeys.swift:53-55` | — | — | — |
| F4 | F1–F12 play graphics rows; Shift+F12 plays all out (Studio and Graphics tabs) | `W/Views/StudioWorkspace.xaml.cs:46-80` | Missing | No function-key handling in mac-shell. Depends on the graphics playlist port (graphics audit) | Med | L | Shell (+ native graphics on Metal) |
| F5 | Scene canvas keys: arrows nudge, Shift keeps aspect, Esc cancels a drag | `W/Controls/SceneCanvasEditorControl.xaml.cs:721-742` | Missing | `M/SceneCanvas.swift:99-210` is drag only: no key handling, no aspect lock, no snapping | Low | S | Shell |
| F6 | App menu bar commands | None: custom title bar (`W/MainWindow.xaml:1-31`, `W/Views/StudioWorkspace.xaml:139-141`) | Different | Default SwiftUI menus with only New Window removed (`M/ShellApp.swift:106-109`). No Take/FTB/Pop-out menu items and no `Settings` scene, so ⌘, does nothing. This is a macOS-convention gap, not a Windows gap | Low | S | Shell |
| F7 | Close/quit guard while recording or streaming | `W/ViewModels/StudioViewModel.CloseGuard.cs:12-25` | Same | `M/QuitGuard.swift:1-20`; prompt at `M/ShellApp.swift:396-430` (Return = Keep running) | — | — | — |

## G. Studio workspace (header, show strip, scene rail, multiviewer, room rail; footer excluded)

| # | Feature | Windows | Mac status | Mac evidence | Impact | Size | Native/Shell |
|---|---|---|---|---|---|---|---|
| G1 | Brand mark + "CoreVideo Pro / Live production" | `W/Views/StudioWorkspace.xaml:150-166` | Same | `M/ShellApp.swift:450-456` | — | — | — |
| G2 | Top nav: Studio, Zoom, Sources, Scenes, Routing, Graphics, Audio, Comms, Media, Health, OHG | `…:174-242` | Partial | `M/AppModel.swift:30-43`: no Comms (social-comms audit), OHG omitted, and Settings added | Med (Comms; see the social-comms audit) | XL (Comms) | Native + Shell |
| G3 | CAPTURE toggle pill | `…:250-262` | Same | `M/ShellApp.swift:615-645` (three states: on, starting, off) | — | — | — |
| G4 | ZOOM status pill, always visible (red when offline, green when live) | `…:263-275` | Partial | "Zoom Live" pill only while in a meeting (`M/ShellApp.swift:461-463`). Offline has no indicator | Low | S | Shell |
| G5 | Update-available banner (once per launch, Get update / remind later) | `W/MainWindow.xaml:16-29` | Missing | No update check in mac-shell | Low–Med: Mac operators will not learn about fixes | M | Shell |
| G6 | SHOW strip: Add sources / Choose inputs / Ready / **On air** + "N inputs · N Zoom guests" | `…:281-307`; `W/ViewModels/StudioViewModel.cs:2859-2888` | Partial | `ReadinessStrip` (`M/ShellApp.swift:690-729`): core-ready and meeting text plus counts. Never says "On air" or "Choose inputs" | Low | S | Shell |
| G7 | Scene list: number badge, name, layout, PGM/PVW/idle status; click cues Preview | `…:325-358` | Same | `M/ShellApp.swift:935-984, 1060-1106`; `M/AppModel.swift:2068-2073` | — | — | — |
| G8 | Rename any scene from the rail (context menu → dialog) | `…:346`; `W/Views/StudioWorkspace.SceneNaming.cs:19-23` | Partial | Only the Preview scene, in the Scenes tab name field (`M/SceneCanvas.swift:56-62`; rules at `M/AppModel.swift:1923-1935`) | Low–Med: you must cue a scene to rename it | S | Shell |
| G9 | Remove any scene from the rail (on-air refused) | `…:347` | Partial | Scenes tab "Scene ▸ Delete" on the Preview scene only (`M/SceneCanvas.swift:47`; `M/AppModel.swift:1983-1997`) | Low | S | Shell |
| G10 | Duplicate per row (copy cued on Preview) | `…:364`; `W/ViewModels/StudioViewModel.cs:4339-4364` | Partial | Scenes tab menu, Preview scene only (`M/SceneCanvas.swift:45`; `M/AppModel.swift:1937-1962`) | Low | S | Shell |
| G11 | "Edit Tiles" on CoreVideo Tiles scenes | `…:359-361` | Missing | No Tiles in mac-shell (studio audit #9) | **High**: Tiles is the Windows 1.0 composition model ("Use CoreVideo Tiles to compose your show", `W/Views/AutomationPage.xaml:7`) | XL | Native + Shell |
| G12 | Open Scene builder | `…:369-379` | Same | `M/ShellApp.swift:960-975` | — | — | — |
| G13 | Scene background picker + import | `…:380-386` | Same | `M/ShellApp.swift:986-1058` (stills only; Import jumps to the Media tab) | — | — | — |
| G14 | Multiview header: title, **program resolution chip**, live-input count | `…:419-428`; `ProgramResolutionLabel` `W/ViewModels/StudioViewModel.cs:2802` | Partial | Chip hard-coded `"1080p60"` (`M/ShellApp.swift:758`). Rail hard-coded "1920x1080 · 60 fps" (`:947`). Both are wrong once the canvas changes | Low–Med: misleading at a glance | S | Shell |
| G15 | Wall clock in the header | `…:430-434` (toggleable) | Same | `M/ShellApp.swift:762-763` (always on) | — | — | — |
| G16 | **"Edit layout"**: in-place PREVIEW scene canvas overlay (drag, resize, snap, Shift aspect, nudge, Done), committed by Take/Update | `…:446-454, 536-570` | Different | The Mac "Edit layout" button is a *multiview layout-mode menu* (`M/ShellApp.swift:764-777`). Scene layout editing is only in the Scenes tab (`M/SceneCanvas.swift:13-97`), with no snap, aspect lock or nudge | **Med**: same label, different action. Re-framing mid-show costs a tab switch | M | Shell |
| G17 | "Add overlay": logo/bug layer on the PREVIEW scene with corner presets | `…:458-468`; `W/Views/OverlayLayerMenuBuilder.cs` | Missing | Only a global logo bug that rides every scene (`M/AppModel.swift:402`; `M/Overlays.swift:277`) | Low–Med | M | Shell |
| G18 | Tile click cues that source solo to Preview | `…:513`; `W/ViewModels/StudioViewModel.cs:4218-4248` | Same | `M/ShellApp.swift:812-818`; `M/AppModel.swift:2055-2066` (two reserved solo scenes, `060d6fa7`) | — | — | — |
| G19 | Drag one tile onto another to swap input slots | `…:514`; `W/ViewModels/StudioViewModel.cs:4254-4273` | Missing | No `onDrag`/`onDrop`/`draggable` anywhere in mac-shell | Low–Med | M | Shell |
| G20 | Tile labels and tally borders | `W/Controls/ShowMultiviewHost` | Same | `M/ShellApp.swift:847-893` | — | — | — |
| G21 | Master L/R meter overlay on the multiview | `W/Views/StudioWorkspace.xaml:517-519` | Missing | — | Low | S | Shell |
| G22 | Empty-state guidance | `…:522-534`; `MultiviewEmptyStatePolicy` | Same | `M/ShellApp.swift:821-835`; `M/StudioHealth.swift` | — | — | — |
| G23 | "Room" toggle shows or hides the room panel (and its ✕) | `…:482-490, 593-597` | Different | "Room" switches to the Zoom tab (`M/ShellApp.swift:789`). The rail is always shown (`:737-741`) | Low | S | Shell |
| G24 | Graphics toggle, On Air strip, compact playlist column (Play In/Out) | `…:492-500, 503, 678-703` | Missing | Only an "LT in/out" readout chip (`M/ShellApp.swift:778-788`). See the graphics audit | Med | L | Shell + native graphics |
| G25 | Room participants: name, role · health, talking, role badge; click selects the audio-processing target | `…:600-671`; `W/ViewModels/StudioViewModel.cs:4689-4693` | Partial | Name, hard-coded "Guest", talking, muted (`M/ShellApp.swift:1160-1196`). No role, health or selection | Low | S | Shell |

---

## Counts by status

| Area | Same | Partial | Different | Missing | N/A | Rows |
|---|---|---|---|---|---|---|
| A. Automation | 3 | 1 | 0 | 2 | 0 | 6 |
| B. Settings (Production Settings) | 3 | 9 | 2 | 8 | 3 | 25 |
| C. Zoom tab | 3 | 4 | 1 | 2 | 1 | 11 |
| D. Diagnostics / Health | 7 | 4 | 0 | 5 | 1 | 17 |
| E. Pop-outs | 4 | 2 | 2 | 0 | 0 | 8 |
| F. Menu bar / shortcuts | 4 | 0 | 1 | 2 | 0 | 7 |
| G. Studio workspace | 9 | 8 | 2 | 6 | 0 | 25 |
| **Total** | **33** | **28** | **8** | **25** | **5** | **99** |

## Top 10 gaps by operator impact

1. **Production settings are not persisted.** Canvas size and FPS, stream bitrate, recording format and prefix, and multiview layout and tile count are all session-only (B3, B7, B18, B19, B22, B23). A 4K or 30 fps show comes back as 1080p60. Effort: S, shell.
2. **No CoreVideo Tiles / Edit Tiles (G11).** This is the Windows 1.0 composition model. Effort: XL, native + shell.
3. **No "Join and end my other Zoom session" (C6).** The macOS engine also lacks `end_other_meeting`. If the operator's own Zoom app is already in the meeting, the join cannot be recovered in the app. Effort: M, native + S shell.
4. **Stream and recording cannot have separate render profiles (B5).** Recording quality is fixed at "high", with no bitrate or audio kbps controls. Effort: M.
5. **Recording folder is fixed to `~/Movies/CoreVideoPro` (B20).** There is no Browse, so an operator cannot record to an external drive. Effort: S, shell.
6. **Monitor output follows the system default device (B15).** Choosing a monitoring device means moving every other app's audio too. Effort: M, native + shell.
7. **Studio "Edit layout" does something else on Mac (G16).** It changes the multiview layout instead of opening an in-place Preview canvas editor. Rename, duplicate and remove work only on the cued scene in the Scenes tab (G8–G10). There is no Add overlay (G17) and no drag-swap (G19). Effort: M, shell.
8. **The Zoom tab status ignores SDK readiness (C8).** A blocked SDK or bad credentials surface only in Health. Effort: S, shell.
9. **No Studio graphics.** The playlist column, On Air strip and F1–F12 hotkeys are all absent (G24, F4). Effort: L; depends on the graphics port.
10. **The remote control surface is a 9-action subset with no OSC (A5, A6).** Windows Stream Deck pages for transition, input assign, lower third or join/leave fail on Mac. Effort: L, mostly shell.

Next in line:
- No "Extra logs" verbose toggle (D6).
- No SRT, HLS or NDI (B11–B13).
- No update banner (G5).
- Hard-coded "1080p60" labels (G14).

## What the Mac has that Windows does not

- **Passcode field** on the Zoom tab (`M/ShellApp.swift:1347-1350`). Windows relies on the link's `pwd=`.
- **Stream key in the Keychain** (`M/StreamKeychain.swift`). Windows persists it in production preferences (`W/ViewModels/StudioViewModel.cs:12053`).
- **Assign or unassign a participant straight from the Studio room rail** with +/− (`M/ShellApp.swift:1190-1196`). Windows' "Add Participant" button has no command (`W/Views/StudioWorkspace.xaml:673-674`).
- **"Join meeting / Manage meeting" button on the room rail** (`M/ShellApp.swift:1143-1155`).
- **PGM/PVW name chips on the scene rail** (`M/ShellApp.swift:949-952`).
- **"Grade" and "LT in/out" chips in the multiview header** (`M/ShellApp.swift:778-792`).
- **Media-core connection dot** in the header with state tooltip (`M/ShellApp.swift:663-686`).
- **Live main multiview while popped out.** Windows blanks the main view because its texture has a single consumer.
- **Clock and "waiting for pixels" text on the P/P pop-out.**
- **Pop-out restore at launch** via `COREVIDEO_OPEN_POPOUTS` (`M/ShellApp.swift:166-191`).
- **Native full-screen** for every pop-out.
- **Health extras** (`M/Diagnostics.swift:929-955, 1283-1312, 1333-1380`):
  - core and engine binary paths;
  - a perf scan of the core log;
  - Reveal buttons for the shell log, core log and bundle;
  - a crash-report listing in the bundle;
  - an "empty snapshot" warning.
- **System card in Settings** showing media core, renderer, virtual-camera state and Reveal prefs (`M/SettingsPane.swift:194-213`).
- **Hardened control API.** No CORS, a JSON content-type is required for POST, Host-header DNS-rebinding defence, browser-WS refusal in open mode, and constant-time token compare (`M/ControlHttpServer.swift:5-31`).
- **Windows-side orphans worth cleaning up:**
  - the unreachable Automation tab;
  - the uncalled `DiagnosticsWindow`;
  - the dead "Add Participant" button.
- **Mac-side dead code to clean up:** `AutomationExtrasSection` / `runMagicScene` (`M/AutomationExtras.swift:132-360`).
