# CoreVideo Pro — ranked backlog

**This is the only ordered list of work.** Status lives on the linked GitHub
issue (`backlog` label). How agents must treat this file:
[`AGENTS.md`](../AGENTS.md).

Owner-approved as the single list on 2026-09-10. Reconciled 2026-09-17: intake
from architecture review + completion-plan F1 was not on this file; those items
are now issues #535–#540 and sit in **Later** until the owner promotes them.
**MXL is not Now** (#539).

`docs/FOCUS_PLAN.md`, `docs/beta-plan.md`, and
`docs/native-production-completion-plan.md` are rationale or build specs.
Do not trust their checkboxes.

## How to use this file

1. Work the **Now** sequence. Do not pick from Later because it is interesting.
2. New finding → GitHub issue the same day → row here → owner ranks.
3. Max 3 Now items in flight. A PR names one issue.
4. Move a row to **Done** only when the issue is closed by a PR or an owner
   “won’t do.”

Rank clauses (beta = 5–20 external operators on machines we cannot see):

1. Stops a show breaking on an unseen machine
2. Lets us diagnose a failure nobody watched
3. Removes a papercut during a real show
4. Required for the first external install

Sizes: XS < 1 h, S ≈ 1–3 h, M ≈ half to one day, L > one day (agent time).

---

## Now — owner approved 2026-09-14

ISO recorder work shipped in PR #531. Next implementation order:

| Order | Issue | Item | Note |
|---|---|---|---|
| 1 | [#513](https://github.com/iamfatness/CoreVideoPro/issues/513) | Idle XAML crash / lifetime hardening | First change cut meter-control recreation. Does **not** close the fail-fast. Evidence: `docs/xaml-lifetime-hardening-2026-09-14.md` |
| 2 | [#516](https://github.com/iamfatness/CoreVideoPro/issues/516) | Command handler held `coreMutex` 143 ms | Freezes the render thread |
| 3 | [#532](https://github.com/iamfatness/CoreVideoPro/issues/532) | RTMP failed near startup and recovered once | Cause unverified; keep separate from ISO throughput |
| 4 | [#456](https://github.com/iamfatness/CoreVideoPro/issues/456) | Media In/Out points | Needs concrete UI design review before implementation |

Unblock #449 step 1 (hold outgoing picture on a plain cut) only after an owner
ruling — it changes Take, not just the cue decoder.

---

## Unranked observations (need an owner slot)

Found 2026-09-14 during ISO/stream validation. Not in Now until ranked.

| Issue | Item |
|---|---|
| [#526](https://github.com/iamfatness/CoreVideoPro/issues/526) | Intermittent Program-buffer delivery failures (startup path improved in #528; steady-state remains) |
| [#529](https://github.com/iamfatness/CoreVideoPro/issues/529) | ISO queue drops video while streaming 1080p60, including a stationary 8-person wall |
| [#530](https://github.com/iamfatness/CoreVideoPro/issues/530) | Control API advertises `programPreview`, implemented name is `program-preview` |
| [#533](https://github.com/iamfatness/CoreVideoPro/issues/533) | ISO fMP4 video timeline shorter than audio despite complete writer counters |
| [#521](https://github.com/iamfatness/CoreVideoPro/issues/521) | GPU-direct encode (slice 1 landed #523; leftover slices #524 #525) |
| [#518](https://github.com/iamfatness/CoreVideoPro/issues/518) | Verify Zoom audio on air — fader law drops `zoom-mix` as unrouted |
| [#517](https://github.com/iamfatness/CoreVideoPro/issues/517) | Program render hits one-frame budget at 16 participants + 1080p wall |
| [#519](https://github.com/iamfatness/CoreVideoPro/issues/519) | RTMP `bytesSent` / `latencyMs` are estimated and mislead diagnosis |
| [#509](https://github.com/iamfatness/CoreVideoPro/issues/509) | Operator stutter: participant structure signature + 197 ms rebuild |
| [#508](https://github.com/iamfatness/CoreVideoPro/issues/508) | Multiview labels/hit targets misalign after unassign |
| [#507](https://github.com/iamfatness/CoreVideoPro/issues/507) | Debug text still in operator UI, shifts transport buttons |

---

## Tier 0 — owner actions (calendar clocks)

| ID | Issue | Item | Clause |
|---|---|---|---|
| T0.1 | [#423](https://github.com/iamfatness/CoreVideoPro/issues/423) | Buy the code-signing certificate | 4 |
| T0.2 | [#424](https://github.com/iamfatness/CoreVideoPro/issues/424) | Zoom SDK redistribution answer on the record | 4 |
| T0.3 | [#425](https://github.com/iamfatness/CoreVideoPro/issues/425) | 2–3 reference machines (mid GPU, hybrid laptop, AMD) | 1 |
| T0.4 | [#426](https://github.com/iamfatness/CoreVideoPro/issues/426) | Tester feedback channel | 3 |
| T0.5 | [#427](https://github.com/iamfatness/CoreVideoPro/issues/427) | Rule remaining handoff decisions (1500 ms staleness, monitor isolation, 18.6 fps recording) | 1/2 |

---

## Tier 1 — show-stoppers

**Open**

| Order | ID | Issue | Item | Size |
|---|---|---|---|---|
| 1 | T1.11 | [#449](https://github.com/iamfatness/CoreVideoPro/issues/449) | Clip to Program shows placeholder before first frame. Step 2 (warmed cue) shipped #492 and live-checked. Step 1 needs an owner ruling: plain cut via `load-scene-graph` never captures the outgoing scene | S |

T1.13 observability shipped; [#473](https://github.com/iamfatness/CoreVideoPro/issues/473) stays open until the ProRes defect recurs and names itself.

**Done (tests; live-check noted)**

| ID | Issue | Closed by |
|---|---|---|
| T1.1 | [#428](https://github.com/iamfatness/CoreVideoPro/issues/428) | Not reproduced |
| T1.2 | [#429](https://github.com/iamfatness/CoreVideoPro/issues/429) | #459 live-checked |
| T1.3 | [#430](https://github.com/iamfatness/CoreVideoPro/issues/430) | #462 |
| T1.4 | [#431](https://github.com/iamfatness/CoreVideoPro/issues/431) | #460 |
| T1.5 | [#432](https://github.com/iamfatness/CoreVideoPro/issues/432) | #462 |
| T1.6 | [#455](https://github.com/iamfatness/CoreVideoPro/issues/455) | #458 live-checked |
| T1.8 | [#461](https://github.com/iamfatness/CoreVideoPro/issues/461) | #467 live checklist |
| T1.9 | [#463](https://github.com/iamfatness/CoreVideoPro/issues/463) | #464 live-checked |
| T1.10 | [#471](https://github.com/iamfatness/CoreVideoPro/issues/471) | #472 |
| — | [#478](https://github.com/iamfatness/CoreVideoPro/issues/478) | #484 live A/B |
| — | [#481](https://github.com/iamfatness/CoreVideoPro/issues/481) | #483 |
| T1.12 | [#475](https://github.com/iamfatness/CoreVideoPro/issues/475) | #493 (engine half still needs a live check) |
| T1.13 | [#473](https://github.com/iamfatness/CoreVideoPro/issues/473) | #494 observability only |
| T1.14 | [#480](https://github.com/iamfatness/CoreVideoPro/issues/480) | #487 live-checked |
| T1.15 | [#465](https://github.com/iamfatness/CoreVideoPro/issues/465) | #488 live-checked |
| T1.16 | [#485](https://github.com/iamfatness/CoreVideoPro/issues/485) | #489 live-checked |
| T1.17 | [#479](https://github.com/iamfatness/CoreVideoPro/issues/479) | #490 live-checked |

Betas: `beta-2026-09-11-404602b` (T1.1–T1.10, #478, #481);
`beta-2026-09-12-aac2d98` (T1.14–T1.17 + T1.11 step 2, owner live check passed).
Unsigned — that is T0.1.

---

## Tier 2 — first external install

| ID | Issue | Item | Size |
|---|---|---|---|
| T2.1 | [#433](https://github.com/iamfatness/CoreVideoPro/issues/433) | **Partial (#497).** Vcam register + recording folder done. WER LocalDumps is HKLM-only; per-user write does nothing. Remaining = elevated step | S |
| T2.2 | [#434](https://github.com/iamfatness/CoreVideoPro/issues/434) | Crash/telemetry production endpoint in the shipped build | S |
| T2.3 | [#435](https://github.com/iamfatness/CoreVideoPro/issues/435) | Closed-beta access gate | M |
| T2.4 | [#436](https://github.com/iamfatness/CoreVideoPro/issues/436) | OAuth broker on a stable production domain | S |
| T2.5 | [#437](https://github.com/iamfatness/CoreVideoPro/issues/437) | Wire certificate into `release.yml` after T0.1 | XS |
| T1.7 | [#457](https://github.com/iamfatness/CoreVideoPro/issues/457) | WinUI 0xc000027b on graceful close (related: #513) | S |

Done in this tier: T2.6 #504, T2.7 #497, T2.8 #496, T2.9 #499, T2.10 #499.

---

## Tier 3 — first show on a machine we cannot see

| ID | Issue | Item | Size |
|---|---|---|---|
| T3.1 | [#439](https://github.com/iamfatness/CoreVideoPro/issues/439) | GPU-tier defaults from `GpuTierProbe` (today telemetry only) | S |
| T3.3 | [#441](https://github.com/iamfatness/CoreVideoPro/issues/441) | First-run wizard — screens first | M |
| T3.4 | [#442](https://github.com/iamfatness/CoreVideoPro/issues/442) | Hardware sweeps on T0.3 machines | M |
| T3.5 | [#476](https://github.com/iamfatness/CoreVideoPro/issues/476) | **Mostly done (#501 #504).** Residual product call: vestigial per-layer borders → [#505](https://github.com/iamfatness/CoreVideoPro/issues/505) | XS |

Done: T3.2 #504, T3.7 #502, T3.8 #503.

---

## Tier 4 — release gates

| ID | Issue | Item | Size |
|---|---|---|---|
| T4.1 | [#443](https://github.com/iamfatness/CoreVideoPro/issues/443) | Record + stream + vcam drill as one command | M |
| T4.2 | [#444](https://github.com/iamfatness/CoreVideoPro/issues/444) | Scripted churn/resize soak (`0xc000027b`) | M |
| T4.3 | [#445](https://github.com/iamfatness/CoreVideoPro/issues/445) | Engine-off teardown audit vs five ZoomISO rules | S |
| T4.4 | [#446](https://github.com/iamfatness/CoreVideoPro/issues/446) | `scripts/qa/live-meeting-soak.mjs --takes N` for #422 | XS |

---

## Tier 5 — papercuts in the owner's show

| ID | Issue | Item | Size |
|---|---|---|---|
| T5.1 | [#447](https://github.com/iamfatness/CoreVideoPro/issues/447) | Reconcile persistent sources with #419 (`SourceRegistry`, atomic Take) | S |
| T5.2 | [#448](https://github.com/iamfatness/CoreVideoPro/issues/448) | Tiles wall stops re-animating on the cut. Plan 1 in #511. Residual #512 | M–L |
| T5.4 | [#450](https://github.com/iamfatness/CoreVideoPro/issues/450) | OHG redesign on screens first | L |
| T5.5 | [#451](https://github.com/iamfatness/CoreVideoPro/issues/451) | Scene canvas shows live GPU video | L |
| T5.6 | [#456](https://github.com/iamfatness/CoreVideoPro/issues/456) | Media In/Out points (also in Now #4, pending design) | M |

T5.3 moved to T1.11.

---

## Housekeeping

| ID | Issue | Item |
|---|---|---|
| H.1 | [#452](https://github.com/iamfatness/CoreVideoPro/issues/452) | Rebase #419 onto `main` so foundations stay current |
| H.2 | [#453](https://github.com/iamfatness/CoreVideoPro/issues/453) | Prune stale worktrees and merged remote branches |

---

## Later — post-beta, parked on purpose

Do not start these because a conversation was good. Owner promotes them into
Now. Specs stay in the completion plan; work starts from the issue.

| Issue | Item | Depends on |
|---|---|---|
| [#535](https://github.com/iamfatness/CoreVideoPro/issues/535) | **F1 — one source bus.** Every adapter publishes real `VideoFrame.pixels` + PCM. Biggest architectural hole in the current app | — |
| [#536](https://github.com/iamfatness/CoreVideoPro/issues/536) | SRT ingest decoder onto that bus (UI already adds SRT sources) | #535 |
| [#537](https://github.com/iamfatness/CoreVideoPro/issues/537) | DeckLink/AJA live frames on that bus (FOCUS_PLAN: out of beta) | #535 |
| [#538](https://github.com/iamfatness/CoreVideoPro/issues/538) | Core receive/send SRT, NDI, RTMP, HLS as adapters | #535 for receive; send can trail |
| [#540](https://github.com/iamfatness/CoreVideoPro/issues/540) | Split ingest off `MediaCore.cpp` once F1 has a consumer | #535 |
| [#539](https://github.com/iamfatness/CoreVideoPro/issues/539) | **MXL receive / ZoomISO Cloud.** Owner 2026-09-17: not priority | #535 + Linux domain next to Cloud |
| — | #419 rearch lanes A/B/C after shadow comparison | Measure first; foundation + first consumer together |

---

## Doc map (so this file stays the list)

| Doc | Role after 2026-09-17 |
|---|---|
| This file | Order |
| GitHub issue | Status, evidence, discussion |
| [`AGENTS.md`](../AGENTS.md) | Rules so work does not leak back into plans |
| [`CLAUDE.md`](../CLAUDE.md) | Build, run, crash classes |
| [`README.md`](../README.md) | As-built only |
| `docs/FOCUS_PLAN.md` | Product cut line and demos — no status |
| `docs/native-production-completion-plan.md` | How to implement F1/F2/F3 — linked from #535 |
| `docs/architecture-seams.md` | Process boundaries |
