# RC owner decisions and external inputs — open list

**Date:** 2026-10-23
**Status:** living list; each row closes with a dated ruling in this file and a BACKLOG note
**Why:** every code lane that was blocked on an owner input on 2026-10-23 is now in flight or merged. What remains between the next beta and a release candidate is mostly decisions and purchases only the owner can make. This page is the one place they are listed with a recommendation each, so they can be answered in one sitting.

## Decisions (answer each with one line)

| # | Decision | Options | Recommendation | Unblocks |
|---|---|---|---|---|
| D1 | Telemetry API key delivery to shipped builds (#434) | (a) bake a per-channel key into the release build via a CI secret; (b) fetch a key from the broker at first run after sign-in; (c) ship without uploads until signing lands | (a) for the beta channel: one revocable key, injected by `release.yml` from a repository secret, never in source | first real crash upload |
| D2 | OAuth redirect path | `/oauth/callback` (what the deployed broker serves) or `/callback` (the 2026-10-23 answer) | `/oauth/callback`; already implemented in #1034 and documented | Zoom Marketplace allow-list entry |
| D3 | Closed-beta access gate (#435) | signed key file; `services/licensing-api`; GitHub release visibility only | signed key file checked at launch, issued by hand for the first cohort; licensing-api later | tester onboarding, T2.3 |
| D4 | Installer: operator content left in retired version folders (#883) | move `Recordings\` into a dated backup folder with a manifest; leave in place and report | move with manifest, report the path loudly; never delete | #883 Task 4 |
| D5 | Installer helper may reset `PSModulePath` for its own process (#883) | yes / no | yes, process-local only, logged | #883 Task 2 |
| D6 | Handoff rulings from T0.5 (#427): 1500 ms tile staleness; monitor isolation mitigation | per issue text | rule as the issue proposes unless a drill contradicts it | T0.5 |
| D7 | GPU tier 16/25 tile ceilings versus the 10-slot product cap (#439) | accept the cap as the ceiling for RC; or schedule a wall-size lane | accept for RC; the Settings line already says "(product limit)" | nothing now; records the intent |
| D8 | Drill rule when an ISO (or Program) lands on the software encoder (#935) | encoder placement is a note and the row passes on frame identity alone; or hardware stays required for PASS | note for ISOs; Program on the software encoder stays a FAIL for RC because the 2026-10-22 gate measured hardware | #935 drill rule task |
| D9 | UI severity for a software ISO that is inside its frame budget (#935) | Informational / Warning | Informational with the measured per-frame cost; Warning only when frames drop | #935 operator-line task |
| D11 | Headless fault switch (env var) for the #993 live drill, as the #1030 recording-stall test uses | allow / require a test-only core build | allow, documented in `docs/reference/fault-injection-seams.md`, never set by the installer | #993 live drill task |
| D12 | Close #457 on the retained-log evidence (the crash was the post-exit drain fixed by #462; seven in-meeting closes since, no recurrence) | close / keep open until one more observed close | close; add the standing graceful-close check from the plan | #457 |
| D13 | Zoom helper: after a CRT invalid-parameter fault, absorb and continue, or fail fast (#856) | absorb / fail | absorb and log, as ZComms v0.1.8 did in the field | #856 crash trap task |
| D14 | Zoom helper: a single oversized or unparseable IPC message retires the helper today (#856) | skip and count / keep retiring | skip and count, loud log line | #856 |
| D15 | Helper crash dumps in the consented crash-report upload (#856) | list only / upload | list only; dumps stay local | #856 |
| D16 | Helper dump type (#856) | mini by default, full in QA / full always | mini by default, full under the QA env var | #856 |
| D17 | Render gather roster pre-read leaves one race: a guest A/V trim set during a render-thread stall of tens of ms could be dropped (#1037) | accept (such a stall is already a P0 report) / take the small fix in the spec | accept for this slice; the fix rides with the command-lane follow-up | #1037 |
| D18 | Capture dropout bounds (#562/#758): stall after 1.5 s without a new sample; "hold" keeps the last frame 2.0 s after an explicit loss then shows the named slate; a dead device leaves Program within 5.0 s | confirm / change the numbers | confirm | #562/#758 policy task |
| D19 | After the 5 s deadline on a "black" source, stay black or show the named slate (#758) | black / slate | named slate, so the operator knows which source died | #758 |
| D20 | Preview and multiview also show the failed slate once a source is declared failed (#758) | yes / Program only | yes; one rule in all three compositors | #758 |
| D10 | The five ZoomISO teardown rules as worded in the #445 spec (rules 4 and 5 are reconstructed) | confirm / correct against the original ZoomISO freeze log | confirm unless the log says otherwise | #445 audit tasks 4 and 5 |

## External inputs (purchases, accounts, third parties)

| # | Input | Owner action | Blocks |
|---|---|---|---|
| E1 | Code-signing certificate (#423) | buy Authenticode or Azure Trusted Signing; add the secret to `release.yml` | #437, any public beta |
| E2 | Zoom SDK redistribution answer (#424) | written answer from Zoom or the licence text | public release |
| E3 | Reference machines (#425) | mid-tier GPU, hybrid-graphics laptop, AMD GPU | #442 hardware sweeps, #439 tier verification |
| E4 | Tester feedback channel (#426) | pick Discord/GitHub Discussions/email | T0.4 |
| E5 | Cloudflare custom domains (#434/#436) | `telemetry.corevideo.io`, `ops.corevideo.io` on the two Workers; `auth.corevideo.io` on the broker Worker in the site repo; deploy both | first real upload; Zoom sign-in on the new domain |
| E6 | Zoom Marketplace redirect allow-list | add `https://auth.corevideo.io/oauth/callback`; keep the old entry until old installs age out | Zoom sign-in on the new domain |
| E7 | Staging Workers redeploy | redeploy `-staging` Workers and re-put their secrets (renamed in #1034) | staging smoke (#575) |
| E8 | Dependabot alerts on main (4: 2 moderate, 2 low) | review and merge the bumps | hygiene before RC |

## Already ruled (for the record)

- 2026-10-22: outputs are the release gate, zero tolerance per-frame identity; Studio cells are a tolerance UX metric.
- 2026-10-23: held frames during a source stall pass when counted and labelled (#1038).
- 2026-10-23: crash-resume rule = resume unless the Zoom rejoin failed (#1025).
- 2026-10-23: HKLM WER LocalDumps, full dumps, keep 5 (#433).
- 2026-10-23: three GPU tiers by VRAM, overrides allowed and logged (#439).
- 2026-10-23: production hostnames `telemetry.corevideo.io`, `ops.corevideo.io`, `auth.corevideo.io` (#434/#436).
- 2026-10-23: the installer stays 32-bit NSIS; 64-bit registry view and Sysnative at the seams.

## Rulings 2026-10-23 23:50 EDT (owner, via the pick-one tool)

D1 CI secret baked per channel (one revocable beta key via release.yml). D2 `/oauth/callback`.
D3 signed key file at launch, hand-issued for the first cohort. D4 move operator content to a dated
backup with a manifest, never delete, report loudly. D5 helper may reset PSModulePath for its own
process, logged. D8 ISO on the software encoder = note, passes on identity; Program on the software
encoder stays FAIL for RC. D9 Informational with measured per-frame cost; Warning only when frames
drop. D10 all five ZoomISO rules confirmed as worded. D11 env-var fault switch allowed, documented,
never set by the installer. D12 close #457 on the evidence; add the graceful-close check. D13 absorb
CRT invalid-parameter faults and continue. D14 skip-and-count bad IPC messages. D15 helper dumps
listed only, never uploaded. D16 mini dumps by default, full under the QA env var. D17 accept the
A/V-trim race for this slice. D18 1.5 s / 2.0 s / 5.0 s confirmed. D19 named slate after the
deadline even for "black". D20 failed slate in Preview and multiview too. D6 and D7: pending.
