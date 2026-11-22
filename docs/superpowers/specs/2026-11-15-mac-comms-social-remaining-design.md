# macOS parity: Comms (talkback), social review, and the remaining parity items

Status: DRAFT design for owner review, 2026-11-15.

Covers roadmap milestones M6 (social review), M7 (Comms) and M8 (remaining parity) of
`docs/superpowers/plans/2026-11-14-mac-parity-1.0-roadmap.md`, gap row 2.14 of
`docs/superpowers/audits/2026-11-15-mac-ui-parity-consolidated.md`, and every "missing",
"different" or "partial" row of `2026-11-15-mac-ui-parity-settings-automation-health.md` that
no other Mac-parity spec owns.

This is three sub-designs in one file. Part A (Comms), Part B (Social) and Part C (Remaining)
each carry their own Windows behaviour, Mac state, design and slices, and can be scheduled
separately. Sections 5 to 8 (acceptance, risks, owner decisions, out of scope) are shared.

All code claims below were re-checked in worktree `mac-tier0b` @ `32231cbc`. The audits were
written against older trees, so several of their line numbers and a few of their findings have
moved; section 9 lists what changed. Anything I could not confirm in code is marked UNVERIFIED.

## 1. Why

- **Comms.** A Windows operator cues talent over Zoom talkback from the Comms page. On a Mac
  there is no Comms tab at all (`mac-shell/.../AppModel.swift:11-26` has no `comms` case and the
  comment says "Comms and OHG Show are not on Mac in 1.0"). Intercom-in-Zoom is a product
  pillar (`docs/reference/comms-talkback.md:9`). The A1 or producer running a show from a Mac
  notices the first time they need to talk to a guest without the room hearing it.
- **Social.** A Windows operator can review YouTube and Twitch chat, approve one message and put
  it on Program as a card. A Mac operator has nothing: no review queue, no provider connection,
  no card. Anyone producing an audience-interactive show notices.
- **Remaining.** The Sources slot rows on Mac do not show negotiated format, frame age, the
  configured cap or a REC chip, so the operator cannot see that a guest arrived at 360p or that
  an ISO is not recording. Telemetry and crash upload do not exist on Mac, so the owner gets no
  crash signal from Mac testers once the endpoints go live. And about thirty small rows from the
  settings, automation and health audit have no owner yet.

---

# Part A. Comms (roadmap M7)

## A1. Windows behaviour to match

Reference: `docs/reference/comms-talkback.md` (158 lines). Rulings are quoted from it.

| # | Behaviour or ruling | Where | Windows proof |
|---|---|---|---|
| A1.1 | The talkback station is the Zoom engine process itself (one participant per process; talkback needs host or co-host). | `native/zoom-engine/engine/engine-comms.h:1-30` | n/a (architecture) |
| A1.2 | Ten shell verbs: `comms-key`, `comms-latch`, `comms-assign`, `comms-mic`, `comms-sidetone-out`, `comms-sidetone`, `comms-tone`, `comms-gain`, `comms-release-all`, `comms-private`. | `native/src/modules/ZoomCommsRelay.cpp:75-77` (validation `:86-170`) | `native/tests/CommsRelayTest.cpp` |
| A1.3 | `comms-*` requests bypass `coreMutex`; four are urgent so a key-up is never refused for capacity. | `native/src/rpc/JsonRpcServer.cpp:122,158,1264`; `native/src/rpc/CommandMailbox.h:41-42` | `ZoomEngineRuntime.Comms*` |
| A1.4 | "Releasing (`on:false`, release-all) is never refused, even with no engine." | `native/src/modules/ZoomEngineRuntime.cpp:764-773` | `ZoomEngineRuntime.Comms*` |
| A1.5 | State arrives as an unsolicited `zoom-comms-state` fact, latest-wins, repeated about once per second while held; `"comms":null` means the report was dropped. | `ZoomEngineRuntime.cpp:738-745` | same |
| A1.6 | A held key is leased: `leaseMs` 1500, renewed every 500 ms. A hung shell drops its keys in 1.5 s. | `native-shell/CoreVideoPro.WinUI/ViewModels/Comms/CommsViewModel.cs:93-94`; timer `StudioViewModel.Comms.cs:36-43` | `CommsViewModelTests` |
| A1.7 | Every held key goes up on tab change, window deactivate, meeting end, dropped report and shutdown. | `StudioViewModel.Comms.cs:59,70-76`; `MainWindow.xaml.cs:675-676`; `CommsViewModel.cs:318,326` | `CommsViewModelTests` |
| A1.8 | Talk keys are press and release with pointer capture. "Click and Command are never used." | `native-shell/CoreVideoPro.WinUI/Controls/TalkKey.cs:37-71` | `--verify-comms-page` probe |
| A1.9 | "Digits 1-9 ... and Space (ALL CALL) are panel-scoped ... Never global." No modifier, no auto-repeat, not in a text field. | `Views/CommsPage.xaml.cs:96-106` | `CommsViewModelTests` |
| A1.10 | "A cell is never 'on air' unless the engine reports it keyed and someone confirmed in the channel. Keyed with nobody = armed." | `CommsViewModel.cs:169-170,403` | `CommsViewModelTests` |
| A1.11 | "Never rebuild a control under the operator's finger." Collections sync in place and only on a structure-signature change. | `comms-talkback.md` "UI rules" | `CommsViewModelTests` |
| A1.12 | Private comms (owner ruling 2026-11-10): persisted, "OFF by default". ON: operator muted at once, silent virtual mic installed only with a raw-data licence, unmuted only after `onMicInitialize`, no initialise in 5 s means stay muted. ON to OFF: virtual mic removed and operator "left MUTED, never auto-unmuted". | `engine-comms.cpp:342-353,374-375,473-500`; `comms/mic_guard.{h,cpp}` | `native/tests/MicGuardTest.cpp` |
| A1.13 | The shell re-sends the Private preference when a report disagrees, at most once per 2 s. | `CommsViewModel.cs:102,269-273` | `CommsViewModelTests` |
| A1.14 | "Comms failures never touch subscriptions, ISO, raw media or outputs." | `engine-comms.h:26-29` | n/a |
| A1.15 | Explicit page states: not in a meeting; waiting for the engine's report; not up (verbatim reason plus the host or co-host hint); desk. | `CommsViewModel.cs:133,348` | page probe |

Not proven on Windows either (`comms-talkback.md` "Not proven"): real talkback against Zoom,
audio at the guest. The Mac live acceptance (slice A5) is therefore the first live proof on
either platform.

## A2. Mac today

