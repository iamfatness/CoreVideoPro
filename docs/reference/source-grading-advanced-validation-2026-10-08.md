# Advanced source grading evidence — 2026-10-08

Evidence for #835 and PR #837. This describes the complete implemented Windows
workspace and the limits of its qualification. It does not grant release acceptance.
The installed beta `beta-2026-10-08-51d6281` was unchanged.

## Reviewable behavior

Basic/Advanced expansion preserves the grade and opens private draft editing.
The expanded workspace contains a native GPU source preview, histogram, waveform
and vectorscope, independent original/graded analysis taps, individual scope
expansion, numeric primaries, editable master/R/G/B curves, an ordered adjustment
stack and real `.cube` import. Enable/bypass, operation/global intensity,
duplicate/reorder/remove, complete-grade undo/redo, `.cvgrade` presets and clipboard
reuse are implemented. Apply waits for native control-state acceptance; conflicts
and source-identity changes retain the draft. Stable source grades persist and
restore as drafts for explicit confirmation. Compare changes the monitor only.
Raw ISO behavior is unchanged. CDL/CLF and LUT export are later compatibility
extensions, as the approved specification states.

The authoritative native grade is used before source composition, including
Program, Preview, multiview and Tiles; downstream Program outputs inherit it.
This routing implementation is distinct from external receiver validation.

## Functional verification

Release native and WinUI builds pass. The final shell build has zero errors and
473 existing warnings. The full MediaCore suite passes 2,359 tests; the full shell
suite passes 1,617 tests. Contract checks pass. The evidence judge passes 18 tests
and periodic-sampling regressions pass two tests.

A full native regression during implementation passed 1,550 tests. Later focused
verification after the final production edits passed five advanced math/state tests,
ten preview/controller/RPC tests and eleven protocol checks. These include actual
BGRA/I420 GPU preview-to-Program parity, full/limited BT.601/709 combinations,
held-frame edits, private-editor retirement, independent expected values for
exposure/curves/saturation/stack order/LUT axes and domain/intensity/bypass, and
actual exported scope pixels with independent original/graded taps. The earlier
full regression is not represented as a final-head full regression.

The real offscreen WinUI probe initializes compiled XAML bindings and verifies
both GPU surface hosts are active, editable curve geometry is bound, native apply
is acknowledged, scope selection/expansion works, comparison taps are independent,
undo/preset reload works and Basic/Advanced resizing has valid layout. Report:
`artifacts/grading-workspace-bound-probe.json`; layout image:
`artifacts/grading-workspace-bound-probe-layout.png`. WinUI RenderTargetBitmap
excludes SwapChainPanel pixels. The image is layout evidence; neither it nor the
probe establishes physical display presentation. An initial probe omitted binding
initialization and is retained as incomplete evidence, superseded by the bound probe.

macOS CI exposed a Metal shader compile error in the new scalar saturation
expression. It is corrected to an explicit float3 constructor. Metal pixel tests
now fail pipeline/shader initialization errors instead of silently skipping them;
only a genuinely missing Metal device can skip. A new Metal test checks advanced
primaries, curves, cube axes, bypass and I420 normalization against independent
expected pixels. Hosted results must be checked on the current PR head; successful
C++ compilation alone does not prove runtime Metal shader compilation.

Known-color GPU scope verification passes all six preview pixel tests, including
black, white, gray, RGB and a skin-colored patch. Independent Rec.709 equations
predict the histogram, waveform and vectorscope density locations in the actual
exported texture. The first test consumed the same one-publication keyed-mutex
texture repeatedly and failed its second read; the corrected test copies once
and samples all three scopes from that copy. Both logs are retained. This test
does not qualify arbitrary ramps, exhaustive clipping or a skin classifier.

## Matched synthetic delivery trials

Eight owned trials, each ten seconds warmup and sixty measured seconds. Two pairs
per workspace setting; pair two reverses order. Each uses eight synthetic Zoom
I420 1080p30 sources, nominal-60-Hz BGRA 1080p/1440p mappings, a selected-source
mixed 1080p60 Program, Preview/multiview and local Program MP4 recording. CPU source
preparation and a two-frame Program buffer are enabled. The advanced grade is
applied to native sources before measurement. Open-workspace trials edit three
private drafts at 2 Hz with all three scope types enabled. No simultaneous local
build/test GPU workload ran during these trials.

| Workspace | Trial | Program underruns | Recorder missing frames | Program/recording |
|---|---|---:|---:|---|
| Closed | pair-1-inline | 2 | 2 | FAIL/FAIL |
| Closed | pair-1-isolated | 0 | 0 | PASS/PASS |
| Closed | pair-2-isolated | 0 | 0 | PASS/PASS |
| Closed | pair-2-inline | 0 | 0 | PASS/PASS |
| Three open | pair-1-inline | 2 | 2 | FAIL/FAIL |
| Three open | pair-1-isolated | 0 | 0 | PASS/PASS |
| Three open | pair-2-isolated | 0 | 0 | PASS/PASS |
| Three open | pair-2-inline | 0 | 0 | PASS/PASS |

All isolated-monitoring trials pass the measured Program/recording boundaries.
The first inline trial in each setting fails; these failures remain failures.
This does not prove their cause, eliminate #517, or establish sustained 60-fps
acceptance. No trial recorded encoder queue drops or audio sample loss. Source
admission passed the quarter-second sampled identity/advancement check. Open trials
observed all three actual native scope exports advancing with zero invalid or
unavailable scope observations, matching epochs/revisions/sample metadata.

Production source commit for the measured Windows binary:
`d5c1f55cab44f39e862e723d8c56e6512b0cbd3e`. Later offscreen-probe, Metal shader and absent-internal-curve safety changes
are outside these measured trials. These are pinned implementation-stage trials,
not final-head performance qualification. Native binary SHA256:
`5a0b31cfc07a92146ea129e4e755be674fa94dfe897a9448adf24bb4f680b4f3`.
Harness SHA256: `7cd9dd68e70df31f9c46caaf82335487ce9bed02c2ad9bd1093d8666d6951872`.
Driver SHA256: `2dceb3a4a9e836cd86754318f8355d3eafeae42708017c3e8642786632ec9caf`.
Full manifests, snapshots, original failures, stderr and MP4s remain under
`artifacts/grading-advanced-closed-review` and `artifacts/grading-advanced-open-review`.
Inventory includes NVIDIA RTX 4090 driver 32.0.16.1742 plus virtual/USB display
adapters; the selected adapter was not independently qualified.

## Acceptance boundaries

Scopes sample 256×144 pixels at bounded optional refresh; they are not exhaustive
clipping counters. Rec.709 SDR is an explicit assumption for unknown transfer
metadata. Native apply acknowledgement is control-state acceptance, not GPU
resource preparation, first-frame completion or receiver presentation. Allocation
failure/device-loss rollback remains unqualified.

These tests do not cover actual Zoom acquisition, real webcam/stream receivers,
physical display/resize acceptance, decoded final-file A/V or a sustained fleet
soak. Per-frame identity, GPU readiness and those external boundaries remain
missing release evidence. Keep #835 open and PR #837 in draft for owner review;
no merge, release or installation is implied by this evidence.
