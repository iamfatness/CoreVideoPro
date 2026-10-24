# Capture ReadSample lane isolation: no camera may block the command lane — design

**Date:** 2026-10-23
**Status:** proposed design (needs owner review), for [#993](https://github.com/iamfatness/CoreVideoPro/issues/993)
**Base:** `main` at `1879ccb7` (includes PR #994 and PR #1035)
**Related:** #994 (merged 2026-10-21: async Source Reader callback), #811 (HD60 S+ `0x8007001F` after an immediate reconnect), #758 / #562 (disconnected-device policy; spec being written separately as `docs/superpowers/specs/2026-10-23-capture-dropout-policy-design.md`, not repeated here), #1030 / PR #1035 (bounded writer join and abandon, the precedent used here), #989 (the qualification run that found this)

## Why

### What happened

The unattended #989 qualification preflight froze before recording started.
Native process 27000 stayed alive. Snapshots aged, and `media-core-sync`,
`zoom-media-spine-sync` and `ping` all timed out. CDB stacks
(`artifacts/iso-identity-989/preflight-01/native-stacks-2.txt`) showed two
threads:

- **RPC command lane:** `MediaCore::disconnectCaptureDevice` → `UvcCaptureDeviceAdapter::disconnect` → `UvcCaptureSession::~UvcCaptureSession` → `std::thread::join`.
- **Capture worker:** `UvcCaptureSession::readLoop` → Media Foundation synchronous `IMFSourceReader::ReadSample` → `WaitForMultipleObjectsEx`.

The 2026-10-21 PR #994 comment captured the same shape on the installed
`59db262` build: control thread 0 in `MSVCP140!_Thrd_join`, and a camera worker
waiting forever in `MFReadWrite!CMFSourceReader::ReadSample/GetSampleFromSource`.
API age passed 100 s.

### The blocking call chain on `main` today (file:line at `1879ccb7`)

1. `native/src/rpc/JsonRpcServer.cpp:1172`: the RPC loop thread calls `lockCommandCore()` (`:607-613`), which takes **`coreMutex`**. `:1176` starts the `cmd.handle` hold timer.
2. `JsonRpcServer.cpp:252-258`: `disconnect-capture-device` → `MediaCore::disconnectCaptureDevice` (`native/src/core/MediaCore.cpp:523-527`).
3. `WinUiCaptureDeviceAdapter::disconnect` (`native/src/modules/WinUiCaptureDeviceAdapter.h:59`) forwards to `CompositeCaptureDevice::disconnect` (`native/src/modules/ModuleComposition.cpp:345`). That calls **every** capture adapter's `disconnect` in turn.
4. `UvcCaptureDeviceAdapter::disconnect` (`native/src/modules/UvcCaptureDeviceAdapter.cpp:677`) takes the **adapter `mutex_`** (`:678`), then calls `entry.session.reset()` (`:685`).
5. `~UvcCaptureSession` (`:161-166`) sets `stop_` and calls **`thread_.join()`** (`:164`), with no time limit.
6. The worker `run()` (`:248-309`) must now finish. Before #994 it was stuck in a synchronous `ReadSample`. After #994 the read itself is bounded (`callback.take(..., 50ms)` at `:444`), but the worker still makes these Media Foundation and driver calls, none of them bounded, before the join can return:
   - `callback->cancel()` and `reader->Flush(...)` (`:295-296`). In async mode `Flush` returns at once, and nothing waits for `OnFlush`.
   - The `IMFSourceReader` and `IMFMediaSource` releases at the end of the scope (`:297-298`). The reader shuts its source down when released, which reaches the KS driver.
   - `MFShutdown()` and `CoUninitialize()` (`:305-308`).
   - If the stop arrived during startup: `MFCreateDeviceSource` (`:274`) and `negotiate()` (`:311-414`: `GetNativeMediaType` and `SetCurrentMediaType` are driver property calls). **The no-first-frame watchdog starts only once `readLoop` begins** (`:426`, `:436`), so a device that hangs on open or format selection is not covered by it at all.

The same join also runs from `connect()` (`:658`: reconnect resets the previous session on the lane) and from `~UvcCaptureDeviceAdapter` (`:608-613`).

### What waits behind it

While the join is waiting, the lane thread holds `coreMutex` and the UVC adapter `mutex_`:

| Waiter | Why it stops |
|---|---|
| Every later RPC command, including `ping`, `media-core-sync`, `zoom-media-spine-sync`, Stop and Take | Same single RPC loop thread (`JsonRpcServer.cpp:1112-1215`). |
| The display/render tick (Program, Preview, multiview) | `JsonRpcServer.cpp:688` takes `coreMutex`. The command-handoff flag gives the waiting command priority, but the command never releases the mutex. |
| Every UVC camera's frames, not only the failed one | `captureVideoTick` (`UvcCaptureDeviceAdapter.cpp:714-717`) and `takeCpuVideoFrames` take the same adapter `mutex_`. |
| `audio.gather` and `rpc.resource-drain` | Both take `coreMutex` briefly (named guardrail sites in the soak log). |
| Shell snapshot freshness and the core supervisor's health view | No snapshot can be assembled, so the shell sees the API age climb. |

### What the lane budget looks like in production

`D:\CoreVideo-QA\soak-20261023-01\media-core-final.log` (beta `1072a50`, a
real 13-participant meeting, Program + 6 ISOs + SRT + virtual camera, about 2.5
hours) contains 25 `[lock-guardrail] ... 'cmd.handle'` aggregate lines. The
budget is `kCommandHandleBudgetUs = 1000` (`native/src/core/LockHoldGuardrail.h:43`).
At the end of the run 5,533 of 64,529 holds (8.6 %) were over 1 ms, and the
**worst hold in the whole run was 10.1 ms**. `render.display-tick` was worst at
26.2 ms against its 8 ms budget. A camera join has no upper bound at all, so one
bad device costs the lane more than five orders of magnitude beyond the worst
hold this product has ever logged. The guardrail only reports a hold after it
releases, so a hold that never releases is never logged. That is why the #989
freeze left no `[lock-guardrail]` line.

### Why #994 does not close #993

PR #994 replaced the synchronous pull with an `IMFSourceReaderCallback`
(`native/src/modules/UvcAsyncRead.h`), with one outstanding request, a 50 ms
bounded wait, and fencing for retired or late callbacks. This is the right Media
Foundation mechanism for the read, and this design keeps it. Three things are
still open:

1. **The lane still joins.** The worker now leaves `readLoop` within about 50 ms of `stop_`, but the join also waits for `Flush`, the reader and source release, and `MFShutdown`. Those calls go into the same driver that just stopped answering. A driver that will not complete a read is the most likely one to stall in source `Shutdown` too. Nothing measures whether it does.
2. **Open and negotiate are not covered.** A hang in `MFCreateDeviceSource` or `SetCurrentMediaType` is the original defect again, one step earlier.
3. **Nothing checks the property that matters.** The six `UvcAsyncReadTest` cases cover the callback's state machine. No test checks that the **command lane** stays responsive when a camera stops answering. The live check after #994 was a healthy HD60 S+ ("actual frames and responsive native commands"), not a device fault. The comment at `UvcCaptureDeviceAdapter.cpp:683-684` still claims more than the code guarantees, as the original comment the issue cites did.

### A related honesty defect this exposes

`captureVideoTick` sets `signalPresent = true` and clears `warning` whenever
`snapshot.arrival.frameId > 0` (`:735-737`). The frame ID never goes back to
zero, so **a camera whose reads stopped replying keeps reporting
`signalPresent=true` forever**. It also overwrites the warning that
`refreshLocked` sets for "Camera is no longer present (unplugged?)" (`:900-906`),
while `connectionState` stays `error`. Under the owner's honest-reporting rule,
liveness must be measured or absent. Today it is latched.

**Owner rules this design implements:** no brittle paths. Failures are loud.
**The command lane never waits on a device.** A disconnected device drops from
Program per the #758 policy. Device state is measured or absent.

## Decision: both mechanisms, with async read primary and bounded release as the backstop

### Options considered

| Option | Lane bounded? | Device actually released on unplug? | Verdict |
|---|---|---|---|
| A. Async `ReadSample` with `IMFSourceReaderCallback` only (what #994 shipped) | Only for the read. Join still waits for Flush, release, Shutdown, open and negotiate. | Yes, in the common case: `Flush` cancels pending requests (documented), so teardown proceeds. | Necessary, not sufficient. |
| B. A dedicated reader thread with **synchronous** `ReadSample`, plus bounded join and abandon | Yes | **No.** An abandoned synchronous read keeps the `IMFSourceReader`, the source and the KS pin open in-process for as long as the driver withholds the reply, and with the device lost that can be forever. Nothing in-process can cancel a synchronous `ReadSample`. | Rejected on its own. |
| **C. A + B: async read, plus off-lane retirement with a bounded join and abandon, plus a reopen fence** | Yes, for every Media Foundation call. | Yes in the common case (A). In the residual case the device is reported, loudly, as not released. | **Chosen.** |

### The HD60 S+ `0x8007001F` evidence and why it rules out option B alone

#811 (2026-10-07, before #994): the harness disconnected and **immediately**
reconnected the HD60 S+ in the same core. Negotiation succeeded, then the
**first read** returned `0x8007001F` (`ERROR_GEN_FAILURE`, "a device attached to
the system is not functioning"). The follow-up comment on #811 reproduced it on the
installed October 5 beta `77bac0c` with preparation off. On that code,
`disconnect` **fully joined** the old session, including reader release, source
shutdown and `MFShutdown`, before the reconnect began. The failure still
happened.

Two conclusions follow, and the design rests on both:

1. **In-process teardown finishing does not make a capture device reopenable.** The driver, or the Windows camera Frame Server that brokers it, can still be releasing after the last COM reference is gone. Opening a second reader into that window fails at the first read. Option B makes this much worse: it would deliberately leave the old reader **open** and start a new one against the same device. That is #811 made routine.
2. **A fast reconnect has to be sequential and observable, never two readers at once.** This design adds a **reopen fence**: a device whose previous session has not exited cannot be opened again. The connect is held as `connecting` until the predecessor exits, or reported as an error if the predecessor is abandoned. The shell must not fall back to its WinRT MediaCapture bridge for that device either, because that would be the same two-reader collision through a different API.

This design **does not claim to fix #811.** It adds the per-phase release
timestamps that #811's "done when" asks for (the failing native boundary), and
it guarantees that a `0x8007001F` reconnect fails loudly with its `hr`. It never
stalls the lane. #811 stays open for its own root cause.

### Media Foundation semantics relied on (verify each in Task 1 against the linked pages)

- In async mode, `IMFSourceReader::ReadSample` returns immediately with null output parameters, and the result arrives on `OnReadSample` ([ReadSample](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereader-readsample)).
- `IMFSourceReader::Flush` discards pending samples and cancels pending requests. In async mode it returns immediately and completion is signalled by `OnFlush` ([Flush](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereader-flush)). Today's code issues `Flush` and releases the reader without waiting for `OnFlush`. This design waits for it, with a bound, and records whether it arrived.
- A reader created by `MFCreateSourceReaderFromMediaSource` shuts down its source when it is released, unless `MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN` is set ([MFCreateSourceReaderFromMediaSource](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-mfcreatesourcereaderfrommediasource)). That release is the call most likely to block in a wedged driver, and #811 notes that a missing explicit `Shutdown` is not proven as a root cause. This design does not add an explicit `Shutdown` on faith. Task 1 measures the release phase, and any change goes in as an A/B with evidence.
- No Media Foundation call can be cancelled from another thread once it is inside the driver. The only safe response to a call that never returns is to **stop waiting for it** and keep everything it touches alive until it does return. This is the #1030 contract.

## Design

### 1. The session owns its own lifetime, so it can be abandoned safely

`UvcCaptureSession` becomes a thin handle over `std::shared_ptr<UvcSessionState>`.
The worker thread's lambda captures its own `shared_ptr` copy. Everything the
worker can touch is reachable only through that state, or is reference-counted
on its own:

- `UvcAsyncRead` (already COM ref-counted, and already holds no pointer to the session or adapter).
- `I420CaptureArrival`, `UvcIsoArrivalQueue` and `CapturePresentationWindow` (held in the state).
- `std::shared_ptr<I420SourcePreparation>` (already a `shared_ptr`).
- The backend (Section 3) and its COM objects.

No raw `this` pointer to the adapter, the `DeviceEntry` or `MediaCore` crosses
into the worker. A worker that returns late, after its adapter has forgotten it,
only touches memory it co-owns. Its frames go into an arrival queue nobody
drains. When the predecessor is retired, the device entry gets a new session and
a new source epoch, so a late frame cannot reach Program (`I420CaptureArrival`
already stamps `sourceEpoch`).

The last thing the worker does is set `exited = true` under the state mutex and
notify, the same way `RecordingTrackWorker::run` does (`RecordingTrackWorker.h:176-180`).

### 2. The lane only signals: `BoundedSessionReaper`

A new header-only helper, `native/src/modules/BoundedSessionReaper.h`, owns
retirement off the lane. It is a new shape, modelled on but not reusing `RecordingTrackWorker::joinFor/abandon`
(PR #1035) and `CaptureSessionLifecycle`, under the rule from `CaptureSessionLifecycle.h:15-16`:
"Creation and stop/join belong to this owner, never to a poll/render caller."

```text
lane:    disconnect(id) -> state.requestStop(); callback.cancel()   [microseconds, no MF call]
                        -> reaper.retire(id, handle)                 [enqueue, never waits]
reaper:  joinFor(handle, kUvcSessionReleaseBoundMs)
           exited  -> join (instant), emit release evidence, lift fence(id)
           timeout -> abandon: detach thread, keep shared state alive,
                      mark id "release-stalled" (loud), keep watching;
                      a late exit lifts the fence and logs "late release"
```

- **`retire()` never blocks.** It takes only the reaper's own leaf mutex for an O(1) insert.
- **There is one reaper thread per adapter.** It waits for one session at a time, at most `kUvcSessionReleaseBoundMs` each, so one wedged device delays other devices' release *evidence* by at most one bound. It never delays their commands, because commands never wait on the reaper.
- **Abandoned sessions are capped** (`kMaxAbandonedUvcSessions = 4`). Past the cap, every new UVC connect is refused with a loud adapter-level error ("N camera sessions did not release; restart CoreVideo"). That keeps a flapping device from leaking threads without limit. The cap is evidence, not a timeout tweak.
- **Shutdown is bounded.** `~UvcCaptureDeviceAdapter` asks every session to stop, gives all of them one shared deadline (`kUvcSessionReleaseBoundMs`), and abandons the rest. This matches the module-shutdown order in `docs/reference/engine-teardown-order.md`: the process exit must never hang on a camera.
- **`kUvcSessionReleaseBoundMs` = 4000 ms**, the same as #1030's writer bound, to be confirmed by Task 1's measurement: at least 10x the worst measured healthy release on the dev box. The lane never waits on this value. It only decides when the operator is told "this camera did not let go".

### 3. A backend seam, so faults can be injected without hardware

The Media Foundation calls move behind an internal interface,
`native/src/modules/UvcCaptureBackend.h`:

```cpp
struct UvcNegotiated { int width, height, fps; std::string fourcc; bool nv12, fullRange, bt601;
                       uint32_t rateNum, rateDen; };
class IUvcCaptureBackend {           // one instance per session, worker-thread only
 public:
  virtual ~IUvcCaptureBackend() = default;
  virtual HRESULT open(const std::wstring& symbolicLink, IMFSourceReaderCallback* cb) = 0;
  virtual bool negotiate(UvcNegotiated& out, std::string& error) = 0;
  virtual HRESULT requestRead() = 0;                  // async ReadSample
  virtual bool refreshOutputType(UvcNegotiated& out) = 0;
  virtual HRESULT flush() = 0;                        // async Flush
  virtual void release() = 0;                         // reader/source Release (may block in the driver)
};
```

`MfUvcCaptureBackend` is today's code, moved and not changed. Tests get a
`FakeUvcCaptureBackend` that can hold `open`, `negotiate` or `release` on a
latch the test controls, accept a request and never reply, reply late after
cancellation, return `0x8007001F` on the first read, or deliver real NV12 bytes.
The adapter gains a test-only factory (`createUvcCaptureDeviceForTest(backendFactory, discover)`),
declared in a header that nothing outside `native/tests/` includes. That is the
structural guard required by `docs/reference/fault-injection-seams.md`.

### 4. Watchdog over every phase, not only the first read

The session publishes an atomic **phase** (`opening`, `negotiating`, `streaming`,
`releasing`, `exited`) and the steady-clock time it entered that phase. The
worker cannot run a watchdog while it is stuck inside a driver call, so the
**adapter** reads phase age on its tick, without joining and without any Media
Foundation call:

- `opening` or `negotiating` longer than `kUvcNoFirstFrameTimeoutMs` (4000 ms, `UvcCaptureSupport.h:120`): `connectionState = "error"`, warning "Camera open did not complete in 4 s (driver not responding)". The session is retired through the reaper. Abandon applies if it never returns.
- `streaming` with no first frame: the existing watchdog (`:436`), unchanged.
- A failed read (`FAILED(hr)`, `MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED`, `ENDOFSTREAM`, `ERROR`): unchanged. The failure is loud and includes the `hr`.

### 5. The reconnect fence

`connect(id)` checks the reaper first:

| Predecessor | Result | Reported |
|---|---|---|
| None | Create session (thread spawn only, as today) | `connected`, "Waiting for the first camera frame." (unchanged contract) |
| Retiring, inside the bound | Record the connect intent. The reaper starts the session when the predecessor exits. | `connecting`, "Previous camera session is still releasing the device (N ms)." |
| Abandoned | Refuse | `error`, "Camera did not release after disconnect (N s). Unplug and replug it, or restart CoreVideo." |

A pending intent is dropped by a later `disconnect(id)`. Intents coalesce to the
newest one, as in `CaptureSessionLifecycle`. The reaper starts the successor
from its own thread, after it has released its own mutex, by calling a small
adapter hook that takes the adapter `mutex_` briefly. The lock order is
`adapter.mutex_ → reaper.mutex_` on the lane, and the reaper never holds its
mutex while calling the hook, so the order cannot invert.

### 6. The device-liveness signal this produces for #758

The capture-dropout spec decides what Program does (hold or black, hysteresis).
This spec supplies **measured facts** for it, per UVC device, in the existing
`CaptureDeviceInfo` (`Interfaces.h:1023-1060`) and the `capture-devices` and
snapshot projection (`CoreStateCapture.cpp:303`, `MediaCore.cpp:287`):

| Field | Source | Absent when |
|---|---|---|
| `lastFrameAgeMs` (exists, optional) | now minus the steady time of the last sample that produced pixels | No frame was ever delivered in this session. Never reported as 0. |
| `readReplyAgeMs` (new, optional) | now minus the last `OnReadSample` of any kind (sample, stream tick, error) | No request has been answered yet |
| `readOutstandingMs` (new, optional) | age of the currently pending request | No request is pending |
| `sessionPhase` (new, string) | Section 4 phase, plus `release-stalled` | No session |
| `releaseMs` (new, optional) | stop request to worker exit, for the last retired session | No session retired yet |
| `signalPresent` (exists) | **now derived:** `lastFrameAgeMs <= kUvcSignalLossMs` | (false) |
| `connectionState` (exists) | `connected` / `connecting` / `detected` / `error`. Section 5 adds `connecting`. | |

`kUvcSignalLossMs` = 1500, matching the NDI receive precedent
(`NdiReceiveCaptureAdapter.cpp:214`). The #758 spec owns the on-air threshold
and may use any field. This spec only stops the latch: a device whose reads
stopped replying now loses `signalPresent` within 1.5 s, and its warning is no
longer cleared while `connectionState` is `error`. Device removal shows up as
(a) a failed read with `hr`, (b) `readOutstandingMs` growing with no reply, or
(c) the device dropping out of periodic discovery (2 s, `refreshLocked`). Each
is a measured fact. Nothing is inferred from frame-ID cadence (the #562 trap).

### 7. Release evidence (for #811 and for the bound)

Each retirement emits one line:

```text
[uvc-capture] release '<id>' session=<n> stop->loop-exit=<ms> flush=<ms>(onFlush=<0|1>) reader-release=<ms> mf-shutdown=<ms> total=<ms> outcome=<exited|abandoned|late-exit>
```

The same numbers appear in the device's evidence (`releaseMs`, and an
`abandonedSessions` count on the adapter's capability or diagnostics node). A
support bundle can then tell whether a camera wedged and in which phase.

### 8. What changes in the shell

`NativeUvcCapturePolicy.FindConnectedDevice` accepts only `connected`, and
`TryConnectNativeUvcCaptureAsync` (`StudioViewModel.cs:6736-6790`) falls back to
the WinRT bridge on anything else. With the fence:

- A connect answered with `connecting` is **native-pending**, not a failure. The shell keeps polling through its existing `ConfirmNativeFirstFrameAsync` window, and the bridge is not opened.
- While the core reports this device `release-stalled`, or `error` with a release-stall warning, the shell **does not open the bridge** for that device (no second reader). It shows the core's warning.
- The shell's own no-first-frame fallback (disconnect native, then open the bridge) waits until the core reports the device released (`detected`) before it opens the bridge, with the same bound. If release stalls, the device shows the error and stays off the bridge.

## Acceptance

All automated tests run on the Windows dev core (`--config Release`, the in-house
gtest shim). Stub or mac CI cannot compile the Media Foundation paths.

1. **Lane bound under an injected never-replying read** (`JsonRpcServerTest`, fake backend): connect, wait for frames, make the fake stop answering (request accepted, no callback ever), then send `disconnect-capture-device`, `ping` and `media-core-sync`. All three are answered, and `server.run` finishes in **under 2 s** while the fake's `release()` is still blocked. The `cmd.handle` guardrail's worst hold for this run is **under 5 ms**. The core's sanctioned budget is 1 ms. The 5 ms test ceiling allows for CI jitter and is still 3 orders of magnitude below "unbounded".
2. **Lane bound under each Media Foundation hang** (adapter tests): `open`, `negotiate` and `release` each blocked on a latch. `disconnect()` returns in under 50 ms of caller wall time, and `connect()` for another device on the same adapter returns in under 50 ms. `captureVideoTick` for a healthy second device keeps publishing frames throughout.
3. **Honest state:** a never-replying device loses `signalPresent` within `kUvcSignalLossMs + one tick`, `readOutstandingMs` grows, and `lastFrameAgeMs` is present and growing. A device that never produced a frame has no `lastFrameAgeMs`. An abandoned session shows `release-stalled` and a loud warning. A late exit lifts the fence and logs `outcome=late-exit`.
4. **Reconnect fence:** connecting while the predecessor is retiring reports `connecting` and starts exactly one successor after the exit. The fake records that no two sessions were ever open on one symbolic link. Connecting while the predecessor is abandoned is refused with the error.
5. **Preserved behavior:** the existing `UvcAsyncReadTest` (6), `UvcCaptureSupportTest`, `UvcIsoArrivalQueueTest`, ISO and delivery-trace suites pass unchanged. One outstanding request. Cadence is preserved by priming before conversion. Samples, timestamps, geometry and color hints are unchanged.
6. **Live, injected fault on the dev box** (`COREVIDEO_QA_UVC_FAULT=read-no-reply`, a headless QA knob that is off unless all its values are valid, following `RecordingWriteStallQa.h`): on a real webcam and on the HD60 S+, the drill measures `ping` round-trip at 10 Hz before, during and after a disconnect of the faulted device. Pass: no `ping` above **250 ms**, no `[cmd] 'disconnect-capture-device' ... held core lock` line, Program delivery for the other sources has zero underruns over the window, and the release line is logged. The same drill with `release-hang` shows `release-stalled` after 4 s, the lane stays responsive, and no bridge reader is opened.
7. **Real unplug on the dev box** (the owner physically unplugs, because this product does not inject input): with the webcam and the HD60 S+ live on Program, unplug each in turn. Pass: the lane stays responsive (same `ping` bound), the device moves to `error` or `signalPresent=false` with measured ages within 1.5 s or within the discovery interval (whichever the evidence shows, recorded), the release line is logged, and the other camera keeps delivering. Replug, then reconnect. The outcome is recorded, including any `0x8007001F`, which is reported against #811 and does not count against #993.

**Not established by this work** (say so in the PR): any claim about #811's root
cause, the full #989 workload, installed-shortcut acceptance, or an
unlimited-duration guarantee. A finite drill bounds only what it observed.

## Risks

| Risk | Mitigation |
|---|---|
| An abandoned worker returns after `MediaCore` or the logger is gone at process exit | Bounded shutdown abandons only at exit, and the state is self-owned. The residual risk is a log write during CRT teardown. Task 5 checks it with a fake hung release across `~MediaCore`, and the PR names it if it is not closed. |
| A leaked thread and driver handle per wedged unplug | `kMaxAbandonedUvcSessions` cap with a loud refusal. The operator is told to replug or restart, which is the only real recovery from a wedged driver anyway. |
| `connecting` is a new state the shell has never seen from UVC | Task 7 changes the shell policy, with tests. The core still answers `connected` in the normal case, so an old shell degrades to today's bridge fallback only in the fenced case. Section 8 explains why that fallback is unsafe, so the core change and the shell change ship together in one PR. |
| Waiting for `OnFlush` changes teardown timing | The wait is bounded (500 ms) and logged (`onFlush=0/1`). Task 1 measures before and after on both cameras. |
| The backend extraction changes behavior | Task 2 is a pure move with the existing suites as its gate. No logic edits are allowed in the same commit. |
| Fault knob in a shipped binary (`fault-injection-seams.md` prefers structural seams) | The unit and RPC tests use the structural seam only. The live knob copies the precedent from #1030 (`RecordingWriteStallQa`): off unless every value is valid, a hard ceiling (release hang at most 10 s), and one log line per use. The reviewer decides whether a live knob is acceptable. Without it, acceptance item 6 becomes a fake-backend drill inside the real core. |
| A camera reporting `signalPresent=false` on a legitimately static scene | Media Foundation capture delivers samples at the negotiated rate even for a static picture. The age measures sample arrival, not frame change, so #562's change-driven trap does not apply. If a device is found that withholds samples on a static input, it is filed, not hidden. |
| Discovery-based removal is 2 s, slower than a PnP notification | Accepted for this slice. `CM_Register_Notification` on the device interface is the faster signal. It is listed out of scope with its hazard: it must not be unregistered from inside its own callback. |

## Out of scope

- The Program-side dropout policy and the shell bridge's own disconnect behavior (#758, #562): that is the capture-dropout spec.
- The root cause of #811's `0x8007001F` and any reconnect delay. #811 says explicitly not to add one. This work supplies the evidence and the fence only.
- AVFoundation (`AvfCaptureDeviceAdapter.mm`) and the WinUI bridge adapter. They have their own lifecycles. WGC already uses `CaptureSessionLifecycle`.
- PnP removal notifications (`CM_Register_Notification`).
- Lifting `cmd.handle`'s general 8.6 % over-budget rate in the soak. That is a separate, measured backlog item.