**Shared core: done, and compiled on macOS.** `ZoomCommsRelay.cpp` is in the shared library
(`native/CMakeLists.txt:185`). The relay, the lock-free dispatch, the urgent mailbox and the
state fact are platform-neutral code. `corevideo_comms_core` (channel ladder, eligibility, duck
planner, pacer, `comms_core`, `mic_guard`) builds on every platform (`CMakeLists.txt:705-725`),
and the fake engine links it and answers `comms_*` lines on macOS too
(`CMakeLists.txt:727-732`, `zoom-engine/fake/fake-comms.cpp`). The Comms tests are in the
cross-platform test target (`CMakeLists.txt:1039,1045-1052`). UNVERIFIED: that the macOS CI job
actually runs them.

**Mac Zoom engine: nothing.**
- `main-macos.mm` (2,435 lines) has zero `comms` or `talkback` hits.
- The reader loop now dispatches by parsed command and logs unknown commands once
  (`main-macos.mm:2331-2403`, `shared/engine-command.h:10-33`). Roadmap M0.2 is therefore already
  done on Mac. A `comms_*` line today lands in the unknown branch and is logged, no
  `comms_state` comes back, and the core holds `commsState_` empty. That fails closed.
- The Apple engine target links only ZoomSDK, Foundation and AppKit (`CMakeLists.txt:770-800`).
  `corevideo_comms_audio` (miniaudio mic chain) is defined only in the non-Apple branch
  (`CMakeLists.txt:815-823`).
- The engine bundle already has the microphone usage string and the `audio-input` entitlement
  (`scripts/make-macos-engine-bundle.sh:82`, `scripts/macos-engine.entitlements`).

**PORTED.md, verified.** `native/zoom-engine/engine/comms/PORTED.md` names ZComms
`origin/main @ 168a572d` and lists only `talkback_sdk.{h,cpp}` and `talkback_sdk_win.{h,cpp}`
as the SDK seam (`PORTED.md:12`). There is no `talkback_sdk_mac.*` in the Pro tree. The local
ZComms clone (`~/Developer/ZComms`, branch `macos-port-p1`) does contain
`src/zoom/talkback_sdk_mac.h` (68 lines) and `.mm` (247 lines); that clone does not contain
commit `168a572d`, so the port must fetch upstream first. The shared header already describes
both adapters (`comms/talkback_sdk.h:8-12`). The Mac adapter header states "THREADING IS
UNRESOLVED": whether `sendAudioDataToChannel:` may be called off the main queue is not known.
Also listed as not ported: `zoom_client*`, `mic_source_win`/`virtual_mic`, `roster`.

**SDK 7.1.5 API check, verified against the headers** in
`~/Developer/zoom-sdk-macos/ZoomSDK.framework` (Info.plist says 7.1.5 (84750)). Every SDK call
made by `engine-comms.cpp` and `talkback_sdk_win.cpp` has a Mac equivalent:

| Windows call | macOS 7.1.5 equivalent (header:line) |
|---|---|
| `GetMeetingTalkbackController` | `-[ZoomSDKMeetingService getTalkbackController]` (`ZoomSDKMeetingService.h:640`) |
| `IsMeetingSupportTalkBack` | `isMeetingSupportTalkBack` (`ZoomSDKTalkbackController.h:110`) |
| `CreateChannel(count)` | `createChannel:` (`:118`) |
| `BeginBatchInviteUsers` / `AddUserToInvite` / `ExecuteBatchInviteUsers` | `inviteUsersToChannel:userIDList:` (`:148`), one atomic call |
| `BeginBatchRemoveUsers` / ... | `removeUsersFromChannel:userIDList:` (`:157`) |
| `BeginBatchDestroyChannels` / ... | `destroyChannels:` (`:139`) |
| `SendAudioDataToChannel` | `sendAudioDataToChannel:audioData:dataLength:sampleRate:channel:` (`:169`) |
| `SetChannelBackgroundVolume` | `setChannelBackgroundVolume:backgroundVolume:` (`:177`) |
| `SetEvent` + six callbacks | `ZoomSDKTalkbackControllerDelegate` (`:34-91`), same six plus `onInviterAudioLevel` |
| `SDKERR_TOO_FREQUENT_CALL` | `ZoomSDKError_TooFrequentCall` (`ZoomSDKErrors.h:241`) |
| Talkback event codes | `ZoomSDKTalkbackError_*` (`ZoomSDKErrors.h:2747-2761`), same eight values |
| `GetParticipantsList`, `GetUserByUserID`, `GetMySelfUser` | `getParticipantsList`, `getUserByUserID:`, `getMyself` (`ZoomSDKMeetingActionController.h:924,939,945`) |
| `IsHost`, `GetUserRole`, `GetUserName`, `GetUserID` | `ZoomSDKUserInfo` `isHost`, `getUserRole`, `getUserName`, `getUserID` (`:333,354`) |
| `IUserInfo::IsSupportTalkback` | `-[ZoomSDKUserInfo isSupportTalkback]` (`:532`) |
| `IsAudioMuted` | `getAudioStatus` folded onto a boolean, as the engine already does (`main-macos.mm:509-514`) |
| `GetAudioJoinType` | `getAudioType` (`:349`; `ZoomSDKAudioType_None` = 0) |
| `MuteAudio(self)` / `UnMuteAudio(self)` | `actionMeetingWithCmd:ActionMeetingCmd_MuteAudio / _UnMuteAudio userID:onScreen:` (`:932`; `ZoomSDKErrors.h:61,63`) |
| `HasRawdataLicense` | `hasRawDataLicense` (`ZoomSDKRawDataController.h:153`) |
| `GetAudioRawdataHelper()->setExternalAudioSource` | `getRawDataAudioSourceHelper:` then `setExternalAudioSource:` (`ZoomSDKRawDataController.h:180`; `ZoomSDKRawDataAudioSourceController.h:65`) |
| `onMicInitialize` / `onMicUninitialized` | `ZoomSDKVirtualAudioMicDelegate` (`ZoomSDKRawDataAudioSourceController.h:38,50`) |

So the roadmap's claim holds: the SDK is not the gap. Two semantic differences need care:
mute is a meeting action command rather than a per-user audio-controller call, and mute status
is an enum, not a boolean.

**Mac shell: nothing.** No Swift file other than the tab comment mentions comms.
`AppModel.handleEvent` handles only texture events and drops everything else at `default: break`
(`AppModel.swift:1898-1919`). `ShellPrefs` (`ShellPrefs.swift:105`) has no Private comms field.

## A3. Design

### Engine (lane: native)

1. **Port the adapter verbatim.** Add `comms/talkback_sdk_mac.{h,mm}` from ZComms upstream at
   the commit PORTED.md names (or re-pin PORTED.md to a newer commit and re-copy every listed
   file; do not hand-diverge). Update PORTED.md's "Verbatim" table.
2. **Build the comms libraries into the Apple engine.** Link `corevideo_comms_core`. Move the
   `corevideo_comms_audio` definition out of the non-Apple branch so it builds on both, with
   the same PUBLIC `MA_NO_*` definitions; on Apple link CoreAudio, AudioToolbox, AudioUnit and
   CoreFoundation. UNVERIFIED: that miniaudio v0.11.21 compiles clean as C++ in this target
   with those flags; slice A1 proves it.
