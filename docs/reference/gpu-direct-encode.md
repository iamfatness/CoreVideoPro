# GPU-direct hardware encode for streaming (#521 slice 1, 2026-09-13)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

The live STREAM is now encoded directly from the compositor's GPU texture by the
Media Foundation hardware H.264 MFT — vMix/Vectar parity — instead of the old
GPU→CPU-readback→~186 MB/s raw pipe→external ffmpeg path that capped 1080p60 at
~0.76-0.82x realtime with NVENC idle. Localhost gate now measures **60.0fps of 60,
realtime** on the GPU path. Slice 1 is the stream only; recording/ISO and macOS
(VideoToolbox) are later slices.

- **HEVC AND AV1 RIDE THE SAME PATH (2026-09-20, owner rulings after the YouTube
  "not enough data" incident).** `GpuVideoEncoderConfig::codec` selects the hardware
  MFT (NVIDIA H.264/HEVC/AV1 Encoder MFTs); HEVC is bound with
  `CODECAPI_AVEncMPVDefaultBPictureCount = 0` because the FLV muxer refuses
  reordered raw HEVC ("Packet is missing PTS", measured); bitstream mode names the
  raw demuxer per codec (`-f h264|hevc|obu`) and copies into FLV, where this FFmpeg
  writes the enhanced-RTMP fourcc itself. **A codec the machine or destination
  cannot honor REFUSES the start** (`StreamStartAdmission.h`: `enhanced-rtmp-required`,
  `codec-not-deliverable`, `no-hardware-encoder`, `gpu-encoder-start-failed`) — the 2026-09-20 failure was a
  silent H.265→H.264 downgrade onto the raw path at 0.87x real time. H.264 keeps
  every path it had; HEVC/AV1 are GPU-direct or nothing until the raw fallback is
  made real-time (sub-project 2). The 2026-08-06 HEVC exclusion in
  `EncoderPolicy.h` was reversed by the owner on 2026-09-20 (patent exposure
  accepted). **AV1 does NOT ship: it is REFUSED with `codec-not-deliverable` — read
  the AV1 bullet at the end of this section before touching it.**
  Gate: `node scripts/validate-gpu-encode.mjs --codec h264|hevc|av1` (the av1 leg
  passes by observing the refusal, never by streaming);
  the per-codec real-GPU round-trips in `MediaFoundationGpuVideoEncoderTest` are
  Windows-only and must run on a `COREVIDEO_WITH_MF_ENCODER=ON` build before merge.
  Spec: `docs/superpowers/specs/2026-09-20-gpu-direct-hevc-av1-stream-design.md`.

