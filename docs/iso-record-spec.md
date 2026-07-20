# ISO Record (video + audio) — Design Spec

_Status: designed 2026-07-20 with the owner. Implements the **highest-leverage spine
feature** in `docs/FOCUS_PLAN.md` §4/§8 (Phase 2 item #1) to its Definition of Done, and
gates **Demo E** (§6). Grounded in a same-day architecture map of the recording pipeline —
every seam below is a real file:line, not an invention._

## 0. What ISO means here (owner decisions 2026-07-20)

**ISO = per-source recordings an NLE/DAW can open, time-aligned to the program.** Not "ISO
taps exist in the mixer" — **files on disk with audio.**

Three decisions, locked:

1. **Self-contained muxed A+V per source.** Each ISO is one MP4 carrying that source's
   own video **and** that source's own audio (`ISO-01-<Name>.mp4`). Drops straight onto an
   editor timeline. (Not video-only, not separate audio stems.)
2. **Sources in v1: Zoom participants + host UVC/capture.** Not media playout (you already
   have those files). Screen/window capture is capture-class, so it rides the same path.
3. **Raw ISO stems for audio.** Each source's *isolated* audio — Zoom per-participant
   `isolate_audio`, or the capture device's own PCM — **before** the channel-strip DSP and
   the bus mix. True stems for post.

## 1. The load-bearing reality (why this is mostly plumbing, not new machinery)

The MF encoder already holds `Mp4Writer program_` + `std::vector<Mp4Writer> isoWriters_`
(`MediaFoundationEncoderAdapter.cpp:753-754`), opens one `IMFSinkWriter` per participant
(`:643-671`), and shares ONE `RecordingPtsClock recordingClock_` (`:758`) across all
writers so every track sits on the program's epoch. **N concurrent MP4 writers already
work today.**

What is wrong today: the ISO writers are fed the **composed program frame** and the
**program mix**, not per-source signals — the code says so at
`MediaFoundationEncoderAdapter.cpp:529-534` ("the IEncoderSink boundary does not (yet)
carry per-source pixel buffers, so the composed program frame is the per-participant
proxy") and `:595-597` (same program audio to every ISO writer). ISO record is therefore:

> **widen the encoder boundary to carry distinct per-source video + audio, feed each ISO
> writer its own source, and keep them on the shared epoch.**

Both signals already exist CPU-side, so no new GPU readback is needed:
- **Per-source video:** Zoom I420 planes per `participantId` in
  `ZoomEngineRuntime::latestDecodedVideoFrames()` (`ZoomEngineRuntime.cpp:425-447`), plus
  `capture:<id>` frames — both gathered on the render thread at
  `MediaCore.cpp:4138-4175`. Today they only reach the compositor.
- **Per-source audio:** pre-mix per-`sourceId` PCM already sits in `work.audioFrames`
  (`MediaCore.cpp:4427`) and `RoutedAudioSource` (`:4563-4569`); Zoom ISO stems arrive
  already separated (`ZoomEngineRuntime.cpp:1031`, keyed by participant id).

## 2. Architecture

```
 RENDER/GATHER THREAD (coreMutex)          AUDIO WORKER (audioOutputMutex_)         ASYNC ENCODER (own thread)
 ─────────────────────────────────         ────────────────────────────────        ──────────────────────────
 gather selected ISO sources:              per tick, build IsoEncodeWork:            AsyncEncoderSink FIFO
  • zoom I420 planes (shared_ptr, 0-copy)   • program frame (as today)                (drop-to-latest, bounded)
  • capture BGRA frames (shared_ptr)        • per-source video refs (0-copy)            │
  • snapshot into work.isoSources           • per-source PCM (from work.audioFrames)     ▼
        │                                         │                                  MediaFoundationEncoderSink
        └───────── work item ─────────────────────┘                                   • program_  Mp4Writer
                                                                                       • isoWriters_[i] Mp4Writer  ← real per-source A+V
                                                                                       • ONE recordingClock_ (shared epoch)
```

- **Zero-copy across threads.** The gather snapshots `shared_ptr` refs to the existing
  I420/BGRA source buffers into the work item — no pixel copy under `coreMutex` (obeys the
  "no pixel work under shared locks" LAW). The async encoder holds the refs until it has
  encoded the sample, then drops them.
- **All ISO writers go through the existing `AsyncEncoderSink`** (`Interfaces.h:732-737`,
  `AsyncEncoderSink.h:19-56`) so N writers' disk I/O can never wedge the 4 ms audio
  deadline. Drop-to-latest per source under back-pressure (a slow disk drops ISO frames,
  never program, never audio — logged as ISO health, not a crash).

### 2a. Encoder boundary widening (`IEncoderSink`)

`ProgramFrame`/submit path stays for program. Add an ISO submit carrying, per selected
source: `sourceId`, display name, a video sample (see 2b), and that source's interleaved
PCM for this tick. `MediaFoundationEncoderSink` maps `sourceId → isoWriters_[i]`; unknown
source opens a writer, dropped source finalizes its writer (see §4 stop).

### 2b. Video sample format — avoid N CPU converts

Program `Mp4Writer::writeVideo` takes BGRA→RGB32 (`MediaFoundationEncoderAdapter.cpp:232`).
For ISO, converting N sources' frames to BGRA on the CPU per tick is the wrong cost. Zoom
ISO frames are already **I420** on the CPU; MF encodes NV12/I420 natively. **Add an
I420/NV12 input path to `Mp4Writer`** (`MFVideoFormat_NV12` input media type; a cheap
I420→NV12 plane interleave, or feed I420 directly where the encoder MFT accepts it) so
zoom ISO needs no color convert. Capture frames arrive BGRA — keep the BGRA path for
those. The writer picks its input type per source at open.

### 2c. Per-source PTS + the gap problem (stems are choppy by design)

- Reuse the one `recordingClock_` so every ISO shares the program epoch — a clap on
  program lands at the same timeline position on every ISO (Demo E head-skew < 50 ms is
  inherited, not re-earned).
- **Video dedup is program-scoped today** (`RecordingPtsClock.h:36-51` dedups by program
  `frameNumber`). Per-source needs a per-source dedup key — Zoom frames carry their own
  `frameId`/sequence, capture frames carry `frameId`. Extend the clock to dedup video per
  `(sourceId, frameId)` while keeping ONE epoch.
- **Silence-fill gapped audio stems.** Zoom gates non-active speakers server-side
  (`zoom-audio-spec.md:31-33`), so a guest's `isolate_audio` stops between talk bursts.
  For a time-aligned stem, when a source delivers no PCM this tick the ISO audio path
  writes **silence to the source's next expected sample position** (epoch-anchored
  accumulate, same as program audio) so the stem stays in sync with program and never
  drifts. Documented as expected behavior, not a bug.

## 3. Commands + snapshot (three protocol mirrors, lockstep)

The TS layer already scaffolds ISO (`native-core/src/protocol.ts:243-291`,
`MediaCoreRecordingStream.kind:"iso"` + `participantId`, `isoParticipantIds` on targets) —
extend, don't invent. Parity edits:

- **`native/src/core/Protocol.h`** — capability `"iso-recording"` already declared
  (`:20,:39`). Recording commands already carry `isoParticipantIds`
  (`start-program-output`/`set-recording-targets`). Add ISO **source selection** that
  includes capture ids (not just participant ids) — generalize `isoParticipantIds` →
  `isoSourceIds` (accept `zoom:<pid>` and `capture:<id>`), keeping back-compat parse.
- **`native-core/src/protocol.ts`** — widen `isoParticipantIds` → `isoSourceIds`; extend
  `MediaCoreRecordingStream` with `sourceId`, `displayName`, `path`, and per-stream
  `frames`/`audioSamples`/`warning`.
- **`src/engine/isoRecording.ts`** — an ISO **planner** already exists here (tracks,
  bitrate/disk estimate, folder layout — writes `.mov`, unwired). Reconcile its folder
  scheme with §5 and reuse its disk-estimate math server-side (§6); do not leave two
  divergent schemes.
- **Snapshot** (`MediaCore.cpp:3755-3814`): the `recording.streams[]` array gains one node
  per ISO with `sourceId/displayName/path/frames/audioSamples/warning`;
  `recording.warning` (the #286 loud-fail channel) also fires if ANY ISO writer fails to
  open or loses its track — a video-only ISO must be as loud as a video-only program was.

## 4. Stop / finalize / failure (the honesty rules)

- **Every ISO writer finalizes independently and cleanly** on `stop-recording-session` —
  each `Mp4Writer::close()` writes its own moov; no 0-byte / unplayable tails (the DoD
  bullet). A source that dropped mid-show still finalizes the file it has.
- **#286 double-start lesson applies per writer.** `encoder->start()` runs twice per
  recording (arm at `start-program-output` `:1530`, restart at `start-recording-session`
  `:1633`). Every ISO writer's per-session state MUST reset in `Mp4Writer::open()`
  (`:81-97`) — a stale `audioConfigured_`-class flag on a reused ISO writer silently
  produces a track-less ISO. Add each ISO's audio stream up-front, never after
  `BeginWriting` (`:656-667`).
- **Kill the silent temp-dir fallback.** `resolveTargetDir()` today silently redirects to
  `%TEMP%` when the folder is bad (`:698-710`). For ISO (many files, easy to lose) this
  becomes a **loud** failure: bad/uncreatable ISO folder → `recording.warning` + refuse to
  start ISO (program may still record), never a silent temp write.
- **Disk-full = loud.** A writer's WriteSample failure already surfaces
  (`:342-354,586-594`); ensure ISO writer failures reach `recording.warning` with the
  source name, and stop-drain finalizes what exists.

## 5. Folder layout (pick one, stick to it — DoD)

Per recording session, one subfolder so a show's artifacts stay together:

```
<RecordingFolder>/<prefix>-<yyyymmdd-hhmmss>/
  Program.mp4
  ISO-01-<SafeName>.mp4        # display/roster name, sanitized; 01.. in selection order
  ISO-02-<SafeName>.mp4
  manifest.json               # session id, epoch, program + ISO entries {sourceId,name,path,kind}
```

Use the **roster/display name**, not the raw participant id (ids are per-meeting and
meaningless in post). `manifest.json` is what the support bundle and any future
auto-import reads.

## 6. Disk pre-flight + support bundle

- **Pre-flight estimate** before start: sum program + selected ISO bitrates × expected
  duration vs free space on the target volume (port the math from
  `src/engine/isoRecording.ts:173-192` / `diskSpace.ts` into the core, or compute
  shell-side and warn). Low-space → loud warning before arming, not a mid-show surprise.
- **Support bundle** (`SupportBundleBuilder.cs:264-288`): list every ISO path + per-stream
  health (frames, audio samples, warning) so a failed ISO is diagnosable from the bundle —
  the DoD "support bundle lists ISO paths + encode health" bullet.

## 7. UI (Show mode — minimal, progressive disclosure per N1)

- A transport-level **"Program only" ↔ "Program + ISOs"** switch (DoD bullet).
- Per-source ISO enable in the Sources/Inputs surface (a small "ISO" toggle per Zoom
  guest / capture device), feeding `isoSourceIds`.
- ISO health on the transport/record readout: N ISOs armed, any warning surfaced (reuse
  the existing recording-warning surface — no new snapshot-rate bound collections;
  0xc000027b rules hold).
- No Pro-mode complexity; ISO is Show-mode spine.

## 8. Phasing (each independently shippable; Demo E gates on ISO-2)

- **ISO-1 — per-source VIDEO for Zoom participants.** Widen the encoder boundary, zero-copy
  per-source frame refs from gather, NV12 input path on `Mp4Writer`, per-source PTS dedup
  on the shared epoch, folder scheme + `manifest.json`, per-stream snapshot health, loud
  disk/path errors. Proves the pipeline; ISO video files openable and time-aligned.
- **ISO-2 — per-source AUDIO stems muxed into the ISO MP4s.** Tap `work.audioFrames`
  pre-mix PCM per source, silence-fill gaps epoch-aligned, mux into each ISO writer →
  self-contained A+V (the owner's decision-1 shape). **This is the Demo E claim** (program
  + ISO stems openable and aligned in an NLE/DAW).
- **ISO-3 — UVC/capture sources.** `capture:<id>` video (BGRA path) + capture-device PCM;
  `isoSourceIds` accepts capture ids. Broadens to the host camera / screen share.
- **ISO-4 — polish.** Disk pre-flight gate, support-bundle ISO health, the Show-mode UI
  switch + per-source toggles, quickstart note.

Ship ISO-1+2 for Zoom before ISO-3 — the podcast/interview ICP (Zoom guests + host cam) is
covered by 1+2+3, but Demo E is claimable after 1+2 on Zoom alone.

## 9. Invariants (do not violate)

- **Lock order absolute:** `coreMutex → audioOutputMutex_ → ZoomEngineRuntime::mutex_ →
  ::sendMutex_`. Per-source gather stays under `coreMutex` (render thread, zero-copy);
  encode stays under `audioOutputMutex_` via the async sink; never nest the first two on
  the worker.
- **4 ms audio deadline is sacred.** Every ISO writer rides `AsyncEncoderSink`; disk
  pressure drops ISO frames (drop-to-latest, logged), never program A/V, never a worker
  stall.
- **No pixel work under `coreMutex` or hot ticks.** ISO carries `shared_ptr` refs, not
  copies; any convert happens in the async encoder thread.
- **Loud, never silent.** Every ISO failure mode (bad folder, disk-full, track-less file,
  dropped source) reaches `recording.warning` + the support bundle. A silent video-only or
  audio-less ISO is the #286 class and is forbidden.
- **Program recording is never regressed by ISO.** Program is priority-1 on every
  back-pressure/failure decision; ISO degrades around it.
- **Three protocol mirrors stay in lockstep** on every command/snapshot change.

## 10. Test strategy

- **Pure/unit (native):** `RecordingPtsClock` per-`(sourceId,frameId)` dedup + shared
  epoch (extend the existing clock tests); silence-fill accumulate keeps a gapped stem
  sample-aligned; folder/name sanitizer; disk-estimate math.
- **Encoder (native, real MF):** N-writer open/reset (the #286 regression shape per ISO
  writer — a reused ISO writer must not lose its audio track); independent finalize (no
  0-byte tails); NV12 input path produces a playable file.
- **Headless E2E:** extend `scripts/validate-record-audio.mjs` into a
  `validate-iso-record` — fake engine with N participants → enable ISO on 2 → record →
  ffprobe each ISO has video+audio, and head-clap alignment vs program < 50 ms (Demo E,
  scripted so re-runs are one command per FOCUS_PLAN §7).
- **Rig / Demo E:** stop → open program + ≥2 ISOs in an NLE/DAW → sync within a few
  frames.

## 11. Non-goals (v1)

Media-playout ISO (you have the file); separate audio-stem WAVs (decision = muxed A+V);
per-ISO independent encode settings / bitrate ladders; ISO of the multiview or a bus;
post-hoc re-mux tools. All post-beta unless beta demands otherwise.
