# Wave 0 production qualification

This is an offline evidence harness, not a meeting operator or a replacement for
the real-time runtime instrumentation. It launches no app, encoder, media decoder,
meeting or network request. Evidence collection and decoder adapters are separate
workstreams. Until their completion records exist, qualification is **unverified**.

## Commands

```powershell
node --test scripts/qa/production-qualification.test.mjs
node scripts/qa/production-qualification.mjs --synthetic
node scripts/qa/production-qualification.mjs --input run-evidence.json --output new-verdict.json
node scripts/qa/production-qualification.mjs --legacy-soak --input resilient-live-soak-results.json
node scripts/qa/production-qualification.mjs --runtime-snapshots --input captured-native-snapshots.json
```

Output is created exclusively; existing reports cannot be overwritten. Input is
bounded to 256 MiB. Longer evidence must be partitioned explicitly; a partition
does not independently establish the full-hour release gate. Exit 1 means failed
or unverified. Synthetic validation exits 0 only for harness success and always
sets `productionAccepted:false`, with status `synthetic-passed`.

The legacy adapter accepts resilient-soak `samples[].buffer` or Tiles
`health[].buffer`. It carries observed losses and intermediate resets forward,
but never invents slot records from averages, frame counts, scene labels or
recording booleans. Legacy reports therefore cannot pass production acceptance.
Original reports stay untouched; participant names and raw operation arguments
are not included in the verdict.

## Evidence contract: production-qualification-v1

`syntheticEvidence()` provides a concrete safe fixture. Required top-level fields:

| Field | Meaning |
| --- | --- |
| `version`, `mode` | Version above; `measured` or `synthetic` |
| `hardware` | Explicit `os`, `cpu`, `gpu`, `gpuDriver`; collector supplies actual hardware, never inferred from a build |
| `binarySha256` | Exact tested `shell`, `native`, `zoom` hashes |
| `processGeneration` | Stable tested process generation; restarts require a new run |
| `workload` | `width:1920`, `height:1080`, `fps:60`, `bufferFrames:2` or `3`; explicit `sources`, `outputs`, `operations` arrays |
| `interval` | Monotonic integer-string `anchorNs`; independently planned `slots`, `durationSeconds:slots/60`, `startedUtc` |
| `requestedRevision`, `appliedRevision` | Exact configuration convergence; not proof of rendering |
| `coverage` | `complete:true` and matching `processGeneration`, supplied only by a collector covering the entire declared interval |
| `errors` | Explicit collector/runtime error array, including incomplete captures |
| `counters.before/after` | Nonnegative integers for underruns, overflows, deadlineMisses, outputSequenceGaps, gpuNotReady, audioLostSamples, audioReanchors |

Each `rendered`, `delivered`, `presented` path contains `measurement` and `frames`.
Frames are ordered, without holes, from slot 0 to slots-1, with unique `frameId`,
matching process generation and revision, and integer-string `completedNs`.
Rendered/delivered measurement is `gpu-completion`; presented measurement is
`display-completion`. Present submission, nominal fps and first-frame latches
cannot populate these fields. Downstream identity must match upstream; downstream
completion cannot precede upstream completion. A render ID must originate from
an actual accepted frame, not a repeated image assigned a fresh counter.

Deadline for slot i is `anchorNs + ceil((i + bufferFrames) * 1e9 / 60)`.
Every frame must complete by its own deadline. Delivered/presented intervals may
not exceed 16,666,667 ns. This one-nanosecond rational representation is not a
performance tolerance. Presentation must also occur strictly after the preceding
slot deadline, so compressed timestamps cannot pretend to cover a long run.
Render/delivery can complete earlier, subject to upstream ordering. Any loss
counter increase or reset fails. The qualification
interval begins after explicit priming; priming cannot hide losses inside the
declared interval. Both depths and both hardware matrices need separate runs.

## Recording evidence

Declare every admitted destination in `workload.outputs` as `{id,kind}`; kind is
`recording` or `transport`. Each recording needs `destinations[id]`:

- `generation` and matching `processGeneration`.
- Separate `muxed` and `committed` records with `firstSlot:0`, `lastSlot:slots-1`,
  `uniqueFrames:slots`, `gaps:0`. Accepted encoder input is neither record.
- `completed:{state:'completed',closed:true,artifactSha256}` after finalization.
- Independent `decode` of that exact artifact: matching hash, decoder `toolVersion`,
  `success:true`, actual dimensions, integer-string time-base numerator/denominator,
  `durationTicks`, and every decoded frame's `pts`, `slot`, `frameId` and
  `identityMethod:'decoded-content-marker'`.
- Decode audio with sampleRate 48000, samples `slots*800`, firstSample 0,
  lostSamples 0 and independently checked `avAlignmentVerified:true`.

The positive container time base must resolve at least 60fps. Decoded PTS must be
strictly increasing and follow the exact 60fps grid within less than one declared container tick;
duration must cover the requested interval with the same quantization bound.
Zero duration is never accepted.
Counts alone cannot detect padded/duplicated media: the decoder must extract an
embedded deterministic content marker to bind pixels to delivered frame identity.
The writer must not manufacture decoded identities from its input log. Retain the
hashed artifact, decoder invocation/version and raw analysis as evidence sidecars.
`program-buffer-recorded-av.mjs` and `av-content-decode.mjs` remain useful actual
record/decode probes, but their pulse/aggregate evidence must not be relabeled as
per-frame marker proof or durable committed ranges.