- **`enhanced-rtmp-required` IS RTMP-ONLY (2026-09-20 fix wave).**
  `RtmpOutputSenderAdapter` serves RTMP/RTMPS **and SRT egress** through one class
  and one `OutputDestinationSettings`, and the compatibility refusal shipped with
  no protocol guard — so an SRT operator who picked H.265 without ticking
  "Enhanced RTMP (H.265 / AV1)" got NO stream plus a sentence telling them to
  enable an RTMP setting for a transport that never touches FLV. Enhanced RTMP is
  an RTMP/FLV concept; SRT carries MPEG-TS, which takes H.265 natively. The guard
  is `RtmpOutputSenderAdapter::resolveCompatibility()` — the ONE resolution every
  call site in that class goes through — which bypasses the matrix entirely when
  `protocol_.isSrt`, so neither the refusal NOR its E-RTMP advisory (which rides
  `runtimeDetail_`) can claim an RTMP constraint on an SRT destination. **Nothing
  else moved:** AV1 on SRT still refuses `codec-not-deliverable` (that defect is
  in our encoder and is protocol-independent), and the `no-hardware-encoder` /
  `gpu-encoder-start-failed` clauses are untouched for every protocol. Pinned at
  the SENDER, not the policy: `OutputSenderAdapter.SrtNeverRefusesH265ForThe
  EnhancedRtmpCheckbox` / `RtmpStillRefusesH265WithoutEnhancedRtmp` /
  `SrtStillRefusesAv1AsNotDeliverable` /
  `RtmpRefusesAv1AsNotDeliverableEvenWithEnhancedRtmpOn` in
  `MediaCoreCommandTest.cpp`. Those last two exist because
  `StreamStartAdmissionTest` proves the POLICY honors `codecKnownNotDeliverable`
  and NOTHING proved the sender ever SET it — deleting
  `admission.codecKnownNotDeliverable = (... == "av1")` left every C++ and shell
  test green (the #481 rule again). They start no FFmpeg: the frames carry full
  program BGRA with no encoder shared texture, which pins
  `chooseStreamEncodePath` to the CPU fallback on every build.

- **The seam is platform-free.** `modules/GpuVideoEncoder.h` — `GpuVideoEncoder`
  (start/submit/stop/healthy), `GpuVideoEncoderConfig/Frame`, `GpuEncodedChunk(Sink)`,
  and the pure `GpuEncodePathPolicy` + `chooseStreamEncodePath` (unit-tested, no GPU).
  It carries an OPAQUE handle (`sharedHandleHex`/`iosurfaceId`), never a D3D11/MF type,
  so the macOS VideoToolbox impl drops in behind it without touching the sender.
- **Windows impl:** `modules/MediaFoundationGpuVideoEncoder.cpp` runs its OWN D3D11
  device + thread (never coreMutex, never the render thread — the vcam-tap rule), binds
  the hardware H.264 MFT via `IMFDXGIDeviceManager`, opens the compositor's shared
  texture (legacy `OpenSharedResource`), converts BGRA→NV12 with an `ID3D11VideoProcessor`,
  and drives the async MFT event loop (`MF_TRANSFORM_ASYNC_UNLOCK`,
  NeedInput/HaveOutput). Emits an H.264 Annex-B bitstream through the sink.
- **The compositor exports a DEDICATED keyed-mutex encoder texture**
  (`exportEncoderSharedTexture`/`ensureEncoderSharedTexture` in `D3D11CompositorAdapter`),
  separate from `ProgramFrame::sharedTexture` so encode never contends with WinUI's
  preview consumer. It runs whenever `fullProgramReadback` (streaming), BUFFERED or not:
  when buffered, `ProgramFrame::encoderSharedTexture` rides the program buffer to the
  sender — the handle is stable and the copy is the latest composed frame, so the stream
  taps live pixels rather than inheriting the buffer's delay. Producer keying is
  `AcquireSync(0,0)`/blit/`ReleaseSync(1)`; the encoder is the consumer
  `AcquireSync(1,34)`/`ReleaseSync(0)`. **The 34ms (≈2 frame) consumer timeout is
  load-bearing:** a 4ms wait missed the 16ms production cadence and starved the encoder
  to ~2fps; and the encoder must NOT also wait for a fresh submit per NeedInput (that
  serialized with the mutex wait to ~30fps) — it reads the latest handle and lets the
  keyed mutex alone pace it to the producer's 60fps.
- **FFmpeg is demoted to a muxer.** `RtmpFfmpegArgs.h` `videoBitstreamInput` mode emits
  `-use_wallclock_as_timestamps 1 -r <fps> -f h264 -i pipe:0 … -c:v copy`. **Both
  timestamp args are load-bearing:** a raw Annex-B stream on a live pipe carries no
  container timestamps, `-r` alone left stream 0's PTS unset once a second (audio) input
  was present, and `-c:v copy` then muxed a stream the endpoint reads at 0x/stalled;
  wallclock stamps each arriving access unit at realtime (monotonic for a 60fps feed).
- **Path is chosen ONCE at stream start**, logged `[gpu-encode] path=<gpu-direct|cpu-fallback>
  reason=<...>`. GPU-direct requires: an MF encoder impl on the platform, a hardware
  session the `EncoderCapacityProbe` allows (never REFUSED on a pending probe — the
  TESTER rule; `encoder->start()` is the real gate), a hardware MFT exists for the
  resolved codec (H.264/HEVC/AV1 as of 2026-09-20 — see the bullet above; a codec the
  machine or destination cannot honor REFUSES the start instead of downgrading), the
  compositor is exporting the encoder texture on the starting frame, and
  `COREVIDEO_GPU_ENCODE` is not `0`. Otherwise the
  raw NV12/BGRA pipe path (unchanged) carries the stream. The encoder starts BEFORE
  ffmpeg so a failed `start()` downgrades to raw before ffmpeg is launched in bitstream
  mode. On device loss the encoder retires (`GetDeviceRemovedReason`), `healthy()` goes
  false, `submit()` fails, and the existing `OutputDestinationSupervisor` restarts the
  sender, which re-decides the path.
- **Acceptance gate:** `node scripts/validate-gpu-encode.mjs [--seconds N] [--force-raw]
  [--keep]` streams the fake-engine program to a localhost SRT sink and FAILS unless the
  GPU path is taken and the RECEIVED stream is ≥58fps and the sink's own `-stats speed`
  ≥0.97x. SRT (not RTMP) for the sink only because ffmpeg's `-listen 1` RTMP server is
  too flaky to gate on; the GPU path is protocol-agnostic (same sender + `-c:v copy`
  muxer). `--force-raw` sets `COREVIDEO_GPU_ENCODE=0` and confirms the fallback still
  streams. GPU-direct RTMP/RTMPS uses `-tcp_nodelay 1` and a 1 MiB
  compressed-video pipe on Windows. The 2026-09-13 live test isolated seconds of
  encoder-thread blocking in the FFmpeg bitstream pipe. Transport settings alone
  remained timing-sensitive; the Windows bitstream writer now runs separately
  from the MFT event loop, with a queue bounded to 60 chunks / 2 MiB (plus one
  in-flight chunk). Queue overflow or a broken pipe fails the sender for supervisor
  recovery; stop cancels pending writes and joins the worker before closing stdin.
  The asynchronous MFT retains each NeedInput credit across missing-frame and
  keyed-mutex timeouts until ProcessInput succeeds, and consumes one output per
  HaveOutput event. Discarding input credits can leave a healthy-looking encoder
  permanently starved; the real-GPU round-trip test includes a delayed first frame.
  Always measure actual FFmpeg
  frame-count deltas against wall time: arrival-time timestamps can report
  `speed≈1.0x` with only 19 encoded frames/sec. SRT must not receive this RTMP option.
  **Live RTMP to real YouTube is the final MANUAL acceptance step** (speed≈1.0x,
  `nvidia-smi utilization.encoder` non-trivial, CPU down vs raw) — not this gate.
- Tests: `GpuVideoEncoderPolicyTest.cpp` (policy + `chooseStreamEncodePath`),
  `MediaFoundationGpuVideoEncoderTest.cpp` (real-GPU compositor→encoder→ffmpeg round-trip:
  decoded coded-Y-plane luma within 16 of the encoded gray; self-skips without a hardware
  MFT or ffmpeg; plus the submit-fails-when-not-running supervisor contract),
  `RtmpFfmpegArgsTest.cpp` (bitstream mode).
- **AV1 SHIPS REFUSED, NOT BROKEN (2026-09-20, this rig, RTX 4090 / driver
  616.92 / Windows SDK 10.0.26100; issue
  [#565](https://github.com/iamfatness/CoreVideoPro/issues/565)).** GPU-direct AV1
  **binds the hardware AV1 MFT, starts, and runs at the correct cadence** — and
  emits **near-empty access units**: ~54 bytes per sample at 1920x1080@60 (~49
  after a normal 5,892-byte keyframe) against H.264's ~12,483 on the same build,
  i.e. a muxed stream of **~18 kbit/s against a configured 6 Mbps**, three
  consecutive 30 s runs. The 320x180 / 12-frame unit round-trip
  (`DirectSharedTextureAv1RoundTrip`) passes; only the full-resolution, full-rate
  stream is empty. H.264 and HEVC pass the identical 1080p60 gate on the same
  build. A codec that streams at 0.3% of its configured bitrate is exactly the
  defect the refuse-never-downgrade rule exists to remove, so **AV1 is REFUSED at
  start** with its own code, `codec-not-deliverable`
  (`StreamStartAdmission.h`: `codecKnownNotDeliverable` + `notDeliverableDetail`;
  TERMINAL in `isTerminalResultCode`, so it bypasses the supervisor ladder — no
  retry can change settings-shaped truth). The operator reads *"AV1 does not
  produce a usable stream on this machine's hardware encoder (near-empty access
  units, ~18 kbit/s against the configured bitrate). Choose H.264 or H.265."* and
  the compact chip reads `Codec refused`. It is refused BEFORE
  `startGpuEncoderIfChosen`, so no `path=gpu-direct codec=av1` is ever logged.
  **THREE HYPOTHESES ARE ELIMINATED — they are the expensive part of this work
  and must not be repeated:** (1) *deep encoder pipeline (lookahead / alt-ref)* —
  low-latency mode is accepted (`av1 b-frames off via low-latency-mode`, the same
  ladder HEVC uses) and the rate was unchanged; (2) *FFmpeg's `obu` demuxer on a
  live pipe* — a 10 s 1080p60 `av1_nvenc` OBU stream through the sender's exact
  flags gave 600/600 frames, 1.15 MB in / 1.18 MB out; (3) *our async MFT loop
  reading one output per HaveOutput event* — instrumented, `av1 output drain:
  events=1260 samples=1260 mean=1.00 max-per-event=1`, identical to H.264 (commit
  `6cedb9b7` is defensive correctness and a no-op here). What is left is the
  encoder itself producing empty access units — vendor/driver level, and
  deliberately not this sub-project's work.
  **What flips it back:** one named predicate,
  `admission.codecKnownNotDeliverable = (compatibility.requestedVideoCodec == "av1")`
  in `RtmpOutputSenderAdapter::startFfmpegProcess` (an obvious home for a future
  rig-specific override) — and the thing that DECIDES is the gate,
  `node scripts/validate-gpu-encode.mjs --codec av1`, which today **passes by
  observing the refusal** (asserts `stream start REFUSED
  code=codec-not-deliverable` and that NO `path=gpu-direct codec=av1` stream was
  established) and prints `av1: REFUSED as designed (codec-not-deliverable)` so
  nobody mistakes the pass for AV1 working. It must stop passing by refusal and
  start passing by streaming before AV1 can be called done.

### Stream backpressure: shed frame rate, never rebuild the encoder (#597, 2026-09-23)

A live 10 Mbps H.264 YouTube stream on beta `58d4fab` stalled the operator's app for
~20 s: FFmpeg slipped just under 1x, the compressed-video queue hit its bound, the
sender was failed to its supervisor, and the core tore down and rebuilt the hardware
encoder **eight times in 20 s** — each rebuild pushing a fresh ~75 KB keyframe into an
already-backed-up pipe. **The core was starving; the UI was never blocked.** Two levers
plus a restart floor now absorb a destination that cannot carry the configured bitrate.
Spec + full evidence: `docs/superpowers/specs/2026-09-23-stream-backpressure-design.md`.

**#538 update (2026-09-29):** The Lever A compositor export divisor described
below is historical behavior. It reduced every output's frame rate when one
destination was congested. The shared Program encoder now keeps full export
cadence; each destination's queue performs its own GOP discard and IDR recovery.
The sender's `divisor` remains a pressure recommendation for compatibility,
while `appliedDivisor` reports the compositor's actual rate. Validate blocked
RTMP against received SRT/HLS media and Program render slots.

**#538 Slice 8 (2026-09-30): one clock at the mux.** Shared H.264 and shared AAC
reach each FFmpeg as ONE core-stamped MPEG-TS on stdin (`EncodedVideoTransportStream`
with an ADTS PID; `-f mpegts -i pipe:0 -map 0:v:0 -map 0:a:0 -c copy`, no second
pipe, no wallclock). `ProgramStreamClock` (in `CompositeOutputSender`) anchors AAC
once per stream session to the frame clock, using the frame's
`timelineTimestamp100ns` and the audio worker's SCHEDULED tick time. Do not use
`now()` at gather: that carries up to a tick of lateness exactly at stream start.
Three lessons, all measured:
- **Frame number is exact.** Frame number vs timeline slipped 0.00 ms over a run.
  An anchor on frame number alone still quantizes to a whole frame per session.
- **The clap gate was misreading audio.** It read audio from the first decoded
  sample and video at PTS. A fenced TS starts AAC up to one AAC unit after the
  IDR, which read as 2-19 ms of per-session "skew". It now adds the audio
  `start_time`. After that fix RTMP−Record measured 0.3-1.7 ms over five sessions.
- **The process log is lossy.** `nativeLogf` is a 128-slot best-effort queue that
  drops startup lines. Gate evidence lives in each sender's snapshot
  `streamClock {muxInput, firstVideoPts100ns, firstAudioPts100ns, audioUnits, …}`.
  The TS PMT also used to omit the low byte of `program_info_length`, and FFmpeg
  had been finding streams by probing PES; it is now well-formed.
- **Frame number is not the delivery slot.** A render stall moves the program
  buffer's grid relative to frame numbers. `K = timeline - frame/fps` steps by
  whole slots. Frame-number video PTS then sat 4-6 frames early against audio for
  the whole session. It bit about half of all streams started right at core launch
  (a take opening media stalls ~100 ms). The recording was immune; the clap gates
  anchor after startup and missed it. Senders now shift video PTS by the slots K
  moved since the audio anchor (`ProgramSlotClock`).
  - **How it was found:** a temporary frame-number label drawn into Program pixels
    plus a per-frame K trace. Labels proved pixels matched their PTS, so the error
    was the clock, not the encoder.
  - **Rule:** re-run an A/V gate many times before calling it green. This defect
    was a per-session coin flip.

- **THE DIAGNOSTIC TECHNIQUE, worth more than the fix: compare `perf.log` gaps against
  the sample COUNTER, not wall time.** The incident's one 21.01 s gap
  (21:09:12.06 → 21:09:33.07) carried a NORMAL counter delta (10860 → 10890, the usual
  30) with `dispatchQueue=0.0ms` on both sides — a PRODUCER slowdown. A frozen counter
  would have been a UI block. The operator's Stop was recorded while the gap was still
  open, which is the same evidence read a second way.

- **The signal is the wall-clock AGE of the oldest queued chunk** (`bufferedMs = now -
  oldestQueuedChunkEnqueuedAt`), published atomically where the queue is already mutated
  under `bitstreamQueueMutex_`. **Deliberately not a frame-count conversion**: the frame
  rate is being changed underneath the measurement by Lever A, which is exactly when
  frames→ms would lie. It is honestly a LOWER bound — FFmpeg buffers behind us.

- **TWO levers, because throttling does not recover latency already accumulated.**
  Lever A (input divisor 1/2/3/4 feeds the encoder at **programFps / divisor** — 60/30/20/15
  fps at the 60 fps program this product targets, but 30/15/10/**7.5** at a 30 fps one, because
  the divisor is applied to RENDER FRAME NUMBERS and `startProgramOutput` clamps `outputFps_`
  to 1–120; never quote the ladder as an invariant) stops the queue growing and holds
  per-frame quality; Lever B (GOP-tail discard — drop from the head up to a keyframe)
  clears the ~1 s already sitting there. Thresholds, against that ~1 s budget:
  throttle >250 ms (a quarter — act while the response is still invisible), discard
  >750 ms (three quarters — only after Lever A failed to hold it), recover <100 ms (the
  100–250 ms band is the anti-flap hysteresis, since throttling itself drains the queue),
  enter after 30 ticks (0.5 s — a single keyframe spikes the queue), recover after 600
  (10 s — network capacity changes far more slowly than render load), one step at a time
  both ways. Same shape as `core/MonitorShedPolicy.h`; read that one first.

- **LEVER A'S LOCATION WAS MEASURED, NOT REASONED — and the obvious site is inert.**
  Skipping only the sender's `submitFrameToGpuEncoder` left the stream **byte-identical
  (ratio 0.998)**: the encoder's thread advances on the KEYED MUTEX when the compositor
  releases a frame, so a skipped submit delays nothing. The gate is the compositor's
  encoder-texture export (`D3D11CompositorAdapter`, via
  `ICompositor::setEncoderExportDivisor`, a control-plane call made on transitions only)
  — halving that halved egress: 74,401,369 B at 60/s vs 37,260,524 B at 30/s, **ratio
  0.500 / 0.501**, ~124 KB per frame either way, so the CBR bits-per-frame assumption
  holds. **Consequence: Lever A is per-ENCODER, not per destination** (one texture feeds
  every GPU-direct sender, so MediaCore takes the MAX divisor across the active ones) —
  a healthy sibling runs at the struggling destination's frame rate until it recovers.
  Lever B (the queue discard) stays genuinely per destination.

- **A SHED FRAME MUST STILL PUBLISH THE ENCODER TEXTURE HANDLE.**
  `exportEncoderSharedTexture` does two separable things: SUBMIT pixels and PUBLISH
  metadata. Shedding skips only the submit. The sender reads the handle's presence as
  "GPU-direct is available"; an absent handle resolves to `CpuFallback`, flips
  `gpuPathChanged`, and relaunches FFmpeg **and the encoder once per shed frame** — #597
  amplified to tens per second (and on HEVC the restart takes the CPU path,
  `startRefusedInadmissible_` latches, and the stream is dead for the rest of the show).
  A shed frame publishes the LAST SUBMITTED frame number, tracked on the render thread —
  `D3DDecoupledExport::publishedFrameNumber()` is written by its own export thread and
  lags the render thread by an unbounded amount.

- **THERE WERE TWO RESTART AUTHORITIES, and the plan only knew about one.** The
  supervisor's 5→10→20→40→60 s ladder was never bypassed — it was never CONSULTED: five
  of the seven rebuilds were `RtmpOutputSenderAdapter::ensureFfmpegProcess` re-opening
  its own transport from the media tick under an ADAPTER-LOCAL backoff whose first rung
  was **1 s** and whose streak was cleared by the first accepted frame. `1 s wait + ~2 s
  of life` is the measured 2.5–3.5 s cadence exactly. Both authorities are now
  independently bounded and no longer share a key: `TransportRestartFloor` for the
  adapter, and `IOutputSender::restartForSupervisor()` split from the operator's
  `recover()`, so a supervisor restart cannot erase the floor that bounds it. A healthy
  INSTANT must not release a pending fault's rung (it did — that was the 21:09:10.088
  log line the investigation had listed as "Unsure"; **an "Unsure" entry is a lead, not
  noise**). The floor's refusal path also STOPS the child before serving the rung: a
  live-but-unfed child holds the single SRT caller slot, and this repo has been bitten by
  exactly that before.

- **A DESTINATION SERVING ITS OWN RESTART BACKOFF IS WAITING, NOT FAILING (final review,
  #603 — un-deferred and fixed here).** The floor publishes `status="failed"` /
  `lastResultCode="ffmpeg-retry-backoff"` for the WHOLE rung it serves — the only
  vocabulary it had for "not delivering" — and the supervisor classed that Retryable,
  armed a fault per rung and gave up after five. So the floor that exists to PROTECT the
  encoder became a new path from "congested link" to "stream off until the operator
  re-arms", and this branch raising the rungs from 1–30 s to 5–60 s grew that exposure
  window two- to sixfold. It was first deferred as #603 on the grounds that fixing it
  changes give-up semantics; **that reasoning does not survive making the defect worse,
  and shipping a known stream-off regression behind a filed issue is not acceptable.**
  `isSelfManagedRetryResultCode` is the distinction, and it took TWO halves: `classify()`
  returning `None` is not enough on its own, because a destination serving a 60 s rung
  accepts no units and the "stopped accepting output for Ns" branch arms a fault after
  5 s by a different door — the whole fault-arming block is skipped while a self-managed
  retry is pending. Deliberately narrow: an already-armed fault keeps its rung, the
  terminal bypass and give-up are untouched, an unknown code is still RETRYABLE, and the
  next observation carrying any other code restores the ordinary rules.

- **A TRANSPORT REOPEN RESETS THE BACKPRESSURE POLICY, not just an operator stop (final
  review, finding 2 — cross-task, which is why no per-task review saw it).** Only the
  `!wantsRtmp` operator-stop path used to reconstruct `backpressure_`. `reopen()` — both
  `recover()` and `restartForSupervisor()` — stopped FFmpeg and cleared the floor but
  left the policy holding its pre-failure divisor, so the destination came back
  republishing divisor 4 against an EMPTY queue and needed ~30 s of healthy streaming
  (`kRecoverAfterHealthyTicks = 600`, one step per 10 s) to return to full rate, while
  the compositor visibly snapped 4 → 1 → 4 across the outage. A reopen IS a new run by
  every other measure the adapter keeps (`framesSent`, `bytesSent`, `startedAtMs` all
  reset there), so the per-run counters reset with them and `runId` advances.
  `resetBackpressureForNewRun()` is the one place, shared by both doors. **Task 7 owned
  the restart and Tasks 4/6 owned the policy lifetime — a lifetime that spans two tasks'
  seams belongs to neither, and only a whole-branch read finds it.**

- **THE NODE MUST SAY THE RATE THE DESTINATION IS FED AT, NOT ONLY THE RATE IT ASKS FOR
  (final review, finding 3).** Lever A is per-ENCODER, and that limitation was named in
  three comments — but not on the NODE, and the node is what an operator readout binds
  to. With two GPU-direct destinations the healthy sibling published `divisor: 1`, a
  textbook-healthy reading, while being fed at the max across senders; a destination
  added mid-show beside a throttled sibling started at a quarter rate and looked
  perfect. The per-sender node now carries BOTH: `divisor` (this destination's REQUEST,
  which its own hysteresis and counters are keyed on) and `appliedDivisor` (what the
  compositor is actually exporting at, written by MediaCore where that fact exists).
  Read `appliedDivisor` for the rate. **Naming a limitation in the implementation's
  comments is not the same as publishing it where the consumer reads.**

- **BACKPRESSURE'S OWN LAST-RESORT BOUND CALLED THE STORM.** `enqueueBitstream`'s
  overflow path failed the sender ON PURPOSE so the supervisor would restart it — which
  rebuilds the encoder. It predates this work and only the live gate could see it (two
  of three 240 s congested runs were clean; the third stormed with eleven encoder
  starts). It now runs the GOP-tail discard FIRST and fails only when the discard frees
  nothing. Two rulings behind that: ordinary Lever B cuts to the FIRST queued keyframe
  (the smallest clean skip that recovers meaningful latency), the OVERFLOW path cuts to
  the LAST (last resort, whose alternative is rebuilding the encoder, so maximising freed
  room is right) — one pure policy, parameterised, never a second copy; and **a keyframe
  ARRIVAL makes the whole backlog discardable**, because cutting forward to an arriving
  IDR rests on the identical safety argument as cutting to a queued one. That was
  measured, not assumed: the burst gate twice found 60 queued chunks averaging 9.8 KB,
  **all reference frames, no keyframe anywhere**, while the refused 22.6 KB chunk was the
  keyframe at the door. All three discard paths are forward-only PREFIX deletions, so the
  survivor is always a contiguous suffix starting at a self-contained IDR. **Never teach
  the discard to reason about which preceding chunks are parameter-set headers** — that
  is a judgement it will get wrong under load. (Re-entrancy: the discard splits into a
  `…Locked` mutation plus a thin locking wrapper — no recursive mutex, no unlock/relock
  window, since `enqueueBitstream` already holds `bitstreamQueueMutex_`.)

- **PARAMETER SETS ARE IN BAND, and that is what makes the GOP-tail discard safe.**
  Traced on this rig, 200 chunks each codec: every h264 CleanPoint sample is
  `AUD,SPS,PPS,IDR` and every hevc one `AUD,VPS,SPS,PPS,IDR_W_RADL`, with **zero**
  non-keyframe chunks carrying a parameter set. No encoder change was needed; the
  pre-ruled mitigation (`CODECAPI_AVEncVideoPrependSPSPPSToIDR`) is NOT used. Re-derive
  this only if the encoder configuration changes.

- **The gate:** `node scripts/validate-gpu-encode.mjs --seconds 240 --slow-sink`
  (sustained congestion via a bandwidth-limited TCP proxy) and the same with
  `--burst-sink` (stalls the link outright, reproducing the 22→60-chunk onset so the
  overflow branch is actually ENTERED — and a run with no overflow-discard line FAILS).
  It asserts the stream never stops, the divisor steps down and later steps back up,
  zero encoder rebuilds, Program holds 60 fps, and the `perf.log` sample cadence stays in
  its 5–6 s band measured against the COUNTER — the assertion that would have caught
  #597.

- **FOUR MEASURING INSTRUMENTS IN THIS SUB-PROJECT COULD HAVE REPORTED SUCCESS WHILE
  MEASURING NOTHING, and we introduced one of them ourselves while fixing another.**
  (1) The first slow sink used SRT `transtype=live`, which DROPS rather than blocks, so
  the queue never aged past 0 ms and the gate was a silent no-op; (2) FFmpeg's
  `-readrate` throttles MEDIA TIME, not bandwidth, which Lever A cannot answer by
  construction — a confident WRONG NEGATIVE; (3) the burst gate's branch-entry assertion
  was written as a DISJUNCTION that a `nothing-safe-to-drop` FAILURE line satisfied, so
  a run where the discard never worked cleared the check; (4) one commit tightened that
  assertion to require zero `nothing-safe-to-drop` lines AND reworded the very string
  being grepped — leaving it vacuous by construction, so every reported zero in that
  column was UNMEASURED, not measured-zero. The repair was not the grep: the two failure
  messages and the discard line are now composed in `modules/BitstreamQueueOverflow.h`
  and pinned by `BitstreamQueueOverflowTest.cpp`, so a rewording breaks a test instead of
  silently disarming a gate. **Two rules: an assertion nobody has watched FAIL is not yet
  an assertion — mutate the code and see it go red; and a load-bearing log string is an
  INTERFACE that needs a test.** A related habit that paid twice here: a subagent's green
  is not evidence until re-run independently (one reported 1208/0 was a 1-in-3 flake that
  would have merged).

- **WHAT IS NOT PROVEN, precisely — do not upgrade any of these.** Six-plus clean gate
  runs are a FINITE SOAK, not a guarantee (three earlier greens were three draws at a
  ~1-in-3 failure rate, which is exactly what the next round's burst runs exposed).
  Restart-ladder **rung 1 is pinned only by pure unit tests** — no end-to-end run has
  measured its value. **The HEVC overflow branch has never fired**, because the queue's
  cap counts CHUNKS while the throttle limits the ARRIVAL RATE, so at divisor 4 the queue
  reaches seconds of latency in ~23 chunks and never approaches 60 (that is #607). HEVC
  discards were observed NOT to corrupt the RTMP/FLV path (nine ordinary Lever B
  discards, 164 chunks dropped, 7808 frames decoded cleanly) — but **that evidence
  establishes DECODABILITY, NOT A/V SYNC**, and saying so precisely strengthens it: the
  GPU-direct HEVC path sets `timestampedHevcInput` and rides the mpegts envelope with
  real encoder PTS, so a discard's gap is carried honestly through the container instead
  of being renumbered away — which is why 7808 frames decoded cleanly across a 164-chunk
  cut. The resulting audio/video DRIFT across that cut was never measured. And **the
  MPEG-TS/SRT case, where an unsignalled PCR jump was the actual concern, remains
  UNOBSERVED, because the slow-sink harness runs on an RTMP sink.** The operator-facing
  "this stream is degraded and at what frame rate" readout spec §7 requires was **not
  built at all** — the evidence is on the wire and nothing in the console binds to it
  ([#610](https://github.com/iamfatness/CoreVideoPro/issues/610)). The POSIX RTMP compile fix is structural
  only (no POSIX toolchain here), and the overflow call-site test self-skips wholesale
  without `C:\ffmpeg\bin`.

- **Nine issues were filed for defects too wide to fix here; one of them (#603) was
  then un-deferred and fixed before merge.**
  [#601](https://github.com/iamfatness/CoreVideoPro/issues/601) is the one to read first,
  and it may well be the incident's REAL TRIGGER: the GPU-direct encoder **ignores its
  configured bitrate** — ~59.5 Mbps measured at 10000 kbps and ~59.4 Mbps at 2000 kbps,
  the same bytes either way, because only `MF_MT_AVG_BITRATE` is set and no
  `CODECAPI_AVEncCommonRateControlMode` / `MeanBitRate` call exists, so
  `rateControl="cbr"` is inert. **Backpressure MASKS that symptom without fixing it** —
  it sheds input frames so egress falls, while the stream still does not honour its rate.
  Then [#602](https://github.com/iamfatness/CoreVideoPro/issues/602): every supervised
  sender creates a Destination record for every OTHER name in the sync list and faults it
  forever — doubled restart traffic and misleading supervisor logs during exactly the
  incident you would read them to diagnose.
  [#603](https://github.com/iamfatness/CoreVideoPro/issues/603) was one of them and is
  now **FIXED AND CLOSED** — see "A DESTINATION SERVING ITS OWN RESTART BACKOFF IS
  WAITING, NOT FAILING" above; it is recorded here because the REASONING for un-deferring
  it is the transferable part.
  [#604](https://github.com/iamfatness/CoreVideoPro/issues/604) is FIXED (#689:
  `retireFfmpegChild` waits 500 ms, then TerminateProcess and waits for the process
  object, and a start refuses while an unconfirmed child lives). **#708 (2026-09-30):
  the "child still alive 20 s after Stop" that outlived #689 was the HARNESS.**
  `validate-gpu-encode`, `validate-hls-output` and `validate-srt-output` sent
  `stop-program-output`, a command the core does not implement: `[cmd] rejected unknown
  command`, the destination stayed requested, FFmpeg kept publishing, and only the job
  object at core exit ended it. Stop is the desired-state `start-program-output` without
  the destination, exactly what the shell sends. With the real stop, the congested
  child is gone inside the gate's now-ASSERTED 5 s bound. One real product gap rode
  along: `AsyncOutputSender` interrupted removed `rtmp`/`srt`/`ndi` but never `hls`, so
  a stopped HLS FFmpeg blocked on a stalled origin was not released by the removing
  sync. **Rule: a harness that stops something must check the command was ACCEPTED** —
  an unknown command is loud only in a log nobody reads.
  [#605](https://github.com/iamfatness/CoreVideoPro/issues/605):
  `keyframeIntervalSeconds` is NEVER applied (there is no GOP codec-API call in
  `MediaFoundationGpuVideoEncoder`) and nothing can request an IDR on demand, so the MFT
  runs at its driver default — and because the interval counts FRAMES while Lever A sheds
  frames, a GOP spans ~4 s at divisor 4, which is why a 60-chunk queue can hold no cut
  point. [#606](https://github.com/iamfatness/CoreVideoPro/issues/606):
  `MediaCore::startProgramOutput` unconditionally overwrites `videoCodec`, fps and
  `targetBitrateMbps` on every rtmp/rtmps destination from `streamOutputProfile` while
  leaving SRT alone — a **silent codec downgrade path** sitting BEFORE the sender, so the
  refuse-never-downgrade guard sees nothing to refuse (masked today only because the shell
  always sends a profile; it took a gate failure to find it).
  [#607](https://github.com/iamfatness/CoreVideoPro/issues/607): the 60-chunk and 2 MiB
  caps are MEMORY bounds doing a LATENCY bound's job, and both are elastic in the wrong
  variable (1 s at divisor 1 vs 4 s at divisor 4; ~2 s h264 vs ~9 s HEVC) — the last place
  in this sub-project where a count stands in for time, and the final line of defence
  before a destination fault reaches the encoder.