3. **New file `native/zoom-engine/engine/engine-comms-macos.mm`** (new), the ObjC++ twin of
   `engine-comms.cpp`. Same public surface as `EngineComms` (`start`, `handle_line`,
   `owns_mic`, `set_capture_only`, `on_meeting_status`, `shutdown`), same policy objects
   (`comms_core`, `mic_guard`), only the Zoom glue differs:
   - a 100 ms `dispatch_source` timer on the main queue replaces the Windows thread timer;
   - the roster is polled each tick through `getParticipantsList`, as on Windows (PORTED.md
     "roster: polled");
   - the silent virtual mic is an ObjC object adopting `ZoomSDKVirtualAudioMicDelegate` that
     never sends; `mic_guard` decides install, ownership, unmute-after-initialise, the 5 s
     give-up and the revoke, unchanged.
4. **Dispatch.** In the reader loop (`main-macos.mm:2334`), test
   `edgecomms::IsCommsCommand(line)` (`comms/comms_protocol.h:171-173`) before
   `engine_parse_command`, and hop the line to the main queue. This keeps free-text device
   names away from every other branch. Call `on_meeting_status` first in the meeting-status
   delegate, `set_capture_only` from the join line (same `"capture_only":true` flag Windows
   reads at `main.cpp:1536`; Pro never sends it, so Private comms governs the mic), and
   `shutdown` before SDK cleanup.
5. **Threading rule.** Every SDK call runs on the main queue (`main-macos.mm:30-42`). The mic
   chain keeps its own worker thread. The one open point is the 20 ms TX pacer calling
   `SendAudio` off-main. Design: a small Pro-owned decorator `MainQueueTalkbackSdk` (new,
   implements `zc::TalkbackSdk`, lives beside the glue, not in the ported files) that forwards
   every call unchanged except `SendAudio`, which it either calls directly or copies one frame
   and `dispatch_async`es to the main queue. Which mode ships is decided by the spike in slice
   A2, by measurement (`stats.underruns`, `stats.sendErrors`, and audio at a guest), not by
   reading headers. The decorator keeps the ported adapter verbatim either way.
6. **Isolation.** No call from the comms glue reaches the video, share or audio-subscription
   code, and every tick is exception-fenced (`@try` plus C++ `try`), matching A1.14.
7. **Wire.** No new commands or events. The engine accepts the existing `comms_*` lines and
   emits the existing `comms_state` line produced by `comms_core`.

### Shell (lane: shell)

New files, all pure logic separated from views so they run under `COREVIDEO_SHELL_TESTS`:

- `ZoomCommsProtocol.swift` (new): decode `zoom-comms-state` into value types mirroring
  `ZoomCommsModels.cs` (`available`, `reason`, `lamps`, `room`, `channels[16]`, `roster[]`,
  `mic`, `stats`, `log[]`, `private`). `comms: null` decodes to "no report".
- `CommsModel.swift` (new): the `CommsViewModel` rules. Held set, lease renewal (1500 ms lease,
  500 ms tick), release-all-local, the Private re-send throttle (2000 ms), the structure
  signature that gates collection rebuilds, tone per cell (on air, armed, ready, idle,
  unreachable), and the four page states. Takes an injected clock and an injected send closure
  so tests need no core.
- `CommsPage.swift` (new): the grid, ALL CALL, LATCH, EDIT TALENT, SETTINGS, LEAVE, the four
  lamps and the ops strip. Colours come from existing `Studio` design tokens only.
- `TalkKey`: an `NSViewRepresentable` whose NSView overrides `mouseDown` and `mouseUp` (AppKit
  delivers the up to the view that took the down, which is the pointer-capture equivalent) and
  also releases on `viewDidMoveToWindow(nil)` and window resign-key. Never a SwiftUI `Button`.

Wiring into existing code:

- `AppModel.handleEvent` gains a `zoom-comms-state` case that forwards to `CommsModel`.
- `StudioTab` gains `comms`, placed between Audio and Media to match Windows
  (`StudioWorkspace.xaml:174-242`).
- Commands go through the existing `MediaCoreBridge.request` (`MediaCoreBridge.swift:311`) as
  `{"type": <verb>, "payload": {...}}`. A `comms-refused` failure's message is shown verbatim
  in the ops strip.
- Release-all fires on: `selectedTab` leaving `.comms`, `NSApplication.didResignActive`, main
  window resign-key, meeting state leaving `in_meeting`, a null report, and the quit path (send
  `comms-release-all` before the core is stopped; the existing quit guard is the hook).
- Keys: a local `NSEvent` monitor, installed only while the Comms tab is selected and the main
  window is key, following the `TransportKeys.swift` pattern. Digits 1 to 9 and Space; ignored
  with any modifier, on auto-repeat, and when a text field is first responder. Space = Take is
  Studio-tab-only today (`AppModel.swift:3165-3168`), so there is no conflict; a test pins it.
- Persistence: `ShellPrefs.commsPrivate` (new, optional Bool, absent = false).
- Core restart: nothing to re-arm except Private, which the report-disagrees rule already
  re-sends. This satisfies the "one-shot command on every core generation" rule by design.

Degraded states the operator sees: "Join a meeting to use Comms"; "Waiting for the engine's
report" (also what a Mac engine without comms would show forever, so slice A3 must not ship
ahead of A2 except behind the fake engine); the engine's verbatim reason with the host or
co-host hint; a NO TALKBACK tag on web-client guests; "Talkback is silent: ..." when the
virtual mic did not initialise.

## A4. Slices (Comms)

| ID | What lands | Lane | Size | Depends on | Proof |
|---|---|---|---|---|---|
| A1 | Comms libraries build and link in the Apple engine; `talkback_sdk_mac.{h,mm}` ported; PORTED.md updated; the reader routes `comms_*` to a glue that answers `comms_state` with `available:false` and a reason ("not host", "not in a meeting"). Real consumer: the core's existing relay and `scripts/probe-comms.mjs`. | native | M | none (M0.2 already done) | Engine builds signed; `probe-comms.mjs` against the real Mac engine outside a meeting reads a `zoom-comms-state` with a reason; device-name line containing "join" does not join (unit test on the dispatch order). |
| A2 | Full glue: roster poll, host and co-host detection, channel bank, keys, latch, assign, tone, gain, device pickers, MicGuard and Private comms. Includes the `SendAudio` threading spike and the decorator mode chosen from it. | native | L | A1 | Native unit tests for the glue's pure parts; live spike in a co-host meeting: tone keyed to one guest is heard by that guest only; `stats.sendErrors` and `underruns` stay flat for 10 minutes while 8 video feeds run. |
| A3 | Shell protocol parser and `CommsModel` with tests ported from `ZoomCommsProtocolTests` and `CommsViewModelTests`; `handleEvent` stops dropping the fact. Consumer: a minimal Comms tab (states and lamps only). | shell | M | none (develops against the fake engine via `COREVIDEO_ZOOM_ENGINE_PATH`) | `COREVIDEO_SHELL_TESTS`; each test fails first against the old drop. |
| A4 | Full Comms page: talk keys, ALL CALL, latch, edit talent, settings, leases, release rules, panel-scoped keys, Private comms pref. | shell | L | A3 | Shell tests for every release trigger and the lease cadence; `COREVIDEO_SHELL_SNAPSHOT` images of the four states plus on-air and armed cells; fake-engine end-to-end run. |
| A5 | Live acceptance in a co-host meeting, owner-run. | owner | S | A2, A4 | Checklist in section 5. |

