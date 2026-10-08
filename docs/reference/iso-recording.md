# ISO recording

## Windows recording queue ownership (#814, October 8, 2026)

When wrapped by `AsyncEncoderSink`, Media Foundation gives Program its own
`RecordingTrackWorker`, alongside the per-ISO workers. Dispatch does not call
steady-state Program `WriteSample`; a blocked Program file cannot stop feeding
ISO files. Each ISO has a four-picture dispatcher handoff and its own configured
file FIFO (default ten). A/V stays FIFO per file, stamped at capture/gather.
Program audio carries its source sample position through both queues, including
refused packets. Known position gaps become silence; callback jitter does not
retime subsequent audio. ISO audio keeps its existing capture-time gap filling.
Only that file's oldest queued video may be evicted at overflow. Stop closes the
file gates, drains accepted media, and finalizes before publishing completion.

`set-recording-targets.writeQueueDepth` is 4–30, defaults to ten, and applies on
the next Record start. WinUI persists it in the existing recording flyout.
The live two/three-frame Program buffer and audio delay remain independent.
Each real `recording.streams[].writeQueue` reports configured depth, backlog,
high-water, accepted items, completed **calls**, loss split from startup, byte
high-water, age and the last overflow reason. Actual muxed pictures are still
`framesWritten` reports source pictures; `muxVideoFrameCount` reports actual file
samples including A/V tail padding and is the decoded-file comparator. Completed
calls do not prove a committed sample. Missing queue
evidence is null. Support bundles retain these fields and summarize full queues.

Startup keeps the bounded burst allowance under per-file byte reservations; it
must drain into the steady limit. See [the queue spec](recording-write-queue-spec.md)
for the 512 MiB projection and the retained startup regression. AVFoundation
retains its existing dispatch policy until it has independent file workers;
this implementation and its real-file qualification are Windows-specific.

Headless fault injection uses `COREVIDEO_QA_RECORDING_STALL_SOURCE` (`program`
or an exact ISO source id), `COREVIDEO_QA_RECORDING_STALL_MS` (1–2000), and
`COREVIDEO_QA_RECORDING_STALL_AFTER_FRAMES` (30–36000). All must be valid.
It blocks only that file worker once, with begin/end evidence, and is disabled
by default. `validate-iso-record.mjs --sources 8 --stall-source program
--stall-ms 100 --seconds 20 --keep-artifacts --evidence <file>` retains
snapshots, decoded file counts and explicit Program loss judgments. A declared
`--allow-capacity-warning` permits only the known CPU-placement admission note;
it does not waive media loss, writer warnings or missing evidence.

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

## ISO recording — ISO-1 (per-source Zoom VIDEO ISO, 2026-07-20)

`docs/iso-record-spec.md` is the source of truth; ISO-1 ships the video slice for
Zoom participants (audio stems = ISO-2, capture sources = ISO-3, UI/pre-flight =
ISO-4). What landed:

- **The encoder boundary is widened, not rebuilt.** The MF sink already held
  `Mp4Writer program_` + N ISO writers on ONE shared `RecordingPtsClock`. ISO-1
  stops feeding ISO writers the composed program frame and instead carries each
  source's OWN video across `IEncoderSink::submitIsoVideo(vector<IsoSourceVideoFrame>)`
  (`Interfaces.h`). The frames are **zero-copy** — `VideoFrame` holds `shared_ptr`
  I420/BGRA payloads, so the whole hop (render gather under `coreMutex` →
  `latestIsoSourceFrames_` → `gatherAudioOutputWork` `work.isoSources` → async
  sink) copies refs, never pixels. Any I420→NV12 interleave happens on the
  **AsyncEncoderSink writer thread** (`i420ToNv12` in `MediaFoundationEncoderAdapter.cpp`),
  never under a lock or the audio worker. The convert law holds.
- **NV12 input path on `Mp4Writer`** (`VideoInput::Nv12`): Zoom I420 needs no CPU
  color-convert — the writer opens LAZILY at the source's FIRST frame, sized to
  that frame's native dims (no scaling), and picks NV12 input for `zoom:` sources
  / RGB32(BGRA) for capture (ISO-3). Program keeps its BGRA path untouched.
