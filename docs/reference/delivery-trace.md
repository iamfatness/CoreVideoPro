# Bounded core delivery trace

The first #823 implementation observes Windows CPU-source preparation GPU
completion, source upload start/submission/refusal, source draw submission, Program-buffer submission, completed GPU
snapshot preparation and scheduled delivery. Source request/draw submission identifies
its exact requested and selected descriptors; it does not prove GPU completion. The later
Program-ready record is emitted only after the real completion query succeeds.
Source observation time remains distinct from verified acquisition/exposure time.
Camera reader emission, camera receiver, monitor completion and physical display
remain unobserved in this slice; their existing diagnostics continue unchanged.

Set `COREVIDEO_DELIVERY_TRACE_PATH` to a **new** explicit capture file before
launching the owned core. Without it there is no trace allocation, exporter or
clock query on the media path. Media workers stop before capture destruction.
This is an internal diagnostic option, not a rollout/default change.

The ring allocates at most 16 MiB of fixed numeric slots. Append uses at most
four CAS attempts and never waits, formats strings or writes files. Full storage
or writer contention increments lost events. A single background exporter drains
128-event batches to a binary file capped at 256 MiB. Finalization writes the
accepted export count, loss count, export failures and end time into the header.
Existing captures are refused. Sink/open/capacity/finalization failures are
explicit, and an incomplete file never qualifies. Shutdown waits at most two
seconds; a blocked exporter retains only its owned state until it completes.
No new camera-DLL thread is introduced.

V1 uses the 80-byte `DeliveryTraceHeader` and 72-byte `DeliveryTraceEvent` in
little-endian form, pinned by native layout assertions and the Python structs.
Windows event timestamps are raw QPC ticks with the host frequency in the
header; other hosts use steady-clock nanoseconds. The judge also reports requested-versus-drawn source observation age; this is
selection lag, not actual acquisition-to-receiver content latency. Source observation timestamps
retain their separate steady-clock 100ns domain. Do not subtract these clocks
or claim actual content latency without calibration and acquisition evidence.

Events carry capture/session epoch, anonymous session-salted source tag, source
epoch, Program composition sequence, source frame identity, event timestamp,
source observation timestamp, layout signature, numeric stage and reason. For
Program-only events, sourceEpoch stores a process-unique buffer instance epoch; it separates
recreated buffers when joining Program stages. A layout signature is attribution,
not a substitute for the original layout revision. No media, names, URLs or
meeting secrets are written. Source-ID hashes are correlation tags, not security
credentials or global identities.

The additive `deliveryEvidence` snapshot is governed by the native/C#/Swift
contracts and observation projections. Missing old-peer fields remain unknown;
zero losses or enabled capture never imply receiver/display health. Counter
snapshots are not evidence of individual frame delivery. The QA judge joins raw
exact Program identities and the expected scene ingredients and rejects trace loss, incomplete/malformed captures,
missing submission/completion, changed layout attribution and delivery gaps.
The scope field always leaves broader release qualification missing.

```powershell
python scripts/qa/monitor-isolation-ab.py --core C:/path/corevideo-native.exe `
  --fake C:/path/corevideo-zoom-engine-fake.exe --source-commit EXACT_COMMIT `
  --output artifacts/trace-reference --program-scene mixed `
  --cpu-source-preparation 1 --delivery-trace --pairs 3 --duration 120 --warmup 15
```

The harness retains the entire capture and pins its measured window to the core's
raw-clock snapshot observations after fixed warmup. It does not discard startup
or shutdown evidence. All trace losses invalidate the boundary verdict, including
losses outside the measured media interval. Independent native/recording/source
judges remain separate. Explicit source/program selection and workload limits
are retained in the manifest.

This first slice does not complete #823: shared camera diagnostics, monitor/shell
stage correlation, bounded once-per-second aggregate distributions, explicit
interactive capture, calibrated content latency and the matched <1% p95
instrumentation-cost gate remain required by the parent spec. Until qualification,
capture stays opt-in and its partial stage coverage cannot certify #517 or a beta.