## Current boundaries and next adapters

Wave 0 supports one fixed applied revision per qualification segment. It does not
yet qualify mixed-revision live transitions or ISO-specific source timelines;
those need declared per-slot revision/source expectations. Transport completion
is explicitly unverified until a destination-specific adapter is implemented.
No current runtime field is treated as physical presentation evidence. Hardware
and coverage declarations must come from the collector; the evaluator does not
authenticate arbitrary hand-authored JSON.

The gate accepts a measured run only with at least 3600 seconds of complete
evidence. A short diagnostic can demonstrate a failure, but cannot pass the
production gate. The implementation intentionally prevents today's functional
soaks from passing through missing instrumented boundaries.

Next integration steps: native slot/GPU-completion exporter; independent shell
display-completion exporter; sample-range audio exporter; recorder mux/commit
journal exporter and marker decoder; per-slot revision expectations for operations;
streaming/chunked evidence processing for larger multi-output runs. None may infer
missing completion from requested state or erase overload counters.

## Current-runtime snapshot adapter

`--runtime-snapshots` reads captured native sessionState objects only; it never
connects to the application. The envelope is:

```json
{
  "expectedWorkers": ["render", "audio", "videoOutput"],
  "recordingExpected": true,
  "samples": [
    {"processGeneration": "collector-assigned-native-start-identity", "collectedAtMs": 1000,
     "snapshot": {"realtimeEvidence": {}, "programBuffer": {}, "encoderEvidence": {}}}
  ]
}
```

Preserve the native objects, including their metricVersion and generation fields.
The collector's monotonic timestamp must increase. Process identity must identify
the process start, not a reusable PID. Workers expected to be inactive must not be
listed as expected. Missing fields are unverified. A clean capture always remains
unverified because sampled counters cannot prove GPU/display/mux/commit completion.

The adapter attributes render skipped slots/deadline misses, Program losses,
audio discarded timeline/reanchors, writer audio/video drops, stale queues,
stuck operations and pending/running finalization. It detects intermediate process,
worker, buffer and encoder generation changes and counter resets. First sample
counters are a baseline: positive historic loss is not misattributed to the captured
interval. Retain earlier history separately to qualify the entire show.

Diagnostic defaults are staleWorkerMs=250, encoderQueueAgeMs=1000,
operationAgeMs=5000, finalizeAgeMs=10000, overridable through `policy` and recorded
in the verdict. These identify stalls, not acceptable 60fps deadline tolerances;
exact per-slot qualification remains mandatory. Display busy/unconsumed counters
are observations because the adapter cannot infer whether a monitor was enabled.

For an executing stop, native operationAgeMs supplies its elapsed age. For stop
queued behind another operation, total elapsed age is unavailable unless the
collector supplies `nativeNowMs` from the **same native monotonic clock** as
stopRequestedMs. Never substitute wall time or the collector's unrelated clock.
Unknown total stop age is reported unverified; old successful writer counts do not
make an outstanding finalize healthy. Raw writer error text is not copied into
the verdict to avoid exposing paths or other private content.

## Producing the snapshot envelope

`scripts/qa/collect-runtime-snapshots.mjs` is the collector that fills it. It runs
its own media core over the same stdio JSON-RPC wire the shell and the other
headless probes use, samples bare `{"type":"snapshot"}` (the minimum-work read
path: no command mutation, no synthetic render tick) on a schedule, and writes the
envelope.

```powershell
node scripts/qa/collect-runtime-snapshots.mjs --out capture.json --seconds 30 --load 2 --recording
node scripts/qa/production-qualification.mjs --runtime-snapshots --input capture.json
```

- `--interval-ms` (default 250, floor 50) is **declared** in the envelope as
  `sampleIntervalMs`, because the judge reasons about worker staleness and a
  reader has to know how coarsely we looked. The floor exists so the collector
  cannot quietly become the load it is measuring. Honest caveat: every request
  path in the core serialises on `coreMutex`, so no read is free — 4 Hz is simply
  well under the 2 Hz-per-shell traffic the product already generates, and the
  collector never touches `audioOutputMutex_`, the encoder or the engine.
- `processGeneration` comes from the handshake `processEpoch`, not a PID.
- **`nativeNowMs` is deliberately not emitted.** The judge will only age a queued
  stop against the core's own monotonic clock (`AsyncEncoderSink` steady_clock),
  and the collector's clock is unrelated. The core publishes no monotonic "now",
  so an outstanding queued stop stays unverified instead of being guessed at.
  Closing that needs one field in `MediaCore::sessionState`.
- The envelope is written in a `finally`, so a crashed or aborted run still
  yields what it captured. A run that captured nothing is written too, but marked
  `collectorFailed` with populated `errors[]` and exits non-zero — never a quiet
  empty-but-well-formed envelope.

A clean capture is `unverified` by construction; that is the adapter working, not
a defect. Measured 2026-09-09 on this rig: 60 idle samples -> `unverified` with
only the three structural caveats plus a first-sample audio worker that had not
reported progress yet; 80 samples under 2x1080p60 with recording -> `failed`, one
finding, `encoder.droppedVideo +9` in the first interval after recording start.

Run adapter regressions with:

```powershell
node --test scripts/qa/production-qualification.test.mjs scripts/qa/runtime-snapshot-qualification.test.mjs
```
