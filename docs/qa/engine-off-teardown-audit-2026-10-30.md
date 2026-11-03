# Engine-off teardown audit against the five ZoomISO rules (#445, T4.3)

_Audit date 2026-10-30, code at `main` 157add46. Rules are quoted from
`docs/reference/engine-teardown-order.md`; this document records what the code
does today, rule by rule, with the file and line of each claim. It proposes no
code change; the follow-ups at the end are filed as issues or left to the
owner. Lines move; the function names will not._

| Rule | Verdict | One line |
|---|---|---|
| 1. Engine exit order | **PARTIAL** | The ordered block exists and is bounded; it runs on `quit` and on pipe EOF, but any kill path (plain `Stop()`, app-exit kill fallback, core crash) skips it, and on app exit it is capped at 1 s before `TerminateProcess`. |
| 2. Callback vs teardown serialize | **HOLDS** | `~ParticipantSubscription` flags, drains, then makes SDK calls unlocked. EngineShare now uses `RendererCallbackGate` (suspend + unlock around every SDK call), not the single mutex the doc describes. |
| 3. EngineVideo map lock order | **HOLDS** | Both maps under `m_mtx`; every create/destroy outside it; `m_mtx` -> `m_targets_mtx` only. |
| 4. Shell stops off the UI thread | **PARTIAL** | App exit and the post-failure force stop are off the UI thread; leave-meeting and respawn no longer stop the core at all (doc stale); one `Stop()` still runs synchronously on the UI thread (bridge health change), cheap in-process, blocking in hosted mode. |
| 5. Never delete the vcam SHM in `stop()` | **HOLDS** | `stop()` unmaps and closes only; the writer reopens in place; no production code deletes the slot; the #563 test redirect is pinned. |

## Rule 1: engine exit order

Rule: `Leave()` -> `stop_raw_media("shutdown")` -> bounded message-pump drain
(~250 ms) -> `share_engine.detach()` / audio shutdown -> `CleanUPSDK()`;
renderer destructors never after `CleanUPSDK()`.

**The block exists and matches**, `native/zoom-engine/engine/main.cpp:1647-1678`:

```cpp
running.store(false, ...); if (heartbeat.joinable()) heartbeat.join();      // 1648-1649
if (meeting_svc) meeting_svc->Leave(ZOOMSDK::LEAVE_MEETING);                // 1651
meeting_event.stop_raw_media("shutdown");                                   // 1660
for (int i = 0; i < 5; ++i) { pump_windows_messages(); sleep_for(50ms); }   // 1667-1670
pump_windows_messages();                                                    // 1671
share_engine.detach();                                                      // 1674
EngineAudio::instance().shutdown();                                         // 1675
ZOOMSDK::CleanUPSDK();                                                      // 1676
```

- The drain is bounded: five 50 ms sleeps plus non-blocking `PeekMessage`
  passes (`main.cpp:174-181`).
- `stop_raw_media` (`main.cpp:1051-1078`) calls `StopRawRecording()`, then
  video `set_raw_media_active(false)` + `unsubscribe_all()`, share
  `set_raw_media_active(false)` (destroys the share renderer,
  `engine-share.cpp:71-81`, keeps share targets and SHM), audio
  `set_raw_media_active(false)` + `reset_subscription(...)`. "unsubscribe_all"
  is literal for video only.
- No renderer destructor runs after `CleanUPSDK()`: `EngineVideo::m_subs` is
  emptied first (`engine-video.cpp:681-692`); `~EngineShare` -> `detach()` finds
  renderer and controller already null. Re-arms during the drain are gated on
  `m_raw_media_requested`, false after the stop (`main.cpp:1253, 1271`); the
  live-stream re-arm (`1320-1327`) is compiled out (`COREVIDEO_HAS_LIVE_STREAM_CTRL`
  is never defined).

**Which stop paths reach it:**

