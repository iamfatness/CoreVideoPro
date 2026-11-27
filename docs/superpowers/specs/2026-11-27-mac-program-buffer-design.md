# macOS (Metal) Program buffer: design and estimate

Date: 2026-11-27. Base: `origin/main` 40a00d6f. Status: **proposed, awaiting owner approval**.
Design only. No product code was written, and nothing was built, run or tested for this document.

Evidence labels used below:

- **Read**: seen in the source at 40a00d6f (or on a named branch).
- **Logged**: seen in the retained logs of the 2026-11-26 qualification
  (`mac-release-1.0.0` worktree, `artifacts/qual-1.0.0-installer14/` and
  `native/build-release-core/gpu-encode-core-stderr-*.txt`).
- **Inferred**: follows from the code, not observed.
- **Unverified**: not established.

## 1. Intent

60 fps is a per-frame delivery requirement (CLAUDE.md, "Frame-delivery acceptance"). The approved
Program buffer holds 2 or 3 frames (default 3, applied after app restart; 33.333 or 50 ms at 60 Hz),
with matching Program audio delay. An overrun the buffer absorbs is diagnostic. Any underrun or
missed scheduled output deadline fails acceptance.

Windows implements it. The Metal compositor overrides none of `configureProgramBuffer`,
`programBufferFrames`, `latestDeliveredProgramFrame` or `takeDeliveredProgramFrame` (Read), so it
publishes `programBuffer.status = "unsupported"` with all counters at zero. Mac underrun counts mean
nothing. This design gives macOS the same operator contract, and evidence that can prove or disprove
it per frame and per output.

### Goals

1. Same contract as Windows: depth 2 or 3, default 3, latched at launch, matching audio delay.
2. Every enabled Program output consumes the same scheduled delivery, with pixels that belong to
   that frame's identity.
3. Underrun, late GPU and per-output missed deadlines are measured separately and cannot be hidden.
4. The scheduling policy is testable on any platform without a GPU.
5. No change to Windows behaviour.

### Non-goals

- Fixing source ingest loss (section 3). The buffer sits after the compositor.
- Delaying ISO video, ISO stems, Preview, input tiles, cue or monitor audio, or meters. Windows
  leaves these at source time.
- Proving physical display scan-out. Studio cells are a UX metric with a tolerance (owner ruling,
  2026-10-22). That ruling was argued from Windows DWM; see decision D6.
- NDI graphics key on Metal (`supportsGraphicsKey()` is false today).
- The Windows source-budget path (`PreparedProgramComposition`), which composes one tick late.
- Moving `D3DProgramBuffer` onto shared code.

### Anti-goals (a failure even if counters look good)

- An "active" status while any enabled output still taps the live render target.
- Zero underruns bought by repeating a frame, moving the anchor, adding hidden depth, lowering
  resolution or quality, or resetting counters.
- Audio delayed while video is not, or the reverse.
- A published counter that nothing increments (see 4.3, item 3).

## 2. Constraints

