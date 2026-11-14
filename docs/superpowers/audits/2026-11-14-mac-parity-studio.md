# macOS parity audit — Studio workspace, transport, scenes, Auto, Multiview, EngineHost

Date: 2026-11-14. Tree: `main @ 00838940` (Release 1.0.0). Method follows
`docs/mac-parity-plan.md` (2026-10-09): every claim about the Mac is a direct
`mac-shell/` read with file:line; anything inferred is marked **INFERRED**, anything
not checked is marked **UNVERIFIED**. OHG is a deliberate non-port and is ignored.
The "do not re-litigate" list in that plan's §2 stands (shared core gated on macOS,
mac-shell builds and is tested).

Window: `git log --since=2026-10-09 origin/main` = 959 commits. Only **6** touch
`mac-shell/` (04dc0173 #852, 7e75dce2 #926, 75bb8cca/be945982/fd0015e8 OAuth host
strings, 87bebbd7 #1205). Of those, only #1205 is in this area.

**Context that reframes half this area:** the owner decision of 2026-11-09
(`docs/reference/first-release-scene-control.md`) *retired* Set & Forget and the
legacy Magic Scene / Auto director for 1.0: "Scene selection and Take remain manual
through Studio, keyboard, and Stream Deck/Companion" and "CoreVideo Tiles owns
composition." So the #1146–#1150 / #1173 / #1144 Auto work is behind a hard-off
gate on Windows too, and the 1.0 parity bar for this area is the *manual* path.

## Summary

| Feature | Class | Effort | Deps | Mac evidence |
|---|---|---|---|---|
| 1. RC Studio footer / layout (#1205) | MAC-UI-ONLY | M | 2, 3, 5 | `ShellApp.swift:1097-1188` one-row bar; Take mid-bar, no FTB, no transition picker |
| 2. Native Fade to Black (#1205) | SHELL-LOGIC (S) + **native Metal fix** (S–M) | M total | Metal `hasFillColor` branch | zero `blackout`/FTB refs in mac-shell; `MetalCompositorAdapter.mm:820-842` no fill branch (INFERRED: fades to grey `0xff444444`) |
| 3. Take transition (Cut/Fade, default Fade 300 ms) | SHELL-LOGIC | S | — | `AppModel.swift:1776-1784` swap only; `ShellApp.swift:1139` hard-labelled "Cut" |
| 4. Take path correctness (immediacy, rollback) | SHELL-LOGIC | M | 3 | Take rides 120 ms debounced `syncScenes` (`AppModel.swift:1793-1799`); failure only warns (`:1859`) |
| 5. Spacebar Take / Ctrl+Enter / Ctrl+Shift+B (#1206) | MAC-UI-ONLY | S | 2, 4 | no `keyboardShortcut`/`NSEvent`/`onKeyPress` anywhere in mac-shell |
| 6. Remote transport/scene control (transport.take, transport.ftb.*, scene.list/select/cueWithRoutes/restoreStarters) | SHELL-LOGIC | XL | 2, 4, 9 | no HTTP/OSC/listener in mac-shell (grep empty) |
| 7. Scene naming / renaming (#1196) | SHELL-LOGIC | S | — | **BUG**: `AppModel.swift:1672-1677` rename drops `backgroundAssetId`; per-keystroke, no validation (`SceneCanvas.swift:57-61`) |
| 8. Scene removal rule (#1070) | SHELL-LOGIC | S | — | **BUG**: `AppModel.swift:1717-1726` deletes the on-air scene and swaps Program without a Take |
| 9. Scene catalog, starters, Restore starters, Tiles starter (#1199) | SHELL-LOGIC (catalog) / N/A-until-Tiles (starter) | M (+XL Tiles) | Tiles port | 3 built-ins `AppModel.swift:350-354`; no Tiles anywhere |
| 10. Auto director policy (#1146-#1150, #1173, #1144), Set & Forget, Magic Scene | N/A (retired for 1.0) | — | — | gated `AppModel.swift:371`, prefs `ShellPrefs.swift:65,119` |
| 10a. Free-input auto-fill *setting* (survives 1.0) | MAC-UI-ONLY | S | — | auto-fill unconditional `AppModel.swift:871-890`, no toggle |
| 11. Multiview placeholder ownership (#1172) | MAC-UI-ONLY | S | — | `ShellApp.swift:613-621` guidance keyed on tiles only, can overlay a live picture |
| 12. Multiview input inspector / versioned ten-row projection (#674 A-F) | SHELL-LOGIC | L | — | no `zoom-source-format`, format/age/REC chips; `SlotRow` `ShellApp.swift:1763` |
| 13. EngineHost (#960/#961) | N/A for 1.0 | (XL if ever) | — | Windows-only and off by default; Mac has no analogue |
| 14. Popout windows | SHARED-DONE (Mac equivalent exists) | — | — | `ShellApp.swift:78,86,95,102` |
| 15. Participant-list refresh (#919) | N/A (WinUI XAML lifetime) | — | — | SwiftUI `@Published`, no deferred ListView callbacks |
| 16. Production settings window | Partial — UNVERIFIED detail | — | outputs audit | `SettingsPane.swift` 269 lines vs Windows 7-section window |
| 17. Diagnostics / support bundle export | SHARED-DONE (export); SHELL-LOGIC (crash offer/upload) | M (upload) | owner decision | `Diagnostics.swift:353`; "deliberately no uploader" `Diagnostics.swift:20` |
| 18. Footer health line (stalled feeds / network congested, #1074) | SHELL-LOGIC | S | — | grep `congest|stalled feed` empty in mac-shell |

---

## 1. RC Studio footer and layout (#1205, #1086 cafe0523)

**Windows.** `StudioWorkspace.xaml` (604-line churn in #1205),
`Controls/StudioFooterLayoutPanel.cs`, `Services/StudioWorkspaceLayoutProbe.cs`. Ruling
text (`first-release-scene-control.md`): "The Windows Studio footer groups Graphics and
Outputs on the left, Take/FTB on the right, above a separate health/audio row."
"Take, transition choice, and the black FTB key are anchored to the far right." "Graphics
and output shortcuts form a left-aligned group with a fixed gap; only the space before
Take grows with the window." "Manual blend is excluded from the RC." "RC footer outputs use
compact Record, broadcast, webcam icons and an NDI badge." "Zoom and Health are direct top
navigation actions." Pure WinUI.

**Mac.** #1205 touched mac-shell only to *remove* automation (diff: `ShellApp.swift`
-85 lines, `AppModel.swift`, `AutomationExtras.swift`, `ShellPrefs.swift`). It removed the
Magic Scene pill and Set & Forget toggle from `TransportBar` and the Automation tab pill
(`ShellApp.swift:242`). It added **none** of the RC layout. The current Mac footer
(`ShellApp.swift:1097-1188`): LT chip, Preview/DSK chips, spacer, outputs label, **Take**,
Record, ISOs checkbox, StreamControl, VirtualCamChip. There's one row with no health row.
Take sits mid-bar rather than far right, and there is no FTB or transition selector.

**Class** MAC-UI-ONLY. **Effort** M. **Deps** §2, §3 (the controls the layout anchors).
NDI badge parity belongs to the outputs audit (UNVERIFIED here).

## 2. Native Fade to Black (#1205)

**Windows / core.** Core: `native/src/core/ProgramBlackout.h` (whole class),
`MediaCore.cpp` `applyCommandMutation` branch `type == "program-blackout"` reading
`enabled` (bool, required) and `durationMs` (default 300, clamped 0–5000); appended last in
`buildCompositorRenderPlan` (after Global graphics); advanced per render tick. Snapshot:
`sessionState.programBlackout` (bool) and `programBlackoutOpacity` (float)
(`CoreStateCapture.cpp`). Command type registered in `Protocol.h`
`kNativeMediaCoreCommandTypes`. Shell: `StudioViewModel.ProgramBlackout.cs`
(`SetProgramBlackoutAsync`): one in-flight guard; refuse if core offline; **poll first and
refuse if `programBlackout` is not a bool** ("This media core does not support Fade to
Black"); send `{"type":"program-blackout","enabled":<target>,"durationMs":300}`; verify the
returned snapshot echoes the target, else "Media core did not acknowledge FTB". Labels: "Fade
to Black" / "Fading to black" / "BLACK · Restore" / "Restoring picture" from opacity.
Remote: `transport.ftb.toggle`, `transport.ftb.set(enabled)`
(`ControlActionRegistry.cs:55-56`).

**Portability.** The core logic is in `native/` (SHARED). The shell admission logic lives
only in WinUI. It is about 40 lines and must be rewritten in Swift.

**Mac.** No `blackout`/`FTB` reference anywhere in `mac-shell/Sources` (grep). **Metal
gap (INFERRED, not pixel-verified):** the blackout layer is `kind "tiles-background"`,
`hasFillColor=true`, `fillColor "#000000"`, no participantId/mediaAssetId.
`MetalCompositorAdapter.mm:820-842` documents that Metal has **no `hasFillColor` branch**.
The layer therefore keeps the default `ResolvedLayer::color = 0xff444444`
(`MetalCompositorAdapter.mm:350`) and is drawn at `compositorLayerOpacity`
(`:1128`). So FTB on Mac would fade Program to **dark grey, not black**. The only
compositor pixel test is `native/tests/D3D11CompositorTest.cpp`, and
`ProgramBlackoutTest.cpp` tests the state machine only. Wiring the Mac button without
fixing Metal would ship a visible on-air defect. The same missing branch is the known Tiles
wall-background gap.

**Rulings to honour (quoted):** "FTB fades Program video over 300 ms; pressing again
restores the picture from the current fade level." "Preview, source ISOs, audio, and
running output sessions are unchanged." "Taking another scene while black does not release
the blackout." "No saved preference automatically engages FTB; a new media-core session
starts clear." "Older cores without the blackout observation reject FTB rather than
implying success." Button: "black fill with no visible text and a red outline when black is
selected."

**Class** SHELL-LOGIC (S) plus a native Metal `hasFillColor` branch (S–M; it also
fixes the Tiles background). Add a Metal pixel test mirroring the D3D11 one.

## 3. Take transition selector (Cut / Fade)

**Windows.** `StudioViewModel.cs:828` `_takeTransitionMode = "fade"` (**default Fade**).
Normalizer `:4073` accepts cut/dip/wipe and defaults to fade. The footer flyout offers
`cut` / `fade` (`StudioWorkspace.xaml:1235,1238`); "Instant" vs "· 300 ms". On a Take,
`SyncActiveSceneAsync` (`StudioViewModel.cs:9062-9083`) adds
`MediaCoreTakeTransitionWire(operationId, revision, mode, DurationMs: 300, GraphicsLayout)`.
`MediaCoreCommandBuilder.BuildSyncCommands` (plain net9, `:40-45`) puts
`begin-take-transition` **first in the same batch** as `load-scene-graph`. Fields:
`operationId, revision, mode, durationMs, direction, dipColor[, graphicsLayout]`. The core
(`MediaCore.cpp:1472-1482`) defaults mode to `cut` for anything not fade/dip/wipe.

**Mac.** `take()` (`AppModel.swift:1776-1784`) swaps ids and calls `syncScenes()`. It sends
no `begin-take-transition`. The button is hard-labelled "Cut" (`ShellApp.swift:1139`).
Every Mac Take is a cut, while the Windows 1.0 default is a 300 ms fade.

**Class** SHELL-LOGIC, **S** (one command plus picker state). Core support is SHARED.
Metal honours layer opacity (INFERRED from `drawLayer`), so a fade should render. A
Metal live check is UNVERIFIED. The `graphicsLayout` guard belongs to the graphics audit.

## 4. Take path correctness

**Windows.** `TransportCoordinator.TakeAsync` (WinUI) provides an in-flight guard, #286
rollback (scenes plus media selection, T1.3), backpressure retry and `CanTake` gating
(`StudioViewModel.cs:4274-4278`).

**Mac.** Take goes through the **120 ms cancellable debounce** (`AppModel.swift:1793-1799`).
Every Take is therefore delayed by at least 120 ms, and a slot edit inside that window
re-arms the timer. On RPC failure `pushScenes` only calls `pushWarning`
(`AppModel.swift:1859`). The local Program/Preview ids stay swapped, so the shell claims a
Program the core never received. The coordinator logic is WinUI-only, so this is a Swift
rewrite.

**Class** SHELL-LOGIC, **M**. Bypass the debounce for Take, add rollback on failure, and
add an in-flight guard.

## 5. Keyboard Take / FTB (#1206)

**Windows.** `Views/StudioWorkspace.TransportHotkeys.cs`. Spacebar is handled on
**PreviewKeyDown** so it beats a focused button's own Space activation. It is ignored for
text-entry focus or any modifier. It is consumed even when Take is unavailable and fires
only on the initial press (`!WasKeyDown`). Ctrl+Enter is an alternate Take; Ctrl+Shift+B is
FTB. Studio tab only.

**Mac.** mac-shell has **no keyboard shortcuts at all**. A grep for `keyboardShortcut`,
`onKeyPress`, `NSEvent`, `keyDown` and `addLocalMonitor` is empty.

**Rulings (quoted):** "Spacebar invokes the existing Take command in Studio; Ctrl+Enter
remains an alternate. Ctrl+Shift+B invokes FTB. These shortcuts ignore text-entry
controls." From the code comment: "Space must never open a flyout or toggle an output
because the focused control happened to change."

**Class** MAC-UI-ONLY, **S**. On macOS use Cmd for Ctrl (owner call). SwiftUI
`.keyboardShortcut(.space)` will not preempt a focused control or text field. Use an
`NSEvent.addLocalMonitorForEvents` monitor that checks the first-responder class to get
the Windows precedence.

## 6. Remote transport / scene control

**Windows.** `CoreVideoPro.Control` (plain net9.0) is the HTTP/OSC control surface and the
Companion module. Relevant actions: `transport.take`, `transport.ftb.toggle`,
`transport.ftb.set`, `scene.list`, `scene.restoreStarters`, `scene.cueWithRoutes` (#861),
`scene.select`. The retired `automation.magic|toggle|autoTake.set` IDs are rejected.

**Mac.** There is no listener, HTTP, OSC or Companion code in mac-shell (grep). The 1.0
decision names "Stream Deck/Companion" as one of the three manual Take paths. Today the Mac
has only one of the three: the on-screen button.

**Class** SHELL-LOGIC, **XL**. The router is portable C# in principle (dotnet runs on this
Mac), but the Mac shell is Swift with no host for it. Likely shared with another audit area;
listed here because it is the transport path.

## 7. Scene naming and renaming (#1196)

**Windows.** `Services/SceneNamingPolicy.cs`, `SceneNamingDialog.cs`,
`StudioViewModel.SceneNaming.cs`, prefs v17 `SceneNames` keyed by id.

**Rulings (quoted, `docs/reference/scenes.md` "Naming"):** "The Sources tab's **+** starter
chooser asks for a name before creating a scene." "Blank and case-insensitive duplicate
names are rejected; surrounding whitespace is trimmed. Cancel changes nothing." "Rename
updates metadata only … It must not call `UpdateScene`, commit route edits, refresh Auto
policy or Take. Scene IDs, ordering, saved/unsaved content and Program routes stay
unchanged." A context-menu rename "targets that row's stable ID without cueing it."

**Mac.**
- **Defect.** `renameScene` (`AppModel.swift:1672-1677`) rebuilds `SceneDef(id:name:layout:layers:)`
  and omits `backgroundAssetId`, which defaults to `""` (`AppModel.swift:70`). Renaming a
  scene silently **removes its SuperSource background**. The next `syncScenes()` (any slot or
  canvas edit) takes it off air if that scene is on Program or Preview, which violates "must
  not change live content". `duplicateScene` (`:1688`) drops the background the same way.
- The rename field is a live `TextField` binding (`SceneCanvas.swift:57-61`). It renames on
  every keystroke, with no trim, blank check or duplicate check, and no cancel.
- The field always targets "Preview, else Program" (`SceneCanvas.swift:17-19`). There is no
  per-row rename on the Studio rail (`SceneRow`, `ShellApp.swift:857+`).
- `newScene` (`AppModel.swift:1702-1715`) auto-names "Scene N" with no prompt.
- Persistence is fine. All scenes, built-ins included, save names via `PersistedScene`
  (`ShellPrefs.swift:10-16`, `AppModel.swift:445-455`).

**Class** SHELL-LOGIC, **S**. Fix the background drop first; it is a live-content bug
today.

## 8. Scene removal rule (#1070)

**Rulings (quoted, `scenes.md` "Removal"):** "the **on-air** scene (`ActiveSceneId`) cannot
be removed"; "the **last visible** scene cannot be removed"; "Removing the Preview scene
moves Preview to Program."

**Mac.** `deleteScene` (`AppModel.swift:1717-1726`) guards only `scenes.count > 1`.
- It **allows deleting the Program scene**, then sets `programSceneId = scenes.first` and
  syncs. That is a cut to a different scene with **no Take**.
- Removing the Preview scene sets `previewSceneId = ""` rather than moving Preview to Program.
- With Preview empty, the Scenes tab's target becomes the Program scene
  (`SceneCanvas.swift:17-19`). The "Delete" menu item (`SceneCanvas.swift:46`) and every
  canvas edit then act on the **live Program scene**. The pane's own caption claims
  "program is untouched until Take" (`SceneCanvas.swift:66`).

**Class** SHELL-LOGIC, **S**.

## 9. Scene catalog, starters, Restore starters, Tiles starter (#1199/#1200)

**Windows.** Five catalog scenes (`intro`, `interview`, `speaker-slides`, `panel`,
`closing`), plus starters for every catalog layout via `StarterSceneFactory`. **CoreVideo
Tiles · Auto-reflow wall** (`dynamic-gallery`) goes through the same naming dialog. There is
also "Restore starter scenes" (`scene.restoreStarters`). The Tiles payload and membership
policies are WinUI (`Services/TilesLayerPayloadBuilder.cs`, `TilesMembershipPolicy.cs`, …).
Only `TilesAudioSourceLatch` is in plain-net9 MediaCore.

**Mac.** Three built-ins with different ids and layouts (`fullscreen`/`two-up`/`pip`,
`AppModel.swift:350-354`; rects `:1469-1473`). Starters are New fullscreen, side-by-side
and PiP (`SceneCanvas.swift:40-42`). There is no restore. There are no Tiles: no `tiles`
layer, no gallery, and `ZoomSourceBudget.swift:5` says the Mac model "has no Tiles wall".

**Class.** The catalog and restore are SHELL-LOGIC (M). The Tiles starter is
N/A-until-Tiles. **Note:** the 1.0 ruling says "CoreVideo Tiles owns composition". The
2026-10-09 plan's §5.3 "N/A-until-Tiles" framing is therefore now load-bearing for 1.0:
Mac lacks the product's primary composition mode. The Tiles port is XL and is the real
dependency (outside this area's scope).

## 10. Auto director, Set & Forget, Magic Scene (#1146–#1150, #1173, #1144, #761, #668)

**Windows.** All of this work landed 2026-11-05/06 in `MagicSceneCoordinator*` (WinUI).
Native pieces reach Mac free: `set-director-policy` with `preferScreenShare` and
`panelGuestThreshold` 2–10 (`DirectorPolicy.h`), and `Director.h`. On 2026-11-09 the
owner retired it: "The production MagicSceneCoordinator constructor cannot enable scene
automation … there is no user setting, environment variable or license override."

**Mac.** It is already gated, in the #1205 diff:
- `static let sceneAutomationAvailable = false` (`AppModel.swift:371`).
- `autoTakeEnabled` is forced false at load (`:495`, `ShellPrefs.swift:119`).
- `setAutoDirect` and `evaluateAutoDirect` are guarded, and `runMagicScene` and
  `canRunMagicScene` are guarded (`AutomationExtras.swift`).
- The tab pill is removed (`ShellApp.swift:242`). `case .automation: AutomationPane()`
  (`ShellApp.swift:1024`) remains, so it is unreachable from the header (`AutomationPane`
  now shows only "Manual production" text). `AutomationExtrasSection` is dead code.

**Class** N/A for 1.0, matching the ruling: "macOS removes the same controls and
navigation." Do not port #1146–#1150.

**10a.** One item survives 1.0: "Automatic filling of free input slots … remain[s]
available", and Windows keeps "the separate free-input filling setting". Mac auto-fill is
unconditional (`AppModel.swift:871-890`) with no toggle. MAC-UI-ONLY, **S**.

## 11. Multiview placeholder ownership (#1172 / #1175)

**Windows.** `Models/MultiviewEmptyStatePolicy.cs` (WinUI, pure). Assignment guidance shows
only when there are 0 tiles **and** the surface has no frames, preview bitmap, pending GPU
handle or media path, **and** its status is one of the generic "Waiting for video frames",
"Preparing video" or "Preparing Program". "Unknown or failure statuses remain visible.
Never infer that every surface without a picture is merely preparing." Guidance and video
host are mutually exclusive.

**Mac.** `MultiviewerCanvas` (`ShellApp.swift:587-621`) draws the guidance text whenever
`multiviewTiles.isEmpty`, **on top of** `SurfaceView` if a surface exists (`:588-590`).
Mac has only one message, so the Windows double-message overlap cannot occur. But "an
available picture is never covered by assignment guidance" is not honoured, and there is no
failure-status channel to preserve.

**Class** MAC-UI-ONLY, **S**.

## 12. Multiview input inspector and versioned ten-row projection (#674 slices A–F, 2026-09-26)

These landed before the 2026-10-09 audit window closed. That audit listed only three
Windows-only features, so these were **missed** there.

**Windows.** `ViewModels/ShowInputs/MultiviewInputRowsProjection.cs`,
`MultiviewInputRow.cs` and `MultiviewTileCropPolicy.cs` (all WinUI). They project
**versioned** roster facts onto ten stable rows. A fact is accepted only if
`rosterRevision > 0`, `rosterEpoch` is non-empty and `ZoomRosterSnapshotPolicy.Accept`
passes; an epoch change resets frames; a retired epoch is ignored. Each row carries status
(IDLE/LIVE/TALKING/STALLED/PGM), negotiated format `W×H@fps` from the core's
**`zoom-source-format`** event (Slice F, `ZoomEngineRuntime.cpp:1936`), and stale frame
age. It also carries a configured-cap note ("up to X requested", "local max N fps"), a REC
chip from the ISO output lifecycle (Slice C, `IsoOutputLifecyclePolicy` in plain-net9
MediaCore), and a per-row preview crop of the multiview texture. Core facts are SHARED.

**Mac.** It has `RosterParticipant.talking` and the generated `rosterEpoch`/`rosterRevision`
decode (`Lifecycle.generated.swift:249-257`, used by `ZoomRosterProjection`). It has **no**
handling of `zoom-source-format`, no per-row format, frame-age, cap or REC chip, and no row
preview (`SlotRow`, `ShellApp.swift:1763`).

**Rulings:** "a request ceiling is never an FPS measurement" (`MultiviewInputRow.cs`
comment). "The ceiling accepted at this meeting's join, never a pending preference"
(`SetAppliedCameraCap`).

**Class** SHELL-LOGIC, **L**.

## 13. EngineHost (#960, #961)

**What it is.** `native-shell/CoreVideoPro.EngineHost` is a 33-line net9 exe hosting
`MediaCoreSupervisor` behind a named pipe (`EngineHostServer` in MediaCore). The console UI
can then die or reattach while the core and outputs keep running. The rest is fencing
tokens, a read-only second console, and (#961) continuing accepted Zoom subscriptions after
detach via a `captureRefresh` spine payload validated against
`rosterEpoch`/`captureGeneration`/`rosterRevision` (`ZoomEngineRuntime.cpp` `syncSpine`).
It is **off by default** (`EngineHostBootstrap.cs:9` requires `COREVIDEO_ENGINE_HOST=1`),
and `Program.cs:3` throws on non-Windows ("EngineHost requires Windows."). Its README says
"The feature is **off by default**" and "Host packaging/default enablement belongs to #963."

**Mac impact.** The native half reaches Mac and is inert for it. Mac always sends
`startCapture` (`AppModel.swift:1372`) and never `captureRefresh`, so the only change it
sees is `captureGeneration` bumping on a capture toggle. A Mac equivalent would be a
launchd/XPC agent owning the core process. That is architecture work, not parity.

**Class** N/A for 1.0. If Windows ever enables it by default, reopen as XL.

## 14. Popout windows

Windows has `MultiviewPopoutWindow`, `ProgramPreviewPopoutWindow`, `AudioMixerWindow`,
`ColorGradeEditorWindow`, `DiagnosticsWindow` and `ProductionSettingsWindow`. Mac has
`Window("Multiviewer")`, `Window("Program / Preview")`, `Window("Color Grade")` and
`Window("Audio Mixer")` (`ShellApp.swift:78,86,95,102`). Diagnostics and Settings are tabs.
Since 2026-10-09 Windows changed no popout window in this area (git log). **SHARED-DONE**
(an equivalent surface exists).

## 15. Participant-list refresh after Studio unload (#919 / #1176)

`Services/ParticipantListRefreshScheduler.cs` fences deferred ListView clear/rebind
callbacks across XAML Loaded/Unloaded lifetimes. This is WinUI dispatcher plumbing; SwiftUI
roster rendering has no analogue. **N/A.**

## 16. Production settings window

Windows `ProductionSettingsWindow.xaml` (1141 lines) has sections output, streaming,
recording, audio, multiviewer, ffmpeg and ohg. Mac `SettingsPane.swift` (269 lines) has
canvas, multiviewer mode, recording container, stream key, monitor and Zoom sign-in. Most of
the content is outputs/audio. **UNVERIFIED** at field level; defer to the outputs audit.

## 17. Diagnostics and support bundle

- **Export: SHARED-DONE.** `Diagnostics.swift:300-460` is the redacted bundle: core, zoom
  and shell snapshots, log tails, a crash-report *listing*, and a zip via `ditto`. It
  mirrors `SupportBundleBuilder`/`ArchiveBuilder`/`LogRedactor`.
- **#1182 (#1178) native phase timing: SHARED-DONE.** It lives in `native/src/rpc` plus CI
  scripts. **#1178 itself is still open:** a recurrent Mac command-p99 gate failure, now
  attributed in one sample to a 186 ms core-lock wait behind a render stall
  (`docs/reference/mac-command-attribution-1178.md`). It blocks #1180.
- **Crash offer and upload: SHELL-LOGIC, M, owner decision.** Windows has
  `CrashReportCoordinator` (WinUI) plus `CrashReportUploader` (plain net9; production
  endpoint `https://telemetry.corevideo.io` since 75bb8cca), the WER LocalDumps scan
  (#1039) and a consented Send. Mac states "Local export only. There is deliberately no
  uploader and no telemetry here" (`Diagnostics.swift:20`). That position predates
  production telemetry existing; confirm it is still the owner's intent for 1.0.

## 18. Footer health line (#1074)

Windows `StalledFeedsReadout` and `StreamDegradationReadout` are pure functions in
plain-net9 MediaCore. They produce "stream N ms behind and K Zoom feeds stalled" using the
on-air 1.5 s rule, from `outputSenderSession.senders[].backpressure` plus `sources[]`. Both
nodes reach Mac via the shared core. Mac has no equivalent line (grep). **SHELL-LOGIC, S.**
The rule to keep: "it does not claim a cause."

---

## Suggested order (if this area is scheduled)

1. §7 and §8 defects (S each). They change live content today.
2. §2 Metal `hasFillColor` plus pixel test, then the FTB shell (it must not ship before the
   Metal fix).
3. §3 transition, then §4 Take path, then §5 keyboard. Together these give 1.0 manual-Take
   parity on-screen and by keyboard.
4. §1 footer layout, then §11, §18 and §10a polish.
5. §12 (L), §9 catalog (M), §6 control API (XL) and Tiles (XL) as separately-scoped work.