| Path | Reaches the ordered block? |
|---|---|
| `quit` on the IPC pipe (`main.cpp:1436-1437`) | Yes. The core rarely sends it: `ZoomEngineProcessClient::stop()` only sends `quit` while the pipe is connected (`ZoomEngineProcess.cpp:210-211`), and every runtime path calls `terminate()`, which runs `closeIpc()` first (`204-207`). |
| Pipe EOF (the engine reads a named pipe, not stdin) | Yes. `ipc_read_line_with_message_pump` returns false (`main.cpp:201, 214-218`), the loop breaks (`1430`), the block runs. |
| App exit: `~ZoomEngineRuntime` -> `stopReader()` -> `process->terminate()` (`ZoomEngineRuntime.cpp:107-108, 1040`) | Attempted with a **1 s cap**: `terminate()` closes the IPC handles (the EOF path above) then `WaitForSingleObject(..., 1000)` and `TerminateProcess` on timeout. |
| Operator leave | Not a teardown: `ZoomEngineRuntime::leave()` queues `"leave"` (`ZoomEngineRuntime.cpp:378`); the engine calls `Leave()` (`main.cpp:1591-1593`), the `DISCONNECTING`/`ENDED` callback runs `stop_raw_media("meeting_left")` and detaches participants and share (`1165-1193`). The engine stays alive with the SDK loaded until the next join terminates it (`restartBeforeJoin_` -> `stopReader()`, `ZoomEngineRuntime.cpp:225-228`). |
| Kill by the shell: `Process.Kill(entireProcessTree: true)` (`MediaCoreSupervisor.cs:1908`); the engine is also in a `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` job (`ZoomEngineProcess.cpp:141-169`) | **No.** No Leave, no `stop_raw_media`, no `CleanUPSDK`. This is plain `Stop()`, the app-exit kill fallback after the 2 s grace, and any core crash. |

The reference doc's "remaining gap" (app close terminates the engine without
the ordered exit) is therefore half right: a graceful app exit now attempts the
order through the EOF path, bounded at 1 s; the gap is real on every kill path.

**Deviations worth an issue:** IPC commands are matched by substring
(`line.find(IPC_CMD_QUIT)`, then `"init"`, `"join"`, `"leave"`,
`main.cpp:1436-1598`), so a join line whose display name contains `quit` is
treated as quit. No test pins the engine `main()` teardown order;
`ZoomEngineRuntimeTest.cpp:432` only checks that the runtime destructor does
not hang, against a fake client.

## Rule 2: callback vs teardown must serialize

Rule: `~ParticipantSubscription` sets `m_stopping` first, drains
`m_targets_mtx` (acquire + release), then `unSubscribe()`/`destroyRenderer()`,
never holding a mutex across those SDK calls; EngineShare's single-mutex
pattern is the other accepted shape.

**Holds.** `engine-video.cpp:118-141`:

```cpp
m_stopping.store(true, std::memory_order_release);
{ std::lock_guard<std::mutex> drain(m_targets_mtx); }
if (m_renderer) { m_renderer->unSubscribe(); ZOOMSDK::destroyRenderer(m_renderer); m_renderer = nullptr; }
std::lock_guard<std::mutex> lock(m_targets_mtx);  // SHM free only
```

Both callbacks check `m_stopping` before and after taking the lock
(`209/224`, `353/355`). Residual window: a frame callback past the first check
(`209`) but not yet holding the lock (`221`) is not covered by the drain;
whether the object can be freed under it depends on `destroyRenderer` waiting
for in-flight callbacks, which the code comment says cannot be proven.
`onRendererBeDestroyed` writes `m_renderer` without synchronization (`365-368`).

**EngineShare's shape has changed** since the rule was written: it uses
`RendererCallbackGate` (`native/zoom-engine/shared/renderer-callback-gate.h`).
`Transition` sets `suspended` and **unlocks** `m_mtx` for the duration of every
SDK call (`SetEvent`, `GetViewableSharingUserList`, `createRenderer`,
`setRawDataResolution`, `subscribe`, `unSubscribe`, `destroyRenderer`;
`engine-share.cpp:45-65, 140, 198-250`). Renderer callbacks use `enter()`,
which returns no lock while suspended (`301-302, 309-310, 391-392`);
share-controller events set `m_pending_share_refresh` instead (`403-406,
442-445`). The share deadlock fix (PR #791 for #790: `onRawDataStatusChanged`
on the same thread during an SDK call) is still in place and pinned by
`native/tests/EngineShareLifecycleTest.cpp:79`
(`ShareEndAllowsSynchronousRendererCallbacks`) and lines 85, 114, 136, 149,
167, 189. Remaining risk: `lifecycle()` waits on a condition variable while
suspended, so a same-thread `detach()` or `set_raw_media_active()` arriving
from inside a share SDK call would wait forever; no test rules that out.

