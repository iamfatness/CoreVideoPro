# Capture dropout policy from adapter liveness, not frame cadence — design

**Date:** 2026-10-23
**Status:** proposed design, for owner review
**Issues:** [#758](https://github.com/iamfatness/CoreVideoPro/issues/758) (a disconnected bridged camera stays on Program as a frozen frame), [#562](https://github.com/iamfatness/CoreVideoPro/issues/562) (capture "On dropout" needs adapter liveness, not frameId cadence)
**Related:** #993 (camera disconnect blocks the command lane; its reader-lane design is `docs/superpowers/specs/2026-10-23-capture-readsample-lane-isolation-design.md`, which this spec consumes and does not redesign), #535 slice 4a (bus health on air, `docs/reference/source-bus.md`), #537 (DeckLink adapter, unbuilt), `docs/reference/operator-reporting-behavior-plan.md` row "#758, #562"
**Plan:** `docs/superpowers/plans/2026-10-23-capture-dropout-policy.md`
**Code read at:** `main` 1879ccb7 (2026-10-23). Line numbers below are from that commit.

## Owner rules this design must satisfy

1. Per source, hold-or-black is the operator's choice on the Sources page and is honoured exactly (2026-09-19 ruling). Default is hold.
2. A static but live picture (a slide, a test card, a covered lens) is never treated as dropout.
3. A dead device leaves Program within a bounded time, named here: **5.0 s after its last delivered sample, worst case, under either policy, on either adapter.**
4. Honest reporting: the Sources page shows what the core actually did, from facts the adapter observed. Nothing is inferred in the shell (`docs/reference/honest-reporting.md`).
5. Both capture adapters in scope: native Media Foundation UVC (`UvcCaptureDeviceAdapter`) and the WinUI shared-memory bridge (`WinUiCaptureDeviceAdapter` + `ShmCapturePreparation`, fed by the shell's `CaptureDeviceFrameReaderService`).
6. Capture on the source bus holds frames by design (slice 2: the adapter re-emits its held frame every tick). So "hold" is the natural default and "black" must be an explicit operator choice.
7. Existing rulings stay: warming = neutral dark slate, failed = dark slate with the source's resolved name, the pink tile is retired, black is a Program-only cut (Preview and multiview keep the held frame, R3).

## Why

### #758: the frozen frame, traced

A bridged camera is unplugged. Nothing on the path from the device to Program ever notices.

1. **The shell never tells anyone.** `CaptureDeviceFrameReaderService.CaptureSession` does not subscribe to `MediaCapture.Failed`. Its only loss detector is the stall watchdog (`CaptureDeviceFrameReaderService.cs:890-934`, 2.5 s threshold), which restarts the reader with backoff and, after five failed restarts, deliberately "leave[s] the last frame frozen (the operator sees it)" (`:979-990`). When the device watcher reports the removal, `StudioViewModel.ApplyDiscoveredCaptureDevices` (`StudioViewModel.cs:7238-7290`) rebuilds the device list. The vanished row just disappears. Its reader session is not stopped, and its shared-memory buffer is not released.
2. **The shell releases the buffer on one path only.** `ReleaseCaptureBridgeBuffer` (`StudioViewModel.cs:10015-10026`) is the only caller of `unregister-capture-shm`. It runs only when native capture takes a device over from the bridge (`:6790-6796`, added by #791). Unplug, give-up, reader stop and shutdown never call it.
3. **The core re-serves the last frame for as long as the buffer is registered.** `ShmCapturePreparation::latest()` (`ShmCapturePreparation.cpp:107-120`) returns the last completed frame, or the frozen `held` frame, for every registered request. Nothing ages it out. `WinUiCaptureDeviceAdapter::captureVideoTick` (`WinUiCaptureDeviceAdapter.cpp:32-51`) re-publishes it every tick with a fresh `timestampMs` (`:43-48`). `unregisterBuffer` (`ShmCapturePreparation.cpp:99-106`) is the only thing that drops it.
4. **The bus keeps it.** `publishHeldCaptureFrame` (`CaptureBusRoster.h`) adopts every frame the adapter emits. By the slice-2 rule, a capture id present in the tick is a connected device.
5. **On-air health says "producing" for every capture source.** `MediaCore::annotateLayerSource` (`MediaCore.cpp:1844-1849`) reads `stalled` only when `kind == "zoom"`. Every capture layer reads `producing` regardless of cadence (slice 4a R1). This was correct: frame cadence would have blacked out static screen shares.
6. **The policy is refused for capture anyway.** `MediaCore::setSourcePolicy` refuses any non-`zoom:` dropout policy (`MediaCore.cpp:1797-1806`). The shell refuses it in `StudioViewModel.SetSourceDropoutPolicy` (`:4808-4820`), in the control action `source.dropout.set` (`StudioControlSurface.cs:317-330`), and on preferences load (`StudioViewModel.cs:12163-12177`). The Sources page has no capture combo (`SourcesInputsPage.xaml:115-120`, and `ShowInputSlotViewModel.ShowSourcePolicies` is true only for Zoom, `:378`).
7. **The compositor draws whatever frame it finds.** `D3D11CompositorAdapter::resolveLayers` (`D3D11CompositorAdapter.cpp:1387-1428`) draws a matched frame unless `blackOnStalled` fires. `ProgramFramePreview.cpp:386-421` and the Metal adapter behave the same way.

Result: the last picture stays on Program, recording, stream and virtual camera indefinitely.

**The native path freezes too.** This is not in #758 as filed, but the same reading shows it. When the MF reader fails, `UvcCaptureSession::fail()` (`UvcCaptureDeviceAdapter.cpp:197-204`) sets `error_` and exits the read loop, but keeps `latestArrival_`. `UvcCaptureDeviceAdapter::captureVideoTick` (`:714-753`) folds the error into `connectionState = "error"` (`:725-728`), and then still re-emits the held arrival (`:741-750`), because the `hasI420()` check runs regardless of error. Steps 4-7 then apply unchanged. A native camera that stops delivering without an error (a driver that never completes the async read, the #993 shape) also re-emits forever: there is no mid-stream watchdog once the first frame has arrived (`:405-433`).

### #562: why cadence cannot decide

The bus's `lastNewFrameNs` advances when a new `frameId` arrives. Cadence is valid evidence for Zoom only, because decoded Zoom video always advances. Several capture producers legitimately re-serve one `frameId` for long stretches while healthy: WGC (frames on change only), browser sources (repaint only), stills, paused clips. Cadence alone cannot tell a static picture from a dead device, so slice 4a removed the capture policy rather than act on noise.

The two adapters in scope are different in kind. A camera or capture card delivers samples at its negotiated rate whatever the picture shows, and the bridge's shell reader writes every `FrameArrived` into shared memory, which advances the sequence. So for these two, **sample arrival measured at the adapter** is a fact about the device, not about the content. But only the adapter knows its producer is sample-driven, and only the adapter sees the explicit loss signals: HRESULTs, reader flags, removal notifications, `MediaCapture.Failed`. The core must therefore never infer liveness from `frameId` equality, pixel content or bus cadence. The adapter reports liveness, and the core applies the policy.

## Design

### 1. The liveness record

One small value type, owned by the adapter, carried on every capture `VideoFrame` the adapter emits:

```cpp
// native/src/modules/Interfaces.h, beside VideoFrame
enum class CaptureLivenessState : uint8_t { Unreported = 0, Live = 1, Lost = 2 };
enum class CaptureLossReason : uint8_t {
  None = 0, DeviceRemoved, DeviceInvalidated, StreamError, EndOfStream,
  ReaderFailed, ShellReaderLost, ShellStopped };
struct CaptureLiveness {
  CaptureLivenessState state = CaptureLivenessState::Unreported;
  bool cadenceTrusted = false;  // sample arrival proves the device is alive (camera-class producers only)
  int64_t lastSampleNs = 0;     // core steady_clock ns when the adapter last observed a NEW device sample
  int64_t lostAtNs = 0;         // core steady_clock ns when the adapter first observed an explicit loss signal
  CaptureLossReason reason = CaptureLossReason::None;
};
// VideoFrame gains: CaptureLiveness captureLiveness;
```

Why on the frame, not a new `ICaptureDevice` method. Capture frames pass through three wrappers that re-collect and re-publish them: `CompositeCaptureDevice::captureVideoTick` (`ModuleComposition.cpp:378-385`), `WinUiCaptureDeviceAdapter`'s `Collect`, and `deliverVideo`'s slot mailbox (`Interfaces.h:1881-1903`). A side channel would need forwarding in each one. The header comment at `WinUiCaptureDeviceAdapter.h:73-77` records two shipped bugs of exactly that shape. A field on the frame survives every copy for free. It costs 24 bytes per frame copy and needs no per-tick `enumerate()`, which is the cheap cache #562 asks for.

Rules:

- **Clock.** Every time is core-process `std::chrono::steady_clock` nanoseconds, the same clock as `MediaCore`'s `nowNs` (`MediaCore.cpp:5170-5171`). The shell's clock never crosses the process boundary. The core stamps the times the bridge observes.
- **`Unreported`** is the default. It reproduces today's behaviour exactly: on air reads `producing`, and the policy is stored but not applied. WGC, browser, SRT, NDI, the grade and inspect probes, and a bridge writer that does not write the signal word all stay `Unreported` in this slice.
- **`cadenceTrusted`** tells the core whether sample silence is evidence. It is true for native UVC and for the bridge when the shell writes `Live`. An adapter that is not sure leaves it false. Then only an explicit loss can fail the source. This is the general answer to #562: an adapter that cannot vouch for its cadence never gets a stall decision based on cadence.
- **`lastSampleNs`** advances on a new device sample, never on a re-emit. (UVC: a new MF sample is published. Bridge: a new even SHM sequence is copied.)
- Liveness never looks at pixels.

### 2. What each adapter observes

#### Native MF UVC (`UvcCaptureDeviceAdapter`)

Signals, in order of authority:

| Signal | Where | Maps to |
|---|---|---|
| `ReadSample` result `MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED` | `readLoop`, `:446-451` | `Lost / DeviceInvalidated` |
| any other failed `ReadSample` HRESULT | same | `Lost / ReaderFailed` (the HRESULT text stays in `warning`) |
| `MF_SOURCE_READERF_ERROR` | `:457-459` | `Lost / StreamError` |
| `MF_SOURCE_READERF_ENDOFSTREAM` | `:453-456` | `Lost / EndOfStream` |
| device-interface removal: `CM_Register_Notification` (`CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE`, all interface classes), `CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL`, symbolic link matched case-insensitively against the session's link | new, adapter-owned registration | `Lost / DeviceRemoved` |
| media-source events delivered to `UvcAsyncRead::OnEvent` (today a no-op): `MEVideoCaptureDeviceRemoved`, `MEError` | `UvcAsyncRead.h` | `Lost / DeviceRemoved` / `StreamError`, **only if** the Task 1 spike shows they arrive on the dev box |
| a new sample published | `:534-544` | `Live`, `lastSampleNs = now` |
| discovery no longer lists a streaming device | `refreshLocked`, `:893-906` | already sets `connectionState = "error"`. Not a liveness source, because it runs only on `enumerate()` and its timing is unbounded |

Why the removal notification is needed even though `ReadSample` usually errors on unplug: #993's failure mode is a driver that never completes the read. The CM callback is independent of the reader and needs no window. It runs on a system thread-pool thread and must only set an atomic loss stamp in the session. No adapter mutex, no MF call, no logging under a lock.

The no-first-frame watchdog (`:410-433`) is not a loss. A device that never delivered has no picture, is never on the bus, and keeps today's warming and failed handling.

**Consumed from #993.** The session's terminal error and its loss time are the outputs of the reader lane that the #993 spec defines. This spec needs exactly three facts from it: the time of the last published sample, the time of the first terminal error, and the error class. If #993 names or places them differently, use its names. If #993 has not merged, the facts come from today's `UvcCaptureSession` (`latestArrival_` publish and `fail()`), with a `lastSampleNs_` and `lostAtNs_` added beside `error_`. #993's guarantee that disconnect never joins a hung reader is a precondition of the native unplug acceptance (A1), not something this spec provides.

`captureVideoTick` stamps `frame.captureLiveness` from the session snapshot, and **keeps re-emitting the held frame after a loss**. Slot removal is no longer how a loss is expressed. The core decides when the picture leaves Program (§3). `connectionState`, `signalPresent` and `warning` in `CaptureDeviceInfo` are set from the same record, so the device list and `sources[]` cannot disagree (`signalPresent = state == Live && sample within kCaptureStallNs`).

#### WinUI bridge (shell reader + `ShmCapturePreparation`)

What the WinUI side can observe:

| Observation | API | Meaning |
|---|---|---|
| `MediaCapture.Failed` | `MediaCaptureFailedEventArgs.Code/Message` | device lost or reset → Lost |
| device watcher removal of this device id | `DeviceWatcher.Removed` (already watched by `CaptureDeviceDiscoveryService`, but today only triggers a refresh) | device removed → Lost |
| stall watchdog give-up after 5 restarts | `CaptureDeviceFrameReaderService.cs:979-990` | no signal → Lost |
| `MediaFrameReader.StartAsync` status other than `Success` on a restart | `RecoverFromStallAsync` | counted by the watchdog; becomes Lost only through give-up |
| `MediaCapture.CaptureDeviceExclusiveControlStatusChanged` | reader is `SharedReadOnly` (`:353-359`) | logged only; frames stopping is caught by cadence |
| `FrameArrived` delivering a frame | `OnFrameArrived` | Live (the SHM sequence advances) |
| the shell process dies or hangs | not observable by the shell | the core sees the sequence stop → cadence |

**Transport: the reserved header word.** The SHM header is `{sequence, width, height, reserved}` (`CaptureDeviceSharedMemoryWriter.cs:15-16`). Offset 12 becomes `signal`: `0` = legacy or unknown (core: `Unreported`), `1` = live, `2` = lost, `3` = stopped. The shell writes `1` with every frame and writes `2` or `3` once, as a single aligned 32-bit store outside the seqlock. The preparation owner reads it with a volatile 32-bit load on every loop iteration (2 ms). This carries loss to the core with no command, so it cannot queue behind a blocked command lane (#993), and it survives a core respawn: the re-announced mapping (`StudioViewModel.cs:10028-10041`) still holds the last signal written. The grade and inspect probes never write the word, so they stay `Unreported`.

The core side (`ShmCapturePreparation`): per mapping, record `lastSampleNs` when a new even sequence is copied (`ShmCapturePreparation.cpp:258`), and `lostAtNs` the first time the word reads `2` or `3`. Publish a per-id liveness snapshot through the same immutable `atomic_store` handoff as `completed_`, republished when it changes. `latest()` stamps every frame it returns, including the frozen `held` frame. `cadenceTrusted` is true when the word is `1` or was `1` before the loss. Word `3` (stopped) maps to `Lost / ShellStopped`.

**Shell lifecycle, so that #758's "done when" holds:**

- Every bridge stop goes through one view-model path, `StopBridgeCaptureAsync(deviceId, reason)`. It writes the signal word, stops the reader and releases the buffer (`CaptureDeviceSharedMemoryWriter.Remove` + `unregister-capture-shm`). This covers native takeover (today's only caller), operator disconnect, device removal, watchdog give-up, the end of a `MediaCapture.Failed` handling and shutdown.
- **On loss** (Failed, Removed, give-up), the shell writes `2` immediately. It releases the buffer `BridgeLostReleaseDelay = 6 s` later, which must exceed the core's `kCaptureSilentFailNs`. A test pins this inequality, so the core can apply the hold window to the picture. An early release is fail-safe: the picture leaves Program sooner.
- **On deliberate stop** (operator disconnect, native takeover, shutdown), the shell writes `3` and releases immediately.
- Each reader session carries a session token. The writer refuses a frame whose token is not the device's current one, so a late `FrameArrived` from a disposed reader cannot write after a reconnect.

### 3. The policy state machine

The pure function `decideCaptureOnAir(liveness, sourceHasFrame, nowNs)`, in a new header `native/src/core/CaptureLivenessPolicy.h`, runs once per layer in `annotateLayerSource`. The constants live in the same header:

```
kCaptureStallNs        = 1'500'000'000   // = kOnAirStallNs; no new sample -> stalled
kCaptureLostHoldNs     = 2'000'000'000   // explicit loss -> policy window, then failed
kCaptureSilentFailNs   = 5'000'000'000   // no new sample at all -> failed (the owner's bound)
```

| State | Entered when | On-air health | Program shows | Preview / multiview |
|---|---|---|---|---|
| **unreported** | `state == Unreported` | `producing` (today) | the frame | the frame |
| **live** | `Live`, `now - lastSampleNs < kCaptureStallNs` | `producing` | the frame, static or not | the frame |
| **stale-but-live** | `Live`, `cadenceTrusted`, `now - lastSampleNs >= kCaptureStallNs`, no loss | `stalled` | hold → held frame. Black → black | held frame (R3) |
| **lost (holding)** | `Lost`, `now < failDeadline` | `stalled` | hold → held frame. Black → black | held frame (R3) |
| **failed** | `now >= failDeadline` | `failed` | named failed slate | named failed slate |

`failDeadline` is `min(lostAtNs + kCaptureLostHoldNs, lastSampleNs + kCaptureSilentFailNs)` for an explicit loss, and `lastSampleNs + kCaptureSilentFailNs` for silence with `cadenceTrusted`. With `cadenceTrusted == false`, only an explicit loss sets a deadline.

Transitions back:

- stale-but-live → live on the first new sample, with no hysteresis on recovery (the Zoom rule).
- lost or failed → live only on a sample with `lastSampleNs > lostAtNs` and `state == Live`. That means a reconnected session or a bridge reader that recovered. Late arrivals from the previous session are already fenced by `sourceEpoch` / generation (UVC: a new session object. Bridge: `registerBuffer` bumps the generation, `ShmCapturePreparation.cpp:83`) and by the shell's session token (§2).

Bounds, from the device's last delivered sample to Program leaving that picture:

| | silent death (no signal) | explicit loss signal observed by the core |
|---|---|---|
| **black** | 1.5 s | one render tick after observation |
| **hold** | 5.0 s (then failed slate) | ≤ 2.0 s after observation, never past 5.0 s from the last sample |

The owner's single number is 5.0 s. Signal-to-core latency is part of the acceptance measurement (A1-A4), not assumed.

**Failed never draws a picture.** A new shared rule in `compositor/CompositorLayout.h`, `suppressFrameFor(health)`, returns true for `"failed"`. It is applied in all three compositors (`D3D11CompositorAdapter`, `ProgramFramePreview`, `MetalCompositorAdapter`) at the same site as `blackOnStalled`. The existing name label (`drawFailedSlateName`, `D3D11CompositorAdapter.cpp:1897-1905`) then draws because `layer.frame == nullptr`. Today no Zoom or media layer can be `failed` with a matched frame, so the rule changes nothing for those kinds. It applies to Preview and multiview too: R3 keeps the held frame for a *stall*, but showing a picture of a device declared dead would be dishonest.

**Black then slate.** Under `black`, a stall cuts Program to black. At the failure deadline the owner's failed ruling takes over, and Program shows the named failed slate. Both policies converge on the same honest end state. The alternative, black that stays black after failure, is owner question Q1 below.

**Hold is bounded for capture, deliberately unlike Zoom.** A Zoom hold is unbounded, because Zoom loss is a subscription gap that heals. A capture hold ends at the failure deadline, because owner rule 3 forbids a dead device's frame from staying on Program. The Sources page says so in the option label (§5).

**When the frame leaves the bus.** On unregister or disconnect, the adapter's slot ends (`replaceVideo` / `postVideoEnd`) and `endHeldCaptureFrame` removes the bus entry. A layer with no bus entry falls back to `annotateLayerSource`'s frames scan: `failed` if other frames exist, otherwise `""`, which draws the warming slate (`MediaCore.cpp:1850-1856`). So that a lost device reads failed in every case, `endHeldCaptureFrame` keeps a **video-less tombstone** for a capture source whose last liveness was reported (not `Unreported`). The tombstone holds no pixels, has liveness `Lost`, and is replaced by the next frame for the id. It is cleared by `disconnect-capture-device` for that id, and capped at 32 entries with the oldest evicted. `annotateLayerSource` reads a tombstone as `failed`. An `Unreported` source keeps the slice-2 remove-on-absence rule unchanged.

### 4. Wire and snapshot

- **`set-source-policy`** accepts `dropoutPolicy` for `capture:<id>` as well as `zoom:<pid>`, keyed verbatim (`MediaCore.cpp:1772-1775` already defines the key rule). Every other prefix is refused, with the warning text updated. The policy is stored even when the source's liveness is `Unreported`, because the shell re-sends persisted policies before devices connect. It is applied only when the decision function sees reported liveness.
- **`sources[]`** (`SourceVideoIngress.cpp:96-117`) gains these fields per capture source. The values come from the decision the render path last applied, recorded on the `CaptureDeviceSource` under `coreMutex`, so the snapshot reports what Program did rather than recomputing it with a different `nowNs`:
  - `liveness`: `"unreported" | "live" | "stalled" | "lost"`
  - `onAir`: `"producing" | "stalled" | "failed"`
  - `lastSampleAgeMs`: number, or `null` when unreported
  - `lossReason`: a reason code string, or `""`
  - `failInMs`: time left to the failure deadline while holding; `null` otherwise
  - `dropoutPolicyApplies`: bool, true iff liveness is reported
  The existing `health` field (200 ms diagnostic) is unchanged.
- **`captureDevices[]`** keeps `connectionState`, `signalPresent` and `warning`, now consistent with the same record (§2).

### 5. Sources page

- The shell lifts the Zoom-only guard for `capture:`. `SetSourceDropoutPolicy`, `source.dropout.set` and the preferences prune accept `zoom:` and `capture:`, and still refuse `media:` and others. `ProductionStateHelper.PopulateCaptureDeviceDropoutPolicy` resolves the stored value instead of being forced to hold. `BuildSourcePolicyWires` sends capture policies. No preferences migration is needed: v13 files contain no capture keys, because they were pruned.
- **One control, in the existing place.** The inspector's Source options flyout (`SourcesWorkspaceControl.Inspector.cs:127-135`, `SourcePolicyFlyout`) opens for capture show-input slots as well as Zoom guests. `ShowSourcePolicies` becomes true for a Zoom participant or a capture device, and the flyout is keyed by canonical `sourceId`, not `ParticipantId`. No combo returns to the capture catalog row, because #590 asks for one place per control.
- The combo is enabled only when the core reports `dropoutPolicyApplies` for that source. Otherwise it is disabled, with the note: "This source does not report signal loss yet, so it always shows its last picture."
- Option labels for capture: "Hold last frame (up to 5 s, then slate)" and "Black (then slate)". The Zoom labels stay unchanged.
- **Status line, verbatim from `sources[]`:**
  - live: `Live`
  - stale-but-live: `No new frames for {lastSampleAgeMs} — holding last frame` or `— Program black`
  - lost: `Lost ({lossReason}) — holding, slate in {failInMs}`
  - failed: `Lost ({lossReason}) — slate on Program`
  - unreported: no liveness line

  The shell computes no ages and no states.

### 6. What does not change

- Zoom on-air logic, the 1.5 s Zoom hysteresis, R1, R3, R4 and the 200 ms diagnostic `health`.
- WGC, browser, SRT, NDI, stills and media: `Unreported`, so they behave exactly as today. The existing test `ANonZoomKindSourceNeverReadsStalledOnAirFromCadenceAlone` (`MediaCoreCommandTest.cpp:8562`) must stay green and is extended to a reported-but-unknown-cadence case.
- ISO recording takes capture arrivals (`takeCpuVideoFrames`), not the held re-emit. A lost device stops contributing arrivals, and the ISO writer handles that per `docs/reference/iso-recording.md`. Program recording, stream and virtual camera show what Program shows.
- Mac (AVF, Metal): Metal gets the shared `suppressFrameFor` rule (CI compile only). AVF liveness reporting is out of scope.

### 7. Vendor-named devices (#758's last done-when)

The native UVC discovery skips devices whose names contain "blackmagic" or "decklink" (`UvcCaptureDeviceAdapter.cpp:841-846`), and the shell maps those names to vendor `blackmagic`, which `NativeUvcCapturePolicy.ShouldPreferNativeCapture` (`NativeUvcCapturePolicy.cs:44-53`) keeps off the native path. With #537 unbuilt, those devices can only use the bridge. Change:

- the core skips them only when `COREVIDEO_WITH_DECKLINK` is on;
- the shell prefers native for a `blackmagic` / `aja` vendor device when the core's native list contains it as vendor `uvc`.

Both adapters then get the same liveness.

## Acceptance

All runs use a Release core (check `--config Release` and the binary size, `CLAUDE.md`), the local control API (`GET /snapshot`, `127.0.0.1:8011`), and no input injection. Removal is simulated with `pnputil /disable-device <instanceId>` and restored with `/enable-device` (UAC is off on the dev PC; record the instance id). The owner's physical unplug is the final confirmation for each adapter. Program is judged from a Program recording's decoded frames (luma and slate-signature sampling, as in `scripts/qa/zoom-gap-hold-ab.py`), not from the Studio cells.

| # | Test | Passes when |
|---|---|---|
| A1 | Native UVC webcam on Program, policy hold. Disable the device. | `sources[]` goes live → lost → failed. Program shows the held frame, then the named failed slate ≤ 2.0 s after the core observed the loss, and ≤ 5.0 s after the last sample. The command round trip stays under budget during and after (requires #993). |
| A2 | A1 with policy black. | Program is black ≤ 1 tick after observation, then the named slate at the deadline. Preview and multiview show the held frame until failed. |
| A3 | Bridged camera (`COREVIDEO_NATIVE_UVC=0`), hold, then black. Disable the device. | Same bounds as A1 and A2. The shell writes signal 2, and `unregister-capture-shm` follows ≥ 6 s later (`launch.log`). No frozen frame after 5.0 s in any run. |
| A4 | Bridged camera. Kill the shell's reader without a signal (debug hook, or suspend the shell process for 10 s). | The silence path fails the source at 5.0 s ± 1 tick. Program never keeps the frame past that. |
| A5 | Static picture: an HDMI capture card showing a fixed test card, plus a webcam with its lens covered, each on Program for 10 minutes, under both policies. | `onAir` stays `producing` the entire time. No black, no slate. |
| A6 | Unreported kinds: a static WGC window and an idle browser source with the policy set to black. | Never stalled, never black (regression). |
| A7 | Re-enable or replug, then reconnect through the existing path. | Live again on Program. No frame from the previous session appears after the new session's first frame. |
| A8 | Core respawn while a bridged device is lost. | The new core reads signal 2 from the re-announced mapping and reaches failed within 2.0 s of its first observation. |
| A9 | Sources page | Each state's status line matches `sources[]`. The combo is enabled for native and bridged cameras, and disabled with the note for WGC and browser. The choice persists across restart. |
| A10 | A Blackmagic-named UVC-class device, if one is on the rig | It connects natively when the DeckLink adapter is not built. If no such device is attached, record `MISSING_EVIDENCE`, not a pass. |

Update `docs/qa/installed-acceptance-checklist.md` row #758 to A1 and A3 (unplug → slate within the bound), and add a #562 row for A5.

## Risks

- **A device that stops delivering on static content.** Some capture cards or virtual cameras may deliver only on change, and would be falsely failed after 5 s. Mitigation: `cadenceTrusted` is per adapter, and per device if needed. The Task 1 spike records cadence for every camera-class device on the rig with a static picture. Any device that pauses gets `cadenceTrusted = false`, so only explicit loss fails it. Under hold, a false stall is invisible for 5 s, which keeps the blast radius small, but it is not zero.
- **Low frame rate in low light.** Webcams drop to about 7.5 fps, well inside 1.5 s. A device under 1 fps would read stale. None is known on the rig, and the spike checks.
- **Removal notification races the reader.** The CM callback and the read loop both stamp loss. The first wins, and the stamp is atomic. The callback must never take the adapter mutex: CM callbacks are serialized per registration, and blocking one stalls every later notification.
- **Unregister arriving before the hold window ends.** It is fail-safe: the tombstone reads failed, so Program goes to the slate sooner. It is never frozen.
- **SHM word tearing.** It is an aligned 32-bit store and load on x64. The writer uses `MemoryMappedViewAccessor.Write(12, uint)` outside the seqlock. Document this in `docs/reference/gotchas.md`.
- **Two clocks.** Only core steady time is compared. The shell's `TickCount64` never crosses.
- **Snapshot and render disagreement.** Removed by projecting the render path's applied decision (§4).
- **The #993 dependency.** Without it, a native disable can hang `disconnect`, and A1 cannot be judged. Record A1 as blocked, not failed, and run A3 independently.
- **Operator expectation of hold.** Capture hold now ends at 5 s. The label says so. If the owner prefers a longer bound, it is one constant (Q2).
- **WinUI crash class.** The flyout and status changes touch bound UI (`docs/reference/winui-crash-class-0xc000027b.md`). There must be no new `x:Bind` on recycled containers, and the flyout keeps capturing identity at open.

## Owner questions (asked with the pick-one tool, not prose)

- **Q1.** After the failure deadline, should black stay black, or go to the named slate? Default in this design: the slate, because both policies end in the honest failed state.
- **Q2.** Confirm the bounds: 1.5 s stall, 2.0 s hold after an explicit loss, 5.0 s silent-death maximum.
- **Q3.** Preview and multiview show the failed slate once the source is failed, not the held frame. Confirm.

## Out of scope

- Liveness for WGC, browser, SRT, NDI and AVF. Each is a later adapter-by-adapter opt-in with the same record.
- Automatic reconnect on replug (the existing reconnect path is used and measured, not changed).
- The #993 reader-lane isolation itself.
- A recent-window `deliveredFps` (slice 2 note).
