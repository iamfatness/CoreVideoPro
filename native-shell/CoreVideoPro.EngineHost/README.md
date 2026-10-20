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
#962. These restrictions mean #960/#959 acceptance is not complete.

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

Limits: four admitted pipe connections, 8 MiB per message, 64 remembered clients
(old observers retired before the writer), 128 cached outcomes with an 8 MiB result
budget, and 32 structural resource descriptors of at most 64 KiB each. CPU thumbnail
frames are not forwarded. Client reads/writes and lane admission have deadlines;
native commands are serialized and can still delay later control during a long
operation. Reserved urgent-control scheduling and full hung-client qualification
remain open under #960. Local pipe ACLs restrict user identity, remote clients are
rejected, and clients verify host executable path and Windows session before sending.

## Verification

```powershell
dotnet build native-shell/CoreVideoPro.EngineHost/CoreVideoPro.EngineHost.csproj -c Release
dotnet build native-shell/CoreVideoPro.WinUI/CoreVideoPro.WinUI.csproj -c Release -p:Platform=x64
dotnet test native-shell/CoreVideoPro.MediaCore.Tests/CoreVideoPro.MediaCore.Tests.csproj -c Release
node scripts/qa/generate-av-pattern.mjs artifacts/engine-host-960/flash-beep.mp4
pwsh -File scripts/qa/engine-host-crash-drill.ps1 -NativeCore C:/path/to/corevideo-native.exe -Pattern artifacts/engine-host-960/flash-beep.mp4
```

The drill runs the compiled WinUI executable's isolated `--verify-engine-host` mode
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
typed payload casing, observer churn and oversized framing. Windows process evidence
is explicitly skipped on other platforms, not reported as a successful Windows test.

See [design](../../docs/reference/engine-ui-crash-survival-plan.md) and
[#960](https://github.com/iamfatness/CoreVideoPro/issues/960). Work order remains
[BACKLOG](../../docs/BACKLOG.md).
