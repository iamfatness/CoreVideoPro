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
  turns probing off entirely; `COREVIDEO_ENCODER_PROBE_MAX_SESSIONS` raises the count cap
  (default 8).
- **Diagnosability:** the probe summary and the admission code go into the session
  `manifest.json` (`encoderCapacity`, `isoAdmission`) and to `[encoder-probe]` /
  `[recording] iso-admission` log lines.
- **Measured here (RTX 4090, 28 logical CPUs):** `hw=yes sessions<=8 (creation-proof
  only) (probe cap reached; true ceiling may be higher) mft="NVIDIA H.264 Encoder MFT"
  sw=yes`, ~0.5-1.0 s per workload on a background thread. **One machine proves the probe
  RUNS, not that it is correct everywhere** — nothing here has been seen on an Intel or
  AMD integrated GPU, and no over-subscribed machine has been observed refusing a real
  show (the refusal is covered by unit tests only).
- **Tests that arm a real recording must pin the capacity** with
  `corevideo::testing::ForcedEncoderCapacity` (`tests/EncoderCapacityProbeTestSupport.h`)
  — otherwise they race an asynchronous, GPU-dependent probe.
