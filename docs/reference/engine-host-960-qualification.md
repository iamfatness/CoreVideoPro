# EngineHost #960 qualification

PR: [#965](https://github.com/iamfatness/CoreVideoPro/pull/965). Audio fix: `cdd010ed`; final implementation and diagnostic/harness changes tested: `e6226296` (includes current-main integration).

**October 21 decision: scoped #960 crash-survival content checks pass; #968 is corrected.** Four consecutive corrected-build full Studio trials have no decoded interior audio gaps or frame-ID anomalies in recording or received RTMP. Host/native/session identity survives UI termination and read-only reconnect. Merge checks remain visible on the PR. This does not qualify zero-loss startup/shutdown, physical presentation, editable reconnect or a full production workload; #959/#961–#963/#930 remain open. No installed app or production settings were changed.

## October 21: #968 diagnosis and repair

The audio worker already owns a fixed 20 ms steady-clock deadline grid and runs catch-up ticks after late wakes. Source ingress incorrectly asked MediaTransports for the slot due at the actual wake time instead. Its demand clock anchored at the first wake; subsequent wakes on either side of that phase boundary could return no block, then skip a source window as expired. Both output encoders received the resulting missing PCM, so continuous packet timestamps did not imply continuous content.

The retained studio-staged native log reports udio_windows_expired reaching 100, 200 and 300. The studio-gpu failure also reports expiry; the clean ridge-control and studio-69df85fc controls do not. This accounts for the intermittent start-of-session behavior without attributing it to UI death.

Source ingress now selects media PCM using the worker's scheduled timestamp. Actual observation time remains unchanged for source health and other capture adapters. Unscheduled callers retain their wall-time fallback; genuine deadline reanchors still advance the demand clock. No audio padding, decoder change, quality reduction or analyzer threshold change is involved.

SourceAudioIngress.ScheduledMediaDemandPreservesEveryBlockThroughWakeJitterAndCatchup checks exact sample payloads through a late first wake, phase-crossing jitter and two catch-up ticks at the same wall time. The previous wall-time selection fails the first expected block in that sequence.

The first October 21 trial (`studio-968-a`) is rejected as post-fix qualification: the initial native compile overlapped editing, and the harness still used the older x64 Studio output. It retained 76 recording / 87 receiver quiet intervals. The affected native translation units and the explicit `Platform=x64` Studio target were rebuilt before trials b/c/d. The final committed-build trial records all three executable hashes and rejects binaries that change during a run.

### Repeated full Studio evidence

All trials below preserved host epoch, native PID and recording session; recording/RTMP continued advancing without a stream retry, and the replacement read-only Studio Program GPU surface became active. Source preflight (`source-analysis-968.json`) independently passed frame-ID and continuous-tone checks. No analyzer thresholds changed.

| Trial | Native PID unchanged | Recording / RTMP decoded frames | Interior audio gaps, both files | Interior frame anomalies, both files | Recording final held frames / writer counter | Startup video drops |
|---|---:|---:|---:|---:|---:|---:|


| `studio-968-b` | 56592 | 1390 / 1575 | 0 / 0 | 0 / 0 | 1 / 1 | 22 |
| `studio-968-c` | 45844 | 1388 / 1575 | 0 / 0 | 0 / 0 | 1 / 1 | 23 |
| `studio-968-d` | 41848 | 1385 / 1568 | 0 / 0 | 0 / 0 | 1 / 1 | 23 |
| `studio-e6226296` | 43724 | 1387 / 1571 | 0 / 0 | 0 / 0 | 1 / 1 | 22 |

The strict recording analyzer still exits 1 for the single final duplicate; the receiver analyzer exits 0. This duplicate is the existing `MediaFoundationEncoderAdapter::reconcileTail` stop-time policy, which holds the final real video buffer to the audio endpoint without cutting speech. In every trial, `recordingVideoTailFrameCount == 1` accounts exactly for the decoded duplicate. It occurs at explicit finalization, after the crash/reconnect interval. It is not silently trimmed into a strict whole-file pass. The source-window expiry/missing-window logs are absent in all four corrected trials. `framePerformancePassed` remains false, and startup drops remain reported separately.

Final trial executable SHA256:

- `corevideo-native.exe`: `68722c550db41ba77439452a023e39431f938a7405cc9a1b955a2d58887c8791`
- `CoreVideoPro.EngineHost.exe`: `b1a55313cf2e6bc9395a43d1e83c58d5d883f84b798c90014c37e2c4e3708366`
- `CoreVideoPro.WinUI.exe`: `80762176b022765a0cd7c6f02722ce9b3de4dbbd15e275c3dc68b92323c6376c`

Final output SHA256:

- recording-analysis.json: `19e6749f1665c17d5023fcdfe10e1c9c35185f0d669eec61c41504c53acb25bc`
- receiver-analysis.json: `6494f517aecb53c48c3863d7279191717a5301f055a440fd562b94d98eadc2a1`

Validation: Release native core/tests and x64 Studio build succeeded (Studio: 491 existing warnings, 0 errors). Merged-branch MediaCore/host tests: **2,586 passed**; relevant Studio startup/transport/shutdown tests: **88 passed**. The focused native ingress run executed all **10 tests**, including exact PCM under wake jitter/catch-up. The initial full Windows native run had **1,776 pass / 1 fail**: `D3DProgramBuffer.MultiviewProgramPixelsAdvanceAtFixedConsumerPhases` published at 16.782 ms against 16.667 ms (depth 3, phase 14 ms, tick 112). This retained display-deadline failure is separate from #968 and remains evidence for #930; no threshold was relaxed. Final native rerun and hosted CI outcomes are recorded on PR #965 / issue #968 so the mutable merge gate is not inferred from this historical snapshot.

Evidence is retained locally under `artifacts/engine-host-960/`; generated media files are not uploaded CI artifacts. The same scoped workload and exclusions described below apply. The following October 19 results are history, retained unchanged.

## October 19 implementation and regression evidence

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
