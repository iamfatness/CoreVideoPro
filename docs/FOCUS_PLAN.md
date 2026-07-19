# CoreVideo Pro — Focus & Competitiveness Plan

_Status: owner decisions 2026-07-19. Companion to [`alpha-plan.md`](alpha-plan.md),
[`beta-plan.md`](beta-plan.md), and [`COREVIDEO_PRO_PRODUCT_SPEC.md`](../COREVIDEO_PRO_PRODUCT_SPEC.md).
This document is the **product spine**: what we build, what we freeze, and how we stay
competitive without turning the app into an unfocused switcher autobiography._

---

## 1. The bet

> **CoreVideo Pro is the fastest path from a Zoom room to a broadcast-looking show on
> Windows** — clean multi-participant feeds, Magic Scene, lower-thirds/captions, ISO
> stems, program record + RTMP + virtual camera, plus studio interop (NDI/SRT) and live
> graphics (browser sources) — without screen-capture hacks.

We are not “another OBS.” We are not full-career vMix. We win when:

1. A Zoom room becomes a show in minutes (Magic Scene).
2. Outputs and stems are **production-real** (program + ISO V/A, RTMP, vcam).
3. The show hands off into real studios (NDI/SRT) and takes live graphics (browser).
4. Someone who is not us can install it and finish a first show.

---

## 2. Two products inside one app

| Product A — “Magic Scene” | Product B — “Windows vMix” |
|---|---|
| Join → polished show → record/stream/vcam | Routing matrix, VST rack, DSK, Companion |
| Wins on **speed + Zoom-native quality** | Wins on **depth for full-time switchers** |

**Next 90 days prioritize Product A**, with a fixed set of **studio-class capabilities**
that are not “vMix cosplay” — they are how pro buyers leave OBS:

- ISO record (video + audio)
- NDI / SRT
- Browser sources

Product B features already shipped (VST, routing, DSK, Companion) stay in the app as
**Pro tools**, secondary in onboarding and marketing.

---

## 3. Non-negotiables (product constitution)

### N1 — One golden path

**Join → Magic Scene → Take → Record / Stream / Vcam** must work on a clean machine in
**under 10 minutes** the first time, **under 3 minutes** the second time.

Progressive disclosure:

- **Show mode (default):** multiview, inputs, scenes, transport, lower-thirds, simple
  audio (gain/mute/solo + master limiter), browser source, ISO toggle, NDI/SRT/RTMP/vcam.
- **Pro mode (secondary):** routing matrix, VST, DSK/key buses, Companion/OSC, mastering rack.

### N2 — Quality bar is A/V, not feature count

Low latency **and** high quality at 1080p/60. Never downgrade quality to dodge perf.
Never ship a release that fails:

1. Real Zoom multi-participant video + audio
2. A/V sync on a **packaged** recording (clap test head + tail)
3. Record + RTMP + vcam simultaneously for ≥30 min without embarrassment
4. Leave / rejoin mid-show without deadlock or orphan state
5. When ISO is enabled: program + ISO files on disk, time-aligned (Demo E)

### N3 — Freeze outside the spine

Until the **Alpha Gate Packet** (§7) is green, the only allowed work is:

- Reliability / correctness (A/V, crash, teardown, capture)
- Packaging / install / update
- Onboarding / golden path UX
- Supportability (bundles, crash reports)
- Polish on Magic Scene, Set & Forget, templates, lower-thirds
- **Spine features:** ISO record V+A, NDI, SRT (primary direction), browser BR-1 hardening

No DeckLink/AJA, ASIO, VST round-2, macOS, multi-operator, BR-2/3, or net-new categories.

### N4 — Every PR answers one question

> Which metric does this improve: **time-to-first-show**, **show survival**, **output
> correctness (program/ISO/RTMP/NDI/SRT/vcam)**, **graphics path**, or **install success**?

If none, park it in a post-alpha backlog and do not build it.

### N5 — One maturity story

| Stage | Definition | Who |
|---|---|---|
| **Alpha** | Owner runs real shows without the app embarrassing itself | Owner |
| **Closed beta** | 5–20 external operators; installable; supportable | Not us |
| **1.0** | Public/paid; competitive on the spine; Pro tools stable | Market |

---

## 4. Beta spine cut line (owner 2026-07-19)

