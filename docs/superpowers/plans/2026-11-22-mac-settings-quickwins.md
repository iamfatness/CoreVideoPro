# Plan: macOS settings quick wins (A1, A5, Z1, Z2, O2, O1, Companion autoassign)

**Branch:** `mac-settings-quickwins`, stacked on `mac-control-audio-phase2` at `2d766924`.
**Sources:** master plan `2026-11-15-mac-parity-master-plan.md` Phase 1 rows A1, A5, Z1, Z2, O1, O2
(never done in Phase 1), specs `2026-11-15-mac-audio-parity-design.md` §4.2 to §4.3 and
`2026-11-15-mac-outputs-ingest-browser-design.md` §3.1, §3.4, §5.1, §5.4, plus the RC5 follow-up in
`2026-11-21-mac-control-audio-phase2-outcomes.md` ("`automation.autoAssignInputs.set` is the one
Companion preset that does nothing on a Mac").
**Rules:** `docs/superpowers/plan-authoring-rules.md`. Every test names the mutation it must turn red.

Order: A1 → A5 → Z1 → Z2 → O2 → O1 → C1. O2 comes before O1 because a recording size smaller than
the canvas is cropped today (`AVFoundationEncoderAdapter.mm:197`); O1 must not expose a size picker
until O2 scales.

## Decisions (made here, flagged in the outcomes)

- **D1 (A1).** The Mac keeps its own edit rule: an operator removal of a default send wins
  (`baseRoutingSends`, existing). Windows never lets a default be removed. On top of that, PerGuestIso
  applies the Windows doubling guard after the edits: any `zoom-mix` send to the five default buses is
  dropped, even one the operator enabled. ProgramMix gives `zoom-mix` the five default sends only
  while the meeting mix is subscribed, gives stems none, and keeps operator stem edits.
- **D2 (Z2).** The audio-sync offset is **session state, not persisted**. That matches Windows: the
  offset lives on `CaptureDeviceItem` and is in no prefs file. The outputs spec said "persisted"; the
  audio spec (§A9) says "not persisted". Windows is the arbiter. The offset rides
  `sync-capture-audio-sources` (`audioSyncOffsetMs`, `MediaCore.cpp:3461`), so every core generation
  gets it back. The dedicated `set-capture-audio-sync-offset` also goes out, because the AVF video
  device applies its own copy (`AvfCaptureDeviceAdapter.mm:184`).
- **D3 (O1).** The stream keeps **canvas size and the destination's fixed 30 fps** (spec §5.1: "stream
  size follows the canvas"; a stream fps picker would put 1080p60 BGRA through the CPU pipe). This is
  shown as text, not a picker. **Owner decision O-1:** allow 60 fps streaming before GPU-direct encode
  (O4) lands?
- **D4 (O1).** The stream codec stays H.264 only (no `hevc_videotoolbox` in `EncoderPolicy.h` on
  Apple; that is slice O3). The recording codec offers H.264 and H.265; AV1 is listed disabled with
  "AV1 recording is not available on this Mac". Recording quality is not exposed: the AVF writer has
  no quality knob, the bitrate is authoritative. Recording containers stay mp4 and mov (AVF cannot
  write mkv).
- **D5 (O1).** The hardware encoder is shown as fixed text, "VideoToolbox", with no picker (Windows
  lists NVENC, QSV, AMF and CPU, none of which exist on a Mac).
- **D6 (O1 merge).** The stream advanced settings are a pure value, `StreamEncoderSettings`, with
  `apply(to: JSONObject) -> JSONObject` that overwrites the encoder fields of one destination row.
  `streamDestinationSettings()` calls it once. The streaming lane's `StreamDestinationPolicy`
  builder makes every row; the merge applies `apply(to:)` to each of its rows (a one-line change).
- **D7 (C1).** Only `automation.autoAssignInputs.set` and the `autoAssignInputs` field are unblocked.
  `viewMode` (STU: the Mac has no Program/Preview/Multiview view switch) and `autoLowerThirds` (GFX)
  stay blocked. Publishing `false` for a feature that does not exist would be a guess.

## A1: Zoom audio mode

**Interfaces.**
- `enum ZoomAudioMode: String { case programMix, perGuestIso }` with `static func parse(_: String?)`
  (trim, case-insensitive, anything else → `.perGuestIso`) and `var persisted: String` (total switch).
  A line-for-line port of `ZoomAudioModePreference`.
- `AudioConsolePolicy.baseRoutingSends(zoomAudioIds:captureAudioIds:mediaRouted:operatorEdits:
  zoomAudioMode: = .perGuestIso, meetingAudio: Bool = false)`.
- `AppModel.zoomAudioMode` (published), `setZoomAudioMode(_:)` (sets, `commandStatus` = the Windows
  sentence, persists, `pushRoutingBase()`), `ShellPrefs.zoomAudioMode: String?`.
- `ControlSurface`: `audio.zoomMode.set` (`perGuestIso` | `programMix`, case-insensitive, else
  "mode must be 'perGuestIso' or 'programMix'."); state `zoomAudioMode`.
- UI: AUDIO header toggle "Per-guest ISO" / "Zoom program mix"; a guest strip reads "meter only" in
  ProgramMix.

**Tests** (shell). Fixture invariant: stems `11`, `22`; buses are exactly the five defaults.
1. Ports of the four `EnsureDefaultZoomAudioRoutingSends_*` cases with Mac edit semantics: ProgramMix
   gives `zoom-mix` the 5 buses at 0 dB; an operator stem edit `11→master −6` survives; `22` has none;
   total = 6. PerGuestIso with an operator `zoom-mix→master` edit drops it; both stems have 5 sends.
   Flip-back gives no stem sends. ProgramMix with `meetingAudio: false` gives no `zoom-mix` sends.
   *Mutations:* remove the guard (red: PerGuestIso test); always seed `zoom-mix` (red: meetingAudio
   false); seed stems in ProgramMix (red: count 6).
2. The five preference cases (parse, case/trim, fallbacks, format, round trip).
   *Mutation:* parse default `.programMix` (red).
3. Model: `setZoomAudioMode(.programMix)` → the next routing base observed through
   `routingBaseSendObserver` has `zoom-mix→master` and no stem sends; it persists and restores; the
   core-restart reassert batch carries the ProgramMix matrix.
   *Mutation:* forget `pushRoutingBase` in the setter (red: observer sees nothing).
4. Control: invoking `audio.zoomMode.set` with `programMix` changes the model; `chaos` refuses and
   changes nothing; state `zoomAudioMode` reads `programMix`.
   *Mutation:* drop the `case` (red: dispatch-drive plus this test).

## A5: Local audio source

**Interfaces.** `AppModel.localAudio: LocalAudioSetting { enabled, deviceId, deviceName }`, persisted as
`ShellPrefs.localAudioEnabled/localAudioDeviceId/localAudioDeviceName` (`localAudioKind` is always
`coreaudio-input` for A5, so it is not stored).
`LocalAudioPolicy.entry(setting:, devices:) -> JSONObject?` returns nil unless the setting is enabled
AND the device is connected. `LocalAudioPolicy.status(setting:, devices:, captureAudioSources:)`
ports `ResolveLocalAudioOperatorStatus`: "Local audio source off" / "Choose an audio input" /
"Waiting for audio" / "Audio detected" / "Starting audio source" / "No audio detected"; a saved
device that is absent reads "<name> is not connected", as the monitor picker does.
`captureAudioSourcesCommand()` appends the entry; `expectedAudioSourceIds` and `routingBaseCommand()`
treat `local-machine-audio` as a capture source (strip plus the 5 default sends).
UI: a SETUP card, "Local audio source", with a toggle, a device picker and the status line.

**Tests.**
1. Full-state command: enabled + connected → the entry `{captureDeviceId: local-machine-audio,
   audioDeviceId: <uid>, audioSourceKind: coreaudio-input, nativeAudioDeviceId: <uid>}`;
   disabled → absent; enabled but the device unplugged → absent; the core-restart reassert batch
   carries it. *Mutation:* ignore `enabled` (red).
2. Routing: enabled → `local-machine-audio` has the 5 default sends and a strip.
   *Mutation:* leave it out of `routingBaseCommand` (red).
3. The status table with a fixture `captureAudioSources[]` row
   (`signalPresent`/`captureStreaming` all four combinations). *Mutation:* swap two branches (red).
4. Prefs round trip, and a saved device that is absent keeps the choice.

## Z1: Zoom quality pickers

**Interfaces.** `ZoomCameraQualityPolicy` ports `ZoomCameraResolutionPreference` (choices, normalize,
`toSdkTier`, `fromSdkTier`, `status`) and `ZoomCameraFrameRatePreference` (choices, normalize,
`status`) verbatim. `AppModel.zoomCameraMaxResolution: String` ("1080p" default) and
`zoomCameraMaxFps: Int` (60), persisted; `zoomCameraApplied: (tier: Int, fps: Int)` is latched when a
join succeeds (the Windows `onMeetingJoined(tier, fps)` passes the values that join sent). The join
payload carries `cameraMaxResolution` (0-2) and `cameraMaxFps`. UI: two pickers and two status lines
on the Zoom tab.

**Tests.** The Windows `ZoomCameraResolutionPreferenceTests` table (tiers, defaults, pending/applied
copy, dimensions, fps copy). Join payload: set 720p/30 → the `zoom-join` payload seen by the harness
core has `cameraMaxResolution == 1` and `cameraMaxFps == 30`. A mid-meeting edit leaves the applied
values alone, and the status says "pending for next Zoom join". Prefs round trip.
*Mutations:* send the tier as a string (red: payload); latch at edit time instead of at the join (red:
pending test).

## Z2: Capture audio-sync nudge

**Interfaces.** `AppModel.captureAudioSyncOffsets: [String: Int]` (by device id, session only),
`nudgeCaptureAudio(deviceId:, deltaMs:)`, `resetCaptureAudioSync(deviceId:)`; clamps to ±500, sends
`set-capture-audio-sync-offset {deviceId, offsetMs}` (payload-wrapped, added to the
`CoreCommands` payload list) and re-sends `sync-capture-audio-sources` with `audioSyncOffsetMs` on that
device's entry. `CaptureAudioSyncLabel.text(ms)` ports `AudioSyncLabel` ("Audio sync 0 ms" /
"Audio delayed +N ms" / "Audio advanced N ms"). UI: −20 ms / +20 ms buttons and the label on each
capture device row (Sources capture section).

