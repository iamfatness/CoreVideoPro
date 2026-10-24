# RC batch 2 — index of specs, plans and executors

**Date:** 2026-10-23 (evening, after the six 2026-10-23 merges)
**Owner direction:** "cut a beta so I can test current state vs the next batch of work"; "as close to zero issues as possible".
**Decisions page:** `docs/superpowers/specs/2026-10-23-rc-owner-decisions.md` (22 rulings, 1 standing fact and 8 external inputs, each with a recommendation).

Every item below has a design spec under `docs/superpowers/specs/` and a task plan under `docs/superpowers/plans/` in the stream-backpressure format: header, Global Constraints, Review Focus, tasks with Files, Interfaces and one-action steps, tests first, and a per-task **Recommended executor** table (model and effort). Hours are AI-assisted engineering estimates from each plan, not human-only.

## Order of work

| # | Item | Spec / plan (date prefix `2026-10-23-`) | Hours | Starts when | Needs the owner for |
|---|---|---|---|---|---|
| 0 | RC beta acceptance soak (gate + installed acceptance of every 2026-10-23 PR) | `rc-beta-acceptance-soak` | about 1.5 owner-days wall clock | #1038 merged | the test meeting with 2 helpers; the real show run |
| 1 | #781 Frame Server serves an old camera DLL after upgrade | `frame-server-dll-refresh` | 9.75 | now | Task 1 probe and Task 7 upgrade on the dev box |
| 2 | #935 ISO software-encoder notice fails the drill / invisible to the operator | `iso-capacity-warning` | 12 | now | D8, D9; Task 8 30-min drill |
| 3 | #1037 render tick takes the engine lock under coreMutex (14 sites) | `render-gather-engine-mutex` | 10 | now | D17 |
| 4 | #883 installer cleanup: Get-FileHash fixed, 29 stale folders remain | `installer-cleanup-883` | 8.5 | now | D4, D5; Task 7 real upgrade |
| 5 | #457 shell crash during forced stop (triage: it was the #462 drain crash) | `shell-forced-stop-crash-triage` | 4.5 | now | D12 (close on evidence) |
| 6 | #993 camera disconnect blocks the command lane (bounded release, reopen fence, liveness fields) | `capture-readsample-lane-isolation` | 21.25 | after the soak verdict | D11; Task 9 physical unplug |
| 7 | #562 + #758 capture dropout policy on adapter liveness | `capture-dropout-policy` | 15.75 | after #993 Task 5 (shared `UvcCaptureDeviceAdapter` regions); acceptance A1 needs #993 merged | D18, D19, D20; Task 9 replug |
| 8 | #856 Zoom helper exits with no cause recorded | `zoom-helper-exit-diagnostics` | 31 | after the soak verdict | D13 to D16; Task 10 installed drill |
| 9 | #445 engine-off teardown audit against the five ZoomISO rules | `engine-off-teardown-audit` | 14.5 | after the soak verdict | D10 (confirm rules 4 and 5) |
| | **Total** | | **about 127 h** plus the soak | | |

Items 1 to 5 are independent of each other and of the soak; they can run as five parallel lanes tomorrow morning in `D:\CoreVideo-QA\wt-<n>` worktrees while the beta is cut and installed. Items 6 to 9 wait for the soak verdict because a gate FAIL would reorder them. Item 7 consumes item 6's liveness fields (exposed by #993 Task 6) and shares `UvcCaptureDeviceAdapter` regions with it: #758 Task 4 must follow #993 Task 5, and its acceptance A1 needs #993 merged.

**Installed-acceptance serialization.** The installed-acceptance tasks of items 1 to 5 (#781 Task 7, #883 Task 7, #935 Task 8 step 4) must be serialized **after soak Task 5** (the owner's show run on the installed beta), or run against a separate `-InstallRoot`; none may install over the owner's beta while the soak is in progress. Each is an owner-run elevated step in an owner-scheduled window (standing fact: UAC on the dev PC has EnableLUA=1 with admin prompts suppressed; plans must also work on a normal PC).

**Soak additions.** The soak plan also carries the #457 graceful-close gate (Task 5 Step 3, `scripts/qa/graceful-close-check.ps1`, if the #457 lane has merged) and the #1039 observation (Task 4 Step 2b: after the deliberate dump the crash offer and support bundle must list it; expected FAIL until #1039 ships).

New issues filed during planning: #1037 (render gather lock), #1039 (shell crash scanner and support bundle ignore the installer's WER folder; small, should ride with the soak's #433 acceptance).

## Executor summary

Models by task count across the ten plans (the per-task tables are authoritative):

| Model | Use | Count |
|---|---|---|
| fable | installed or live acceptance on the owner's box where a false pass is expensive (#781 T7, #935 T8, #993 T9, #856 T5 crash trap and T10), the lock-across-SDK-call rule in the audit (#445 T3), dump reading if the #457 dump is ever found | 7 |
| opus | concurrency, lock ordering, COM lifetime, release-gate rule changes, live drills that need real-time judgement, every once-per-plan review | about 30 |
| sonnet | table-driven pure code, scripts and harness work, shell readouts, measurements with fixed expected outputs, docs assembly | about 30 |
| haiku | mechanical doc edits from a settled spec, trivial scope wrappers | 6 |

Effort: `high` on any task that touches the on-air path, a lock, a gate rule or a device; `medium` for pure code with specified tests; `low` only for docs and mechanical edits. One opus review per plan after its last task, re-deriving the Review Focus lines.

## Findings from the planning pass that changed the picture

- #935: the shell-side cancel was fixed on 2026-10-17 (commit 935b2800); what remains is the validator, the drill rule, operator visibility, and a suspected start-before-probe race on the 4090.
- #457: the dump and its build are gone; Event Log, error-report signature and the shell log show a graceful close and the #462 drain crash, with seven clean in-meeting closes since.
- #883: Get-FileHash was replaced on 2026-10-13; 29 metadata-only folders and 2 with stranded recordings remain on the dev box.
- #993: PR #994 already made the read asynchronous; the lane still joins the worker with no bound during disconnect.
- #1037: the render tick budget is 8 ms, not sub-ms; 14 sites take the engine lock under coreMutex, the worst being three participant-list copies per tick for the active speaker.
- #856: Zoom's own crash handler is enabled by us, so helper crashes never reach WER; the shell scans the wrong dump folder on installed builds (#1039).
- #445: the five rules existed only as a one-line parenthetical; rules 4 and 5 are reconstructed and need the owner's confirmation.
- #758: half fixed by #791 (unregister exists but only the native-takeover path calls it); the native camera path freezes too.

## Execution method

Subagent-driven per plan: a fresh agent per task at the model and effort in the table, a fresh reviewer per PR, the same reviewer re-checking the fixed head, merge only on the owner's word. That is the method that landed six PRs on 2026-10-23 and caught the stream-tick timeline defect before it shipped.