```text
BETA SPINE
IN:  Golden path (Join → Magic Scene → Take → Program record/RTMP/vcam)
IN:  ISO record video + audio (files on disk, time-aligned to program)
IN:  NDI (program out minimum; harden)
IN:  SRT (one primary direction done well; second trails)
IN:  Browser sources BR-1 (supported, documented, health/isolation)
IN:  UVC, screen/window, basic + pro audio (pro tools secondary in onboarding)
HOLD: ML face framing (only if quality-clear before beta invite)
OUT:  DeckLink/AJA, ASIO, VST-r2, BR-2/3, multi-op, macOS, “more layouts for sport”
```

### Definition of done — spine features

#### ISO record (video + audio)

Not “ISO taps exist in the mixer.” **Files on disk an NLE/DAW can open.**

- [ ] Operator enables ISO on N sources (Zoom / capture / media as applicable)
- [ ] Program + each ISO land on disk with audio
- [ ] Shared epoch with program; clap-test head skew within G2 budget (e.g. &lt; 50 ms)
- [ ] Stop finalizes all ISOs cleanly (no 0-byte / unplayable tails)
- [ ] Disk-full / bad path = loud error, not silent skip
- [ ] Folder layout documented (e.g. `Program.mp4`, `ISO-01-Name.mp4` / audio stems —
      pick one scheme and stick to it)
- [ ] Support bundle lists ISO paths + encode health
- [ ] UI: “Program only” vs “Program + ISOs”

#### NDI

- [ ] Program appears in Studio Monitor / OBS NDI (or equivalent)
- [ ] Start/stop does not leak sender or require app restart
- [ ] Discoverable name pattern + firewall note in quickstart
- [ ] Health: fps / dropped / connected

#### SRT

- [ ] **One primary direction** shipped well (owner picks: program **output** for
      contribution/CDN-style, or **ingest** for remote cameras — do not half-build both)
- [ ] 30+ min under load
- [ ] Passphrase uses existing DPAPI prefs
- [ ] Clear failed-connect vs connected health
- [ ] Second direction may trail by one release after primary is boringly green

#### Browser sources (BR-1)

- [ ] Supported beta surface (not buried “experimental”)
- [ ] Add URL, size presets, health, reload
- [ ] Host death isolated; source unhealthy; app survives
- [ ] Quickstart includes one HTML overlay example (scorebug / LT page)
- [ ] Security note: untrusted URLs are operator responsibility
- [ ] BR-2/3 (page audio, interactivity) remain post-beta

#### ML face framing (HOLD)

- No beta commitment.
- Optional pre-beta spike (time-boxed, e.g. 3–5 days):
  - **Pass:** better headroom/centering on 4+ Zoom tiles at 1080p without dropping
    program fps; failure degrades to fit/fill
  - **Fail:** slip; no partial “AI framing” toggle in the UI
- Heuristic fit/fill / manual pan-zoom remains the default story.

---

## 5. Where to win vs table stakes

### Must win (invest)

| Capability | Competitive bar |
|---|---|
| True Zoom multi-feed + metadata | Stable join, roster, active speaker, clean tiles, rejoin |
| Magic Scene + 3–5 templates | Interview / panel / presenter+share / solo / intro-outro; one click |
| Set & Forget that doesn’t thrash | Debounce, hold, screen-share awareness; manual override wins |
| Lower-thirds + captions | Brand kit, roster names, readable |
| **ISO V+A** | Stems for edit; Demo E |
| **NDI / SRT** | Studio interop / contribution |
| **Browser sources** | Live graphics without AE |
| Program outputs | Record (A+V), RTMP sustained, vcam |
| Audio that doesn’t click | Auto-level defaults + limiter; deep mix optional |

### Table stakes (good enough)

Multiview layouts (enough already), UVC + screen/window, basic scenes, support
bundle/crash pipeline (finish wiring, don’t expand forever), Companion/OSC action set
(don’t grow weekly).

### Explicit OUT until post-beta (unless beta churn forces it)

DeckLink/AJA, ASIO, VST round-2, mastering-from-file analysis, multi-track beyond
defined ISO scheme if already covered, multi-operator, PTZ, macOS, browser BR-2/3.

---

## 6. The demos that matter

Build the company around these. If a change doesn’t improve one, it is optional.

| Demo | Flow | Pass |
|---|---|---|
| **A — Magic Scene** | Join 3–4 people → Magic Scene → LTs → Set & Forget → Record | Stranger says “I’d use that” |
| **B — Hybrid / interop** | Host UVC + Zoom guests → Magic Scene → **NDI and/or vcam** (+ SRT if primary) | No OBS bridge required for core story |
| **C — Survive** | 30–60 min program record + RTMP + vcam; leave/rejoin; resize | No click, freeze, silent video-only MP4, deadlock |
| **D — Graphics** | Browser HTML overlay on program; reload; kill host | App survives; source recovers or fails loud |
| **E — ISO post** | Stop → open program + ISO stems in NLE/DAW | Sync within a few frames |