- **Per-`(sourceId,frameId)` PTS dedup** on the SAME epoch (`RecordingPtsClock::videoPtsForSource`):
  the audio worker re-submits every selected source's latest frame each tick, and
  each source advances on its own Zoom frameId, so a per-source last-frameId map
  (one shared epoch) muxes each real frame once. Proven headless: two ISO guests
  recorded **different** frame counts (402 vs 375 over 12s) — real per-source
  video, not the program proxy, and deduped well below the ~50/s resubmit rate.
- **Folder scheme + manifest (spec §5):** per-session subfolder
  `<prefix>-<yyyymmdd-hhmmss>/` with `Program.mp4` + `ISO-NN-<SafeName>.mp4`
  (roster/display name, sanitized, selection order) + `manifest.json`
  ({sessionId, epochMs, entries[{sourceId,name,path,kind}]}). `sanitizeForFilename`
  in the core mirrors `sanitizeIsoName` in `src/engine/isoRecording.ts` (the older
  planner was reconciled to this scheme — `ISO-NN-*.mp4`, no more `track-NN-*.mov`).
- **Command surface:** `isoParticipantIds` generalized → `isoSourceIds` accepting
  `zoom:<pid>` (capture ids arrive in ISO-3), with back-compat parse (a bare id =
  `zoom:<id>`) across all THREE mirrors in lockstep: `Protocol.h` (capability),
  `native-core/src/protocol.ts` (types), and the core parser
  (`MediaCore::readIsoSourceIds`/`normalizeIsoSourceId`). `src/engine/isoRecording.ts`
  reconciled. A "Program only ↔ Program + ISOs" switch is a payload flag; per-source
  selection is `isoSourceIds` (UI wiring is ISO-4).
- **Loud, never silent (spec §4/§7):** the silent `%TEMP%` fallback is KILLED for
  ISO — a bad/uncreatable target or session subfolder → `recording.warning` +
  ISO refused, program still records (priority-1). Per-ISO-writer open/write
  failures fold into `recording.warning` with the source name AND surface per
  stream in `recording.streams[]` ({sourceId, displayName, path, kind:"iso",
  framesWritten, warning, trackOpen}). A video-only-broken ISO is as loud as
  #286 made a video-only program. Each ISO writer finalizes independently on stop
  (its own moov, no 0-byte tails).
- **INVARIANTS honored:** lock order `coreMutex → audioOutputMutex_ → …`
  unchanged; ISO gather is under `coreMutex` (zero-copy refs), encode under the
  async sink; ISO frames drop-to-latest under disk pressure (video budget in
  `AsyncEncoderSink`), NEVER program A/V. **PROGRAM IS NEVER REGRESSED** — proven
  both ways: `EncoderRecordingSession.MediaFoundationIsoWritersProduceIndependentPlayableFiles`
  (program A+V green with 2 ISO writers present) and
  `validate-record-audio.mjs` (program A+V unchanged with ISO disabled).
- **Tests:** `EncoderRecordingSessionTest.cpp` (RecordingPtsClock per-source
  dedup + monotonic; real-MF N-writer open/reset #286 shape, NV12 playable,
  independent finalize, bad-folder-loud) + headless
  `node scripts/validate-iso-record.mjs` (fake engine, ISO on 2 → 2 ISO mp4s with
  h264 video, deduped). ISO-2 extends it into A+V + clap alignment.

## ISO video is ARRIVAL-DRIVEN, and its loss counters are SPLIT (2026-09-09)

Three linked corrections to the ISO video path. Read the Program video-tick section
above first — this is the same lesson, applied where it had not been carried across.

