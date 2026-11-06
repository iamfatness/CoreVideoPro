# Resumed reliability work — November 6, 2026

This document specifies work; docs/BACKLOG.md is the sole ranked queue. The owner authorized another eight hours and replenishment after early batch completion. Evidence root: D:/CoreVideo-QA/resume-20261106. Baseline main: ad8559b2. Do not infer a new defect merely from an older issue title.

## Auto — #1146

Problem: the retained 15e1aa3b real-meeting trial stayed on interview for 60 seconds despite native panel recommendations. Three binding-key spans were stable for 20.345, 11.695 and 25.940 seconds. The capture omitted the shell's resolved recommendation, hold reason, CanTake and binding result. Later success is not an explanation.

Specification: observe one coherent UI-thread snapshot of Auto mode, native and resolved recommendation/policy, exact binding identities, configured confidence/timing, current Preview/Program, readiness, last decision and transport outcome. Reuse existing local control/state diagnostics and focused coordinator types; do not add behavior to StudioViewModel.cs. Expose bounded read-only evidence without meeting URLs, credentials or arbitrary payloads. Do not alter policy thresholds simply to obtain a Take.

Execution: map the real state/control consumer; add a tested additive diagnostic if the required state is absent; package a shell against the unchanged qualified native core; run the retained sequence in the authorized meeting. Identify the first diverging boundary and reproduce it deterministically before a behavioral patch. Exercise source/policy mismatch, unavailable Preview, same-scene person replacement, rejected Take, manual override and session recovery. Keep failed attempts and mutation/red evidence. Run focused/full changed-component tests and compiled/live checks. A resolved observed hold may be legitimate, but must be explained and actionable rather than reported as success.

Acceptance: either native-selected people reach Preview and confirmed Program after continuous eligibility/readiness, or a precise truthful hold is observed. Manual selection pauses Auto; failed or stale-session Takes cannot masquerade as success; captions remain unavailable. Keep the broader scripted host/guest/share and webcam matrix explicitly pending wherever real controls or matching camera evidence are unavailable.

## Received geometry — #1171

Specification: trace requested and acknowledged subscription geometry, actual callback/ingest geometry, generations, relevant demand and recording state in the same monotonic interval. Retain original source pixels/quality. No inference of congestion from simultaneous changes alone.

Execution: inspect request/SDK mapping and retained eight-feed transitions, then compare a controlled idle-to-record transition with the actual available senders. Add focused diagnostic/regression only at an identified missing boundary. Verify an application-side request or adaptation defect before changing it; otherwise document the exact external account/sender prerequisite and move to independent work.

Acceptance: explain/reproduce the geometry transition with evidence; any fix preserves 1080p requests and actual received quality. Full eight-feed delivery remains unqualified until the declared workload holds actual geometry and per-frame output identities.

## Empty Studio state — #1172

Specification: one readable prioritized multiview state at compact and ordinary sizes; renderer errors stay visible, and instructions do not cover live video. Fix the actual combined state owner rather than hiding text indiscriminately.

Execution: reproduce the captured 1360x768 overlapping assignment/preparation labels; assert combined visibility/layout precedence, make a focused UI change, and inspect new compact/normal compiled captures.

Acceptance: no overlapping empty-state labels; loading, empty, failure and live states retain useful meaning. Automated UI evidence is not owner usability acceptance.

## Deferred participant-list work — #919

Specification: queued clear/rebind callbacks must not touch XAML after unload or act on a later window lifetime. Preserve live roster coalescing and responsiveness; no blanket exception swallowing as the fix.

Execution: reproduce pending clear/rebind at teardown, add a focused generation/lifetime fence and regression for both phases, then run the compiled workspace teardown probe. Capture the original caught COMException separately from an unhandled crash.

Acceptance: no stale clear or rebind after unload; a new lifetime still refreshes correctly; normal roster updates remain functional.

Implementation contract: a focused UI-thread scheduler owns the active generation and coalesces the entire clear/rebind pair. Both callbacks validate their captured generation before touching the ListView; an old callback cannot clear a new lifetime's pending flag. Unload invalidates the generation before unsubscribing. Load reconnects the current ViewModel and schedules a fresh bind. Rebind resolves the latest roster, and a reentrant notification during binding schedules one further refresh. Exceptions still reach the existing tagged dispatcher logger, while the current pending flag is reset so later updates can recover. A dispatcher refusal during shutdown is harmless; a later lifetime starts with a fresh pending flag.

Validation: deterministic queued-phase tests cover unload before clear and before rebind, old callbacks draining after reload, coalescing/latest roster, reentrant updates, teardown during clear, visible failures and dispatcher shutdown. The compiled workspace probe alternates immediate and queued detachment over six reload cycles, verifies the unloaded list is untouched, and checks both latest-roster binding and live ViewModel notifications after reload. Run the compiled probe against the old implementation as a negative control, then against the fixed implementation. No local build or UI probe overlaps measured live media.

## Tiles — #1074

Specification: retain the current four-second held-slot behavior, immediate explicit video-off/departure handling and truthful picture/black labels. Do not retune the unresolved owner policy or claim a network cause from symptoms.

Execution: check merged runtime behavior and current tests first; use actual controllable interruptions if available, otherwise a clearly labeled production-runtime fixture. Test short hold, long gap, recovery, video-off/departure and local static sources. Only patch a reproduced discrepancy.

Acceptance: slot identity and actual render/observation agree; unavailable live interruption evidence remains explicit.

## Output qualification — #443 / #989 / #1056

Specification: same matched candidate, actual sources, registered camera identity and independent camera-bounded interval. Program, every ISO and stream retain exact identities under the existing held-frame exception. No judge relaxation.

Execution: inspect prerequisites before a long run. If all are available, collect the full 30-minute workload with no overlapping local build/probe, finalize and decode. If not, spend the allocation on the next actionable queue item rather than repeat an unbounded run and call it qualification. Restore the current session's backed-up profile only after owned processes exit.

## Estimation and completion discipline

Each item records estimated active minutes, actual investigation/edit minutes, local validation minutes, hosted wait minutes used for independent work, and remaining external dependency. Reforecast on evidence. Preserve time for safe finalization/restoration at the window end. Completing or merging an item triggers selection of the next item, not goal completion. Replenish from the open backlog if these scoped items finish early.
