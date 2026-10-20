# EngineHost #960 qualification — October 19, 2026

PR: [#965](https://github.com/iamfatness/CoreVideoPro/pull/965). Implementation tested: `69df85fc86489d0f8e6d496e100246dc5f74e738`.

**Decision: keep draft.** Process/session survival and reserved control admission pass the scoped checks. Full continuity qualification remains blocked by [#968](https://github.com/iamfatness/CoreVideoPro/issues/968). A later passing audio trial does not waive the earlier failures. #960 remains open; no installed app or production settings were changed.

## Implementation and regression evidence

- Reserved control endpoint: two admission slots, 64 KiB frames, two-second incomplete-request deadline. Ordinary endpoint: four slots, 8 MiB frames, ten-second incomplete-request deadline. Admission and reply writes are bounded separately.
- Stop/Cancel/Leave-only batches and explicit host administration use reserved admission. Pure urgent syncs also bypass ordinary bridge/supervisor sync waits. Mixed state batches retain ordinary ordering. Native execution and a blocked native stdin remain bounded by their existing mechanisms; host priority does not preempt native execution.
- State admission stays serialized. Concurrent replies cannot roll the client revision backward; stale writer fences, unknown outcomes and bounded result retention remain enforced.
- Added fault coverage for delayed native replies, four hung clients, blocked readers, partial reserved frames, ordinary floods rejected from the reserved endpoint, an actual disconnected lost ACK, synchronous UI detach, bounded bridge stop admission, and resource descriptor identity/deduplication.
- Fixed a UI-context deadlock discovered in the first full Studio graceful-detach trial. Fixed repeated descriptor delivery and per-participant resource cache keys.
- Release builds: EngineHost 0 warnings/0 errors; WinUI 491 existing warnings/0 errors.
- MediaCore: **2,586 passed**, including **22 focused host cases** and the independent stop-scheduler regression. Relevant WinUI startup/transport/shutdown: **80 passed**. Local checks only; no CI checks were reported on the draft at evidence capture. `git diff --check` passed. Native binaries were not rebuilt or unit-tested in this C# change.

## Exact committed-build drill

`artifacts/engine-host-960/studio-69df85fc/` retains configuration, before/after snapshots, fault timestamp, logs, finalized recording, received RTMP and decoded analysis. This folder is ignored locally, not an uploaded CI artifact.

The driver stages a unique synthetic host show, then opens the actual compiled StudioWorkspace, StudioViewModel and Program/Preview GPU monitor offscreen. It exercises the normal startup, snapshot bindings and teardown with isolated preferences. A first read-only console is forcibly terminated, the show runs without that client for five seconds, and a replacement console attaches read-only. Only the unique QA host is explicitly finalized/shut down afterward. The driver fails if production is not active before injection, if epoch/PID/session changes, if output stops advancing, or if the Program GPU monitor fails to reopen.

| Measurement | Before / after or result |
|---|---|
| Native PID | 47952 / 47952 |
| Host epoch | `e4051f103a644c8ab3c5fd916b63250d` unchanged |
| Recording session | `qa-6f38dcf2e4824225804b05888eccfa51` unchanged |
| Recording frame counter | 654 / 1,351 |
| Program GPU monitor | Active before and after |
| Fault time | 2026-10-19 22:31:43.1558431 -04:00 |
| Recording decoded video | 1,422 frames; no interior repeated/skipped frame IDs; **one repeated final frame** |
| Received RTMP decoded video | 1,600 frames; no repeated/skipped frame IDs |
| Decoded audio | Contiguous sample timestamps and no interior quiet windows in either file on this trial |
| Startup dropped video | **23**, retained separately; not a zero-loss whole-session pass |

The strict recording analyzer returns failure for the repeated final frame. It is not silently trimmed into a pass. The decoded files support continuous content through the crash/reconnect interval on this trial, not unrestricted production readiness.

SHA256:

- Recording: `c03b82804444d1e177072923fc620c429eb4a313f885a4da10a2bd61f25c6033`
- Receiver: `b534e52a859b412084d60da87ff447d3e6e2d88c19006bca59c360f9e9d58db1`
- Reused Release native executable: `8ecd8093d752da222f5733ecaaaa88bb13ee5073dcad8a618728732263fecd65`, at `D:/CoreVideo-QA/beta-release-952/native/build-dev/corevideo-native.exe`.

Rig: Intel i7-14700K, NVIDIA RTX 4090 driver 32.0.16.1742, Windows 11 Home build 26200. Parsec and DisplayLink virtual/display adapters were also present. Workload: one generated 1080p60 moving media source with per-frame barcode and continuous 1 kHz / 48 kHz tone, Program recording, localhost RTMP receiver, full Studio bindings and Program monitor. Duration approximately 27 seconds. No real Zoom, camera, ISO fleet, physical display measurement, MainWindow activation or production soak was performed. Editable reconnect remains #961; capture fallback remains #962; packaging/full-workload release qualification remains #963/#930. `framePerformancePassed` stays false.

## Retained failures and controls

- `studio-first`: UI crash/reconnect reached teardown, then replacement disposal deadlocked on a captured UI context. Fixed and covered by `SynchronousUiDetachDoesNotRequireTheBlockedDispatcher`.
- `studio-continuity`: invalid harness injected an independent editable show behind Studio's model. Normal model sync replaced it; recording never produced a usable file. Rejected, not counted as survival evidence. Full Studio mode now stages the show before a read-only attach and asserts output progress.
- `studio-staged`: recording showed a startup duplicate/skip pair and recurring near-silent PCM intervals; sample timestamps remained contiguous. Central recording range had 263 near-zero 10 ms windows. This prompted #968.
- `studio-monitors`: recording content passed; offscreen Window bindings had not initialized, so this was **not** GPU-monitor evidence. The drill now initializes those bindings without activating the desktop and asserts Program GPU activity.
- `studio-gpu`: GPU monitor survived; video IDs were continuous in 1,411 recording and 1,591 received frames. **74 recording and 76 receiver interior quiet intervals** failed audio continuity. Some began before the UI crash; causation remains unresolved.
- `bridge-control`: no full Studio; recording frame IDs and interior audio passed. This control and later successes show intermittency, not an explanation or a waiver.

Reproduction commands and admission semantics are in [EngineHost README](../../native-shell/CoreVideoPro.EngineHost/README.md). Owner ranking/status remain in [BACKLOG](../BACKLOG.md) and issues #960/#968.