- Apple Silicon only (earlier owner decision).
- Resolution, quality, audio continuity and enabled outputs are preserved. Mac recording today muxes
  canvas-size Program, so the buffered path must too (Windows' NV12 tap is pinned to 1080p).
- CPU submission and advancing counters prove nothing about GPU completion or delivery.
- No GPU wait, readback or large copy on the render thread or the delivery thread.
- `ProgramFrame` timestamps stay in the `steady_clock` domain that audio and recording use.
- A #419-style foundation lands only with its first real consumer (BACKLOG rule).
- `MediaCore` always calls `configureProgramBuffer(2|3)` from `enableAudioOutputWorker` (Read).
  Once Metal returns non-zero from `programBufferFrames()`, all of these switch at once: audio delay,
  skip of CPU readback, buffered video-out tick, delivered-frame attribution, the multiview PGM
  marker layer and sender settings. **So Metal must keep returning 0 behind a development gate
  (`COREVIDEO_MAC_PROGRAM_BUFFER=1`) until every consumer is ready.**

## 3. Does the buffer explain or fix the 2026-11-26 frame loss?

**No, not on the evidence.** The buffer is required whatever the answer, but it must not be
scheduled as the fix for the failed gates.

| Symptom | What the evidence shows | Would the buffer fix it? |
|---|---|---|
| Show-drill delivery 65 to 91 % | The drill counts new source frames the compositor fetched. In failing runs `[zoom-slot]` shows about 10 to 15 % `overwritten` with a similar `starved` count; passing runs checked show about 2.6 % (Logged). `takeDueZoomVideo` releases frames by a 60 ms age test sampled at render-tick time (Read). Failing p50 values cluster near 61 to 63 ms or 74.8 ms, consistent with source phase on that boundary (hypothesis; not every run fits). The dip also occurred on e117a8fa. | **No.** The loss happens before composition. A frame never sampled cannot be buffered. |
| SRT and HEVC 60 fps lose frames inside the core | Program ran 60.00 slots/s in the polled window. In the worst runs the sender's `framesSent` matches muxed and received video to within about a dozen (HEVC r2: 2888 / 2886 / 2885; SRT r1: 3130 / 3127 / 3120). `asyncWorker.droppedSyncs` was 4 in each of the three snapshots checked, including a passing 59.6 fps run. The full-resolution tap log reported at most 1 or 2 missed ticks per window, in one or two startup-sized windows per run (Logged). | **Unverified. Possibly, as a side effect.** |
| GPU-direct 1080p30 timestamps | A units defect (Read; in the qualification report). | **No.** Fixed on a separate branch. |
| 6 and 2 render deadline misses in two drill windows | CPU-submission lateness only. | Inferred: overruns shorter than the depth would be absorbed. Unverified. |
| 137 to 172 ms gap before the last packet of recordings | Not investigated. | Unknown. |

How the stream loss narrows. `framesSent` counts a GPU submit that returned true, and a
VideoToolbox skip or a missing encoder texture also returns true, so those, and the muxer, can
explain at most the small `framesSent`-to-received difference. Sender-queue coalescing and the
full-resolution tap skip are ruled out by the numbers above. **Inferred:** most of the loss is
between a rendered frame and `RtmpOutputSender` accepting it. Two uncounted stages remain there:

1. `renderVideoOutputTick`'s unbuffered path copies the *latest* `lastProgramFrame_` after a wake.
   A frame replaced before the copy disappears with no counter. The harness did not record
   `realtimeEvidence.videoOutput`, and its `completedTicks` also counts idle timeouts.
2. `RtmpVideoFramePacer` returns early with no counter, judging a whole-millisecond arrival time
   against a fixed grid, with 1 ms tolerance.

The cause is **not found**. The buffer's FIFO `takeDeliveredProgramFrame` would remove stage 1, and
its steadier cadence would feed stage 2 less jitter. Neither is a reason to skip attribution: with a
buffer on top, an unfixed pacer or sink would still lose frames, now against a missed-deadline gate.

Investigate first, as slice S0: (a) count both stream stages and rerun the SRT and HEVC cases;
(b) log per-frame ingest age at fetch to confirm or reject the 60 ms boundary hypothesis (file and
fix it separately); (c) measure render-thread hold against GPU waits (4.2, item 1).

## 4. What exists

### 4.1 Windows

- `core/ProgramPlayoutTimeline.h` (portable). Deadline of slot s = anchor + (s + depth) periods,
  rationally rounded. The render loop supplies slot and anchor. A stall never moves the anchor;
  `takeDue` discards expired slots and counts them. macOS already passes the same slot and anchor
  through `renderDisplayTick`.
- `D3DProgramBuffer`: a pool of depth + 3 slots. States Free, Writing, Submitted, Preparing, Ready,
  Delivering, Exporting (plus AwaitingSources for source budget). A preparation thread snapshots
  the frame, proves GPU completion and reads 1080p NV12. A delivery thread (MMCSS, high-resolution
  timer) touches no D3D. If the target is not Ready at its deadline it is an underrun and is never
  delivered later. Publication that reaches the next deadline is discarded and counted. Delivered
  packets go to a FIFO of depth + 2 (oldest dropped, counted `overflows`) consumed by the video-out
  tick, and to a callback that feeds the virtual camera. Shell and multiview exports run on two
  optional threads that can refuse (`displayBusy`) without delaying delivery.
- `MediaCore` follows delivered frames only when `deliverySequence` advances
  (`RenderedSceneAttributionPolicy`), delays Program and stream PCM by 1600 or 2400 samples
  (`ProgramAudioDelay`), and draws the multiview PGM cell from the delivered texture through a
  `multiview-pgm:buffer-cell` marker layer.
- `stopProgramDelivery` runs right after the render thread joins (#941). A failed buffer stays
  failed: `ensureProgramBuffer` reuses it while size and depth match, so video stops until restart.
- Tests: `ProgramPlayoutTimelineTest`, `ProgramAudioDelayTest`, `ProgramBufferIntegrationTest`,
  `D3DProgramBufferTest` (real-GPU stop, in-flight callback, blocked optional reader, leases,
  reservations, deferred completion, late head not stealing the next deadline, failed buffer still
  pacing empty reads, startup allocation without starting the clock, NV12 cadence, retained pixel
  identity, resize generations, Multiview pixels at fixed consumer phases).

### 4.2 The Mac delivery path today

| Stage | Today (Read) | Consequence |
|---|---|---|
| Render pacing | `sleep_for` to 1.5 ms short, then a yield spin. No QoS or real-time policy on render, audio or video-out threads (`ScopedVideoScheduling` does nothing off Windows). No App Nap or latency-critical activity assertion was found in the shell or core. | Timer coalescing and core placement are uncontrolled. Effect unmeasured. |
| 1. Render GPU waits | With output active (`encoder encoding` or recording) the display tick is a "full tick": `waitUntilCompleted` on Program, then again for the 320x180 preview, under `coreMutex`. Multiview and Preview passes wait too (`renderSecondaryPass` default). One `MTLCommandQueue`. | GPU time is charged to the render thread and the core lock. |
| Program display | One global IOSurface that is the live render target. `SurfaceNSView` re-attaches it from a 60 Hz `Timer`. | No synchronization or frame identity. Tearing possible (Inferred). |
| Multiview PGM cell | Recomposes the Program plan. Metal has no handling for the buffered marker layer. | Enabling the buffer as-is would leave the PGM cell without a picture (Inferred). |
| Full-res tap | Render blits into one texture; a tap thread `getBytes` it into a vector that a later tick swaps into `programFullBgra`. | Pixels do not belong to the frame carrying them; unread tap results are overwritten; the blit can race the CPU read (Inferred, a tearing hazard). |
| Recording | `AVFoundationEncoderSink::submit` uses only `programFullBgra`; an empty one is skipped, uncounted. `readyForMoreMediaData == NO` returns success and drops the frame, uncounted. Queue of 6, drop-oldest, counted. | Silent video loss is possible. |
| Recording audio | The sink overrides only `submitAudio`, so the timestamp passed down is discarded and PTS comes from writer-thread arrival. No `RecordingStartBoundary`. | Windows fixed this with the buffer. Mac needs it too. |
| ISO | Own arrival-driven worker. | Unaffected by the buffer. |
| GPU stream | Three-slot BGRA IOSurface ring, publish-on-GPU-completion, use-count protection; VideoToolbox allows 2 in flight. `framesSkipped()` is read only by tests. | Good ownership model. Skips are invisible. |
| Raw stream, NDI | Use `programFullBgra`. NDI falls back to the 320x180 preview when it is empty. | NDI can send a thumbnail on a frame without the tap (Inferred). |
| Virtual camera, core | Metal NV12 compute tap, four buffers, FIFO, skips counted; sink gets `deliveredAt = 0`; `AsyncVirtualCameraPublisher` holds a one-frame latest slot. | Fed from the render tick, not a schedule. |
| Virtual camera, extension | Sink stream consumes every 8 ms and keeps only the newest buffer; the source stream serves it from its own 60 Hz `DispatchSourceTimer` and restamps it. | Two unlocked 60 Hz clocks resample the video. Repeats and skips are expected with jitter or phase drift (Inferred). A Mac core buffer alone cannot pass per-frame camera identity. |
| Graphics gating | `supportsGraphicsInstances` true; facts and plan evidence are produced at render time. Graphics key unsupported. | Production-time "draw-submitted" leads air by the buffer depth. It already claims no delivery. |
| Fault seams | `CompositorFaultInjection` is not consulted by Metal. `COREVIDEO_QA_PROGRAM_COUNTER` exists only in the D3D11 adapter. | No Mac monitor-stall drill or pixel counter yet. |

**Audio delay today:** Zoom video is held 60 ms to match the three-tick Zoom audio feed
(`ZoomPlayoutTiming.h`); the operator's capture audio sync and per-guest A/V trim act per source;
plugin latency shifts recording PTS. Nothing delays Program for a buffer on Mac, because
`ProgramAudioDelay` sees depth 0. The unified-TS stream anchors AAC samples to the video clock
(`ProgramStreamClock`).

### 4.3 Where "matching Windows" needs care

1. **Windows' GPU-direct stream does not carry buffered pixels.** The D3D11 encoder export is
   written at render time to one live texture (its comment says so), and
   `MediaFoundationGpuVideoEncoder::convertToNv12` takes the frame number from that live
   publication. A late render is therefore not absorbed for the GPU stream (Inferred). In the
   two-input layout the sender skips the audio delay at start
   (`gpuStreamAudioStartupSkipSamples`). The unified-TS path times video by content frame number,
   so this reading does not by itself show an A/V error there. Not measured on Windows.
2. A failed Windows buffer needs an app restart (4.1).
3. `ProgramBufferDiagnostics::deadlineMisses` is published and gated on by
   `program-buffer-smoke.mjs`, and no code in `D3DProgramBuffer` or the D3D11 adapter increments
   it (Read, by search). `docs/program-buffer.md` says it measures GPU readiness. Mac must
   increment it; Windows is out of scope here.

## 5. Approaches

**A. Port `D3DProgramBuffer` to a `MetalProgramBuffer`, policy included.**
Smallest conceptual step and no Windows risk. But the admission, expiry and miss rules would exist
twice, testable only with a GPU, and much of the Windows class is keyed-mutex handoff between
devices that Metal does not need. The two would drift.

**B. A portable schedule core plus a thin Metal backend (recommended).**
Extract the *rules* into pure, clock-injected `core/ProgramBufferSchedule.h`, next to
`ProgramPlayoutTimeline`. Metal supplies textures, GPU completion, threads and leases. The rules get
deterministic tests on every platform, including cases a GPU test cannot time reliably. Windows is
not edited. Cost: until Windows is migrated there are still two implementations of the rules, held
together by a shared table of scenarios taken from the Windows code and tests. The schedule has a
real consumer (Metal) in the same landing series.

**C. A platform-neutral buffer inside `MediaCore`**, delaying `ProgramFrame` leases from any
compositor that reports readiness. Most reuse on paper. It moves ownership away from the existing
`ICompositor` contract, and either forces a Windows migration or leaves two buffer systems at two
layers. Too much risk to a qualified Windows path.

Not viable: delaying only audio or timestamps, or a separate delay per sink (no single deadline, no
shared absorption); a display link as the clock (outputs must run headless, and display refresh is
not the output schedule).

**Recommendation: B.** It meets the contract, keeps Windows untouched, and moves the part most
likely to be subtly wrong into code that can be tested exhaustively.

## 6. Design

### 6.1 Shared and platform code

Shared unchanged: `ProgramPlayoutTimeline`, `ProgramAudioDelay`, `RenderedSceneAttributionPolicy`,
`ProgramBufferDiagnostics`, `DeliveryTrace`, `DeliveryCounterPattern`, the `ICompositor` methods
and `MediaCore`'s buffered branches.

New and portable, `ProgramBufferSchedule`: no threads, clock reads or GPU types. Per-frame
allocation is not allowed. Inputs and outputs:

- `admit(productionSlot, anchorNs) -> ticket | refusal(expired, notMonotonic, noCapacity)`
- `submitted(ticket)`, `gpuCompleted(ticket, gpuEndNs, ok)`, `ready(ticket, readyNs)`
- `decide(nowNs) -> deliver(ticket) | wait(untilNs) | miss(slot, reason, skippedSlots)`
- `release(ticket, leaseKind)`; a ticket is Free only after GPU completion and all leases return
- counters, per-reason misses, and the bounded delivered-FIFO accounting

Rules copied from Windows: the anchor and first slot come from the first admitted frame; the Ready
deadline is the slot's delivery deadline; an expired slot is never delivered; publication at or past
the next slot's deadline is discarded; every skipped slot counts as an underrun; production slots
are strictly increasing; `stop` rejects admission and preserves counters.

Metal-specific, `MetalProgramBuffer.{h,mm}` (a separate unit; do not grow the adapter or
`MediaCore.cpp`): slot textures, command buffers, completion, derivative pools, leases, threads.

Windows behaviour does not change because no Windows file is edited. Moving D3D11 onto the schedule
later needs Windows GPU tests and a Windows qualification, which cannot be done on this Mac.

### 6.2 Slots and ownership

- The Program pass renders **directly into a free slot's IOSurface-backed texture**. No snapshot
  copy: one Metal device, and a slot is not reused while leased. `program_` becomes "the current
  slot". Anything that assumes a stable Program texture or ID must be found in S3.
- Slot count: depth + 3, plus headroom for encoder use counts (VideoToolbox, 2 in flight). Proposed
  depth + 5, to be proven by the holder inventory in tests and published with its byte size
  (about 8.3 MB each for 1080p BGRA, about 33 MB at 4K, before derivatives).
- No free slot is a counted `overflows` and a lost frame. Render never blocks.
- A slot with `IOSurfaceIsInUse` is not chosen.
- One command buffer per slot holds the Program pass and then every derivative demanded by outputs
  enabled *at submission*, so one completion proves them all:

| Derivative | Format and size | For | Owner after Ready |
|---|---|---|---|
| none (the slot itself) | BGRA, canvas | VideoToolbox | slot lease and IOSurface use count |
| recording buffer | BGRA `CVPixelBuffer` from a `CVPixelBufferPool`, canvas | AVAssetWriter | pool buffer retained by the packet, then the writer |
| camera NV12 | 1080p, BT.601 video range, mirror applied (existing kernel) | virtual camera | immutable bytes |
| CPU picture | shared `MTLBuffer`, canvas | NDI, raw-stream fallback | immutable bytes, made on the preparation thread |

Keep the colour handling of each current path. Camera NV12 must not be reused as recording or NDI
input: its matrix, range, size and mirror differ. Do not read 8 MB with `getBytes` from an
IOSurface (the adapter records about 20 ms for that); blit to a shared buffer, then copy once.
A slow consumer must pin its own pool, not buffer slots. The encoder is the exception, bounded by
its in-flight limit.

### 6.3 The delivery clock

The scheduled deadline is **monotonic time on `ProgramPlayoutTimeline`**, anchored to the render
cadence. Not a display link, vsync or audio device. Output-device clocks resample downstream and are
measured per output.

- Wait with `mach_wait_until` on a dedicated thread with `THREAD_TIME_CONSTRAINT_POLICY`
  (period 16.67 ms; starting point about 0.5 ms computation and 1 ms constraint, tune by
  measurement). Log whether the policy was applied and publish it; a refused policy is visible.
  Joining an audio workgroup is optional and not proposed.
- Convert deadlines from the `steady_clock` domain by sampling both clocks next to each other just
  before each sleep. **Unverified:** I expect libc++ `steady_clock` here to differ in clock source
  from `mach_absolute_time` (which CoreMediaIO host time and Metal GPU timestamps follow), notably
  across sleep. S2 must measure offset and drift, and define behaviour on wake: new buffer
  generation, counted, never a silent anchor move.
- Measure wake lateness on every tick. Give the render and audio threads a declared QoS or policy as
  a separate, measured A/B in S2, not as an unmeasured part of the buffer.
- While a show is live, hold an `NSProcessInfo` latency-critical, no-idle-sleep activity (shell, and
  core if measurement shows the child needs it).
- Unlike Windows, the thread need not wake early on Ready: it wakes at the deadline and reads state.

At depth d, a frame whose render starts at slot s has d periods until delivery, covering render,
GPU, preparation and handoff. The render loop already catches up at most 3 slots and then skips; a
skipped slot is an underrun.

### 6.4 GPU readiness

- Encode a signal on an `MTLSharedEvent` at the end of each slot's command buffer, and add a
  completion handler. The handler wakes preparation and records status, `GPUEndTime` and its own
  arrival time.
- **GPU-ready** means the command buffer completed without error. **Ready** means GPU-ready and all
  demanded CPU copies are finished. Only Ready is deliverable. As on Windows, a required derivative
  that is late makes the frame miss for all outputs; optional display and multiview never gate.
- At the deadline, the signalled value lets delivery tell "GPU not finished" from "completed but the
  handler had not run". These are separate miss reasons. `GPUEndTime`, converted and checked in S2,
  gives GPU slack against the deadline for every frame, including on-time ones.
- A late command buffer keeps its slot until it completes. Command-buffer error, or no completion
  in 2 s, fails the buffer and quarantines the slot.
- Replace the 1 s `inflight_` wait (which proceeds on timeout) for buffered Program: slot
  availability is the back-pressure, and it never blocks.

### 6.5 Threads and locks

| Thread | Does | Never does |
|---|---|---|
| render (producer) | admit, encode, commit | wait on GPU or any consumer |
| Metal completion handler | stamp times, mark complete, wake preparation | copy, log, take core locks |
| preparation (serial, strict slot order) | CPU copies, assemble the immutable packet, Ready | touch `MediaCore` |
| delivery (time-constraint) | wake, `decide`, publish packet pointer, push FIFO, call camera sink outside the lock | Metal calls, allocation, I/O, formatting |
| display export (optional) | copy delivered slot to the shell surface | delay delivery |
| video-out (existing) | `takeDeliveredProgramFrame`, fan out | take `coreMutex` in buffered mode |

One leaf mutex guards the schedule and FIFO. It is never held across Metal calls, callbacks, copies,
allocation or logging, and is never taken together with `coreMutex` or `audioOutputMutex_` in either
order (render reaches `admit` while holding `coreMutex`, so buffer threads must never ask for
`coreMutex`). Because a time-constraint thread shares this lock, prefer `os_unfair_lock` or a mutex
with priority inheritance. Completion handlers hold shared state by `shared_ptr` (the
`VcamTapShared` pattern) so a late handler touches nothing freed. Packets and strings are built on
preparation, before the lock.

### 6.6 Removing render-thread waits, monitors and display

- Buffered mode sets `skipCpuReadback` (existing `MediaCore` behaviour). Remove
  `harvestAndKickFullResTap` and the render-tick camera dispatch from the buffered path. The small
  UI preview and pixel signature, if still needed, come from preparation.
- Multiview, Preview and source sheet must not wait on the render thread. Put monitor passes on a
  second command queue and publish their surfaces on completion. A second queue gives scheduling
  freedom, not GPU isolation; monitor shedding stays, and its CPU-only inputs no longer see GPU cost
  once the waits go, so S4 must re-measure it with a stall seam.
- PGM cell: sample the newest delivered slot for the marker layer, holding a read lease until the
  monitor pass completes; record the identity and age used.
- Shell Program monitor: keep one stable global IOSurface, written by the display-export thread from
  delivered slots, so the shell protocol does not change. Without a cross-process lock it can still
  tear. This is within the UX scope but must be reported as presentation unverified. Replacing the
  `Timer` re-attach with a leased surface ring is a follow-up if the UX check fails.
- `latestDeliveredProgramFrame` follows Windows: it returns the newest delivered packet **whose
  shell-display copy has completed**, with `deliverySequence`, `renderPlanEvidence` and surface
  identity. A refused or late display copy holds the previous one, so attribution and take records
  keep their rules. Native outputs use only `takeDeliveredProgramFrame`.

### 6.7 What each output consumes

An output's deadline is measured from the scheduled delivery deadline of the slot. Proposed budgets
are "accepted by the consumer before the next slot's deadline" plus each consumer's own completion
limit; S8 fixes the numbers before any qualification run.

| Output | Consumes | Accepted / completed evidence | Loss that must be counted |
|---|---|---|---|
| Virtual camera | camera NV12, from the delivery callback with `deliveredAt` | CMIO enqueue / extension served-identity counts | latest-slot replacement, queue full, extension drop or repeat |
| Program recording | recording `CVPixelBuffer`, via FIFO | `appendPixelBuffer` yes / finalized-file decode | sink queue drop, writer not ready, append failure |
| GPU stream | slot surface via packet | encoder output callback / muxed and received | in-flight skip, publish wait, VT drop, pacer, sender queue |
| Raw stream | CPU picture | pipe write complete / received | the same, no thumbnail fallback |
| NDI | CPU picture | `send_video` return / test receiver | sender queue, conversion; remove the preview fallback |
| Shell display, multiview PGM | delivered slot | export complete | refused or held, with age (UX only) |
| ISO | source frames, unbuffered | unchanged | unchanged |

On an underrun, consumers receive nothing, as on Windows. Recording and streams show a timestamp
gap; the camera extension may hold its last picture; display holds. None of these hides the miss.

Cadence: when `programTick > 0`, choose pictures from the tick grid even when the destination runs
at 60, instead of the wall-clock pacer. That keeps Windows (tick 0) unchanged.

The AVF writer's not-ready drop must become a counted loss, with a pre-agreed choice between a
bounded wait on the writer thread and drop. Bounded wait is recommended; it never blocks delivery.

**Virtual camera extension.** Change the source stream to forward each consumed Program buffer once,
on arrival, with the producer's scheduled host time, and to drain the sink queue in order. Keep the
timer only for hold and slate (spec law "always serve something"). Publish consumed, served,
replaced and repeated counts. This needs a new extension version and the existing version gate; any
sandbox or activation change must account for the known macOS 26.3 `cmioextension.sb` crash loop.

### 6.8 Audio delay and A/V alignment

- Program, master and stream PCM get `ProgramAudioDelay` automatically once depth is non-zero:
  1600 or 2400 samples at 48 kHz, after all per-source holds and trims. It does not replace the
  60 ms Zoom alignment.
- Monitor, cue, meters and ISO stems stay undelayed. An operator monitoring Program audio hears it
  ahead of the delayed Program picture by the buffer depth (parity; decision D5).
- NDI and stream audio come from the delayed tap, matching delayed video.
- Delay follows the *configured* depth. If the buffer fails, video stops and audio must not drop to
  zero delay mid-session.
- **Recording fix required:** override `submitAudioAt` / `submitRecordingAudioAt` in the AVF sink so
  PTS uses producer timestamps, and share one epoch from the first eligible scheduled frame after
  writer readiness (`RecordingStartBoundary`), with ISO on the same epoch. ISO content is not
  delayed; each ISO file stays internally aligned, but a given picture sits one buffer depth earlier
  in an ISO than in Program on that shared timeline (inferred for Windows too; verify and document).
- **Mac GPU stream:** because the slot travels with the packet, video is delayed like audio.
  `gpuStreamAudioStartupSkipSamples` must return 0 for a buffered GPU tap. Make that an explicit
  input (default keeps Windows behaviour). `holdGpuStreamAudio` stays. This matters for the two-pipe
  layout; the unified-TS path does not use that PCM pipe.

### 6.9 Interaction with the 30 fps timestamp fix

Read from `fix/mac-gpu-stream-cadence`, commit e968b2b6, unmerged when read. It may change.

It adds `ProgramFrame::programTick` (Metal: render slot + 1, keeping skipped slots), VideoToolbox PTS
= tick / 60 with duration at destination fps, a tick-grid decimator, and `frameNumber` as picture
identity only. That separation is what the buffer needs:

- A buffered packet must carry `programTick = productionSlot + 1`, and the surface's published
  `frameNumber`, set when the slot completes.
- **Conflict to fix in S5b.** The branch still calls `slotClock_.observe(frame->frameNumber, ...)`
  and applies `kFor(chunk.frameNumber)` once `deliverySequence > 0`. `frameNumber` counts renders;
  the tick already includes skipped slots, and the audio anchor uses the tick. With a Mac buffer,
  after any skipped slot the slot correction would shift video a second time (Inferred from code,
  not tested). With `programTick > 0`, disable `ProgramSlotClock` correction (or feed it ticks and
  prove the shift is always zero). `deliverySequence > 0` becomes true on Mac for the first time
  with this work, so today's tests of that branch cannot reveal it.
- `timelineTimestamp100ns` becomes the exact delivery deadline, which also removes arrival jitter
  from the stream audio anchor.
- `deliverySequence` counts delivered frames and must never be used as a timestamp.
- Recording PTS uses the delivery deadline, so an underrun is an honest gap.
- That branch notes the Media Foundation encoder has the same 30 fps defect. Unchanged here.

### 6.10 Take, Preview and other latency

- Preview is not buffered. A cue appears as fast as today.
- Take reaches every Program output, the Program monitor and the PGM cell 33.333 or 50 ms later than
  on today's unbuffered Mac, plus existing queueing. A fade is shifted, not stretched.
- Take records and graphics facts complete at production. Their wording must stay
  production-side; add delivered identity if a consumer needs on-air time.
- Zoom source-to-air latency becomes the 60 ms hold plus render plus buffer. The drill's
  source-to-render budget is unaffected; add a separate source-to-delivery measurement.

### 6.11 Failure and shutdown

- `prepareProgramBuffer` builds pipelines, pools, derivative kernels and threads before the cadence
  anchor, without starting delivery. Startup misses are counted. First-use costs such as encoder
  or camera preparation must be prewarmed.
- A size change on the render thread creates a new generation. Old leases stay valid until released;
  measurements spanning generations are invalid. Pool creation must not run on the render thread
  mid-show; prepare off-thread and swap.
- Shutdown order: join render; `stopProgramDelivery()` (stop admission, wake and join delivery and
  preparation, wait for any in-flight callback); then existing output teardown; destroy resources
  last. Stop is idempotent, keeps counters, and never waits without a bound on the GPU: after the
  limit, quarantine and log. Stop-after-intent slots are not underruns; every earlier one is.
- A stopped or failed buffer's `takeDeliveredProgramFrame` still honours its timeout, so the
  video-out worker cannot spin.
- Failure: status `failed`, active 0, a loud log and operator-visible output degradation. **No
  silent fallback to unbuffered**, which would change latency and A/V mid-show. Windows then stays
  down until restart. Decision D4 asks whether Mac should instead attempt one bounded new
  generation (counted, never erasing the failure).
- Core respawn: the same latched depth is passed again; a new process is a new generation.

### 6.12 Telemetry

Keep every Windows field with the same meaning, including `deadlineMisses`, which Mac increments
(GPU not complete at its deadline). Add, additively:

- `misses`: `neverProduced`, `gpuLate`, `preparationLate`, `publicationLate`, `noCapacity`,
  `failed`, with the first and latest miss (sequence, slot, time, reason, lateness).
- Distributions with maxima: render CPU, GPU duration, GPU slack to deadline, preparation, delivery
  wake lateness. `renderDeadlineMisses` stays and is labelled diagnostic.
- `realtimePolicyApplied`, slot count and bytes.
- `outputs[]`, always present for every enabled output: `eligible`, `accepted`, `completed`,
  `missedDeadline`, drops by reason, maximum lateness, first failure, last sequence, and
  `completionEvidence` (what "completed" means for that output, or `unverified`).
- `displayPresentationVerified` stays false. `destinationCompletionVerified` stays false.

Reconciliation invariants, checked by tests and the judge:
`eligible slots = delivered + misses`; for each output,
`delivered while enabled = completed + counted drops + still in flight`; after Stop, in flight is 0.

Update `observation.schema.json`, the support bundle, `DeliveryTrace` stages and mac Diagnostics.

### 6.13 How acceptance is proven

Three layers. All must agree; counters alone never pass.

1. **Counters:** status `running`, requested = active depth, one generation, zero underruns,
   deadline misses, overflows and sequence gaps; every enabled output reconciles with zero
   missed deadlines.
2. **Timestamps:** a bounded delivery trace joins, per sequence, submit, GPU end, Ready, scheduled
   deadline, actual wake, and each output's accept and complete. GPU end must precede the deadline.
   Missing or overwritten trace rows are `unverified`.
3. **Pixels:** the QA counter painted into the Metal Program (port
   `COREVIDEO_QA_PROGRAM_COUNTER`) is decoded from outputs: every identity exactly once, in order,
   not torn.

| Output | Pixel evidence | Tool state |
|---|---|---|
| Program recording | decode the finalized file; also check PTS spacing | port or reuse `recording-pixel-verdict.py` |
| ISO files | `iso-pixel-verdict.py` | check on Mac |
| RTMP, SRT, HEVC, 30 and 60 | decode the local-sink capture (30 fps expects every second identity) | extend `validate-gpu-encode.mjs` |
| Virtual camera | a separate AVFoundation capture client through the installed extension | **new tool** |
| NDI | a local test receiver | **new tool** |
| Source-to-output continuity | `source-picture-verdict.py` rules | current manifest is Windows-clock based; port |
| Display | UX cadence check; physical presentation unverified | new, optional |

A/V: finalized recording and received stream with real tones and flashes (the 2026-11-26 streams
were digital silence), at both depths, through `program-buffer-recorded-av.mjs` and
`validate-av-clap.mjs` on Mac, plus a measurement that the delay really equals 1600 or 2400
samples against delivered video.

The Windows `render-delivery-spec.md` proposes two supplementary limits worth reusing: no
unexplained receiver inter-arrival gap above 33.4 ms, and absolute flash/beep skew within 50 ms with
at most 5 ms median drift between the first and last five minutes. They never replace per-frame
identity. A receiver harness must first be proven lossless against a known-good source.

Release run: 30-minute full workload with every enabled output, at both depths, on AC power, on
named hardware, installed signed build, nothing reset or discarded. The measurement window is
declared beforehand; startup counts are reported beside it. Record machine, macOS, binary hashes,
workload, load, duration and every failure. The held-frame amendment of 2026-10-23 applies to ISO as
written. A finite pass is not a guarantee.

### 6.14 Shell setting and control API

- `ShellPrefs.programBufferFrames` as an **optional** key (a required key would reset older prefs
  files); normalize to 2 or 3, default 3.
- Settings picker: "Program buffer: 2 frames (33 ms) / 3 frames (50 ms). Applies after you restart
  CoreVideo Pro." Show saved value, this session's value and restart-needed, plus the core's active
  value and status. Name the controls for accessibility.
- Latch the value once per app launch and pass `COREVIDEO_PROGRAM_BUFFER_FRAMES` to every core the
  shell spawns, respawns included (the mac analogue of `ProgramBufferStartupSettings`). An explicit
  environment value from a test harness wins and is shown as such.
- `settings.programBuffer.set`: implement in the mac control surface with the Windows rules (exactly
  2 or 3; fractions rejected; publish only after the atomic save succeeds; no live change). Mark it
  supported in `contracts/control.platforms.json`, regenerate the mac manifest, and publish the same
  `/state` fields: `programBufferRequestedFrames`, `programBufferSessionRequestedFrames`,
  `programBufferRestartRequired` and the native `programBuffer` node (confirm wire casing against
  the contract fixtures). Unblock it only when the Metal buffer is on by default.

### 6.15 Fault injection and drills

Test-only seams, structurally unreachable from env, commands or config (the QA counter and
development gate are the deliberate, labelled exceptions):

- Reuse what is already platform-neutral: `ProgramBufferIntegrationTest` drives `MediaCore` with a
  fake buffered compositor. Port the applicable `D3DProgramBufferTest` cases (4.1) to Metal;
  composition reservations and the D3D display reader do not apply.
- Portable, fake clock: absorbed overrun; stall longer than depth; late and out-of-order GPU
  completion; no capacity; expiry at each boundary by 1 ns; replacement generation; stop at every
  state; 60-minute rational-deadline run. Include the Windows scenarios as a shared table.
- Metal, real GPU: hold GPU completion behind an `MTLSharedEvent` gate (the
  `armPassGateForTest` pattern), so lateness is real and completion is never faked; delay
  preparation; delay delivery wake; forced command-buffer error; stop with work and a callback in
  flight; slow encoder, recorder, NDI and camera consumers; blocked display export; pool
  exhaustion; retained-slot pixel identity after later renders.
- Consult `CompositorFaultInjection` in Metal's Multiview and Preview passes and add the stall test:
  with shedding off and on, record what Program loses.
- Each new test must be shown to fail without the behaviour it protects. Timing tests need a quiet
  machine.
- `mac-show-drill.py`: when a buffer is expected, **fail** if status is not `running`; today's two
  program-buffer rows read as zeros. Add `--program-buffer-frames`, the output ledger, and
  video-out and pacer counts. `validate-gpu-encode.mjs`: gate per frame interval, not only average
  fps. Mirror the drill change on shared code so Windows rows keep their meaning.

## 7. Slices and estimate

Estimates are focused engineer-days (one developer or agent lane with review), excluding owner
waiting time. They are judgement from reading the code, not measured velocity.

| # | Slice (each independently reviewable) | Depends on | Days | Confidence | What drives uncertainty |
|---|---|---|---|---|---|
| S0 | Attribution and honest counters: video-out coalescing, pacer, AVF not-ready and empty-tap skips, VT skips, harness capture; rerun SRT/HEVC; ingest-boundary and render-wait measurements. Fixes found are separate PRs and not in this number | none | 2 to 4 | medium | whether the loss reproduces on demand |
| S1 | `ProgramBufferSchedule` and tests. Merge with S3 (foundation plus consumer) | none | 2 to 3 | high | little; rules are readable |
| S2 | Delivery clock and thread policy, clock-domain proof, sleep/wake, wake-lateness measurement; render/audio QoS A/B | none | 1.5 to 3 | medium | macOS scheduling under load |
| S3 | `MetalProgramBuffer`: slots, render-into-slot, readiness, delivery, `ICompositor` overrides, stop; gated off; GPU fault tests | S1, S2 | 5 to 8 | medium | assumptions about a stable `program_`; GPU timing tests |
| S4 | No render-thread GPU waits, monitor queue, PGM cell, display export, shed re-measure, Metal monitor seam | S3 | 3 to 6 | medium-low | contention exposed once waits are gone |
| S5a | Recording consumer (`CVPixelBuffer` path, counted writer loss) | S3 | 2 to 4 | medium-low | AVAssetWriter back-pressure at 4K, colour parity |
| S5b | Stream consumers: slot to VideoToolbox, tick cadence, slot-clock conflict, raw path | S3, timestamp branch merged, S0 | 2 to 4 | medium | the branch may change |
| S5c | Camera core feed and NDI from delivery | S3 | 1.5 to 3 | medium | NDI runtime behaviour |
| S6 | Audio and A/V: AVF timestamps and epoch, skip-policy input, alignment tests | S3, S5a, S5b | 2 to 4 | medium | existing A/V offsets surfacing |
| S7 | Camera extension: forward on arrival, counters, version, installed check | S5c | 3 to 7 | low | CMIO behaviour, consumer pacing, activation and signing |
| S8 | Ledger, miss reasons, schema, trace, support bundle. Skeleton lands with S3; each output adds its row in S5 | S3 | 2 to 4 | medium-high | per-output completion definitions |
| S9 | Shell setting, latch, control action. Merge last | S3 | 1 to 2 | high | little |
| S10 | Verifiers: Metal QA counter, pixel judges, camera and NDI receivers, A/V, drill gates. Start early, beside S3 | S3 | 4 to 7 | medium-low | the two new receivers must be proven not to lose frames themselves |
| S11 | Enable by default, both-depth qualification and one fix round | all | 3 to 10 | low | what a 30-minute per-frame gate finds |

**Total: 34 to 69 engineer-days** (sum of the rows). With two or three lanes after S3, roughly 4 to 8 calendar weeks. The critical path
is S1, S2, S3, S5, S10, S11. **The total excludes** fixing the ingest drill loss, any other defect S0
finds, and further rounds after the first failed qualification. Overall confidence: low to medium.

Landing rule: every merge before S11 leaves Mac `unsupported` by default and has no effect on
Windows behaviour. Shared-code edits (policy input, drills, schema) need Windows CI and a Windows
reviewer.

## 8. Left to the builder

Class and file names inside the boundaries above; the lock primitive; completion-handler against
shared-event polling details; time-constraint numbers; final slot count (proven); `CVPixelBuffer`
against blit-buffer mechanics for the CPU derivative; where the preview signature is computed;
ledger field names within the schema; test fixture layout.

Not left to the builder: the anchor and deadline rules, no silent fallback, no thumbnail or repeated
frame counted as delivery, the default-off gate, and Windows behaviour.

## 9. Key risks

1. The buffer ships and the drill still fails on ingest loss. Mitigation: S0 and a separate fix.
2. Apple scheduling misses wake deadlines under real load.
3. The camera chain (core, CMIO queue, extension, client) cannot meet exact-once even when
   forwarding on arrival, because the client has its own timing.
4. A hidden consumer keeps reading "the Program texture" and gets another frame's pixels.
5. Per-output verifiers lose frames themselves and blame the product.
6. 4K memory and bandwidth with several derivatives; unmeasured.
7. Base M-series hardware behaves unlike the M3 Max used so far.
8. The timestamp branch changes while this is built.

## 10. Owner decisions needed

| # | Decision | Recommendation |
|---|---|---|
| D1 | Must the Mac GPU-direct stream carry buffered pixels? It is a deliberate difference from what the Windows code appears to do (4.3). Should Windows be checked separately? | Yes, buffered on Mac. Have Windows verified on Windows. |
| D2 | Is the camera extension change (S7, new version) in scope? Without it, per-frame camera acceptance is unlikely. | Yes. |
| D3 | Release order: does macOS 1.0.0 wait for the buffer? And where does this rank in BACKLOG? | Resolve S0 and the failed gates first; state the buffer's absence plainly in anything shipped before it. The release call is yours. |
| D4 | On buffer failure: stay down until restart (Windows), or one counted automatic rebuild? | Parity first: stay down, loudly. |
| D5 | Monitor and cue audio stay undelayed (Windows behaviour)? | Yes. |
| D6 | Does the windowed-display tolerance ruling (made from Windows evidence) apply to Mac Studio cells? | Apply it provisionally, with a Mac app-less control to confirm. |
| D7 | Which Macs, canvas sizes and output combinations define acceptance? | M3 Max plus the lowest Mac you intend to support; 1080p60 full workload; 4K reported separately. |
| D8 | Per-output deadline budgets and writer back-pressure policy (6.7). | Next-slot acceptance; bounded wait on the recorder's own thread. |

## 11. Not verified

No build, test or run was done. Not verified: any timing or GPU behaviour of the proposed design;
Apple clock-domain relations; time-constraint scheduling; Windows behaviour in 4.3; the camera
resampling, PGM-cell, tap-race and NDI-fallback inferences; the cause of the stream loss and of the
ingest dip; the final form of the timestamp branch; all estimates.

Reading limits: some middle sections of `D3DProgramBufferTest.cpp` (mainly the display-reader fault
tests) were surveyed by test names and assertions; parts of `program-display-zero-miss-plan.md`
were skipped; D3D11 monitor-worker internals were not read beyond their buffer interface. A
separate fresh-reader check of this document was not run.
