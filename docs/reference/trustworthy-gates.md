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
