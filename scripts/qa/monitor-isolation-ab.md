# Mixed-source monitor isolation evidence (#794, parent #517)

This Windows headless test compares the existing inline and isolated monitor
paths using one **Release** binary. It creates eight fake Zoom I420 1080p30
sources, two BGRA shared-memory captures (1080p/1440p at nominal 60 Hz), a
1080p60 Program and Preview, a 1080p multiview, and local Program recording.
It explicitly selects the two-frame Program buffer and disables GPU capture
ingress and CPU source GPU preparation so that only monitor isolation differs. The capture producer advances
a header/pixel counter on a solid image; it does not exercise physical camera
or Windows Graphics Capture acquisition.

```powershell
python scripts/qa/monitor-isolation-ab.py `
  --core native/build-dev/corevideo-native.exe `
  --fake native/build-dev/corevideo-zoom-engine-fake.exe `
  --source-commit <commit-that-built-these-executables> `
  --output artifacts/monitor-ab-unique-run
```

Defaults: three pairs, two measured minutes per trial, ten seconds of warmup.
`--cpu-source-preparation 1` requests optional SHM BGRA and decoded Zoom I420 preparation;
its value stays constant across both monitor modes and is pinned in the manifest.
The default `0` overrides any inherited flag. Neither a requested flag nor
aggregate Program progress proves that every selected source used a ready GPU
view; pixel, preparation-health and latency evidence remain separate requirements.
Pair orders alternate inline/isolated then isolated/inline. Every trial gets a
fresh owned core and unique Local shared-memory mappings. Do not run a build,
other QA workload or live production concurrently. The source commit and
Release configuration are operator declarations; SHA256 hashes identify the
actual binaries, and the manifest inventories available adapters/drivers.
Adapter inventory does not prove which adapter the compositor selected.

`--program-scene capture` selects the 2560x1440 BGRA mapping alone;
`--program-scene mixed` selects that mapping and two 1080p I420 guests in
three equal-width regions. The default `zoom` keeps the original single guest.
Run identical scenes with preparation `0` and `1` to compare preparation
while comparing like monitor modes. Snapshots retain `programSourceAdmission`
for requested/actual identities and explicit ready/held/unavailable state;
periodic observations do not prove every rendered frame or display presentation.

The output directory must be new. Each trial retains its snapshots as JSONL,
stderr, result JSON and recording even on failure. RAM retains two snapshots,
one pending IPC request/response and no event backlog; stderr drains directly
to disk. The driver closes stdin and waits for its owned process to exit,
killing that process only on shutdown timeout. It never edits settings,
restarts the installed app or searches for unrelated processes to kill.
Evidence on disk grows with the requested duration; retain it until reviewed.

`programBufferVerdict` checks measured deltas for underruns, scheduled deadline
misses, output sequence gaps, skipped production slots and audio sample loss.
Render overruns absorbed by the buffer remain visible without being relabeled
as output loss. `recordingVerdict` separately checks measured recorder missing
frames, encoder video/audio queue drops and failures. Any missing/reset
counter, stopped recording, inactive buffer, changed profile/generation or
non-progressing worker invalidates the trial. Nonzero measured loss fails its
boundary. Exit 1 means at least one trial failed or is invalid; a failing inline
control is useful evidence and must not be discarded. Short smoke tests may
override duration/pairs but do not meet the specification's repeatability gate.

When preparation is enabled, `sampledSourceAdmissionVerdict` independently
requires every selected reference source to have ready/held, nonfuture,
same-epoch actual identities that advance with new CPU arrivals at the
quarter-second sample boundaries. Missing evidence and a frozen actual image
fail even if Program keeps delivering slate frames without a buffer underrun.
The preparation-off control reports NOT_REQUESTED. This is sampled source
evidence, not every-frame source continuity or physical presentation proof.

Both verdicts remain aggregate evidence. Advancing texture metadata and a
playable MP4 do not establish per-frame identity, actual display presentation,
GPU readiness, decoded A/V skew or camera receiver delivery. Release
qualification remains **MISSING_EVIDENCE**. Real SDK/capture, fault injection,
resource bounds, installed soak and hardware coverage remain separate gates
in the [approved specification](../../docs/reference/render-delivery-spec.md).

The portable judge/IPC tests run in CI without a GPU:

For an independent CPU p95 measurement, enable `--render-work-distribution` in
both trace-on and trace-off runs. The conservative matched comparison is
documented in [render-work-evidence.md](../../docs/reference/render-work-evidence.md).
Its result does not certify GPU completion or release qualification.

```powershell
python -m unittest discover -s scripts/qa -p test_monitor_evidence.py -v
```

`--grade-previews 1|2|3` adds private native warm-film draft monitors, with
2 Hz lease refresh, without applying their grades to Program. Three selects two
Zoom sources and the BGRA screen mapping. The manifest pins the count; snapshots
retain measured `gradePreview` worker facts. Run otherwise identical trials with
`0` and `3` to compare the overhead. Worker completion is required, but does not
prove preview pixels or shell presentation; the native pixel tests cover the
former and an operator window/resize soak must cover the latter.
