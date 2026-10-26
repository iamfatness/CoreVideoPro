# Encoder capacity is PROBED, and the software spill is LOUD (beta slice, 2026-09-09)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

ISO encoder placement used to be planned against a hard-coded literal —
`hardwareSessionLimit = 8, reserved = 1, hardware = true, software = true` — passed
straight into `planIsoEncoders`. Every machine was told it had eight hardware encode
sessions, and an over-subscribed one **spilled to the CPU software MFT in silence**:
the only record was a `fallbackReason` in a manifest nobody opens. Beta testers have
GPUs we have never seen, so that is exactly the unwitnessed failure this slice exists
to remove.

- **`modules/EncoderCapacityProbe`** replaces the literal. Per `(codec, width, height,
  fps)` it names the DXGI adapter (description/vendor/device/LUID — a support bundle
  now says which GPU), finds the hardware encoder MFT, and counts how many independent
  sessions it can CREATE at that exact size and rate, taking each one to
  `MFT_MESSAGE_NOTIFY_BEGIN_STREAMING` (where NVENC's driver-side limit is actually
  enforced), plus whether an OS software H.264 MFT exists.
- **The number is a CEILING, and says so** (`ceilingIsCreationProofOnly`).
  `production-realtime-architecture.md:136` is explicit that hardware-session creation
  is not proof of sustainable capacity, and this probe establishes creation and nothing
  more. Never promote it to a guarantee without measuring sustained throughput.
- **Two traps found on the rig, both now fixed in the probe, both would have produced a
  confidently wrong answer:** a hardware encoder MFT is an ASYNC MFT and refuses
  `SetInputType` with `MF_E_TRANSFORM_ASYNC_LOCKED` (0xC00D6D77) until
  `MF_TRANSFORM_ASYNC_UNLOCK` is set — without it an RTX 4090 reported "no hardware
  encoder"; and two probes running concurrently (the sink's default-profile prewarm and
  the `configureRecording` prewarm) cannibalise each other's sessions, so probes are
  **serialised process-wide**. Relatedly, "an MFT exists but not one session could be
  created" is reported as an INCONCLUSIVE probe, never as "no hardware" — that shape is
  contention far more often than incapability.
- **Never on a hot path.** `lookup()` is a leaf-mutex map read and returns immediately;
  a miss reports `pending` and kicks a detached background probe (the `startPluginHostScan`
  / `StillMediaFrameCache` law). Prewarm happens at sink construction (default profile)
  and at `configureRecording` (the real one), rate-limited to once a minute per workload
  and **suppressed entirely while a recording is live** — the probe transiently occupies
  encoder sessions and must never compete with a show. Cached per workload; the whole
  cache is dropped when the DXGI adapter LUID changes (eGPU, driver reinstall,
  switchable graphics). The singleton is deliberately leaked so a detached probe cannot
  publish into a destroyed object at process exit.
- **`modules/IsoEncoderAdmission` decides admit / warn / refuse** and is pure and
  unit-tested (`IsoEncoderAdmissionTest.cpp`), in the `CaptureReaderStallPolicy` /
  `DeviceLossPolicy` shape. Tracks with nowhere to go, or a spill bigger than the
  machine's software budget, **refuse ISO before the show** — program still records,
  same priority-1 treatment as the unwritable-folder refusal — with an ACTIONABLE
  message naming how many ISO sources this machine is good for. A spill within budget
  arms but rides `recording.warning`.
