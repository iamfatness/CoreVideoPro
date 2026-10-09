# Webcam framer Program loudness readout

Owner request: #839, October 9, 2026. Extends the existing webcam-only framer
with PGM LUFS-S, a one-decimal readout and a -60..0 bar. It reads the existing
post-master Program meter; it does not meter a receiver's audio. The three-second
window must be filled. Warmup/nonfinite/stale (>500 ms) values show `--`, and
measured silence shows `-INF`. Composition stays in the existing GPU pass.

Release developer build on NVIDIA RTX 4090:

- GPU overlay tests: 5 passed, 0 failed. Changing values change the panel,
  pixels outside it retain the original framing result, clean input is immutable,
  and backend mirroring preserves the full panel/guide result.
- Freshness/window test: 1 passed, 0 failed, including the 500/501 ms boundary,
  missing measurement, incomplete window, nonfinite measurement and silence.
- Asynchronous camera publisher tests: 5 passed, 0 failed.
- Existing loudness tests: 9 passed, 0 failed, including BS.1770 Program tap.
- Disabled-GPU focused suite: 12 passed, 0 failed.
- Combined framer + changing LUFS values, alternating mirror, 120 warm 1080p
  frames: mean 0.661 ms, worst 1.094 ms upload/blend/readback. This is processing
  cost, not receiver cadence acceptance.

Logs remain in `artifacts/lufs-*.log`. An initial colon-separated custom test
filter selected zero tests; it is retained and is not counted as validation.
Individual filters above ran the intended tests. Native Release build and
`git diff --check` passed. The HTML review shows a explicitly labeled fixed
-18.4 example, not live audio from the running app.

The installed combined grading/framer review candidate is still
`beta-2026-10-09-5852cd0` and does not contain this LUFS change. Its separate
installed smoke received 300 OS-camera samples in five seconds with the
original framer off and 300 with it on, at negotiated 1080p60, and its installed
native/XAML grading probe passed. Earlier Store-Python-launched smoke failures
are retained: that runtime exposed the October 5 per-user registration while
desktop registry tools showed October 9. The normal desktop Python runtime
activated the installed camera successfully. Neither registration metadata nor
that short smoke proves the camera DLL loaded by Frame Server or lossless cadence.

The previous worker-reference deadline failures remain open and retained in
`webcam-framer-development-2026-10-08.md`. This change is not yet installed or
qualified against real audio, an OS receiver or mixed production load. #839
remains open; no average-cost result waives missed output deadlines.
