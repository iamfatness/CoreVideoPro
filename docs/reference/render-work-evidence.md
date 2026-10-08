# Independent CPU render-work evidence

`COREVIDEO_QA_RENDER_WORK_DISTRIBUTION=1` enables an internal bounded timing
collector for matched trace-on/off experiments. It is disabled by default and
allocates 32,769 atomic counters (256 KiB plus one overflow counter). Record
uses the render worker's existing duration and one relaxed atomic increment;
it does not read a clock, allocate, lock, export or inspect media. This collector
is independent of the production delivery trace's exporter-owned aggregates
and must be enabled identically in both comparison conditions.

The measured CPU duration includes `renderDisplayTick` and event drain while
holding the core lock. It excludes waiting for that lock, pacing and subsequent
worker reporting. Those other costs and output continuity have separate evidence.
It is not GPU time, source/content latency or receiver/display presentation.

`realtimeEvidence.render.workDistribution` is `render-work-distribution-v1`.
Disabled observations contain no invented empty distribution. Enabled snapshots
carry process-lifetime counts in sparse, ordered `[bucket, count]` pairs, a 1,000 ns
bucket width, sample/invalid counts and raw steady-clock scan start/end strings.
Bucket 32,768 is overflow starting at 32.768 ms. A disabled collector allocates no
histogram. Scanning and JSON construction run on the requesting diagnostics thread,
not the render worker. No source quality, buffer setting or launch default changes.

The QA judge subtracts warmup-end counts from measured-end counts and rejects
unsupported geometry, resets, missing bins, malformed counters, invalid samples,
worker-generation changes, short intervals, excessive scan uncertainty or sample
accounting inconsistent with completed slots. Each endpoint must scan in less
than one 60 fps frame. The current scheduler permits three catch-up slots; the
judge conservatively allows six uncertain ranks per endpoint, including deadline
equality, an ordinary slot and the reporting handoff. Percentiles retain both
histogram-resolution and twelve-rank endpoint uncertainty. A percentile reaching
overflow has no finite upper bound and cannot qualify.

```powershell
python scripts/qa/monitor-isolation-ab.py --core C:/path/corevideo-native.exe `
  --fake C:/path/corevideo-zoom-engine-fake.exe --source-commit EXACT_COMMIT `
  --output artifacts/work-off --program-scene mixed --cpu-source-preparation 1 `
  --render-work-distribution --pairs 3 --duration 120 --warmup 15
python scripts/qa/monitor-isolation-ab.py --core C:/path/corevideo-native.exe `
  --fake C:/path/corevideo-zoom-engine-fake.exe --source-commit EXACT_COMMIT `
  --output artifacts/work-on --program-scene mixed --cpu-source-preparation 1 `
  --render-work-distribution --delivery-trace --pairs 3 --duration 120 --warmup 15
python scripts/qa/render-work-cost.py artifacts/work-off/report.json artifacts/work-on/report.json
```

Repeat with reversed condition order and retain every attempt. The read-only
comparison requires matching source/executable/driver/judge hashes, flags,
hardware, scene, formats, buffer setting, duration, warmup and complete paired
monitor trials. Output/recording failures or incomplete/lossy traces invalidate
the comparison. Compare the trace-on p95 **upper** bound with the trace-off p95
**lower** bound: regression must be strictly below 1%. A FAIL cannot be waived by
mean work, rounded percentiles or a later favorable trial. Normal logging remains
enabled. Individual percentile bounds and failed runs remain in the artifacts.

This closes only a measurement gap in #823. A timing-collector PASS is not the
trace-cost comparison PASS, and neither is #517 release qualification. Actual
content latency, camera/display correlation, A/V, real meetings, resources and
lower-tier hardware remain independent gates.