**Tests.** Twenty-six presses of +20 clamp at 500; the label table; the observed commands carry
`offsetMs` and `audioSyncOffsetMs`; the core-restart reassert includes the offset; the payload contract
test still matches the core source. *Mutations:* drop the clamp (red); leave the offset out of the
sources entry (red: reassert).

## O2: AVFoundation recording, scale not crop, HEVC, honest AV1 refusal

**Interfaces** (`AVFoundationEncoderAdapter.mm`). `AvfMp4Writer::open` honours `codec`: `h264` →
`AVVideoCodecTypeH264`; `h265`/`hevc` → `AVVideoCodecTypeHEVC` (AVAssetWriter writes `hvc1`); any other
value (`av1`) → `false` with errorOut "AV1 recording is not available on this Mac (no AV1 encoder in
AVFoundation)." `writeVideo` with a source of another size: aspect-fit scale with
`vImageScale_ARGB8888` into a centred rect, black bars, on the writer thread; the matching-size path
is unchanged. The session reports the real codec (`h264`/`h265`) and container (`mp4`/`mov`), not
literals.

**Tests** (native, `AvfEncoderRecording.*`).
1. `ProgramScalesToTheRequestedSizeWithoutCropping`: request 640x360, Program frames 1280x720 with a
   bright bottom-right quadrant; decode → 640x360, luma at (480,270) > 180 and the other quadrants
   < 110. *Mutation:* restore the crop (red).
