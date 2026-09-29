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

For the external leg, play the pattern from a dedicated source in the test
meeting, record a matching Program file while streaming, and obtain the
same-run YouTube playback. Decode the *same cue ids* in each artifact. Local
RTMP reception and a prior YouTube replay cannot establish the YouTube leg.
Do not apply a global delay from a single destination's offset.
