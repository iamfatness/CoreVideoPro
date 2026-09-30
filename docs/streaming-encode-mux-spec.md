# Shared Program encode and destination muxing (#538)

Owner direction, 2026-09-29: streaming encode/mux is the top priority. This
spec describes the remaining work against the code that is actually shipped.
The [#538 issue](https://github.com/iamfatness/CoreVideoPro/issues/538) owns
the edge-output integration; #703 owns the current YouTube sync incident and
#605 owns hardware GOP and on-demand IDR. This document does not rank work.

## Shipped path (Windows)

The normal stream path exports a D3D11 Program texture to one Media Foundation
hardware H.264 encoder shared by every profile-matched RTMP/SRT/HLS
destination (Slice 1). One native Media Foundation AAC encoder (48 kHz stereo,
160 kbps, ADTS) encodes the Program stream tap once for the same destinations
(Slice 6); PCM-to-FFmpeg AAC is a fallback only. Each destination owns one
FFmpeg process that remuxes (`-c copy`) and never encodes. A raw-video FFmpeg
encoder remains a start-time fallback. Explicit Program mute writes exact
silent samples; source-video dropout recovery and SEI-padded CBR keep a still
Program near the configured receiver rate (Slice 7).

Since Slice 8, H.264 and AAC reach each FFmpeg as **one** core-timestamped
MPEG-TS on stdin (see "Ownership and clock"). Before it, video arrived in a TS
envelope while AAC was bare ADTS on a second pipe, so FFmpeg derived the two
input timelines independently and aligned them by arrival (plus a fixed
three-packet startup trim). The PCM fallback and AV1 elementary input keep
their two-input layout; macOS keeps its PCM fallback and has no shared AAC.

Each sender has a bounded compressed-video queue. Before Slice 2, the
compositor took the largest requested export divisor from all senders, so a
blocked destination reduced the frame rate fed to healthy destinations.
Slice 2 leaves the shared encoder texture at full Program cadence and lets
each sender discard to a decodable GOP boundary in its own queue. The legacy
per-sender `divisor` remains a pressure recommendation; `appliedDivisor` is
the actual compositor rate and must remain 1 under stream congestion.
Configured bitrate is not delivered bitrate; a simple still picture can
consume far less than a CBR target. The changing-content hardware probe is
the meaningful rate-control gate.

## Ownership and clock

The C++ media core owns Program pixels, PCM, encode, packet fanout, and queue
policy. The shell controls settings and displays evidence. The compositor
publishes a numbered, timestamped GPU Program frame. One profile-matched
`ProgramEncodedStream` owns the hardware video encoder and an AAC encoder
using the *existing Program PCM tap*. Its session clock is monotonic:

- Video PTS and DTS are the encoder's timestamps derived from the published
  compositor frame number (`frame x 10^7 / fps` in 100 ns). Burst callback
  arrival and pipe read time never become media timestamps.
- AAC timestamps advance by samples at 48 kHz from a single session origin.
  Missing audio is filled with exactly the absent sample count. A mute emits
  silence, preserving the sample clock.
- `ProgramStreamClock` (owned by the composite sender that owns the shared AAC
  encoder) fixes the origin once per stream session, the way the recording
  aligns its tracks. The Program frame the video tick hands the encoder carries
  its steady-clock timeline time (the program buffer's delivery deadline); the
  first PCM block of the session carries the audio worker's SCHEDULED tick
  time (its absolute 20 ms deadline grid, not `now()`, which adds up to a tick
  of scheduling lateness exactly when a stream is starting). Every AAC packet
  is stamped `anchorFrame / fps + (T_audio - T_frame) + (sample - anchorSample)
  / 48000`. Measured on the Windows rig: frame number and timeline slip 0.00 ms
  over a run, so the frame clock is exact. A frame-number-only anchor quantized
  the relation to a whole frame per session. A destination reconnect or a fresh
  IDR does not touch the anchor. The session ends only when no RTMP/SRT/HLS
  destination is requested, because the sample counter then stops while frames
  continue.
- A missed video deadline is observable. The encoded stream either repeats
  the prior content frame with the missing timestamp or inserts a black frame
  with a fresh IDR after a discontinuity. Synthetic frames are counted
  separately from real compositor frames; they cannot make the 60 fps render
  gate look green.
- Stream startup fences audio and video to the first decodable video GOP.
  A reconnect starts at a new IDR without rebuilding the encoder.

The common output is H.264 Annex-B plus AAC access units with PTS/DTS and
codec configuration. **Chosen mux input (Slice 8): one aligned MPEG-TS
program per destination** - H.264 on PID 0x100 (PCR) and ADTS AAC (stream
type 0x0f) on PID 0x101 - written by the destination's single writer thread
into FFmpeg's stdin. It is smaller than two timestamped pipes (one pipe, one
writer, one demuxer) and FFmpeg's one input offset applies to both streams,
so FFmpeg cannot invent a relation between them. The only per-process value is
the TS epoch, the first written IDR's DTS, subtracted from both PIDs: a
container offset that never changes A/V relation. Startup fences audio to the
first IDR (earlier audio is dropped) and writes the first AAC PES before the
IDR so FFmpeg's bounded probe sees both streams. The live command line is
`-f mpegts ... -i pipe:0 -map 0:v:0 -map 0:a:0 -c:v copy -c:a copy`; no live
input gets `-re`, `-use_wallclock_as_timestamps`, `-async` or an `aresample`
clock. FFmpeg may copy/remux but does not encode raw Program video on the
normal path.

## Profile sharing and admission

The profile identity includes Program tap, codec, resolution, frame rate,
target bitrate, rate control, H.264 profile, B-frame mode, and GOP interval.
Destinations with identical profiles subscribe to one video and AAC encode.
The profile is negotiated before starting any muxer. A destination that needs
an incompatible codec/profile must either be refused with a named reason or
admitted as an explicitly counted second encoder session under
`EncoderCapacityProbe`; it must never silently change another destination's
profile or quality. The supported baseline is H.264 High, 1080p30 or 60,
48 kHz stereo AAC at 160 kbps, CBR target 4.5–6 Mbps at 30 fps or 6–9 Mbps
at 60 fps, and an IDR at most two seconds apart. Hardware and destination
acceptance decide whether B-frames are safe; no global B-frame assumption.

The raw FFmpeg path remains an operator-visible fallback on systems where a
hardware encoder cannot start. It is not called GPU-direct and is not used to
claim hardware throughput or destination independence.

## Destination isolation

RTMP/RTMPS, SRT caller, and classic HLS are peer mux workers. Each owns a
bounded packet queue, process or native socket, publish/retry state, and
delivery counters. Fanout copies immutable access-unit references into each
queue without waiting for a network write. If one queue is blocked, only that
queue drops to a decodable IDR boundary and requests a fresh IDR when needed.
The shared encoder and compositor keep their cadence. A transport restart
does not reset the shared PTS origin or tear down another worker.

RTMP copies H.264/AAC into FLV with one process per URL. SRT copies the same
access units into MPEG-TS with caller mode and a destination-specific latency
setting. HLS writes two-second independent segments and a sliding playlist
to the configured PUT origin. LL-HLS is a separate compatibility step after
the origin/player supports it; classic HLS remains first-class. A test-only
local mux tap can be attached to one destination, but production URLs are
never teed into one FFmpeg process.

## Evidence contract

Per destination, publish configured and measured video bitrate separately;
actual encoded and delivered fps; audio bitrate and sample continuity;
last IDR age; accepted bytes and packets; queue age/drops; transport publish
state; and SRT RTT/loss or HLS PUT result where available. A live counter or
FFmpeg `speed=` alone cannot assert decoded A/V sync. #703's timed pattern
compares Program/monitor, recording, muxed packets, local receiver, and the
same-run YouTube playback using identical cue IDs.

| Gate | Required observation |
|---|---|
| Hardware throughput | 1080p30 and 60 for ten minutes with no destination; encoder and compositor frame delivery measured separately, no silent hardware skip. |
| Rate | Changing-content receiver video payload within 10% of the chosen CBR target after settling, with no input shedding. A simple still source is diagnostic only. |
| A/V | At least 15 minutes of decoded output with no growing drift; same-run local and YouTube cues for the incident; mute/unmute and source dropout preserve the clock. |
| Independence | RTMP, SRT, and HLS armed together; blocking or killing RTMP leaves SRT, HLS, Program, and recording live with one shared encoder build. |
| Reconnect | New RTMP publish begins with a self-contained IDR; the encoder and other mux workers do not restart. |
| Recording | Program and ISO files finalize and decode while all three outputs run. |

Shipping code moves in vertical slices with a real consuming destination in
each PR. The first slice proves profile-matched RTMP and SRT share one
hardware encode; the same sender path also covers HLS. Slice 2 pins full
Program cadence while an RTMP socket blocks and requires SRT receiver bytes
and HLS PUT segments to keep advancing. Shared AAC is shipped (Slice 6), and
Slice 7 holds receiver bitrate near CBR. Slice 8 stamps compositor/sample PTS
on the shared H.264 and AAC in the core and muxes both through one TS input.

Evidence for Slice 8 is in the snapshot, not the process log (the log is a
bounded best-effort queue that drops startup lines): each sender publishes
`streamClock {muxInput, firstVideoPts100ns, firstAudioPts100ns, audioUnits,
audioRefused, audioUnanchored}`, and `validate-shared-stream-encode.mjs`
requires `muxInput: unified-ts` with fenced audio on RTMP, SRT and HLS. The
clap gate reads decoded audio on the container timeline (it adds the audio
stream's `start_time`). Before this slice both FFmpeg inputs started at zero,
so the gate never saw that it ignored the start; a fenced TS legitimately
starts AAC up to one AAC unit after the first IDR.

#708 is closed: its lingering child was harness-caused (the gates sent the
unimplemented `stop-program-output`); the real stop reaps within 5 s under
congestion and removed HLS destinations are now interrupted too.

Still open after Slice 8: #703 same-run
YouTube watch-URL evidence on the Slice 8 head; #615 operator readout; macOS
native AAC (macOS keeps the PCM fallback and does not claim Slice 6 or 8).
Found, not fixed here: with shared AAC enabled but no audio layout (or
`COREVIDEO_RTMP_DISABLE_REAL_AUDIO=1`), the builder still copies the `anullsrc`
PCM input (`-c:a copy`), which FLV refuses; the same condition exists on main.
