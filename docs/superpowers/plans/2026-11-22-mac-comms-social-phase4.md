# macOS parity Phase 4: Comms talkback and social review, implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Mac operator can cue guests over Zoom talkback from a Comms tab that behaves like Windows CoreVideo Pro 1.0, and can review YouTube and Twitch chat and put one approved message on Program as a card.

**Architecture:** The shared core already relays `comms-*` and emits `zoom-comms-state` on macOS. The shared core also gates `social-review-*` graphics. This plan adds the three missing pieces. (1) The Mac Zoom engine gets the ported ZComms `TalkbackSdkMac` adapter and a new ObjC++ glue file, `engine-comms-macos.mm`. The glue is the twin of `engine-comms.cpp`, and its SDK-specific decisions live in pure headers that are tested on every platform. (2) The Swift shell gets a pure `CommsModel` (a port of `CommsViewModel`) and a Comms page that copies Windows' five-row layout. The tab stays behind a gate until the live co-host test passes. (3) The Swift shell gets a pure social review queue, a serialized social playout that uses its own gated gateway path, a review sheet, and YouTube and Twitch transports on `URLSession`. The C# tests are the oracle for every Swift test.

**Tech Stack:** ObjC++ (Zoom Meeting SDK macOS 7.1.5.84750, miniaudio v0.11.21 CoreAudio backend), C++20 and gtest (`corevideo-native-tests`), Swift 5 / SwiftUI / AppKit (`mac-shell`, in-binary harness `COREVIDEO_SHELL_TESTS`), `URLSession` and `URLSessionWebSocketTask`, Security.framework (`KeychainStore`).

**Spec:** `docs/superpowers/specs/2026-11-15-mac-comms-social-remaining-design.md` (Parts A and B, sections 5 to 7). Also read `docs/superpowers/plan-authoring-rules.md`, `docs/reference/comms-talkback.md`, and `docs/reference/graphics-playout.md` (Social review sections).

Every `file:line` below was read on `origin/main` at `e117a8fa` on 2026-11-22. Re-check a line number before editing at it; other Phase 4 lanes are landing.

## Global Constraints

- Private comms default: **OFF** (owner ruling 2026-11-10, re-confirmed for Mac 2026-11-15). ON mutes the operator's meeting mic at once. The silent virtual mic is installed only with a raw-data licence, and the operator is unmuted only after `onMicInitialize`. If initialisation does not arrive within 5 s, the operator stays muted. ON to OFF removes the virtual mic and leaves the operator "MUTED, never auto-unmuted" (`docs/reference/comms-talkback.md`; `comms/mic_guard.cpp`).
- **The Comms tab is not visible by default until Task 11 passes** (owner decision 2). Before that, it shows only with `COREVIDEO_SHELL_COMMS=1`.
- **Social order:** YouTube (Task 15) and Twitch (Tasks 16-17) are scheduled last (owner decision 3). B1 Keychain is already on main as `KeychainStore.swift`.
- Lease: `leaseMs` **1500**, renewed every **500 ms** (`CommsViewModel.cs:93-94`). Private re-send at most once per **2000 ms** (`:102`). Gain is clamped **-20..20 dB**. There are **16** channel slots (`ZoomCommsRelay.cpp:11`).
- Engine comms tick: **100 ms**. Roster poll: **1000 ms**. Every Zoom SDK call runs on the main queue (`main-macos.mm:29-41`). The mic chain runs on its own worker thread.
- Do not hand-edit any ported file. Copy it verbatim from ZComms `168a572d0f537a01a762ba97c8004ca7d7d0ff39` and add the banner (`native/zoom-engine/engine/comms/PORTED.md`).
- "A cell is never 'on air' unless the engine reports it keyed and someone confirmed in the channel" (`comms-talkback.md` UI rules). "Never rebuild a control under the operator's finger."
- Talk keys are press and release only. "Click and Command are never used." Digits 1-9 and Space are panel-scoped and never global.
- Social approval is per exact revision. The ruling is "Approval alone sends no native command and leaves Program unchanged." Wire identity:
  - `clientId` `graphics-social-review`
  - `instanceId` prefix `social-review-`
  - the cue uses `rideTake:false` and no `cueOwner`
  - Show sends `target:"cue"`
- Social bounds: message **192** UTF-8 bytes, author **80**, queue **200**, removal budget **4096**, lifetime **15 min**. The card stays on air at most **15 s** and a hidden card expires after **10 s** (`GraphicsInstances.cpp:561,1161`).
- "No chat payload, author, API key or cursor goes into show files, telemetry, crash annotations or logs" (slice-6 spec :15). The same rule covers Twitch access and refresh tokens. A secret never appears in a URL, a log line, an error string, `description`, or the support bundle.
- YouTube:
  - the key goes in header `x-goog-api-key`, never in the URI
  - redirects are refused
  - polls are never faster than **15 s**
  - retry backoff is **30, 60, 120 s**
- Twitch:
  - scope `user:read:chat` only
  - device-code grant only, with no client secret and no redirect URI
  - validate at startup, then hourly
  - the EventSub message cap is **65536** bytes
  - four subscriptions, in this exact order: `channel.chat.clear`, `channel.chat.clear_user_messages`, `channel.chat.message_delete`, `channel.chat.message`
  - reconnect budget **3** (1, 2, 4 s)
- Colours come only from `Studio` tokens (`DesignSystem.swift:12-34`). On air = `Studio.red` (0xE5433F, Windows `StudioAirBrush`). Armed = `Studio.amber`. Lamp on = `Studio.accent` (0x22C86E, Windows `StudioLiveBrush`).
- Every test that involves time takes an injected clock (`now: () -> Int64` ms or `now: () -> Date`) and an injected `sleep`. No test sleeps for wall time to observe a rule.
- State that a core or engine restart loses is re-applied on the next generation. This happens either in `AppModel.onConnected()` (`AppModel.swift:1771`) or through a rule the report drives. Each task that adds such state names its rule and tests it.
- The roughly 30 s wait for Zoom video after a join is normal ramp-up. Do not chase it in any live step.

## Review Focus

1. **The Mac SDK reports meeting status values that Windows does not have.** macOS has no LOCKED/UNLOCKED and adds `AudioReady = 8`, and the numeric values differ. A reasonable person expects talkback to stay up through a mid-meeting audio-ready or reconnect notification, with no 16-channel re-request. Pinned in Task 2 (`classifyMeetingStatus`) and checked live in Task 5.
2. **Microphone permission is denied or not yet asked for.** On macOS a denied mic reads as digital silence, not as an error. Talkback would then look healthy (TX lamp on) while the guest hears nothing. The expected behaviour is the stated reason in the settings drawer and ops strip. Pinned in Task 4 (`micAccessError`) and checked live in Task 5.
3. **The core or engine restarts while Private comms is ON or a key is held.** A reasonable person expects held keys to drop and Private to be re-asserted once the new report disagrees, at most once per 2 s, without them having to re-toggle anything. Pinned in Task 8.
4. **The operator switches apps, closes the window, quits, or leaves the tab while holding a key.** A reasonable person expects the key to go up immediately, and in any case within 1.5 s if the shell hangs. Pinned in Task 10 and checked live in Task 11.
5. **The message changes, or chat reconnects, between Approve and Show.** This covers a Twitch reconnect, a moderator delete, a YouTube edit, and the queue expiring. A reasonable person expects the card never to air an unapproved or older revision, and expects the card to leave Program within 15 s. Pinned in Tasks 13 and 17 and checked live in Task 18.

---

## Boundary audit: Windows vs Mac (verified 2026-11-22)

This section lists what each task must honour. Every row was read in code or in the SDK headers at `~/Downloads/zoom-sdk-macos-7.1.5.84750/ZoomSDK/ZoomSDK.framework/Versions/A/Headers`.

**Zoom SDK calls Windows makes for comms or audio send, and the Mac form** (Windows: `engine-comms.cpp`, `main.cpp`):

| # | Windows | Mac today | Mac form (header:line) | Semantic difference the glue must handle |
|---|---|---|---|---|
| 1 | `SetTimer(100 ms)` on the SDK thread (`engine-comms.cpp:614`) | none | `dispatch_source` timer on the main queue | Must not call `getMeetingService` before `initSDKWithParams` succeeds. The timer starts in `main()`, before init. |
| 2 | `GetMeetingStatus` | none | `-[ZoomSDKMeetingService getMeetingStatus]` (`ZoomSDKMeetingService.h:786`) | Different enum (row 15). |
| 3 | `GetMeetingTalkbackController` | none | `getTalkbackController` (`ZoomSDKMeetingService.h:640`) | none |
| 4 | `TalkbackSdkWin` (Begin/Add/Execute batches, `SendAudioDataToChannel`, `SetChannelBackgroundVolume`, `SetEvent`) | none | `TalkbackSdkMac` at the pin (one atomic `inviteUsersToChannel:` and the other calls) | The adapter requires `-fobjc-arc` (its `#error`). Whether `sendAudioDataToChannel:` may be called off the main queue is unknown; Task 5 measures it. |
| 5 | `GetMySelfUser`, `GetParticipantsList`, `GetUserByUserID` | roster only (`main-macos.mm:526`) | `getMyself`, `getParticipantsList`, `getUserByUserID:` (`ZoomSDKMeetingActionController.h:924-945`) | The list is an `NSArray` of `NSNumber`. |
| 6 | `GetUserRole` == `USERROLE_HOST`/`COHOST` | none | `getUserRole` → `UserRole_Host`/`UserRole_CoHost` (`ZoomSDKErrors.h:750-760`) | none |
| 7 | `GetUserName`, `IsHost`, `IsSupportTalkback`, `GetUserID` | name/host only | `getUserName`, `isHost`, `isSupportTalkback` (`:532`), `getUserID` | none |
| 8 | `IsAudioMuted()` (bool) | folded at `main-macos.mm:510-515` | `getAudioStatus` enum (`ZoomSDKErrors.h:524-536`) | `None(0)` means audio is not joined, and the glue reads it as muted. The existing roster fold leaves `None` unmuted; the comms fold must not. |
| 9 | `GetAudioJoinType() != AUDIOTYPE_NONE` | none | `getAudioType` (`:349`), `ZoomSDKAudioType_None = 0` | none |
| 10 | `MuteAudio(myId, true)`, guarded by `GetUserID() != 0` | none | `actionMeetingWithCmd:ActionMeetingCmd_MuteAudio userID:myId onScreen:ScreenType_First` (`:932`; `ZoomSDKErrors.h:61`) | A meeting action, not an audio-controller call. Never send userID 0. |
| 11 | `UnMuteAudio(myId)` | none | `ActionMeetingCmd_UnMuteAudio` (`ZoomSDKErrors.h:63`) | Can be refused when the host forbids self-unmute. The code goes into the ops log verbatim. |
| 12 | `HasRawdataLicense()` → **bool** | none | `-[ZoomSDKRawDataController hasRawDataLicense]` → **ZoomSDKError** (`ZoomSDKRawDataController.h:153`) | Licensed only when it returns `ZoomSDKError_Success`. The spec's table listed this as a 1:1 match. |
| 13 | `GetAudioRawdataHelper()->setExternalAudioSource(&mic / nullptr)` | none | `[[ZoomSDK sharedSDK] getRawDataController]` → `getRawDataAudioSourceHelper:` (`:180`) → `setExternalAudioSource:` (`ZoomSDKRawDataAudioSourceController.h:65`) | The delegate is an ObjC object. The glue must hold a strong reference for the process lifetime, as Windows keeps `SilentMic` in static state. |
| 14 | `IZoomSDKVirtualAudioMicEvent` (4 callbacks) | none | `ZoomSDKVirtualAudioMicDelegate` (`:33-50`) | Same four callbacks. Keep the state in atomics, matching Windows. |
| 15 | `on_meeting_status(int)` first in `onMeetingStatusChanged`; returns early on `LOCKED`/`UNLOCKED` (`engine-comms.cpp:567`) | none | `ZoomSDKMeetingStatus_*` (`ZoomSDKErrors.h:454-482`) | The Mac enum has **no** Locked/Unlocked, adds `AudioReady = 8`, and every value differs from Windows (Mac 9 = `OtherMeetingInProgress`). Passing the raw int to a Windows-shaped function would be a bug. Task 2 maps it. |
| 16 | `set_capture_only(line has "capture_only":true)` from the join line (`main.cpp:1536`) | none | the same call in `handle_join` | Pro never sends the flag, so this is false and Private comms governs the mic. |
| 17 | `EngineComms::shutdown()` before `Leave` and `CleanUPSDK` (`main.cpp:1668`) | the quit block runs `ipc_teardown` then `unInitSDK` (`main-macos.mm:2389-2397`) | comms shutdown is the first statement in that block | IPC must still be open: shutdown writes debug lines. |
| 18 | WASAPI capture and sidetone through miniaudio | none | miniaudio CoreAudio backend. Link CoreAudio, AudioToolbox and CoreFoundation, as ZComms `CMakeLists.txt:81-88` (macOS CI) does. | Mic TCC is the next block. |

**Join audio parity:** Windows sets `isAudioOff=false` and `isMyVoiceInMix=true` (`main.cpp:1041-1042,1592-1593`). Mac sets `isNoAudio=NO` and `isMyVoiceInMix=YES` (`main-macos.mm:839-840`). Neither engine calls an auto-join-audio setting. The Mac has `-[ZoomSDKSettingAudioController enableAutoJoinVoip:]` (`ZoomSDKSettingAudioController.h:209`), and nothing calls it. With `needCustomizedUI = NO` on both, whether the Mac SDK joins VoIP without a dialog is **UNVERIFIED**. The MTG MIC lamp depends on it (`mic_open = audioType != None && !muted`). Task 5 records `comms_audio_join` and stops for an owner decision if the answer is `None` (see owner decision N3).

**Microphone TCC and entitlements:**
- The core starts the engine with `fork()` (`native/src/modules/ZoomEngineProcess.cpp:180`) and does not disclaim responsibility. The core is itself the shell's child, with no disclaim (`RecordingFolder.swift:58-60`). TCC therefore attributes the engine's microphone use to **CoreVideo Pro.app**, so the prompt shows the shell's string: "CoreVideo Pro captures audio inputs for the production mix." (`make-macos-shell-bundle.sh:133`). The engine bundle's own string (`make-macos-engine-bundle.sh:82`) is not the one the operator sees. The spec placed this risk on the engine bundle.
- The hardened-runtime `com.apple.security.device.audio-input` entitlement is present on both the shell (`scripts/macos-shell.entitlements`) and the engine (`scripts/macos-engine.entitlements`). No entitlement change is needed.
- A denied grant gives silence, not an error. The glue checks `+[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]` before opening the capture device (Task 4). Whether a forked child reads the responsible app's grant is **UNVERIFIED**; Task 5 records it.

**OAuth redirect handling for YouTube and Twitch:** none is needed. YouTube uses a Data API key only, and Twitch uses the device-code grant only. A grep of the Windows social sources for `redirect_uri`, `localhost`, `127.0.0.1` and `client_secret` finds nothing. The Mac `corevideo://` scheme and the Zoom loopback (`ZoomOAuthLoopback.swift`) are Zoom-only and must not be reused. The Twitch client ID is **user-entered**, not baked in (`TwitchAuthorization.cs:46`).

**Keychain:**
- `KeychainStore` is on main (`KeychainStore.swift:89-115`), with no `kSecAttrAccessible` set and no access group. The shell is not sandboxed.
- Windows stores the social secrets as DPAPI JSON files (`YouTubeChatCredentialStore.cs`, `TwitchChatCredentialStore.cs`). Mac uses two generic-password items under the service `us.iamfatness.corevideopro.social`:
  - account `youtube-api-key` holds the key string
  - account `twitch-credentials` holds one JSON blob, so the rotated access+refresh pair is atomic, as on Windows
- Windows persists **only** credentials. Video ID and channel live in memory (`StudioViewModel.GraphicsSocialReview.cs:36-38`), and Mac does the same.