## Rule 3: EngineVideo subscription maps

Rule: maps guarded by `m_mtx`; SDK calls outside the lock; lock order
`m_mtx` -> `m_targets_mtx`, never reversed.

**Holds.** `engine-video.h:100-108` guards both maps with `m_mtx`.
`subscribe()` declares `retired` before the `unique_lock` so retired
subscriptions die after release (`376-379`) and unlocks around both
constructions (`483-488, 541-545`); `unsubscribe()` (`586-594`),
`resubscribe_all()` (`665-667`) and `unsubscribe_all()` (`681-692`) move entries
out under the lock and destroy after release. Under `m_mtx` the only nested
acquisitions are `m_targets_mtx` (through `sources()`, `add_source`,
`remove_source`, `empty`, `target_count`); the frame callback takes only
`m_targets_mtx`; no reversal found. Minor: `EngineIpc::write` (a blocking pipe
write, not an SDK call) runs under `m_mtx`; `active()` reads `m_renderer`
without the lock.

## Rule 4: shell stops off the UI thread

Rule: every core stop rides `Task.Run`; leave-meeting, respawn and the
post-failure `ForceStopMediaCoreAsync` use `_bridge.Stop()` (kill-tree +
`WaitForExit(1500)` under the supervisor gate); app exit uses
`StopForAppExit(2 s)`.

| Call site | Today |
|---|---|
| Leave meeting | **No core stop any more.** `SettingsViewModel.cs:917-944` calls `_bridge.LeaveZoomAsync()`; its comment says the media core stays up and `_bridge.Stop()` is the app-exit path only. The comment's claim that zoom-leave makes the engine exit cleanly is wrong: the engine stays alive until the next join (Rule 1). |
| Respawn | **No `Stop()`.** `OnChildExited` -> `SpawnChild()` under `_gate` (`MediaCoreSupervisor.cs:1519-1528`); the old child has already exited. |
| `ForceStopMediaCoreAsync` | Off the UI thread as documented: `await Task.Run(() => ViewModel.ForceStopMediaCoreAsync()).WaitAsync(1 s)` (`MainWindow.xaml.cs:885`) -> `ForceShutdownMediaCore()` -> `_bridge.Stop()` (`StudioViewModel.cs:14499-14562`). |
| App exit | Off the UI thread as documented: `Task.Run(async () => await ViewModel.DisposeAsync())` with the 5 s `ShutdownTimeout` (`MainWindow.xaml.cs:872-876`) -> `ForceShutdownMediaCore(ShutdownBudget.CoreExitGrace)` (2 s) -> `_bridge.StopForAppExit` (`StudioViewModel.cs:14524, 14554`); worst case 2 s + `KillWaitMilliseconds = 1500` (`ShutdownBudget.cs`, `MediaCoreSupervisor.cs:191`). |
| Hosted mode | `DetachHostedEngineOnExit` -> `StopForAppExit(TimeSpan.Zero)` (`StudioViewModel.EngineHost.cs:24`) detaches only, never kills. |
| Bridge health change | **A `Stop()` on the UI thread**: `OnBridgeHealthChanged` -> `RunOnUiThread(ApplyBridgeHealthChanged)` -> `StopMediaCoreSession` -> `_bridge.Stop()` (`StudioViewModel.cs:10077-10078, 10166, 10563`). Cheap when there is no live child (`StopChild` finds `_process` null, the normal case after `Stopped`), but in hosted mode `Stop()` is `_hostClient.ControlAsync("shutdown").GetAwaiter().GetResult()` (`MediaCoreSupervisor.cs:197-201`), a blocking wait on the UI thread. Unhooked during shutdown (`StudioViewModel.SyncOwnership.cs:31`). |

"Under the supervisor gate" is inaccurate: `StopChild` only retires state under
`lock (_gate)` (`227-257`); closing stdin, `Kill(entireProcessTree)` and
`WaitForExit(1500)` run in `DisposeChild` outside the gate (`273, 1858-1923`).
Plain `Stop()` gives no grace: stdin is closed and the tree killed at once, so
neither the core's EOF path nor the engine's ordered exit runs.

## Rule 5: never delete the vcam SHM file in `stop()`

