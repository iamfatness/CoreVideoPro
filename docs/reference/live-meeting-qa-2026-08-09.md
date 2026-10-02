# Live-meeting QA day (2026-08-09) — eight defects found in ONE real session

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

An afternoon of the owner operating a real 7-guest meeting surfaced more product
truth than a month of synthetic drills. Each fix carries its full story as a
comment at the code site; this is the index.

- **Zoom video froze ~2s after join — SHM regions cannot GROW on Windows**
  (`engine-ipc.h`, `engine-video.cpp`, `engine-share.cpp`): regions were sized to
  the FIRST ramp frame (256x144); a named section cannot grow while the core
  holds a read handle, so the 640x360→1080p ramp failed silently forever (the
  failure log was gated on frame_count==0). Regions are now allocated ONCE at
  capacity (1080p video / 4K share ≈ 12.4MB, Zoom's ceiling) and both sides log
  loudly on shm failure. NEVER size a shared mapping to the current frame.
- **Leaving a meeting killed the entire studio** (`SettingsViewModel.LeaveZoomAsync`):
  the leave path kill-treed the media core (engine-distrust-era sledgehammer), and
  the supervisor treated it as deliberate → no respawn → endless deferred syncs
  ("unstable" until app restart; core log ends the second the leave runs). A
  meeting is one SOURCE. `_bridge.Stop()` is the app-exit path ONLY. Proof:
  `node scripts/validate-leave-keeps-core.mjs` (join → leave → still rendering →
  rejoin on the same core).
- **Recording restart storm — start-recording-session is IDEMPOTENT per sessionId**
  (`MediaCore::startRecordingSession`): the command rides the REPEATING sync
  channel, and every delivery restarted the writer → with Magic Scene flipping
  scenes ~1/s a live meeting produced 465 one-second shards. Same-id repeat = the
  channel re-asserting state = no-op. The sessionId's ISO suffix is also SORTED
  (`MediaCoreCommandBuilder`) so a roster flap reordering the same selection
  cannot mint a "new" session mid-recording.
- **Zoom ISO audio isolation is REAL — proven against live Zoom**: 7 stems from a
  real meeting; only the talker carried signal, six were digital silence, zero
  pairwise correlation. The per-guest-stems product story holds.
- **…which convicted the meters: they FABRICATED levels** (`AudioDsp.h
  analyzeAudioParticipantFrame`): frames with no PCM got a level synthesized from
  a HASH (pre-real-audio leftover, untested) — seven strips pulsing identically
  while six stems were silence on disk. Meters now show measured PCM or explicit
  producer levels only; no evidence = silence.
- **THE FADER LAW (owner rule): no audio source reaches any bus without a strip.**
  Core: a routed source with no channel strip is DROPPED from the bus mix, loudly
  (`MediaCore` routed-source build; headless callers that sync no console keep
  unity). Shell: `zoom-mix` — the audible Zoom path — was EXPLICITLY excluded from
  getting a strip (`IsConcreteAudioMixSourceId`), which is why muting every fader
  left audio on master. It has a "Zoom program mix" fader now.
  **Media clips are governed by the shell's `"media"` strip and sends via a CORE
  ALIAS (T1.6 / #455, `core/AudioControlSourcePolicy.h`).** Since #408 the decoder
  labels each clip's PCM `media:<assetId>`, but the shell has ONE "Media playback"
  row/strip/send set keyed `"media"`; exact-id matching made the FADER LAW drop every
  clip and no send reached a bus, so media audio was silent on master, stream and
  every recording while the shell looked fine. The routed-source build now
  PRE-SUMS every `media:*` clip without its own strip/send into ONE routed source
  keyed `"media"` (worker-owned scratch, no per-tick allocation), so the Media strip's
  gate/compressor/inserts/VST run ONCE on the combined signal (a VST insert must never
  be exchanged once per clip against one host instance), the `"media"` sends route it,
  and the strip meters/GR-meters the sum. Clips keep their own ids in the mixer
  session. An exact `media:<assetId>` strip or send keeps that clip separate; a clip
  with its own send rows does NOT inherit the generic row's other cells. Do NOT fix it by sending
  per-clip ids from the shell: the routing grid un-routes cells the core did not
  echo, so the Media row would switch itself off ~2 s later. Proof:
  `MediaCoreCommand.SceneMediaAudioReachesMasterThroughTheShellMediaStrip` and
  `node scripts/validate-record-audio.mjs --media` (real MF decoder, AAC 440 Hz clip,
  judges the recording's decoded audio). The FADER LAW line now says "unrouted
  source (no sends)" for a strip-less source nothing routes (perGuestIso's zoom-mix).
- **THE A1's MUTE IS ONLY SET BY THE A1 (#481).** A live meeting caught CoreVideo
  muting Courtney, Guy and CJ on its own while the console showed nobody muted: a
  new audio channel's `Muted` was seeded from the core's EFFECTIVE mute
  (`nativeChannel.Muted`, which folds in the Zoom mute), and then `prior?.Muted`
  latched that forever — a guest who was Zoom-muted the instant their channel
  first appeared stayed muted on every bus after they unmuted in Zoom. Fix: a new
  channel's `Muted` starts `false`, full stop, via the pure
  `StudioViewModel.ResolveMergedChannelMute(prior)`; the Zoom mute lives only in
  `SourceMuted` and is never adopted into `Muted`. It is still ORed into the
  EFFECTIVE mute sent to the core (`ResolveEffectiveAudioMute`) — harmless, since
  Zoom sends no audio while muted — but that gating is recomputed fresh every
  wire build, never latched into the A1's state. The core also now publishes
  PRE-MUTE `inputRmsDbfs`/`inputPeakDbfs` per channel (measured before mute/
  fader) so a muted, talking guest still shows on the meter (dimmed) instead of
  reading silence — the OUTPUT `rmsDbfs`/`peakDbfs` stay exactly as documented
  above. **Round 1 review correction: test the WHOLE merge, not just the leaf.**
  `ResolveMergedChannelMute(prior)` alone takes no native channel, so a
  regression that put `?? nativeChannel.Muted` back at the call site could not
  fail any test built only against that function. The real call site is now
  `StudioViewModel.MergeNativeAudioChannel(nativeChannel, prior, sourceMuted)` —
  the WHOLE native-channel→`ParticipantAudioMix` merge, extracted as one pure
  static — and the tests drive it through the live two-rebuild sequence (arrives
  Zoom-muted with no prior → live; guest unmutes in Zoom before the core's next
  wire echoes it → still live). Also fixed there: `SourceMuted` must never carry
  forward from `prior` on a roster miss (a missing roster entry is not evidence
  of a Zoom mute) — the caller resolves it fresh every rebuild and passes
  `false` on a miss, never `prior?.SourceMuted`. **Any new derived-state merge in
  this codebase should default to the pure-function-over-the-whole-decision
  shape, not a leaf function that omits the variable the regression would
  restore** — a test that cannot construct the regressed expression cannot catch
  it.
- **A throwing DispatcherQueue.TryEnqueue callback fail-fasts the process with NO
  managed log** (`UiDispatch.cs`): three live crashes decoded to ordinary NRE /
  ArgumentOutOfRange inside queued callbacks (stowed 0x80004003 / 0x8000000b at
  DeferInvokeCallback). ALL queued UI callbacks now route through `UiDispatch`
  (log-with-stack + survive). A raw `TryEnqueue` with a throwing body is a
  process-killer — never add one.
- **Sources kept reverting — it took THREE kills, one writer per report.**
  (1) auto-assign refilled operator-removed guests every sync
  (`ShowInputsCoordinator`): the fill pass now only places ids it has NEVER seen
  this meeting (real newcomers); flipping the auto-assign toggle explicitly
  reassigns everyone. (2) `EnsureAssignedSlotsForInShow` stuffed the first
  participant/first connected webcam into any in-show-but-unassigned slot every
  refresh — an unassigned slot now just leaves the show ("NEVER INVENT A
  SOURCE"). (3) the VESTIGIAL dual-capture selection
  (`StudioViewModel.ApplyDualCaptureSelection`) force-wrote the auto-picked
  primary/secondary capture devices (the local webcams) into ShowInputs[0]/[1]
  — slots 1-2 — on EVERY capture-fleet pass (device-watcher event, Inputs-tab
  visit, capture connect), with no UI bound to it at all, and the roster save
  then persisted the stomp; the slot write is deleted
  (`ShowInputAssignmentLawTests.TheDualCaptureSlotStufferStaysDead`). THE LAW:
  sources appear in slots by OPERATOR action or newcomer auto-assign ONLY.
  Enforcement: every `ShowInputSlot` setter logs `slot-write: slotN field
  old->new by=<reason>` with the ambient `ShowInputWriteScope` reason — an
  UNTRACKED slot-write in launch.log is a bug (wrap the writer in a scope). The
  roster also saves SYNCHRONOUSLY on every editor-observed change (the old save
  rode only the coalesced Low-priority refresh, so a crash lost the operator's
  pending change), and `LoadShowInputRoster` refuses a second load (persisted
  state restores ONLY at startup). Also: `DefaultMaxVideoSubscriptions` was 6, so
  the 7th+ camera-on guest was silently never subscribed — now 8 (the product's
  advertised feed count; the engine's downgrade ladder handles SDK refusals
  loudly). And `Selector.SelectedValue` must never be driven by x:Bind inside an
  ItemsRepeater template (`SourcesInputsPage` role ComboBox crash) — apply
  selection on Loaded, guarded.
- **Meters clipped when not fullscreen** (`AudioLevelMeter`): fixed-size segments
  (36×9px = 324px minimum) overflowed smaller windows, clipping the GREEN end.
  Segments now scale (spacing → size → count) and re-fit on resize.
- **Transport buttons had no `AutomationProperties.Name`** — screen readers and
  UIA (including our own tooling) could not find Record/Stream/VirtualCam. Named
  now; give every new interactive control an automation name.
