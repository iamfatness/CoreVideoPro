# Live Tiles validation: runtime publisher warning

The public-style alpha newly placed Visual C++ runtime DLLs beside the Zoom
helper. During the live test, the helper blocked inside `sdk!InitSDK` in a
`USER32!MessageBoxW` dialog titled **Zoom Workplace**. The SDK contains the text
“from an unknown publisher.” This was not adequately explained by the initial
assumption that ordinary Windows unsigned-application prompts caused it.

The saved SDK verifier frame's module argument (`r15=00007ffb23870000`) resolves
to the packaged **MSVCP140.dll**. Windows Authenticode validates that runtime;
the reported signer is Microsoft Windows Software Compatibility Publisher.
The SDK includes a Microsoft Corporation publisher check. The old working app
folder did not contain the app-local CRTs. No security policy was disabled or
publisher check patched.

An isolated SDK folder without app-local CRTs passed credential-free SDK
initialization. The first attempt to use that folder in the app was invalid:
`MediaCorePaths` overwrote `COREVIDEO_ZOOM_ENGINE_PATH` with the root helper path.
The resolver now honors explicit paths, rejects invalid overrides, and prefers
the isolated packaged SDK helper before legacy paths. Its 16 tests passed.

Auth/join timeouts also left an SDK generation alive and allowed reinitializing
it after late callbacks. The recovery fix quarantines timed-out events and
restarts the helper before retry; all 16 Zoom runtime tests passed.

With both fixes and the actual isolated helper path verified, live join succeeded
in about six seconds. Capture started and 20 API-driven scene selections/Takes
passed with native Program identity/frame checks, restoration, and maximum HTTP
latency of 80 ms. Seven live Tiles sources were admitted.

A 60-minute API soak began at **17:44:50 UTC**, using the 3-frame buffer. Reports
are local at `artifacts/tiles-live-soak-b94a188/soak/`. The run is based on b94a188
plus these runtime fixes; the earlier b94a188 ZIP remains unchanged. One Program
delivery deadline miss was recorded within the first four minutes, so this run
already fails strict every-frame performance acceptance. The soak continues to
characterize recurrence. Hidden-window display-unconsumed counts are retained;
this run does not prove physical display cadence, recording or A/V acceptance.

At 576 seconds, all ten scheduled scene switches had completed, with maximum
HTTP latency 112 ms. Counter deltas were 224 underruns, 212 overflows and 21
deadline misses, with zero GPU-not-ready events or output sequence gaps.
Bursts occurred while either scene remained selected, not only during Takes.
The buffer counts discarded expired packets as overflows, so paired underrun
and overflow growth does not establish that the queue was full. The scheduling
cause still needs diagnosis after the measurement completes.

The packaging fix preserves the SDK headers/import library required by the
managed readiness gate beside the isolated bin directory. Both packaging
scripts passed PowerShell parsing; a newly built ZIP still needs validation.

The soak terminated at **17:55:28 UTC** after `/state` timed out. The final
healthy sample was at 621 seconds, after eleven scene switches: 294 underruns,
278 overflows and 28 delivery deadline misses since the baseline. Restoration
of Program/Preview/view/auto-Take also timed out. Functional acceptance failed.
The monitoring heartbeat was paused after detecting the final report.

All three owned processes remained alive. The user confirmed the UI was
unresponsive despite Windows reporting `Responding=True`. Noninvasive native
stacks show the audio thread blocked writing stderr, the render thread holding
the core mutex while waiting for stderr's CRT lock, and the RPC thread waiting
for that core mutex. The stdout writer independently waits on its pipe. Managed
stacks show the UI waiting for the bridge snapshot lock and timer threads waiting
for the supervisor lock. These establish a hang and pipe backpressure cascade;
the upstream managed deadlock was subsequently confirmed with SOS lock owners.

The UI thread owned the supervisor gate while `Running` called
`Process.HasExited`. Its Windows process wait pumped an STA callback into scene
refresh, which queried `LastSnapshot` and waited for the bridge gate. A spine
sync thread owned that bridge gate and waited for the supervisor gate. This
cycle does not require the native process to exit. Periodic callbacks also
accumulated waiting for those locks. Captures are in
`managed-app-syncblocks.txt` and `managed-app-lock-owner-stacks.txt`.

After capture, the owned test app did not exit in response to CloseMainWindow.
Its verified process tree was terminated; the separate personal Zoom client was
not targeted. Scene restoration remains unconfirmed for this failed run.

Private failure evidence is in `artifacts/tiles-live-soak-b94a188/soak-failure/`.

The resulting fix uses cached process lifecycle state instead of OS process
waits under the supervisor gate. Polling, spine sync, and frame-drain timers
admit only one callback before entering application locks, with retired timer
generations ignored. The full managed MediaCore suite passed **521/521**.

Native diagnostics now use a fixed-capacity, best-effort queue; media producers
drop and count messages rather than waiting for a blocked sink. The writer owns
its lifetime independently so shutdown does not join a blocked pipe writer.
Release build and all **704 native tests** passed, including a deliberately
blocked sink regression. POSIX SIGPIPE protection is authored but was not
executed on this Windows host. Final queued diagnostics can be lost on immediate
process exit. Live validation of the combined replacement is still pending.

Detailed SDK debugger evidence is local under
`artifacts/tiles-live-soak-b94a188/join-failure/`. These private diagnostic files
are not release contents. The standalone Zoom client was not manipulated.
