# Shared Program encode and destination muxing (#538)

Owner direction, 2026-09-29: streaming encode/mux is the top priority. This
spec describes the remaining work against the code that is actually shipped.
The [#538 issue](https://github.com/iamfatness/CoreVideoPro/issues/538) owns
the edge-output integration; #703 owns the current YouTube sync incident and
#605 owns hardware GOP and on-demand IDR. This document does not rank work.

## Current path and defects

On Windows, the normal stream path exports a D3D11 Program texture to a Media
Foundation hardware encoder. The RTMP/SRT/HLS sender copies its compressed
video into a separate FFmpeg process. A raw-video FFmpeg encoder is a
start-time fallback. The Program PCM tap is a second FFmpeg input, encoded to
AAC there. The same sender class constructs one hardware encoder and one
FFmpeg process per active destination. Thus the brief's raw-BGRA default
diagnosis is historical, while duplicate encoders and separate audio/video
clocks are current.

Each sender has a bounded compressed-video queue. The compositor takes the
largest requested export divisor from all senders, so a blocked destination
can reduce the video frame rate fed to healthy destinations. This violates
destination independence. Configured bitrate is not delivered bitrate; a
simple still picture can consume far less than a CBR target. The existing
changing-content hardware probe is the meaningful rate-control gate.

## Ownership and clock

The C++ media core owns Program pixels, PCM, encode, packet fanout, and queue
policy. The shell controls settings and displays evidence. The compositor
publishes a numbered, timestamped GPU Program frame. One profile-matched
`ProgramEncodedStream` owns the hardware video encoder and an AAC encoder
using the *existing Program PCM tap*. Its session clock is monotonic:

- Video PTS and DTS are the encoder's timestamps derived from the published
  compositor frame number. Burst callback arrival and pipe read time never
  become media timestamps.
- AAC timestamps advance by samples at 48 kHz from a single session origin.
  Missing audio is filled with exactly the absent sample count. A mute emits
  silence, preserving the sample clock.
- A missed video deadline is observable. The encoded stream either repeats
  the prior content frame with the missing timestamp or inserts a black frame
  with a fresh IDR after a discontinuity. Synthetic frames are counted
  separately from real compositor frames; they cannot make the 60 fps render
  gate look green.
- Stream startup fences audio and video to the first decodable video GOP.
  A reconnect starts at a new IDR without rebuilding the encoder.

The common output is H.264 Annex-B plus AAC access units with PTS/DTS and
codec configuration. An aligned MPEG-TS envelope may carry both through one
process pipe if it preserves these timestamps. No live input gets `-re` or
`-use_wallclock_as_timestamps`. FFmpeg may copy/remux but does not encode
raw Program video on the normal path.

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
hardware encode. The next moves AAC into the shared stream and puts both
access-unit clocks on one timeline. HLS joins the same publisher, then the
per-destination fault and 15-minute acceptance drills establish isolation.
