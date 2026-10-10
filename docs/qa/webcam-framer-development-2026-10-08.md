# Webcam framer development evidence — 2026-10-08

Issue: [#839](https://github.com/iamfatness/CoreVideoPro/issues/839).
Base: `b2dd125`; implementation branch: `codex/webcam-framer-839`.
Hardware: NVIDIA RTX 4090, driver `32.0.16.1742`, Windows, MSVC Release.
Core SHA256: `f6e8bb07d80c94bd6990a7f74512181626751d49c38ec80568e9b178b55faea8`.
Built-in PNG SHA256: `db55fb29960c09493fa5e17e7e560641b7fee3e4d8bcd25a5c624f6280f7964d`,
identical to the owner's original file.
Native gates: STUB=OFF, DEV_ADAPTERS/D3D11/VIRTUALCAM/MF_ENCODER=ON;
Zoom, WGC and network output adapters were not enabled for this development build.

## Functional checks

- Native regression suite: **1,537 passed, zero failed**.
- Disabled-GPU/camera contracts: **10 passed, zero failed**.
- MediaCore managed regression suite: **2,350 passed, zero failed**.
- WinUI managed regression suite and Release XAML build: **1,606 passed, zero failed**.
- Real core JSON control probe, camera off: requested framer intent round-trips as
  `waiting`; disabling it returns `off`, with zero publication/overlay frames.
- GPU pixels: clear center and chroma retain their original bytes; lower mask
  alpha 127 blends Y=100 to 58; the opaque gray guide produces Y=74. Original
  shared NV12 stays immutable. Mirrored video retains readable, correctly placed
  labels; the asymmetric-input mirror test compares the entire NV12 result.
- Publisher integration: separate decorated buffer, preserved sequence/timestamp,
  original pointer when disabled, fixed 1080p format through the BGRA fallback.
- HTML review: a separate webcam preview shows the original PNG mask over the
  graded demonstration, with the grading image/scopes clean; browser error log empty.

Warm GPU upload/blend/readback in the final full suite measured **1.214 ms mean,
4.255 ms worst over 120 frames**. An earlier focused run measured 0.612/1.148 ms.
These are stage costs, not a claim of receiver cadence or a fleet guarantee.

## Cadence evidence — qualification remains open

`corevideo-webcam-framer-qa` uses the actual publication worker and a test sink,
with a synthetic immutable 1920x1080 NV12 image at 60/1. It does not exercise
camera shared-memory writes, the OS receiver, Program/recording load or a meeting.
The two-second preparation window is separate; startup replacements were zero
in all final/diagnostic runs below. No failed run was discarded.

| Run | Seconds | Published / submitted | Identity gaps | Late past next tick | Bad sampled pixels | Verdict |
|---|---:|---:|---:|---:|---:|---|
| framer-final-off | 60 | 3597 / 3600 | 3 | 5 | 0 | FAIL |
| framer-final-on | 60 | 3594 / 3600 | 6 | 9 | 0 | FAIL |
| framer-diagnostic-off | 30 | 1797 / 1800 | 3 | 4 | 0 | FAIL |
| framer-diagnostic-on | 30 | 1796 / 1800 | 4 | 5 | 0 | FAIL |

Earlier exploratory 60-second off/on runs both delivered all 3,600 frames with
zero gaps/replacements/deadline failures. The later final runs above failed and
remain failures; the earlier passes do not waive them.

The added producer timestamps explain the diagnostic baseline: all four late
received frames were already 16.858–24.502 ms late at submission, and reached the
sink another 12–14 microseconds later. The test producer itself missed seven
next-tick deadlines. Maximum queue-to-sink time over received baseline frames was
1.469 ms; no received frame took a full tick after actual submission.

With the framer enabled, four of five late received frames were already late at
submission. The fifth was submitted 16.160 ms late, leaving 0.507 ms of the slot;
its queue/GPU/sink work took 3.247 ms. The producer missed eight deadlines;
maximum queue-to-sink time was 9.359 ms, with zero received frames requiring a
full tick after actual submission. This identifies synthetic producer lateness
in these traces; it does not establish the cause of a real CoreVideo/Zoom incident
or turn either overall result into PASS. The bounded latest-frame slot replaced
frames during the producer's catch-up bursts.

Installed candidate UI/receiver pixels, real source/Program/recording pressure,
and a real-meeting bake are **unverified**. Leave #839 and the broader #517 camera
qualification open; this evidence does not authorize a merge, beta or installation.
The installed beta and its binaries were unchanged.

Raw local evidence is retained under the task worktree's `artifacts/`: final and
diagnostic JSON/CSV, producer CSV, test/build logs, control probe and HTML screenshot.
The initial mixed STUB=ON/D3D11=ON suite also remains retained (21 profile/compositor
assertions failed because the stub disables compositor construction). Correcting
that build configuration produced the full native pass reported above.
