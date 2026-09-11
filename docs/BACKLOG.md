# CoreVideo Pro — ranked backlog

**This is the single ordered list of work.** Owner-approved 2026-09-10. It supersedes the
scoreboards in `docs/beta-plan.md` (§2) and the status claims in `docs/FOCUS_PLAN.md`,
which are kept for their rationale, not their checkboxes. Every item has a GitHub issue
labelled `backlog`; the issue is where status lives, this file is where order lives.

**How items are ranked.** Beta means 5-20 external operators running real shows on
machines we cannot see (`docs/beta-plan.md` §1). An item earns beta priority only if it:

1. stops a show breaking on a machine we cannot see,
2. lets us diagnose a failure nobody watched,
3. removes a papercut an operator hits during a real show, or
4. is required for the first external install at all.

Anything else is post-beta, however valuable. Owner actions that start a calendar clock
go first; then show-stoppers; then the first external install; then the first show on an
unseen machine; then release gates; then papercuts in the owner's own show.

**Where this came from.** A four-lane evidence audit on 2026-09-10 against code, merged
PRs, releases and git history (not the checkboxes). Key findings: the rearch's beta slice
is on `main` (#420); everything else in the rearch has no code on `main` and is post-beta
by the 2026-09-09 ruling; the first-external-install path has stalled, mostly on owner
actions; roughly half of the prior three weeks' changed lines went to the OHG engine,
which is not a beta workstream; and no known defect was tracked as an issue.

Sizes: XS < 1 h, S ≈ 1-3 h, M ≈ half to one day, L > one day (agent time, including the
TDD + review loop). Estimate for tiers 1-4 ≈ 3-4 working days; tier 5 ≈ 5-8 days.

---

## Tier 0 — owner actions (start the clocks)

| ID | Item | Clause | Why it is first |
|---|---|---|---|
| [T0.1](https://github.com/iamfatness/CoreVideoPro/issues/423) | Buy the code-signing certificate (Authenticode / Trusted Signing) | 4 | `release.yml` already refuses to ship unsigned; the 09-08 beta shipped unsigned (SmartScreen). Org validation takes weeks — the calendar critical path. |
| [T0.2](https://github.com/iamfatness/CoreVideoPro/issues/424) | Get the Zoom SDK redistribution answer | 4 | Builds already bundle the SDK from a private URL; practice is ahead of a legal answer that was never recorded (`docs/zoom-windows-sdk-packaging.md:27`). |
| [T0.3](https://github.com/iamfatness/CoreVideoPro/issues/425) | Acquire 2-3 reference machines (mid-tier GPU, hybrid-graphics laptop, AMD GPU) | 1 | Everything is proven on one RTX 4090. Lead time. |
| [T0.4](https://github.com/iamfatness/CoreVideoPro/issues/426) | Pick the tester feedback channel | 3 | `docs/beta-tester-guide.md` names none. |
| [T0.5](https://github.com/iamfatness/CoreVideoPro/issues/427) | Rule on the open handoff decisions: 1500 ms tile staleness; monitor isolation mitigation; the unexplained 18.6 fps recording | 1/2 | From `docs/overnight-handoff-2026-09-10.md` (#419 branch). Auto-take (T1.1) is closed, not reproduced — no longer an open ruling here. |

## Tier 1 — show-stoppers on any machine

| ID | Item | Clause | Size |
|---|---|---|---|
| [T1.1](https://github.com/iamfatness/CoreVideoPro/issues/428) | ~~Auto-take must not fire while automation is off (observed ~1/s on 2026-09-10; uncommanded on-air cuts)~~ — **Closed — not reproduced (#428)** | 1 | S |
| [T1.2](https://github.com/iamfatness/CoreVideoPro/issues/429) | Pause/Play on a Program clip restarts it: make pause a clock state on one decoder (incl. bin-row tap) | 1 | M |
| [T1.3](https://github.com/iamfatness/CoreVideoPro/issues/430) | A rolled-back Take restores the media selection (ledger unchanged by design) | 1 | S |
| [T1.4](https://github.com/iamfatness/CoreVideoPro/issues/431) | Multiview/Preview must not halve Program under load: apply the cheap multiview tick-divisor mitigation (proper fix = monitor compositor split, post-beta) | 1 | S |
| [T1.5](https://github.com/iamfatness/CoreVideoPro/issues/432) | While Engine is off, the shell stops polling the core: frozen core state and a stale launch-time warning | 2/1 | unsized |
| [T1.6](https://github.com/iamfatness/CoreVideoPro/issues/455) | Media clip audio never reaches the audio engine (fix in PR #458) | 1/3 | S |
| [T1.8](https://github.com/iamfatness/CoreVideoPro/issues/461) | Closing the app while recording kills the recording unfinalized (and skips the Zoom leave order) — **fix in this branch** (`codex/t1-8-close-guard`): close while recording/streaming asks "Stop outputs and close?", Stop and close (also taken when the dialog cannot be shown) waits ≤15 s for the close-request sessions to report finished, on evidence bound to the stop (fresh `RawReceivedUtc`, session id, core generation); the app-exit core stop closes stdin and gives the core 2 s to exit on its own before the kill-tree, then sweeps surviving descendants. Streams are stopped before recording (a record stop sent while streaming restarts the encoder and erases the recording lifecycle — core defect filed separately). Remaining: the pre-merge live checklist (not yet run; must include record+stream), and the Zoom engine is still terminated by the core, not taken through Leave → stop_raw_media | 1 | M |
| [T1.9](https://github.com/iamfatness/CoreVideoPro/issues/463) | Multiview sources 9 and 10 not clickable / undecorated (overlay capped PGM+PVW+sources at 10 total) | 1 | S |

## Tier 2 — first external install

| ID | Item | Clause | Size |
|---|---|---|---|
| [T2.1](https://github.com/iamfatness/CoreVideoPro/issues/433) | Installer (`scripts/alpha/installer.nsi`) registers the virtual camera, enables WER LocalDumps, creates the recording folder | 1/2/4 | S |
| [T2.2](https://github.com/iamfatness/CoreVideoPro/issues/434) | Telemetry and crash upload point at a production endpoint baked into the shipped build (today: a staging `workers.dev` URL only in `scripts/load-staging-env.ps1`) | 2 | S |
| [T2.3](https://github.com/iamfatness/CoreVideoPro/issues/435) | Closed-beta access gate (signed key or `services/licensing-api` — owner picks) | 4 | M |
| [T2.4](https://github.com/iamfatness/CoreVideoPro/issues/436) | OAuth broker on a stable production domain (monitoring already shipped, #324) | 2 | S |
| [T2.5](https://github.com/iamfatness/CoreVideoPro/issues/437) | Wire the certificate into the release pipeline when T0.1 lands | 4 | XS |
| [T2.6](https://github.com/iamfatness/CoreVideoPro/issues/438) | Reconcile `docs/beta-engineering-spec.md` (signed MSIX) with what ships (NSIS installer, #417) | 4 | XS |
| [T1.7](https://github.com/iamfatness/CoreVideoPro/issues/457) | WinUI shell crash on a graceful close: 0xc000027b in XAML's post-Exit dispatcher drain, after cleanup (2 of 10 closes; false crash prompt on the next launch). (recommended tier 2 by investigation; fixed in this batch) | 1 | S |

## Tier 3 — first show on a machine we cannot see

| ID | Item | Clause | Size |
|---|---|---|---|
| [T3.1](https://github.com/iamfatness/CoreVideoPro/issues/439) | GPU-tier defaults: gate the 4K canvas and multiview tile counts on `GpuTierProbe` (today it only feeds telemetry) | 1 | S |
| [T3.2](https://github.com/iamfatness/CoreVideoPro/issues/440) | Friendly message when the WebView2 runtime is missing (browser host exits 3; the shell shows a stalled source) | 3 | S |
| [T3.3](https://github.com/iamfatness/CoreVideoPro/issues/441) | First-run wizard — screens first, owner markup, then code (the OHG lesson) | 3 | M |
| [T3.4](https://github.com/iamfatness/CoreVideoPro/issues/442) | Hardware sweeps on T0.3 machines: integrated GPU, audio devices (USB/Bluetooth, unplug mid-show), cheap UVC webcams | 1 | M |

## Tier 4 — release gates

| ID | Item | Clause | Size |
|---|---|---|---|
| [T4.1](https://github.com/iamfatness/CoreVideoPro/issues/443) | The record + stream + virtual camera drill as one command | 2 | M |
| [T4.2](https://github.com/iamfatness/CoreVideoPro/issues/444) | Scripted churn/resize soak (the `0xc000027b` class; resize mitigation never soak-verified) | 1 | M |
| [T4.3](https://github.com/iamfatness/CoreVideoPro/issues/445) | Written engine-off teardown audit against the five ZoomISO rules | 1 | S |
| [T4.4](https://github.com/iamfatness/CoreVideoPro/issues/446) | Run `scripts/qa/live-meeting-soak.mjs --takes N` for #422 (needs the app stopped) | 2 | XS |

## Tier 5 — papercuts in the owner's show

| ID | Item | Clause | Size |
|---|---|---|---|
| [T5.1](https://github.com/iamfatness/CoreVideoPro/issues/447) | Reconcile persistent sources with the rearch: which #419 foundations slices 2-3 build on (likely `SourceRegistry`, atomic Take, `DeliveredProgramPacket`) | 3 | S |
| [T5.2](https://github.com/iamfatness/CoreVideoPro/issues/448) | Tiles wall stops re-animating on the cut (persistent-sources slice 2, on T5.1) | 3 | M-L |
| [T5.3](https://github.com/iamfatness/CoreVideoPro/issues/449) | A clip entering Program cold-starts with a placeholder flash (hand the warmed cue decoder to Program) | 3 | M |
| [T5.4](https://github.com/iamfatness/CoreVideoPro/issues/450) | OHG: redesign on screens first, integrate into existing tabs; cheap parity gaps (preview tally, gallery order, black/bars/FTB, on-air clock, nameplates) | 3 | L |
| [T5.5](https://github.com/iamfatness/CoreVideoPro/issues/451) | The scene canvas editor shows live GPU video (redesign, not a whitelist) | 3 | L |
| [T5.6](https://github.com/iamfatness/CoreVideoPro/issues/456) | In and Out points for media playback (lower priority) | 3 | M |

## Housekeeping

| ID | Item |
|---|---|
| [H.1](https://github.com/iamfatness/CoreVideoPro/issues/452) | Rebase #419 onto `main` so the foundations stay current and green (draft stays open) |
| [H.2](https://github.com/iamfatness/CoreVideoPro/issues/453) | Prune ~10 stale worktrees and merged remote branches |

## Post-beta (ordered, not scheduled)

- **Measure before migrating:** the rearch's shadow comparison (completion plan §7) — the
  number that says how far apart the two authorities are — before anyone commits a
  quarter to Lane A.
- **Rearch lanes** (completion plan §3 on the #419 branch): Lane A control plane (A1-A9),
  Lane B B2-B9, Lane C C4-C8, integration to G4. **Rule: a #419 foundation lands on `main`
  together with its first real consumer, never as an unwired island.**
- Persistent sources slices 3-4 fold into the rearch's atomic Take / transitions and Metal
  parity.
- OHG architectural gaps: non-Zoom sources in slots and a video aux bus ride the rearch's
  `SourceRegistry` and per-destination ownership.
- NDI out of process (in-process today; risk is published, not fixed).
