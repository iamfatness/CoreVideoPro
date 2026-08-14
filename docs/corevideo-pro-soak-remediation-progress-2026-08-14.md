# CoreVideo Pro soak remediation progress — 2026-08-14

## Current decision

**Core eight-source live Program/ISO and forced media-core recovery gates passed; not yet a production-launch GO.**

The automated regression suite, a 10-minute real Zoom recording with eight participant ISOs, and a live forced media-core recovery run pass. A fresh 60-minute Zoom soak and release installation lifecycle validation are still required on this build.

## Completed remediation

- Recording lifecycle is idempotent. Repeated Program/Preview/Take synchronization no longer reopens active Program or ISO writers.
- Program and ISO video/audio carry capture-time monotonic timestamps into their writers.
- Windows recordings use fragmented MP4 with one-second target fragments; an abrupt child-process termination test produces playable recovery output.
- Recording diagnostics now publish actual Program/ISO frame, audio, byte, duration, disk-rate, encoder-queue-drop, and render-deadline counters instead of estimates.
- ISO encoder placement reserves hardware capacity for Program, assigns up to seven ISO writers to hardware, and moves overflow ISO writers to software with an explicit reason in status and manifests.
- Optional capture-source warnings remain scoped to that source rather than poisoning global audio health.
- Launch, performance, and media-core logs use bounded UTF-8 append/rollover behavior.
- Windows UTF-8 conversion buffers were corrected in the Zoom engine, browser host, hardware capture, WASAPI capture, and monitor paths.
- The async encoder now suppresses held-frame re-submissions before queueing and uses weighted Program/ISO fairness. This fixes the real meeting failure where continuous Program traffic starved every ISO after its first frame.
- MP4 finalization failures are now surfaced explicitly, and byte telemetry no longer counts uncompressed input bytes as on-disk bytes.
- The managed supervisor remembers successful Zoom join intent and automatically rejoins after a native core crash.
- The native Zoom helper is owned by a Windows kill-on-close job, preventing an orphan helper from retaining SDK callback ownership after its parent core dies.
- Zoom recovery uses bounded retries with a 2.5-second SDK teardown cooldown. Live validation reproduced code 14 on the immediate attempt and succeeded with code 0 on attempt two.
- Successful joins no longer interpret the brief pre-privilege `rawMediaActive=false` snapshot as an explicit operator pause.
- The local control API now supports `zoom.join` and `zoom.leave`, and `/state` publishes per-source meter telemetry plus Program peak/loudness and audio validation state.

## Automated evidence

- Real Media Foundation native test suite: pass, including fragmented-MP4 abrupt-exit recovery and eight-ISO encoder-capacity coverage.
- Native suite after the live-failure repair: **560 passed, 0 failed**.
- MediaCore tests: **390 passed, 0 failed**.
- Control API tests: **31 passed, 0 failed**.
- WinUI tests: **690 passed, 0 failed**.
- WinUI build: **0 errors**.
- Zoom engine and browser host: build successfully.
- `git diff --check`: pass; only repository line-ending notices were emitted.
- Real Zoom repair gate: Program plus all eight ISOs finalized playable with video and AAC audio over approximately 88 seconds. See `docs/corevideo-pro-live-iso-validation-2026-08-14.md`.
- Forced recovery gate: the app relaunched the core/helper, retried Zoom SDK initialization, rejoined automatically, resumed live Program/source meters, and finalized Program plus eight playable H.264/AAC ISO files.

## Remaining launch gates

1. Run a 60-minute real Zoom meeting with eight participant ISOs plus Program (the 88-second gate passed).
2. Confirm every ISO meter responds only to its assigned participant for multiple talk/mute cycles. The short run proved distinct audio for the three speaking sources and digital silence for the five non-speakers.
3. Exercise scene edits, Preview, Take, participant churn, screen share, mute/unmute, and active-speaker changes while recording; confirm writer generations and file sizes remain monotonic.
4. Validate Program and every ISO with `ffprobe`: playable, non-zero audio/video, monotonic timestamps, expected duration, and no unexplained A/V drift.
5. Repeat the now-passing abrupt-exit recovery during the 60-minute soak and confirm no regression under sustained resource pressure.
6. Confirm diagnostics remain truthful under load: encoder queue drops, render deadline misses, disk rate, encoder path, and fallback reason.
7. Run the release packaging/install/upgrade/uninstall gates after the live-media gates pass.

## Acceptance rule

Ship only if the fresh soak has no silent or mux-cloned ISO audio, no recording writer restart during production synchronization, no unrecoverable recording after abrupt exit, and no unexplained sustained drop growth.
