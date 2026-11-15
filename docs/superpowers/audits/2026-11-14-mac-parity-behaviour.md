# macOS parity audit: behaviour rulings (2026-11-14)

**Scope.** Every non-OHG **fix** on `main` since 2026-10-09 (worktree `mac-parity-1.0`, `main @ 00838940`, Release 1.0.0) in these areas: recording and ISO, Zoom sources/roster/subscriptions, capture sources and dropout, audio routing and monitor, shutdown and close, Program delivery, stream output, and bridge/recovery. Graphics, lower thirds, Tiles, Auto director, scene builder, social and comms belong to sibling audits.

**Method.** I listed every commit with `git log --since=2026-10-09 origin/main --no-merges -- native-shell native mac-shell` (509 commits) and classified each by touched tree. Then I filtered to fixes in this area. For each shell half I read the Windows diff, stated the rule, and read the Mac code path. Every verdict below cites `file:line`. I did not run any code.

**Finding beyond the brief: "touched `native/`" does not always mean "reaches Mac".** Several fixes in this window touched `native/` but only inside **Windows-only adapters**: `MediaFoundationEncoderAdapter.cpp`, `D3D11CompositorAdapter.cpp`, `WasapiMonitorOutputAdapter.cpp`, `UvcCaptureDeviceAdapter.cpp`, and the Windows Zoom helper `zoom-engine/engine/engine-video.cpp`. Mac runs different code for each of these:

- `AVFoundationEncoderAdapter.mm` for recording
- `MetalCompositorAdapter.mm` for compositing
- `CoreAudioMonitorOutputAdapter.cpp` for the monitor
- `AvfCaptureDeviceAdapter.mm` for capture
- the separate ObjC++ Zoom helper `zoom-engine/engine/main-macos.mm`, which has **zero commits since 2026-10-09**

The parity plan's §5 assumption ("fixes that touched `native/` reach Mac for free") holds only for shared core code (`core/`, `rpc/`, `ZoomEngineRuntime`, `AsyncEncoderSink`). Two of the five MAC-MISSING items come from this class, not from shell-only fixes.

Classes: **MAC-HAS-IT**, **MAC-MISSING**, **N/A** (no Mac analogue, or explicitly fenced), **UNVERIFIED**, plus **NATIVE**: the fix lives in shared core code and reaches Mac with nothing to port.

## Summary table

