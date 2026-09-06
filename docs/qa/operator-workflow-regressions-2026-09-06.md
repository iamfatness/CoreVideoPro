# Operator workflow regressions — 2026-09-06

The running Windows build repeatedly logged `preview-scene sync failed: Sequence contains no matching element`. Code tracing found that the scene picker's two-way selection could briefly clear Preview while its item list refreshed. Take accepted the invalid ID and could put it into Program, preventing subsequent scene/overlay synchronization. Existing unit-suite success did not cover this operator sequence.

## Changes and automated evidence

- Scene selection ignores temporary selection loss and restores the visible selection after item refresh. Take validates both scene IDs before mutation.
- Source, geometry, framing, effects, layer order and removal in a live-scene draft enable Take. Untouched drafts do not. SceneTakeRulesTests exercises these differences.
- Take swaps once, retries busy synchronization, blocks concurrent takes, and reconciles failed attempts without overwriting newer edits. TransportCoordinatorTests exercises invalid selections, retry, failure and concurrency.
- Failed lower-third phase synchronization settles desired state instead of replaying an animation forever. Operator status remains pending until the native overlay snapshot confirms the desired key. LowerThirdPhaseRecoveryTests checks generated native commands and confirmation matching.
- One-shot Magic Scene cues Preview. Manual selection or manual Take pauses automation. Automatic decisions cannot rewrite Preview while Take is pending, claim offline/failed success, or repeatedly override manual graphic settings. MagicSceneCoordinatorTests covers these sequences.
- Shell screen-share preference and panel threshold are honored when a native recommendation conflicts with those settings. AutomationPolicyTests covers the conflict cases.

Combined Release WinUI suite: **799 passed, 0 failed, 0 skipped**. No native source changed in this fix. The package includes the previous Scene Builder preview fix.

## Live acceptance still required

The existing running app/meeting was not stopped or operated during this repair. These rows are **not yet visually verified**:

| Sequence | Expected result |
| --- | --- |
| Change scenes repeatedly in Scene Builder, then Take | Selected scene stays valid; Program changes once; old Program returns to Preview. |
| Edit a source/position in a draft of the live scene, then Take | Preview shows the draft; Program stays stable until Take; Take enables for the edit. |
| Show/hide a lower third, change source, then show it again | Correct source/text, one build-in/out, no permanent animation or false on-air status. |
| Run Magic Scene in Manual mode | Recommendation cues Preview without changing Program or manual graphics. |
| Enable automation, then manually cue/Take | Automation pauses and preserves the operator's choice. |
| Change panel threshold/share preference | Recommendations follow the selected settings. |
| Switch source and revisit Scene Builder | Live composite resumes and dragging updates the live image. |

Use a rehearsal session for these checks before relying on the build for a show. Automated checks above are evidence of the repaired logic, not completion of this live acceptance matrix.