- **ISO submission is signalled by ISO FRAME ARRIVAL, never by Program cadence.**
  ISO used to be submitted inside `renderVideoOutputTick` gated on `programSubmitted`,
  which made a Program-paced ~60Hz sampler read the render thread's independently
  published ~60Hz ISO set. Two free-running 60Hz clocks beat: 15-22% of submissions
  were rejected as duplicate `(sourceId, frameId)` by `AsyncEncoderSink`, and the
  result was **NON-MONOTONIC** — a faster source wrote FEWER stem frames. The render
  gather now APPENDS each newly-seen `(sourceId, frameId)` to an accumulating queue
  (`pendingIsoVideoQueue_`, per-source dedup, per-source pending cap
  `kMaxPendingIsoFramesPerSource = 4`) and bumps `isoVideoPublishSeq_`;
  `MediaCore::renderIsoVideoTick` — its own `isoVideoThread` in `JsonRpcServer` —
  waits on that signal and DRAINS EVERYTHING pending. **It does not sample, it
  drains**: a late tick costs latency, never frames. Measured with the fake engine
  (`validate-iso-record.mjs --source-fps N`, 20s, 2 ISO sources): 30 -> 29.8fps both
  before and after; 60 -> **58.1-58.8 before, 59.3-59.5 after**; 120 -> **56.8-58.3
  before, 59.4-59.5 after**. Rules it keeps: `isoVideoQueueMutex_` is a LEAF (taken
  under `coreMutex` for shared_ptr ref copies only — no pixel work, no I/O) and never
  reaches back for `coreMutex`/`audioOutputMutex_`; the worker touches neither; and
  the async sink's writer already gives Program items weighted priority over ISO, so
  an ISO burst cannot displace Program work. **ISO frames are stamped at GATHER, not
  at submit** — stamping at submit collapses a whole drain onto one instant, which
  `RecordingPtsClock` then de-collides into a 100ns clump.
  Remaining cap, honestly: the render gather still samples each source
  latest-per-tick, so a source above the render rate is capped at ~60 distinct ISO
  frames/s (monotonic, but not 1:1). Making that lossless means changing
  `ZoomEngineRuntime`'s per-participant latest-frame slot, not this path.
- **A one-line change with teeth: the sink's per-source ISO coalesce now fires ONLY
  at the cap.** It used to erase a source's older pending item unconditionally, which
  is a silent fidelity ceiling the moment a producer legitimately hands the sink two
  distinct frames for one source in quick succession — exactly what an arrival-driven
  drain does when it catches up. Its stated purpose (stop a fast participant evicting
  every slower guest when the GLOBAL cap bites) is preserved by gating it on that cap.
- **Video startup drops are counted apart from steady-state loss** — the concept audio
  has had since `recordingStartupDroppedAudioPackets`. The recording writer's Media
  Foundation open is SYNCHRONOUS and applies as a FIFO item on the writer thread
  (95-250ms), while the producer keeps submitting at 60Hz because `recording.status`
  already reads "recording". 7-12 frames are shed there. **No frame is missing from
  the file** — the head of the show is clipped — but they landed in the same
  `droppedVideo` the Wave 0 judge is fail-closed on, so a clean run reported `failed`.
  `startupDroppedVideo` (evidence) / `recordingStartupDroppedVideoFrames` (recording
  proof) now carry them, and **the window ends at the writer's first committed video
  frame (or failure), NOT when Start was applied** — the first WriteSample calls into
  a freshly opened MF sink are slow too, and closing the window at Start left ~7 of 13
  drops still poisoning the steady-state counter (measured: judge still `failed`;
  after: `droppedVideo` flat 0 for the whole run, judge clean). NOTHING IS HIDDEN —
  `runtime-snapshot-qualification.mjs` tracks it as a non-loss counter plus an
  observation, `validate-recording-finalization.mjs` reports it, and no threshold in
  either judge was weakened. Known remaining: each ISO writer performs its OWN lazy
  synchronous open at its first frame, and the items shed there still land in
  `droppedVideo` (bounded, one-time, before the first sample, so the delta-based judge
  does not trip on it).
