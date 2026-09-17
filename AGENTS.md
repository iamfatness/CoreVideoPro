# AGENTS.md — rules for anyone (human or agent) working in this repo

Read this before writing code, issues, or plan docs. Build/run detail lives in
[`CLAUDE.md`](CLAUDE.md). Product rationale lives in
[`docs/FOCUS_PLAN.md`](docs/FOCUS_PLAN.md) and
[`COREVIDEO_PRO_PRODUCT_SPEC.md`](COREVIDEO_PRO_PRODUCT_SPEC.md).
**Work order lives in one place only:** [`docs/BACKLOG.md`](docs/BACKLOG.md).

## 1. One list

| Question | Answer |
|---|---|
| What do I work on? | The first unblocked **Now** row in `docs/BACKLOG.md` |
| Where does status live? | The GitHub issue linked from that row (`backlog` label) |
| Where does a new finding go? | A new GitHub issue **the same day**, then a row in BACKLOG |
| May I start work that is only in a plan, chat, or README table? | No |

If it is not a BACKLOG row with an issue number, it is not work. Chat, HTML
architecture decks, overnight handoffs, and completion-plan matrices are
**intake**. Promote them or discard them.

## 2. What other docs are allowed to say

| Doc | Allowed | Forbidden |
|---|---|---|
| `docs/BACKLOG.md` | Order: Now / Next / Later / Done | Long design essays |
| GitHub issue | Symptom, evidence, PRs, owner ruling | A second ranked list |
| `FOCUS_PLAN.md`, product spec | Why the product exists, cut line | Checkboxes or “current status” |
| `native-production-completion-plan.md`, compositor/GPU plans | How to build a thing, linked from an issue | A queue of work |
| `README.md` | As-built capabilities | A to-do list |
| `CLAUDE.md` | How to build, run, and not break it | New epics |

If you add a checkbox list to a plan, delete it or move each box to an issue +
BACKLOG row.

## 3. Intake rule (this is how F1 went missing)

A useful observation from a soak, a plan section, or a conversation must become
a GitHub issue within a day or it is discarded. Label `backlog`. Do not “leave
it in the completion plan.”

Then stop. The owner ranks. Agents do not invent order and do not promote
Later items to Now.

## 4. WIP and PRs

- Max **3** Now items in flight.
- A PR names **one** BACKLOG issue in the title or first line (`Fixes #NNN` when
  it actually closes it).
- No agent-started epic without an issue. No foundation PR onto `main` without
  its first real consumer (the #419 rule).
- New behavior does not grow `StudioViewModel.cs` or dump another ingest path
  into `MediaCore.cpp`. Extract a focused type; pixels stay in the core.

## 5. Close rule

An issue closes only with:

- a merged PR that meets the issue’s “done when”, or
- an owner comment: won’t do / not planned.

A plan doc never closes work. “Implemented in the contract” is not done if
pixels/PCM are empty.

## 6. North star (do not trade this)

Low-latency **and** high-quality A/V. 1080p up to 60 fps. 60 fps is per-frame
delivery, not an average. The compositor stays on the GPU. Do not downgrade
quality to hide a pipeline problem. Evidence over anecdotes; a soak on one
RTX 4090 is not a fleet.

Shell owns no real-time media. Zoom SDK stays in `corevideo-zoom-engine`.
Spine features (ISO, NDI, SRT, browser, later MXL) are **core adapters** behind
typed commands.

## 7. Parking lot vs Now

Owner 2026-09-17: **Now is show survival** (#529/#533, #518, #516, then #526, #513).
**Next is F1 (#535) then SRT/NDI send (#538) then SRT ingest (#536).**
**MXL / ZoomISO Cloud / K8s (#539) stay Later.** Do not design a fifth private
pipe until F1 exists.

Architecture that does not save tonight's show does not jump Now.

## 8. When you find a new defect mid-task

1. File the issue (`backlog`).
2. Add a Later or unranked row in `docs/BACKLOG.md` in the same change if you
   are already touching that file; otherwise file the issue and tell the owner
   it needs a rank.
3. Finish the Now item you were on unless the owner re-ranks.

Do not context-switch the repo onto the newest incident.

## 9. Commands (Windows)

See `CLAUDE.md` for the full runbook. Defaults:

```powershell
npm run app                 # best-available core + WinUI
npm run test:gate           # full Windows gate
# native tests: always  cmake --build native/build-dev --config Release
```

Never believe a native perf number from a Debug `corevideo-native.exe`.
Do not take over the desktop with Computer Use unless the owner asks.

## 10. Definition of “real”

A capability is real when the adapter is behind its `COREVIDEO_WITH_*` gate,
the stub still round-trips, `profileCapabilities()` only announces what was
built, snapshots carry measured counters, and there is evidence on a packaged
or dev-rig path — not only a contract test.