A3 and A1 can run in parallel in different lanes. Calendar: about 2.5 weeks native, 2 weeks
shell.

---

# Part B. Social review (roadmap M6)

## B1. Windows behaviour to match

Operator flow: `docs/reference/graphics-playout.md:209-227` (YouTube) and `:808-860` (Twitch).
Rulings: `docs/superpowers/specs/2026-11-04-graphics-phase3-slice6-social-feeds.md`.

| # | Behaviour or ruling | Where | Windows proof |
|---|---|---|---|
| B1.1 | Exact-version review queue: generation + provider id + revision; 200 messages; tombstones; expiry. "Approval and rejection require the exact revision displayed in review." | `native-shell/CoreVideoPro.MediaCore/Services/GraphicsSocialReview.cs:7,34,79-177` | `GraphicsSocialReviewTests` |
| B1.2 | "Approval alone sends no native command and leaves Program unchanged." Show is a separate action. | `GraphicsSocialPlayout.cs:70-73` | `GraphicsSocialPlayoutTests` |
| B1.3 | One serialized playout owner; the approval is checked before native preparation and again immediately before submission; "do not show a newer revision using an older approval". | `GraphicsSocialPlayout.cs:73-150` | `GraphicsSocialPlayoutTests` |
| B1.4 | Wire identity: `instanceId` prefix `social-review-`, `clientId` `graphics-social-review`, cue with `rideTake:false` and no `cueOwner`, Show targets `cue`. | `GraphicsSocialPlayout.cs:9-11,215` | same |
| B1.5 | **Shared-core gate.** The core refuses any `social-review-*` request whose client is not `graphics-social-review` (`social-graphics-require-operator-review`), any cue that rides Take or has an owner (`social-graphics-private-cue-only`), and any Show not targeting the cue (`social-graphics-require-prepared-cue`). Hide and Clear are always allowed. | `native/src/graphics/GraphicsInstances.cpp:20,508-518` | `native/tests/GraphicsInstancesTest.cpp:74` |
| B1.6 | A created-but-hidden social instance expires after 10 s; the on-air card ends after 15 s "even if the console detaches"; never drawn on Preview. | `GraphicsInstances.cpp:561` (10 s); 15 s and Preview exclusion per audit at `:1161`, `:852` (UNVERIFIED line numbers) | `GraphicsInstancesTest` |
| B1.7 | "Show the item as on air only when confirmed by the native snapshot." | `GraphicsSocialPlayout.cs:44` | `GraphicsSocialPlayoutTests` |
| B1.8 | Bounds: message 192 UTF-8 bytes, author 80; oversize or undisplayable items are rejected, never truncated. | `GraphicsSocialReview.cs:79` | `GraphicsSocialReviewTests` |
| B1.9 | "No chat payload, author, API key or cursor goes into show files, telemetry, crash annotations or logs." | slice-6 spec `:15` | review |
| B1.10 | YouTube: Data API key in the `x-goog-api-key` header, never in the URI; redirects rejected; 15 s minimum poll; not via the Zoom broker. | `YouTubeChatTransport.cs:27,119`; `YouTubeSocialFeed.cs` | `YouTubeChatTransportTests` |
| B1.11 | Twitch: device-code grant with scope `user:read:chat` only; hourly `/validate`; EventSub WebSocket with four subscriptions, ordering fences, three reconnects, 64 KiB frame cap. "Every connection gap clears approvals and retires its card." "Budget exhaustion clears eligibility instead of forgetting a removal." | `TwitchAuthorization.cs`, `TwitchHttpTransport.cs:19`, `TwitchChatSocket.cs`, `TwitchEventSubSession.cs`, `TwitchSocialFeed.cs` | `TwitchAuthorizationTests`, `TwitchChatSocketTests`, `TwitchEventSubSessionTests`, `TwitchSocialFeedTests` |
| B1.12 | "Ordinary Take, Show-from-cue, hotkeys and the control API cannot use a social item." | slice-6 spec `:17` | `GraphicsInstancesTest.cpp:74` |
| B1.13 | Secrets at rest are never plaintext; the Twitch access and refresh pair rotates as one atomic unit. Windows uses DPAPI JSON files, not Credential Manager. | `WinUI/Services/YouTubeChatCredentialStore.cs`, `TwitchChatCredentialStore.cs`, `DpapiSecretProtector.cs:16-21` | `*CredentialStoreTests` |

## B2. Mac today

- **Shared core gate: present and tested on macOS.** `GraphicsInstances.cpp` is shared code and
  `GraphicsInstancesTest.cpp` is in the cross-platform suite. This is the safety half: a Swift
  port that gets the client id, the cue flags or the Show target wrong is refused by the core
  and nothing airs. A wrong port fails closed.
- **No pixels on Metal.** `MediaCore.cpp:1272-1273` refuses every `graphics-*` command with
  `graphics-instance-backend-not-built` unless the compositor supports instances; the default
  is false (`Interfaces.h:1488`) and only D3D11 returns true
  (`D3D11CompositorAdapter.cpp:268`). A social card is a native graphics instance using the
  `youtube-chat-card` / `twitch-chat-card` templates (`contracts/graphics-social/`). So no
  social card can reach a Mac Program until the Metal graphics backend spec lands.
- **Mac shell: nothing.** No `social`, `twitch` or chat code. The Mac sends no
  `graphics-instance-*` command. Two Keychain items exist with their own copies of the
  `SecItem*` code (`StreamKeychain.swift:9-44`, `ZoomOAuth.swift:274-315`); there is no shared
  helper. The only `URLSession` user in the app is `ZoomOAuth.swift`.
- **Mac control API** (`ControlApi.swift:62-91`) has no graphics actions yet, so rule B1.12
  holds today by absence. It must be kept true when the remote-control spec adds
  `graphics.*`.

## B3. Design

All shell lane. About 1,300 lines of plain .NET become Swift; the C# tests are the oracle.