Gate every release on **A + C**. Gate ISO claims on **E**. Use **B** and **D** for
positioning content.

**Weekly rule:** every week improves Demo A, C, E, B, D, or install success.

---

## 7. Alpha Gate Packet

Evidence artifacts (files under `artifacts/`), not chat checkboxes. Until green, no
net-new capability **outside** the spine.

1. Packaged install on clean profile → launch → join real Zoom
2. Clap-test recording: head/tail A/V offset numbers
3. 30+ min soak: record + RTMP + vcam; working-set curve; drop counters
4. Leave/rejoin mid-show notes
5. Support bundle zip after run
6. System-audio citizenship verdict (vcam in Zoom + browser audio)
7. Magic Scene screenshot set for 1–4 and 5+ participants
8. *(When ISO claims ship)* Demo E: program + ≥2 ISOs openable and aligned

Link evidence from release notes. Prefer scripted soaks via the control API (`:8011`)
so re-runs are one command.

---

## 8. Phased plan (90 days)

### Phase 0 — Focus freeze (week 0)

- [ ] This document is the cut line (done when merged)
- [ ] README “Current focus” points here
- [ ] WIP limit: max **3** active workstreams; max **1** distribution epic
- [ ] Agent/PRs require a gate tag: `alpha-G*`, `spine-iso`, `spine-ndi`, `spine-srt`,
      `spine-browser`, `demo-A`, `beta-B1`, etc.

### Phase 1 — Alpha close (weeks 1–3)

**Goal:** owner trusts the product for real shows.

- Close remaining alpha verification with artifacts (G0–G5 / Alpha Gate Packet)
- Browser BR-1: soak under real show load (shipped — verify, don’t expand)
- Script soak via control API; keep `validate-record-audio` green
- Start calendar-bound items: signing identity + Zoom SDK redistrib answer

**Exit:** Alpha Gate Packet 1–7 complete; tag e.g. `0.2.0-alpha` “owner-show-ready.”

### Phase 2 — Competitive spine (weeks 2–6, overlaps late Phase 1)

**Strict order (product leverage):**

1. **ISO record V+A** — definition of done above; Demo E  
2. **Browser sources as supported** — docs, quickstart, health UX  
3. **NDI program out** — harden; Demo B  
4. **SRT primary direction** — owner pick; 30 min green  
5. **Magic Scene / Set & Forget polish** — parallel only if not blocked on same hot files  

ML face framing: only the time-boxed spike if schedule allows; default is HOLD.

**Exit:** Demos A, C, D, E recordable; B if NDI/SRT primary ready.

### Phase 3 — Closed beta readiness (weeks 3–8)

Align with [`beta-plan.md`](beta-plan.md) B1–B6 and [`beta-engineering-spec.md`](beta-engineering-spec.md):

```text
Signing + Zoom SDK redistrib
  → signed MSIX + appinstaller hosted
  → first-run wizard (sequence existing settings)
  → clean-box matrix (laptop + mid GPU + AMD if possible)
  → invite ≤5 operators with 1-page quickstart
```

Beta test script **includes** ISO + NDI + browser + SRT primary.

**Exit for first external invite:**

- Clean-box install ≥ 4/5 on reference machines
- Time-to-first-record &lt; 15 min with quickstart only
- Support bundle from someone else is diagnosable
- No P0 on join / program record / ISO (if enabled) / stream / vcam / browser crash isolation

### Phase 4 — Compete (weeks 7–12)

- Iterate on beta pain only (join, audio defaults, Magic Scene misses, ISO disk, install)
- Publish Demo A + B (+ E for agencies/podcasts)
- Primary ICP recommendation: **podcast / interview producers** (Zoom guests + host cam +
  ISO stems + YouTube RTMP / NDI handoff)
- Freeze net-new categories; deepen only what beta proves

---

## 9. Operating cadence

| When | Ritual |
|---|---|
| **Mon** | Scoreboard: Alpha Gate Packet + spine DoD only. Max 3 workstreams. |
| **Tue–Thu** | Build only inside those three. PR linked to a gate tag. |
| **Fri** | Demo day: A, C, D, or E; drop evidence in `artifacts/week-YYYY-MM-DD/`. Fail → fix-only next week. |
| **Anytime** | Temptation PR: 5-line decision log (§11), then backlog or reject. |

### Agent / PR policy

1. Work only from gate-tagged issues.
2. No agent-started epics without a human one-pager: problem, non-goals, demo impact.
3. Refactors only when blocked (e.g. split `StudioViewModel` because a golden-path or
   ISO bug cannot be fixed safely).
