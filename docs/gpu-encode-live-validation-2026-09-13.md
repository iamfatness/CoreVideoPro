# GPU-direct streaming: live investigation, 2026-09-13

Base: `85ee3d37`, PR #523. Windows, RTX 4090, real Zoom meeting with eight camera-on
sources in CoreVideo Tiles, 1920x1080 at 60 fps, 6 Mbps, YouTube RTMP, three-frame
Program buffer. Evidence is under `artifacts/live-gpu-encode-2026-09-13/`.

## Findings and changes

The initial live run submitted approximately 59 frames/sec but FFmpeg wrote only
18.77 encoded frames/sec. Its `speed=1.01x` measured the arrival-stamped timeline,
not frame delivery. Neither that value nor sender acceptance counters prove 60 fps.

Temporary per-stage instrumentation located the dominant stall in the synchronous
bitstream sink: about 4.7 seconds of a five-second encoder-thread window were spent
writing to FFmpeg; texture waits had no timeouts in those samples. Increasing the
pipe and disabling RTMP TCP delay improved throughput, but removing instrumentation
exposed timing sensitivity and output near 35 fps. These experiments alone were not
a reliable fix.

The Windows sender now copies compressed chunks into a bounded queue and writes
them on a separate worker. The queue allows at most 60 chunks / 2 MiB, plus one
in-flight chunk; overflow and write failures mark the sender unhealthy. Shutdown
cancels synchronous writes, joins the worker, then closes stdin. GPU-direct uses a
1 MiB pipe and RTMP/RTMPS uses `-tcp_nodelay 1`; raw fallback retains its pipe size.

The localhost gate also exposed lost asynchronous MFT input credits: a missing
texture consumed a NeedInput event without supplying a sample, eventually starving
the encoder. Each credit is now retried until supplied or stopped; invalid-handle
retries have a bounded wait. Output handling consumes one sample per HaveOutput
event and preserves ownership of caller-allocated samples.

## Evidence

- Real YouTube run with the separate writer, without diagnostic timers: more than
  two minutes at about 60 encoded fps and 6 Mbps. A sampled 47.61-second window
  delivered 2,865 encoded frames (60.17 fps), with zero buffer underruns, one
  Program-buffer deadline miss, and no sender restarts.
- YouTube Studio showed the eight-source picture and reported excellent stream
  health. This does not override the frame-delivery deadline requirement.
- Deliberately terminating only the test muxer exercised broken-pipe recovery. A
  stalled restart hit the bounded queue and was rejected; the supervisor recovered
  to a live stream at about 60 fps. See `recovery-log.txt` and `recovery.json`.
- The delayed-first-frame GPU test decoded 12 frames at mean coded luma 128.0.
- After retaining input credits, the localhost GPU gate passed: H.264 1920x1080,
  1,717 received frames / 28.56 seconds = 60.1 fps, final sink speed 1.10x.
- Forced raw fallback passed: 1,739 received frames / 29.04 seconds = 59.9 fps.
- `RtmpFfmpegArgs` tests cover RTMP, RTMPS and exclusion of TCP options from SRT.
- Final Release native suite: 946 tests passed, zero failed.
- Final staged core SHA-256:
  `6877B5B9A85829CAD1D4F4CE4F592338CC2FCC069C7FC2CA26509A71607584F2`.
  The meeting was rejoined with the full invite link and the saved scene restored.
  The final live sample delivered 2,744 encoded frames over 46.14 wall seconds
  (59.47 fps), with zero new deadline misses, two new buffer underruns, and zero
  sender restarts. These underruns also fail strict delivery acceptance.
  YouTube's previous broadcast ended during restart testing; the current receiver
  at video `68FRSG3aWkQ` reports Excellent, initially with Preparing stream shown.
  CoreVideo remains joined and transmitting. See `final-live-*.json`.

The earlier deadline miss remains a failed strict delivery observation. Throughput
improvement and finite clean windows must not be presented as an unlimited 60 fps
guarantee or used to waive that failure. PR #523 has not been merged by this work.

References: [asynchronous MFT event contract](https://learn.microsoft.com/en-us/windows/win32/medfound/asynchronous-mfts),
[FFmpeg RTMP protocol options](https://ffmpeg.org/ffmpeg-protocols.html#rtmp).