**Holds.** `VirtualCameraPublisher.cpp:242-285`: `stop()` stops the publish
thread, `history_.stop()`, `camera_->Stop()`/`Remove()`, then
`UnmapViewOfFile(view_)`, `CloseHandle(mapping_)`, `CloseHandle(shmFile_)`, with
the comment "NEVER delete the slot file here". The writer reopens in place:
`CreateFileA(..., OPEN_ALWAYS ...)` with `FILE_SHARE_DELETE` and "NEVER
DeleteFileA here" (`VirtualCameraShm.h:104-146`); `start()` re-initializes the
header under the seqlock (`VirtualCameraPublisher.cpp:80-94`). History and
correlation sidecars only unmap and close (`VirtualCameraHistory.h:56-61,
87-94`; `VirtualCameraCorrelationMapping.h:24-29`). `DeleteFile`, `remove` and
`unlink` appear only in comments and tests. The vcam names are fixed
(`vcam-frame.shm`, `vcam-history-v1.shm`, `vcam-correlation-v1.shm`); the
PR #189 "a section cannot grow" lesson lives elsewhere (engine regions sized to
capacity with an instance-token prefix, `engine-video.cpp:192-201`; capture SHM
names `..._{Pid}_{device}_{w}x{h}`, `CaptureDeviceSharedMemoryWriter.cs:141`).
The #563 test redirect `COREVIDEO_VCAM_SHM_DIR` (`VirtualCameraShm.h:59-71`) is
set at static-init in `native/tests/VirtualCameraShmRoundtripTest.cpp:55-80` and
pinned by `TheSuiteNeverResolvesTheProductionSlotPath` (line 261).

## Core shutdown on stdin EOF, and where writers finalize

1. The reader sets `inputClosed` (`JsonRpcServer.cpp:627-631`); the command
   loop breaks (`1360-1365`).
2. Teardown joins the join worker, calls `assembly.finish()`, then joins the
   render, resource, preview, zoom-pump, video-output, ISO-video, audio and
   reader threads; remaining events are drained and the writer joined last
   (`1370-1432`).
3. `main` leaves scope (`native/src/main.cpp:23-27`): `server`, then
   `mediaCore`. `~MediaCore` only clears the vcam sink (`MediaCore.cpp:492-500`);
   members then destroy in reverse declaration order: `virtualCamera_`
   (`MediaCore.h:1044`), `zoomEngineRuntime_` (`1033`), and last `modules_`
   (`680`), which owns the encoder and the output sender.

**Finalize runs after engine teardown, not before.** Nothing on this path stops
recording or ISO output explicitly; Program and ISO files finalize only in
`~AsyncEncoderSink` (4 s `finalizeGrace`, `AsyncEncoderSink.h:78`) and
`~MediaFoundationEncoderSink` -> `closeWriters()`
(`MediaFoundationEncoderAdapter.cpp:1544-1546`), which run after
`~ZoomEngineRuntime` (up to about 2 s: a 1 s wait, `TerminateProcess`, another
1 s wait). That sits inside the shell's 2 s `CoreExitGrace`, after which the
tree is killed, so outputs still open when stdin closes can lose their
finalize to the engine teardown. The normal "Stop and close" flow avoids this
because `OutputShutdownCoordinator` stops outputs before `ShutdownAsync`.

## Follow-ups

- **Kill paths skip the ordered engine exit** (Rule 1): plain `Stop()`, the
  app-exit fallback and a core crash. Decide whether plain `Stop()` should close
  stdin and wait a bounded grace (as `StopForAppExit` does) before the kill.
- **EOF path finalizes outputs after the engine** and inside the same 2 s
  grace. Either stop the encoder sinks before `zoomEngineRuntime_` destroys
  (destroy order or an explicit stop in the EOF teardown), or lengthen the
  grace when a recording lifecycle is open.
- **Substring IPC command matching** in the engine `main()`.
- **UI-thread `Stop()` from the bridge health change** in hosted mode.
- **No test pins the engine `main()` teardown order.**
- **Reference doc drift** (`docs/reference/engine-teardown-order.md`), corrected
  in the same change as this audit: leave-meeting and respawn no longer stop the
  core; "under the supervisor gate" covers only state retirement; EngineShare
  uses `RendererCallbackGate`; the app-exit path attempts the ordered exit with
  a 1 s cap.