**Other Mac boundaries found:**
- The bridge throws away `error.code` (`MediaCoreBridge.swift:280-284`). Windows shows a comms refusal's `message` verbatim and never branches on the code (`CommsViewModel.cs:600-613`), so the message alone is enough.
- `CoreLaunchEnvironment.extras` overwrites `COREVIDEO_ZOOM_ENGINE_PATH` whenever a real engine app is found (`AppModel.swift:6167-6173`). The spec's "develop the shell against the fake engine via `COREVIDEO_ZOOM_ENGINE_PATH`" therefore does not work on a dev machine with an engine build. Task 6 fixes this so a pre-set value wins, matching Windows `run-studio.ps1`.
- `TabPage` wraps every non-Studio tab in one outer `ScrollView` (`ShellApp.swift:1286-1294`). Windows Comms is a 5-row grid: a fixed rail, a scrolling desk, a fixed settings drawer, a fixed strip and a fixed ops strip (`CommsPage.xaml:51-58`). Inside the outer scroll, the strip and ops would scroll away. This is the stacked-in-one-scroll defect again, and Task 9 opts Comms out.
- The public `GraphicsGateway.invoke` refuses `clientId == "graphics-social-review"` (`GraphicsGateway.swift:282-286`). `GraphicsPlayout.sendAs` goes through it (`GraphicsPlayout.swift:559-568`). Social needs its own gated path, as on Windows (`GraphicsCommandGateway.cs:45-46`).
- The social card templates use the font `"Segoe UI"` (`contracts/graphics-social/youtube-chat-card.json:11-15`). On Metal this resolves through the G2 font substitution, so Task 14 proves the card's pixels.
- Windows filters only the `youtube-chat-card` template from the workspace and strip, while instances are filtered by the `social-review-` prefix on both platforms (`GraphicsWorkspaceSession.cs:144-148`). The Mac strip knows only `youtube-chat-card` (`GraphicsState.swift:254-273`). Task 14 filters both providers by prefix and labels Twitch. The Windows inconsistency is owner decision N4.
- No control API action can reach comms or social. Windows has no `comms.*` or `social.*` ids, and every Mac `graphics.*` id except the lower-third pair is blocked (`ControlManifest.generated.swift`). Task 14 keeps that true with a test.

---

## Owner decisions

Already made, and binding: (1) Private comms defaults to Off, as on Windows. (2) The Comms tab ships only after the live co-host test passes. (3) YouTube and Twitch come last.

New, needed before the named task. Each has a recommendation:

| ID | Question | Recommendation | Needed by |
|---|---|---|---|
| N1 | Mac-only copy for a denied microphone (Windows has no equivalent) | "Microphone access is off for CoreVideo Pro. Turn it on in System Settings > Privacy & Security > Microphone." | Task 4 |
| N2 | Should the shell's `NSMicrophoneUsageDescription` mention talkback, now that the prompt names the app? | Yes: "CoreVideo Pro captures audio inputs for the production mix and your talkback microphone." | Task 11 |
| N3 | If Task 5 finds the Mac join leaves meeting audio un-joined (audio type None) while Windows joins VoIP: call `enableAutoJoinVoip:YES`? | Only if Task 5 shows it, and only after the owner confirms Windows' behaviour in one Windows session. | Task 5 outcome |
| N4 | The Windows strip and workspace filter only `youtube-chat-card`, not `twitch-chat-card`. | Mac filters both. File a Windows `backlog` issue to fix Windows the same way. | Task 14 |
| N5 | Social HTTP on Windows ignores the system proxy (`UseProxy=false`). | Match it on Mac (proxy dictionary disables HTTP/HTTPS proxies). | Task 15 |
| N6 | The A5 checklist asked for 8 live video feeds. Owner + co-host + one web guest is what is available. | Run A5 with the available guests plus the synthetic `mac-show-drill --load 8` before and after. Record that live 8-feed comms load is unverified. | Task 11 |

---

## Task list

| # | Spec slice | Task | Lane | Size | Depends on |
|---|---|---|---|---|---|
| 1 | A1 | Port `TalkbackSdkMac`; build the comms libraries into the Apple engine | native | M | none |
| 2 | A1 | Pure engine policy, line routing, `MainQueueTalkbackSdk` decorator (tests everywhere) | native | M | none |
| 3 | A1 | Engine glue skeleton: route, start, status hook, shutdown; `available:false` report; idle probe | native | M | 1, 2 |
| 4 | A2 | Full glue: bank, roster, roles, MicGuard on the Mac SDK, audio worker, mic TCC gate | native | L | 3 |
| 5 | A2 | Send-threading spike and live engine proof (direct-engine probe, co-host meeting) | native + owner | M | 4 |
| 6 | A3 | Shared comms fixture, Swift parser, `handleEvent` case, engine-path override | shell | M | none |
| 7 | A3 | `CommsModel` (pure port of `CommsViewModel`) | shell | L | 6 |
| 8 | A3 | Wiring: gate, tab, prefs, lease timer, meeting state, restart re-assert, minimal page | shell | M | 7 |
| 9 | A4 | Full Comms page (Windows layout), `TalkKey`, snapshots | shell | L | 8 |
| 10 | A4 | Release triggers and panel-scoped keys | shell | M | 9 |
| 11 | A5 | Live co-host acceptance, remove the gate, docs, outcomes | owner + shell | S | 5, 10 |
| 12 | B2 | `SocialReview` model | shell | M | none |
| 13 | B2 | `SocialPlayout`, gated social gateway path, bundled card templates | shell | L | 12 |
| 14 | B2 | Review sheet, view model, entry point, strip filter, real-core fail-closed and pixel check | shell | M | 13 |
| 15 | B3 | YouTube transport, feed, key store, connect form | shell | M | 14 |
| 16 | B4 | Twitch HTTP transport, device authorization, credential store | shell | M | 14 |
| 17 | B4 | Twitch EventSub socket, session, feed | shell | L | 16 |
| 18 | B3/B4 | Social live acceptance (YouTube, Twitch), redaction sentinel run, docs, outcomes | owner + shell | S | 15, 17 |

**Parallelism:** Tasks 1-5 are native. Tasks 6-11 and 12-18 are two shell streams. Task 5 drives the real engine directly, so the native spike does not wait for the shell page. Task 11 joins the two Comms streams. Calendar estimate: native about 2 weeks, Comms shell about 2 weeks, Social shell about 3.5 weeks. Only Tasks 5, 11 and 18 need people.

**Live-test needs:**
- Task 5: owner + co-host, one Zoom meeting, about 45 min. The engine joins as a guest using the public app key, and the owner makes it co-host. Two desktop-client guests are better than one.
- Task 11: owner + co-host + one Zoom **web-client** guest, about 60 min, on the installed signed build.
- Task 18: the owner's YouTube public livestream with an API key, and a Twitch channel with a moderator account (owner or co-host), about 60 min.

---

### Task 1: Port `TalkbackSdkMac` and build the comms libraries into the Apple engine

**Files:**
- Create: `native/zoom-engine/engine/comms/talkback_sdk_mac.h`, `native/zoom-engine/engine/comms/talkback_sdk_mac.mm` (verbatim from ZComms `168a572d` `src/zoom/`, banner added)
- Modify: `native/zoom-engine/engine/comms/PORTED.md` (add a `talkback_sdk_mac.{h,mm}` row to the Verbatim table; note it is compiled only into the Apple engine)
- Modify: `native/CMakeLists.txt:799-883` (engine block)

**Interfaces:**
- Produces: `zc::TalkbackSdkMac(void* controller)` (a `ZoomSDKTalkbackController*`), which implements `zc::TalkbackSdk` (`comms/talkback_sdk.h`). Also the CMake targets `corevideo_comms_audio` (all platforms with the engine) and `corevideo-zoom-engine` linking it on Apple.

- [ ] **Step 1: Fetch and copy.** In `~/Developer/ZComms`, `git fetch origin` (the pin `168a572d` is `origin/main` as of 2026-11-22; `git cat-file -t 168a572d` must print `commit`). Copy `src/zoom/talkback_sdk_mac.h` and `.mm` from `git show 168a572d:<path>`. Prepend the same 3-line banner the other ported files carry ("Ported from ZComms (MIT, same owner) @ 168a572: src/zoom/talkback_sdk_mac.mm. …").
- [ ] **Step 2: Prove verbatim.** Run `diff <(git -C ~/Developer/ZComms show 168a572d:src/zoom/talkback_sdk_mac.mm) <(tail -n +4 native/zoom-engine/engine/comms/talkback_sdk_mac.mm)`. It must print nothing. Do the same for the `.h`. Also re-diff `talkback_sdk.h`, `talkback_channels.{h,cpp}`, `tx_pacer.cpp` and `audio_defs.h` against the pin; each may differ only by the banner. This was checked on 2026-11-22 and they matched.
- [ ] **Step 3: CMake.**
  - Move the `corevideo_comms_audio` definition (`CMakeLists.txt:862-873`) above the `if(APPLE)` split, so both branches define it with the same PUBLIC `MA_NO_*` definitions. Keep `NOMINMAX` and the `set_source_files_properties` for `engine-comms.cpp` and `talkback_sdk_win.cpp` in the non-Apple branch only.
  - In the Apple branch: add `zoom-engine/engine/comms/talkback_sdk_mac.mm` to `corevideo-zoom-engine` with `COMPILE_OPTIONS "-fobjc-arc"` set on that one source. Link `corevideo_comms_audio` and add `"-framework CoreAudio" "-framework AudioToolbox" "-framework CoreFoundation"` to the engine link (`corevideo_comms_audio` PUBLIC on Apple, as ZComms does).
- [ ] **Step 4: Build the engine.** Run `DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake -S native -B native/build-engine -G Ninja -DCMAKE_BUILD_TYPE=Release -DCOREVIDEO_ENABLE_DEV_ADAPTERS=ON -DCOREVIDEO_STUB=OFF -DCOREVIDEO_BUILD_ZOOM_ENGINE=ON -DZOOM_SDK_DIR=$HOME/Developer/zoom-sdk-macos && cmake --build native/build-engine --target corevideo-zoom-engine`.
  - Expected: it links with no warnings from the new sources. The CLT `DEVELOPER_DIR` works around the 2026-11-22 SDK linker break; drop it if the default toolchain links.
  - Then run `nm -C native/build-engine/corevideo-zoom-engine | grep -c 'zc::TalkbackSdkMac::SendAudio'`. Expected: `0`, because nothing references the adapter yet and the linker drops it. Record that. Task 3 makes it non-zero.
