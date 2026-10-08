# Native source grading preview evidence — 2026-10-08

Evidence for #835 / PR #837. The installed beta was not modified or launched.
Advanced curves/scopes and physical operator acceptance remain outside this increment.

## Functional evidence

Release native core/tests and WinUI compile. Seven native grading tests pass:
actual D3D BGRA/I420 preview pixels agree with the production grade shader to
within one code value, a private draft leaves Program unchanged, a held source
can be regraded, editor exports are independent, another editor's revisions do
not prune a held editor's attribution, and final close retires retained inputs.
Controller tests cover capacity/lease expiry, unsupported status, revision/source
matching and stale detection. The RPC stub acknowledges and explicitly reports
unavailable. Eleven protocol parity checks pass after registering the command,
request, capability and event. The source grading capability is omitted on
unsupported adapters, including Metal in this increment.

The MediaCore suite passes 2,353 tests. The shell suite passes 1,610 tests,
including draft/compare isolation, revision rejection, shared-texture presentation
selection, coalescing and close ordering. The evidence judge has 18 passing tests;
two deterministic cadence/preparation regressions cover #838.

The initial full native run passed 1,539 tests and failed five: four missing
protocol registrations (repaired and focused-verified) and one missed production
slot in the no-destination 1080p60 encoder probe (0.83 ms worst lateness). A fresh,
isolated 10-second-per-rate repeat had zero missed production slots at 30 and
60 fps. Keep the earlier failure; the repeat does not establish its cause or an
unlimited guarantee. The final full regression result is recorded in PR #837.

## Mixed-source comparison

Native binary built from `31a764897885cb874bc2d501bdac00fb55c4d5d5`, Release,
SHA256 `7395f2bb02db32049df58813822cd858699ffc8aa66361772ec5f583a7a61580`.
QA sampling code is pinned separately by each manifest. Desktop inventory includes
an NVIDIA RTX 4090, driver 32.0.16.1742; selected adapter identity was not independently
verified. Each fresh owned core uses eight synthetic Zoom I420 1080p30 sources,
1080p and 1440p BGRA mappings at nominal 60 Hz, a mixed selected-source Program,
1080p60 Program/Preview/multiview, local Program MP4 recording, enabled CPU source
preparation, and the same two-frame Program buffer. Five seconds warmup and thirty
measured seconds per trial, one pair per monitor mode. Normal logging; no delivery
trace, camera receiver, real SDK, physical acquisition or display reader.

Corrected-cadence trials:

| Monitoring | Grading editors | Program underruns | Recording missing frames | Sampled source advancement | Mean render CPU work |
|---|---:|---:|---:|---|---:|
| Inline | 0 | 82 | 82 | Pass | 4.593 ms |
| Inline | 3 | 2 | 2 | Pass | 3.590 ms |
| Isolated | 0 | 0 | 0 | Pass | 1.074 ms |
| Isolated | 3 | 0 | 0 | Pass | 1.100 ms |

All four trials had zero Program-buffer scheduled deadline misses/output sequence
gaps, encoder queue drops, audio lost samples and recorder failures. The inline
underruns nevertheless fail acceptance. Do not infer an improvement from the
variable inline counts or declare grading responsible for a closed-editor failure.
Both isolated trials pass these sampled boundaries. Three-editor trials retained
all three requested inputs, completed 319/321 grading jobs with no worker failure,
and kept the pending queue bounded. A single short pair does not meet sustained,
repeatability, output-receiver or fleet qualification.

Earlier manifests and failures are also retained. An original closed isolated
trial's source-advancement failure was caused by duplicate initial snapshots
about 3.5 ms apart, at elapsed 0.0: requested screen frame advanced 583 to 587
while the completed frame correctly held 583 pending preparation. #838 changes
sampling to respect the declared 250 ms cadence and reuses the final sampled
snapshot. Source identity and advancement assertions are unchanged. The corrected
closed/open trials all pass that judge.

Local evidence under `artifacts/`: `grading-preview-native-full.log`,
`grading-preview-native-final-full.log`, `grading-preview-rate-qualified.log`,
`grading-preview-contract-qualified.log`, `grading-preview-focused-qualified.log`,
`grading-preview-mediacore-qualified.log`, `grading-preview-winui-full.log`,
`grading-preview-closed-31a7648/`, `grading-preview-open-31a7648/`,
`grading-preview-closed-sampled/`, and `grading-preview-open-sampled/`. Reports pin
binary/harness hashes, flags, hardware inventory and snapshots; recordings and
stderr survive failed verdicts. These local artifacts are not committed.

## Acceptance limits

Keep #835 open and the PR in draft. Physical WinUI presentation/resize, real Zoom,
webcam/stream receivers, decoded A/V and sustained qualification remain unverified.
The native preview is a bounded 10 Hz monitor at original source resolution;
configured production cadence/resolution is unchanged. Basic defaults to labeled
live editing; draft mode and original comparison remain private. Existing Basic
apply acknowledgement/rollback limitations remain explicit in the specification.
The inline delivery failure is recorded against parent #517; this change does not
alter monitor defaults or add production buffering to conceal it.
