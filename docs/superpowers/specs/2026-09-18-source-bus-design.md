# One source bus: every adapter publishes real pixels + PCM on one contract (#535)

Status: DRAFT design for owner review, 2026-09-18. Not yet approved.
Ranked #535, Now[2], promoted from Next on 2026-09-18 (`docs/BACKLOG.md`).
Continues the persistent-sources model (`docs/superpowers/specs/2026-09-10-persistent-sources-design.md`).

## 1. Why

The UI already treats Zoom, UVC, screen/browser, SRT ingest, capture cards and
media as first-class sources. The core does not. It merges **three private
pipes** on every render tick, and each has a different shape, a different clock,
and a different rule for whether a `VideoFrame` even carries pixels:

- **Zoom** — `IZoomCaptureSource::pollVideoFrames()` / `pollAudioFrames()`, no
  arguments (`Interfaces.h:783-787`). I420 pixels, one latest-frame slot per
  participant, its own frame-sync cushion.
- **Capture** (UVC, screen, browser, SRT ingest) — `ICaptureDevice::pollVideoFrames(int64_t)`
  / `pollAudioFrames(int64_t)`, defaulting to `{}` (`Interfaces.h:1136,1143`).
  BGRA pixels when a real reader fills them; empty for the probe-only kinds.
- **Media** (clips, stills, backgrounds) — `IMediaFrameSource::pollMediaFrames(layers, ts)`
  (`Interfaces.h:912-915`). Takes the render plan's **layers** as an argument
  because a media asset used to be decoded per bus; slice 1 of persistent-sources
  already collapsed that to one decoder per asset.

`MediaCore::renderSyntheticTick` fans these three polls out, merges the results
into one `videoFrames` vector keyed by `participantId`, and everything downstream
(compositor, multiview, ISO gather, encode) reads that merged vector. The seams
that decide "does this source have a frame" are scattered across the three
adapters and the compositor's per-kind fallbacks.

