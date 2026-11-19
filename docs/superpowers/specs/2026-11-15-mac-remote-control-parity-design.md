# Remote control parity on macOS: one manifest, two hosts, 110 actions

Status: DRAFT design for owner review, 2026-11-15.

Covers roadmap milestone **M2** (M2.1 to M2.4) in
`docs/superpowers/plans/2026-11-14-mac-parity-1.0-roadmap.md` and gap row **2.7** in
`docs/superpowers/audits/2026-11-15-mac-ui-parity-consolidated.md` (owner priority 3). It also
closes rows A5 and A6 of `2026-11-15-mac-ui-parity-settings-automation-health.md`, section 6 of
`2026-11-14-mac-parity-studio.md` and sections 3 and 6 of `2026-11-14-mac-parity-social-comms.md`.

Path shorthand: `C/` = `native-shell/CoreVideoPro.Control/`, `CT/` = `native-shell/CoreVideoPro.Control.Tests/`,
`W/` = `native-shell/CoreVideoPro.WinUI/`, `WT/` = `native-shell/CoreVideoPro.WinUI.Tests/`,
`M/` = `mac-shell/Sources/CoreVideoProShell/`.

## 1. Why

A Windows operator can run a whole show from a Stream Deck: join the meeting, assign guests to
inputs, pick a transition, cue scenes, key the lower third, ride the monitor. The Windows shell
exposes **110 actions** for this over HTTP, WebSocket and OSC. The Mac shell exposes **13** of them
over HTTP and WebSocket and has no OSC listener. Three groups notice:

- **Companion / Stream Deck operators.** The Companion module builds its buttons from the live
  `GET /manifest`, so a Mac shows 13 buttons. Three of the module's seven shipped presets call
  actions the Mac refuses with "Unknown action": `transport.engine.toggle`,
  `automation.autoAssignInputs.set` and `graphics.lowerThird.toggle`
  (`companion-module-corevideopro/src/presets.ts:49,58,67`).
- **OSC rigs** (QLab, TouchOSC, an OSC Stream Deck plugin). Nothing listens on UDP 8010 on a Mac.
- **Automation and QA.** The Volt rundown drives Pro only through `media.rundown.*`; a Mac has no
  such action. QA scripts can already read `GET /snapshot` on a Mac, but cannot set up a show
  through the API (no join, no input assign).

## 2. Windows behaviour to match

### 2.1 Catalog, binding and manifest

- **The registry** is a hand-authored, ordered list of 110 actions with stable ids
  (`C/ControlActionRegistry.cs:45-332`). Ids never follow C# names (`:7-9`).
- **Binding** is positional. Extra trailing args are ignored, a missing required arg is an error,
  a missing optional arg is null (`C/ControlActionRegistry.cs:24-31`, implemented in
  `C/ControlCatalog.cs`). Bind errors name the schema, never the caller's data, because args can
  carry stream keys. Proven by `CT/ControlActionRegistryTests.cs:9,26,68,82,142`.
- **The manifest** is derived from the registry: `Version`, `Root` (`/cvp`), `Actions` (each with
  `Id`, `Title`, `Description`, `OscAddress`, `Params`) and `FeedbackFields`, serialized
  PascalCase (`C/ControlManifest.cs:22-66`). `StateFields` is the feedback list (`:38-47`).
- **Dynamic providers** can add actions (the OHG `ohg.*` set) with an OSC exposure of `Lan` or
  `LoopbackOnly` (`C/IControlActionProvider.cs:5-12`, `C/ControlCatalog.cs:71-72`).

### 2.2 Transports

- **HTTP** on `127.0.0.1:8011`: `GET /manifest`, `GET /state`, `GET /snapshot`, `POST /invoke`
  with `{"action","args"}`; anything else is 404 `{"ok":false,"error":"Not found"}`
  (`C/Http/HttpControlRouter.cs:45-70`). Bind failure is 400, a refused action is 422. Proven by
  `CT/HttpControlRouterTests.cs` and `CT/HttpControlServerTests.cs:24`.
- **WebSocket** `/ws`: sends the current `ControlState` on connect and a fresh one on every
  change. Inbound frames are drained; commands are not accepted over WS
  (`C/Http/HttpControlServer.cs:205-222`). Proven by `CT/HttpControlServerTests.cs:91`.
- **OSC** on UDP `127.0.0.1:8010`. An address maps to an id by replacing `/` with `.` under the
  root; `/cvp/state/...` is the feedback namespace and never an action
  (`C/Osc/OscAddressMap.cs:19-37`). An unknown address is ignored, not an error
  (`C/Osc/OscControlRouter.cs:28-32`). OSC sends no reply; a refusal is only logged
  (`C/Osc/OscControlServer.cs:92-96`). Feedback goes to every endpoint that sent a command in the
  last five minutes (`:147-163`). Booleans are sent as int 0/1; field names equal the JSON state
  names (`C/Osc/OscFeedback.cs:3-41`). The codec handles `i f s T F b` and flattens `#bundle`
  (`C/Osc/OscCodec.cs:8-167`). Proven by `CT/OscCodecTests.cs`, `CT/OscControlRoutingTests.cs:57-151`
  and `CT/OscControlServerTests.cs:11,37`.

### 2.3 Authorisation