- **ISO fidelity is measurable now.** `framesWritten` on an ISO stream is an APPEND
  count and cannot tell a distinct picture from a repeat — which made any change to the
  ISO cadence unverifiable. `encoderEvidence.isoVideoBySource` carries the whole chain
  per source: `queued` / `heldFrameSuppressed` / `queueOverflowed` (arrival side, from
  the render gather's queue) and `submitted` / `duplicateRejected` / `dropped` /
  `written` (sink side). Live at 60fps after the fix: `queued == submitted` exactly,
  `heldFrameSuppressed = 1`, `duplicateRejected = 0` — i.e. the sink-side dedup that
  was rejecting 15-22% of submissions now rejects nothing, because the repeats are
  suppressed where they are actually observed.

## The ISO dispatch thread is not the encode thread — and the throttle belongs on the READ side (#529, 2026-09-18)

`AsyncEncoderSink` runs ONE writer thread. `submitIsoVideo` splits a batch to one
source per queue item, so eight ISOs plus Program at 60 fps is **~540 items/s
through that single thread**. The encode is NOT there — every ISO file has its
own `RecordingTrackWorker`, so they encode in parallel — which makes that thread a
**dispatcher**, and anything it does per item is multiplied by 540.

Two costs rode that path. `MediaFoundationEncoderSink::submitIsoVideo`/
`submitIsoAudio` ended with an unconditional `refreshIsoStreams()`, which takes
each writer's snapshot mutex AND its worker's evidence mutex (16 acquisitions at
eight ISOs) and rebuilds `session_.isoStreams` — two string-bearing structs per
writer. And the writer loop called `inner->session()` after EVERY item, which for
that sink runs the same rebuild again and then copies the whole `OutputSession`
by value. **And it fed back on itself:** once a writer has dropped anything its
status warning is non-empty, so every later pass recomposes `"ISO recording lost
N video frames…"` with `std::to_string` — the cost grew with the damage, which is
why a live 1080p60 eight-ISO run reported 8,482 dropped video items over ~100 s
and never recovered inside the session (it kept dropping with no Takes at all,
which is what ruled Takes out as the trigger).

**THE FIRST FIX WAS WRONG AND THE TESTS SAID SO — IN A CONFIG CI CANNOT BUILD.**
It throttled `refreshIsoStreams()` inside the MF sink's SUBMIT path. That broke
**seven `EncoderRecordingSession.*` tests**, which submit a handful of ISO frames
and then read `encoder->session()` synchronously, asserting exact cumulative
state (`videoFrameCount == 9`, `audioSampleCount > 0`, `isoStreams.size() == 2`).
They were right to: **that sink's contract is that a caller which submits and
then reads sees exact, current counts.** They are Windows-only — they do not
compile on the stub build — so CI was green through the whole thing and only a
real `COREVIDEO_WITH_MF_ENCODER=ON` build found it. **A green CI on this repo
says nothing about the Media Foundation sink.** `ci.yml` DOES have a Windows
runner (`native-shell-windows`), and an earlier note here claiming otherwise was
wrong — but that job builds the .NET shell and runs the native STUB gate
(`scripts/test-native.ps1`), never a `COREVIDEO_WITH_MF_ENCODER=ON` core. No CI
job on any platform compiles `MediaFoundationEncoderAdapter.cpp`.

The layer that is ALLOWED to lag is `AsyncEncoderSink::session()`, and its own
header has said so all along: *"eventually consistent within a few frames — fine
for the live app; unit tests that need exact synchronous counts use the wrapped
sink directly"* — which is exactly what those seven tests do. So:

- **The MF sink refreshes ON READ.** `refreshIsoStreams()`/`updateBytesWritten()`
  moved out of the submit path and into `session()` (via a narrow `const_cast`:
  the object is never actually const, and the async writer thread is its SOLE
  owner, so submit and read are the same thread — no lock, no race). Submit-then-
  read is exact again, and nobody pays for a status nobody read.
- **The async wrapper reads LESS OFTEN, and asks a cheaper question per item.**
  `IEncoderSink::progress()` (`EncoderProgress`, `Interfaces.h`) answers the only
  per-item question — did the writer move, did it fail — with a **default
  implementation deriving from `session()`**, so every existing sink stays correct
  with no edit; the MF sink overrides it to read counters it already maintains.
  The full `session()` is read on the policy in
  `modules/EncoderSessionReadPolicy.h` (`EncoderSessionReadGate`).

**The read rule is BURST-shaped, not a fixed rate, and that is the point.**
`MediaCore::renderIsoVideoTick` DRAINS everything pending, so one 60 Hz tick
enqueues ~9 items at once. Reading once per **drained burst** is ~9x fewer reads
at eight ISOs — and the saving GROWS with the ISO count, i.e. with the load that
caused the incident. It costs no observable freshness: the published snapshot is
exact whenever the writer is idle, which is the only moment a settled reading
exists (`drainForTest` waits for precisely that). Structural items
(configure/start/stop) and any failure always read. `kStaleAfterMs` (100 ms) is a
BACKSTOP so a queue that never drains cannot freeze the snapshot — a bound, not
the mechanism. **Rule: status is diagnostic, media is not — and when you throttle
a status, throttle the READER, never make the producer answer with stale numbers.**

**A READ-PATH REBUILD MUST NOT OUTLIVE THE WRITERS IT READS.** Moving the
rebuild into `session()` introduced a second Windows-only defect, caught on a
real `COREVIDEO_STUB=OFF` core by `MediaFoundationIsoVariableRateKeepsElapsed
TimelineAcrossFragments` and `MediaFoundationIndependentIsoWritersDrainAudio
VideoBeforeFinalize` (both read `session()` AFTER `stopRecording()`).
`closeWriters()` does its OWN final refresh and then clears `isoWriters_`, so an
unconditional rebuild on a later read regenerated `isoStreams` from zero writers
and wiped the finalized status it had just captured — empty `isoStreams`,
`videoFrameCount` gone, on a recording that had completed correctly. `session()`
now rebuilds only `if (!isoWriters_.empty())`; once stopped, the finalized
snapshot IS the answer. **The guard is at the CALL SITE, never inside
`refreshIsoStreams()`** — the rebuild at the top of a take must stay
unconditional, because that clear is what stops a previous ISO recording's
streams carrying into a new program-only one. Two rounds of this fix were caught
by the same seven tests, in a configuration no CI job builds: treat them as the
gate for anything touching this path, and expect a read-side change to have a
stop-side consequence.

**A drop is charged to the source that LOST the picture, not the one arriving.**
Same PR, `AsyncEncoderSink.cpp`: when the ISO budget is full and the arriving
source has no pending frame of its own to replace, the sink evicts the OLDEST
queued ISO picture — another source's — and used to charge the arrival. A slow
guest was billed for frames a fast guest lost, and that per-source spread
(`encoderEvidence.isoVideoBySource`) is exactly what gets read to decide which
writer is unhealthy. The victim is now identified BEFORE the counters move; with
no victim the arriving item really is the one refused, so it keeps the charge.
Pinned by `AnEvictedIsoPictureIsChargedToItsOwnSourceNotTheArrivingOne` (mutation
-proved) and `ARefusedIsoPictureIsStillChargedToTheArrivingSource` (the guard
against over-correcting).

**Telling the two ISO loss sites apart, because one number sums both.**
`session.encoderQueueDroppedVideoFrames` adds the sink's queue drops to the
per-writer worker drops, so it cannot diagnose anything on its own. Read them
separately: `encoderEvidence.isoVideoBySource.<id>.dropped` is the DISPATCHER
behind; `recording.streams[].droppedFrames` is that WRITER's encode too slow, and
`videoWorkUs ÷ completedVideo` (both cumulative) is its mean µs per frame — over
16,667 means that track cannot hold 60 fps, and `encoderPath` will usually read
`software`.

Tests: `EncoderSessionReadPolicyTest.cpp` (the rules, incl. the burst-rate
collapse), `AsyncEncoderSink.TheWrappedSinksFullStatusIsNotReadOncePerItem`
(mutation-proved: disabling the gate fails it) and
`TheSnapshotIsExactOnceTheBurstHasDrained`. The seven
`EncoderRecordingSession.*` tests are the Windows gate and must be run on a real
`COREVIDEO_STUB=OFF` build before believing any change to this path.

## ISO recording — ISO-2 (per-source AUDIO stems muxed into the ISO MP4s, 2026-07-20)

ISO-2 completes the **Demo E** shape: each Zoom-participant ISO is now a
self-contained **A+V** MP4 (its own video from ISO-1 **and** its own raw-stem
audio), time-aligned to program. Stacked on ISO-1 (`submitIsoVideo` boundary,
per-source `Mp4Writer` map, folder scheme). What landed:

- **Raw-stem tap = PRE-DSP, PRE-MIX** (owner decision-3). The stem is
  `work.audioFrames[i].pcm` — each source's isolated PCM, resampled to the 48k bus
  rate at gather but tapped BEFORE the channel-strip DSP and the bus mix. Proof
  it's pre-DSP: `RoutedAudioSource.pcm` is a `const` pointer into these buffers and
  `mixRoutedBuses` runs the gate/EQ/comp/inserts on COPIES — the source buffers are
  never mutated (`MediaCore.cpp` runAudioOutputWork, just after the program
  `submitAudio`). Do NOT move the tap after `mixRoutedBuses`; that would be the
  post-DSP signal (the option the owner explicitly did NOT choose).
- **`IEncoderSink::submitIsoAudio(vector<IsoSourceAudio>)`** (`Interfaces.h`), a
  separate boundary paired with `submitIsoVideo`. Submitted **every tick for EVERY
  selected source**: a source with PCM this tick muxes it; a source Zoom gated
  silent this tick rides an **empty** entry (frameCount==0). Rides its own
  `AsyncEncoderSink` `Kind::IsoAudio` with the audio budget but SEPARATE
  drop-to-latest accounting, so a slow disk drops ISO audio to silence-filled gaps
  and can NEVER evict a program-audio packet (program is priority-1, spec §9).
- **Silence-fill (spec §2c), the correctness core.** `RecordingPtsClock::isoAudioAdvance`
  anchors every stem to the ONE shared epoch (t=0 == program start): the expected
  sample position at wall time `now` is `(now-epoch)` worth of samples, so a buffer
  emits exactly enough leading silence to reach that position, then the real
  samples. A guest silent for K ticks (empty submits) advances by silence alone and
  lands the next real burst at the correct, program-aligned position — never a
  drift EARLIER of program. A dropped ISO-audio tick simply becomes silence in the
  stem (the next tick's wall-anchored fill covers it), timeline intact. The sink
  chunks long leading silence (`Mp4Writer::writeAudioSilence`, 0.1s blocks) so a
  guest who talks minutes in never emits one giant sample.
- **#286 up-front audio stream, per ISO writer.** The ISO writer opens LAZILY at
  its first video frame; the AAC stream is added THERE — `open()` →
  `ensureAudioStream(2, 48000, …)` → `beginWriting()` — never after BeginWriting
  (0xC00D36B2). `Mp4Writer::open()` already resets `audioConfigured_`, so a REUSED
  ISO writer across the double `start()` re-adds its stream cleanly (regression
  test proves a reused ISO writer keeps its audio track). ISO AAC is uniformly 48k
  **stereo**; mono Zoom `isolate_audio` stems are up-mixed L=R in `submitIsoAudio`.
- **Snapshot + manifest:** `recording.streams[]` ISO nodes now carry
  `audioSamples` (silence+real) and `hasAudio` (= `audioSamples > 0`);
  `manifest.json` marks every entry `"hasAudio": true`. A track-less ISO where
  audio was expected folds into `recording.warning` (as loud as #286 made a
  video-only program).
- **Tests:** `RecordingPtsClock.IsoAudioSilenceFillKeepsGappedStemAligned` (the key
  gapped-stem test — silent K ticks then resume lands at the right sample
  position) + `IsoAudioLateStartSilenceFillsFromEpoch`; real-MF
  `EncoderRecordingSession.MediaFoundationIsoWritersMuxOwnAudioStems` (2 ISO writers
  with DIFFERENT audio, #286 reused-writer audio-track reset, **program A+V not
  regressed with ISO audio enabled**); and `scripts/validate-iso-record.mjs`
  extended to the **Demo E leg** — ffprobe each ISO has h264 video AND aac audio,
  head-clap alignment (ISO audio start vs program audio start on the shared epoch)
  measured **0.0 ms** (budget 50 ms). Fake tone engine gives distinct
  per-participant sines (220Hz + pid%8·110), so the two ISO stems carry different
  content (956685 vs 969374 samples over 20s), not the program mix.

## ISO recording — ISO-3 (UVC/capture sources, 2026-07-21)

ISO-3 broadens ISO to **capture-class** sources (`capture:<id>` — UVC cameras,
screen/window capture, browser sources). Most of the machinery was already
capture-generic in ISO-1/2 — the delta is small and surgical:

- **Capture VIDEO rides ISO-1's BGRA writer path, no new code.** Capture frames
  merge into `videoFrames` keyed `capture:<id>` (`capture:browser:<n>` for
  browser) at the render gather, and ISO-1's `latestIsoSourceFrames_` snapshot
  already keys ANY `<scheme>:<id>` frame and skips only `media:`. So a capture
  frame flows to `submitIsoVideo`, which already branches `frame.hasI420() ?
  NV12(Zoom) : RGB32(BGRA)` — capture is BGRA, so it takes the RGB32 path (spec
  §2b, "the writer picks input type per source at open"). Per-`(sourceId,frameId)`
  dedup is scheme-agnostic; all three capture paths (WinUI bridge / native UVC /
  browser host) carry advancing `frameId`, so it holds.
- **Capture AUDIO pairing — THE decision (owner rule confirmed against the
  codebase).** A capture VIDEO source and its audio can be SEPARATE devices. The
  codebase pairs them via `sync-capture-audio-sources`: a `CaptureAudioSourceInput`
  has a `captureDeviceId` (the VIDEO device) + an optional `audioDeviceId`, and
  `WasapiAudioCaptureSourceAdapter::participantIdForSource` keys the PCM
  `capture:<captureDeviceId>` — the SAME id as the video. So paired capture audio
  muxes into the same ISO writer AUTOMATICALLY (ISO-2's `work.audioFrames` tap,
  same sourceId match). **Rule: a capture ISO carries audio IFF the operator paired
  an audio input to that capture device (Elgato-class embedded audio / a mic
  assigned to the camera). A pure camera (no paired audio) → VIDEO-ONLY ISO — no
  all-silence AAC track, no fabricated stem.** Implemented via
  `IsoSourceSelection.hasAudio` (`MediaCore::isoSourceHasAudio`: zoom→always,
  capture→matched real pairing in `captureAudioSources_`, browser→false); the ISO
  writer skips `ensureAudioStream` at lazy-open when `hasAudio==false`, so
  `submitIsoAudio` naturally skips it (`audioConfigured()` stays false). Snapshot
  `hasAudio`/`audioSamples` and `manifest.json` reflect the per-source decision.
- **Display names:** `resolveIsoDisplayName` resolves `capture:<id>` to the
  enumerated device name (`CaptureDeviceInfo.name`, match by id/`nativeDeviceId`),
  a browser source's URL, or the paired audio device name — so post sees
  `ISO-NN-<CameraName>.mp4`, falling back to the id tail (loud, never fabricated).
- **Command/snapshot parity (3 mirrors):** `isoSourceIds` already accepted
  `capture:<id>` (ISO-1 generalized `normalizeIsoSourceId`); the snapshot now also
  emits the canonical `isoSourceIds` list alongside `isoParticipantIds`
  (`canonicalIsoSourceIds`); `src/engine/isoRecording.ts` planner gains a
  `capture` `IsoTrackSource` (+`captureSources` option, `capture:<id>` track ids,
  participant-tier bitrate). `Protocol.h` (`iso-recording` capability + the
  scheme-qualified reader) needed no change.
- **Capture-stall interaction (CaptureReaderStallPolicy):** a stalled capture
  source either holds its last frame (same `frameId` → dedup muxes once, no churn)
  or stops appearing in `videoFrames` (its writer simply stops advancing and
  finalizes gracefully at stop) — never a churn/spam loop on the ISO writer. Loud
  in `recording.warning` only on a real writer failure.
- **Tests:** `MediaFoundationCaptureBgraIsoMixedWithZoomNv12` (capture BGRA +
  zoom NV12 in ONE session, both playable, **program A+V green with capture ISO**,
  paired capture audio muxed), `MediaFoundationVideoOnlyCaptureIsoHasNoAudioTrack`
  (a pure camera → `audioSampleCount==0`, no all-silence track),
  `MediaCoreResolvesCaptureIsoDisplayNamesAndAudioPairing` (display name from
  enumerate + the paired/unpaired hasAudio decision),
  `RecordingPtsClock.IsoVideoDedupsCaptureSourceIndependentlyOfZoom`; TS planner
  tests for the `capture` source. **Harness gap (honest):** the fake zoom engine
  is Zoom-only, so capture ISO has no headless E2E — it is covered by the real-MF
  unit tests + synthetic capture frames above, and is **rig-verified only** for a
  live camera. `validate-iso-record.mjs` (Zoom) still PASSES (2 ISO A+V streams,
  clap 0.0 ms) — proof ISO-1/2 is not regressed.

## ISO recording — ISO-4 (disk pre-flight + support-bundle health + Show-mode UI, 2026-07-21)

ISO-4 is the operator-facing polish; it adds NO new media protocol beyond the
`isoSourceIds` selection ISO-1/2/3 already defined, and — critically — **program
recording is never regressed**: the new "Program + ISOs" switch DEFAULTS OFF, so a
fresh install records program-only exactly like the pre-ISO product (no ISO writers
arm). Four pieces:

- **Disk pre-flight (spec §6) is SHELL-SIDE by design.** `IsoDiskPreflight.Evaluate`
  (`CoreVideoPro.MediaCore/Services/IsoDiskPreflight.cs`, pure/unit-tested) ports the
  TS `isoRecording.ts`/`diskSpace.ts` math: combined rate = program bitrate + N×6.192
  Mbps (1080p video + one raw-stem AAC), vs free bytes on the target volume
  (`DriveInfo`). Runs at the top of `StudioViewModel.ToggleRecordingAsync` BEFORE
  arming: **Insufficient** (< 5 min headroom) hard-blocks the start with a loud
  `OutputStatus`; **Low** (< 30 min planning window) sets a persistent
  `RecordingDiskWarning` (surfaced in the record flyout, survives the "start
  requested" status) but proceeds; an unmeasurable volume never blocks (warn-not-
  silent). No core/protocol/snapshot field — the shell already owns the folder,
  program bitrate, and ISO selection, so core-side would need a needless 3-mirror
  protocol change.
- **Support-bundle ISO health (spec §6, DoD).** `NativeMediaCoreRecordingStream` (wire)
  + `SupportBundleMediaCoreRecordingStream` (model) gained `SourceId/DisplayName/Path/
  AudioSamples/HasAudio` (camelCase deserialize auto-populates the ISO-1/2/3 snapshot
  fields that were previously dropped). `SupportBundleBuilder` maps them + adds an "ISO
  recordings: N stream(s)…" triage block listing each ISO's path + encode health.
  Paths are NOT secrets and are emitted verbatim; redaction stays green (no new field
  carries a key/token) — `SupportBundleBuilderTests.Build_ListsIsoStreamPathsAndEncodeHealth`.
- **Show-mode UI (spec §7, N1).** A transport-level **"Program only" ↔ "Program +
  ISOs"** ToggleSwitch in the record-output flyout (`StudioWorkspace.xaml`) bound to
  `IsoRecordingEnabled`; a per-source **"ISO" checkbox** on each eligible row in
  Sources → Inputs (`SourcesInputsPage.xaml`, `ShowInputSlotViewModel.IsoEnabled`/
  `ShowIsoToggle` — Zoom guests + capture devices only, media excluded); and an **ISO
  health readout** ("Program + N ISOs" + first per-stream warning) that reuses the
  recording-warning surface. **0xc000027b-safe:** the toggle rides the EXISTING
  signature-gated `ShowInputEditors` collection (never a new snapshot-rate bound
  collection; re-projected in place via `ApplyIsoSelectionToEditors` under the same
  id-set signature as `RefreshShowInputEditors`); ISO health strings are scalar props
  notified per snapshot apply (the `WorkspaceCompGrLevel` pattern), UI mutations via
  `RunOnUiThread`.
- **The pure selection logic is EXTRACTED and tested.** `IsoSourceSelectionResolver`
  (MediaCore) turns (enabled, selected set, eligible-present roster) → ordered
  `isoSourceIds` (OFF → empty; drops departed sources; deduped; capped at 8) — so the
  logic trapped in `StudioViewModel` (`BuildIsoSourceTargets`) is unit-tested without
  the VM. The command builder now emits canonical `isoSourceIds` (`zoom:<pid>`/
  `capture:<id>`) on all three recording payloads (the core prefers it over legacy
  `isoParticipantIds`; `SyntheticMediaCore` mirrors the preference).
- **Persistence: prefs schema v8.** `ProductionOutputPreferences.IsoRecordingEnabled` +
  `IsoRecordingSourceIds` persist the switch + selection; restore rides the O1/vcam
  BACKING-FIELD pattern (a setter would sync a core that isn't up), re-projected onto
  the editors on first `RefreshShowInputEditors`. v7→v8 migrates to program-only
  defaults. (v7 was the true current version — the "v6" in the B2 notes was stale; v7
  added VstInsertStates.) v9 (2026-08-10) persists the Zoom→program audio topology
  (ZoomAudioMode: "programMix"/"perGuestIso"); absent = programMix, and an
  unrecognized value falls back to programMix rather than guessing ISO.
