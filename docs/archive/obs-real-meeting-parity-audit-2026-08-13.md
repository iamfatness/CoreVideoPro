# CoreVideo Pro real-meeting parity gate

Date: 2026-08-14

Source of learnings: CoreVideo OBS plugin releases v0.1.33 through v0.1.39 (2026-08-08 through 2026-08-13), inspected from the plugin repository's `origin/main` changelog and implementation.

## Product decision

CoreVideo Pro should ship as the reliable, focused Zoom production studio—not as a broad OBS clone. A premium feature is ready only when its source identity, Program routing, audio, recording, recovery, and operator-visible state remain correct in a real meeting. Controls that merely render correctly in a demo are not enough.

## Status legend

- **Proven**: implemented and covered by focused automated evidence.
- **Live proof**: implemented, but a real Zoom meeting must prove the full path.
- **Partial**: useful foundation exists; a production behavior is still missing.
- **Gap**: not yet implemented in the standalone product.

## Parity matrix

| Real-meeting lesson | CoreVideo Pro status | Evidence / remaining work | Launch priority |
|---|---|---|---|
| Zoom color request is BT.709 full-range | **Proven** | Windows and macOS engine joins request `BT709_F`. The compositor supports BT.709/BT.601 and full/limited I420. | P0 |
| Delivered luma range matches the declared conversion | **Live proof** | Added a once-per-source first-frame luma probe (`min`, `max`, `<16`, `>235`). Today Zoom IPC carries no color metadata and decoded Zoom frames still default to full-range BT.709. Capture controlled black/white evidence in a live meeting, then carry an explicit range/matrix decision through IPC; do not silently infer range from ordinary picture content. | P0 |
| ISO sources meter independently and reach their own stems | **Live proof** | Dedicated meeting-mix and participant-audio subscriptions plus ISO-audio submission are implemented in the current worktree. Must prove per-participant meters move, Program contains the intended mix, ISO stems differ from Program, and camera-off participants still produce audio. | P0 |
| Meeting mix is a first-class Program source | **Live proof** | The payload now creates a `meeting-audio` / `program` subscription independent of active-speaker video. Validate reconnect and engine restart in a live meeting. | P0 |
| Active-speaker changes do not flap Program | **Live proof** | Uses 500 ms challenger stability, 2 s incumbent hold, candidate video/mute eligibility, and 60 s roster-loss grace. A challenger cannot replace an incumbent until its decoded frame is fresh; deterministic tests prove the director holds after both timing windows and switches only after frame ingest. The frequent shell planner follows the directed speaker ID rather than raw `talking` churn. Still add operator-configurable exclusions and prove the visual handoff in a live meeting. | P0 |
| Scene cuts have warm Zoom frames | **Live proof** | The finite subscription budget now prioritizes directed speaker, Program routes, queued Preview routes, then multiview-only roster feeds. Participant video UUIDs remain stable across Preview-to-Program purpose changes, so a Take retains the warmed SHM frame instead of tearing down the subscription. Tests cover a queued guest beyond the eight-feed roster cap and no-resubscribe promotion. Prove the cut visually in a live meeting. | P0 |
| Resolution raises do not invalidate shared memory | **Proven** | Standalone uses fixed-capacity video/share SHM regions rather than resizing mappings, with per-engine instance-scoped names. This is structurally stronger than the plugin's post-fix resize generation scheme. | P0 |
| Engine restart cannot reuse stale SHM or lose subscriptions | **Proven** | Per-process IPC tokens prevent stale mapping collisions; replacing the process clears sent-subscription state and has a focused resubscription test. Extend the live test to audio meters and Program mix. | P0 |
| Recording stop cannot stall the render/control thread | **Proven** | `AsyncEncoderSink` queues stop/finalize off-thread and has a bounded teardown grace. | P0 |
| Disk/encoder pressure cannot grow an unbounded queue | **Proven** | Program and ISO video/audio use bounded drop-to-latest queues with separate Program/ISO accounting. Surface drop counters prominently during recording. | P0 |
| A crash or power loss leaves playable recordings | **Proven** | Windows Program and ISO writers now use Media Foundation's fragmented MP4 sink. The manifest records `containerMode: fragmented-mp4`, crash-safe intent, and the target fragment duration. A regression child writes real H.264/AAC for four seconds and is forcibly terminated with no stop, destructor, or finalizer; the surviving artifact contains `moov` plus multiple `moof` fragments and opens through `IMFSourceReader`. All nine real Media Foundation recording tests pass. | P0 |
| Temporary participant loss does not destroy an ISO immediately | **Gap** | Add an unresolved grace window (plugin learning: 60 seconds), visible state, and deterministic segment rollover when identity returns or changes. | P1 |
| ISO encoder capacity automatically spills GPU overflow to CPU | **Partial** | A tested auto/hardware/software placement policy exists, including reserved hardware capacity and visible failure reasons. It is not yet wired into Media Foundation writer creation or the operator UI. | P1 |
| Hardware acceleration failure is visible and recoverable | **Partial** | Standalone owns D3D11/Metal compositing rather than the plugin's FFmpeg graph, so the exact regression does not transfer. Add startup capability evidence, hardware-session counts, software fallback status, and an operator warning when quality/FPS must step down. | P1 |
| Invalid/rotated OAuth refresh tokens recover truthfully | **Proven** | Refresh `invalid_grant` now clears stale credentials and requires a clean sign-in instead of retrying forever while appearing signed in. | P1 |
| CoreVideo Tiles is a first-class scene type | **Gap** | Port the plugin's dynamic gallery model and animated reflow. Controls required in the main app: border enable/color, shape/radius, thickness, glow enable/color/strength, padding/gap, active-speaker emphasis, labels, and animation timing. See `corevideo-tiles-iso-scaling-plan.md`. | P1 |
| HQ production MOV backdrops play without proxy conversion | **Live proof** | Media Foundation remains the fast path; unsupported production codecs fall back to an off-render-thread FFmpeg decoder. The native path is verified against the actual 4K Apple ProRes HQ 10-bit 4:2:2 BT.709 backdrop `OH2_EarthBG_03-HQ.mov`, including production-speed decoding beyond its original 270-frame loop boundary and valid video-only audio diagnostics. Confirm visual playback in the rebuilt WinUI app; SuperSource/Tiles must reuse the same loop contract. | P1 |
| Optional Tiles effects fail gracefully | **Gap** | A missing/failed effect must degrade to a clean tile, never make the source disappear or take Program down. Add effect-load failure tests. | P1 |
| Logs remain useful during failure storms | **Partial** | Standalone has health/support-bundle foundations and several bounded warnings. Apply rate limiting to repeated subscription, stale-frame, encoder, and effect failures; keep state transitions unthrottled. | P1 |
| UI reflects engine truth after refresh/restart | **Partial** | Preserve route/settings intent separately from observed media health. Validate stale IDs, recording-finalizing state, reconnect, audio source readiness, and encoder fallback across UI refresh. | P1 |
| Native regression process is heap-clean | **Proven** | The UTF-8-to-wide media-path allocation no longer writes its terminator past the vector. A dedicated MSVC AddressSanitizer build passes all 73 `AudioDsp.*` and 62 `MediaCoreCommand.*` tests, and the rebuilt normal executable passes the full 548-test suite without `0xC0000374`. The test target now rebuilds its real plugin-host dependency so broad runs cannot launch a stale transport executable. | P0 |

