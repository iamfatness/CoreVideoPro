# Experimental EngineHost (#960)

An on-demand, same-user/session host owns the existing native media supervisor.
WinUI attaches over a bounded local named pipe; native media and GPU/shared-memory
transport stay in the C++ core. The feature is **off by default**.

Build the host and WinUI in Release. For an isolated developer console, set
`COREVIDEO_ENGINE_HOST=1`, `COREVIDEO_ENGINE_HOST_PATH` to this project's built
`CoreVideoPro.EngineHost.exe`, and `COREVIDEO_ENGINE_HOST_PROFILE` to a unique QA
profile before launching the candidate. This does not isolate WinUI preferences;
use the explicit probe below for unattended verification. Do not use the installed
show for development tests. Host packaging/default enablement belongs to #963.

The first attached console is the writer. A replacement console is deliberately
read-only: it observes live snapshots/shared resources, and the host rejects its
saved startup state. Full editable model reconstruction and autonomous shell-owned
spine sync/automation belong to #961. Camera fallback still belongs to WinUI until
#962. The broader #959 production gate remains open. #960 qualification is currently blocked by intermittent audio-content failure #968.

There is no automatic replay after a lost acknowledgement. Epoch, fencing token,
expected command revision and a monotonically increasing command ID protect writes.
The bounded outcome cache can return a retained result; expired IDs return unknown.
Explicit takeover creates a new writer incarnation and fences the old one. Core
replacement also fences the writer. New/reconnected clients cannot silently restore
saved outputs. The host's current revision covers commands, not the complete #961
live-fact authority contract.

Client disposal detaches. Explicit hosted Stop shuts down the host/core with the
existing finalization grace. The existing normal-window-close output confirmation
still applies; the new console teardown avoids the legacy kill-tree fallback.
Polished continue-show/stop-show UX and installer retirement are not implemented here.
The host uses explicit stop, not an idle timeout. Production state survives console
loss only while its actual producer lives; host/core failure is not uninterrupted
output. Raw forwarded Zoom joins do not yet transfer all shell recovery intent.

Limits: four ordinary pipe connections (8 MiB messages), plus two reserved
connections (64 KiB messages) on the same user/session/profile endpoint's `.urgent`
suffix. Reserved admission accepts attach/detach/takeover/shutdown and explicit
Stop/Cancel/Leave commands. Mixed batches cannot use it. Incomplete reserved
requests expire after two seconds; ordinary requests after ten. Command admission
and response writes each have a two-second deadline. The host keeps 64 remembered
clients, 128 outcomes within 8 MiB, and 32 structural resource descriptors within
64 KiB each. Stable descriptors are published once per client; participant identity
is retained separately. CPU thumbnails are not forwarded.

One ordinary and one urgent command can await native replies concurrently; lease,
sequence and revision admission remain serialized. A delayed ordinary ACK cannot
roll back a later urgent revision. Pure urgent sync batches also bypass the bridge's
ordinary scheduler and supervisor sync guard; mixed production-state batches retain
ordinary ordering. This reserves host admission, not preemption of a blocked native
command or pipe write. Native mailbox/execution bounds still apply. Disposal avoids
UI-context capture so synchronous console detach cannot deadlock its dispatcher.
Shutdown is committed even if its client never reads the ACK. Identity-query races
with a dying client are treated as disconnects, not fatal host errors.

## Verification

```powershell
dotnet build native-shell/CoreVideoPro.EngineHost/CoreVideoPro.EngineHost.csproj -c Release
dotnet build native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj -c Release -p:Platform=x64
dotnet test native-shell/CoreVideoPro.MediaCore.Tests/CoreVideoPro.MediaCore.Tests.csproj -c Release
node scripts/qa/generate-av-pattern.mjs artifacts/engine-host-960/flash-beep.mp4
pwsh -File scripts/qa/engine-host-crash-drill.ps1 -NativeCore C:/path/to/corevideo-native.exe -Pattern artifacts/engine-host-960/flash-beep.mp4
```

For full Studio qualification, generate the continuous source and add `-FullStudio`:

```powershell
node scripts/qa/engine-host-continuity.mjs generate artifacts/engine-host-960/continuity.mp4
pwsh -File scripts/qa/engine-host-crash-drill.ps1 -NativeCore C:/path/to/corevideo-native.exe -Pattern artifacts/engine-host-960/continuity.mp4 -OutputDirectory artifacts/engine-host-960/new-trial -FullStudio
node scripts/qa/engine-host-continuity.mjs analyze C:/path/to/Program.mp4 C:/path/to/recording-analysis.json
node scripts/qa/engine-host-continuity.mjs analyze C:/path/to/receiver.flv C:/path/to/receiver-analysis.json
```

This mode stages a unique synthetic show before opening the actual StudioWorkspace,
StudioViewModel and Program/Preview GPU monitor offscreen. Both consoles reattach
read-only; it does not inject a foreign editable show behind Studio's model. It
requires active recording/RTMP, unchanged epoch/PID/session, advancing output, and
an active Program GPU monitor before/after UI death. The real startup/snapshot and
shutdown paths run, with in-memory preferences and no activation/device registration.
This is not MainWindow activation, real Zoom/capture, physical presentation or a soak.
The analyzer independently decodes frame IDs and continuous-tone PCM in both files;
interior quiet windows fail even when packet/sample timestamps are contiguous. It
reports startup and repeated tail frames separately and never converts this narrow
qualification into a full frame-performance claim. Use a fresh output directory.

Without `-FullStudio`, the drill runs the compiled WinUI executable's isolated `--verify-engine-host` mode
with the production bridge, generated media, local recording and a localhost RTMP
receiver. It kills only that WinUI PID, waits five seconds, reconnects another WinUI
process, then explicitly stops the unique QA host. It opens no operator window,
does not load production preferences or join Zoom, and does not prove Studio display
continuity. FFmpeg/ffprobe must be on PATH; a free local port is required.

Reports preserve before/after snapshots, files, logs and the fault timestamp.
`framePerformancePassed` remains false until independent receiver/content/audio and
full-workload evidence qualifies the actual requirement; timestamps or a surviving
PID alone cannot establish that. Keep startup drops separate and visible.

Tests exercise same-host concurrent launch, read-only reattach, explicit fencing,
revision/epoch rejection, bounded/expired replay results, incompatible protocol,
typed payload casing, observer churn and oversized framing. Additional fault tests
cover four hung ordinary clients, stalled readers, partial reserved frames, rejected
ordinary floods on the reserved endpoint, delayed native ACKs, lost ACKs after a
real disconnect, bounded bridge stop admission, stable resources and synchronous
UI-context detach. Windows process evidence
is explicitly skipped on other platforms, not reported as a successful Windows test.

See [design](../../docs/reference/engine-ui-crash-survival-plan.md) and
[#960](https://github.com/iamfatness/CoreVideoPro/issues/960). Work order remains
[BACKLOG](../../docs/BACKLOG.md).
