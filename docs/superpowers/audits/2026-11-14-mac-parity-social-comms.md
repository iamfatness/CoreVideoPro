# macOS parity audit: Social, Comms, Volt rundown and external integrations (2026-11-14)

Scope: `main` @ `00838940` (Release 1.0.0). Covers every commit in
`git log --since=2026-10-09 origin/main` (959 commits) that touches social graphic
review (#1110/#1113/#1118), Comms talkback (#1207), Volt rundown staging
(#1201/#1202), production endpoints (#434/#436, `75bb8cca`, `fd0015e8`), secrets at
rest, and the Companion/OSC/HTTP control surface. The method follows
`docs/mac-parity-plan.md` §1-§3. OHG is ignored (§3). This is a read-only audit:
nothing was built or run. Every Mac claim below comes from a grep of
`mac-shell/Sources/CoreVideoProShell/*.swift` (13,693 lines) or
`native/zoom-engine/engine/main-macos.mm` (2,381 lines). Claims I could not prove
are marked **UNVERIFIED**.

Sibling audit: `docs/superpowers/audits/2026-11-14-mac-parity-graphics.md`. Its §0
finding blocks the whole Social section here (see §1.0).

## Summary

| # | Feature | Class | Effort | Deps | Mac evidence |
|---|---|---|---|---|---|
| 1a | Social: native submission gate (`social-review-*` private cue, 10 s/15 s expiry, preview exclusion) | SHARED-DONE (logic); **blocked on Metal** | none (core) | Metal graphics backend (graphics audit §0) | Gate code is portable (`GraphicsInstances.cpp:508-518`), but a Metal core refuses every `graphics-*` command: `MediaCore.cpp:1256-1258`, `Interfaces.h:1480`. Only `D3D11CompositorAdapter.cpp:268` returns true. |
| 1b | Social: exact-version review model + gated playout owner | SHELL-LOGIC | L | 1a, Mac graphics playout (none today) | `grep -i social` gives 0 hits in mac-shell. No `graphics-instance-*` command is sent anywhere (`CoreCommands.swift` has no `"graphics` entry). |
| 1c | Social: YouTube live chat transport (API key) | SHELL-LOGIC | M | 1b, Keychain item | `grep -i youtube` hits only the RTMP default URL (`AppModel.swift:385`, `ShellPrefs.swift:80`). |
| 1d | Social: Twitch device authorization + EventSub moderation | SHELL-LOGIC | L | 1b, Keychain item | `grep -i twitch` gives 0 hits. |
| 1e | Social: review dialog UI (Operate → Social review) | MAC-UI-ONLY | M | 1b-1d, a Graphics Operate surface on Mac | none |
| 2a | Comms: core relay, `comms-*` verbs, `zoom-comms-state` fact | SHARED-DONE | none | n/a | `ZoomCommsRelay.cpp` in the shared lib (`native/CMakeLists.txt:183`). Comms tests are in the cross-platform test target (`CMakeLists.txt:993-1000`). macOS CI run of those tests is UNVERIFIED. |
| 2b | Comms: Mac Zoom engine talkback (ZComms station in the engine) | ENGINE-WORK | XL | ZComms `talkback_sdk_mac.mm` (exists upstream, not ported); live co-host meeting to qualify | `main-macos.mm` has 0 comms/talkback hits. The reader loop has no comms branch and silently drops `comms_*` lines (`main-macos.mm:2300-2342`). The Mac engine target links no comms libs (`CMakeLists.txt:735-757`). |
| 2c | Comms: shell page, leases, release rules, Private comms pref | MAC-UI-ONLY + SHELL-LOGIC | L | 2b | `grep -i comms` gives 0 hits. `handleEvent` drops `zoom-comms-state` (`AppModel.swift:1002-1021`, `default: break`). |
| 3 | Volt rundown media staging (#1201) | SHELL-LOGIC | S (once deps land) | **#7 control API**, `mediaSources[]` read model, media-clip routes on Mac | `MediaBin.swift:27` kinds lack `volt-rundown`. There is no control endpoint to receive `media.rundown.*`. |
| 4a | Zoom OAuth broker host (#434/#436) | SHELL-LOGIC, **already ported (done)** | none | none | `ZoomOAuth.swift:18-19` = `https://corevideo.iamfatness.us` (matches the owner ruling). Tokens are in the Keychain (`ZoomOAuth.swift:209-253`). |
| 4b | Telemetry heartbeat + crash-report upload (#434/#436) | SHELL-LOGIC | M | owner checklist (hosts do not resolve, no API key), owner decision on Mac upload | Deliberately absent: `Diagnostics.swift:20-21` ("no uploader and no telemetry here"). Crash reports are listed only, never sent (`Diagnostics.swift:316, 422-425`). |
| 5 | Credential stores (secrets at rest) | SHELL-LOGIC | S per secret | lands with each feature | Two Keychain items exist: `StreamKeychain.swift:9` (RTMP key) and `ZoomOAuth.swift:209` (Zoom tokens). **The premise is wrong:** Windows uses DPAPI-encrypted files, not Credential Manager (§5). |
| 6 | Companion / OSC / HTTP+WS control surface | SHELL-LOGIC | XL (full), L (core subset) | none; it **unblocks** #3, Companion and the QA scripts | 0 hits for `NWListener`, `osc`, `companion`, `websocket`, `8011` and `8010`. The Mac exposes no control endpoint at all. |

Totals for this area: about 2×XL + 3×L + 3×M + S items. Comms (2b+2c) and the control
surface (6) are the only items that are not blocked on another audit's work.
Social (1b-1e) cannot ship before the Metal graphics backend exists.

---

## 1. Social graphic review (#1110, #1113, #1118)

### 1.0 Blocker: graphics are not rendered on Metal

The sibling graphics audit (§0) found that the Metal compositor does not override
`supportsGraphicsInstances()`. I re-verified this here:

- `native/src/core/MediaCore.cpp:1256-1258` refuses every `graphics-*` command with
  `graphics-instance-backend-not-built` unless the compositor supports instances.
- `native/src/modules/Interfaces.h:1480`: the default is `false`. Only
  `native/src/modules/D3D11CompositorAdapter.cpp:268` overrides it to `true`.

A social card is a native graphics instance (`social-review-<n>`, using the
`youtube-chat-card` / `twitch-chat-card` templates). So **nothing in this section
can put pixels on a Mac Program until the Metal graphics backend exists.** The
consequence is that the Mac fails closed: a refused command airs nothing. That is
safe, but it means the feature is absent.

### 1.1 What Windows does

The operator flow is in `docs/reference/graphics-playout.md:209-227` (YouTube) and
`:808-860` (Twitch):

- Graphics → Operate → **Social review**. YouTube: enter the 11-character video ID
  and an API-restricted Data API key, then Connect. Twitch: enter a channel and a
  public Client ID, Connect, follow **Open Twitch activation** and enter the code.
  The device-code grant requests only `user:read:chat`
  (`TwitchHttpTransport.cs:19`).
- Select a message and read the full author and text. **Approve** applies to that
  exact revision. **Show approved** is a separate action. Also available: Reject,
  Withdraw, Hide social graphic, and Clear local data. The card expires after 15 s.

Code, with portability noted:

| Piece | File (lines) | Project |
|---|---|---|
| Exact-version review queue (generation + provider ID + revision, tombstones, 200 messages, 15 min expiry, 4096 fence budget) | `MediaCore/Services/GraphicsSocialReview.cs` (233) | plain net9.0 |
| Gated playout: one serialized owner, check before prepare and again just before submit, private cue, `clientId = "graphics-social-review"` | `MediaCore/Services/GraphicsSocialPlayout.cs` (222; `ClientId` at `:9`, prefix at `:10`, sent at `:215`) | plain net9.0 |
| YouTube paged chat: `https://www.googleapis.com/youtube/v3/` with the `x-goog-api-key` header, 15 s minimum poll | `YouTubeChatTransport.cs` (161; origin `:27`, header `:119`), `YouTubeSocialFeed.cs` (62) | plain net9.0 |
| Twitch: device grant at `https://id.twitch.tv/oauth2/device` and `/token`, `/validate` hourly, Helix, EventSub `wss://eventsub.wss.twitch.tv/ws` (4 subscriptions, nanosecond ordering fences, 3 reconnects, 64 KiB frame cap) | `TwitchAuthorization.cs` (124), `TwitchHttpTransport.cs` (163), `TwitchChatSocket.cs` (70), `TwitchEventSubSession.cs` (177), `TwitchSocialFeed.cs` (87) | plain net9.0 |
| Card templates | `contracts/graphics-social/{youtube,twitch}-chat-card.json`, embedded via `CoreVideoPro.MediaCore.csproj:13` | contract (portable) |
| Review dialog + view-model glue | `WinUI/Controls/GraphicsSocialReviewDialog.cs` (201), `WinUI/ViewModels/StudioViewModel.GraphicsSocialReview.cs` (251) | WinUI only |
| Credential stores | `WinUI/Services/YouTubeChatCredentialStore.cs`, `TwitchChatCredentialStore.cs` (DPAPI, `%LOCALAPPDATA%\CoreVideoPro\graphics\social\`) | WinUI only |
| Native gate | `native/src/graphics/GraphicsInstances.cpp:508-518` (refuses `social-review-*` unless `clientId == graphics-social-review`; requires `rideTake:false` and no `cueOwner`; Show must target `cue`; Hide/Clear are always allowed), `:561` (10 s created-hidden expiry), `:1161` (15 s on-air expiry), `:852` (never on Preview) | `native/` (portable) |

Wire: ordinary `graphics-instance-create` / `-cue` / `-trigger` requests with
`instanceId: "social-review-…"`, `clientId: "graphics-social-review"`, and on the cue
`rideTake: false`.

### 1.2 Portability

All social logic is in plain net9.0, about 1,300 lines across 9 files. The Swift
shell cannot reuse C#, so it must be rewritten in Swift. The native gate is shared,
and that is the important safety half: the core itself refuses a social instance
that arrives through any other client. A Swift port that gets the client ID wrong
fails closed rather than airing unreviewed content.

### 1.3 Mac state

- `grep -rniE 'social|twitch' mac-shell/Sources` gives 0 hits.
- `youtube` hits only the RTMP ingest default (`AppModel.swift:385`,
  `ShellPrefs.swift:80`, `Diagnostics.swift:592/610/622`).
- The Mac sends no `graphics-instance-*` command anywhere. Its only graphics surface
  is the legacy lower third (`AppModel.swift:1053-1056`, `set-overlay-asset`).

### 1.4 Class / effort

- 1a SHARED-DONE but blocked on Metal.
- 1b SHELL-LOGIC L.
- 1c M.
- 1d L.
- 1e MAC-UI-ONLY M.

Dependencies: the Metal graphics backend, then a Mac graphics playout service
(template install and instance lifecycle). Both belong to the graphics audit.
Social is the last layer on that stack.

### 1.5 Rulings the Mac must honour

Quoted from `docs/superpowers/specs/2026-11-04-graphics-phase3-slice6-social-feeds.md`:

- §Workflow: "Approval alone sends no native command and leaves Program unchanged."
  Also: "Show the item as on air only when confirmed by the native snapshot. The
  native card ends after 15 seconds even if the console detaches."
- :13: "Approval and rejection require the exact revision displayed in review."
- :14: "On loss of ordering, overflow of the tombstone budget or uncertain
  continuity, clear eligibility and require a fresh generation. Never evict a
  tombstone and continue accepting replays in that generation."
- :15: "No chat payload, author, API key or cursor goes into show files, telemetry,
  crash annotations or logs."
- :16: "Check once before native preparation and again after the prepare wait,
  immediately before submission. … do not show a newer revision using an older
  approval."
- :17: "Ordinary Take, Show-from-cue, hotkeys and the control API cannot use a
  social item. Only the dedicated operator Show action can consume the approval."
  For the Mac this means: when #6 lands, its control surface must not resolve
  `social-review-*` targets.
- :18: "Bound the UTF-8 message to 192 bytes and the author to 80 bytes … Reject
  oversize or undisplayable items rather than silently changing the approved
  content."
- :22 (YouTube): "keep it out of the URI, reject redirects, and do not use the Zoom
  broker."
- `graphics-playout.md`, Twitch: "Every connection gap clears approvals and retires
  its card. … It does not claim migration continuity." Also: "Budget exhaustion
  clears eligibility instead of forgetting a removal."

---

## 2. Comms: Zoom talkback station (#1207, `1284c8bb`, 2026-11-10)

### 2.1 What Windows does

Reference: `docs/reference/comms-talkback.md`.

- **Comms page.** A grid of talk-key cells, with ALL CALL / LATCH / EDIT TALENT /
  SETTINGS / LEAVE, and lamps LINK / MTG MIC / CHANNEL / TX.
- **Who must send.** Talkback must come from a host or co-host participant, and the
  SDK allows one participant per process. So the station is Pro's own Zoom engine
  (`EngineComms` + `engine/comms/**`), built against Meeting SDK 7.1.5.

Data path:
`CommsViewModel` → `MediaCoreSupervisor.SendCommsAsync` (`MediaCoreSupervisor.cs:673`)
→ core `JsonRpcServer` (lock-free, `JsonRpcServer.cpp:122,158,1264`)
→ `MediaCore::zoomCommsCommandUnlocked` (`MediaCore.cpp:808`)
→ `ZoomEngineRuntime::commsCommand` (`ZoomEngineRuntime.cpp:757-772`; `ZoomCommsRelay`
validates) → engine `comms_*` line. The return path is engine `comms_state` →
`zoom-comms-state` fact (`ZoomEngineRuntime.cpp:737-749`, repeated about once per
second while held).

Wire verbs: `comms-key {slot|all, on, leaseMs}`, `comms-latch`, `comms-assign`,
`comms-mic`, `comms-sidetone-out`, `comms-sidetone`, `comms-tone`, `comms-gain`,
`comms-release-all`, `comms-private`. Four of them are urgent in the mailbox:
`comms-key`, `comms-latch`, `comms-release-all` and `comms-private`
(`CommandMailbox.h:41-42`).

Portability:

| Layer | Where | Portable? |
|---|---|---|
| Core relay, lock-free dispatch, urgent mailbox, state fact | `native/src/modules/ZoomCommsRelay.*`, `ZoomEngineRuntime.*`, `JsonRpcServer.cpp` | **yes, shared** |
| Talkback channel ladder, eligibility, duck/room/reach, pacer, `comms_core`, `mic_guard` | `native/zoom-engine/engine/comms/*` (ZComms port), library `corevideo_comms_core` (`CMakeLists.txt:665-681`, built on every platform, linked by the fake engine and the tests) | **yes** (SDK-free) |
| Mic chain (miniaudio capture + sidetone) | `corevideo_comms_audio` (`CMakeLists.txt:775-783`) | portable source (miniaudio has a CoreAudio backend), but **only built in the non-Apple branch** |
| SDK adapter | `talkback_sdk_win.{h,cpp}` (Windows `IMeetingTalkbackController`) | Windows only |
| Engine glue: roster poll, `IsHost`, `GetMeetingTalkbackController`, `MuteAudio`/`UnMuteAudio`, `HasRawdataLicense`, `setExternalAudioSource(&silent_mic)`, `onMicInitialize` | `native/zoom-engine/engine/engine-comms.cpp` (627 lines; e.g. `:308,:375,:443,:473`) | Windows C++ SDK only |
| Protocol parse | `MediaCore/Services/ZoomCommsProtocol.cs` (127), `MediaCore/Models/ZoomCommsModels.cs` (44) | plain net9.0 (Swift rewrite needed) |
| Page, view-model, TalkKey, brush tokens | `WinUI/ViewModels/Comms/CommsViewModel.cs` (614), `Views/CommsPage.xaml` (371) + `.cs` (160), `Controls/TalkKey.cs` (95), `Converters/CommsBrushConverter.cs` (83), `StudioViewModel.Comms.cs` (78) | WinUI only |

### 2.2 What the Mac engine needs vs what exists

**The SDK is not the gap.** The local Mac SDK is ZoomSDK.framework **7.1.5 (84750)**
(the Info.plist under `~/Developer/zoom-sdk-macos`), the same version
`main-macos.mm:10` names. Its headers expose everything the Windows glue uses:

- `ZoomSDKTalkbackController` (`ZoomSDKTalkbackController.h:99-177`): create, invite,
  remove, destroy, `sendAudioDataToChannel`.
- `ZoomSDKMeetingService` getter (`ZoomSDKMeetingService.h:637`).
- `ZoomSDKVirtualAudioMicDelegate -onMicInitialize:` and `setExternalAudioSource:`
  (`ZoomSDKRawDataAudioSourceController.h:33-65`).
- `hasRawDataLicense` (`ZoomSDKRawDataController.h:153`).
- `isHost` and `onMeetingCoHostChanged` (`ZoomSDKMeetingActionController.h:333,640`).

**A macOS adapter exists upstream and was left out of the port.** `PORTED.md` names
ZComms `origin/main @ 168a572d`. That commit is itself *"macOS port P1-A: ZoomClient
and VirtualMic seams (#49)"* (2026-10-08). Its tree contains
`src/zoom/talkback_sdk_mac.{h,mm}` (68 + 247 lines), `src/audio/virtual_mic.h` and
`.github/workflows/macos.yml` (macos-14, SDK 7.1.5.84750). I checked this with
`gh api repos/iamfatness/ZComms/git/trees/168a572…`. Pro's `PORTED.md:12` copied
only `talkback_sdk_win.{h,cpp}`. The adapter header itself says: *"THREADING IS
UNRESOLVED … whether sendAudioDataToChannel: is too decides whether the 20ms TX
pacer can call it directly or must hand frames across."* The `ZoomClient` mac
implementation (P1-B) does not exist upstream yet. The local clone at
`~/Developer/ZComms` (branch `macos-port-p1`, 2026-09-05) is stale against that
commit.

**What exists in the Mac engine:** nothing for comms.

- `main-macos.mm` has 0 hits for `comms` or `talkback`.
- The reader loop (`main-macos.mm:2286-2342`) is an `if/else if` chain over
  `init`/`join`/`leave`/`start_media`/`stop_media`/`subscribe_audio`/`unsubscribe`/
  `subscribe` with **no final else**. So a `comms_*` line is silently discarded and
  no `comms_state` ever comes back. The core then holds `commsState_` empty, and a
  Windows-equivalent shell would sit in "waiting for the engine's report" forever.
  That fails closed: no key reads as live.
- **Latent hazard:** that dispatch is substring-based (`msg.find(IPC_CMD_JOIN)` etc.),
  and `comms-mic` / `comms-sidetone-out` carry a free-text device name. A device
  name containing `init`, `join`, `leave` or `quit` would be routed into an SDK
  action. When the comms branch is added it must be checked **first**, using
  `ipc_command_is`.
- The CMake Apple branch (`CMakeLists.txt:735-757`) links only ZoomSDK, Foundation
  and AppKit. It does not link `corevideo_comms_core` or `corevideo_comms_audio`.
- The engine bundle already declares `NSMicrophoneUsageDescription`
  (`scripts/make-macos-engine-bundle.sh:76`).

**Engine work list (2b):**

1. Link `corevideo_comms_core`, and build `corevideo_comms_audio` on Apple (miniaudio
   CoreAudio; see whether the Windows NOMINMAX scoping matters). Size S.
2. Port `talkback_sdk_mac.{h,mm}` verbatim per the PORTED.md rule, then resolve its
   threading question against a live meeting. Size M.
3. Write an ObjC++ `engine-comms` equivalent: participants poll (the Windows engine
   polls because the event slot is owned elsewhere), host/co-host detection, MicGuard
   wiring to `ZoomSDKVirtualAudioMicDelegate` / `setExternalAudioSource:` /
   `hasRawDataLicense`, audio mute/unmute, `comms_state` emission, and the reader
   branch. Every SDK call must hop to the main queue (`main-macos.mm:30-42`). Size L.
4. Live qualification as co-host with desktop-client guests. Windows has not done
   this either (`comms-talkback.md`, "Not proven").

### 2.3 Mac shell state (2c)

- `grep -i comms` gives 0 hits.
- `AppModel.handleEvent` (`AppModel.swift:1002-1021`) handles only texture events,
  and `default: break` drops `zoom-comms-state`.
- No Private comms preference exists in `ShellPrefs.swift`.

Needed:

- A `ZoomCommsProtocol` parser in Swift.
- A Comms view with press/release talk keys. SwiftUI must use a zero-distance drag
  or press gesture, never a Button action, because "Click and Command are never
  used".
- A 1500 ms lease renewed every 500 ms (`CommsViewModel.cs:93-94`).
- Release-all on window resign-key, app deactivate, tab change, meeting end and
  shutdown.
- Panel-scoped digit and Space keys.
- Private comms, persisted and OFF by default, re-sent at most once per 2 s when
  the report disagrees.

### 2.4 Rulings the Mac must honour

Quoted from `docs/reference/comms-talkback.md`:

- Owner ruling 2026-11-10, Private comms: "**OFF by default**". "OFF: the
  operator's own mic is left exactly as they set it." "ON: … the operator is muted
  at once, the virtual mic is installed only with `HasRawdataLicense()`, the mic is
  unmuted ONLY after `onMicInitialize`, no initialise within 5 s = stay muted …"
  "ON -> OFF: `setExternalAudioSource(nullptr)` and the operator is left **MUTED**,
  never auto-unmuted."
- "Comms failures never touch subscriptions, ISO, raw media or outputs."
- "Releasing (`on:false`, release-all) is never refused, even with no engine."
- "A cell is never 'on air' unless the engine reports it keyed **and** someone
  confirmed in the channel. Keyed with nobody = armed."
- "Never rebuild a control under the operator's finger." Also: "Digits 1-9 … and
  Space (ALL CALL) are **panel-scoped** … Never global."
- "Intercom-in-Zoom is a product pillar (marketing positioning); keep it native."

---

## 3. Volt rundown media staging (#1201, `8751b8d2`, 2026-11-08)

### 3.1 What Windows does

An external Volt app copies `<key>.mp4` into `<media root>/Volt Rundown/` and then
drives Pro **only through the control API**. The actions are registered at
`Control/ControlActionRegistry.cs:128-135` and dispatched at
`WinUI/Services/StudioControlSurface.cs:357-365`:

- `media.rundown.load {clip}`: refreshes the bin and returns
  `{"sourceId":"media:<id>"}`.
- `media.rundown.preview {clip, scene, route?}`: re-points the one eligible route on
  the Preview scene to `media:<id>`.
- `media.rundown.take {clip, scene, route?}`: runs Take only after
  `CheckVoltRundownPreview` passes.

Supporting code:

- Classification is in plain net9.0 (`MediaCore/Services/MediaBinClassifier.cs`,
  new kind `volt-rundown`, `.mp4` only).
- The rest is WinUI (`StudioViewModel.VoltRundown.cs`, 105 lines). It uses the clip
  key regex `\A[a-z0-9-]{1,72}\z` and checks that the path equals the expected
  file. The decode-readiness check reads `LatestMediaSources`
  (`mediaSources[].onPreview/decodedVideoFrames/onProgram`).
- No core change.

### 3.2 Mac state

- `MediaBin.swift:27` has `kinds = ["stinger", "lower-third", "audio-bed", "slate"]`,
  with no `volt-rundown`.
- There is no control endpoint (§6), so Volt has nothing to call.
- No Swift reader of `mediaSources[]` was found (`grep mediaSources` gives 0 hits).
  It is UNVERIFIED whether Mac media-clip routes reach Preview and Take; that
  belongs to the media audit.
- It is UNVERIFIED whether Volt itself runs on macOS or knows the Mac media root
  (`~/Library/Application Support/CoreVideoPro/media`, `MediaBin.swift:2`). The
  backlog row says "in the **Windows** media bin" (`docs/BACKLOG.md`, #1201).

### 3.3 Class / effort

SHELL-LOGIC, size S *after* #6 exists. On its own it is about 100 lines of Swift
plus one bin kind. It is blocked on §6 and on a Mac `mediaSources[]` read model.

### 3.4 Rulings

These are fail-closed messages built into the action. Port them verbatim:

- "The staged Volt clip must be cued on Preview before Take."
- "Wait for the staged clip to decode on Preview before Take."
- "The staged clip is already on Program. Take it off Program before replaying it."
- "Save or discard the clip scene's draft before cueing from Volt."

---

## 4. Production telemetry, crash upload and OAuth broker (#434/#436)

### 4.1 What reached mac-shell through `75bb8cca`

Only the broker host constant reached the Mac, and it went through two flips:

- `75bb8cca` moved `ZoomOAuth.swift` to `https://auth.corevideo.io`, which does not
  resolve.
- `fd0015e8` (owner ruling 2026-10-25) moved it back.

Today `ZoomOAuth.swift:18-19` = `https://corevideo.iamfatness.us/oauth/start` and the
base. The return URI is `corevideo://oauth/callback`, and the redeem/refresh calls
use `URLSession` (`:76,:106,:124,:141`). Tokens are stored in the Keychain service
`us.iamfatness.corevideopro.zoom-oauth` (`:209-253`), and a shell test pins that
name (`ShellTests.swift:672,679`). **4a is done.**

Not ported: the Windows "pending authorization with no timeout" gap
(`production-endpoints.md` header, `ZoomOAuthService.cs:29`). Whether
`ZoomOAuth.swift` has an analogous dangling pending state is UNVERIFIED.

### 4.2 Telemetry and crash upload (4b)

Windows:

- Consent: `MediaCore/Services/TelemetryConsentStore.cs`. It is opt-in, and
  "Absent file or absent value = OFF — consent is never assumed" (`:19`).
- Heartbeat/events: `TelemetryEventClient.cs`, `POST /v1/events` with
  `Authorization: Bearer` (`:60-63`). It is wired by WinUI `TelemetryEventService`.
- Crash archives: `CrashReportUploader.cs`, `POST /v1/crashes` with Bearer plus
  `X-CoreVideo-Version` / `X-CoreVideo-Machine-Class` (`:117-120`). The default
  endpoint is `https://telemetry.corevideo.io` (`:16`). Dumps come from
  `CrashDumpScanner` / `CrashReportArchiveBuilder`, and the WinUI
  `CrashReportCoordinator` offers the upload on the next launch.
- Upload is effectively **off in production**: "with no `COREVIDEO_TELEMETRY_API_KEY`
  the uploader and heartbeat stay quietly disabled", and `telemetry.corevideo.io`
  does not resolve (`production-endpoints.md`; CLAUDE.md:102-106).

Mac:

- `Diagnostics.swift:20-21`: "There is deliberately no uploader and no telemetry
  here: nothing in this file sends a byte off the machine."
- The support bundle lists `~/Library/Logs/DiagnosticReports` (`:316`) and never
  copies the contents (`:422-425, :469`).

Porting the wire is SHELL-LOGIC, size M. That covers a consent store, two
`URLSession` POSTs, and `.ips` collection in place of WER dumps. It should wait for
(a) the owner checklist (hosts and API key), and (b) an explicit owner decision that
reverses the Mac's "deliberately no uploader" position. Both shells currently ship
no working upload, so this is not a user-visible gap today.

### 4.3 Rulings

- Owner ruling 2026-10-25 (`production-endpoints.md`): "the Zoom OAuth broker stays
  on `https://corevideo.iamfatness.us` … do not move." The Mac complies.
- "The telemetry **API key is a secret and is NOT baked in**."
- Telemetry consent defaults OFF (`TelemetryConsentStore.cs:19`).

---

## 5. Credential stores

**The premise is wrong.** Windows does not use Windows Credential Manager. A grep for
`CredRead|CredWrite|PasswordVault|Windows.Security.Credentials` finds nothing outside
vendored miniaudio. Every Windows secret is a **DPAPI CurrentUser blob written to a
JSON file**, stored as `"dpapi:" + base64` with app entropy `CoreVideoPro.secret.v1`
(`WinUI/Services/DpapiSecretProtector.cs:16-21`). That code is WinUI-only.

| Secret | Windows store | Mac today |
|---|---|---|
| Zoom OAuth tokens | `FileZoomTokenStore` + DPAPI (`StudioViewModel.cs:1596-1599`) | **Keychain** `…zoom-oauth` (`ZoomOAuth.swift:209-253`) |
| RTMP stream key | `ProductionOutputPreferencesStore` field DPAPI (`StudioViewModel.cs:11950-11955`) | **Keychain** `…stream-key`/`rtmp` (`StreamKeychain.swift:9-10`) |
| SRT passphrase | same store | n/a: no SRT output on Mac (`grep srt` finds only a redaction fixture in `Diagnostics.swift:629`) |
| RTMP ingest source secrets | `RtmpIngestSourceStore` (`StudioViewModel.RtmpIngest.cs:48`) | n/a (feature absent) |
| Graphics custom-feed bearer tokens (#1110) | `GraphicsFeedCredentialStore.cs:22,93-94` | n/a (graphics absent) |
| YouTube Data API key | `YouTubeChatCredentialStore.cs` (`graphics/social/youtube-key.json`) | n/a (§1) |
| Twitch client/user/access/refresh | `TwitchChatCredentialStore.cs` (`twitch-credentials.json`, rotated pair committed atomically) | n/a (§1) |

The mapping is DPAPI file → Keychain generic-password item. The two existing Mac
items each carry their own `SecItem*` code; there is no shared helper. Each future
secret is about size S and should land with its feature. A shared
`KeychainItem(service:account:)` helper is worth extracting before the third copy.
Neither existing item sets `kSecAttrAccessible`, so both get the default
(when-unlocked, not synced). That is acceptable but implicit.

Rule to carry over (`DpapiSecretProtector.cs` doc, house rule in
`StreamKeychain.swift:1-3`): never persist a secret in plaintext. Twitch's rotated
access/refresh pair must be committed as a single atomic unit
(`graphics-playout.md`, Twitch).

---

## 6. Companion / OSC / HTTP control surface (`CoreVideoPro.Control`)

### 6.1 What Windows does

All three transports start unconditionally at launch, on loopback
(`WinUI/MainWindow.xaml.cs:139-200`):

- **OSC** over UDP `127.0.0.1:8010` (`COREVIDEO_OSC_PORT`).
  `Control/Osc/OscControlServer.cs` (186), `OscCodec.cs` (272), `OscAddressMap`,
  `OscFeedback`.
- **HTTP + WebSocket** on `127.0.0.1:8011` (`COREVIDEO_HTTP_PORT`).
  `Control/Http/HttpControlServer.cs` (336) over `HttpListener`, and
  `HttpControlRouter.cs` (258). Routes: `GET /manifest`, `GET /state`,
  `GET /snapshot` (the core's verbatim sessionState, redacted, with an
  `available/ageMs/stale` envelope), `POST /invoke {action, args}`, and `/ws`
  feedback.
- **Action catalog.** 111 actions in `ControlActionRegistry.cs` (335) with a
  manifest (`ControlManifest`, `ControlCatalog`, `ControlState`).
- **Consumers.** `companion-module-corevideopro/` (default `127.0.0.1:8011`,
  `src/config.ts:18,33`), Stream Deck/OSC, Volt (§3), and every QA live-check
  script (CLAUDE.md "Observing a RUNNING core").

Portability: `CoreVideoPro.Control` is plain net9.0 (`CoreVideoPro.Control.csproj:3`),
but its `IControlSurface` implementation is `WinUI/Services/StudioControlSurface.cs`
(1,388 lines, bound to `StudioViewModel`). That binding is the real cost. The
transports are small; the 111 action bodies are not.

### 6.2 Mac state

The Mac exposes **no control endpoint**. Every one of these greps over mac-shell
gives 0 hits: `NWListener`, `\bosc\b`, `companion`, `websocket`, `httpserver`,
`8011`, `8010`. The Mac shell's only network client is `URLSession` in
`ZoomOAuth.swift`. As a result, Companion, Stream Deck, Volt and every
`/snapshot`-based QA judge cannot target a Mac.

### 6.3 Class / effort

SHELL-LOGIC, size XL for parity (111 actions plus WebSocket feedback plus
`/snapshot` redaction). Size L for a useful core subset: `/manifest`, `/state`,
`/snapshot` and the transport, scene and Take actions.

Suggested shape:

- Generate the manifest from `ControlActionRegistry` into a JSON contract, matching
  how `Lifecycle.generated.swift` is produced, so the action IDs cannot drift.
- Build HTTP/WS and UDP on Network.framework.
- Dispatch onto `AppModel` on the main actor.
- Port the redaction rules from CLAUDE.md (walk the JSON; suffix-match secret
  names) rather than re-deriving them.

### 6.4 Rulings the Mac must honour

- Loopback by default. "a LAN bind refuses to start without a token"
  (`HttpControlRouter.cs:23-26`, enforced at `HttpControlServer.cs:33`).
- OSC LAN needs **both** `COREVIDEO_OSC_LAN=1` and
  `COREVIDEO_OSC_TRUSTED_NETWORK=1`. Otherwise it "remains on loopback"
  (`MainWindow.xaml.cs:148-154`).
- `/snapshot` "never triggers a core round-trip" and answers `available:false` with
  a `reasonCode` rather than anything that reads as current (CLAUDE.md).
- The social rule in §1.5: the control API must never resolve a `social-review-*`
  target.
- Test-interaction preference (CLAUDE.md, 2026-09-07): "Use the local control API
  and headless test processes …". Today that preference cannot be met on a Mac.

---

## Method notes / unverified

- Nothing was built or run. The Mac SDK header facts come from
  `~/Developer/zoom-sdk-macos/ZoomSDK.framework/Versions/A/Headers`.
- **UNVERIFIED:**
  - macOS CI actually executing the `Comms*` and `MicGuard*` tests (they are in the
    cross-platform test sources, but I did not read the macOS CI job's filter).
  - The threading of `sendAudioDataToChannel:` on macOS (upstream says unresolved).
  - Whether Volt runs on macOS.
  - Whether Mac media-clip routes and `mediaSources[]` support the Volt Take check.
  - Whether `ZoomOAuth.swift` has a dangling pending-auth state like Windows.