- **THE TESTER RULE: we never refuse a show on an assumption.** If the probe is pending,
  failed, disabled or unavailable, the capacity falls back to `assumedIsoEncoderCapacity`
  — byte-for-byte the old literal — and the verdict may warn but may NOT refuse. Nobody's
  show gets blocked because we could not read their driver. `COREVIDEO_ENCODER_PROBE=0`
  turns probing off entirely; `COREVIDEO_ENCODER_PROBE_MAX_SESSIONS` overrides the bounded
  count cap. The default is nine: one Program recording plus eight selected ISOs.
  The old default of eight truncated discovery below that supported workload and
  forced the final ISO onto software even when another hardware session was available
  (#1013). This changes how far discovery asks, not what a driver is assumed to support;
  measured lower capacity and the unprobed eight-session fallback remain unchanged.
- **Diagnosability:** the probe summary and the admission code go into the session
  `manifest.json` (`encoderCapacity`, `isoAdmission`) and to `[encoder-probe]` /
  `[recording] iso-admission` log lines.
- **The verdict is in the snapshot, structured (#935):** `recording.isoAdmission`
  carries `decision`, `code`, `message`, the hardware/software/unplaced counts,
  `admissibleIsoCount` (null when unknown), `probed`, `probeStatus`, `capacity`
  (`probe.describe()`) and `workload`, and each ISO stream carries `isoIndex`.
  Readers (validator, outputs drill, shell) use these, never the
  `recording.warning` text. This only PUBLISHES what placement and admission
  already decided: probe semantics, the cap of 9, the unprobed fallback of 8 and
  the software budget are unchanged. Encoder placement is reported, never gated;
  a run fails only on measured loss (Program on software stays a FAIL, D8). See
  `docs/reference/iso-recording.md` "The software-encoder admission is a note".
- **Placement vs the encoder actually used: MEASURED, not requested (#935 review B1).** Asking Media
  Foundation for hardware (`MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`) proves nothing:
  it silently falls back to the OS software H.264 MFT. At `BeginWriting` each writer
  reads back the encoder MFT it actually instantiated (`GetTransformForStream`,
  category `MFT_CATEGORY_VIDEO_ENCODER`) and classifies it `hardware` when the MFT's
  attribute store carries `MFT_ENUM_HARDWARE_URL_Attribute` or
  `MFT_ENUM_HARDWARE_VENDOR_ID_Attribute`, else `software` (logged as
  `[recording-transform] ... encoder=`). Measured on the RTX 4090: "NVIDIA H.264 Encoder
  MFT" reads hardware, the spilled ISO and a Program forced with
  `COREVIDEO_QA_PROGRAM_SOFTWARE_ENCODER=1` (QA only) read software. Published as
  `recording.encoder.encoderPath` (null until Program began writing),
  `recording.encoder.hardwareAccelerated` (= measured path is hardware) beside
  `hardwareRequested` (the request), the Program stream's `encoderPath`, `videoWorkUs`
  and `muxVideoFrameCount`, and each ISO's `encoderPath` (measured, null until its
  writer began) beside `plannedEncoderPath`. A hardware plan served by the software
  MFT reports `fallbackReason: hardware-requested-software-used`. Pinned by
  `MediaFoundationProgramForcedOntoSoftwareMftIsMeasuredSoftware` and the two
  eight-ISO MF tests.
- **Arm can read `pending` (#1042):** on 2026-10-24 the validator's 8-ISO 1080p60
  take armed while the `configureRecording` prewarm for that workload was still
  running, so it got the unprobed fallback (7 + 1 spill), and the late probe
  result was discarded as overlapping live encoding. Only the 1080p30
  default-profile probe completed (`sessions<=9`).
- **Measured here (RTX 4090, 28 logical CPUs):** `hw=yes sessions<=8 (creation-proof
  only) (probe cap reached; true ceiling may be higher) mft="NVIDIA H.264 Encoder MFT"
  sw=yes`, ~0.5-1.0 s per workload on a background thread. **One machine proves the probe
  RUNS, not that it is correct everywhere** — nothing here has been seen on an Intel or
  AMD integrated GPU, and no over-subscribed machine has been observed refusing a real
  show (the refusal is covered by unit tests only).
- **Tests that arm a real recording must pin the capacity** with
  `corevideo::testing::ForcedEncoderCapacity` (`tests/EncoderCapacityProbeTestSupport.h`)
  — otherwise they race an asynchronous, GPU-dependent probe.

## Arm waits for an in-flight probe (#1042)

`configureRecording` prewarms the probe for the take workload, and Record can follow within about 2 s, before it finishes. `start` therefore calls `EncoderCapacityCache::awaitProbe(key)` for the exact workload BEFORE `setRecordingActive(true)`; after that call the probe thread would discard its own result as overlapping the take. It waits ONLY for a probe `configureRecording` already launched: it never starts one, so a cold arm keeps the non-blocking contract and the honest `pending` verdict. Only ISO takes wait. The bound is 5 s. On timeout the take arms on the loud unprobed verdict (`iso-encoder-software-spill-unprobed`, outcome `timed-out`) and the late probe result is discarded; arm is never refused because a probe was slow.

In production `start` runs on the `AsyncEncoderSink` writer thread, so the wait blocks that thread, NOT the command path (the command returns at once); in an unwrapped core (tests) it blocks the caller. Audio that arrives behind the Start barrier meanwhile is counted as startup loss (`recordingStartupDroppedAudioPackets`), never as a blocked writer, and the 15 s preparation clock starts when the writer begins applying Start. While blocked, `recording.detail` reads `waiting for encoder capacity probe`.

Published structurally on `recording.isoAdmission`: `armWaitMs` and `armWaitOutcome` (`not-started` | `cached` | `timed-out` | `discarded`). The `capacity` text also carries `arm-waited=Nms` wording derived from the outcome, but readers must use the fields.