- HTTP binds loopback unless `COREVIDEO_HTTP_LAN` is exactly `1`. A non-loopback bind refuses to
  start without a non-blank `COREVIDEO_CONTROL_TOKEN` (`C/Http/HttpControlServer.cs:24-35`,
  `W/MainWindow.xaml.cs:179-194`). With a token set, every request needs
  `Authorization: Bearer <token>`; `?token=` is accepted only on the `/ws` upgrade
  (`C/Http/HttpControlServer.cs:157-173`). Proven by `CT/HttpControlServerTests.cs:115`.
- OSC has no authentication. It leaves loopback only when **both** `COREVIDEO_OSC_LAN=1` and
  `COREVIDEO_OSC_TRUSTED_NETWORK=1` are set (`W/MainWindow.xaml.cs:148-154`). `COREVIDEO_OSC_PORT`
  overrides the port (`:141-146`).
- Windows sends `Access-Control-Allow-Origin: *` (`C/Http/HttpControlServer.cs:182`). The Mac
  deliberately does not (section 3).

### 2.4 Dispatch, feedback and `/snapshot`

- `StudioControlSurface` maps each id onto the same view-model command the UI button uses, on the
  UI thread (`W/Services/StudioControlSurface.cs:171-216`, switch at `:221-950`).
- **The 1:1 closure test**: `SupportedActionIds` (`W/Services/StudioControlSurface.cs:64-104`) must
  equal the registry exactly, no missing ids and no phantom ids
  (`WT/StudioControlSurfaceCoverageTests.cs:24-36`).
- Wrappers the Mac must copy rather than re-derive: `RunTake` (refuse when `CanTake` is false),
  `RunTransportToggle`, `RunOutputSet` (an identical request is an idempotent success)
  (`W/Services/StudioControlSurface.cs:1214-1260`); `SceneSelectArgs.Refuse` and
  `SceneCueWithRoutesArgs` (unknown scene, route or slot fails and changes nothing).
- Feedback is pushed on a curated property list through a 150 ms debounce so meters and fps never
  spam a transport (`W/Services/StudioControlSurface.cs:25-55,127-130`).
- `GET /snapshot` serves the snapshot the shell already holds, never a core round trip, inside an
  envelope that always states `available`, `ageMs` and `stale` (2000 ms)
  (`C/Http/HttpControlRouter.cs:72-120`). Redaction walks the JSON: secret-named values (suffix
  match on `key token secret password passphrase jwt zak credential`, plus five exact names) become
  `[redacted]`, and every surviving string passes the support-bundle value filter
  (`native-shell/CoreVideoPro.MediaCore/Services/CoreSnapshotObserver.cs:52-114`). Proven by
  `CT/HttpSnapshotRouteTests.cs` and `native-shell/CoreVideoPro.MediaCore.Tests/CoreSnapshotObserverTests.cs`.

## 3. Mac today