- `KeychainItem.swift` (new): one generic-password helper, `KeychainItem(service:account:)`
  with `read() -> Data?`, `write(Data) -> Bool`, `delete()`. Explicit
  `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly` (new behaviour: today's two items rely on
  the default). `StreamKeychain` and the Zoom token store become thin callers with their
  service names unchanged (`ShellTests` pins the Zoom service name). A value is one blob, so
  the Twitch credential set (client id, user id, access, refresh) is stored as one JSON blob in
  one item, which makes the rotated pair atomic by construction.
- `SocialReview.swift` (new): the queue of B1.1 and B1.8 as a pure value-type state machine
  with an injected clock.
- `SocialPlayout.swift` (new): an actor (the "one serialized owner"). `show(key)` checks the
  approval, sends `graphics-instance-create` then `graphics-instance-cue` (`rideTake:false`),
  waits for the prepare fact, re-checks the approval, then sends `graphics-instance-trigger`
  (`action: show`, `target: cue`), all with `clientId: graphics-social-review`. `hide()` sends
  the always-allowed Hide. On-air state is read back from the snapshot's graphics instance
  list, never assumed. It uses the graphics command gateway and snapshot decode that the
  graphics shell spec (roadmap M4.1) provides; it does not define its own.
- `YouTubeChat.swift` and `TwitchChat.swift` (new): transports on `URLSession` (ephemeral
  configuration, redirects refused through the task delegate) and `URLSessionWebSocketTask`.
  Both feed `SocialReview` through the same receive and withdraw calls as Windows. Transport
  logic takes an injected HTTP and socket seam so the C# test cases port as table tests.
- `SocialReviewSheet.swift` (new): the dialog. Provider connect forms, the message list with
  full author and text, Approve, Reject, Show approved, Withdraw, Hide social graphic, Clear
  local data. Entry point: Graphics tab, Operate, "Social review", as on Windows.
- Threading: all network and parsing off the main actor; the review model publishes to the main
  actor at most a few times per second (chat can burst). No core round-trip on receive.
- Persistence: only credentials (Keychain) and the non-secret connection fields (video id,
  channel, client id) in `ShellPrefs`. No message content is persisted, logged, or added to the
  support bundle; the bundle redactor gets a test with a fake chat payload.
- Degraded states: "Graphics are not available on this build" when the core reports the
  backend-not-built failure (the sheet stays usable for review but Show is disabled with that
  reason); provider errors verbatim; "Connection gap: approvals cleared" on any Twitch
  reconnect; a core refusal code shown as-is.

## B4. Slices (Social)