2. `ProgramLetterboxesAnotherAspect`: 4:3 source 960x720 into 640x360 → the bars at x=20 are black,
   the centre is not. *Mutation:* scale without the aspect (red: bars).
3. `HevcRecordingWritesAnHvc1Track`: codec `h265` → the file's sample entry is `hvc1` (box scan) and
   `session.recordingVideoCodec == "h265"`. *Mutation:* ignore the codec (red).
4. `Av1RecordingIsRefusedLoudly`: `recordingStatus == "failed"`, the warning names AV1, and no file
   is written. *Mutation:* fall back to H.264 (red).
5. MOV reports `recordingFormat == "mov"`. *Mutation:* the literal (red).

## O1: Output profiles and advanced stream settings

**Interfaces.**
- `RecordingProfile { width, height, fps, codec, targetBitrateMbps, audioBitrateKbps }`, choices
  copied from Windows `OutputResolutionOptions` (720p, 1080p, 1440p, 4K), `OutputFpsOptions` (24, 30,
  50, 60) and codecs (h264, h265; av1 disabled); bitrate clamp 0.5...80, audio 32...512. It defaults
  to Windows `DefaultRecordingOutputProfile` (1920x1080, 60 fps, 8.2 Mbps, 192 kbps, H.264), which
  is also the core's own recording default, so an untouched Mac records exactly as before.
  (Corrected during implementation: the first draft said "follows the canvas"; Windows does not.) `payload()` gives the Windows
  `OutputProfilePayload` shape `{profileId: "<height>p<fps>", resolution: "WxH", width, height, fps,
  targetBitrateMbps, audioBitrateKbps, codec}`.
