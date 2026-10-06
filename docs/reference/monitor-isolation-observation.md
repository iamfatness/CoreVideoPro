# Monitor isolation observation (#796, parent #517)

`realtimeEvidence.monitorWorker` retains its existing counters and `enabled`
field. `enabled` means a worker exists; it does not prove device readiness,
successful composition, GPU completion or display presentation.

The additive `observationVersion: monitor-isolation-v1` fields are:

| Field | Meaning |
|---|---|
| requestedMode | Startup selection: inline, isolated, invalid or unknown |
| effectiveMode | Actual compositor branch: inline, isolated or unknown |
| selectionSource | default, override, constructor (internal/test) or unknown |
| readiness | starting, ready, degraded or unavailable |
| failureReason | Fixed diagnostic code; never an exception message |
| deliveryEpoch | Existing Program-buffer generation; not a monitor frame identity |

The D3D factory preserves current startup behavior: unset selects inline by
default, explicit `COREVIDEO_ISOLATE_MONITORS=0` selects inline, `1` selects
isolated. Other override values retain inline behavior but are reported as
invalid with `invalid-monitor-override`. There is no hot toggle or default change.
Unsupported compositors report unknown mode and unavailable readiness.

An isolated worker starts as `starting`, becomes `ready` after initialization,
and becomes `degraded` on initialization, rendering or frame-admission failure.
The fixed reasons are `monitor-initialization`, `monitor-render` and
`monitor-frame-admission`. A later successful render restores current readiness
and clears the reason, while cumulative failure counters remain unchanged.
Failure retains the isolated branch and its last completed image; it never
silently borrows Program's immediate context. Ready means backend initialization
or work succeeded, not that the last image is currently fresh.

The shell's focused `MonitorIsolationObservation` reads the existing generated
`CoreObservationModel.RealtimeEvidence` node on support export. It allowlists
mode/state/reason values and emits both structured bundle data and a triage
line. No additional polling, snapshot channel or StudioViewModel state is added.
Missing, older or malformed evidence remains unknown/unavailable; in particular,
an older peer's `enabled: true` cannot imply readiness. Free-text failure strings
are withheld from this view.

These observations support investigation. They do not satisfy the display,
per-frame delivery, camera receiver or installed qualification gates in the
[render specification](render-delivery-spec.md).