- [ ] **Step 5: Unaffected targets still build and pass.** Run `cmake --build native/build-metal --target corevideo-native-tests corevideo-zoom-engine-fake && native/build-metal/corevideo-native-tests --gtest_filter='*Comms*:*MicGuard*:*ZComms*:*Talkback*'`. Expected: PASS, same count as before the change.
- [ ] **Step 6: Confirm macOS CI runs the comms tests** (this resolves the spec's UNVERIFIED item). Run `ctest --test-dir native/build-metal -N | grep -c corevideo-native-tests` and read `.github/workflows/ci.yml:261-278` (`native-stub-macos` runs `ctest -LE realtime`). Record in the task report that the comms gtests are inside that ctest.
- [ ] **Step 7: Commit.** `git add native/zoom-engine/engine/comms/talkback_sdk_mac.* native/zoom-engine/engine/comms/PORTED.md native/CMakeLists.txt && git commit -m "feat(mac-engine): port ZComms TalkbackSdkMac and build the comms libraries into the Apple engine"`

**Mutation (rule 7):** remove `-fobjc-arc` from the adapter's source properties and rebuild. The adapter's own `#error "talkback_sdk_mac.mm requires ARC"` must fail the build. Report the result.

---

### Task 2: Pure engine policy, line routing, and the send decorator

Every decision in the Mac glue that does not need the SDK lives here. That way the decisions are tested on Linux, Windows and macOS CI.

**Files:**
- Create: `native/zoom-engine/engine/engine-comms-policy.h` (header-only, no SDK, no ObjC)
- Create: `native/zoom-engine/engine/main-queue-talkback-sdk.h` (header-only C++)
- Create: `native/zoom-engine/engine/engine-line-route.h` (header-only, includes `comms/comms_protocol.h` and `../shared/engine-command.h`)
- Create: `native/tests/EngineCommsPolicyTest.cpp`, `native/tests/MainQueueTalkbackSdkTest.cpp`, `native/tests/EngineLineRouteTest.cpp`
- Modify: `native/CMakeLists.txt:1128-1133` (add the three tests; add `zoom-engine/engine` and `zoom-engine/shared` to `corevideo-native-tests` include dirs if absent)

**Interfaces (Produces):**
```cpp
namespace engine_comms {
// Mac ZoomSDKMeetingStatus values, copied as ints (ZoomSDKErrors.h:454-482) so this header needs no SDK.
enum class MacMeetingStatus : int { Idle=0, Connecting=1, WaitingForHost=2, InMeeting=3, Disconnecting=4,
  Reconnecting=5, Failed=6, Ended=7, AudioReady=8, OtherMeetingInProgress=9, InWaitingRoom=10,
  WebinarPromote=12, WebinarDepromote=13, JoinBreakoutRoom=14, LeaveBreakoutRoom=15 };
enum class StatusAction { Ignore, ResetOwnership, ResetAndEndMeeting };
StatusAction classifyMeetingStatus(int macStatus);
// ZoomSDKAudioStatus 0..6 (ZoomSDKErrors.h:524-536): None and the three muted variants are muted.
bool selfMuted(int macAudioStatus);
// ZoomSDKAudioType (None=0): open = joined AND not muted.
bool meetingMicOpen(int macAudioType, int macAudioStatus);
// UserRole (ZoomSDKErrors.h:750-760): Host=1, CoHost=2.
bool hostOrCohost(int macUserRole);
// hasRawDataLicense returns ZoomSDKError; Success (0) only means licensed.
bool licensed(int zoomSdkError);
// Owner decision N1 copy; empty string = access granted. authStatus is AVAuthorizationStatus (0..3).
std::string micAccessError(int avAuthorizationStatus);
}
```
```cpp
namespace engine_comms {
enum class SendMode { Direct, MainQueue };
SendMode sendModeFromEnv(const char* value);  // "main" -> MainQueue; anything else, null included -> the shipped default
// Forwards every call unchanged to `inner` on the caller's thread, except SendAudio in MainQueue mode:
// copies the frame and hands it to `post` (production: dispatch_async(main)); returns Ok immediately.
class MainQueueTalkbackSdk : public zc::TalkbackSdk {
 public:
  MainQueueTalkbackSdk(zc::TalkbackSdk* inner, SendMode mode,
                       std::function<void(std::function<void()>)> post,
                       std::function<int64_t()> nowUs);
  // Hop latency of MainQueue sends, microseconds (post -> run), for the comms_send_hop breadcrumb.
  struct HopStats { uint64_t count; int64_t maxUs; int64_t p99Us; uint64_t droppedAfterShutdown; };
  HopStats TakeHopStats();   // resets the window
  void Shutdown();           // after this, posted sends that have not run are dropped (counted), never forwarded
  // ... all zc::TalkbackSdk overrides
};
}
```
```cpp
enum class EngineLineRoute { Comms, Engine };
EngineLineRoute routeEngineLine(const std::string& line);  // Comms iff edgecomms::IsCommsCommand(line)
```

`classifyMeetingStatus` table. This mirrors Windows `on_meeting_status`, where LOCKED/UNLOCKED return early and any other status resets ownership and ends the bank unless INMEETING:

| Mac status | Action | Why |
|---|---|---|
| InMeeting(3) | ResetOwnership | Windows resets on INMEETING (`engine-comms.cpp:568-572`) |
| AudioReady(8) | Ignore | A notification while still in the meeting, the Mac counterpart of LOCKED/UNLOCKED. Tearing down would re-request 16 channels. Confirmed live in Task 5. |
| every other value, unknown ints included | ResetAndEndMeeting | Windows rule |

- [ ] **Step 1: Write the failing tests.**
```cpp
TEST(EngineCommsPolicy, StatusTableMatchesTheWindowsRuleOnTheMacEnum) {
  using engine_comms::StatusAction; using engine_comms::classifyMeetingStatus;
  EXPECT_EQ(classifyMeetingStatus(3), StatusAction::ResetOwnership);
  EXPECT_EQ(classifyMeetingStatus(8), StatusAction::Ignore);
  for (int s : {0, 1, 2, 4, 5, 6, 7, 9, 10, 12, 13, 14, 15, 11, 99, -1})
    EXPECT_EQ(classifyMeetingStatus(s), StatusAction::ResetAndEndMeeting) << s;
}
TEST(EngineCommsPolicy, NotJoinedAudioIsMutedAndClosed) {
  EXPECT_TRUE(engine_comms::selfMuted(0));   // None: not joined
  EXPECT_TRUE(engine_comms::selfMuted(1)); EXPECT_TRUE(engine_comms::selfMuted(3)); EXPECT_TRUE(engine_comms::selfMuted(5));
  EXPECT_FALSE(engine_comms::selfMuted(2)); EXPECT_FALSE(engine_comms::selfMuted(4)); EXPECT_FALSE(engine_comms::selfMuted(6));
  EXPECT_FALSE(engine_comms::meetingMicOpen(/*None*/0, /*UnMuted*/2));
  EXPECT_TRUE(engine_comms::meetingMicOpen(/*Voip*/1, 2));
  EXPECT_FALSE(engine_comms::meetingMicOpen(1, 1));
}
TEST(EngineCommsPolicy, RolesAndLicence) {
  EXPECT_FALSE(engine_comms::hostOrCohost(0)); EXPECT_TRUE(engine_comms::hostOrCohost(1));
  EXPECT_TRUE(engine_comms::hostOrCohost(2)); EXPECT_FALSE(engine_comms::hostOrCohost(3));
  EXPECT_TRUE(engine_comms::licensed(0)); EXPECT_FALSE(engine_comms::licensed(1)); EXPECT_FALSE(engine_comms::licensed(12));
}
TEST(EngineCommsPolicy, MicAccessErrorNamesTheSetting) {
  EXPECT_EQ(engine_comms::micAccessError(3), "");                       // Authorized
  EXPECT_EQ(engine_comms::micAccessError(2),                            // Denied
    "Microphone access is off for CoreVideo Pro. Turn it on in System Settings > Privacy & Security > Microphone.");
  EXPECT_EQ(engine_comms::micAccessError(1), engine_comms::micAccessError(2));  // Restricted
  EXPECT_EQ(engine_comms::micAccessError(0), "Waiting for microphone permission.");  // NotDetermined
}
```
  The integer literals are fixture constants copied from the SDK header lines cited above, never from the implementation (rule 2).
- [ ] **Step 2: Write the decorator tests** in `MainQueueTalkbackSdkTest.cpp`, using a recording fake `zc::TalkbackSdk` and a manual queue (a `std::deque<std::function<void()>>` that `post` appends to):
  - `DirectModeForwardsSendOnTheCallerThread`: send `{1,2,3}` on "ch". The fake records it at once with the same thread id, and the queue stays empty.
  - `MainQueueModeCopiesTheFrameAndForwardsOnlyWhenRun`: send a buffer, overwrite the caller's buffer with zeros, then run the queue. The fake receives the original samples `{1,2,3}`, and the call returned `TalkbackCall::Ok` before the queue ran.
  - `NonSendCallsAreNeverHopped`: `InviteUsers`, `CreateChannels` and `SetChannelBackgroundVolume` reach the fake synchronously in both modes.
  - `ShutdownDropsPostedSends`: post 3 sends, call `Shutdown()`, run the queue. The fake gets 0 sends, and `TakeHopStats().droppedAfterShutdown == 3`.
  - `HopStatsMeasureThePostToRunDelay`: with the injected `nowUs` stepping 0 then 4000 µs, `TakeHopStats()` gives `{count:1, maxUs:4000, p99Us:4000}`. A second `TakeHopStats()` gives count 0.
  - `SendModeFromEnv`: `"main"` → MainQueue, `"direct"` → Direct, `nullptr` → the compile-time default. The default is `Direct` until Task 5 decides; Task 5 updates this assertion in the same commit.
- [ ] **Step 3: Write the routing test** in `EngineLineRouteTest.cpp`:
  - `CommsLinesRouteFirstEvenWhenTheyNameAnEngineVerb`: `{"cmd":"comms_mic","device":"join quit leave"}` → Comms.
  - `{"cmd":"join","meeting_id":"1"}` → Engine.
  - `{"cmd":"comms"}` → Engine (no `comms_` prefix).
  - `{"cmd":"quit"}` → Engine.
  - For the comms line, `engine_parse_command(line) == EngineCommand::Unknown`. This is confirmatory (rule 8): it documents that Mac dispatch is by exact cmd, so the hazard Windows guards against cannot occur on Mac. The route order is kept for parity.
- [ ] **Step 4: Run them and watch them fail.** Run `cmake --build native/build-metal --target corevideo-native-tests && native/build-metal/corevideo-native-tests --gtest_filter='EngineCommsPolicy*:MainQueueTalkbackSdk*:EngineLineRoute*'`. Expected: compile failure (the headers do not exist).
- [ ] **Step 5: Implement** the three headers to the signatures above. The decorator's `post` captures a `std::vector<int16_t>` copy and the channel id string. The p99 is computed over a bounded window of the last 512 hop samples.
- [ ] **Step 6: Run them and watch them pass.** Same command. Expected: PASS.
- [ ] **Step 7: Commit.** `git commit -m "feat(mac-engine): pure comms policy for the macOS SDK enums, line routing, main-queue send decorator"`

**Mutations (report each result):**
- Map `AudioReady` to `ResetAndEndMeeting`: `StatusTable…` goes red.
- Make `selfMuted(0)` false: `NotJoinedAudio…` goes red.
- Make `licensed` return `err != 0`: `RolesAndLicence` goes red.
- Forward the caller's pointer instead of a copy in MainQueue mode: `MainQueueModeCopies…` goes red.
- Skip the shutdown flag: `ShutdownDropsPostedSends` goes red.

---

### Task 3: Engine glue skeleton, `available:false` report, idle probe

**Files:**
- Create: `native/zoom-engine/engine/engine-comms-macos.h` (C++-only header), `native/zoom-engine/engine/engine-comms-macos.mm` (`-fobjc-arc`)
- Modify: `native/zoom-engine/engine/main-macos.mm` (reader loop `:2313-2385`, quit block `:2389-2397`, `-onMeetingStatusChange:` `:591`, `handle_join` `:780`, `handle_init` `:322-330`)
- Modify: `native/CMakeLists.txt` (engine Apple sources)
- Create: `scripts/qa/macos-engine-comms-probe.py`

**Interfaces:**
- Consumes: Task 2 headers; `edgecomms::CommsCore`, `CommsStateEmitter`, `CommsBankSink`, `CommsAudioPort` (`comms/comms_core.h:53-230`); `EngineIpc::write`.
- Produces (the same surface as `EngineComms`, `engine-comms.h:33-67`, with Mac types hidden):
```cpp
class EngineCommsMac {
 public:
  static EngineCommsMac& instance();
  void start();                         // main queue; schedules the 100 ms dispatch_source timer
  void set_sdk_ready(bool ready);       // true after initSDKWithParams succeeds; the tick touches no SDK object before
  bool handle_line(const std::string&); // main queue; true for every comms_* line
  void set_capture_only(bool);
  void on_meeting_status(int macStatus);// first statement of -onMeetingStatusChange:
  void shutdown();                      // first statement of the quit block, IPC still open
  static bool owns_mic();
};
```

- [ ] **Step 1: Implement the skeleton.**
  - `start()` creates `CommsCore` with a placeholder `CommsAudioPort`. That port reports `running=false` and empty devices, and Task 4 replaces it. Then it schedules the timer.
  - `tick()` builds `CommsInputs` with `in_meeting = sdk_ready && [svc getMeetingStatus] == InMeeting`. When not in a meeting it does nothing else, then calls `core->Tick(in)` and the emitter.
  - Each timer firing is wrapped in `@try/@catch` plus C++ `try/catch`. Exceptions write `{"cmd":"debug","stage":"comms_tick_exception"}`, matching `engine-comms.cpp:595-606`.
  - `on_meeting_status` applies `classifyMeetingStatus`. In this task `ResetAndEndMeeting` only resets the guard, because there is no bank yet.
  - `handle_line` mirrors `EngineComms::handle_line` (`engine-comms.cpp:624-639`).
- [ ] **Step 2: Wire `main-macos.mm`.**
  - In the reader loop, before `engine_parse_command`: `if (routeEngineLine(line) == EngineLineRoute::Comms) { const std::string msg = line; dispatch_async(main, ^{ EngineCommsMac::instance().handle_line(msg); }); continue; }`.
  - In `main()` before `[NSApp run]`: `dispatch_async(main, ^{ EngineCommsMac::instance().start(); })`.
  - In `handle_init` after `err == ZoomSDKError_Success`: `set_sdk_ready(true)`.
  - In `handle_join`: `set_capture_only(line.find("\"capture_only\":true") != npos)`.
  - In `-onMeetingStatusChange:`, as its first statement: `EngineCommsMac::instance().on_meeting_status(state)`.
  - In the quit block, as its first statement: `EngineCommsMac::instance().shutdown()`.
- [ ] **Step 3: Write the idle probe** `scripts/qa/macos-engine-comms-probe.py`.
  - Model it on `scripts/macos-engine-live-test.py`: the same token, socket, connect and `send`/`recv_events` helpers.
  - Usage: `--engine <bundle binary> idle`, or `--engine <bin> meeting <meeting-id> [passcode] --minutes N --send-mode direct|main`.
  - `idle` mode does no init and no join. It reads E2P lines for 5 s and passes only if at least one `{"cmd":"comms_state",...}` arrives with `"available":false` and a non-empty `reason`, both printed verbatim.
  - It then sends `{"cmd":"comms_key","slot":0,"on":true,"lease_ms":1500}` and passes if a `comms_state` follows within 1 s whose `log` contains `"refused:"`, since `CommsCore::Apply` refuses outside a meeting and logs it.
  - It then sends `{"cmd":"comms_mic","device":"join quit"}` and passes if the engine is still running and emits **no** `joined` or `auth_*` line within 2 s.
  - Finally it sends `{"cmd":"quit"}` and passes if the process exits 0 within 5 s.
- [ ] **Step 4: Build, bundle, probe.** Build the engine as in Task 1 Step 4. Then run `scripts/make-macos-engine-bundle.sh --build-dir native/build-engine --link-sdk` and `python3 scripts/qa/macos-engine-comms-probe.py --engine native/build-engine/corevideo-zoom-engine.app/Contents/MacOS/corevideo-zoom-engine idle`.
  - Expected: four PASS lines. The quoted reason is whatever `comms_core` says outside a meeting; quote it in the report.
  - `nm -C …/corevideo-zoom-engine | grep -c TalkbackSdkMac` may still be 0 because no bank exists yet; report the value.
- [ ] **Step 5: Core relay end to end.** Build `corevideo-native` with `native/build-metal` (the same build `probe-comms.mjs` expects at `native/build-dev`; pass a `--build-dir` flag, added in this step, defaulting to `native/build-dev`). Then run `node scripts/probe-comms.mjs --build-dir native/build-metal`. Expected: all PASS against the fake engine on macOS. This proves the shared relay on Mac, which the spec had not verified.
- [ ] **Step 6: Commit.** `git commit -m "feat(mac-engine): comms glue skeleton reports available:false; comms lines route first; idle probe"`

**Mutations:**
- Move the route check after `engine_parse_command`, so comms lines reach the unknown branch. The probe's refusal check goes red because no `comms_state` follows.
- Delete `shutdown()` from the quit block. Report whether the probe still exits 0. Expected: yes, because there are no threads yet. That makes this mutation **not discriminating until Task 4**; say so in the report (rule 8).

---

### Task 4: Full engine glue on the Mac SDK

**Files:**
- Modify: `native/zoom-engine/engine/engine-comms-macos.mm`
- Modify: `native/CMakeLists.txt` (Apple engine: add `"-framework AVFoundation"`)

**Interfaces:**
- Consumes: `zc::TalkbackSdkMac` (Task 1); `engine_comms::*` and `MainQueueTalkbackSdk` (Task 2); `zc::TalkbackChannels`, `zc::AudioEngine`, `zc::ListCaptureDevices/ListPlaybackDevices`, `edgecomms::MicGuard`.
- Produces: the full `comms_state` (the same JSON `comms_core.cpp:564-621` builds, which is platform-neutral), plus these debug breadcrumbs. Names and fields match Windows where Windows has them:
  - `comms_silent_mic {code}`
  - `comms_silent_mic_removed {code}`
  - `comms_mic_revoked {why, code}`
  - `comms_tick_exception`
  - `comms_audio_shutdown_timeout`
  - **new on Mac:** `comms_audio_join {audioType, audioStatus}`, written on every InMeeting transition and every change of either value
  - **new on Mac:** `comms_send_hop {count, maxUs, p99Us, mode}`, every 10 s while any channel is keyed

Port `engine-comms.cpp` function by function. The policy and ordering are identical; only the SDK calls change, using the boundary table rows 1-18:

| Windows (`engine-comms.cpp`) | Mac |
|---|---|
| `SilentMic` (`:68-81`) | `@interface CVSilentMic : NSObject <ZoomSDKVirtualAudioMicDelegate>` with `std::atomic<bool>` initialised and sending, held strongly by the State struct. `-onMicUninitialized` calls `on_silent_mic_dropped()`. |
| `CommsAudioWin` (`:86-325`) | `CommsAudioMac`: the same worker, copied, with one addition. Before `open_engine`, the worker reads `[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]`. `NotDetermined` calls `requestAccessForMediaType:` once per process and sets `error = micAccessError(0)`. `Denied`/`Restricted` set `error = micAccessError(2)` and skip the open, retrying every 5 s like any open failure. A header comment names `engine-comms.cpp:86-325` as the source: "keep in step; extract once the Windows engine builds in CI". |
| `mute_self` (`:369-382`) | `actionMeetingWithCmd:ActionMeetingCmd_MuteAudio userID:me onScreen:ScreenType_First`, only when `[me getUserID] != 0`. |
| `remove_silent_mic` (`:394-403`) | `[helper setExternalAudioSource:nil]` on the `getRawDataAudioSourceHelper:` helper. |
| `refresh_roster` (`:432-454`) | `getParticipantsList` / `getUserByUserID:`; name, `isHost`, `isSupportTalkback`; skip self. |
| `tick` (`:456-589`) | Same order. `in.host_or_cohost = hostOrCohost([me getUserRole])`. `gi.licence = … && licensed([rdc hasRawDataLicense])`. `gi.self_muted = !me \|\| selfMuted([me getAudioStatus])`. `in.mic_open = me && meetingMicOpen([me getAudioType], [me getAudioStatus])`. Unmute = `ActionMeetingCmd_UnMuteAudio`. The ops-log strings are copied verbatim. |
| adapter creation (`:474-478`) | `zc::TalkbackSdkMac` wrapped in `MainQueueTalkbackSdk(adapter, sendModeFromEnv(getenv("COREVIDEO_COMMS_SEND_MODE")), post = dispatch_async(main), nowUs)`. The **decorator** is what `TalkbackChannels` gets. Retired adapters are parked, never destroyed (`:338-341`). |
| `end_meeting` (`:405-425`) | Same, and also `decorator->Shutdown()` before `DetachBank`. |
| `shutdown` (`:641-659`) | Same, including the bounded 3000 ms audio join and the leak on timeout. |

- [ ] **Step 1: Write the failing pure tests.** The glue itself needs the SDK, so its testable decisions are the Task 2 functions. This task adds one more pure function and its test in `EngineCommsPolicyTest.cpp`:
  - Signature: `std::optional<std::string> engine_comms::audioJoinBreadcrumb(int prevType, int prevStatus, int type, int status, bool enteredMeeting)`.
  - It returns the JSON line when `enteredMeeting` is true or either value changed, and `nullopt` otherwise.
  - Test `AudioJoinBreadcrumbOnlyOnChange`: `(1,2,1,2,false)` → nullopt. `(1,2,1,2,true)` → `{"cmd":"debug","stage":"comms_audio_join","audioType":1,"audioStatus":2}`. `(0,0,1,1,false)` → the line with 1,1.
- [ ] **Step 2: Run it and watch it fail.** `--gtest_filter='EngineCommsPolicy*'`. Expected: FAIL to compile.
- [ ] **Step 3: Implement** `audioJoinBreadcrumb` and the full glue above.
- [ ] **Step 4: Run the tests and watch them pass.** Expected: PASS.
- [ ] **Step 5: Build, bundle, idle probe again.** Expected: the four Task 3 PASS lines. `nm -C … | grep -c 'TalkbackSdkMac::SendAudio'` is now ≥ 1 (the adapter is linked).
- [ ] **Step 6: Mic TCC cannot be proven offline.** The audio worker opens the microphone only while the bank wants audio (`SetRunning(true)`), and that happens only in a meeting. The denied-permission check is therefore **deferred to Task 5's meeting run**. Record "unverified offline" in the report, and do not add a test-only path to force it.
- [ ] **Step 7: Drill.** Run `python3 scripts/mac-show-drill.py --seconds 40 --load 8` before (from `main`) and after, on a quiet machine (load below 2.5). Expected: identical per-frame delivery verdicts. The drill uses the fake engine, so this proves only that the shared core is unaffected. Say so.
- [ ] **Step 8: Commit.** `git commit -m "feat(mac-engine): full comms glue on the macOS SDK (bank, roster, MicGuard, mic TCC gate)"`

**Mutations (each must show in Task 5's live run; list them in the Task 5 brief):**
- Read `None` as unmuted.
- Call `UnMute` before `onMicInitialize`.
- Skip `decorator->Shutdown()` in `end_meeting`.

Offline, only the pure ones are discriminating; report which mutations stayed green offline.

---

### Task 5: Send-threading spike and live engine proof

This is the first time `TalkbackSdkMac` runs against a meeting, on either upstream or Pro. The spike needs the owner and a co-host.

**Files:**
- Modify: `native/zoom-engine/engine/main-queue-talkback-sdk.h` (the default `SendMode`), `native/tests/MainQueueTalkbackSdkTest.cpp` (the default assertion)
- Modify: `scripts/qa/macos-engine-comms-probe.py` (meeting mode)
- Create: `docs/qa/2026-11-xx-mac-comms-send-spike.md` (evidence; date = run date)

- [ ] **Step 1: Meeting mode in the probe.**
  - Init with the public app key, as `macos-engine-live-test.py` does. Join, then print each `comms_state`'s `available`, `reason` and `lamps`, plus each `comms_audio_join` and `meeting_status` line.
  - Wait until `available:true`; the owner makes the engine co-host in Zoom. Then:
    - `comms_tone on`
    - `comms_key slot 0 on lease_ms 1500`, renewed every 500 ms
    - hold for `--minutes`
    - `comms_key slot 0 off`, `comms_tone off`
  - Report `stats.framesSent`, `channelSends`, `underruns` and `sendErrors` at the start and end, plus every `comms_send_hop` line.
  - The engine is started with `COREVIDEO_COMMS_SEND_MODE=<mode>` in its environment.
- [ ] **Step 2: Live run A, direct mode, 10 min.** Participants: the owner (desktop client, guest A, keyed through slot 0 once auto-assign puts A there), the co-host (desktop client, guest B), and the engine. Each guest records their own Zoom audio locally (QuickTime screen recording with audio is enough).
  - Pass:
    - `sendErrors` is flat
    - `underruns` delta ≤ 1 per minute
    - no `comms_tick_exception`
    - the engine process lives
    - the 700 Hz tone is present in A's recording and absent in B's
- [ ] **Step 3: Live run B, main mode, 10 min.** Same pass rule, plus `comms_send_hop` p99 < 20 ms (one 20 ms frame).
- [ ] **Step 4: Decide by measurement.** Ship `Direct` if run A passed. Otherwise ship `MainQueue` if run B passed. If neither passed, **stop**: write the evidence doc and report to the owner before Task 9 continues (spec risk 1). Set the default in `main-queue-talkback-sdk.h`, update `SendModeFromEnv`'s default assertion, and keep the env override as a documented dev switch.
- [ ] **Step 5: The UNVERIFIED items, recorded live in the evidence doc** (each is a quoted line from the probe output):
  - the `meeting_status` sequence across join, audio connect and a deliberate Wi-Fi blip of about 10 s. It must show whether `AudioReady (8)` fires while in the meeting, and that the bank is **not** re-requested on it (no second `requesting 16 talkback channels` in `log`).
  - `comms_audio_join` after join. If `audioType` is 0 (None), MTG MIC stays off and talkback is silent. **Stop and raise owner decision N3.** Do not add `enableAutoJoinVoip:`.
  - Mic TCC: with mic access off for CoreVideo Pro, the settings `mic` block reports the N1 error and `peakDbfs` is null. With access on, `peakDbfs` is a number. Record whether `authorizationStatus` in the engine (a forked child) agreed with the System Settings toggle.
  - The Task 4 mutations: rebuild each mutant, rerun 2 min, and record red or green.
- [ ] **Step 6: Private comms live.** Send `comms_private on`, then `off`.
  - With ON: the engine's meeting mic is muted at once. `log` contains `silent virtual mic initialised` within 5 s, or the 5 s give-up line. The room recording (guest B) has no engine-mic audio while A still hears the tone.
  - With OFF: `log` contains `left MUTED` and `lamps.meetingMic` stays false.
- [ ] **Step 7: Commit** the default plus the evidence doc. `git commit -m "feat(mac-engine): ship <mode> talkback send after the live spike; evidence"`

---

### Task 6: Shared comms fixture, Swift parser, `handleEvent` case, engine-path override

**Files:**
- Create: `contracts/comms/zoom-comms-state.fixtures.json`
- Create: `mac-shell/Sources/CoreVideoProShell/ZoomCommsProtocol.swift`
- Create: `mac-shell/Sources/CoreVideoProShell/ShellTestsComms.swift` (register `commsCases` in `ShellTests.runAndExit`, `ShellTests.swift:~4139`)
- Modify: `mac-shell/Sources/CoreVideoProShell/AppModel.swift:2361-2391` (`handleEvent`), `:6167-6173` (`CoreLaunchEnvironment.extras`)
- Modify: `native-shell/CoreVideoPro.MediaCore.Tests/ZoomCommsProtocolTests.cs` (one fixture test)

**Fixture provenance (rule 1, and "fixtures from real output"):** the file is a JSON object `{"source": "...", "facts": [ ...whole event lines... ]}` with these entries:
1. `desk`: the camelCase desk from `native-shell/CoreVideoPro.WinUI/Services/CommsPageProbe.cs:35-53`, verbatim, wrapped as `{"type":"zoom-comms-state","comms":<desk>}`. This is the Windows shell's own page-probe input.
2. `not-host`: `CommsPageProbe.cs:79`, verbatim.
3. `probe-joined`, `probe-keyed`, `probe-private-on`: three facts **captured** by running `node scripts/probe-comms.mjs --build-dir native/build-metal --capture contracts/comms/captured.jsonl`. `--capture` is a new flag that writes each `zoom-comms-state` line verbatim. Copy the lines from that output without editing. This is the real shared-core output, the same bytes Windows receives.
4. `dropped`: `{"type":"zoom-comms-state","comms":null}`.
5. `response-not-fact`: `{"id":"7","type":"zoom-comms-state","comms":{"available":false}}`.

Invariants the tests rely on:
- `desk.channels.length == 7`, with slots 0-6.
- Roster ids `201`, `202`, `205`; only 205 has `talkback:false`.
- `probe-joined.channels.length == 16`.
- `not-host.reason == "Make this account host or co-host to bring talkback up."`

**Interfaces (Produces):**
```swift
struct CommsChannel: Equatable { var slot: Int; var label: String?; var listeners: Int; var ready: Bool; var keyed: Bool
  var latched: Bool; var room: String?; var reachOk: Int; var reachDark: Int }
struct CommsPerson: Equatable { var participantId: UInt32; var name: String; var talkback: Bool; var room: String?; var channels: [Int] }
struct CommsMic: Equatable { var device: String?; var devices: [String]; var peakDbfs: Double?; var gainDb: Int; var sidetone: Bool
  var sidetoneDevice: String?; var sidetoneDevices: [String]; var tone: Bool }
struct CommsState: Equatable { var available: Bool; var reason: String?; var lampLink, lampMeetingMic, lampChannel, lampTx: Bool
  var room: String?; var channels: [CommsChannel]; var roster: [CommsPerson]; var mic: CommsMic
  var framesSent, channelSends, underruns, sendErrors: Int64; var log: [String]; var isPrivate: Bool }
enum ZoomCommsProtocol {
  static let verbs: Set<String>   // the 10 verbs
  /// nil = not a comms fact (or a response). .some(nil) = "comms":null (report dropped).
  static func parseFact(_ event: JSONObject) -> CommsState??
}
```
The parse rules are `ZoomCommsProtocol.cs:22-77`:
- An `id` means a response, so the event is rejected.
- `comms` must be an object or null.
- Missing fields become false, 0, nil or empty.
- `peakDbfs` stays nil when absent.
- `participantId` is clamped to at least 0.
- Roster `channels` keeps numbers only.

- [ ] **Step 1: Write the failing tests** (`ShellTestsComms.swift`), each reading the fixture from the repo via `#filePath`:
```swift
("comms/protocol-desk-fixture", {
  let s = fixture("desk")!!
  expectEqual(s.channels.count, 7, "seven channels"); expectEqual(s.channels[0].label, "Ana Lucia Ferreira", "label")
  expectEqual(s.mic.peakDbfs, -18, "peak"); expectEqual(s.sendErrors, 2, "send errors")
  expectEqual(s.roster.map(\.participantId), [201, 202, 205], "roster ids"); expect(!s.roster[2].talkback, "Wes is web client")
  expectEqual(s.log.last, "21:40:15 audio flowing into CH 1", "log order kept") }),
("comms/protocol-not-host", { let s = fixture("not-host")!!; expect(!s.available, "unavailable")
  expectEqual(s.reason, "Make this account host or co-host to bring talkback up.", "reason verbatim") }),
("comms/protocol-captured-sixteen-channels", { expectEqual(fixture("probe-joined")!!.channels.count, 16, "16 slots") }),
("comms/protocol-null-is-dropped", { let r = ZoomCommsProtocol.parseFact(event("dropped")); expect(r != nil && r! == nil, "dropped report") }),
("comms/protocol-response-is-not-a-fact", { expect(ZoomCommsProtocol.parseFact(event("response-not-fact")) == nil, "id = response") }),
("comms/protocol-private-flag", { expect(fixture("probe-private-on")!!.isPrivate, "private true") }),
("comms/protocol-verbs", { expectEqual(ZoomCommsProtocol.verbs, ["comms-key","comms-latch","comms-assign","comms-mic",
  "comms-sidetone-out","comms-sidetone","comms-tone","comms-gain","comms-release-all","comms-private"], "ten verbs") }),
("comms/handle-event-forwards-the-fact", {  // fails against today's default: break
  let m = AppModel.forTesting(); m.handleEventForTesting(event("not-host"))
  expectEqual(m.comms.state?.reason, "Make this account host or co-host to bring talkback up.", "fact reached CommsModel") }),
("comms/engine-path-preset-wins", {
  let env = CoreLaunchEnvironment.extras(base: ["COREVIDEO_ZOOM_ENGINE_PATH": "/x/fake"], engineFound: "/y/real")
  expectEqual(env["COREVIDEO_ZOOM_ENGINE_PATH"], "/x/fake", "pre-set engine path wins (Windows run-studio rule)") }),
```
  `m.comms` is a minimal `CommsModel` holding `state`, created here as a stub that Task 7 fills in. If `CoreLaunchEnvironment.extras` has a different signature today, keep its current parameters and add `base:`. The test pins only the rule.
- [ ] **Step 2: Add the C# fixture test** `SharedFixtureParses` in `ZoomCommsProtocolTests.cs`. It reads `contracts/comms/zoom-comms-state.fixtures.json`, passes each fact line through `TryParseStateLine`, and asserts the same counts and strings as the Swift tests. Run `~/.dotnet/dotnet test native-shell/CoreVideoPro.MediaCore.Tests --filter ZoomCommsProtocolTests`. Expected: PASS. This is now the drift guard between the two parsers.
- [ ] **Step 3: Run the Swift tests and watch them fail.** `cd mac-shell && swift build -c release && COREVIDEO_SHELL_TESTS=1 COREVIDEO_SHELL_TESTS_FILTER=comms/ .build/release/CoreVideoProShell`. Expected: FAIL to compile, then FAIL on `handle-event-forwards-the-fact` and `engine-path-preset-wins`.
- [ ] **Step 4: Implement** the parser, the `handleEvent` case `"zoom-comms-state"`, which calls `comms.apply(fact)`, and the env rule.
- [ ] **Step 5: Run the tests and watch them pass.** Same command. Then run the whole suite with `COREVIDEO_SHELL_TESTS=1 .build/release/CoreVideoProShell`. Expected: all PASS.
- [ ] **Step 6: Commit.** `git commit -m "feat(mac-shell): parse zoom-comms-state from a shared fixture; pre-set engine path wins"`

**Mutations:**
- Accept a line with an `id`: `response-is-not-a-fact` goes red.
- Map null to "no fact": `null-is-dropped` goes red.
- In the C# test, change the expected channel count to 6: the C# test goes red (proves the C# test reads the file).

---

### Task 7: `CommsModel`, the pure port of `CommsViewModel`

**Files:**
- Modify: `mac-shell/Sources/CoreVideoProShell/CommsModel.swift` (created as a stub in Task 6)
- Modify: `mac-shell/Sources/CoreVideoProShell/ShellTestsComms.swift`

**Interfaces (Produces):**
```swift
enum CommsTone { case idle, ready, armed, hot, off }
struct CommsCell: Identifiable, Equatable { let slot: Int; var held: Bool; var name: String; var stateText: String
  var tone: CommsTone; var off: Bool; var keyHint: String?; var id: Int { slot } }
struct CommsChip: Equatable { let slot: Int; var text: String; var on: Bool; var busy: Bool; var tip: String }
struct CommsPersonRow: Identifiable, Equatable { let participantId: UInt32; var name: String; var talkback: Bool
  var chips: [CommsChip]; var id: UInt32 { participantId } }
enum CommsPageState: Equatable { case notInMeeting, waiting, unavailable(text: String, hostHint: Bool), desk }
@MainActor final class CommsModel: ObservableObject {
  typealias Send = (_ verb: String, _ payload: JSONObject) async -> String?   // nil = ok, else refusal message verbatim
  init(send: @escaping Send, now: @escaping () -> Int64)
  static let leaseMs = 1500, renewEveryMs = 500, privateResendMs = 2000, meterSegments = 12
  @Published private(set) var state: CommsState?; @Published private(set) var cells: [CommsCell]
  @Published private(set) var people: [CommsPersonRow]; @Published var latchMode = false, editMode = false, settingsOpen = false, opsOpen = false
  @Published private(set) var error: String?
  var pageState: CommsPageState { get }; var railText: String { get }; var allTone: CommsTone { get }; var allText: String { get }
  var emptyDeskText: String? { get }; var opsLine: String { get }; var opsLines: [String] { get }
  var litSegments: Int { get }; var micLevelName: String { get }; var gainText: String { get }
  private(set) var cellStructureVersion: Int; private(set) var peopleStructureVersion: Int
  var privatePreference: Bool { get }; var onPrivatePreferenceChanged: ((Bool) -> Void)?
  func restorePrivatePreference(_ on: Bool)
  func applyMeeting(_ inMeeting: Bool, label: String?); func apply(_ fact: CommsState??)
  func press(slot: Int) async; func release(slot: Int) async; func allPress() async; func allRelease() async
  var anyHeld: Bool { get }; func renewLeases() async; func releaseAll() async; func sendReleaseAll() async
  func cell(forDigit digit: Int) -> CommsCell?
  func assign(slot: Int, participantId: UInt32, on: Bool) async
  func selectMic(_ device: String) async; func selectOutput(_ device: String) async
  func toggleSidetone() async; func toggleTone() async; func gainDown() async; func gainUp() async; func togglePrivate() async
  static func meetingLabel(for joinText: String) -> String?
}
```
Every rule and string comes from `CommsViewModel.cs`. The line numbers below are the executor's checklist:
- page states and copy: `:132-155`
- lamps: `:157-163`
- ALL CALL: `:167-171`
- private send: `:253-265`
- private reconcile: `:270-275`
- `applyMeeting(false)` and `apply(nil)` clear local holds **only** and send nothing (`:318,:326`). This matches Windows code, not the doc.
- cells, structure signature and tone/StateText priority: `:375-415`
- people/chips signature: `:417-458`
- device lists rebuild only on change: `:462-474`
- ops newest-first: `:478-494`
- meter -48..0 over 12 segments: `:497-501`
- key verbs and payloads: `:505-561`
- gain clamp: `:586-591`

The `send` closure gets payloads as dictionaries. Tests compare them with sorted-key JSON, as `CommsViewModelTests` does.

- [ ] **Step 1: Write the failing tests.** Port every `CommsViewModelTests` case (`native-shell/CoreVideoPro.WinUI.Tests/CommsViewModelTests.cs`) one-for-one, with the same literals.
  - The fixtures are the C# `Desk(...)` and `Channel(...)` helpers (`:39-44`), ported as Swift helpers with the same defaults: mic `"Mic A"` with devices `["Mic A","Mic B"]`, peak -24, sidetone `"Phones"` with devices `["Phones","Speakers"]`, log `["a","b"]`.
  - The recording `send` appends `(verb, sortedJSON(payload))`. The clock is a `var t: Int64` the test advances.
  - Name each case `comms/model-<csharp-name-kebab>`. The cases that pin the rules this plan's Review Focus depends on:
```swift
("comms/model-holding-a-cell-keys-with-a-lease", {
  m.apply(desk([ch(0, label: "Ana", listeners: 1)])); await m.press(slot: 0); await m.press(slot: 0)
  await m.release(slot: 0); await m.release(slot: 0)
  expectEqual(sent, [("comms-key", #"{"leaseMs":1500,"on":true,"slot":0}"#), ("comms-key", #"{"on":false,"slot":0}"#)], "deduped") }),
("comms/model-on-air-comes-from-the-engine-only", {
  await m.press(slot: 0); expectEqual(m.cells[0].tone, .ready, "pressed alone is not on air"); expectEqual(m.cells[0].stateText, "ready", "")
  m.apply(desk([ch(0, label: "Ana", listeners: 1, keyed: true)])); expectEqual(m.cells[0].tone, .hot, ""); expectEqual(m.cells[0].stateText, "on air", "")
  m.apply(desk([ch(0, label: "Ana", listeners: 0, keyed: true)])); expectEqual(m.cells[0].tone, .armed, ""); expectEqual(m.cells[0].stateText, "nobody in channel", "") }),
("comms/model-same-cells-never-rebuild", {
  m.apply(d1); let v = m.cellStructureVersion; await m.press(slot: 0); m.apply(d1WithNewMeterAndStats)
  expectEqual(m.cellStructureVersion, v, "no rebuild"); expect(m.cells[0].held, "hold survives") }),
("comms/model-private-reasserted-once-per-2s", {
  m.applyMeeting(true, label: nil); m.restorePrivatePreference(true)
  for step in 0..<10 { t = Int64(step * 250); m.apply(desk(isPrivate: false)) }      // 0..2250 ms of disagreeing reports
  expectEqual(sent.filter { $0.0 == "comms-private" }.count, 2, "at t=0 and t=2000 only")
  m.apply(desk(isPrivate: true)); t += 5000; m.apply(desk(isPrivate: true))
  expectEqual(sent.filter { $0.0 == "comms-private" }.count, 2, "quiet once they agree")
  expect(m.railText.hasSuffix("· private comms"), "rail says private") }),
("comms/model-leaving-forgets-holds-locally-and-sends-nothing", {
  await m.press(slot: 0); sent.removeAll(); m.applyMeeting(false, label: nil)
  expect(!m.anyHeld, "local holds cleared"); expect(sent.isEmpty, "Windows sends nothing here (CommsViewModel.cs:318)") }),
("comms/model-null-report-forgets-holds-locally", {
  await m.press(slot: 0); sent.removeAll(); m.apply(.some(nil))
  expect(!m.anyHeld, ""); expect(sent.isEmpty, ""); expectEqual(m.pageState, .waiting, "") }),
```
  The remaining ports, with their C# literals:
  - `screen-states-are-explicit` (`"Not in a meeting"`, `"Meeting 123 456 7890"`, the host hint)
  - `only-occupied-named-or-keyed-channels-are-cells` (slots [0,2], `"CH 3"`)
  - `latch-mode-toggles` (`"on air · latched"`)
  - `all-call-keys-every-channel-and-latches-all` (`"ALL CALL · ON AIR"`)
  - `structure-change-keeps-surviving-cells-and-hold`
  - `release-all-releases-every-held-key`
  - `held-key-renews-lease-released-does-not`
  - `latched-channels-are-not-leased`
  - `shutdown-sends-release-all` (`("comms-release-all","{}")`)
  - `someone-in-another-room-cannot-be-keyed` (`"in Green room"`, digit 1 → next cell)
  - `group-says-how-many-listen-and-who-is-elsewhere` (`"3 listening · 2 elsewhere"`, `"CH 5"`)
  - `edit-talent-chips-assign-and-explain-web-clients` (`"Take off Ana"`, `"Put on CH 2 (empty)"`, `{"on":true,"participantId":201,"slot":1}`)
  - `people-rows-rebuilt-only-when-roster-changes`
  - `settings-send-their-verbs` (`{"device":"Mic B"}`, `{"device":"Speakers"}`, `{"on":false}`, `{"on":true}`, `{"db":1}`, `{"db":-1}`)
  - `applying-state-never-sends-a-device-change`
  - `refusal-is-shown-verbatim` (`"Comms is not available: Not in a meeting."`, then cleared on success)
  - `meter-lights-only-from-a-measured-level` (-24 → 6 lit; nil → 0 lit, with `"Microphone level not measured"`)
  - `ops-history-newest-first` (`"b"`, `["b","a"]`)
  - `private-off-by-default-nothing-sent`
  - `toggling-private-sends-and-asks-to-persist` (saved `[true,false]`)
  - `outside-a-meeting-toggle-only-records`
  - `meeting-label` (`zoom.us/j/1234567890?pwd=abc` → `"Meeting 123 456 7890"`; 11 digits → `"Meeting 123 4567 8901"`; `""` → nil; `/my/someone` → nil)
  - `gain-clamps` (at 20, gainUp sends nothing)
- [ ] **Step 2: Run them and watch them fail.** `COREVIDEO_SHELL_TESTS_FILTER=comms/model`. Expected: FAIL.
- [ ] **Step 3: Implement `CommsModel`.** Sync the collections in place, keyed by slot or participant id. Never replace an element whose key survives (`ObservableCollectionSync` semantics).
- [ ] **Step 4: Run them and watch them pass.** Expected: all `comms/model-*` PASS.
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-shell): CommsModel, a one-for-one port of CommsViewModel with its tests"`

**Mutations (report each):**
- Make a press set `.hot` locally: `on-air-comes-from-the-engine-only` goes red.
- Rebuild cells on every apply: `same-cells-never-rebuild` goes red.
- Drop the 2000 ms throttle: `private-reasserted-once-per-2s` goes red (count 10).
- Send a release on `applyMeeting(false)`: `leaving-forgets…` goes red.
- Lease latched cells: `latched-channels-are-not-leased` goes red.

---

### Task 8: Wiring (gate, tab, prefs, lease timer, meeting state, restart) and a minimal page

**Files:**
- Modify: `AppModel.swift:11-54` (StudioTab), `:492-495` (selectedTab), `:1771` (`onConnected`), `:2066-2069` (meeting state), `:802`/`:877` (prefs)
- Modify: `ShellPrefs.swift:105-196`, `:213-284`, `:292-372` (field, decode, self-check)
- Modify: `TabPageLayout.swift:16-25,44-56`, `ShellApp.swift:473`
- Create: `mac-shell/Sources/CoreVideoProShell/CommsPage.swift` (minimal: rail and page states only)
- Modify: `ShellTests.swift:3563-3580` (nav test), `ShellTestsComms.swift`

**Interfaces:**
- Produces:
  - `StudioTab.comms = "Comms"`
  - `static func navigation(commsEnabled: Bool) -> [StudioTab]`, with comms placed **between `.audio` and `.media`** (`StudioWorkspace.xaml:218-229`). The old `static let navigation` becomes `navigation(commsEnabled: CommsGate.enabled)`.
  - `enum CommsGate { static var enabled: Bool }`, read from `ProcessInfo` `COREVIDEO_SHELL_COMMS == "1"` until Task 11.
  - `ShellPrefs.commsPrivate: Bool?` (absent = false)
  - `AppModel.comms: CommsModel`, sending through `gradeRequest`. The core's reply `error.message` arrives as `BridgeError.remote(message)`, which maps to the refusal string.
  - `AppModel.commsLeaseTick(nowMs:)`, driven by a 500 ms `Timer` that is always running (Windows: `StudioViewModel.Comms.cs:36-43`) and sends only when `anyHeld`.

- [ ] **Step 1: Write the failing tests.**
```swift
("comms/nav-hidden-until-gate", { expect(!StudioTab.navigation(commsEnabled: false).contains(.comms), "gated off")
  expectEqual(StudioTab.navigation(commsEnabled: true).map(\.navLabel),
    ["Studio","Zoom","Sources","Scenes","Routing","Graphics","Audio","Comms","Media","Health","Settings"], "Windows order") }),
("comms/tab-name-resolves", { expectEqual(StudioTab(name: "comms"), .comms, "") }),
("comms/prefs-absent-means-off", { let p = try! JSONDecoder().decode(ShellPrefs.self, from: Data("{}".utf8))
  expect(p.commsPrivate != true, "absent = off"); var q = p; q.commsPrivate = true
  expectEqual(try! JSONDecoder().decode(ShellPrefs.self, from: JSONEncoder().encode(q)).commsPrivate, true, "round trip") }),
("comms/private-pref-restored-and-saved", { let m = AppModel.forTesting(prefs: prefs(commsPrivate: true))
  expect(m.comms.privatePreference, "restored"); await m.comms.togglePrivate(); expectEqual(m.currentPrefs().commsPrivate, false, "saved") }),
("comms/lease-tick-cadence", {   // injected time, no wall clock
  let m = AppModel.forTesting(); m.comms.apply(desk1); m.applyMeetingForTesting("in_meeting"); await m.comms.press(slot: 0); core.clear()
  for ms in stride(from: 0, through: 1500, by: 500) { await m.commsLeaseTick(nowMs: Int64(ms)) }
  expectEqual(core.verbs.filter { $0 == "comms-key" }.count, 4, "renew at 0,500,1000,1500")
  await m.comms.release(slot: 0); core.clear(); await m.commsLeaseTick(nowMs: 2000); expect(core.verbs.isEmpty, "nothing held, nothing sent") }),
("comms/meeting-state-feeds-the-model", { let m = AppModel.forTesting(); m.applyMeetingForTesting("in_meeting")
  expectEqual(m.comms.pageState, .waiting, ""); m.applyMeetingForTesting("idle"); expectEqual(m.comms.pageState, .notInMeeting, "") }),
("comms/core-restart-reasserts-private-and-drops-holds", {
  let m = AppModel.forTesting(prefs: prefs(commsPrivate: true)); m.applyMeetingForTesting("in_meeting")
  m.comms.apply(desk1(isPrivate: true)); await m.comms.press(slot: 0); core.clear()
  m.simulateCoreRestartForTesting()                 // .exited then .connected, as handleBridgeStatus does
  expect(!m.comms.anyHeld, "holds dropped on the core generation change")
  m.comms.apply(.some(nil)); m.comms.apply(desk1(isPrivate: false))  // new engine reports private:false
  expectEqual(core.sent.filter { $0.verb == "comms-private" }.map(\.payload), [#"{"on":true}"#], "re-asserted once") }),
("comms/minimal-page-states", { /* CommsPage renders notInMeeting / waiting / unavailable copy; read via the view model strings */ }),
```
  Update `testNavigationMatchesWindowsOrderAndLabels` to call `navigation(commsEnabled: false)`, keeping the existing assertion. The new `nav-hidden-until-gate` pins the enabled order.
- [ ] **Step 2: Run them and watch them fail.** Filter `comms/`. Expected: FAIL.
- [ ] **Step 3: Implement.**
  - `onConnected()` calls `comms.coreGenerationChanged()`, which clears local holds and the state. That is the "every core generation" rule (`AppModel.swift:1177-1181`). Private is re-sent by the report-disagrees rule; nothing re-sends it blindly.
  - `TabPageContent` gains `.comms: CommsPage(model: model.comms)`. `maxWidth(.comms) = .infinity`, matching the Windows Border margin 12 and padding 20 with no cap.
  - The minimal `CommsPage` shows the rail and the four state blocks, with Windows copy:
    - `"Not in a meeting"` / `"Join from the Zoom page. Talkback comes up once this account is in the meeting as host or co-host."`
    - `"Waiting for the Zoom engine's talkback report…"`
    - `"Talkback is not up"` + reason + `"Ask the meeting host to make this account host or co-host. Video keeps working meanwhile."`
- [ ] **Step 4: Run them and watch them pass,** then run the full suite. Expected: PASS, including the updated nav test.
- [ ] **Step 5: Fake-engine run.**
  - Command: `COREVIDEO_SHELL_COMMS=1 COREVIDEO_ZOOM_ENGINE_PATH=$PWD/native/build-metal/corevideo-zoom-engine-fake COREVIDEO_FAKE_ENGINE_PARTICIPANTS=4 COREVIDEO_SHELL_TAB=comms COREVIDEO_SHELL_SNAPSHOT=/tmp/comms-waiting.png mac-shell/.build/release/CoreVideoProShell`. Rebuild the fake first; the local one is from Aug 29.
  - Expected: the PNG shows "Not in a meeting". Join through the Zoom tab's fake join, then re-shoot: the page shows the desk state with lamps.
  - This proves the engine-path override from Task 6.
- [ ] **Step 6: Commit.** `git commit -m "feat(mac-shell): gated Comms tab, Private pref, lease timer, restart rule, minimal page"`

**Mutations:**
- Remove `comms.coreGenerationChanged()` from `onConnected`: `core-restart-…` goes red ("holds dropped").
- Send renewals unconditionally: `lease-tick-cadence` goes red.

---

### Task 9: The full Comms page in Windows' layout, `TalkKey`, snapshots

**Files:**
- Modify: `mac-shell/Sources/CoreVideoProShell/CommsPage.swift`
- Create: `mac-shell/Sources/CoreVideoProShell/TalkKey.swift`
- Modify: `TabPageLayout.swift` (outer scroll opt-out), `ShellApp.swift:1286-1294` (`TabPage`)
- Create: `mac-shell/Sources/CoreVideoProShell/ShellTestsCommsPage.swift`

**Layout contract.** This copies the **structure** of `CommsPage.xaml:51-369`, not only its controls. If the Graphics G4-G7 lane has already landed a "page owns its scrolling" mechanism, reuse it instead of adding a second one.
- `TabPageLayout.ownsScrolling(.comms) == true`. `TabPage` skips its outer `ScrollView` for such tabs, and only the desk scrolls.
- Root is a `VStack(spacing: 0)` of five regions, matching the XAML rows `Auto / * / Auto / Auto / Auto`:
  1. **Rail** (fixed): an HStack with `"Comms"` (section title), the rail text (mono, secondary, truncating), and four lamps on the right. Each lamp is an 8 pt dot plus a mono tag, spaced 16, labelled `LINK`, `MTG MIC`, `CHANNEL`, `TX`, with the XAML tooltips as `.help`. There is a bottom divider.
  2. **Desk** (fills the remaining height, `ScrollView(.vertical)`), padding `0,16,8,16`, spacing 12, in this order:
     - the state blocks from Task 8
     - ALL CALL: a full-width `TalkKey`, min height 56, mono 13, `allText`, help `"Hold to talk to everyone on a channel (Space)"`
     - the empty-desk text
     - the cell grid: `LazyVGrid(columns: [GridItem(.adaptive(minimum: 200), spacing: 8)], spacing: 8)`, with each cell a `TalkKey` of min height 72, corner radius 8, border 2, padding 14/12. Inside each cell: name (14 semibold, truncating) at top left, key hint (mono tag) at top right, state text (mono 11) below.
     - edit talent: a bordered section headed `"TALENT"` / `"CHANNEL: CLICK A NUMBER TO PUT SOMEONE ON IT"`, one row per person, and on the right a 560 pt wide adaptive grid of 32×32 chips (spacing 4, mono 11, corner radius 6, `.help(tip)`). A person without talkback shows the tag `"NO TALKBACK"` and, below the name, `"On the Zoom web client. Ask them to rejoin in the Zoom desktop app."`
     - the hint line: `"Hold a cell to talk · 1–9 key the first nine · Space = ALL CALL · LATCH makes presses stick"`
  3. **Settings drawer** (fixed, shown when `settingsOpen`): four columns, the first two flexible and the last two fitting their content.
     - `"MICROPHONE"` picker
     - `"SIDETONE OUTPUT"` picker
     - `"PROCESSING"`: a `Sidetone` toggle (help `"Hear your own voice in the sidetone output"`), a `Tone` toggle (help `"Replace the microphone with a 700 Hz tone through the whole live chain"`), and `−` / gain text (min width 56) / `+`
     - `"STATION"` (min width 200): Frames sent, Channel sends, Underruns, Send errors. Send errors uses `Studio.amber` when above 0.
     - The mic error string from the report (Task 4, N1) shows under the picker in `Studio.amber`.
  4. **Strip** (fixed, shown when the desk is visible): on the left, `Latch`, `Edit talent`, `Settings`, `Private comms` (help = private hint) and `Leave` (help `"Leave the meeting (video too)"`, the existing leave action). On the right, `"MIC"` and a 12-segment meter, 6×16 pt with spacing 2; unlit segments use opacity 0.18.
  5. **Ops strip** (fixed): a full-width ghost button, min height 32, with the error line in `Studio.red` and the ops line (mono 11, secondary). When open it shows a `ScrollView` of max height 180 listing the ops lines.

Tone to colour, from `CommsBrushConverter.cs:35-79` mapped onto `Studio` tokens:

| Tone | Background | Border | Text |
|---|---|---|---|
| hot | `red` | `red` | `background` |
| armed | `surface` | `amber` | `amber` |
| ready | `surface` | `border` | `textPrimary` |
| idle | `surface` | `border` | `secondary` |
| off | `field` | `line2` | `textDim` |

Other parts:
- meter: hot `red`, armed `amber`, otherwise `accent`
- lamp on: `accent`; lamp off: `textDim`
- toggle on: border `accent`, background `surfaceRaised`
- chip on: background `accent`, text `background`
- busy chip: border `amber`

**`TalkKey` interface:**
```swift
struct TalkKey<Label: View>: NSViewRepresentable {
  init(enabled: Bool, onPress: @escaping () -> Void, onRelease: @escaping () -> Void, @ViewBuilder label: () -> Label)
}
// Backing NSView (TalkKeyView): mouseDown → press (left button only, ignored when disabled or already down);
// mouseUp → release (AppKit routes the up to the view that took the down = pointer capture);
// viewDidMoveToWindow with window == nil, and NSWindow.didResignKeyNotification for its window → release if down;
// acceptsFirstResponder false (Space/Enter go to the page); never an NSButton or SwiftUI Button.
```

- [ ] **Step 1: Write the failing tests.**
  - `comms/talkkey-press-release-once`: send a synthetic `mouseDown` twice, then `mouseUp` twice, to `TalkKeyView`. Expect exactly 1 press and 1 release.
  - `comms/talkkey-releases-when-removed-from-window`: add the view to a window, send `mouseDown`, then `removeFromSuperview()`. Expect 1 release.
  - `comms/talkkey-releases-on-window-resign-key`: post `didResignKeyNotification` for its window while down. Expect 1 release.
  - `comms/talkkey-disabled-ignores`: a disabled key that gets `mouseDown` produces no press.
  - `comms/page-owns-its-scrolling`: `TabPageLayout.ownsScrolling(.comms)` is true and `ownsScrolling(.audio)` is false. Then render `TabPage` with `.comms` offscreen at 1360×768 and assert the ops strip's frame bottom is within 12 pt of the page bottom with 30 cells, which overflows the desk. Measure through `.background(GeometryReader)` preference keys. This drives the structure (rule 10).
  - `comms/grid-fits-six-200pt-columns`: at a 1360 pt window, the grid lays out 6 columns, each at least 190 pt wide. This is the Windows probe check (`CommsPageProbe`).
  - `comms/tone-colours-use-studio-tokens`: for each tone, the colour triple equals the table above, compared against `Studio.*` constants.
- [ ] **Step 2: Run them and watch them fail.** Expected: FAIL.
- [ ] **Step 3: Implement** the page, `TalkKey`, and the opt-out.
- [ ] **Step 4: Run them and watch them pass.** Expected: PASS.
- [ ] **Step 5: Snapshot proofs.** Add the seeded state `COREVIDEO_SHELL_COMMS_SEED=<name>`. It loads the fixture fact `<name>` from `contracts/comms/zoom-comms-state.fixtures.json`; the harness only reads it when the gate is on. Use `COREVIDEO_SHELL_TAB_SNAPSHOT=/tmp/comms` with `COREVIDEO_SHELL_TAB_LIST=Comms` to render:
  - `not-in-meeting`
  - `waiting`
  - `not-host`
  - `desk`, which has on-air slot 0, armed slot 2, unreachable slot 3 and not-ready slot 5
  - `desk` with edit talent open
  - `desk` with settings open

  Expected: six PNGs. **Put them side by side with the Windows `--verify-comms-page` captures if the owner can produce them in one Windows session; otherwise compare against the XAML description above and say so.** Check that the strip and ops stay pinned at the bottom in every image.
- [ ] **Step 6: Commit.** `git commit -m "feat(mac-shell): Comms page in the Windows five-row layout; TalkKey press/release"`

**Mutations:**
- Wrap Comms in the outer `ScrollView` again: `page-owns-its-scrolling` goes red.
- Make `TalkKeyView` act on `mouseUp` only: `press-release-once` goes red.

---

### Task 10: Release triggers and panel-scoped keys

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/CommsKeys.swift`
- Modify: `AppModel.swift` (selectedTab `didSet`, a lifecycle observer, the quit path `:5589-5700`), `ShellApp.swift` (main window resign key), `ShellTestsCommsPage.swift`

**Interfaces (Produces):**
```swift
struct CommsKeyEvent: Equatable { enum Kind: Equatable { case space, digit(Int), other }
  var kind: Kind; var down: Bool; var modifiers: TransportKeyEvent.Modifiers; var isRepeat: Bool
  var isCommsTab: Bool; var deskVisible: Bool; var isTextEntryFocus: Bool }
enum CommsKeyDecision: Equatable { case ignore, swallow, allPress, allRelease, press(slot: Int), release(slot: Int) }
struct CommsKeys { mutating func decide(_ e: CommsKeyEvent, cellForDigit: (Int) -> Int?) -> CommsKeyDecision }  // holds the _down set
final class CommsKeyMonitor { func setActive(_ on: Bool, handle: @escaping (NSEvent) -> NSEvent?) }      // TransportHotkeyMonitor pattern
```
Rules, from `CommsPage.xaml.cs:91-159`:
- Active only on the Comms tab, while the desk is visible and the main window is key.
- Ignored for any modifier (command, option, control or shift) and while a text field is first responder (`TransportKeys.isTextEntry`).
- Digits are 1-9, from both the main row and the keypad.
- An auto-repeat is swallowed.
- A key-up releases only a key whose down this set recorded.
- Space on Comms never reaches Take. Take is Studio and Graphics only (`AppModel.swift:3810-3821`), and the test pins that.

Release triggers: each one calls `comms.releaseAll()` when `anyHeld`, and clears the key set:

| Trigger | Mac hook | Windows |
|---|---|---|
| leaving the Comms tab | `selectedTab` `didSet` where old == `.comms` | `StudioViewModel.Comms.cs:57-60` |
| app resigns active | `NSApplication.didResignActiveNotification` | `MainWindow.xaml.cs:675-676` |
| main window resigns key | `NSWindow.didResignKeyNotification` for `MainWindowTracker`'s window | same |
| quit | `comms.sendReleaseAll()`, bounded to 350 ms, as the first step of both the `.quitNow` path of `handleQuitRequest` and `confirmQuit`, before `controlApi.stop()` and `bridge.stopForAppExit` | `StudioViewModel.SyncOwnership.cs:22` (`ShutdownComms` first) |
| meeting leaves in-meeting / null report | local only (Task 7) | `CommsViewModel.cs:318,326` |

- [ ] **Step 1: Write the failing tests.**
```swift
("comms/keys-digit-press-release", { var k = CommsKeys()
  expectEqual(k.decide(ev(.digit(1), down: true), cellForDigit: { _ in 0 }), .press(slot: 0), "")
  expectEqual(k.decide(ev(.digit(1), down: true, isRepeat: true), cellForDigit: { _ in 0 }), .swallow, "no re-toggle on repeat")
  expectEqual(k.decide(ev(.digit(1), down: false), cellForDigit: { _ in 0 }), .release(slot: 0), "") }),
("comms/keys-ignored-with-modifier-text-field-or-other-tab", { var k = CommsKeys()
  expectEqual(k.decide(ev(.space, down: true, modifiers: .command), cellForDigit: { _ in 0 }), .ignore, "")
  expectEqual(k.decide(ev(.space, down: true, isTextEntryFocus: true), cellForDigit: { _ in 0 }), .ignore, "")
  expectEqual(k.decide(ev(.space, down: true, isCommsTab: false), cellForDigit: { _ in 0 }), .ignore, "") }),
("comms/keys-up-without-recorded-down-does-nothing", { var k = CommsKeys()
  expectEqual(k.decide(ev(.space, down: false), cellForDigit: { _ in 0 }), .ignore, "") }),
("comms/space-on-comms-is-never-take", { let m = AppModel.forTesting(); m.selectedTab = .comms; m.syncTransportHotkeys()
  expect(!m.transportHotkeysActiveForTesting, "Take monitor off on Comms") }),
("comms/release-on-tab-change", { /* hold slot 0 on .comms, set selectedTab = .audio → core saw comms-key {"on":false,"slot":0} */ }),
("comms/release-on-app-resign-active", { /* hold; post NSApplication.didResignActiveNotification → release sent */ }),
("comms/release-on-main-window-resign-key", { /* hold; post didResignKey for the tracked main window → release sent */ }),
("comms/quit-sends-release-all-before-core-stop", {
  /* quitRequestForTesting records order: expect ["comms-release-all", …, "stop"] with release-all first */ }),
```
  Each `/* … */` case uses the existing `AppModel.forTesting()` and recording-core seams (`quitRequestForTesting`, `AppModel.swift:649`). Each asserts the exact payloads from the table, with no extra sends.
- [ ] **Step 2: Run them and watch them fail.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and watch them pass,** then run the full suite. Expected: PASS.
- [ ] **Step 5: Fake-engine end to end.**
  - Hold `1` on the Comms tab. `probe-joined`'s fact shows slot 0 `keyed:true`.
  - Cmd-Tab away. Within one fact (≤ 1 s), slot 0 shows `keyed:false`.
  - `kill -STOP` the shell while holding a key. Within 1.5 s plus one fact, the core's fact shows `keyed:false`. That is the lease working with a hung shell.
  - Record the three fact lines.
- [ ] **Step 6: Commit.** `git commit -m "feat(mac-shell): Comms release triggers and panel-scoped digit/Space keys"`

**Mutations:**
- Remove the resign-active observer: `release-on-app-resign-active` goes red.
- Let a repeat re-press: `keys-digit-press-release` goes red.
- Move release-all after `stopForAppExit`: the quit-order case goes red.

---

### Task 11: Live co-host acceptance, remove the gate, docs, outcomes

**Files:**
- Modify: `AppModel.swift` (`CommsGate.enabled = true`; delete the env read), `AppModel.swift:24-29` (comment), `ShellTests.swift:3561-3580` (nav test now includes Comms)
- Modify: `docs/reference/comms-talkback.md` (a macOS section: engine glue file, send mode chosen, Mac SDK differences rows 8/10/12/13/15, the TCC note, `COREVIDEO_COMMS_SEND_MODE`, `COREVIDEO_SHELL_COMMS_SEED`)
- Modify (owner decision N2 only): `scripts/make-macos-shell-bundle.sh:133`
- Create: `docs/superpowers/plans/2026-11-22-mac-comms-social-phase4-outcomes.md` (the Comms half)

- [ ] **Step 1: Build and install a signed build** with Tasks 1-10. Run `scripts/release-macos.sh` per `docs/reference/beta-release-runbook.md` / memory `corevideo-release-process`; the owner installs. Run `python3 scripts/mac-show-drill.py --seconds 40 --load 8` before and after on a quiet machine (N6).
- [ ] **Step 2: Owner + co-host checklist** (spec section 5, A5), with two desktop-client guests and one web-client guest, on the installed build, launched with `COREVIDEO_SHELL_COMMS=1`. Record each item as PASS or FAIL with evidence:
  1. Page states: not in a meeting → waiting → (engine not co-host) "Talkback is not up" with the host hint → (owner grants co-host) desk.
  2. Key guest A by holding a cell. Guest A's local recording has the operator's voice and guest B's does not. The cell reads "on air".
  3. ALL CALL reaches both desktop guests.
  4. LATCH toggles stick across press and release.
  5. With a key held, force-quit the shell (`kill -9`). Guest A stops hearing the operator within 1.5 s; a stopwatch against A's recording is enough.
  6. With a key held, switching apps or changing tabs drops it at once.
  7. The web-client guest shows "NO TALKBACK" in Edit talent. Assigning them is refused with the core's verbatim text.
  8. Private ON: the operator's voice is absent from the meeting recording (cloud or local host recording) and present at the keyed guest. Private ON → OFF: the operator stays muted, and the rail and ops say so.
  9. Video, ISO recording and Program recording run throughout; the existing `iso-pixel-verdict.py` on a 5-minute recording made during the session passes. Hold a key for 60 s and confirm `realtimeEvidence` shows zero new underruns and zero missed deadlines, and that `zoomSubscriptionChurn` does not move.
  10. Microphone permission off (System Settings): the settings drawer shows the N1 text and the meter is unlit.
- [ ] **Step 3: If every item passes,** set `CommsGate.enabled = true` and update the nav test to the 11-item order from Task 8. Run the full shell suite. If any item fails, **do not** enable the tab. File each failure as a `backlog` issue and report to the owner.
- [ ] **Step 4: Docs and outcomes.** Write the reference section and the outcomes doc: what passed and how, the send mode, every UNVERIFIED item and its answer, mutation results per task, and deviations from this plan.
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-shell): Comms tab on by default after the live co-host acceptance; docs"`

---

### Task 12: `SocialReview` model

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/SocialReview.swift`
- Create: `mac-shell/Sources/CoreVideoProShell/ShellTestsSocial.swift` (register `socialCases`)

**Interfaces (Produces):**
```swift
enum SocialDecision { case pending, approved, rejected }
struct SocialItemKey: Hashable { let source: Int64; let id: String; let revision: Int64 }
struct SocialItem: Equatable { let key: SocialItemKey; let author: String; let message: String; let receivedAt: Date
  let decision: SocialDecision; let provider: String; let authorId: String; let order: Int64 }
enum SocialFeedEvent: Equatable { case text(id: String, author: String, message: String, authorId: String, order: Int64)
  case remove(id: String), ban(authorId: String), clearRoom(order: Int64), clearUser(authorId: String, order: Int64) }
final class SocialReview {
  init(now: @escaping () -> Date, capacity: Int = 200, removalCapacity: Int = 4096, lifetime: TimeInterval = 15 * 60)
  var isConnected: Bool { get }; var items: [SocialItem] { get }   // expires first, newest revision first
  func connect(provider: String) -> Int64    // "YouTube" | "Twitch"; else precondition failure (Windows throws)
  func disconnect()
  @discardableResult func receive(source: Int64, id: String, author: String, message: String, authorId: String = "", order: Int64 = 0) -> SocialItem?
  func withdraw(source: Int64, id: String); @discardableResult func withdraw(_ key: SocialItemKey) -> Bool
  func approve(_ key: SocialItemKey) -> Bool; func reject(_ key: SocialItemKey) -> Bool; func approved(_ key: SocialItemKey) -> SocialItem?
  func apply(source: Int64, _ event: SocialFeedEvent) -> Bool; func isCurrent(_ source: Int64) -> Bool
}
```
Every rule is in `GraphicsSocialReview.cs`:
- Ids are 1-256 printable ASCII `!`..`~`.
- Text is rejected if its UTF-16 length is over the limit or it contains a control character. Otherwise it is trimmed and NFC-normalised; the result must be non-empty and at most 80 (author) or 192 (message) UTF-8 bytes.
- A full queue refuses new ids. Nothing is evicted.
- An identical replay keeps the same key and decision. A changed payload gets a new revision, decision pending, and keeps the original `receivedAt`.
- A tombstone blocks replays.
- The fences are room clear, per-author clear, and a session ban.
- When the removal budget is exceeded, the queue disconnects (fails closed).
- `disconnect()` resets everything except the source counter.

- [ ] **Step 1: Write the failing tests.** Port the five `GraphicsSocialReviewTests` cases with the fixed clock `2026-11-04T18:00:00Z`:
  - `social/review-approval-applies-only-to-exact-revision` (`"first"`, `"other"`, `"Edited message"`)
  - `social/review-removal-before-and-after-approval-fences-replays` (`"early"`, `"shown"`)
  - `social/review-disconnect-and-replacement-fence-old-callbacks`
  - `social/review-expiry-not-extended-by-replay-and-overflow-fails-closed`, with capacity 2, removalCapacity 1, lifetime 10 s, a replay at 9 s and expiry at 10 s
  - `social/review-same-bounded-plain-text` (`"Bad\u{0}text"` refused; 193 × `x` refused; `" Author "` → `"Author"`; `" Cafe\u{301} "` → `"Café"`)

  Add the Twitch fence cases that `GraphicsSocialPlayoutTests.TwitchClear…` drives through the model: a message at order 100, a clear at order 200, `"1234:late"` at order 199 refused, `"1234:new"` at order 201 pending.
- [ ] **Step 2: Run them and watch them fail.** `COREVIDEO_SHELL_TESTS_FILTER=social/review`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and watch them pass.** Expected: PASS.
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-shell): social review queue ported from GraphicsSocialReview"`

**Mutations:**
- Approve by id only, ignoring the revision: `approval-applies-only-to-exact-revision` goes red.
- Refresh `receivedAt` on a replay: the expiry case goes red.
- Truncate instead of refusing: the plain-text case goes red.

---

### Task 13: `SocialPlayout`, the gated social gateway path, bundled card templates

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/SocialPlayout.swift`, `mac-shell/Sources/CoreVideoProShell/SocialCardTemplates.swift`
- Modify: `GraphicsGateway.swift:243-305` (add `invokeSocial`), `GraphicsModel.swift:43-62` (own `social`)
- Create: `mac-shell/Sources/CoreVideoProShell/ShellTestsSocialPlayout.swift`

**Interfaces:**
- Consumes: `GraphicsGateway`, `GraphicsState` (`instances`, `templates`, `coreEpoch`, `GraphicsInstance.isOnProgram`), `GraphicsPlayout.install(_:epoch:)` (`GraphicsPlayout.swift:309-333`), and `SocialReview`.
- Produces:
```swift
extension GraphicsGateway {
  /// The one path that may carry clientId graphics-social-review; `invoke` keeps refusing it (GraphicsCommandGateway.cs:45-46).
  func invokeSocial(_ action: String, _ body: JSONObject) async -> GraphicsOperationResult
}
enum SocialCardTemplates { static let youTube: String; static let twitch: String   // byte copies of contracts/graphics-social/*.json
  static func templateId(provider: String) -> String }                            // "youtube-chat-card" | "twitch-chat-card"
actor SocialPlayout {
  static let clientId = "graphics-social-review", instancePrefix = "social-review-"
  init(gateway: GraphicsGateway, review: SocialReview, sleep: @escaping (UInt64) async -> Void, now: @escaping () -> Date,
       newId: @escaping () -> String)
  func show(_ key: SocialItemKey) async -> GraphicsOperationResult; func hide() async -> Bool
  func connect(provider: String) async -> Int64; func disconnect() async; func disconnect(source: Int64) async
  func apply(source: Int64, _ events: [SocialFeedEvent]) async -> Bool; func expire() async
  func withdraw(_ key: SocialItemKey) async -> Bool
  nonisolated func onProgram(_ state: GraphicsState?) -> Bool
}
```
The `show` sequence and copy follow `GraphicsSocialPlayout.cs:73-148`:
1. The key must be approved, else `"Approve this exact message before showing it."`
2. Retire any previous card, else `"The previous social graphic could not be cleared."`
3. Require a snapshot with a non-empty `coreEpoch`, else `"Native Graphics is unavailable."`
4. Ensure template version 1 is present, installing it if missing. The installed definition must deep-equal the bundled one, else `"The native social graphic template could not be verified."`
5. Re-check the approval, else `"The message changed before Show."`. Then send `graphics.instance.create` with `{instanceId: "social-review-"+newId(), scope:"global", templateId, templateVersion:1, fields:{author,message}, expectedRevision?}`. Every command carries the envelope `{schemaVersion:1, clientId:"graphics-social-review", requestId:"social-"+newId(), coreEpoch}`, and instance commands carry `expectedRevision:1`.
6. Send `graphics.instance.cue` with `rideTake:false`.
7. Poll every 50 ms, through the injected sleep, until `cueRevision == 1 && !cuePending && revision == 1`. `cuePreparation == "failed"` or 5 s gives `"The social graphic could not prepare."`. An epoch change gives `"Native Graphics restarted."`. A lost approval gives `"The message changed before Show."`.
8. Re-check the approval and epoch, then send `graphics.instance.trigger {action:"show", target:"cue"}`.
9. In `defer`, retire the attempted card if Show was not accepted.

Retiring makes up to 3 attempts, 50 ms apart. Each attempt sends `trigger {action:"clear"}` then `remove {}`. A reply of `instance-not-found` counts as success. After every review mutation, an on-air card whose key is no longer approved is retired (`ChangeAsync`, `:157-173`).

- [ ] **Step 1: Write the failing tests** against `ScriptedGraphicsCore` (`ShellTestsGraphicsOperate.swift:11`), extended with a social-aware fake. Port each `GraphicsSocialPlayoutTests` case, and run the Theory cases over YouTube and Twitch:
  - `social/playout-approval-alone-is-private-and-show-uses-exact-revision`: showing a pending item sends 0 commands. After approval, create carries `templateId` = provider card and `fields.message == "First message"`, and cue carries `rideTake == false`. Nothing is on Program before prepare. After Show, `onProgram` is true. Withdrawing takes it off and leaves 0 instances.
  - `social/playout-provider-replacement-cannot-reuse-approval`
  - `social/playout-twitch-clear-invalidates-preparation-or-retires` (room/user × onAir)
  - `social/playout-invalidation-while-preparing-never-submits-show` (edit, remove, disconnect, ban): no `show` trigger is ever sent.
  - `social/playout-moderation-retires-on-air-card-and-rejects-same-batch-replay`
  - `social/playout-replacement-during-preparation-starts-pending`

  Plus these Mac-specific cases:
  - `social/gateway-public-path-still-refuses-social-client`: the existing test `graphics/gateway-social-client-refused` stays green, and `invokeSocial` with `clientId` `graphics-playout` is refused with `social-graphics-require-operator-review`. The C# gate is symmetric: the flag must match the client.
  - `social/templates-are-byte-copies-of-contracts`: `SocialCardTemplates.youTube` == the contents of `contracts/graphics-social/youtube-chat-card.json`, read via `#filePath`, and the same for Twitch.
  - `social/restart-reinstalls-template-on-next-show`: after an epoch change, the scripted core lists no templates; `show` sends `graphics.template.install` and then create.
- [ ] **Step 2: Run them and watch them fail.** Expected: FAIL.
- [ ] **Step 3: Implement.** `invokeSocial` shares the correlation and in-flight logic with `invoke`. Factor that into a private `send(built:)` so the refusal stays only in `invoke`.
- [ ] **Step 4: Run them and watch them pass,** then run the full suite. Expected: PASS.
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-shell): serialized social playout on a gated gateway path, bundled chat card templates"`

