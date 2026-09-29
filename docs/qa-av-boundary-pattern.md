# A/V boundary pattern for #703

Generate the playable 1080p60 file with embedded stereo audio:

```powershell
node scripts/qa/generate-av-pattern.mjs artifacts/issue-703-pattern/flash-beep.mp4
```

The generator decodes its own output and refuses a file with missing cues or
source skew over one 60 fps frame. Its adjacent `.mp4.json` manifest records
the SHA-256, expected frame/sample positions, and measured source skew.
Eight full-frame flashes and 1 kHz beeps have distinct durations. Keep the
entire frame visible and audio unmuted. Distinct cue durations identify a
particular event even if a destination drops one.

For an isolated core run with fake Zoom input, one command captures the same
24-second run at core frame/PCM poll return, Program DXGI texture publish,
WASAPI monitor loopback, recorded Program, and a local RTMP receiver:

```powershell
node scripts/validate-av-clap.mjs --seconds 24 --live-paths --rtmp-local --keep-artifact
```

Use `--monitor-device` and `--monitor-id` for the actual operator endpoint.
`boundary-report.json` gives signed video-minus-audio offsets (negative means
audio late) and links the raw artifacts. Missing local stages fail the run.
The fake Zoom source is a controlled engine input, not a real Zoom client or
Meeting SDK sender. The source markers are core poll-return times, not Zoom
sender timestamps. DXGI texture publish is not physical display vsync.

To test media playout through the engine and finalized recording at both
Program buffer depths:

```powershell
node scripts/qa/program-buffer-recorded-av.mjs --native-core native/build-dev/corevideo-native.exe --output-dir artifacts/issue-703-media-playout
```

The media harness now uses the same generated file and records mixer evidence.
Its result must stay red if media PCM or identifiable audio cues are absent.

For a sender plus recording run, pass the RTMP server without the stream key in
the command line. Set `COREVIDEO_QA_RTMP_KEY` in the process environment from
the app's saved test destination, then run:

```powershell
node scripts/qa/program-buffer-recorded-av.mjs --native-core native/build-dev/corevideo-native.exe --output-dir artifacts/issue-703-youtube-pattern --rtmp-server rtmp://a.rtmp.youtube.com/live2 --duration-seconds 24 --depth 2
```

The report refuses an RTMP sender that has video but no audio frames or bytes,
or whose destination supervisor has no fresh accepted-output proof. It records
sender health and the matching Program file; this is a **send**
measurement. It does not measure what YouTube received or played. The fixture
does not loop in this longer run, so each of its eight cue IDs appears once.
`--depth` isolates one startup buffer; omit it to compare both next-launch
depths in separate core processes. Redact the key from shared artifacts.

To separate FFmpeg's muxed timeline from YouTube ingest on the **same** send,
add `--rtmp-tap 1`. The harness writes `muxed-before-youtube.flv` in its run
folder and fails if the file is empty or lacks identifiable cues. The QA-only
FFmpeg tee sends the same encoded packets to the local FLV and RTMP. A receiver
that never accepts the stream can block the tee before the tap gets packets;
an empty tap is missing evidence. Analyze the tap as `--rtmp` with
`measure-av-pattern.mjs`, alongside the matching Program and YouTube files.

The 2026-09-29 H.264 clock A/B used `validate-av-clap.mjs --seconds 24
--rtmp-local --program-buffer 2 --keep-artifact`. The old elementary stream
got timestamps from FFmpeg pipe reads: decoded RTMP moved from −22.6 ms
(no tap) to +47.1 ms (with tap), video minus audio. The timestamped H.264
envelope kept the encoder frame clock: +13.4 ms without the tap and +12.5 ms
with it. The local tap and receiver agreed. A subsequent default-path solo
run measured RTMP +13.2 ms and recording −11.5 ms. A live-path run measured
RTMP +2.1 ms, recording −8.3 ms, and Program texture publish minus GoXLR
monitor −34.4 ms, with zero monitor underruns and zero audio lost samples.
These synthetic local measurements do not close the YouTube playback or
real-human gates.

The timestamped H.264 path also passed the 55 s GPU-direct slow-sink gate with
`--codec h264 --slow-sink --burst-sink --sink-rate 0.5`: the stream stayed live,
one encoder was built, Program averaged 60.0 fps, and the stream export divisor
recovered to 1 before the next burst. The proxy reached 1379 ms buffered and
the overflow path discarded one GOP tail without failing the sender. A prior
0.85x run was missing congestion evidence because its link did not actually
throttle the encoded stream.

For the external leg, play the pattern from a dedicated source in the test
meeting, record a matching Program file while streaming, and obtain the
same-run YouTube playback. Decode the *same cue ids* in each artifact. Local
RTMP reception and a prior YouTube replay cannot establish the YouTube leg.
The YouTube watch URL for a new test is created only after the owner starts
the live event; record that URL with the send run before claiming a received
measurement.
Do not apply a global delay from a single destination's offset.

Analyze captured files with:

```powershell
node scripts/qa/measure-av-pattern.mjs --source artifacts/issue-703-pattern/flash-beep.mp4 --recording PATH_TO_PROGRAM_MP4 --rtmp PATH_TO_RECEIVED_FLV --youtube PATH_TO_SAME_RUN_YOUTUBE_CAPTURE --require-all --output artifacts/issue-703-pattern/end-to-end.json
```

`--require-all` exits unsuccessfully when a leg or identifiable cue is absent.
All legs report the sign, spread, frames at 60 fps, and offset corrected for
the source file's own tiny skew.

FLV and YouTube HLS captures can quantize AAC timestamps. For those received
legs, the decoder permits equal video PTS and up to 24 samples (0.5 ms) of AAC
PTS jitter against the decoded sample count; it still rejects backward video
PTS and larger audio gaps. The report retains `duplicateVideoPts`,
`audioPtsJitterSamples`, and the applied tolerance. Recording and source files
keep the strict timestamp checks. Trim a YouTube capture around the same cue
interval if stream reconnects elsewhere in the file introduce a discontinuity;
retain the original capture as evidence. This allowance helps measure A/V
content; it is not a frame-delivery pass.