| ID | What lands | Lane | Size | Depends on | Proof |
|---|---|---|---|---|---|
| B1 | `KeychainItem` helper; the two existing items move onto it. Consumers: stream key and Zoom tokens. | shell | S | none (land before the graphics shell spec's remote-feed credentials) | Shell tests: round trip, update, delete, service names unchanged; an existing saved stream key still reads after upgrade (manual check once). |
| B2 | Review model + gated playout + review sheet with a manual "paste a test message" source, on Program through the real core. | shell | L | Metal graphics backend spec (M3.2); graphics shell spec gateway (M4.1) and Operate surface (M4.2) | Ported `GraphicsSocialReviewTests` and `GraphicsSocialPlayoutTests`; a fail-closed test that sends a wrong `clientId` to a real core and asserts the refusal code and an unchanged Program readback; Metal readback of the card on Program and its absence on Preview; card gone after 15 s. |
| B3 | YouTube live chat transport and connect form. | shell | M | B1, B2 | Ported `YouTubeChatTransportTests`; owner live check against a real stream. |
| B4 | Twitch device authorization, EventSub session, ordered moderation, fail-closed recovery. | shell | L | B1, B2 | Ported Twitch test suites; owner live check on a real channel including a moderator delete of an approved message. |

B2's pure model and tests can be written before the graphics backend exists; the slice only
ships when a card can air. Calendar: about 4 weeks of shell lane after its dependencies.

---

# Part C. Remaining parity (roadmap M8 and the leftover sweep)

## C1. Multiview input inspector and the versioned ten-row projection (#674, M8.1)

**Windows.** `ViewModels/ShowInputs/MultiviewInputRowsProjection.cs` (490 lines),
`MultiviewInputRow.cs`, `MultiviewTileCropPolicy.cs`. Ten stable rows. Rules:
- a roster fact is accepted only with `rosterRevision > 0`, a non-empty `rosterEpoch`, and
  `ZoomRosterSnapshotPolicy.Accept` (`MediaCore/Services/ZoomRosterSnapshotPolicy.cs`); an
  epoch change resets frames; a retired epoch is ignored (`ApplyRosterFact`, `:78`);
- each row shows status IDLE / LIVE / TALKING / STALLED / PGM, negotiated `WxH@fps` from the
  core's `zoom-source-format` fact (`ApplySourceFormatFact`, `:106`), and frame age when stale;
- a configured-cap note. Rulings: "a request ceiling is never an FPS measurement"; the cap
  shown is "the ceiling accepted at this meeting's join, never a pending preference"
  (`SetAppliedCameraCap`, `:130`);
- a REC chip derived from writer observations, "never from an ISO selection checkbox"
  (`MediaCore/Services/IsoOutputLifecyclePolicy.cs:5-28`): error, armed, recording, off;
- a per-row preview that is a crop of the one multiview texture (`MultiviewTileCropPolicy`).
Tests that reference these types live in `WinUI.Tests/ShowInputsCoordinatorTests.cs` and
`MediaCore.Tests/NativeMediaCoreStateMapperTests.cs`.

**Mac today.** The shared core emits `zoom-source-format` on macOS
(`ZoomEngineRuntime.cpp:1952-1965`; payload `participantId`, `rosterEpoch`,
`sourceGeneration`, `width`, `height`, `fps`, `frameId`, `frameAtMs`). The shell decodes
`rosterEpoch` / `rosterRevision` (`Lifecycle.generated.swift:249-257`,
`ZoomRosterProjection.swift:21-35`) but `handleEvent` drops the format fact, and `SlotRow`
(`ShellApp.swift:1713`) shows none of the chips. The multiview IOSurface and per-tile rects
are already in the shell (`AppModel.swift:1906-1936`).

**Design.** `InputRowsProjection.swift` (new, pure): the same accept and reset rules, fed by
the snapshot roster, the `zoom-source-format` event (new `handleEvent` case), the recording
node (REC chip, reusing `RecordingLifecycleReadModel`), and the multiview tile list (crop
rect). Row previews are a second layer over the existing multiview IOSurface with a
`contentsRect` crop: no new texture, no new core export, no per-row surface. Updates are
in place on a stable ten-element array. The Sources rebuild spec owns the Sources table
layout; this projection is the model it binds its format, fps and REC chips to. Agree the type
name with that spec before either starts.

## C2. Zoom subscription evidence events (#1171, M8.2)

**Finding: the Mac engine already emits them.** The core retains five debug stages
(`native/src/modules/ZoomSubscriptionSdkEvidence.h:17-20`: `set_resolution`,
`video_subscribe`, `video_resolution_downgraded`, `video_subscribe_failed_all`,
`create_renderer_failed`) with fields `code`, `resolution`, `requested`, `actual`, and
publishes them as `sdkSubscriptionEvidence` (`ZoomEngineRuntime.cpp:1427`). `main-macos.mm`
writes all five with the same field names (`:1295-1351`), matching `engine-video.cpp:69-115`,
and has since August. What is missing is proof and visibility, not engine work:
- no test pins the Mac strings to the parser, so a rename on either side would go unnoticed;
- `sdkCode` is a `ZoomSDKError` on Mac and an `SDKError` on Windows; the numbers are not
  comparable and nothing in the snapshot says which space a row is in;
- Health does not show the evidence.

**Design.** Extract the five line builders in `main-macos.mm` into a small header shared with
a native test that feeds each line through `ZoomEngineClient` parsing and
`ZoomSubscriptionSdkEvidence::observe` and asserts a retained row. Add a `platform` string to
the evidence node (new field, `"macos"` or `"windows"`; additive). Add one Health row listing
the last few evidence rows. This is size S, not the M the roadmap gave it.

## C3. Telemetry and crash upload (#434 / #436, M8.3; audit rows C11, D17)

**Windows.** Consent is opt-in, "Absent file or absent value = OFF"
(`MediaCore/Services/TelemetryConsentStore.cs:19`). Events: `POST /v1/events` with a Bearer
key (`TelemetryEventClient.cs:60-61`). Crashes: `POST /v1/crashes`, zip body, Bearer,
`X-CoreVideo-Version` and `X-CoreVideo-Machine-Class` (`CrashReportUploader.cs:117-120`).
Endpoint `https://telemetry.corevideo.io`, override `COREVIDEO_TELEMETRY_ENDPOINT`; the key
comes only from `COREVIDEO_TELEMETRY_API_KEY` and is never baked in; with no key both stay
"quietly disabled" (`CrashReportUploader.cs:15-17`, `docs/reference/production-endpoints.md`).
UI: opt-in toggle with "Preview what's sent" (`Views/SettingsPage.xaml:187-223`), and a
crash consent bar with "Send report" on the next launch (`Views/StudioWorkspace.xaml:1337-1354`).
Tests: `TelemetryConsentStoreTests`, `TelemetryEventClientTests`, `TelemetryPayloadBuilderTests`,
`CrashReportUploaderTests`, `CrashReportArchiveBuilderTests`, `TelemetryEventServiceTests`.

**Mac today.** `Diagnostics.swift:20-21`: "There is deliberately no uploader and no telemetry
here." The support bundle lists crash files but does not copy them. The owner decision of
2026-11-14 reverses that position.

**Design.** `Telemetry.swift` (new): consent store (a small JSON file in Application Support,
absent = off), payload builder with the same field set as `TelemetryPayloadBuilder`, event
client and crash uploader on `URLSession`, same paths and headers. Crash input on Mac is the
`.ips` reports for the shell, core and engine in `~/Library/Logs/DiagnosticReports`, zipped
with the same manifest shape as `CrashReportArchiveBuilder`, with a watermark file so a report
is offered once. Machine class is `mac-arm64` plus the chip family (new value; the ingest
worker must accept it: UNVERIFIED, see risks). UI: the opt-in card with a payload preview on
the Zoom tab, and a "Send report" bar on the launch after a crash. Nothing is sent without
consent, and nothing at all without a key, exactly as Windows. The header comment in
`Diagnostics.swift` is corrected to scope its promise to the support bundle.

## C4. Dedicated Zoom ISO feed on Mac (#989 / #1189 / #1177, M8.4, optional)

`ZoomEngineRuntime::independentIsoVideoSupported()` returns false on Apple
(`ZoomEngineRuntime.h:114-120`) and `setVideoConsumerDemand` returns early (`.cpp:780`), so Mac
ISOs record from the same decoded frame the compositor uses. Windows sends an `iso_video`
engine line and reads a per-source ISO ring (`shared/iso-video-ring.h`,
`engine/iso-video-demand.h`). `main-macos.mm` has no `iso_video` handling. Porting means: the
Mac engine handles `iso_video`, writes arrival-ordered frames to the ISO ring, and the flag
flips to true. It is L, native, and changes the recording path, so it needs an ISO pixel and
cadence proof (`validate-iso-record.mjs`) before and after. Recommendation: defer past the
parity release unless the M0.4 / M0.5 ISO fixes leave a visible ISO defect that only this
transport fixes.

## C5. Leftover sweep: rows no other spec owns

Rows from `2026-11-15-mac-ui-parity-settings-automation-health.md`. Rows already fixed in this
worktree by the Tier 0 work are listed last so nothing looks lost. "Checked" means I confirmed
the row's state in `mac-tier0b`; "audit" means I relied on the audit.

| Row | Leftover | Size | Lane | State |
|---|---|---|---|---|
| A2 | Manual-production copy lacks the Tiles and Stream Deck guidance | S | shell | audit |
| A3 | "Fill free inputs when participants join" toggle, persisted, default on | S | shell | checked: no pref |
| A4 | Delete dead `AutomationExtrasSection` / Magic Scene code | S | shell | checked: still at `AutomationExtras.swift:283` |
| B1 | Footer "More" deep-links to a Settings section | S | shell | audit |
| B4 | GPU-tier limit override, tier status and notice | M | shell | audit |
| B6 | Streaming platform presets (YouTube, Twitch starting points) | S | shell | audit; hand to the outputs spec if it wants it |
| B24, E5, G21 | Multiview overlay toggles (labels, tally, meters, clock) and the master L/R meter overlay, main view and pop-out | S | shell | checked: no toggles |
| C1 | Join fields locked while joining or in a meeting | S | shell | checked: never disabled (`ShellApp.swift:1379-1389`) |
| C3 | Recent meetings: hide when empty, summary rows, no false "selected" label | S | shell | checked (`ShellApp.swift:1400-1409`) |
| C5 | Join disabled reasons and a live hint (not only "ID empty") | S | shell | checked (`ShellApp.swift:1444`) |
| D4 | Audio observation (meter age) row in Health | S | shell | audit |
| D6 | "Extra logs" verbose toggle and status | S-M | shell + core log-level check | checked: none |
| D10 | Audio validation checklist and first-issue line in Health | M | shell | audit; hand to the audio spec if it wants it |
| D11 | Monitor and control recovery summary | S | shell | audit |
| D13 | Native lower third status and timing row | S | shell | audit |
| D15 | Per-feed Zoom health rows in Health | S | shell | audit |
| E4 | Pop-out tile drag-swap (the main view has it now) | S | shell | UNVERIFIED whether the shared canvas already gives the pop-out drag-swap |
| E6 | Pop-out header: remove chips that drive the main window | S | shell | audit |
| F5 | Scene canvas keys: arrow nudge, Shift aspect lock, snapping, Esc cancels a drag | S | shell | checked: comment at `StudioSceneEditing.swift:231-232` says not yet |
| F6 | macOS menu bar: Take, FTB, pop-outs, and Cmd+comma opening Settings | S | shell | checked: only New Window removed (`ShellApp.swift:111`) |
| G4 | Zoom status pill always visible (offline state) | S | shell | checked (`ShellApp.swift:464-466`) |
| G5 | Update-available banner, once per launch | M | shell | checked: no app update check |
| G6 | Show strip states "Choose inputs" and "On air" | S | shell | checked: no such strings |
| G14 | Scene rail still hard-codes "1920x1080 · 60 fps" | S | shell | checked (`ShellApp.swift:972`); the header chip is fixed |
| G23 | "Room" toggles the room rail instead of switching tabs | S | shell | checked (`ShellApp.swift:788`) |
| G25 | Room rail: real role, health, selection as the audio-processing target | S | shell | checked: "Guest" hard-coded (`ShellApp.swift:1238`) |
| 2.13a | Zoom quality caps (max resolution and fps); also feeds C1's cap note | M | shell | audit |
| 2.13b | Capture audio-sync nudge of plus or minus 20 ms | S | shell | audit; hand to the audio spec if it wants it |
| M8.5 | Windows engine `main.cpp` substring dispatch (same bug M0.2 fixed on Mac) | S | native, needs a Windows build | not a Mac item; listed so it is not lost |
| n/a | Mac `ZoomOAuth` pending-authorization timeout parity check | S | shell | UNVERIFIED in the 11-14 audit; verify, then fix or close |

Owned elsewhere, so not sized here: A5, A6 (remote control); B5, B8, B9, B11, B12, B13
(outputs and ingest); B14, B15, B17, D12 (audio); D14 (media clip playback); F4, G24 (graphics
shell); G11 (Tiles); G2's Comms entry (Part A).

Already fixed in `mac-tier0b` (checked): B3, B7, B18, B19, B22, B23 (`ProductionSettings.swift`),
B20 (`RecordingFolder.swift:158`), B15 (`MonitorOutputDevice.swift`), C6
(`main-macos.mm:582,659,791`), C8 (`ZoomOperatorStatus.swift`, `ShellApp.swift:1474-1487`),
G8, G9, G10, G16, G17, G19 (`StudioSceneEditing.swift`), and the header half of G14.

The leftover rows total roughly 2 weeks of shell lane. They are independent of each other and
can be batched as fillers between larger slices.

## C6. Slices (Remaining)

| ID | What lands | Lane | Size | Depends on | Proof |
|---|---|---|---|---|---|
| R1 | Input rows projection (accept and reset rules, format, frame age, status) with the format chips on the existing `SlotRow`. | shell | M | none | Shell tests ported from the Windows projection cases (stale revision ignored, epoch change resets, retired epoch ignored, stale format ages out); fake-engine run shows `1920x1080@60`. |
| R2 | REC chip from writer observations, the applied-cap note, and the row preview crop. | shell | M | R1; cap note needs leftover 2.13a or shows nothing | Shell tests for the four chip states from recorded snapshot fixtures; `COREVIDEO_SHELL_SNAPSHOT` image shows each row's crop matching its multiview tile. |
| R3 | Subscription evidence: shared line builders, parser round-trip test, `platform` field, Health row. | native + shell | S | none | Native test fails if any of the five Mac lines stops being retained. |
| R4 | Telemetry consent, heartbeat and event client, opt-in card with payload preview. | shell | M | none | Ported consent, payload and client tests against a local stub server; no request without consent; no request without a key. |
| R5 | Crash report collection, archive, next-launch "Send report" bar, uploader. | shell | M | R4 | Ported uploader and archive tests; a deliberately crashed dev core produces one offered report, sent once to the stub. |
| R6 | Leftover batch 1, operator-facing: C1, C3, C5, G4, G6, G14, G23, G25, A3, F6. | shell | M | none | One shell test per rule; snapshots of the Zoom tab and Studio header. |
| R7 | Leftover batch 2, support and polish: D4, D6, D11, D13, D15, B1, B4, B24/E5/G21, E4, E6, F5, A2, A4, G5, OAuth timeout check. | shell | L | D6 needs a core log-level check | Shell tests; Health snapshot. |
| R8 | Dedicated Zoom ISO feed on Mac (optional). | native | L | owner decision 5 | `validate-iso-record.mjs` before and after; ISO pixel test; drill at `--load 8`. |

---

## 5. Acceptance and test strategy

**Unit tests (Swift, `COREVIDEO_SHELL_TESTS`).** Every ported Windows rule is quoted in its
task and points at the Windows test; the Swift test must fail on the old Mac behaviour first
(roadmap section 6). Ported suites: `ZoomCommsProtocolTests`, `CommsViewModelTests`,
`GraphicsSocialReviewTests`, `GraphicsSocialPlayoutTests`, the YouTube and Twitch suites, the
credential store tests (against `KeychainItem`), the telemetry and crash suites, and the
projection cases.

**Native tests.** Existing `CommsRelay*`, `CommsCore*`, `CommsProtocol*`, `MicGuard*`,
`ZCommsPorted*` must run in the macOS configuration (confirm the CI filter; this is the
UNVERIFIED item from the audit). New: comms dispatch order on Mac, the evidence line
round-trip (R3).

**Contract fixtures.** No new wire. The social card templates are the existing
`contracts/graphics-social/*.json`. For Comms, add a `zoom-comms-state` sample captured from
the fake engine as a shared decode fixture read by both `ZoomCommsProtocolTests` (C#) and the
Swift parser test, so the two parsers cannot drift (new fixture file; propose
`contracts/comms-state.fixtures.json`).

**End to end without Zoom.** `scripts/probe-comms.mjs` against the Mac core and the fake
engine (already platform-neutral), then against the real Mac engine outside a meeting (A1).
The Mac shell against the fake engine via `COREVIDEO_ZOOM_ENGINE_PATH` for A3, A4, R1, R2.

**Pixel proofs.** Social: Metal readback of Program with the card shown, of Preview without
it, and of Program after 15 s. A wrong-client request leaves the Program readback unchanged.
Comms page and inspector rows: `COREVIDEO_SHELL_SNAPSHOT` images. No pixel claim is accepted
from reading code.

**Audio proofs.** Talkback is proven by audio at a guest, recorded on the guest machine: tone
keyed to guest A is present in A's recording and absent in B's and in the meeting recording.
Private comms ON: the operator's voice is absent from the meeting recording while present at
the keyed guest.

**Drills.** `python scripts/mac-show-drill.py --seconds 40 --load 8` before and after A2 (the
engine gains a mic thread and a pacer) and after R8, judged on per-frame delivery. During the
A5 live session, hold a key for 60 s with 8 feeds up and confirm zero new underruns or missed
deadlines in `realtimeEvidence` and zero `zoomSubscriptionChurn` movement.

**Owner live checks.**
- A5, co-host meeting with two desktop-client guests and one web-client guest: page states;
  key one guest; ALL CALL; latch; a held key drops within 1.5 s when the shell is force-quit;
  keys drop on tab change and on switching apps; web guest shows NO TALKBACK; Private ON and
  ON to OFF leave the operator muted; video, ISO and recording unaffected throughout.
- B3: YouTube live stream, approve and show one message, card ends at 15 s.
- B4: Twitch channel, approve a message, delete it as a moderator, confirm it cannot be shown;
  pull the network for 10 s and confirm approvals clear.
- R5: after a forced crash, the next launch offers the report once.

## 6. Risks and unknowns

| Risk | Resolution |
|---|---|
| `sendAudioDataToChannel:` thread affinity on macOS is unknown (upstream says so). A main-queue hop adds jitter under load. | A2 spike in a live meeting, both decorator modes, measured by `stats.underruns` / `sendErrors` and a guest-side recording. Ship the mode that is clean; if neither is, stop and report before building the page. |
| The upstream Mac adapter has only been compiled, never run against a meeting; a `ZoomClient` Mac implementation does not exist upstream. | Pro does not need `ZoomClient` (the engine is the client). A2's live spike is the adapter's first real run; budget for fixes inside L. |
| The local ZComms clone lacks the pinned commit; upstream may have moved. | A1 fetches upstream and either copies at `168a572d` or re-pins and re-copies all listed files in one commit. |
| miniaudio opening the microphone from the engine triggers a TCC prompt attributed to the engine bundle, and re-prompts with ad-hoc signing. | Developer ID dev signing (M0.11) first; A1 records who the prompt names; update the usage string to mention talkback. |
| Mute on Mac is a meeting action command; the "never auto-unmute" and "unmute only after initialise" rules depend on its exact behaviour (for example when the host has disabled self-unmute). | MicGuard already models refusal; A2 tests the host-disallows-unmute case live and the page shows the reason. |
| Whether the Mac engine's join leaves the operator's mic in the same state Windows Pro does (not capture-only). UNVERIFIED. | A2 reads the join path and the live MTG MIC lamp with Private OFF; documented in the plan before the page ships. |
| Social depends on two other specs (Metal graphics, graphics shell). | B2's model and tests are written early; the slice ships only with a card on Program. B1 is independent and lands first. |
| Twitch ordering fences use nanosecond timestamps; Swift date parsing loses precision by default. | Parse the fraction as an integer in the port; the ported `TwitchEventSubSessionTests` ordering cases catch it. |
| Row previews as extra layers on the multiview IOSurface could add compositing cost in the shell at 60 Hz. | Measure with ten rows visible; if it costs frames in the drill, refresh row previews at the multiview cadence divisor rather than per display tick. |
| The telemetry worker may reject an unknown machine class or the `.ips` archive shape; hosts do not resolve yet. | R4/R5 are proven against a local stub; one staging call once the owner checklist in `production-endpoints.md` is done. No Mac-visible failure until a key exists. |
| macOS CI may not run the Comms tests. | Check the job filter in A1; add them if missing. |

## 7. Owner decisions needed

1. **Private comms default and copy on Mac.** Recommendation: identical to Windows, OFF by
   default, same strip button and wording.
2. **Where the Comms tab sits and whether it shows before a live qualification.**
   Recommendation: same position as Windows (after Audio); ship it only after A5 passes, since
   talkback has never been live-proven on either platform.
3. **Social scheduling.** Social cannot air before the Metal graphics backend. Recommendation:
   land B1 now, write B2's model alongside the graphics shell work, and schedule B3/B4 last,
   as the roadmap orders it.
4. **Crash reports from which processes.** Windows uploads shell-side dumps. On Mac the `.ips`
   files exist for shell, core and engine. Recommendation: offer all three, since core and
   engine crashes are the ones the owner most needs.
5. **Dedicated Zoom ISO feed on Mac (R8).** Recommendation: defer past the parity release;
   revisit only if a Mac ISO defect is traced to sharing the compositor's frame.
6. **Update banner (G5).** Windows checks for a newer release and offers it. Mac ships as a
   notarized .pkg. Recommendation: banner with a link to the download page, no in-app install.

## 8. Out of scope

- Auto director, Set & Forget, Magic Scene, OHG, EngineHost, NDI graphics key (settled).
- The Metal graphics backend, the graphics command gateway and Operate surface, Tiles, media
  clip playback, the Sources table rebuild, colour grade and scopes, remote-control action
  coverage and OSC, audio features, outputs and ingest, browser sources, and the virtual
  camera: each has its own spec. This spec only states what it needs from them.
- Breakout-aware talkback, echo cancellation (`ZC_WITH_AEC` is compiled out on both platforms),
  and any talkback feature Windows does not have.
- New social providers, chat moderation actions, or posting to chat.
- Changing the telemetry endpoints, the key delivery mechanism or the ingest worker.
- The Windows engine dispatch fix (M8.5) beyond listing it.

## 9. Where this differs from the roadmap and audits

- **M0.2 is already done on Mac.** The reader dispatches by parsed command with an
  unknown-command log (`main-macos.mm:2331-2403`). The audit's "latent hazard" and "silently
  discarded" text is stale. The comms branch still goes first, by `IsCommsCommand`.
- **M8.2 is mostly done.** The Mac engine has emitted all five evidence stages with matching
  fields since August. The remaining work is a pinning test and visibility: S, not M.
- **The Mac has a control API now** (`ControlApi.swift`, `ControlHttpServer.swift`), so the
  11-14 audit's section 6 ("no control endpoint at all") is stale. The Windows control registry
  has no comms or social actions (grep of `ControlActionRegistry.cs`), so neither Part A nor
  Part B adds remote actions.
- **Many Tier 0 audit rows are already fixed in this worktree** (list at the end of C5), so the
  leftover table is shorter than the audit suggests.
- **Audit line numbers moved.** The graphics refusal is at `MediaCore.cpp:1272-1273` (audit:
  1256), the default at `Interfaces.h:1488` (audit: 1480), `handleEvent` at
  `AppModel.swift:1898-1919` (audit: 1002-1021), and the mailbox and RPC files are under
  `native/src/rpc/`, not `native/src/core/`.
- **The Comms engine effort is L, as the roadmap says, not the audit's XL**, because the
  adapter exists upstream, the policy code is already compiled on macOS, and every SDK call has
  a verified Mac equivalent. The uncertainty is concentrated in one question (send threading).
