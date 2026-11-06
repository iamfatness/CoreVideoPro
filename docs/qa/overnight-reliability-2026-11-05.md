# Overnight reliability: acceptance specifications and execution plans

Execution order and budgets live only in [BACKLOG](../BACKLOG.md). Owner authorized execution November 5 at 21:54 Eastern, including local app/device rehearsals, with the supplied test meeting when populated and local fallback otherwise. Eight-hour endpoint: November 6 at 05:54 Eastern. If the batch ends before 06:00, perform a new evidence-based issue triage and create a four-hour follow-on specification/plan. Do not repeat completed work or relabel blocked provider qualification as complete.

## Defect plan index

| Issue | Specification and method |
|---|---|
| #1143 | [ISO late-frame admission, placement and decoded accounting](../reference/iso-late-frame-1143-spec-plan.md) |
| #1158 | [Graphics cache charged leases and second-asset demand](../reference/graphics-cache-1158-spec-plan.md) |
| #1157 | [Private preview request acceptance and actual pixels](../reference/graphics-preview-1157-spec-plan.md) |
| #1137 | [Consistent monitor retirement observation](../reference/monitor-retirement-1137-spec-plan.md) |

## Baseline and evidence reconciliation

**Specification:** Every result names a source SHA, executable identity, test conditions and evidence location. A running recording/stream or another task's hardware test is never silently interrupted. Installed settings, registered camera path and previous runnable build are recoverable.

**Plan:** Record git/CI/PR state; identify app/core/QA processes and their paths; inspect control state without mutation. Preserve configuration before any rehearsal changes. Check the provided meeting only through the supported app path, keeping credentials in local private storage. Detect usable remote video and actual received geometry; a requested size is not proof. If another test owns GPU/encoder resources, do documentation/source analysis until it ends, and do not kill it. Reconcile #1133 against merged #1132 and the recorded final-head CI; close only when the specific test-only acceptance is proven. Treat #618/#551 as evidence review candidates, not automatic closures.

**Acceptance:** Baseline ledger retained, no secret copied into git/log reports, no concurrent GPU test, rollback/build identity known. Unavailable resources are explicitly recorded.

## Integrated candidate build and regression

**Specification:** Native and shell artifacts come from the same selected integrated source; D3D11 and intended real adapters are enabled. No uncommitted implementation or stale copied executable can masquerade as the candidate.

**Plan:** Use a clean isolated worktree or a traceable committed integration branch. Verify each issue patch independently, then build production Release via `scripts/build-native-dev.ps1` with the private SDK, publish self-contained WinUI with the exact source revision, and build the show engine. Run the full native suite once after final source changes, appropriate managed suites and compiled runtime/Graphics/layout probes. Run exact-source hosted checks. No performance run overlaps local builds/probes. Preserve original failures; additional runs need a new diagnostic or causal change. Merge only reviewed changes whose necessary gates pass; leave unrelated findings open. Never force a green result by disabling a test.

**Acceptance:** Exact hashes, real test counts and exit codes, retained failure/green comparison, bundled runtime provenance and native production flags. Code-only success is not installed output success. A missed gate leaves the candidate unqualified.

## Auto rehearsal — #1146

**Specification:** Follow [speaker-floor director spec](../speaker-floor-director-spec.md) and [Auto evidence/scenarios](auto-director-policy-2026-11-05.md). A recommendation must carry unique valid guest bindings and matching operator policy. Auto captions stay unavailable; explicit caption cues remain independent. Native eligibility, person/scene hold, Preview warmup and confirmed Take are separate observed milestones. Manual takeover stops Auto; late old-session completion cannot overwrite the new session.

**Plan:** Use host plus three real guests when available. Script sustained turns, short backchannels, simultaneous talk, silence, solo/interview/panel changes, sharing, preference changes, camera off/rejoin and manual Preview/Take. Record intended shot and actual Program/webcam person identity at each stimulus, plus elapsed eligibility-to-Take. Exercise a failed same-scene Take via a controlled test seam/API refusal; never deliberately crash the production show to manufacture one. With no remote participants, run deterministic integrated native/shell fixtures and label those as simulated policy evidence only.

**Acceptance:** No duplicate person, unintended host substitution, nonexistent captions, stale reconnect cut or false Take success. A wrong shot is retained with source state/reason/timing. Mark human directing quality and real remote behavior unverified when no populated meeting exists.

## Device and Tiles rehearsal — #758/#993/#1074