- `StreamEncoderSettings { audioBitrateKbps 160, keyframeIntervalSeconds 2.0 (0.5...10), rateControl
  cbr|vbr, h264Profile auto|baseline|main|high (high), bFrames 0...4 (2) }`, plus
  `apply(to:)` (D6). The existing stream bitrate and codec stay where they are.
- `start-program-output` carries `recordingOutputProfile` whenever recording is in the set;
  `set-recording-targets` carries `targetBitrateMbps`, `audioBitrateKbps` and `renderProfile`
  (Windows `MediaCoreCommandBuilder.cs:799-805`).
- Persisted under `ShellPrefs.recordingProfile` and `ShellPrefs.streamEncoder` (optional structs;
  a garbage value keeps the defaults).
- UI (Settings): a "Recording profile" group (size, fps, codec, bitrate, audio kbps), and in Streaming
  "Audio kbps", "Keyframe interval", "Rate control", "H.264 profile", "B-frames", "Hardware encoder:
  VideoToolbox", and "Stream size follows the canvas at 30 fps". The footer recipe line is built from
  the live values.

**Tests.** The payload builder vs a fixture copied from the Windows builder's field list (keys
exactly equal); a recording start carries `renderProfile` and the profile on both commands; the
destination row carries the advanced values, for every field; clamps; prefs round trip and garbage;
the recipe line reflects audio kbps and rate control.
*Mutations:* omit `renderProfile` (red); `apply(to:)` skipping `bFrames` (red: every-field loop);
recipe literal (red).

## C1: Companion `automation.autoAssignInputs.set`

`control.platforms.json`: `automation.autoAssignInputs.set` and `autoAssignInputs` → `supported`
(plus `audio.zoomMode.set` from A1); run `npm run contract:generate`. Dispatch:
`model.setAutoAssignInputs(bool(0))`, `.success`; state `autoAssignInputs`. Update
`ControlApiTestsPhase2`'s "absent on Mac" assertions to the new truth (they encode the old platform
file, so they must flip, not be deleted).
**Tests.** Invoking the action over the real HTTP server flips the model and the saved prefs; state
reflects it; the closure and dispatch-drive tests pass. *Mutation:* drop the `case` (red:
dispatch-drive).

## Carried obligations

- `docs/reference/gotchas.md`: a one-shot command must be re-applied on every core generation. A1
  (routing base), A5 and Z2 (capture sources) ride `reassertAudioConsole()`; O1 values ride every start.
- `/Library` core copy rule: no item here loads a file next to the core executable.
- Honest reporting: the AV1 option is disabled with its reason; "meter only" is stated; local audio
  status comes from the core's `captureAudioSources[]` row, never from intent.

## Verification

`swift build -c release` and `COREVIDEO_SHELL_TESTS=1` (baseline 5994 checks),
`COREVIDEO_SHELL_SELFCHECK=1`, native `corevideo-native-tests --gtest_filter='AvfEncoderRecording.*'`
(baseline 14) plus the full Metal ctest `-LE realtime`, `npm run contract:check`. Pixels from
`COREVIDEO_SHELL_TAB_SNAPSHOT` for SETUP, Zoom, Sources and Settings. Live checks (the owner's) are
listed in the outcomes.