**Mutations:**
- Skip the re-check before trigger: `invalidation-while-preparing…` goes red.
- Let `invokeSocial` accept any client: `gateway-public-path…` goes red.
- Send `rideTake:true`: the first case goes red, and at the real core in Task 14 it is refused with `social-graphics-private-cue-only`.

---

### Task 14: Review sheet, view model, entry point, strip filter, real-core checks

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/SocialReviewModel.swift` (view model), `mac-shell/Sources/CoreVideoProShell/SocialReviewSheet.swift`
- Modify: `GraphicsPlaylistPane.swift:~18` (header button), `GraphicsState.swift:254-286` (filter both cards by prefix; label Twitch), `AppModel.swift` (`onConnected`, quit, show change → disconnect)
- Create: `mac-shell/Sources/CoreVideoProShell/ShellTestsSocialSheet.swift`; extend `ShellTestsGraphicsLive.swift`

**Entry point:** a ghost button `"Social review…"` in the PLAYLIST header, before `"＋ Add graphic"`, matching `GraphicsPlayoutControl.cs:46,66-68`. It is shown only on Graphics → Operate (not the compact Studio playlist). While the sheet is open the button is disabled.

**Sheet layout** (`GraphicsSocialReviewDialog.cs:37-77`; Windows uses a `ContentDialog` about 548 pt wide). The Mac `.sheet` is `frame(width: 548)` with a `ScrollView` of max height 485 around a `VStack(spacing: 8)`. Title `"Social review"`, and a `"Close"` button. Regions, top to bottom:
1. Muted note: `"Review each message before approving it. Show approved is a separate action."`
2. A 160 pt column with the `"Provider"` picker (YouTube, Twitch), next to a flexible column with the muted text `"Selected source\n<label>"`.
3. A 1:2 two-column row:
   - YouTube: `"Public YouTube video ID"` (max 11) | `SecureField` `"YouTube Data API key"` with placeholder `"Leave blank to reuse the local key"` (max 128)
   - Twitch: `"Twitch channel name"` (max 25) | `"Twitch public app Client ID"` with placeholder `"Leave blank to reuse the saved sign-in"` (max 128)
4. Twitch device prompt (hidden unless pending): `"Twitch sign-in code: <code>"` and a link `"Open Twitch activation"` that opens the validated `verification_uri` with `NSWorkspace.open`.
5. Buttons: `"Connect"`, `"Disconnect"`, `"Clear local data"`.
6. Status text, prefixed `"On Program · "` when on air.
7. A list 96 pt tall, single selection. Rows read `"{Provider} · {Author} · {Decision} · {time}"`.
8. Full text: a `ScrollView` of min 42 / max 85 pt showing `"{Provider} · {Author}\n{Message}"`, or `"Select one message to read it."`
9. Buttons: `"Approve"`, `"Reject"`, `"Withdraw"`.
10. Buttons: `"Show approved"`, `"Hide social graphic"`.

Enable rules (`:164-178`):
- Approve and Reject: connected, not busy, and the selection is pending.
- Show: the selection is approved.
- Withdraw: any selection.
- Hide: on Program.
- Connect: the input validators pass.

A revision change drops the selection. The key field is cleared on Connect and on close. All status strings are verbatim from `StudioViewModel.GraphicsSocialReview.cs` (listed in the research notes; the executor copies them from the file).

**`SocialReviewModel`:**
- It owns `SocialPlayout`.
- A 1 s expiry tick uses the injected sleep.
- A generation counter drops stale feed status callbacks.
- Disconnect happens on a show change, a provider change, quit (bounded 350 ms, before the core stops), and a core generation change. A core generation change also means the next Show reinstalls the template.
- A dev-only test source, `COREVIDEO_SHELL_SOCIAL_TEST_SOURCE=1`, adds a `"Paste test message"` row. It connects provider `"YouTube"` with a local source so B2 can be proven before B3 exists. With the env var unset, the row does not exist, so this is not a product divergence.

- [ ] **Step 1: Write the failing tests.**
  - `social/sheet-incoming-edit-clears-selection` (port of `GraphicsSocialReviewDialogTests.IncomingEditClearsSelectionAndRequiresFreshReview`)
  - `social/sheet-enable-rules` (each rule above, as a table)
  - `social/strip-hides-both-provider-cards-from-cued`: a state with instances `social-review-a` (youtube) and `social-review-b` (twitch), both cued. `GraphicsStrip` shows 0 cued, and the on-air label for Twitch is `"Twitch chat"`.
  - `social/control-api-cannot-reach-social`: every `ControlApi` action id starting with `graphics.` except `graphics.lowerThird.*` is `.blocked`, and no `comms.`/`social.` id exists. This drives `ControlApiCatalog`, not a list literal (rule 10).
  - `social/disconnect-on-core-generation-change`
  - `social/redaction-sentinel` (started here, extended in Tasks 15-17). Run a scripted session through the test source with author `"SENTINEL_AUTHOR_7f3a"` and message `"SENTINEL_MSG_91c2"`: approve, show, withdraw. Then generate a support bundle into a temp dir via the existing `Diagnostics` writer (`Diagnostics.swift:354-416`) and grep every file in it. Neither sentinel appears.
- [ ] **Step 2: Run them and watch them fail.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and watch them pass.** Expected: PASS.
- [ ] **Step 5: Real-core checks.** Extend `ShellTestsGraphicsLive.swift`, which needs `COREVIDEO_SHELL_LIVE_CORE=<native/build-metal/corevideo-native>` and skips loudly without it:
  - `social-live/wrong-client-refused-program-unchanged`: send `graphics-instance-create` for `social-review-x` with `clientId` `graphics-playout` straight through the bridge, bypassing the gateway. The reply is refused with code `social-graphics-require-operator-review`, and a Program readback before and after is pixel-identical. Read Program through the existing Metal readback used by the graphics live check.
  - `social-live/card-on-program-not-preview-gone-after-15s`, with an injected clock only for the shell; the 15 s is the core's real clock:
    - show an approved test-source item
    - within 1 s, the Program readback differs from baseline inside the card's lower-centre rect (1600×232, y = -54) and the Preview readback does not
    - after 15.5 s, Program equals baseline again
  - `social-live/twitch-card-renders`: the same as the previous check, with the Twitch template. This proves the Segoe UI substitution draws text: the card rect has more than 2 % non-background pixels in the text area.
- [ ] **Step 6: Commit.** `git commit -m "feat(mac-shell): Social review sheet in the Windows layout; real-core fail-closed and card pixel checks"`

---

### Task 15: YouTube transport, feed, key store, connect form

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/SocialHTTP.swift` (the shared seam), `mac-shell/Sources/CoreVideoProShell/YouTubeChat.swift`, `mac-shell/Sources/CoreVideoProShell/SocialCredentials.swift`
- Create: `mac-shell/Sources/CoreVideoProShell/ShellTestsSocialYouTube.swift`