## Required launch rehearsal

Run one recorded 45–60 minute Zoom rehearsal using at least four participant devices, including camera-off audio, screen share, speaker handoffs, reconnects, and an engine restart. The build is launchable only when all of these are captured in the support bundle and recording validation output:

1. Every participant ISO meter moves independently; muting one participant affects only that participant's ISO and the meeting mix as Zoom delivers it.
2. Program contains the meeting mix exactly once—no silence, duplication, or dependence on the active-speaker video subscription.
3. Each ISO MP4 has the intended participant audio, non-zero video duration, monotonic timestamps, and A/V duration within tolerance.
4. Color probe evidence is recorded per Zoom source and a controlled black/white chart matches the selected BT.709 range conversion in Preview, Program, and recording.
5. Speaker handoffs never show a stale/new identity mismatch and do not flap during crosstalk.
6. Preview-to-Program cuts begin with a fresh frame; no black/old frame is exposed while a subscription warms.
7. Engine restart restores video, meeting audio, ISO audio meters, routes, and recording state without stale SHM.
8. Simulated slow disk produces bounded drop counters and operator warnings without freezing Program.
9. Forced process termination yields recoverable recording artifacts; compare the recovered duration against the last support-bundle heartbeat.
10. GPU session exhaustion visibly assigns excess ISOs to CPU (or refuses them explicitly) once placement is integrated.

## Execution order

1. Finish live proof for Zoom color and ISO/Program audio using the new evidence paths.
2. Integrate ISO GPU/CPU placement and expose capacity/status in the UI.
3. Port CoreVideo Tiles as a shared scene/profile schema with graceful effect fallback.
4. Run the full rehearsal above and retain its support bundle as release evidence.