| Fix | Touched | Class | Effort | Mac evidence |
|---|---|---|---|---|
| **Recording / ISO** | | | | |
| #1188 `1d6649c8` + `0203e310`: an active take keeps its ISO set across passive source loss | S | **MAC-MISSING** | S | `AppModel.swift:2004-2011` filters ISO ids by *current* roster presence; `startProgramOutput` (`:2036-2054`) resends them on every Stream toggle mid-take; the core overwrites `recordingIsoParticipantIds_` (`MediaCore.cpp:2529-2531`) without restarting the writers (`:2543-2551`), so the dropped ISOs stop being fed (`:6702-6718`, `:7539+`) |
| #976 `53c3efaf`: the previous take's retained terminal state must not cancel a new Start | S | UNVERIFIED | S | `RecordingCommandPolicy` has no session identity: a `failed`/`interrupted` poll after `begin()` clears `desired` (`RecordingCommandPolicy.swift:17-25, 59-63`). Operator impact not proven, because the Record button reads observed status (`ShellApp.swift:1100-1102`) |
| `3001d4c1`: stop a failed take before retrying | S | MAC-HAS-IT | — | Every Start mints a fresh `UUID()` sessionId (`AppModel.swift:2463`), so the core's same-id dedup (`MediaCore.cpp` `startRecordingSession`) can never swallow a retry |
| #974 `88ad53be`: an explicit ISO re-arm delivers current targets | S | MAC-HAS-IT | — | No remembered-vs-eligible split: targets are resolved fresh at every Start (`AppModel.swift:2447`, `:2004-2011`) |
| #987 `e649d91f`/`4b4a78cd`: ISO capture warnings follow the observed `rawMediaActive`, never stale | S | MAC-HAS-IT | — | Preflight reads `lastZoomSnapshot`, which is replaced wholesale (`AppModel.swift:858`). The core always emits `rawMediaActive` (`ZoomEngineRuntime.cpp:1453, 1505`). Computed on demand at Start (`:2448-2452`) |
| `935b2800`: a capacity *warning* does not cancel a Start | S | MAC-HAS-IT | — | `warning` counts as live (`RecordingCommandPolicy.swift:15`); `toggleRecording` never rolls back on health (`AppModel.swift:2466-2469`) |
| #1009 `ab34f90b`/`fd15607d`: Record is not gated on a Zoom meeting | N+S | MAC-HAS-IT | — | The Record button has no meeting gate (`ShellApp.swift:1153-1173`); the only guard is `bridge` (`AppModel.swift:2422`). See the note on resume-after-crash |
| #859 `e9dc8068`: an idle Stop is not a finalization | N | NATIVE | — | `MediaCore.cpp` only |
| #1203 `219ca1db`: record Zoom camera ISOs at the selected resolution | N (MF + core) | **MAC-MISSING** | M | AVF ignores the new `IsoSourceSelection` target dimensions. It lazy-opens at the **first** frame's size (`AVFoundationEncoderAdapter.mm:466-470`) and crops or pads every later frame to that size (`:230-256`) |
| #1030 `2dcfef25` + #1183 `fc4d254d`: a departed ISO participant is not silence-filled | N (MF + core) | **MAC-MISSING** | S | The core sets `IsoSourceAudio::departed` on every platform. MF honours it (`MediaFoundationEncoderAdapter.cpp:1519-1527, 2119`). AVF `submitIsoAudio` never reads it and keeps silence-filling (`AVFoundationEncoderAdapter.mm:506-535`) |
| #989 `12065235` / #1189 `99e6aeb6` / #1177 `c996da9f`: Zoom ISO arrival ring | N | N/A (fenced) | L | `independentIsoVideoSupported()` returns `false` on `__APPLE__` (`ZoomEngineRuntime.h:114-119`): the macOS helper never writes the `-iso` ring. Mac Zoom ISOs stay on the older latest-picture sampling |
| #984 `5d45a906`: keep camera ISO arrivals before render sampling | N (UVC) | UNVERIFIED | M | Only `UvcCaptureDeviceAdapter.cpp:168-170, 537` has an arrival queue. `AvfCaptureDeviceAdapter.mm` has none. Duplicated or dropped camera-ISO frames on Mac are not measured |
| #1186 `03ebd1c3`: keep one final Program picture at close | N (MF only) | UNVERIFIED | M | MF-only `reconcileTail`. AVF has no A/V tail reconciliation at all: `markAsFinished` then `finishWriting` (`AVFoundationEncoderAdapter.mm:353-358`) |
| #1042 `d62b8c30` / #935 `1ed66561`, `ea40c07a`: ISO encoder capacity probe and placement readout | N+S | N/A | — | Media Foundation MFT probe and its WinUI readout; AVF has no capacity probe |
| **Zoom sources / roster / subscriptions** | | | | |
| #1171 `888c788c`: keep SDK subscription responses next to received geometry | N | **MAC-MISSING** (diagnostic) | M | The core parser is shared, but `main-macos.mm` emits no `video_subscribe` / `set_resolution` / `video_resolution_downgraded` debug events (the Windows helper does, `engine-video.cpp:81-114`) |
| #765 `f3a203ff`/`a2b4e71e`/`f8856f56`: no invented roster audio level, network quality or latency | N+S | MAC-HAS-IT | — | Mac never sent `audioLevel`/`networkQuality` (no matches in `mac-shell/Sources`); sender `latencyMs` now shows "—" (`Diagnostics.swift:1148`) |
| #766 `fc219986`: diagnostics stop over-claiming; revision 0 carries no roster fact | S | MAC-HAS-IT | — | Mac shows no feed or first-frame claim (`Diagnostics.swift:1193`, participant count only). Revision 0 carries the installed roster (`ZoomRosterProjection.swift:34-40`); its `stale` counter is never displayed |
| #920 `b15796d2`: retire the old meeting lifetime before a replacement join | N | NATIVE | — | `ZoomEngineRuntime.cpp` |
| `c22a6030`: one Zoom roster per response; ISO names stay current | N | NATIVE | — | core / rpc |
| #1178 `9ab601f9`: keep Mac command and native phase timing | N | NATIVE | — | `JsonRpcServer.cpp` |
| #961 `40d3ef81`/`d42be5f7`/`8ccad30a`: EngineHost subscription continuity | N+S | N/A | — | The opt-in WinUI EngineHost process has no Mac counterpart |
| **Capture sources / dropout** | | | | |
| #758 (`8f8a9998`, `d7c9444d`, `29380842`, `a1ed294a`, `3b94830f`, `6273fff2`): capture liveness and dropout policy | N+S | N/A | (core gap) | Only the WinUI SHM bridge produces `CaptureLiveness` (`ShmCapturePreparation.cpp:74-86`). AVF reports none, so the core keeps the legacy hold for every Mac capture (`MediaCore.cpp` `annotateLayerSource`, capture branch) |
| #562 `1e06dab4`: expose capture dropout policies in Sources | S | N/A | — | Same reason: a capture policy cannot apply without AVF liveness. Mac's Zoom-only menu (`AppModel.swift:1415-1432`) is correct today |
| #1168 `6611beb3`: refresh availability before publishing a direct assignment | S | MAC-HAS-IT | — | `assignSlot` clears `offline` before `recomputeFromSlots` (`AppModel.swift:926-935`); ISO resolution uses roster presence, not the slot flag (`:2004-2011`) |
| #590 `633103a1`: an offline slot keeps its named assignment | S | MAC-HAS-IT | — | Health refresh only toggles `offline` and never clears identity (`AppModel.swift:968-981`); the slot's name field stays visible (`ShellApp.swift:1837-1840`). The picker label itself is generic (`:1771`) |
| `d550a5d6`: release the resolved native device, not its alias | S | N/A | — | Mac connects and disconnects by the core-reported `device.id`, with no shell-side hash (`AppModel.swift:2537-2552`) |
| **Audio routing / monitor** | | | | |
| #870 `13c2653f`: audio observations carry an age; meters say aging or stale | N+S | **MAC-MISSING** | S | The core publishes `audioObservation` (`CoreStateCapture.cpp:824`); `mac-shell` never reads it (no matches), so a stalled audio worker leaves frozen meters looking live |
| #872 `2564de47` + `e6d4ec21`: monitor device gets a bounded owner; superseded starts are fenced | N | NATIVE | — | The wrapper is applied on every platform (`MediaCore.cpp:6564-6566`) |
| #903 `a3e984a4`: monitor history survives mute and device changes | N (+WASAPI) | MAC-HAS-IT (partial) | S | `CoreAudioMonitorOutputAdapter.cpp:201-216` never resets its dry counters, so history survives. Dry-event classification is not implemented; the `Interfaces.h` default publishes it as unsupported |
| #968 `cdd010ed`: select media PCM on the scheduled audio grid | N | NATIVE | — | core |
| **Shutdown / close / Program delivery** | | | | |
| #941 `e3f45716`: stop Program delivery when the render producer exits | N (D3D + rpc) | N/A | — | Metal has no scheduled delivery thread; its tap thread is woken by render and joined in the destructor (`MetalCompositorAdapter.mm:242-248, 631-660`) |
| `98333af0`: publish RPC terminal predicates under their wait mutexes | N | NATIVE | — | `JsonRpcServer.cpp` |
| #782 `be1b369c`: supervisor tolerates a process that never started | S | MAC-HAS-IT | — | `if proc?.isRunning == true { proc?.terminate() }` (`MediaCoreBridge.swift:115`) |
| D3D / WGC / UVC Program-delivery cluster (#864, #904, #997, #1000, #1014, #1016, #1019) | N (Win adapters) + S | N/A | — | `D3D11CompositorAdapter`, `D3DProgramBuffer`, `WgcScreenCaptureAdapter`, `UvcCaptureDeviceAdapter`, XAML monitor hosts |
| **Stream output** | | | | |
| #519 `5989bd20`: RTMP/SRT bytesSent measured at the pipe | N | NATIVE | — | shared sender adapters |
| #763 `b5bdab46` + NDI series (`a23c2fcd`, `6c465a87`, `08c8e191`, `28974b42`, `22d5018f`) | N+S | N/A | — | No NDI on Mac (no match in `AppModel.swift` / `ShellApp.swift`) |
| `cdcb8179`: ended destinations stay separate from current stream activity | S | MAC-HAS-IT | — | Shows "Idle" whenever Stream is not desired (`ShellApp.swift:1326-1330`); reads only the `rtmp` sender's own status (`AppModel.swift:822-832`) |

**Counts:** MAC-MISSING 5 · UNVERIFIED 3 · MAC-HAS-IT 12 · N/A 9 · NATIVE 8.

---

## Details

### Recording / ISO

**#1188: a running take keeps its ISO set (MAC-MISSING, S).**

*Rule:* while a recording is requested, the ISO set changes only on operator intent. Passive unavailability, such as a roster gap or a missing device, does not revoke a writer already in the take (`RecordingIsoSelection.cs`). `0203e310` narrows the refresh trigger so that an unrelated slot assignment does not count as intent.

*How Mac breaks it:* Mac sends ISO ids in two places only: the Start batch, and every `start-program-output`. `startProgramOutput()` calls `resolvedIsoSourceIds()`, which filters by **current** roster and device presence (`AppModel.swift:2004-2011`). Toggling Stream during a take resends it (`:2066`, `:2089`). The core takes the new list unconditionally (`MediaCore.cpp:2529-2531`). Because the recording owns the encoder generation, the writers are not restarted (`:2543-2551`). From then on the ISO video and audio feeds are selected from the shrunken list (`:6702-6718`, `:7539+`).

*Concrete failure:* the operator records with three Zoom ISOs. A momentary empty roster occurs; #1183 documents these as ordinary. The operator starts or stops Stream during that beat. Every Zoom ISO file in the take stops receiving video and audio for the rest of the take, with no warning, while the Program file continues.

*Fix shape:* keep the active take's ISO list (the Windows `RecordingIsoSelection` semantics) and send that list from `startProgramOutput` while `recordingDesired`.

**#976: the previous take's retained terminal state vs a new Start (UNVERIFIED, S).**

*Rule:* a lifecycle observation whose `sessionId` is the previous take's must not cancel the new Start intent.

*Mac:* `RecordingCommandPolicy.observe` guards only `idle`/`completed` while awaiting Start progress (`RecordingCommandPolicy.swift:21-24`). A retained `failed` or `interrupted` from the old take falls through to `reconcile`, which sets `desired = false` (`:59-63`). The rule is therefore not implemented.

*Why UNVERIFIED:* the visible Record state comes from `recordingStatus` (`ShellApp.swift:1100-1102`), not `desired`, and `begin()` still offers Stop while media is live. The only other consumer is `isRecordingActive` in `toggleStreaming` (`AppModel.swift:2056-2067`). Stopping Stream would then send `stop-encoder-session`, which clears `outputDestinations_` (`MediaCore.cpp:2582-2588`), and I did not prove whether that disturbs the running recording. One more gap: the lifecycle `sessionId` reaches Mac, but `RecordingLifecycleReadModel` never reads it.

**#1203: Zoom camera ISO resolution (MAC-MISSING via the native AVF adapter, M).** The core now passes the selected camera tier as `targetWidth`/`targetHeight` on each Zoom `IsoSourceSelection`, and only MF uses them. AVF lazy-opens each ISO at the source's **first** frame (`AVFoundationEncoderAdapter.mm:466-470`); `writeVideoNv12` then crops or pads to that size (`:230-256`).

*Concrete failure:* a guest's ISO opens on an early, smaller Zoom frame, such as during the post-subscribe ramp or while the 8-camera cap holds them at 720P. Every later 1080P frame is recorded as its top-left 640×360 or 1280×720 crop for the rest of the take. The Program file is unaffected. *Fix shape:* open at the target dimensions and scale I420 to fit (vImage or VTPixelTransfer).

**#1030 / #1183: a departed ISO participant (MAC-MISSING via AVF, S).** The core marks `IsoSourceAudio::departed` on all platforms, and #1183 extends it across an empty roster. MF refuses to queue departed silence (`MediaFoundationEncoderAdapter.cpp:1519-1527, 2119`). AVF ignores the flag and silence-fills via `isoAudioAdvance` (`AVFoundationEncoderAdapter.mm:506-535`).

*Failure:* a guest leaves mid-show. Their ISO keeps growing as an audio-only silent tail with no video until Stop, so the file does not end at its real duration. The MF wedge (ThrottleStream) does not apply as-is, because AVF drops frames when `readyForMoreMediaData` is false (`:219-221`) rather than blocking. Whether real-time interleaving stalls the video input during that tail is UNVERIFIED.

**#989 / #1189 / #1177 (N/A, fenced, L).** The independent Zoom ISO ring is explicitly disabled on Apple (`ZoomEngineRuntime.h:114-119`, "The separate macOS SDK helper has not adopted this transport"). The guard is honest, and #1189 and #1177 harden a path Mac does not run. It is still a parity gap: Mac Zoom ISOs sample the latest picture, which is exactly what #989 fixed on Windows. Closing it needs `main-macos.mm` to write `-iso` rings.

**#984 (UNVERIFIED, M):** camera ISO arrivals have a queue only in the Windows UVC adapter. **#1186 (UNVERIFIED, M):** AVF has no tail reconciliation, so the final-picture refinement has nothing to attach to. The underlying A/V-delta question for Mac recordings predates this window.

**#1009 note.** Record is not meeting-gated on Mac, so the rule holds. The same commit extended `OutputResumeAfterCoreRestart` on Windows. Mac never resumes outputs after a core exit: it marks them `interrupted` and warns (`AppModel.swift:604-618`). That divergence dates from #733/#746 (pre-window) and is a deliberate, loud Mac behaviour, not a regression.

### Zoom

**#1171 (MAC-MISSING, diagnostic, M).** Shared core parsing for SDK subscription evidence, but the macOS helper (untouched since 2026-10-09) emits none of the debug events the parser reads. On Mac, a Zoom SDK refusal or downgrade of a camera subscription is invisible in evidence.

The macOS helper also lacks the #765 `network_quality` event. Because absent means unknown, that is truthful, not a gap.

### Audio

**#870 (MAC-MISSING, S).** *Rule:* meters and status readers report the age of the audio observation: fresh up to 250 ms, aging up to 1 s, then "stale — last values held" (`AudioObservationPolicy.cs`). *Mac:* meters bind straight to snapshot values; `audioObservation` is never read. *Failure:* the core's audio worker stalls while the render-driven snapshot keeps flowing. The Mac console keeps showing the last meter values as if live, which is the case an A1 most needs to see.

**#903 (partial).** The CoreAudio monitor's dry-event counter is monotonic for the life of the adapter, so the "history survives mute and device change" half holds. Classifying intentional silence is not implemented; the core publishes it as unsupported, which is honest.

### Out of window, noted so nobody re-derives it

- **#659 / #661 / #663** are cited in the brief but landed 2026-09-26/27, before this window. Each touched `mac-shell` directly (2–6 files), giving `ZoomRosterProjection.swift`, `AudioRouteControlProjection.swift`, `MonitorControlProjection.swift` and the bridge recovery barriers.
- **#758 `44395355`** (2026-10-02) was shell-only for the WinUI bridge reader (N/A).
- **Mac has no quit-while-recording guard** (Windows T1.8). This is pre-window and UNVERIFIED for impact: I did not check whether the core finalizes on stdin EOF.

### Spillover outside this area (not counted)

- **#764 `32ead048`/`f5fb0ebf`:** the core no longer stores or applies `set-brand-kit` `logoText`, `lowerThirdStyle` or `captionStyle`. Mac still offers all three controls and sends them (`Overlays.swift:73-112, 194-196, 259-260, 327-328`), so they do nothing. This belongs to the graphics audit.
- Mac still sends route `audioRole` and the core now ignores it. That is harmless.