**Interfaces (Produces):**
```swift
struct SocialHTTPRequest { var method: String; var url: URL; var headers: [String: String]; var body: Data? }
struct SocialHTTPResponse { var status: Int; var headers: [String: String]; var body: Data }
protocol SocialHTTP { func send(_ r: SocialHTTPRequest, deadline: TimeInterval) async throws -> SocialHTTPResponse }
final class URLSessionSocialHTTP: NSObject, SocialHTTP, URLSessionTaskDelegate  // ephemeral; no cookies, no cache, no proxy (N5);
  // willPerformHTTPRedirection → completionHandler(nil) so a 3xx surfaces as the response; body capped at 1 MiB (1_048_576)
enum SocialFailure: Error, Equatable { case authorization, ended, retry, invalid }   // messages are fixed strings, never response text
enum YouTubeChatTransport {
  static func resolve(videoId: String, key: String, http: SocialHTTP) async throws -> String   // activeLiveChatId
  static func page(chatId: String, cursor: String?, key: String, http: SocialHTTP) async throws -> (events: [SocialFeedEvent], next: String?, waitSeconds: Int, ended: Bool)
  static func isVideoId(_ s: String) -> Bool; static func isApiKey(_ s: String) -> Bool
}
enum YouTubeSocialFeed { static func run(videoId: String, key: String, playout: SocialPlayout, http: SocialHTTP,
  sleep: @escaping (UInt64) async -> Void, report: @escaping (String) -> Void) async }
struct SocialCredentials {   // KeychainStore(service: "us.iamfatness.corevideopro.social", ...)
  static var youTubeKey: KeychainStore { get }      // account "youtube-api-key"
  static var twitch: KeychainStore { get }          // account "twitch-credentials" (one JSON blob)
}
```
The rules are in `YouTubeChatTransport.cs` and `YouTubeSocialFeed.cs`:
- Origin `https://www.googleapis.com/youtube/v3/`.
- The exact query strings come from `:51` and `:69-71`, including `fields`.
- The key goes in `x-goog-api-key`.
- The request deadline is 12 s.
- A 3xx maps to authorization. Status ≥ 500 or 429 maps to retry. The listed error reasons map to ended or retry. Anything else maps to authorization.
- The wait is `max(pollingIntervalMillis, 15000)`.
- Event kinds:
  - `textMessageEvent` → text
  - `messageDeletedEvent`, `messageRetractedEvent`, `tombstone` → remove
  - `userBannedEvent` → ban
  - anything else is ignored
