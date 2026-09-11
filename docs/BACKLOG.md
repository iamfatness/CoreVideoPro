# CoreVideo Pro — ranked backlog

**Refined 2026-09-11 (after the first real meeting; see Tier 1 order).** Earlier today (owner: "Your rank makes sense"): #449 up to T1.11; new T1.12 #475, T1.13 #473; T2.7-T2.10 (#474, #469, #466, #468); T3.5-T3.7 (#476, #465, #470).

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

**Open, in the order to work them.** Ranked 2026-09-11 after the first real multi-guest meeting.
The owner's top priority is core functionality: Zoom sources and audio.

| Order | ID | Item | Clause | Size |
|---|---|---|---|---|
| 1 | [T1.14](https://github.com/iamfatness/CoreVideoPro/issues/480) | Scene layer sources wipe themselves every few seconds (the source dropdown's list refresh writes a blank as an operator pick, `SourcesPage.xaml.cs:153`), then the core's positional fallback shows a RANDOM guest (`RouteSourcePolicy.h`). Shell guard first, then the core "empty route = blank" sentinel | 1 | S + M |
| 2 | [T1.15](https://github.com/iamfatness/CoreVideoPro/issues/465) | The first roster participant's isolated audio never arrives (channel silent while talking; the meeting-mix subscription is keyed to the same participant). Needs ONE Extra-logs diagnostic run first (engine `debug` audio events), then the fix | 1 | unsized |
| 3 | [T1.16](https://github.com/iamfatness/CoreVideoPro/issues/485) | Mixer shows only sources (follow-up to #484; non-sources show dead strips) | 3 | S |
| 4 | [T1.17](https://github.com/iamfatness/CoreVideoPro/issues/479) | Tiles manual slots / "never show" are saved by Zoom's per-session user ID, so they go stale or hit the wrong person next meeting; key on stable identity, clear stale entries. Audit the Show Input roster for the same issue | 1/3 | M |
| 5 | [T1.11](https://github.com/iamfatness/CoreVideoPro/issues/449) | A clip going to Program shows a placeholder colour before its first frame. Step 1: hold the outgoing picture until the first real frame; step 2: hand over the warmed decoder | 1/3 | S then M |
| 6 | [T1.12](https://github.com/iamfatness/CoreVideoPro/issues/475) | A join that lands in the waiting room (or waits for the host) is reported as a failed join after ~52 s; forward the waiting states, always log meeting status, leave on give-up | 1/2 | S |
| 7 | [T1.13](https://github.com/iamfatness/CoreVideoPro/issues/473) | Installed build never starts FFmpeg for a ProRes media source (a dev launch does) | 1/2 | S-M |

**Done** (verified by tests; the live check is noted where one ran):

| ID | Item | Closed by |
|---|---|---|
| [T1.1](https://github.com/iamfatness/CoreVideoPro/issues/428) | Auto-take while automation is off | Not reproduced; closed |
| [T1.2](https://github.com/iamfatness/CoreVideoPro/issues/429) | Pause/Play restarted a Program clip | #459 (live-checked) |
| [T1.3](https://github.com/iamfatness/CoreVideoPro/issues/430) | Rolled-back Take leaves the media selection | #462 |
| [T1.4](https://github.com/iamfatness/CoreVideoPro/issues/431) | Monitors halve Program under load (monitor shedding) | #460 |
| [T1.5](https://github.com/iamfatness/CoreVideoPro/issues/432) | Engine-off: shell stops polling the core | #462 |
| [T1.6](https://github.com/iamfatness/CoreVideoPro/issues/455) | Media clip audio never reaches the engine | #458 (live-checked) |
| [T1.8](https://github.com/iamfatness/CoreVideoPro/issues/461) | Close while recording kills the recording | #467 (8-step live checklist passed) |
| [T1.9](https://github.com/iamfatness/CoreVideoPro/issues/463) | Multiview sources 9/10 not clickable | #464 (not yet re-checked live by the owner) |
| [T1.10](https://github.com/iamfatness/CoreVideoPro/issues/471) | Poll start/stop race freezes meters for a session | #472 (diagnosed from a dump) |
| [#478](https://github.com/iamfatness/CoreVideoPro/issues/478) | Zoom: wall guests starved, video-off/unrouted feeds, talk-driven flashing → **sources-only feeds**, stable resolution tiers | #484 (live A/B: talk churn 4→0; unroute drops video+audio) |
| [#481](https://github.com/iamfatness/CoreVideoPro/issues/481) | App muted guests on its own; meters hid talkers → **A1-only mute**, pre-mute meters, ZOOM MUTED badge | #483 |

Shipped in beta-2026-09-11-404602b (installed on the owner's machine 2026-09-11).

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
| [T2.7](https://github.com/iamfatness/CoreVideoPro/issues/474) | Installer: one `CoreVideo Pro` shortcut that always opens the newest beta; uninstall removes the first-run FFmpeg runtime; an install with no registry key can still be removed | 4 | S |
| [T2.8](https://github.com/iamfatness/CoreVideoPro/issues/469) | The default recording folder is relative, so recordings land inside the install folder (at risk on uninstall/upgrade); resolve to a user folder and show the absolute path | 1/4 | S |
| [T2.9](https://github.com/iamfatness/CoreVideoPro/issues/466) | Core: stopping Record while a stream stays up (or a stream start) erases the recording lifecycle, so status reads "stopping" forever with no completed/failed | 2 | M |
| [T2.10](https://github.com/iamfatness/CoreVideoPro/issues/468) | RTMP sender keeps `status: live` / `destinationHealth: ok` while its destination is unreachable | 2 | S |

## Tier 3 — first show on a machine we cannot see

| ID | Item | Clause | Size |
|---|---|---|---|
| [T3.1](https://github.com/iamfatness/CoreVideoPro/issues/439) | GPU-tier defaults: gate the 4K canvas and multiview tile counts on `GpuTierProbe` (today it only feeds telemetry) | 1 | S |
| [T3.2](https://github.com/iamfatness/CoreVideoPro/issues/440) | Friendly message when the WebView2 runtime is missing (browser host exits 3; the shell shows a stalled source) | 3 | S |
| [T3.3](https://github.com/iamfatness/CoreVideoPro/issues/441) | First-run wizard — screens first, owner markup, then code (the OHG lesson) | 3 | M |
| [T3.4](https://github.com/iamfatness/CoreVideoPro/issues/442) | Hardware sweeps on T0.3 machines: integrated GPU, audio devices (USB/Bluetooth, unplug mid-show), cheap UVC webcams | 1 | M |
| [T3.5](https://github.com/iamfatness/CoreVideoPro/issues/476) | Colour fields need a colour picker (Tiles border/glow/background, layer colour, caption text); keep the hex box beside it | 3 | S |
| [T3.6](https://github.com/iamfatness/CoreVideoPro/issues/465) | One guest's per-guest audio distorted while the meeting mix is clean (investigate: +6 dB strips + noise suppression on every guest, stem quality) | 3 | unsized |
| [T3.7](https://github.com/iamfatness/CoreVideoPro/issues/470) | ISOs armed with Engine off write nothing and give no warning | 2/3 | S |

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
| ~~T5.3~~ | ~~A clip entering Program cold-starts with a placeholder flash~~ — **moved to T1.11** (owner, 2026-09-11) | | |
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