4. Docs that expand vision without closing a gate are optional; gate-closing docs are required.

### Maintainability (reliability investment)

- **`StudioViewModel` / hot core files:** extract by vertical slice after Alpha Gate
  Packet is green, or sooner if a spine bug forces it.
- Strangler pattern: no new methods on god files; new behavior in focused types
  (`MagicScene*`, `IsoRecord*`, `Transport*`, etc.).

---

## 10. Metrics (weekly)

| Metric | Alpha target | Beta target |
|---|---|---|
| Time to first good frame after Join | &lt; 15 s | &lt; 15 s on mid GPU |
| Join → Magic Scene → Record armed | &lt; 60 s (owner) | &lt; 3 min (stranger + quickstart) |
| Packaged program MP4 has A+V | 100% | 100% |
| ISO files openable + head skew | Demo E green | 100% of ISO-enabled test runs |
| \|A/V start skew\| clap test | &lt; 50 ms | &lt; 50 ms |
| 30 min triple-output soak | 0 critical | 0 critical on 2 machine classes |
| Clean install success | n/a | ≥ 80% first try |
| Magic Scene “accept without edit” | ≥ 70% test rooms | Ask beta users |

---

## 11. Decision log template

```markdown
## Decision: <title>
Date:
Gate impact: (none | blocks Gx | improves Demo A/B/C/D/E)
ICP impact: (podcast host | webinar | agency | power switcher)
Effort: (S/M/L)
Opportunity cost: what gate slips if we do this now?
Verdict: DO NOW | SCHEDULE POST-ALPHA | REJECT
```

### Locked decisions (2026-07-19)

| Item | Verdict |
|---|---|
| ISO record video + audio | **DO NOW** (spine; Demo E) |
| NDI program out | **DO NOW** (spine; harden) |
| SRT | **DO NOW** (one primary direction well) |
| Browser sources BR-1 | **DO NOW** (supported beta surface) |
| ML face framing | **HOLD** unless quality-clear pre-beta |
| DeckLink/AJA | **REJECT** for beta |
| ASIO / VST-r2 / BR-2/3 / macOS | **REJECT** for beta |
| First-run wizard + signed install | **DO NOW** (beta critical path) |
| Magic Scene / Set & Forget polish | **DO NOW** (Demo A) |

---

## 12. Messaging discipline

Until public 1.0:

- **Hero:** Zoom → Magic Scene → stream/record/ISO/vcam.
- **Second beat:** studio interop (NDI/SRT) and live graphics (browser).
- Do **not** lead marketing with VST, Companion, or routing matrix.
- Prefer **auto-production / assistant producer** over “AI” until ML framing (if ever)
  is actually good.

**Buyer one-liner:**

> “vMix if you need a career. CoreVideo Pro if you need a show from Zoom — with stems
> and studio handoff — before the guests get impatient.”

---

## 13. Relationship to other docs

| Doc | Role |
|---|---|
| This file | Product focus, cut line, demos, sequencing |
| [`alpha-plan.md`](alpha-plan.md) | Owner-show verification gates |
| [`beta-plan.md`](beta-plan.md) | External beta workstreams (B1–B7) — **cut line B7 is superseded by §4 here** for ISO/NDI/SRT/browser |
| [`beta-engineering-spec.md`](beta-engineering-spec.md) | Distribution, onboarding, support engineering design |
| [`COREVIDEO_PRO_PRODUCT_SPEC.md`](../COREVIDEO_PRO_PRODUCT_SPEC.md) | Vision; where it conflicts with this file on sequencing, **this file wins for the next 90 days** |
| [`README.md`](../README.md) | As-built architecture and capability status |

When beta-plan B7 said NDI/SRT OUT and browser experimental, **owner override 2026-07-19**
moved them onto the beta spine (see §4). Update beta-plan in a follow-up if desired;
until then, treat this document as authoritative for cut line.

---

## 14. Immediate next actions

1. Keep Alpha Gate Packet 1–7 moving with evidence under `artifacts/`.
2. Spec + implement **ISO record V+A** to the DoD in §4 (highest spine leverage).
3. Promote **browser BR-1** to supported in docs/quickstart; soak under load.
4. Harden **NDI program out**; choose **SRT primary** direction and ship that first.
5. Continue distribution: signing identity, Zoom SDK redistrib, MSIX + appinstaller.
6. Magic Scene accept-rate pass: 5 room sizes + screen share; every manual fix is Demo A backlog.
7. ML framing: only a time-boxed spike if ISO/NDI/SRT/browser are on track.