- Retry backoff is 30/60/120 s with up to 3 attempts, and the count resets after a good page.
- Status strings and error messages are copied verbatim (`:8-14`).

- [ ] **Step 1: Write the failing tests.** Port the four `YouTubeChatTransportTests` cases and the credential-store case. The Swift JSON bodies are the C# test literals copied verbatim, with a comment citing the C# file and line.
  - `social/youtube-requests-use-fixed-tls-host-and-map-kinds`: host `www.googleapis.com`, https, key absent from `url.absoluteString`, header present, `pageToken=previous%2B%2F`, a 21000 ms interval gives 21 s, kinds `[text, remove, remove, remove, ban]`, `superChatEvent` ignored.
  - `social/youtube-failures-classified-without-exposing-key`: a redirect and 401 map to authorization; 403 `quotaExceeded` to retry; 403 `liveChatEnded` to ended; 400 `invalidPageToken` to retry. `String(describing: error)` never contains the key, even when the fake response body echoes it.
  - `social/youtube-wrong-source-or-missing-removal-target-fails-closed`
  - `social/youtube-cursor-advances-and-reconnect-requires-review` (waits `{15, 30, 15}` s through the injected sleep)
  - `social/youtube-key-in-keychain-not-prefs`: after connect, the `MemoryKeychain` holds `(us.iamfatness.corevideopro.social, youtube-api-key)`, and `ShellPrefs` JSON does not contain the key.
  - `social/youtube-redirect-is-not-followed`: `URLSessionSocialHTTP` against a local `NWListener` that answers 302 to `/elsewhere`. The response status is 302 and `/elsewhere` is never requested.

  Extend `social/redaction-sentinel` with the key `AIzaSENTINELKEY1234567890ab` passed through a full scripted feed session.