**Specification:** Source disconnect cannot indefinitely block commands. A stopped bridge cannot keep manufacturing fresh frames. Dropout behavior follows the configured source policy. Tiles hold short Zoom gaps with the currently implemented four-second window and measured held status; video-off/departure remove immediately. Four seconds remains a recorded working assumption, not a new policy ruling.

**Plan:** Inventory connected sources and output registration. Use a test source and isolated scene/profile. Observe genuine hardware unplug/replug only when it can be controlled safely; software disconnect/reconnect is a separately labeled test, not physical acceptance. Measure command responsiveness, source age/generation and Program/OS-camera identity before, during and after. For remote gaps use controlled source/feed interruption, not disabling the machine's network during other sessions. Cover 1–3-second stalls, beyond-window stall, video-off/departure and return. Keep frame/source status evidence and restore the original configuration.

**Acceptance:** No command-lane hang, stale-generation delivery or falsely fresh frozen frame. Slots do not reflow during the implemented short hold; held status appears; explicit departure is honored. No claim that correlated stalls prove upload congestion. Physical hotplug remains unverified if it was not physically exercised.

## Graphics operator rehearsal — #1076/#1157/#1158

**Specification:** Private Cue never changes Program. Explicit Show/Hide target the intended graphic and have visible corresponding controls. The current builder's text and final inspector fields are accessible. The four owner tasks remain: name a person, edit text/style, add logo/title, identify/clear live versus cued graphics.

**Plan:** Use synthetic local content and an isolated show. Exercise missing/valid private-preview requests and lease-release/re-preparation through relevant compiled probes and actual textures; then inspect rendered Program where available. Record action count/time for the four tasks and matching Hide/Clear. Use current opaque screenshot capture; RenderTargetBitmap does not qualify native video. If owner-at-keyboard timing cannot be performed, record agent technical workflow only. No public Twitch/YouTube posts or live-provider moderation claims from fixtures.

**Acceptance:** Complete text/controls, intended graphic identity, private isolation, actual Show/Hide, no silent preparation refusal. Owner usability acceptance requires owner performance; no test-only proxy closes it.

## Full-output workload — #517/#823/#1056/#989

**Specification:** Apply current `CLAUDE.md` outputs-only gate and exact held/tail-frame exception. Program, every selected ISO, registered OS camera and configured stream must preserve expected identities without pipeline loss, unexplained duplicates or reorder. Desktop UX is judged separately: no hold over 100 ms, omissions below 1%, no tearing/reorder. Use the user's real mixed-refresh desktop; no monitor/driver changes to manufacture a pass.

**Plan:** Use `scripts/qa/outputs-drill.ps1` and existing `source-picture-verdict.py` / `iso-pixel-verdict.py` evidence. Confirm eight remote feeds plus webcam and moving screen with actual geometry before calling a run full workload. Warm sources, then run all outputs simultaneously for 30 minutes without local build/probe contention. Stream to a local controlled receiver unless a public endpoint is already explicitly authorized. Finalize every recording; judge actual source identities and held-frame counts, not average FPS or counters alone. Retain render/command lock and source-arrival diagnostics. If required remote content is absent, use a generated-source local recording/stream/camera trial and clearly identify its reduced load and duration.

**Acceptance:** Per-output verdicts and exact duration/hardware/build/source inventory. Any unexplained loss fails that row; a partial run never becomes full-load/Zoom acceptance. Hour-scale idle failure #513 is not closed by a 30-minute pass. Stop abnormal workloads safely, preserve evidence and restore the agreed state.

## Handoff, release and conditional next batch

**Specification:** Issue status reflects demonstrated results; original failures remain traceable. A release needs exact-source build/tests/package checks. No automatically published beta is implied by this execution request alone.

**Plan:** Write one execution ledger linking each issue's PR, evidence and remaining condition. Record stable/failed build and restore paths. Update issue comments/close only satisfied acceptance. At the first batch's end, read Eastern time. If before November 6 06:00, re-enumerate open issues, account for new findings and completed fixes, and produce a new four-hour backlog/spec/plan with realistic dependencies. Prioritize reproducible remaining defects and acceptance; do not invent features just to consume time. Preserve at most three active items and use the same no-secret/no-loss standards.

**Acceptance:** A self-contained completion report, exact unfinished dependencies and a checked four-hour follow-on plan when the condition applies. Time limits constrain scope, not truthfulness.
