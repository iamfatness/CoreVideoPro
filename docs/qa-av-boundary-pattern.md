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

The report refuses an RTMP sender that has video but no audio frames or bytes.
It records sender health and the matching Program file; this is a **send**
measurement. It does not measure what YouTube received or played. The fixture
does not loop in this longer run, so each of its eight cue IDs appears once.
`--depth` isolates one startup buffer; omit it to compare both next-launch
depths in separate core processes. Redact the key from shared artifacts.

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

FLV carries millisecond timestamps. For the RTMP leg only, the decoder permits
equal video PTS and up to 24 samples (0.5 ms) of AAC PTS quantization against
the decoded sample count; it still rejects backward video PTS and larger audio
gaps. The report retains `duplicateVideoPts` and `audioPtsJitterSamples`.
Recording and source files keep the strict timestamp checks. This allowance
helps measure A/V content; it is not a frame-delivery pass.