- [ ] **Step 2: Run them and watch them fail.** Expected: FAIL.
- [ ] **Step 3: Implement,** and wire the sheet's YouTube connect form.
- [ ] **Step 4: Run them and watch them pass.** Expected: PASS.
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-shell): YouTube live chat transport and feed for social review"`

---

### Task 16: Twitch HTTP transport, device authorization, credential store

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/TwitchAuthorization.swift`
- Create: `mac-shell/Sources/CoreVideoProShell/ShellTestsSocialTwitchAuth.swift`

**Interfaces (Produces):**
```swift
struct TwitchCredentials: Codable, Equatable, CustomStringConvertible { var clientId, accessToken, refreshToken: String
  var expiresAt: Date; var userId, login: String; var description: String { "Twitch credentials" } }
struct TwitchDevicePrompt: Equatable { let userCode: String; let verificationUri: URL }
actor TwitchAuthorization {
  init(http: SocialHTTP, store: KeychainStore, now: @escaping () -> Date, sleep: @escaping (UInt64) async -> Void)
  func authorize(clientId: String, prompt: @escaping (TwitchDevicePrompt) -> Void) async throws -> TwitchCredentials
  func maintain(force: Bool) async throws -> TwitchCredentials    // refresh at <= 60 s left; validate hourly; serialized
  func clear()
  static func isChannel(_ s: String) -> Bool; static func isClientId(_ s: String) -> Bool
}
```
The rules are in `TwitchHttpTransport.cs` and `TwitchAuthorization.cs`:
- Device start is `POST https://id.twitch.tv/oauth2/device` with `client_id` and `scopes=user:read:chat`.
- `verification_uri` must be exactly `https://www.twitch.tv/activate` on port 443, with no userinfo and no fragment.
- `user_code` is at most 32 characters, `expires_in` at most 3600, and `interval` at most 60.
- The token poll uses `grant_type=urn:ietf:params:oauth:grant-type:device_code`. `authorization_pending` keeps polling. `slow_down` adds 5 s, capped at 60. Polling stops at the deadline and never restarts the grant.
- Refresh uses `grant_type=refresh_token` and **no client secret**. The rotated pair is stored **before** it is used.
- Validate is `GET https://id.twitch.tv/oauth2/validate` with `Authorization: OAuth <token>`. It runs at startup and then when at least 1 h has passed. A changed user id is treated as authorization failure and clears the store.
- The token type must be `bearer` and the scope must be present.
- The whole `TwitchCredentials` is stored as one JSON blob in `SocialCredentials.twitch`. The client ID is user-entered; a blank field reuses the stored one.

