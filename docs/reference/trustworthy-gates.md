# Three gates that could not be trusted, and what each was hiding (2026-10-01)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

A check that is red for a reason nobody acts on teaches everyone to stop reading it. Each of
these was failing or flaking on `main`, and each was hiding something.

- **Show drill, `source->render` latency: failing on every build since 2026-09-21.** The #579
  lip-sync fix holds Zoom video for `kZoomVideoReserve` (60 ms = 3.6 frames at 60 fps,
  `ZoomPlayoutTiming.h`) so it plays out with its primed audio. The drill kept a 33/50 ms
  budget from before the hold. The budget is now DERIVED from that header (hold + 20 ms p50,
  hold + 33.3 ms p99), so it gates latency above the deliberate hold and cannot go stale the
  same way. Measured 68.6 / 73.9 ms against 80.0 / 93.3. **The 60 ms itself is an owner
  decision (kept 2026-10-01); cutting it means shrinking the Zoom audio reserve.**
- **`validate-gpu-encode.mjs`: failed 2 runs in 5 with a healthy stream.** It grepped the
  lossy process log for `[gpu-encode] path=`. It now reads the path from the sender snapshot
  (`muxInputVideo` is published only on the GPU-direct path); a log line that says
  `cpu-fallback` still overrules, only its absence no longer fails. 5 of 5 after.
  **Its own control, `--force-raw`, was the real finding (#735, fixed in the same PR): the
  CPU-fallback stream path delivered NOTHING on every build from 2026-09-30.** #538 Slice 6
  (`afc32ebc`, shared AAC) made `CompositeOutputSender` withhold PCM from every sender that
  ACCEPTS shared AAC. Accepting is a capability; a sender on the CPU fallback path (no usable
  hardware encoder, a capacity refusal, `COREVIDEO_GPU_ENCODE=0`) ignores the encoded packets
  and has an FFmpeg waiting on a PCM input that never arrived, so it stopped reading video
  after 12 frames. PCM now goes to every sender; one on the shared path already drops it
  (`writeAudioToFfmpeg` returns first). After: raw path 3 of 3 at 60 fps, GPU path 3 of 3,
  and the same-run A/V harness still within one frame.
  - **The sender's `lastError` was a red herring:** "waiting for composed BGRA program
    pixels" is the sticky first-tick message, not the cause. Read `framesSent` and
    `supervisor.lastProgressAgeMs`.
  - **The first bisect of this was WRONG.** It trusted one 6 s run per step and never checked
    the build had produced a new binary; its first verdict tested a stale exe and it blamed
    an unrelated commit. The second deleted the exe before each build and required two
    agreeing runs. Confirm a bisect boundary with repeated runs on both sides before
    believing it.
  - Run `--force-raw` whenever the sender, the composite or the Program export changes.
- **Windows CI meter probe: failed three runs in a day and passed each re-run.**
  `test-audio-meter-stability.ps1` threw the probe's exit code BEFORE reading its report, so a
  probe that failed with a reason looked the same as one that crashed. It now prints the
  report and says which. The cause of the flake is still unknown; the next one names itself.
- **Rule:** when a gate is red on `main`, A/B it against an older build before reading it as
  noise or as your regression. Copy an installed beta's `corevideo-native.exe` over
  `native/build-dev` for one run (and restore it), or point `COREVIDEO_BUILD_DIR` at a scratch
  folder holding it and the fake engine.

## GPU fixture mode selection (#869)

The test runner prints incoming monitor/preparation overrides. Mode-sensitive
fixtures additionally print and verify the compositor's actual `effectiveMode`;
an unset override does not imply isolation for a direct factory call. A scoped
guard restores prior environment values after worker teardown, even on failure.

`SourceTextureCacheDedupsUploadsAcrossPasses` explicitly selects inline monitors
and disables CPU preparation: its counters describe the production texture cache.
`ParticipantBgraExportCopiesCachedPixelsAcrossDevices` exercises both modes. The
isolated branch requests its participant export from the monitor worker and waits
up to three seconds for the matching job and exact cross-device pixels. The
subscription-notice fixture also exercises both modes and waits up to two seconds
for the attributed layout event, checking both notice addition and removal.
`MultiviewProgramPixelsAdvanceAtFixedConsumerPhases` explicitly selects each
branch's mode before constructing the compositor; its bounded wait and zero
repeat/gap requirements remain unchanged.

### Phase timing interpretation and retained failure (#898)

The October 18 #947 follow-up uses the newer absolute frame-boundary assertion,
not the historical relative ten-millisecond wait below. The fresh production
suite retained 1,774 passes and one failure: depth 3, isolated phase 14 ms,
tick 52 observed request sequence 52 instead of 53 at the anchored boundary.
The early Program-copy observation passed; this is a later monitor-result
publication failure, not proof of missing Program-copy pixels. The two
deadline-bearing monitor owners now use the same scoped Playback scheduling as
other video workers; optional grade previews retain ordinary scheduling.
Keep the failed log and validate the unchanged phase/pixel/deadline assertions.
A later scoped pass does not establish full-workload or display acceptance.

The explicit-mode full Release run at `706a446` with incoming isolation `0`
retained 1,599 passes and one failure: the isolated phase fixture timed out
waiting ten milliseconds for the next worker sequence. This is a different
failure from the previous missing-mode warm-up assertion. It is not classified
as fixture noise or a production regression without a quiet matched comparison.

The fixture aims to start consumption at tick + 16 ms but does not enforce that
deadline: a ten-millisecond completion wait begun at phase 14 can extend to
24 ms after the tick, and keyed-mutex acquisition/readback can extend it further. Therefore
exact pixels and zero repeats/gaps establish the sampled sequence, not on-time
presentation. Per-tick timing records now expose tick wake, render return,
monitor submission/completion observation, consumer start and readback completion
relative to the scheduled tick. Timeout records include expected/observed
sequence and monitor counters/readiness. These diagnostics do not change the
ten-millisecond wait or pixel thresholds. CPU observation timestamps alone do
not establish GPU-ready time or OS presentation; #898 tracks that distinction.

On a quiet Windows hardware GPU, build Release and run the complete suite once
with `COREVIDEO_ISOLATE_MONITORS=0` and once with `COREVIDEO_ISOLATE_MONITORS=1`.
Preserve both logs, including `[gpu-test-run]` and `[gpu-test-mode]` records.
Focused fixture success or a stub suite is not a substitute for these full GPU
runs, nor proof of installed OS-camera presentation or unlimited zero misses.
