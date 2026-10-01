# Overnight handoff — morning of 2026-09-10

Branch: `codex/production-realtime-architecture` (PR #419, draft)
Everything below is committed and pushed unless it says otherwise.
**Nothing has been merged to main.** See "The merge decision" at the end.

---

## Before anything else: the meeting ended

**Zoom meeting `8916561023` ("CVP Soak") is down.** The app had to be closed and rebuilt to land the observability work, and the app was the meeting's only host, so closing it ended the meeting. Two rejoin attempts authenticated fine (`auth_ok`, jwt and zak both set) and then waited forever on a meeting that no longer exists.

**Start the meeting again before item 1 below.** The app is up and healthy on `127.0.0.1:8011`, syncing, with the real core, real GPU compositing and the render/audio/video-output workers running — it just has no Zoom sources in it. Nothing needs changing; `/snapshot` will show the sources as soon as the meeting is back.

This also means **everything after roughly 23:30 was verified without live Zoom sources.** Where that matters, it is called out.

---

## After the reboot (07:30)

The machine rebooted after this was written. The take/scene telemetry and the output supervisor were built clean and passed the native suite three times (999/999) before the reboot, but the commit had not gone through. They are now committed as `329e33c` and `ceb334c` and pushed. Nothing is running: the app, the core and the meeting all need starting.

## Read this first: what needs your eyes

These cannot be closed without a human watching. They are batched so you are not hunting.

| # | What to do | Why it needs you |
|---|---|---|
| 1 | **Watch a gallery cut to program.** Cue a Tiles scene to Preview, let it settle, Take. | Two fixes landed for this tonight. My instruments can prove the layers were emitted continuously; they cannot tell me the cut *looked* seamless. You caught the background refresh when my measurements saw nothing. |
| 2 | **Watch the same cut with the background as a live source.** | The live background had a separate bug from the tiles. Fixed, unverified visually. |
| 3 | **If either still refreshes**, say so and stop there. | A third contributor is already identified and instrumented: the take can churn Zoom subscriptions. The telemetry to see it landed tonight. |

---

## New: you can now watch the core directly

`GET http://127.0.0.1:8011/snapshot` serves the core's own `sessionState` verbatim — **49 nodes**, including `realtimeEvidence` (per-worker progress ages, completed slots, skipped slots, deadline misses, `audioLostSamples`), `encoderEvidence` (queue depth, oldest queued age, dropped video/audio, per-ISO fidelity), `programBuffer` (underruns, overflows, gpuNotReady, occupancy), `recording`, `tiles`, `multiviewer`, `sourceAuthority`.

Until tonight the shell forwarded **ten hand-picked fields** out of that document and dropped the rest. Every measurement I fumbled during your show failed for want of this.

It is behind the same auth as every other endpoint (loopback open, LAN hard-refuses without `COREVIDEO_CONTROL_TOKEN`), redacts fail-closed, and reports its own staleness rather than implying freshness — one call during a wedged sync correctly reported `ageMs: 127998, stale: true`, which is exactly the condition that used to be invisible.

Two things it proved about itself worth keeping: it refused to serve a synthesized record that had not come from a real core sync, and it caught a genuine redaction bug — the existing log redactor's rtmp rule is greedy over non-whitespace, so run over JSON as text it silently swallows the closing quote and merges the following fields. It now walks the document tree instead.

**Per-layer geometry is still not available.** Rect, fit mode, opacity and fill colour live on the render plan inside the core and never reach the wire. The `tiles` node does publish a rect per member. Layer `order` is now exposed on `/state`, which answers "was the background layer continuous across the cut".

---

## Four decisions I deliberately did not make

1. **The 1500 ms tile staleness threshold** (`compositor::kTilesStaleFrameMs`). A member whose frames lapse past it is dropped from the wall and the wall reflows. Real Zoom feeds gap past 1500 ms routinely. You said membership churn is acceptable, so this may be fine as-is — but the number was chosen without evidence and is worth a look.
2. **Auto-take is live while automation is off.** Observed tonight: `automationOn=false` but `autoTake=true`, and takes fired that I had not asked for. During a show that means the system can cut without an operator. I turned it off on this machine to get clean measurements. Is that combination intended?
3. **Monitor isolation.** Program, Preview and Multiview share one render thread and one D3D context. Measured on the 4090: a sustained Preview overrun of ~1.5 frame periods costs Program **half its produced frames** (121 → 65 per 2 s window). The proper fix is the monitor compositor split, which is post-beta. A cheap mitigation exists — the multiview tick divisor, currently at full rate.
4. **The second recording session.** Two recordings were made during the live show. The first is clean. The second is degraded and I do not know why. Details below.

---

## What the live show established

All measured on your real 8-source meeting, not the fake engine.

**The good, and it is substantial.** A 214-second recording produced Program at **59.7 fps** (12,784 frames) alongside **eight ISO streams**, two of which were spilled to CPU software encoding. The encoder received, encoded and processed every Program frame with none lost. The compositor held 60 fps with a 6.6 ms render and zero drops. The core survived the meeting ending without taking the studio down — this codebase has a documented incident where leaving a meeting killed the entire studio, and that did not happen.

**The encoder capacity probe works on real hardware.** It measured **seven** concurrent H.264 sessions at 1080p60 on the RTX 4090 (eight at 1080p30), gave Program one, put six ISO on hardware and told the operator in words that two were going to the CPU. Before this morning that was a hard-coded literal claiming eight sessions on every machine regardless of the card.

**ISO fidelity** ran 11–17 fps per participant, which is consistent with what real Zoom senders actually push rather than a cap we impose.

**What is unexplained.** The second recording session (`corevideo-recording-20260909-222914`) has Program at 18.6 fps against the first session's 59.7. Its window overlaps the meeting teardown and its ISO durations range 116–154 s against a 143 s Program, which is the signature of sources dropping out rather than a steady-state encoder problem. **I nearly reported this as "Program collapses under real load" before noticing I had paired a log line from one session with a file from another.** Treat it as unexplained, not as a defect.

**Audio is being shed.** Four pacer re-anchor events, each logged as the worker falling >500 ms behind with real-time audio discarded. That is permanent PCM loss. The counter to measure it properly landed this morning.

---

## Where the beta slice stands

`docs/production-realtime-completion-plan.md` (on the PR #419 branch) §7 has the full framing.

| Item | State |
|---|---|
| D3D device-loss recovery | Done. Proven on two machines. |
| ISO video off the audio grid, arrival-driven | Done. ~99% at a true 60 fps source. |
| Truthful destination lifecycle | Done. Stop no longer claims completion early. |
| Fault-injection seams | Done. Device loss, blocked present, blocked monitor render. |
| Encoder capacity probing | Done. Verified on real hardware tonight. |
| Output supervisor | Done (`ceb334c`). Restart ladder, health from accepted units, loud give-up. Not exercised against a live RTMP/SRT destination yet. |

---

## Why so much of tonight went into instruments

I was confidently wrong three times, each time because a measurement lied:

- The **fake engine double-subscribed** every participant, so per-participant source rate was twice what it reported. Three ISO numbers I gave you were revised downward once that was fixed — and the interleaving was itself manufacturing part of the loss it appeared to measure.
- The **rendered-scene field is frozen**. It reported the previous scene 15+ seconds after a take while Program was compositing the new one. I measured a take through it and read "no change" as "clean cut".
- I **paired a log line with the wrong recording session** and nearly blocked the merge on a phantom 18.6 fps regression.

There is a fourth instance of the same family in the product itself: the perf drill hard-coded a failure cause that had already been fixed, and the ISO validation harness hard-coded the very 50 Hz assumption it existed to detect. Both were *true statements that led a reader to a wrong conclusion*.

That is why telemetry took priority over further testing. More autonomy without better instruments mostly buys more confident wrong answers.

---

## The merge decision

You authorised merging to main once a soak confirmed the product was not worse than before. I have not merged, for one reason: **the soak did not pass cleanly, it surfaced live defects we are mid-way through fixing.** Merging a branch in flux would be the wrong reading of that instruction.

What is true right now: every CI job is green except the macOS drill, which is red on `main` as well and which you have deprioritised. Local suites are green. The branch is ready to merge the moment you are satisfied with items 1–3 at the top of this document.

**On splitting it up**, which you raised: the branch is ~38 commits and two genuinely different kinds of work. The beta stability work is live, user-visible, and independently reviewable. The architecture foundations are inert — constructed by their own tests and referenced by nothing in the core or the RPC server. If you want a smaller first merge, that is the natural seam.