- [ ] **Step 1: Write the failing tests.** Port the five `TwitchAuthorizationTests` cases and the credential-store case:
  - `social/twitch-oversized-response-fails-closed` (1 MiB + 1 → invalid)
  - `social/twitch-device-flow-read-chat-only-stores-only-validated-tokens`: host `id.twitch.tv`, no `client_secret` in any body, `scopes=user%3Aread%3Achat`, 2 polls, nothing stored before validate, `String(describing: creds)` contains no token
  - `social/twitch-denied-or-expired-consent-never-stores` (`access_denied`, `expired_token`)
  - `social/twitch-device-deadline-never-repeats-grant` (one device start)
  - `social/twitch-hourly-validation-single-use-refresh-serialized-identity-fixed`: 8 concurrent `maintain` calls produce 1 refresh; the rotated refresh token is stored; foreign user `"9988"` throws and clears the store
  - `social/twitch-rotated-pair-stored-atomically`: one Keychain write holds both tokens, and the item is never left holding a new access token with an old refresh token
  - `social/twitch-verification-uri-must-be-twitch-activate`: `https://evil.example/activate` and `http://www.twitch.tv/activate` are both invalid

  Extend `social/redaction-sentinel` with an access token `SENTINELACCESS0123456789` and a refresh token.
- [ ] **Step 2: Run them and watch them fail.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run them and watch them pass.** Expected: PASS.
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-shell): Twitch device authorization and atomic credential storage for social review"`

---

### Task 17: Twitch EventSub socket, session, feed

**Files:**
- Create: `mac-shell/Sources/CoreVideoProShell/TwitchEventSub.swift`
- Create: `mac-shell/Sources/CoreVideoProShell/ShellTestsSocialTwitchEventSub.swift`

**Interfaces (Produces):**
```swift
protocol TwitchSocket { func connect(_ url: URL, deadline: TimeInterval) async throws; func receive(deadline: TimeInterval) async throws -> String
  func close() }   // receive throws SocialFailure.invalid for binary / bad UTF-8 / > 65536 bytes, .retry on close or deadline
final class URLSessionTwitchSocket: TwitchSocket   // URLSessionWebSocketTask, maximumMessageSize = 65536, no client ping
struct TwitchEventSubSession {
  static let requiredTypes = ["channel.chat.clear", "channel.chat.clear_user_messages", "channel.chat.message_delete", "channel.chat.message"]
  mutating func welcome(_ json: String) throws -> (sessionId: String, keepaliveSeconds: Int)
  mutating func confirm(type: String, response: Data) throws
  enum ReadOutcome: Equatable { case keepalive, reconnect(URL), revoked, events([SocialFeedEvent]) }
  mutating func read(_ json: String) throws -> ReadOutcome
}
enum TwitchTimestamp { static func nanoseconds(_ iso: String) -> Int64? }   // integer parse of the fraction; never Date
enum TwitchSocialFeed { static func run(channel: String, clientId: String, auth: TwitchAuthorization, playout: SocialPlayout,
  http: SocialHTTP, socketFactory: () -> TwitchSocket, sleep: @escaping (UInt64) async -> Void, report: @escaping (String) -> Void) async }
```
The rules are in `TwitchChatSocket.cs`, `TwitchEventSubSession.cs` and `TwitchSocialFeed.cs`:
- URL `wss://eventsub.wss.twitch.tv/ws`, 12 s connect.
- The welcome must be `session_welcome` with status `connected` and a keepalive of 10-600 s.
- Subscribe in the `requiredTypes` order through `POST helix/eventsub/subscriptions`. `read` throws until all four are confirmed. Only then does the feed call `playout.connect("Twitch")` and report `"Connected to Twitch live chat: <channel>."`
- Deliveries are de-duplicated by `message_id` over a window of 4096.
- Ids are `"<broadcasterId>:<message_id>"`.
- Non-text messages and shared-chat messages from another origin are dropped.
- A `session_reconnect` URL must be `wss://eventsub.wss.twitch.tv` on port 443.
- `revocation` stops with no retry.
- The retry budget is 3 attempts (1, 2, 4 s). Each reconnect after the first forces a credential refresh. **Every connection gap disconnects the review**, which clears approvals and retires the card.
- `maintain` runs every 30 s.
- Status strings are copied verbatim (`TwitchEventSubSession.cs:11-14`, `TwitchSocialFeed.cs`).

- [ ] **Step 1: Write the failing tests.** Port every case of `TwitchChatSocketTests`, `TwitchEventSubSessionTests` and `TwitchSocialFeedTests`, with the C# literals verbatim:
  - `social/twitch-socket-*`: a fragmented UTF-8 `"{é}"`; a missed keepalive → retry; binary, bad UTF-8 or oversize → invalid; a close → retry.
  - `social/twitch-session-requires-all-four-subscriptions-and-dedupes` (`"1234:message-a"`)
  - `social/twitch-session-foreign-subscription-cannot-enable-review`
  - `social/twitch-session-nanosecond-ordering`: room-clear order minus user-clear order **== 1**. This is the precision trap from spec risk 7: a `Date`-based parse loses it and goes red.
  - `social/twitch-session-clear-fences-retire-only-pre-clear-items`
  - `social/twitch-session-revocation-and-migration-explicit` (a 65537-character body refused; keepalive 0 refused)
  - `social/twitch-feed-removal-subscriptions-ready-before-any-message`
  - `social/twitch-feed-failed-required-subscription-never-enables-review`
  - `social/twitch-feed-revocation-clears-approved-without-retry` (read deadlines 10 s, exactly 1 connection)
  - `social/twitch-feed-unexpected-disconnect-and-migration-clear-approvals`
  - `social/twitch-feed-gap-retires-on-air-card`: an approved card is on air through the scripted core, then the socket closes. The playout sends clear+remove, and `review.items` has no approved item.
- [ ] **Step 2: Run them and watch them fail.** Expected: FAIL.
- [ ] **Step 3: Implement,** and wire the Twitch half of the sheet, including the device prompt.
- [ ] **Step 4: Run them and watch them pass,** then run the full suite. Expected: PASS.
- [ ] **Step 5: Commit.** `git commit -m "feat(mac-shell): Twitch EventSub chat for social review with ordered moderation fences"`

**Mutations:**
- Parse timestamps with `ISO8601DateFormatter`: the nanosecond case goes red.
- Subscribe to `channel.chat.message` first: `removal-subscriptions-ready-before-any-message` goes red.
- Keep approvals across a reconnect: `gap-retires-on-air-card` goes red.

---

### Task 18: Social live acceptance, docs, outcomes

**Files:**
- Modify: `docs/reference/graphics-playout.md` (a macOS note under Social review: files, the Keychain items, no redirect, the template font substitution)
- Modify: `docs/superpowers/plans/2026-11-22-mac-comms-social-phase4-outcomes.md` (the Social half)
- Create: `mac-shell/Sources/CoreVideoProShell/` fixture files only if a real provider response is captured (Step 2)

- [ ] **Step 1: Full suite and redaction.** Run `COREVIDEO_SHELL_TESTS=1` (everything) and `COREVIDEO_SHELL_SELFCHECK=1`. `social/redaction-sentinel` must pass with all four sentinels: author, message, YouTube key and Twitch tokens.
- [ ] **Step 2: YouTube live (owner).** Use the owner's public livestream with chat on.
  - Connect with the API key; the status reads `"Connected to YouTube live chat."`.
  - The owner posts a message. Approve it, then Show approved.
  - The card appears on Program and not on Preview. Check this through the Program monitor and a recording.
  - The card ends by 15 s.
  - Withdraw a pending message.
  - Disconnect; the status reads `"Disconnected. Review decisions cleared."`.
  - Then generate a support bundle and grep it for the API key and the message text: nothing.
  - Optionally save one real `liveChat/messages` response body that contains only the owner's own test message as `contracts/graphics-social/fixtures/youtube-live-page.json`, and add a parse test against it.
- [ ] **Step 3: Twitch live (owner + moderator).**
  - Device sign-in with the owner's client ID; the code shows and "Open Twitch activation" opens `twitch.tv/activate`.
  - Approve a message, then have the moderator delete it in Twitch. It cannot be shown, and if it was on air it is retired.
  - Approve another message, then pull the network for 10 s. The status reads `"Twitch chat disconnected. Review decisions cleared. Reconnecting…"`, the approval is gone, and the card (if shown) is gone.
  - Reconnect succeeds within the budget.
  - Relaunch the shell: the saved sign-in is reused with a blank client ID field.
  - Check the support bundle as in Step 2.
- [ ] **Step 4: Outcomes.** Record per task: tests added, mutation results, live evidence, UNVERIFIED items left, deviations, and the Windows backlog issue for N4 (if the owner agreed).
- [ ] **Step 5: Commit.** `git commit -m "docs: Mac social review live acceptance and outcomes"`

---

## Where the master plan and the spec were wrong or incomplete

Each item below was verified in code on 2026-11-22.
- **The pinned ZComms commit already contains the Mac adapter.** The spec said the local clone lacked `168a572d` and implied a re-pin. A `git fetch` shows `168a572d` is ZComms `origin/main` and includes `src/zoom/talkback_sdk_mac.{h,mm}` (added 2026-09-05, `a24bb28..7c4abd0`). No re-pin is needed. The adapter requires `-fobjc-arc`.
- **The SDK mapping table in spec A2 is not 1:1:**
  - `hasRawDataLicense` returns a `ZoomSDKError`, not a bool.
  - The meeting-status enum has no Locked/Unlocked, adds `AudioReady`, and every numeric value differs.
  - Audio status `None` must read as muted for MicGuard.
- **The mic TCC risk is attributed to the shell, not the engine** (fork with no disclaim). A denied grant gives silence, not an error, so the plan adds an explicit authorization check. The spec's "the prompt names the engine bundle" is wrong in mechanism.
- **"Develop the shell against the fake engine via `COREVIDEO_ZOOM_ENGINE_PATH`" does not work** on a machine with an engine build. `CoreLaunchEnvironment` overwrites it (fixed in Task 6).
- **The spec's Comms page design did not account for `TabPage`'s outer `ScrollView`.** Windows Comms pins the rail, strip and ops strip. Without an opt-out, the Mac page would repeat the stacked-in-one-scroll defect found on the grade editor and the Graphics Operate tab.
- **Spec A1.7 and the reference doc say meeting end and a dropped report release keys.** In Windows code those two only clear local holds (`CommsViewModel.cs:318,326`). The engine lease and the core's `comms:null` do the real release. The Mac port follows the code.
- **"miniaudio compiles on macOS: UNVERIFIED"** is already answered by ZComms' macOS CI (CoreAudio, AudioToolbox and CoreFoundation; no AudioUnit is needed).
- **The A2 spike had no driver.** The Mac shell cannot send `comms-*` until Task 10. The plan drives the real engine directly over IPC (`scripts/qa/macos-engine-comms-probe.py`), so the native lane does not wait for the page.
- **A5's "8 video feeds" cannot be staffed** with owner + co-host (N6).
- **B2's dependencies are met.** Metal G1/G2 and graphics shell G1-G3 are on main (#1220, #1223, #1224). However, the Mac gateway deliberately refuses the social client, so B2 needs a separate gated path (Task 13), which the spec did not mention.
- **No OAuth redirect is involved for either provider** (API key and device code), so no URL scheme, loopback, or `ASWebAuthenticationSession` work is needed. The spec's "redirects refused" means refusing HTTP 3xx, not OAuth.
- **The spec proposed persisting video ID, channel and client ID in `ShellPrefs`.** Windows persists only the credentials, so the plan matches Windows.
- **B1 landed as `KeychainStore` without the explicit `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`** that spec B3 asked for. Login-keychain generic-password items do not sync, so this plan does not change it.
- **The card templates use `"Segoe UI"`,** which needs the Metal font substitution, and Windows filters only the YouTube card from its strip (N4). Neither was in the spec.
- **The master plan lists only "send threading" as the Comms risk.** The audio-join state (VoIP joined or not, with the SDK UI shown) is an equal unknown, because talkback is silent unless meeting audio is open (Task 5, N3).
- **Outside this lane, recorded so it is not lost:**
  - The Mac engine's quit block neither leaves the meeting nor stops raw media before `unInitSDK`, unlike Windows (`main.cpp:1666-1697` vs `main-macos.mm:2389-2397`).
  - The unmerged Zoom HD fix (#1226, branch `mac-zoom-hd-video`) edits `CVAuthDelegate` in `main-macos.mm`. Land it before Task 3, or expect a trivial conflict.