**`SourceRegistry` (#419/#447) is identity, not ingest.** It answers "who exists,
who is bound, who is subscribed" (`SourceRegistry.h`), and slice 2 of
persistent-sources gave it its first writer (the Tiles wall as `Kind::Composed`).
It carries no pixels and no PCM. So today there is an authority for *who a source
is* and no single authority for *what that source is currently delivering*.

The cost is exactly what the completion plan calls F1 (`docs/native-production-completion-plan.md:100`):
the compositor renders a slate for every non-synthetic source whose pipe did not
fill pixels, the rule for that is per-kind, and adding a real device (DeckLink,
SRT decode, NDI receive, MXL) means adding a **fourth** private pipe rather than
implementing one contract. The backlog calls this bus "the bone every later
adapter hangs off," which is why it is design-first and ranked ahead of the
adapters themselves.

Every competitor is built the other way. vMix/Vectar/Ecamm/mimoLive and our own
OBS plugin model a source as one object that publishes frames on one contract;
the mixer selects. This design adopts that shape for the ingest layer, the same
way persistent-sources adopted it for the scene/bus layer.

## 2. The contract

One interface. Every source kind implements it; nothing downstream knows the
kind.

```cpp
// A source's delivery for one ingest tick. Zero-copy: video/audio payloads are
// shared_ptr inside VideoFrame/AudioFrame, so the whole struct is cheap to copy
// across the coreMutex -> gather -> compositor/encode hops (the ISO-1 economics).
struct SourceTick {
  std::vector<VideoFrame> video;   // 0..1 for a normal source; N for a share+cam pair
  std::vector<AudioFrame> audio;   // 0..1 program-rate PCM chunk
  SourceHealth health;             // producing | warming | stalled | failed | idle
  int64_t clockOffset100ns = 0;    // source clock - program epoch; 0 = aligned/arrival
};

class ISource {
 public:
  virtual ~ISource() = default;
  virtual const SourceDescriptor& descriptor() const = 0;   // id, kind, format, capabilities
  virtual SourceTick poll(int64_t programTime100ns) = 0;    // pull, on the gather thread
  virtual SourceIngestCounters counters() const = 0;        // framesIngested/droppedFrames, cumulative
};
```

The `SourceDescriptor` carries the issue's required identity + format fields
(`source_id`, kind, width/height, fps, pixel format, capabilities: hasVideo,
hasAudio, isComposed). The `SourceTick` carries pixels/PCM/timestamp/health/clock
offset. `VideoFrame` already holds pixels, I420, dimensions, timestamp and
frameId (`Interfaces.h:14-69`); it is reused unchanged. `source_id` IS
`VideoFrame::participantId` — the existing `scheme:id` key (`zoom:<pid>`,
`capture:<id>`, `media:<assetId>`) — so the merged-vector keying downstream does
not move.

Three deliberate choices, each an owner decision (section 7):

- **Pull, not push (recommended).** `poll()` is called from the render gather,
  exactly where the three polls run today. The whole downstream — the frame-sync
  cushion, the per-participant latest-frame slot, the ISO drain — is pull-shaped,
  and A/V sync is tuned around it. Push (a source thread writing a bus buffer) is
  the bigger change and the one most likely to break sync; the contract leaves
  room for it (a pushing source can buffer internally and answer `poll()` from
  its buffer) without requiring it now.
- **No `layers` argument.** A source owns one clock and one output
  (persistent-sources §2), so it does not need the render plan to produce its
  frame. Dropping the media adapter's `layers` argument is what makes media fit
  the same signature as Zoom and capture. Media that legitimately needs the plan
  (per-layer crop) reads it at the compositor, not at ingest.
- **`clockOffset100ns` is reported, behavior is unchanged in slice 1.** Each kind
  keeps its current timing (Zoom's cushion, media's `MediaPlaybackTimeline`,
  capture's arrival stamping). The field makes the offset *observable and
  uniform* so ISO silence-fill and A/V alignment can later read one number
  instead of three special cases. Changing timing while restructuring ingest is
  how you break sync; that is a later slice, gated on the clap test.

## 3. The bus

`SourceBus` (new, `native/src/core/SourceBus.h`), owned by `MediaCore`. It is the
one place ingest happens and the one place counters live.

- **It owns the `ISource` set**, keyed by `source_id`, added/removed on the same
  lifetime rule sources already follow (a Zoom participant while subscribed, a
  capture device while connected, a media asset while a live scene references
  it). It joins to `SourceRegistry` by `source_id`: the registry stays the
  authority for identity/subscription/lifetime, the bus is the authority for
  frames. A source registers in both; the bus never invents a source the registry
  does not know.
- **`ingest(programTime100ns)`** polls every source once, under `coreMutex`,
  copying shared_ptr refs only (no pixel work under the lock — the standing law).
  It produces the single merged `videoFrames` / `audioFrames` the render gather
  uses today, so the change is *upstream* of every existing consumer: compositor,
  multiview, preview, ISO gather and encode read the same merged vectors and do
  not change in slice 1.
- **It counts per source.** `framesIngested` (a `poll()` that returned a new
  frameId), `droppedFrames` (a bounded frame pool refusing a frame, or a source
  reporting its own drop), published unconditionally in the snapshot under a
  `sources[]` node — the multiviewer-node rule: present even at zero, absent only
  when there is genuinely no bus.
- **Health is derived at read**, the `OutputLifecyclePolicy` shape already used
  for senders and recording: `producing` requires a fresh frameId within a
  budget; a source whose last new frame is older decays to `stalled`, and the
  compositor's slate becomes a bus state (`stalled`/`warming`) instead of a
  per-kind guess.

A bounded, ref-counted **frame pool** (the F1 spec's back-pressure requirement)
lives behind the bus so a high-rate source (a 60fps capture card, SRT ingest)
does not allocate per frame and a full pool is observable as `droppedFrames`
rather than heap growth. This reuses the buffer-ring discipline already proven in
`CaptureDeviceFrameReaderService` (the 5.2GB→350MB fix) rather than inventing a
new allocator.

## 4. Mapping each kind onto the contract

Each existing adapter becomes an `ISource`. The frame *production* inside each is
untouched in the slice that migrates it — only the interface it presents changes.

| Kind | Today | As an `ISource` |
|---|---|---|
| **Test pattern** | synthetic tick, proven into program pixels (F1 gate) | trivial `ISource`; the migration proof (section 5, slice 0) |
| **Zoom** | `IZoomCaptureSource`, latest-slot per pid, I420 | `poll()` returns the participant's latest I420 slot; cushion stays inside the adapter; `clockOffset` reports the cushion depth. Clarification (as-shipped, 2026-09-19): the live key is the RAW engine participant id, matching `ZoomEngineRuntime`'s roster/`SourceContinuityLedger` — `zoom:<pid>` is the separate ISO/registry id scheme, not the live compositor key. |
| **Capture** (UVC/screen/browser/SRT-ingest) | `ICaptureDevice`, BGRA, per-device reader | one `ISource` per device; `poll()` serves the reader's latest BGRA; `droppedFrames` from the reader's own counter |
| **Media** (clip/still/background) | `IMediaFrameSource::pollMediaFrames(layers, ts)` | `poll(ts)` returns the asset's one decoder frame; `layers` argument dropped (persistent-sources already made it one decoder per asset) |
| **Composed** (Tiles wall; later lower-thirds) | rendered in the compositor, registered `Kind::Composed` | a composed `ISource` whose `poll()` returns its offscreen texture frame (persistent-sources slice 2/3 territory) — contract-ready, not migrated here |

The DeckLink/AJA, SRT-decode and NDI-receive adapters that do not exist yet
implement `ISource` when they are built — that is the whole point of landing the
contract first, and it is why #538/#536 sit behind #535 in the backlog.

## 5. Phasing — each slice ships with a real consumer, stub green

The #419 rule holds: a foundation lands on `main` only with a real consumer,
never as an unwired island. So the contract does not land alone.

- **Slice 0 — contract + bus + test-pattern source, alongside the old pipes.**
  Define `ISource`/`SourceBus`/`SourceTick`/`SourceDescriptor`, wire the bus into
  `renderSyntheticTick` so the test-pattern source flows through it into a
  captured `ProgramFrame`, and add `framesIngested`/`droppedFrames` to the
  snapshot for that one source. The other three pipes still run unchanged in
  parallel. Gate: the existing F1 gate (a CPU-compositor test asserting non-empty,
  correctly-sized program pixels from the bus) plus stub-build green. This proves
  compositor/ISO/encode can consume the bus without a per-kind case, at zero risk
  to live kinds.
- **Slice 1 — Zoom onto the bus, old Zoom pipe deleted.** The highest-value kind
  and the one with the most existing tests (frame-sync, churn, continuity). Live
  soak on the test meeting: fps, program-buffer underruns, subscription churn flat
  across a preview run, A/V clap unchanged.
- **Slice 2 — capture onto the bus** (UVC/screen/browser/SRT-ingest). The kind
  whose `pixels` are empty today for probe-only devices becomes a uniform
  `warming`/`stalled` health instead of a per-kind slate.
- **Slice 3a (shipped on branch 2026-09-19) — media frames onto the bus,
  parity, `layers` retained.** `SourceBus::ingest` gained a kind-selector
  overload; `MediaAssetSource`/`syncMediaSources` mirror the producer (removal
  on absence per kind, like slice 2's capture rule) at the two existing
  injection points (post-roster-merge for stills, post-plan for decoded media).
  The request set, pause/hold, the cue→Program hand-off (#449) and the
  `preview:` poster key all stay inside `OwnedMediaFrameSource` — unchanged.
- **Slice 3b — `layers` dropped.** Media request state (asset, playing, loop,
  which bus) moves from per-tick plan layers to source state set at command
  time (`load-scene-graph`/`set-preview-scene`/`set-media-playback`); `poll(ts)`
  applies hold/roll from that state; the cue→Program hand-off (#449) and the
  `preview:` poster key move inside the source; the still cache becomes the
  still source's decoder. Needs the owner's Take-semantics ruling (#449 step 1
  "hold outgoing picture on a plain cut") because go-live/roll-from-0 and
  hand-off are one decision. Own spec.
- **Slice 4 — retire the three old interfaces.** Once every kind is on `ISource`,
  `IZoomCaptureSource`/`ICaptureDevice`/`IMediaFrameSource` are deleted and the
  compositor's per-kind empty-frame fallbacks collapse to one bus-health path
  (issue done-when #3).

Each slice is independently shippable, stub-green, and validated on the live test
meeting per the recording-PR-needs-Windows-build rule (the MF encoder path never
compiles in CI).

## 6. Proof

- **Unit:** `SourceBusTest` (ingest merges, counters increment on new frameId
  only, a full pool charges `droppedFrames`, health decays on a stale source),
  and the extended F1 CPU-compositor test (real bus pixels reach `ProgramFrame`).
- **Continuity:** the existing `SourceContinuityLedger` and take-record judges
  must read identically before and after each slice — the bus must not change
  what "a source restarted" means.
- **Live soak:** `scripts/mac-show-drill.py --load 8` (sustained fps/delivery
  through the real ingest path) plus a test-meeting soak per slice; A/V clap
  unchanged when Zoom and media migrate.
- **Counters honest:** `framesIngested`/`droppedFrames` come from real
  producer/pool counters, never a proxy — the standing "measure the thing, not a
  proxy that survives the bug" rule.

## 7. Open questions for the owner

1. **Pull vs push.** Recommended: pull for the whole migration; push left as a
   per-source internal detail. Accept, or require push for high-rate cards now?
2. **Slice-0 scope.** Recommended: land the contract with the test-pattern source
   only, keeping the three live pipes untouched, before migrating Zoom. Accept, or
   go straight to Zoom (higher value, higher risk)?
3. **Clock-offset semantics.** Recommended: report `clockOffset100ns`, change no
   timing behavior until a later, clap-gated slice. Accept, or unify timing as
   part of the kind migration?
4. **Frame pool.** Recommended: one bounded ref-counted pool behind the bus,
   sized per source from its declared rate. Any cap the owner wants stated up
   front (the persistent-sources 16-decoder cap is the cautionary precedent)?
5. **Registry relationship.** Recommended: `SourceRegistry` stays identity/
   lifetime; the bus joins by `source_id` and never invents a source the registry
   lacks. Accept this split, or fold frames into the registry?

## 8. Out of scope

MXL receive, DeckLink/AJA vendor capture, SRT decode, NDI send/receive — those
adapters implement `ISource` *after* this contract lands (#538/#536 and later).
No timing/A-V-sync change beyond reporting the offset. No compositor upload-path
change (it already uploads populated `VideoFrame` pixels). No shell/UI change —
the bus is core-internal; the snapshot gains a `sources[]` node the shell may
read later.

**Risks.** Zoom is the migration's real test: its cushion, churn ledger and
continuity ledger are all tuned to the current pipe, so slice 1 must prove them
unchanged on a live meeting, not just in unit tests. The bus adds one indirection
on the render gather's hot path; it must stay zero-copy under `coreMutex` (refs
only) or it reintroduces the pixel-work-under-lock regression this codebase has
fixed twice.