The roadmap (line 42) and both 11-14 audits say the Mac has no listener. That is out of date:
commits `7bc70f76` (#1211) and `c01a9fb7` shipped most of M2.1. The 11-15 audits say "9 actions";
the code has 13.

**Exists and tested**

- Catalog, binder, coercion and manifest writer, ported by hand with Windows text verbatim
  (`M/ControlApi.swift:58-220`). 13 actions (`:62-93`); 10 feedback fields (`:98-101`).
- Router with the Windows routes, statuses and bodies (`M/ControlApi.swift:318-457`).
- Snapshot envelope and redactor, including a linear-time JWT scan (`M/ControlApi.swift:393-610`).
- HTTP/1.1 and `/ws` on `NWListener`, port 8011, same three environment variables, same LAN-needs-token
  refusal (`M/ControlHttpServer.swift:40-86,332-410`).
- **Hardening to keep** (`M/ControlHttpServer.swift:6-25`): no CORS header at all; `POST /invoke`
  requires `Content-Type: application/json`; on a loopback bind the `Host` header must name
  loopback (DNS-rebinding guard, `:135-148`); a WebSocket upgrade carrying `Origin` is refused when
  no token is configured; constant-time token compare. Bounds: 16 KiB head, 64 KiB body, 64
  connections, 10 s to read a request (the timer is cancelled once the request reaches the router,
  `:536`), 1 MiB unsent WebSocket backlog.
- Dispatch onto `AppModel` on the main actor with the Windows wrappers
  (`M/ControlSurface.swift:76-191`), `/state` with 11 fields (`:208-224`), and a 100 ms
  latest-value throttle for `/ws` pushes (`:269-308`).
- 20-odd harness cases (`M/ControlApiTests.swift`, registered at `M/ShellTests.swift:4059`), run in
  CI by `COREVIDEO_SHELL_TESTS=1` (`.github/workflows/ci.yml:153`).

**Weak**

- The Mac catalog is a hand copy. Its proof is a golden string pasted into the test file, produced
  once by running the Windows code under dotnet on 2026-11-14 (`M/ControlApiTests.swift:58-83`).
  A Windows action added tomorrow does not fail any Mac test.
- There is no Mac closure test. The dispatch `default:` returns "Unknown action" at run time
  (`M/ControlSurface.swift:136-138`).

**Missing**

- 97 actions (section 5.1). OSC: only the address strings exist (`M/ControlApi.swift:103-108`).
- Most `ControlState` fields: `inputs[]`, `zoomStatus`, `engineStatus`, `lowerThirdOnAir`, audio
  monitor and route revisions, `viewMode`, graphics strip and playlist (`C/ControlState.cs:50-172`).
- `OscExposure` and dynamic providers. The Mac needs neither while OHG is out of scope.

**Shared core.** The control surface is shell-only. No action here needs a new core command; each
maps to a command the Mac shell already sends, or is blocked on a feature another spec delivers.

## 4. Design

### 4.1 One manifest, two hosts

Windows stays the authoring source. Three new checked-in files under `contracts/` make the Mac
answer to it. All three paths are **new**.

1. **`contracts/control.manifest.json`** is the exact output of `ControlManifest.Build().ToJson()`
   (static registry only). A new test in `CT/` regenerates it and fails when the file differs, with
   an environment switch to rewrite it. Nobody edits it by hand.
2. **`contracts/control.platforms.json`** is hand-authored: one row per action id with
   `mac` = `supported`, `blocked` or `notApplicable`, plus `blockedOn` (a workstream name from
   section 5.1) and `reason` (the sentence the operator sees). The same `CT/` test fails if any
   registry id lacks a row or a row names an unknown id. **Adding a Windows action therefore forces
   a Mac decision in the same PR.**
3. **`contracts/control.fixtures.json`** holds wire cases both hosts must answer identically
   against a fake surface: `{method, path, contentType, body}` to `{status, body}` for manifest
   shape, every bind error, coercion, reply shapes (`previewSceneId`, `data`), 404, and the
   `/snapshot` envelope and redaction cases. A second block holds OSC cases: hex datagram to
   decoded message, address to action id, and `ControlState` to the ordered feedback messages.

**Generated Swift.** `contracts/generate.mjs` already emits checked-in Swift and `contract:check`
rejects stale output (`contracts/README.md:3-7`, `package.json:54-55`). It gains a step that reads
the two JSON files and writes `M/ControlManifest.generated.swift` (**new**): the full 110-action
table in Windows order, the feedback field list, and each action's Mac availability. The hand
table at `M/ControlApi.swift:62-101` is deleted. `ControlCatalog.actions` becomes the generated
table filtered to `supported`.

**Two closure tests.**

- Mac (new harness case `control/closure`): the set of ids the Mac dispatcher handles equals the
  generated `supported` set exactly. `AppModelControlSurface` gains a static
  `supportedActionIds: Set<String>` (**new**), the twin of Windows `SupportedActionIds`.
- Mac (new `control/fixtures`): runs every case in `control.fixtures.json` through `ControlRouter`
  with the existing `FakeSurface`. Windows runs the same file through `HttpControlRouter` with
  `CT/FakeControlSurface.cs`. The pasted golden in `M/ControlApiTests.swift:58-83` is removed.

I recommend generation **and** fixtures, not one or the other. Generation removes the copy;
fixtures prove the behaviour around the copy (binder, router, redactor, OSC codec), which is where
a re-implementation actually drifts.

**What the Mac serves.** `GET /manifest` lists only `supported` actions, so Companion never draws
a button that cannot work (the existing rule at `M/ControlApi.swift:14-19`). `FeedbackFields`
lists only fields the Mac publishes truthfully.

**Invoking an action the Mac does not have** (owner decision 1). Recommended: an id that is in the
shared manifest but `blocked` or `notApplicable` answers **422** with the row's `reason`, for
example `"'scene.tiles.names.set' is not available on macOS yet (CoreVideo Tiles)."`. An id in
neither file keeps Windows' 400 `Unknown action '<id>'.` Today both cases answer 400.

### 4.2 Dispatch

`AppModelControlSurface.dispatch` (`M/ControlSurface.swift:76`) stays one switch, split into
per-family extensions (`ControlSurface+Zoom.swift` and so on, **new** files) so it does not become
a 1,400-line file like its Windows twin. Rules:

- Every action calls the entry point the Mac UI already uses. No action re-implements logic.
- Where the Mac entry point is fire-and-forget and Windows awaits an outcome, the model gains an
  awaitable variant. Needed for `zoom.join` and `zoom.leave` (Windows answers from `IsInMeeting`,
  `W/Services/StudioControlSurface.cs:228-251`; the Mac `joinZoom` at `M/AppModel.swift:2177`
  returns nothing) and `source.capture.online.set`. Proposed **new** model methods:
  `joinZoomForControl(target:displayName:) async -> ControlInvokeResult`,
  `leaveZoomForControl() async -> ControlInvokeResult`,
  `setCaptureDeviceOnline(_:online:) async -> ControlInvokeResult`.
- **Source ids.** Windows actions take canonical ids (`zoom:<pid>`, `capture:<id>`, `media:<id>`).
  Mac slots store a `kind` and a bare `sourceId`. One pure helper, `ControlSourceId` (**new**),
  converts both ways and is the only place that knows the mapping. The Sources rebuild (gap 2.4)
  may change the Mac model; the helper is the single point to update.
- Refusals carry Windows' text where one exists. Each ported refusal is a fixture case.

### 4.3 State and feedback

`AppModelControlSurface.state` (`M/ControlSurface.swift:208`) grows toward the Windows
`ControlState` shape, one slice at a time, using the exact Windows field names
(`C/ControlState.cs:50-172`). A field is added only when the Mac can state it truthfully.

- Slice RC2 adds `zoomStatus`, `engineStatus`, `inputs[]` (`slot, inShow, kind, sourceId, name`),
  `zoomParticipantIds`, `virtualCameraOn`.
- Slice RC3 adds `lowerThirdOnAir`, `audioMonitorOn`, `audioMonitorVolume`,
  `audioMonitorAuthorityEpoch`, `audioMonitorRevision`, `audioRouteAuthorityEpoch`,
  `audioRouteRevision`, `audioRoutes[]`, `masteringOn`, `masteringTarget`, `multiviewTileCount`,
  `audioSources[]`. The epoch and revision values come from the projections the Mac already keeps
  (`M/MonitorControlProjection.swift:21-22`, `M/AudioRouteControlProjection.swift:20-21`).

**Push rate.** Today a push happens when the encoded state differs from the last one
(`M/ControlSurface.swift:301-308`). Once `audioSources[]` carries meter levels that would push ten
times a second forever. The comparison key therefore **excludes meter fields** (`audioSources`
level, peak and RMS, `programTruePeakDbfs`, `programLoudnessLufs`); they still appear in `/state`
and ride along on a push caused by something else. This is the Mac form of Windows' curated
`FeedbackProps`.

**Threading.** State is built on the main actor at most every 100 ms. Socket I/O stays on the
server's private serial queue. Nothing here touches the render or audio path, so there is no
60 fps exposure; RC2 still records main-thread time for a state build with ten inputs assigned
and the budget is under 1 ms.

### 4.4 OSC

Three **new** files: `M/ControlOscCodec.swift` (pure: encode, decode, bundles),
`M/ControlOscRouter.swift` (pure: address to id, availability, bind, invoke; feedback encoding in
Windows' message order) and `M/ControlOscServer.swift` (UDP socket).

- Port 8010, `COREVIDEO_OSC_PORT`, loopback unless both `COREVIDEO_OSC_LAN=1` and
  `COREVIDEO_OSC_TRUSTED_NETWORK=1`; the same log line as Windows when only the first is set.
- Decoded values become the binder's transport types: `i` to Int64, `f` to Double, `s` to String,
  `T`/`F` to Bool. The existing coercion (`M/ControlApi.swift:140-189`) does the rest.
- Unknown address: ignored. Malformed datagram: ignored. Datagram over 64 KiB: dropped.
- Feedback targets: senders seen in the last five minutes, capped at 16 endpoints (the cap is
  **new**; Windows is unbounded). Feedback uses the same throttle and comparison key as `/ws`.
- OSC carries no token on either platform. It stays loopback-only by default, and the Mac's HTTP
  hardening is not weakened by it: browsers cannot send UDP.
- A start failure (port in use) logs, raises the existing warning banner
  (`M/ControlSurface.swift:296`), and never blocks HTTP or the app.

### 4.5 Operator-visible states and persistence

- Settings gains a read-only **Remote control** row: HTTP endpoint, OSC endpoint, loopback or LAN,
  token required or not, connected `/ws` clients, last refused action. Windows only logs these; the
  row is a Mac extra in the Health-extras spirit and costs one view.
- Ports, LAN opt-in and token stay environment-only, as on Windows. No new persistence.
- Refusals reach the caller as 422 text and the launch log. Remote actions never raise a modal.

## 5. Action inventory

### 5.1 Workstream codes

| Code | Meaning | Where it is delivered |
|---|---|---|
| NOW | On the Mac today | shipped |
| RC2, RC3, RC5, RC7 | Underlying Mac feature exists; lands in that slice of this spec | this spec |
| STU | Studio / Settings follow-ons: auto-assign toggle (M1.5), multiview overlay toggles (row B24), starter-scene restore and named routes, view modes, program-buffer preference | Studio and Settings specs |
| AUD | Audio parity (gap 2.9): Zoom audio mode, limiter switch | Audio spec |
| MED | Media clip playback (gap 2.1) plus the `volt-rundown` media kind (M2.4) | Media spec |
| TIL | CoreVideo Tiles (gap 2.3, M5) | Tiles spec |
| GFX | Graphics engine and Operate surface (gap 2.2, M3/M4) | Graphics spec |
| OUT | SRT and NDI output (gap 2.11) | Ingest/output spec |
| BRW | Browser sources (gap 2.10) | Browser spec |
| N/A | Out of scope for Mac 1.0 by settled decision | never |

### 5.2 Every Windows action

Parameters are positional; `?` marks optional. Types: s string, i int, d double, b bool.
Line numbers are in `C/ControlActionRegistry.cs`.

| # | Action id (line) | Params | Mac | Depends on (Mac feature) | Slice |
|---|---|---|---|---|---|
| 1 | `zoom.join` (45) | meetingUrl s, displayName s?, webinar b? | no | `joinZoom` exists (`M/AppModel.swift:2177`); needs awaitable variant. `webinar` on Mac UNVERIFIED | RC2 |
| 2 | `zoom.leave` (52) | none | no | `leaveZoom` (`:2234`) | RC2 |
| 3 | `transport.ftb.toggle` (55) | none | yes | | NOW |
| 4 | `transport.ftb.set` (56) | enabled b | yes | | NOW |
| 5 | `transport.take` (58) | none | yes | | NOW |
| 6 | `transport.transition.set` (59) | mode s | no | `setTakeTransition` (`:2972`) | RC2 |
| 7 | `transport.record.toggle` (61) | none | yes | | NOW |
| 8 | `transport.record.set` (62) | on b | yes | | NOW |
| 9 | `transport.stream.toggle` (64) | none | yes | | NOW |
| 10 | `transport.stream.set` (65) | on b | yes | | NOW |
| 11 | `transport.stream.rtmp.configure` (67) | enabled b, protocol s, serverUrl s, streamKey s? | no | Stream URL and Keychain key exist (`M/StreamKeychain.swift`) | RC2 |
| 12 | `transport.stream.srt.configure` (69) | enabled b, mode s, host s, port s, latencyMs s, streamId s, keyLength s, passphrase s? | no | SRT output | OUT |
| 13 | `transport.ndi.toggle` (72) | none | no | NDI output (Mac core built without it, `M/StudioFooter.swift:93-97`) | OUT |
| 14 | `transport.ndi.set` (73) | on b | no | NDI output | OUT |
| 15 | `transport.ndi.graphicsKey.set` (75) | on b, name s? | no | NDI graphics key is out of scope | N/A |
| 16 | `transport.engine.toggle` (77) | none | no | `setCapture(enabled:)` (`:1889`) | RC2 |
| 17 | `transport.engine.set` (78) | on b | no | same | RC2 |
| 18 | `transport.virtualcam.toggle` (80) | none | yes | | NOW |
| 19 | `transport.virtualcam.set` (81) | on b | yes | | NOW |
| 20 | `transport.virtualcam.mirror.set` (83) | on b | yes | | NOW |
| 21 | `transport.virtualcam.name.set` (85) | name s? | yes | | NOW |
| 22 | `scene.select` (89) | sceneId s | yes | | NOW |
| 23 | `scene.cueWithRoutes` (91) | sceneId s, routes s (JSON) | no | Named scene routes. Mac route ids are generated per layer (`:2557-2657`), not Windows route ids | STU |
| 24 | `scene.route.set` (98) | sceneId s, routeId s, slot i | no | same | STU |
| 25 | `scene.list` (106) | none | yes | | NOW |
| 26 | `scene.restoreStarters` (108) | none | no | Starter-scene restore (studio audit section 9); no Mac entry point found | STU |
| 27 | `scene.dynamicGallery.create` (110) | none | no | Tiles | TIL |
| 28 | `scene.tiles.autofill.set` (112) | enabled b | no | Tiles | TIL |
| 29 | `scene.tiles.slot.assign` (114) | slot d, sourceId s? | no | Tiles | TIL |
| 30 | `scene.tiles.exclusion.set` (116) | sourceId s, excluded b | no | Tiles | TIL |
| 31 | `scene.tiles.background.set` (118) | color s, sourceId s? | no | Tiles | TIL |
| 32 | `scene.tiles.override.set` (120) | sourceId s, x d, y d, width d, height d, cropLeft d, cropRight d, order d | no | Tiles | TIL |
| 33 | `scene.tiles.override.clear` (124) | sourceId s | no | Tiles | TIL |
| 34 | `scene.tiles.names.set` (126) | enabled b | no | Tiles | TIL |
| 35 | `view.setMode` (128) | mode s | no | Mac has no operator view mode (grep only, UNVERIFIED); owner decision 3 | STU |
| 36 | `media.rundown.load` (131) | clip s | no | Media bin `volt-rundown` kind | MED |
| 37 | `media.rundown.preview` (133) | clip s, scene s, route s? | no | Clip playback and media routes | MED |
| 38 | `media.rundown.take` (136) | clip s, scene s, route s? | no | same | MED |
| 39 | `input.assign` (141) | slot i, sourceId s | no | `assignSlot` (`:1816`) | RC2 |
| 40 | `source.capture.online.set` (147) | sourceId s, online b | no | `connectCaptureDevice` (`:4510`) | RC2 |
| 41 | `input.unassign` (149) | slot i | no | `unassignSlot` (`:1827`) | RC2 |
| 42 | `input.name` (151) | slot i, name s? | no | Slot name edit (`M/ShellApp.swift:1790`) | RC2 |
| 43 | `source.dropout.set` (157) | sourceId s, policy s | no | `setDropoutPolicy` (`:2498`) | RC2 |
| 44 | `input.inShow.set` (163) | slot i, on b | no | `toggleSlotInShow` (`:1836`) | RC2 |
| 45 | `graphics.lowerThird.toggle` (171) | none | no | Legacy lower third (`showLowerThird`/`hideLowerThird`, `:3841,3869`) | RC3 |
| 46 | `graphics.lowerThird.set` (172) | on b | no | same | RC3 |
| 47 | `graphics.caption.set` (174) | text s, speaker s? | no | Legacy caption cue (`M/Overlays.swift:14`); mapping UNVERIFIED | RC3 |
| 48 | `graphics.graphic.toggle` (180) | graphicId s | no | Graphic ids | GFX |
| 49 | `graphics.automation.configure` (182) | request s | no | Native graphics rules | GFX |
| 50 | `graphics.automation.control` (183) | request s | no | same | GFX |
| 51 | `graphics.authority.set` (184) | request s | no | Graphics instances | GFX |
| 52 | `graphics.instance.create` (186) | request s | no | same | GFX |
| 53 | `graphics.instance.update` (188) | request s | no | same | GFX |
| 54 | `graphics.instance.cue` (190) | request s | no | same | GFX |
| 55 | `graphics.instance.trigger` (192) | request s | no | same | GFX |
| 56 | `graphics.clock.control` (194) | request s | no | same | GFX |
| 57 | `graphics.instance.remove` (196) | request s | no | same | GFX |
| 58 | `graphics.program.clear` (198) | request s | no | same | GFX |
| 59 | `graphics.template.install` (200) | request s | no | Template catalog | GFX |
| 60 | `graphics.template.remove` (202) | request s | no | same | GFX |
| 61 | `graphics.template.list` (204) | request s | no | same | GFX |
| 62 | `graphics.template.select` (207) | templateId s, version i | no | Graphics workspace | GFX |
| 63 | `graphics.cue.create` (209) | templateId s, version i, scope s, sceneId s? | no | same | GFX |
| 64 | `graphics.studio.assign` (211) | followProgramSource b | no | same | GFX |
| 65 | `graphics.image.import` (213) | path s | no | same | GFX |
| 66 | `graphics.logo.import` (216) | path s | no | same | GFX |
| 67 | `graphics.cue.select` (219) | instanceId s | no | same | GFX |
| 68 | `graphics.cue.action` (221) | action s | no | same | GFX |
| 69 | `graphics.cue.save` (223) | name s | no | same | GFX |
| 70 | `graphics.saved.restore` (225) | savedId s | no | same | GFX |
| 71 | `graphics.playlist.list` (231) | none | no | Playlist (Operate surface) | GFX |
| 72 | `graphics.playlist.row.set` (232) | rowJson s | no | same | GFX |
| 73 | `graphics.playlist.clock` (234) | rowId s, action s | no | same | GFX |
| 74 | `graphics.playlist.row.remove` (236) | rowId s | no | same | GFX |
| 75 | `graphics.playlist.hotkey.set` (238) | rowId s, key s? | no | same | GFX |
| 76 | `graphics.playlist.default` (240) | starterId s? | no | same | GFX |
| 77 | `graphics.playIn` (242) | rowId s, sourceId s? | no | same | GFX |
| 78 | `graphics.toPreview` (244) | rowId s, sourceId s? | no | same | GFX |
| 79 | `graphics.playOut` (246) | rowId s?, sourceId s?, instanceId s? | no | same | GFX |
| 80 | `graphics.preview.remove` (248) | instanceId s | no | same | GFX |
| 81 | `graphics.playOutAll` (250) | none | no | same | GFX |
| 82 | `graphics.preview.clear` (251) | none | no | same | GFX |
| 83 | `graphics.strip` (252) | none | no | On Air strip | GFX |
| 84 | `graphics.layout.list` (254) | none | no | Shot Layouts | GFX |
| 85 | `graphics.layout.set` (255) | layoutJson s | no | same | GFX |
| 86 | `graphics.layout.remove` (257) | layoutId s | no | same | GFX |
| 87 | `graphics.layout.cue` (259) | layoutId s | no | same | GFX |
| 88 | `graphics.layout.take` (261) | layoutId s | no | same | GFX |
| 89 | `audio.zoomMode.set` (265) | mode s | no | Zoom audio mode toggle | AUD |
| 90 | `audio.monitor.set` (267) | on b, authorityEpoch s?, expectedRevision d?, operationId s? | no | `setMonitor` and `MonitorControlProjection` (`:4441`) | RC3 |
| 91 | `settings.programBuffer.set` (272) | frames d | no | No Mac program-buffer preference (grep empty); core support on Mac UNVERIFIED | STU |
| 92 | `audio.monitor.volume` (274) | volume d, authorityEpoch s?, expectedRevision d?, operationId s? | no | same as 90 | RC3 |
| 93 | `audio.route.set` (279) | sourceId s, busId s, enabled b, gainDb d, authorityEpoch s, expectedRevision d, operationId s | no | `AudioRouteControlProjection` and route edits (`:3604-3660`) | RC3 |
| 94 | `audio.masterLimiter.set` (287) | on b | no | Mac sends `limiterEnabled: true` unconditionally (`:4170`); needs a switch | AUD |
| 95 | `audio.mastering.set` (289) | on b | no | `editMastering` (`:3812`) | RC3 |
| 96 | `audio.mastering.target` (291) | index i | no | `MasteringParams.targetLufs` (`:250`); index maps to -14/-16/-23 | RC3 |
| 97 | `audio.vst.scan` (293) | none | no | `scanVstPlugins` (`M/VstInserts.swift:264`) | RC3 |
| 98 | `multiview.layout.set` (296) | mode s | no | `configureMultiviewer` (`:942`) | RC3 |
| 99 | `multiview.tileCount.set` (298) | count i | no | `multiviewTileCount` (`:1979`). Windows clamps (`W/ViewModels/StudioViewModel.cs:3002`); Mac range is 1-16 (`M/ProductionSettings.swift:22`) | RC3 |
| 100 | `multiview.showLabels.set` (300) | on b | no | Overlay toggles (row B24); labels always on today | STU |
| 101 | `multiview.showTally.set` (302) | on b | no | same | STU |
| 102 | `multiview.showMeters.set` (304) | on b | no | same; no meter overlay on Mac | STU |
| 103 | `multiview.showClock.set` (306) | on b | no | same | STU |
| 104 | `automation.autoAssignInputs.set` (310) | on b | no | Mac auto-assigns unconditionally (`:1752-1760`); needs the M1.5 toggle | STU |
| 105 | `automation.lowerThirds.set` (312) | on b | no | Native graphics rules on Windows (`W/Services/StudioControlSurface.cs:700-705`) | GFX |
| 106 | `automation.captions.set` (314) | on b | no | None: true is refused, false is a no-op (`:709-712`) | RC3 |
| 107 | `browser.add` (318) | url s, width i?, height i? | no | WKWebView browser host | BRW |
| 108 | `browser.remove` (325) | browserId s | no | same | BRW |
| 109 | `browser.reload` (327) | browserId s | no | same | BRW |
| 110 | `showEngine.restart` (332) | none | no | OHG show engine is out of scope | N/A |

Totals: NOW 13, RC2 12, RC3 12, STU 12, AUD 2, MED 3, TIL 8, GFX 42, OUT 3, BRW 3, N/A 2 = 110.
After RC2 and RC3 the Mac answers 37 actions. The dynamic `ohg.*` actions are not in the static
registry and stay out of scope.

There are **no** comms or talkback actions and no clip-transport actions (play, pause, seek) in
the Windows registry. Comms (M7) and media transport therefore add nothing to this spec beyond
`media.rundown.*`.

## 6. Slices

| Id | What lands | Lane | Size | Depends on | Proven by |
|---|---|---|---|---|---|
| RC1 | Shared manifest: the three `contracts/` files, the Windows export and row-coverage test, generated `ControlManifest.generated.swift` replacing the hand table, Mac closure test, shared fixtures run on both hosts, the 422 "not available on macOS yet" reply. Consumer: the 13 live actions now run from the generated table. | control | M | none | `contract:check`; `CT/` export test; Mac `control/closure` and `control/fixtures`; mutation: add a fake registry action and see both the Windows row test and the Mac generator check fail |
| RC2 | Session and inputs: actions 1, 2, 6, 11, 16, 17, 39 to 44; `ControlSourceId`; awaitable join, leave and capture-online; state fields `zoomStatus`, `engineStatus`, `inputs[]`, `zoomParticipantIds`, `virtualCameraOn`; meter-free push key. | control | M | RC1 | Harness cases per action against the real `AppModel` with the test bridge; fixture cases for each refusal; owner check 1 |
| RC3 | Audio, multiview and legacy overlays: actions 45 to 47, 90, 92, 93, 95 to 99, 106; state fields listed in 4.3. | control | M | RC1 | Harness cases; revision-conflict cases for 90, 92, 93 ported from `CT/ControlActionRegistryTests.cs:106,121`; owner check 2 |
| RC4 | OSC: codec, router, UDP server, feedback, environment gating, the Settings "Remote control" row. | control | M | RC1 (RC2 and RC3 make it useful) | OSC fixture block on both hosts; loopback UDP end-to-end harness case (twin of `CT/OscControlServerTests.cs:11,37`); owner check 3 |
| RC5 | Companion verified against the Mac: run the module's own tests, fix any module assumption, confirm all seven presets, update the module README table for macOS. | control | S | RC2, RC3, STU auto-assign toggle for one preset | `companion-module-corevideopro` vitest; owner check 4 on a real Stream Deck |
| RC6 | Volt rundown: actions 36 to 38 with the Windows refusal texts quoted in the social-comms audit section 3. | control | S | MED | Fixture cases for the three refusals; drill: stage a clip file, load, preview, take over HTTP and confirm the clip frame in a Program readback |
| RC7 | Follow-on batches, one PR per unblocked workstream. Each PR flips rows in `control.platforms.json` from `blocked` to `supported`, adds the dispatch cases and the state fields: STU (12), AUD (2), TIL (8), GFX (42, in the graphics spec's own slice order: playout 71 to 88 first, then instances and templates), OUT (3), BRW (3). | control | S to M each; GFX is L | the named spec | The closure test fails until dispatch exists for every flipped row; per-family harness cases |

Order: RC1, RC2, RC3, RC4, RC5, then RC6 and RC7 as their features arrive. This differs from the
roadmap's `M2.1, M2.2, M2.4, M2.3`: M2.1 is already shipped, and M2.3 "full coverage" cannot be one
slice because 73 actions wait on other workstreams.

For RC7 the owning feature spec should treat "flip the rows and add the dispatch" as part of its
own done criteria, so no feature ships on Mac without its remote actions.

## 7. Acceptance and test strategy

**Unit and contract (CI on both platforms)**

- `contract:check` fails on a stale generated Swift file.
- Windows: export test (manifest file is current), row test (every id has a platform row), fixture
  run through `HttpControlRouter` and the OSC router.
- Mac harness: `control/closure`, `control/fixtures`, plus one case per action family that drives
  the real `AppModel` through the test seam the existing cases use
  (`M/ControlApiTests.swift:456-600`). Every existing hardening case stays: no CORS header, Host
  guard, content-type rule, WebSocket Origin rule, bounds, slow-client drop.
- Each ported Windows refusal is written as a Swift case that fails on the current Mac first.
- Mutations to run in each slice plan: remove one dispatch case (closure fails); mark a supported
  row blocked (closure fails); change one description in the registry (export test fails); drop the
  meter exclusion from the push key (a push-count case fails).

**End to end**

- A **new** read-only script `scripts/control-conformance.mjs` takes a base URL and optional token
  and runs the fixture file's live-safe cases (manifest, bind errors, 404, snapshot envelope)
  against a running shell on either platform. It is the cross-host proof for the owner and for
  release qualification.
- `scripts/mac-show-drill.py` today talks to the core directly. RC2 adds an optional mode that
  performs join-free setup (input assign, scene select, Take, record start and stop) over HTTP
  against a running shell, and asserts each step in `GET /snapshot`.

**Pixels and audio.** Action results that claim a picture or sound are proven with pixels or audio:
the RC2 Take and transition cases reuse the existing Program readback proof; the RC3 lower-third
case asserts the keyed pixels through `COREVIDEO_SHELL_SNAPSHOT`; monitor on and off is an owner
listening check.

**Live owner checks**

1. Companion on the same Mac: join a meeting, assign two guests, set a fade transition, Take.
2. Monitor on, volume to 0.3, mastering on, from Companion; hear it.
3. TouchOSC or `oscsend` on loopback: `/cvp/transport/take`, and see `/cvp/state/canTake` return.
4. A physical Stream Deck running the seven presets; every button lights correctly.
5. From a second machine with `COREVIDEO_HTTP_LAN=1` and a token: works with the token, 401
   without, and the app refuses to open the port when the token is blank.

## 8. Risks and unknowns

| Risk | Resolution |
|---|---|
| Network.framework UDP models each remote endpoint as a connection; replying from port 8010 to a sender's source port, and listener behaviour with many senders, is UNVERIFIED. | Half-day spike at the start of RC4. Fallback: a BSD datagram socket on a DispatchSource. The codec and router are pure either way. |
| macOS Local Network privacy may prompt when the shell sends OSC feedback to a LAN address; the bundle has no `NSLocalNetworkUsageDescription`. The application firewall may prompt on a LAN bind. | Owner check 5 on the notarized build. Add the usage string in RC4 if the prompt appears. Loopback is unaffected. |
| Mac `zoom.join` semantics differ: the Mac has a passcode field and no verified webinar mode. | RC2 reads `parseZoomTarget` (`M/AppModel.swift:2179`) and the engine join command before fixing the mapping; `webinar=true` answers 422 until verified. |
| Mac source ids are not the Windows canonical ids, and the Sources rebuild may change them. | `ControlSourceId` is the only mapping point; the Sources spec owns keeping its tests green. |
| The Windows export test needs dotnet in the Mac contributor loop. | dotnet runs on this Mac for `CoreVideoPro.Control.Tests`; CI already runs `test:native-shell`. |
| A bigger `/state` built on the main actor could cost UI time. | Measure in RC2 (budget under 1 ms); if over, cache per-family fragments and rebuild only the dirty one. |
| `graphics.caption.set` and `multiview.tileCount.set` may not map cleanly to the Mac's legacy overlay and 1-16 range. | Verify in RC3; if either does not, move the row to GFX or STU with a reason. The platform file makes that a one-line change. |
| Long invokes (join can take many seconds) hold a connection. | The 10 s timer covers only reading the request (`M/ControlHttpServer.swift:536`). RC2 adds a case with a slow fake surface to pin it. |

## 9. Owner decisions needed

1. **Reply for a Windows action the Mac does not have yet.** Recommended: 422 with a plain reason
   naming the missing feature, while `GET /manifest` continues to list only what works. The
   alternative is today's 400 "Unknown action", which is indistinguishable from a typo.
2. **OSC LAN opt-in on Mac.** Recommended: identical to Windows (two environment variables, no
   token), because OSC clients cannot send one. The stricter alternative, loopback-only OSC on
   Mac, would block a TouchOSC tablet on the control-room network.
3. **`view.setMode`.** The Mac studio has no program / preview / multiview view switch.
   Recommended: leave it `blocked` on the Studio workstream and decide there whether the Mac gets
   view modes; do not invent a Mac-only meaning for the action.
4. **Settings "Remote control" status row** (a Mac extra). Recommended: yes; it is the only place
   an operator can see whether Companion is connected without reading the log.

## 10. Out of scope

- Commands over WebSocket (Windows drains inbound frames too).
- TLS, user accounts, or any authentication beyond the bearer token.
- Dynamic action providers, `ohg.*`, `showEngine.restart`, `transport.ndi.graphicsKey.set`.
- A settings UI for ports, LAN opt-in or the token (environment-only on both platforms).
- Changing the Windows CORS wildcard. It is noted here as a Windows-side hardening candidate and
  should be filed as its own backlog issue.
- New core commands. Every action maps to a command the shell already sends or to a feature
  another spec delivers.
