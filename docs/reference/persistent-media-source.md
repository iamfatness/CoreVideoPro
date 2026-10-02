# Media is a persistent source (slice 1, 2026-09-10)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Slice 1 of `docs/superpowers/specs/2026-09-10-persistent-sources-design.md`: a
media asset is now one decoder with one clock, not one per bus.

**READ THIS FIRST: parts of this section were REPLACED by #535 slice 3b
(merged 2026-09-21, [#567](https://github.com/iamfatness/CoreVideoPro/pull/567)).**
The `preview:media:<id>` cue-poster namespace, the
`media:<id>:live:<n>` go-live generation, `MediaGoLiveLedger`'s operator-paused
set and the cue→Program decoder hand-off are all DELETED code — play/pause/cue
state lives in the core now, one source per asset. The paragraphs below are kept
because their reasoning is why the replacement has the shape it does; each one
says what its rule WAS and what took its place. The live rules are in "Slice 3b"
in the source-bus section below. **The T1.2 "pause is a clock state, not a new
decoder" reasoning is unchanged and still true.**

- **Same source id on both buses.** `buildPreviewCompositorRenderPlan` no longer
  renames every Preview media layer to `preview:<id>` — Program and Preview
  address the SAME source: `media:<assetId>` for a routed asset or still,
  `background:<assetId>` for a scene background. So the decoder owner runs one
  decoder (and `StillMediaFrameCache` one entry) for a background or still
  shared by both scenes, and a Take cannot restart it. (That owner was
  `modules::OwnedMediaFrameSource` until slice 3b; it is `core::MediaTransports`
  now, and the ids are unchanged.) **There WAS one exception,
  retired by slice 3b:** a paused, non-still `media-video` layer (a clip cue
  poster) still got a `preview:` key, because its paused poster frame was a
  different playback position from Program's rolling copy and the two must never
  replace each other in the frame set. Slice 3b removed the difference instead of
  the collision — a cue and its live clip are ONE entry with ONE clock, so the
  `preview:` namespace and the `pausedClipCue` re-key in
  `buildPreviewCompositorRenderPlan` are deleted and a clip on both buses shows
  one rolling picture by construction.
- **Route stills never reach a decoder.** A still on both buses arrives playing
  on Program and paused on Preview; the decoder owner skips `media-video` stills
  (`isStillImageMediaAsset`, the MF adapter's own filter), so they are served only
  by `StillMediaFrameCache` — no dead decoder threads, no false "two playback
  identities" warning. Background stills keep the decoder path. **Still true after
  slice 3b, one level up:** the filter now lives in
  `MediaCore::syncMediaTransportsDesired()`, which never puts a still in the
  desired set, so a still can never mint a `media:<assetId>` transport that would
  shadow the cache's own bus source.
- **The route wire carries the loop flag** (`mediaAssetLoop`, from
  `MediaRoutePlaybackService.IsLoopingAsset`), parsed at BOTH scene parse sites
  onto `SceneRouteState` and the layer — without it a looping route asset played
  once and froze. The preview-scene dedup signature includes the media path,
  playback key, playing and loop flags, so a change to any of them is applied.
  **Slice 3b:** `mediaAssetLoop` is still parsed and is now an input to the
  transport's desired row; the playback-key and playing halves of that signature
  went with the fields themselves.
- **Loop vs clip go-live policy — THIS WAS THE SHELL'S JOB UNTIL SLICE 3B, and
  is now the core's.** Slice 3b deleted the playback key, the generation and
  `MediaGoLiveLedger` outright: the core decides go-live from scene membership at
  command time ("enters Program → roll from 0", "stays on Program across a Take →
  nothing"), and the shell keeps only `AssetsEnteringProgram` + `ChooseAssetToPromote`
  to move the bin SELECTION. The rule as it stood, and why it was shaped that way
  (shell, `MediaRoutePlaybackService` /
  `TransportCoordinator` / `StudioViewModel`): a looping background asset (kind
  "background") plays under key `media:<id>` on both buses, identical to Program,
  matching §2 of the spec ("nothing" on go-live for loops). A clip (non-loop)
  plays under `media:<id>:live:<n>` — paused on first frame while only in
  Preview, rolling from 0 with audio on only when it actually GOES LIVE. `n` is
  the `MediaGoLiveLedger` generation for that asset, which advances only when
  the asset enters Program for the first time — never on every Take, and never
  from Pause/Play on an already-live clip (pause is a clock state, below) — so a
  clip already playing on Program stays rolling across an unrelated cut. `ChooseAssetToPromote` /
  `ITransportHost.RecordProgramMediaGoLive` ran promotion only for the assets
  that actually went live, and never for a still (`SupportsPlayback` filter).
  `ChooseAssetToPromote` and the `SupportsPlayback` filter survive 3b;
  `RecordProgramMediaGoLive` does not.
  **Operator pause is PER-ASSET state, not "is it the selection"** — the rule
  survives slice 3b, its owner does not: pause is now the CORE's per-source
  transport state, set by the one-shot `set-media-transport` gesture and read back
  from `mediaSources[]`, so there is no shell-side paused set to keep in step with
  anything. As it was:
  `MediaGoLiveLedger` kept an operator-paused set — pausing a Program-routed clip
  added it, playing it removed it, the clip GOING LIVE cleared it — and
  `ShouldPlaySceneMediaRoute` / `ResolveSceneRoutePlayback` read that set (loops
  always play). Promotion only moves the selection, so a paused clip that stays
  on Program stays paused when another asset goes live; selecting a Program clip
  in the bin reports its real state instead of pausing it. Both of those
  properties still hold in slice 3b, for a better reason: the pause is a property
  of the core's transport, so nothing the shell does to the selection can disturb
  it, and the bin reads the state back rather than asserting one.
- **PAUSE IS A CLOCK STATE, NOT A NEW DECODER (T1.2, 2026-09-10).** `playing` used
  to be part of the decoder's identity in two places — the owned source's request
  key and the MF adapter's playback identity — so promoting another asset (or a
  bin-row tap) that flipped a clip's `playing` flag opened a fresh decoder: Pause
  cut to the clip's first frame, Play restarted it from 0. **Both identities now
  EXCLUDE `playing`.** `MediaPlaybackTimeline::configure` only resets (new epoch,
  `++generation`) on an identity change; a playing→paused transition freezes
  elapsed time at its current value, and paused→playing resumes from exactly
  that value (the epoch shifts by the paused duration). A paused clip HOLDS its
  on-air frame (same `frameId`, no read) rather than showing a poster; audio
  emits nothing while paused (not even silence) and resumes at the clock
  position — `MediaAudioWindows` keeps 200 ms of decoded-ahead history so the
  windows that would otherwise become a silent hole at the pause point are
  replayed instead of dropped, and resume re-times the already-prepared frames
  by the paused duration rather than snapping them to "now". The FFmpeg fallback
  path (ProRes, which cannot pause a running process in place) is stopped on
  Pause and restarted AT THE FROZEN CLOCK POSITION on Play — never from the top
  — with frame ids kept rising; a restart that fails keeps the resume pending
  and retries at the clock position on a bounded ladder (250 ms, 500 ms, 1 s,
  2 s, give up after 5 attempts), holding the paused frame and warning every
  poll, and a fresh operator Pause re-arms a resume that gave up (the FFmpeg
  tests self-skip with a `[ SKIPPED ]` stderr line, not a silent pass, when
  `C:\ffmpeg\bin` is absent from the build machine). In
  `MediaCore::renderSyntheticTick`, Program's media layers are gathered BEFORE
  Preview's are appended — Program-first ordering is what keeps Program
  authoritative when a shared source id (same clip on both buses) arrives
  paused on Preview: the Program request always wins the one shared clock (the
  owner let the FIRST request for a key win). **Slice 3b retired that whole
  race:** nothing is derived from the plan any more, so there is no per-tick
  request ordering to get right — the desired row for a source id carries
  `onProgram`/`onPreview` together and the state machine decides once, at command
  time. **Restart from the top used to happen ONLY via the go-live generation**
  (`media:<id>:live:<n>` advancing) — never from Pause, Play or a bin-row tap.
  Slice 3b keeps the property and drops the generation: a restart happens only on
  a transition the desired set names (entering Program from absent, leaving
  Program while cued, a path change). The Preview cue-poster exception named
  above is gone with it.
  **The shell surfaces the real on-air state, not "is it the selection"**
  (`MediaRoutePlaybackService.IsPlayingOnAir` / `ResolveTap`): the media bin
  row shows a rolling Program clip as playing even when it is not the current
  selection (`ApplyMediaSelection` calls `IsMediaAssetPlaying` per row), and
  tapping that row pauses it. That rule is unchanged; what carries it is not.
  It USED to be the ledger (`MediaGoLiveLedger.RecordPause`/`RecordPlay`, with
  `PlayMediaAsset` no longer restarting anything and `RecordRestart` deleted as
  dead code once its one caller was removed). **Since slice 3b the row reads
  `mediaSources[]` and the tap sends the one-shot `set-media-transport` gesture;
  the ledger is gone entirely.** **The transport toggle (`MediaPlaybackButtonLabel`,
  "Pause Program"/"Resume Program"/"Audition") still only reflects the
  SELECTED asset** — `FormatMediaPlaybackActionLabel` reads
  `SelectedMediaAssetPlaying`, not the tapped bin row's id, so it does not (yet)
  show an unselected rolling clip's state; only the bin row does. A tap on a
  looping asset (kind `background`) is always just a selection
  (`MediaTapAction.Select`) — a loop is always playing and has no useful pause
  state (since 3b, `decideMediaOperator` refuses pause/play for a loop outright). `TransportCoordinator.TakeAsync` (and
  `StudioViewModel.UpdateScene`) also calls
  `ITransportHost.RefreshMediaBinPlaybackIndicators` so a clip that LEAVES
  Program on a Take stops reading "playing" — `PromoteProgramMediaRouteToPlayback`
  only rebuilds the bin when something ENTERED — and it clears
  `SelectedMediaAssetPlaying` when the SELECTED clip itself is the one that
  left, so the transport toggle cannot keep reading "Pause Program" for a clip
  no longer on air. **It is called CONDITIONALLY, not on every Take** (folded
  in from a review pass): only when the Program media SET actually changed
  (`StudioViewModel.BuildProgramMediaRouteSignature` before vs after) AND
  `PromoteProgramMediaRouteToPlayback` did NOT already run (that call's own
  `ApplyMediaSelection` rebuild already gives every row — not just the
  promoted one — its current on-air state, so a second rebuild in the same
  Take would be pure waste). This is what keeps an automated Magic Scene Take
  between two non-media scenes from rebuilding `MediaBinGroups` on every cut.
  **A ROLLED-BACK TAKE RESTORES THE MEDIA SELECTION TOO (T1.3, #430).** The #286
  rollback (`CaptureTakeRollback`) only ever put the SCENES back. A failed Take
  kept the selection Promote had moved to a clip that went live. That clip was
  still marked playing and kept auditioning locally with audio, and the status
  said it was on Program. Worse, a Program clip X that LEFT on the failed Take
  had its playing flag cleared. The rollback put X back on air, still rolling,
  yet the toggle read "Resume Program", and pressing it paused X ON AIR.
  `TransportCoordinator.TakeAsync` now captures the selection
  (`ITransportHost.CaptureMediaSelection`) before and after the local mutations.
  On a SUCCESSFUL scene rollback the pure `TakeMediaSelectionRollback.Resolve`
  decides what stands: the pre-Take selection, unless the operator moved it
  while the sync was pending (their choice is kept, like the scene rollback's
  newer-edits rule); and for a clip on the restored Program, the playing flag
  and status come from the real on-air state (`IsPlayingOnAir`, which since slice
  3b reads the core's `mediaSources[]` rows rather than a shell-side paused set),
  never from the saved flag. One more case: if the operator picked a clip
  on the ATTEMPTED Program and the rollback takes it off air, it reads "<name> left
  Program" and is not playing. `RequestTakeReconciliation` runs right after the scene
  rollback, before `RestoreMediaSelectionAfterRollback`, so a throwing restore
  cannot skip it. The restore then rebuilds the bin ONCE. A refused rollback
  restores nothing. The go-live
  ledger and the paused set were deliberately NOT rewound — and since slice 3b
  there is neither: the core's transport state is the only play state, so a
  rollback has nothing of the kind left to rewind. Tests:
  `TransportCoordinatorTests.Take_Rollback*` and
  `Take_RefusedRollbackLeavesTheSelectionAlone`.
  Tests: `native/tests/MediaPlaybackTimelineTest.cpp`
  (`MediaPlaybackTimeline.PauseFreezesElapsedAndResumeContinues`,
  `MediaTransports.PauseAndResumeKeepOneDecoder` /
  `PauseHoldsTheOnAirFrame` / `NoAudioWhilePausedAndAudioResumes` — the
  `OwnedMediaFrameSource.*` cases under their post-3b owner),
  `native/tests/MediaCoreCommandTest.cpp`
  (`AFailedFfmpegResumeRetriesAtTheClockPositionNeverFromTheTop`), and
  `MediaRoutePlaybackServiceTests` (`ResolveTap_*`, `IsPlayingOnAir_*`,
  `SelectedAssetLeftProgram_*`) / `TransportCoordinatorTests`
  (`Take_RefreshesTheMediaBinWhenAClipLeavesProgramWithNothingGoingLive`,
  `Take_DoesNotDoubleRefreshWhenPromoteAlreadyRebuiltTheBin`,
  `Take_DoesNotRefreshTheMediaBinWhenTheProgramMediaSetIsUnchanged`) on the
  shell side.
- **A CUED CLIP HANDS ITS WARM DECODER TO PROGRAM (T1.11 / #449, 2026-09-12) —
  REPLACED BY SLICE 3B, 2026-09-20.** The hand-off, `modules/MediaCueHandoff.h`,
  `adoptCuedDecoders` and `Entry::everPlayed` are all DELETED. What replaced it:
  a cue and its live clip are ONE source id and ONE entry, so entering Program is
  an in-place **Resume** of the decoder that was already posting frame 0 — there
  is no arriving request to match, nothing to re-key, and none of the five
  conditions below to get right. The reasoning is kept because it is why the
  replacement is shaped the way it is (and trap 2 below — dropping the queued
  frames scheduled against the paused epoch — survives inside the Resume action).
  As it was:
  The Preview cue poster (`preview:media:<id>`) and the rolling Program source
  (`media:<id>`) are two decoders, because a clip changes identity TWICE on
  go-live: the `preview:` namespace collapses, and `MediaGoLiveLedger` advances
  the generation baked into the playback key (`media:<id>:live:<n>` ->
  `:live:<n+1>`). So the arriving request matched no entry, a cold decoder
  opened, and for the ticks before its first frame `resolveLayers` painted
  `colorFromParticipantId` over PROGRAM — the placeholder flash. Now
  `OwnedMediaFrameSource::adoptCuedDecoders` RE-KEYS the cue's entry onto the
  live request instead of retiring it. `Entry` is a `shared_ptr` whose worker
  holds its own reference, so the hand-over is a map re-key: the decoder and its
  held poster never notice. **This is not an exception to the go-live contract,
  it IS the contract** — the cue poster sits paused at frame 0
  (`MediaVideoPresentation::hold` shows the first prepared frame and never
  advances), so resuming it is exactly "roll from 0, audio on".
  **The decision is pure** (`modules/MediaCueHandoff.h`, the
  `CaptureReaderStallPolicy`/`TakeRecordPolicy` shape) and every condition is
  required: same asset id AND same path (a repointed bin row holds the old
  file's pictures), same loop flag, the retiring `sourceId` is exactly
  `"preview:" + arriving.sourceId`, the arriving generation is exactly the
  retiring one +1, and — the load-bearing one — **the cue NEVER ROLLED**
  (`Entry::everPlayed`). A decoder that has played is at an arbitrary position,
  and adopting it would put a clip on air mid-roll while the take record still
  read `cut`. Two candidates for one arrival is refused loudly and cold-starts:
  never guess which cue is the predecessor.
  **Three traps, each found by a test that failed first:**
  1. **It runs on the REQUEST path (`selectVideo` / `pollMediaAudioFrames`),
     never in `manage()`.** That is the difference between one flashed frame and
     none — the request set changes on the take tick, but `manage()` is a
     separate thread on a 2 ms wait, so an adoption deferred to it lands a tick
     late. Adoption starts no thread and does no I/O, which is what makes it
     safe on the caller's path where creating a worker would not be.
  2. **The queued frames are DROPPED (`MediaVideoPresentation::dropQueued`),
     `current_` is kept.** They were scheduled against the cue's paused epoch
     and can never come due on the go-live clock, so keeping them freezes the
     clip on its poster forever. The held poster is what covers the refill.
  3. **The OWNER names the source, not the decoder.** Every decoder stamps
     `participantId` from the layer it was handed, so `selectVideo` re-stamping
     it is normally a no-op — but an adopted poster was decoded under the
     `preview:` id, and the compositor looks a media layer up by the LIVE source
     id. Without the re-stamp the hand-off delivered a frame nothing could
     match and Program painted the placeholder anyway.
  The take record now reads `missingSources=[]` for this case because the cold
  start stopped happening — **the judge was not touched, and must not be**.
  Tests, as they were: `MediaCueHandoffTest.cpp` (every refusal),
  `OwnedMediaFrameSource.ACuedClipHandsItsWarmDecoderToProgram` /
  `ACueThatAlreadyRolledIsNeverHandedOver` /
  `AnAdoptedCueRollsInsteadOfFreezingOnItsPoster` / `AnAdoptedCueTurnsItsAudioOn`
  — all deleted with the hand-off. What survives and now covers the property is
  `MediaTransports.ACuedClipEnteringProgramRollsTheSameDecoderWithAudio` /
  `LeavingProgramWhileCuedRestartsBehindTheHeldFrame` and
  `ProgramPixelContinuity.ACuedClipTakenToProgramNeverShowsThePlaceholder` (the
  end-to-end pixel proof, unchanged as a test, now driven by a REAL
  `core::MediaTransports` over a real decoder factory).
  **Still cold-starts, honestly:** a clip cut to Program that was never cued in
  Preview has no warm decoder to adopt. That is step 1 of #449 (hold the
  outgoing picture until the first real frame), **still not done** — slice 3b did
  not change it either, and `scripts/qa/media-take-ab.py --skip-cue` is the
  control run that measures exactly that gap (it FAILS, deliberately).
- **The 16-decoder cap warning names the refused source** instead of just stating
  the count, and a loud, once-per-id `[media-playback]` warning fired when two
  different playback identities requested one source id — a real risk once media
  sources outlived buses (spec §5 risk called out up front). **Slice 3b:** the cap
  and its rate-limited `[media-decoder]` refusal moved into `MediaTransports`; the
  two-identities warning is gone because the collision is impossible by
  construction — one source id is one entry with one identity.

Tests: `native/tests/MediaCoreCommandTest.cpp` (media identity across buses),
`native/tests/MediaPlaybackTimelineTest.cpp` (the `OwnedMediaFrameSource.*` cases
were migrated onto `MediaTransports.*` by slice 3b and live in the same file),
`native/tests/ProgramPixelContinuityTest.cpp` (dark 0x10 fill placed outside the
placeholder colour range — fails against the old per-bus rename),
`MediaRoutePlaybackServiceTests` (incl. the per-asset pause rules) /
`TransportCoordinatorTests` / `MediaCoreCommandBuilderTests.SerializesTheRouteLoopFlagNextToPlaying`
(shell), `MediaCoreCommand.ARouteLoopFlagReachesTheMediaSourceOnBothBuses`,
`TakeRecord.AMetadataOnlyZoomFrameDoesNotCountAsHavingAFrame`, and
`scripts/qa/take-verdict-judge.test.mjs`. Not yet run: `scripts/qa/live-meeting-soak.mjs
--takes N [--background <file>]` against a real meeting. Its Takes mirror the
shell's Take (one sync: `load-scene-graph` incoming + `set-preview-scene` outgoing),
both scenes carry the same Tiles wall over the live Zoom members (plus the same
media background with `--background`), and Program is primed to scene B before the
floor is read so N takes score N records. **Remaining limits:** cuts only (no fade
Takes); no clip or still routes, so the go-live cold start above is not exercised;
without `--background` it proves Zoom/wall continuity only; no pixel probe across
the take (the record is the only judge); and the synthesized scenes are not the
shell's own scene payloads.
