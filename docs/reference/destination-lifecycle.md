# Destination lifecycle is TRUTHFUL, and Stop does not claim completion (beta slice, PR22)

_Moved verbatim from `CLAUDE.md` (#737). Paths are relative to the repo root._

Every output destination — the recording writer and each RTMP/SRT/NDI sender — reports
one contract state machine, and it is decided from evidence, never from a request:

```
requested -> preparing -> producing -> stopping -> finalizing -> completed | failed | interrupted
```

Three things changed, each of which was a lie an operator could read:

- **Stop used to report success before the work was done.** `MediaCore::stopRecordingSession`
  assigned `recordingStatus_ = "stopped"` **immediately**, before it had even called
  `encoder->stopRecording()`. The RPC returned there — so the operator was told the
  recording had finished while the FIFO barrier was still draining and the moov atom had
  not been written. The truth lived only in `recording.lifecycle`, and the two fields
  disagreed for the whole finalize window. Stop now reports that stopping has **begun**
  (`status: "stopping"`, `writerStatus: "finalizing"`), and the terminal state arrives from
  the writer thread when the barrier has drained and finalization has actually succeeded or
  failed. **The RPC still does not block** — the asynchronous finalization was always
  correct, and `scripts/validate-recording-finalization.mjs` proves it completes with the
  core alive. Only the lying field was fixed.
- **`recording.status` / `recording.writerStatus` are now PROJECTIONS of the lifecycle**
  (`core::publishedRecordingStatus` / `publishedRecordingWriterStatus`, pure and tested), so
  they cannot contradict it again. `recordingStatus_` survives as the internal desired-state
  gate that the render gather and the idempotent-start dedup read — it is not, and never
  was, evidence of what the writer is doing. New values: `stopping`, `starting`,
  `interrupted`, `idle` (and `opening`/`finalizing`/`stalled` on the writer side).
- **`producing` REQUIRES FRESH PROGRESS.** The old `live` was latched the moment a request
  produced its first frame and was never re-examined, so a wedged writer reported healthy
  for the rest of the show. `AsyncEncoderSink::session()` now re-decides the active state
  against the clock at **READ** time — which is the only thing that works, because a writer
  blocked inside the wrapped sink applies no further items and therefore publishes no
  further snapshots. A destination whose last observed progress is older than the budget
  decays to `interrupted`, and returns to `producing` when real progress resumes.
  **The budget is not a new number:** it is
  `scripts/qa/runtime-snapshot-qualification.mjs` `DEFAULT_RUNTIME_POLICY.encoderQueueAgeMs`
  (1000 ms), reused so there is one declared definition of "the encoder has stopped moving".

**Senders have a lifecycle now.** `OutputSender.lifecycle` (snapshot
`outputSenders.senders[].lifecycle`) gives every destination a state and a terminal
outcome. It is computed centrally in `MediaCore::evaluateSenderLifecycle` from the pure
`core::SenderLifecyclePolicy`, so RTMP/SRT/NDI keep exactly ONE status machine each instead
of gaining a second. Two traps encoded there: **`lastError` is sticky history, not current
state** (a genuinely streaming SRT sender still carries its first-tick "waiting for composed
BGRA program pixels" — treating that as failure reports every live stream as broken; failure
is `status`/`destinationHealth`), and freshness is sampled between snapshot reads, so a dead
sender decays within one poll interval of the budget, not instantly.

**The decision logic is pure and testable without a writer** — `native/src/core/OutputLifecyclePolicy.h`,
the `CaptureReaderStallPolicy`/`NativeUvcCapturePolicy` shape, covered by
`native/tests/OutputLifecyclePolicyTest.cpp` plus two integration tests in
`AsyncEncoderSinkTest.cpp`: a writer wedged inside the wrapped sink decays out of
`producing` and recovers, and Stop never claims completion until finalize returns.

**It reaches a support bundle**, which is the point — `SupportBundleBuilder` projects both
the recording lifecycle and every sender lifecycle (redaction-safe: `Error` rides the same
endpoint filter), and triage names a failed/interrupted destination, a bundle exported
during the finalize window, and a stream that ended without sending media.

**Two places the contract was still being broken, both fixed 2026-09-12:**

- **A recording owns the encoder generation until its Stop barrier publishes
  (#466).** `startProgramOutput` treated `recordingStatus_ == "stopping"` as
  released, so the repeated desired-state `start-program-output` carrying only
  the remaining stream destination called `encoder->start()`, bumped the sink
  generation, and `AsyncEncoderSink` reset the snapshot with no recording
  lifecycle. The old generation's Stop barrier then finalized the file and had
  nowhere to publish `completed`: stop Record with a stream up and
  `recording.status` sat at `"stopping"` for the rest of the show. The file was
  never at risk (the barrier is FIFO ahead of the new Start) — the truthful
  lifecycle was. Ownership is now `"recording" || "stopping"`, which also stops
  the live stream eating a reconnect and a keyframe on every Record stop.
  **The test asserts the RESTART, not the status:** the stub encoder publishes
  no lifecycle, so the visible symptom only appears with the real sink, and a
  status assertion passes with or without the fix (it did — that version was
  thrown away). Counting `encoder->start()` pins the cause where the fix lives.
- **A sender's `status`/`destinationHealth` are PROJECTIONS (#468).** They came
  straight from the adapter's last LAUNCH state, which a dead destination never
  disturbs: live 2026-09-10 an RTMP sender pointed at nothing reported
  `status: live` / `destinationHealth: ok` for 20+ s with `framesSent` stuck at
  8-10. The lifecycle knew (`producing` -> `interrupted`) and the supervisor knew
  (unhealthy, restarting); only the operator-facing pair did not.
  `core::publishedSenderStatus` / `publishedSenderDestinationHealth` project them
  from the lifecycle and the supervisor. **`lastError` is deliberately not an
  input** — it is sticky history, and a genuinely streaming SRT sender carries
  its first-tick error forever. **An ABSENT supervisor keeps the adapter's own
  words**: inventing health for a destination nothing watches is the same lie
  pointing the other way. **And a destination that NEVER connected is its own
  failure mode**: `evaluateActive` holds it at `preparing` while `everProgressed`
  is false, so it never reaches `producing` or `interrupted` and the first cut of
  this projection passed the adapter's `live` straight through. Found by pointing
  RTMP at a hostname that does not resolve (2026-09-12) — FFmpeg dies at DNS
  resolution, so nothing is ever accepted. `preparing` ALONE is never the
  evidence (every healthy destination looks like that for its first moments);
  the supervisor having already failed and restarted it is.

**Contract:** `starting`/`live` remain in the `OutputLifecycle` enum as the RETIRED names so
a newer consumer can read an older core; new producers must not emit them. Absent lifecycle
means UNKNOWN, never healthy. Vocabulary and rules: `contracts/README.md`.
