# Windows handoff — perf/latency findings from the macOS port (2026-08-05)

> **Windows verification status (2026-08-05, owner rig, RTX 4090).** Measured with
> `scripts/mac-show-drill.py --load 8` (8 × 1080p60 fake engine, producer confirmed
> at 59.7–60.3 ticks/s × 8). "Before" = main with ONLY the three shared-code fixes
> reverted, instrumentation kept.
>
> | § | Finding | Windows result |
> |---|---|---|
> | 1 | 8 ms ingest poll loses frames | **CONFIRMED.** Worse here than on macOS: the loss is at the SHM slot, so frames were never even decoded — 373–390 of 480 f/s ingested (81%), 78% composited. With the 2 ms poll: **480/480 ingested, 90–91% composited**, render-thread fetch stall 0.41 ms → 0.06 ms. |
> | 2 | 3.1 MB memcpy under the engine mutex | Fixed in the same build; ingest stage 1.05–1.19 ms → 0.74–0.79 ms. |
> | 3 | Relative-deadline pacer drift | **Present but masked.** Windows held 60.0 fps even before, because the 500 µs spin guard was compensating — exactly as this brief predicted. |
> | 3 (follow-up) | Can `kSpinGuardUs` come back down? | **CONFIRMED, applied.** 200 µs now holds 60.0 fps / 0 dropped with identical delivery and lock behaviour, and saves ~5 s of core CPU per 53 s of wall (non-overlapping ranges over 3 runs each). |
> | 6.4 | Wire the drill into Windows CI | Not yet — blocked on the open finding below. |
>
> **Open finding — RESOLVED 2026-08-06.** Delivery sat at 88–92%, short of the drill's
> `MIN_FRAME_DELIVERY = 0.95`, root-caused to frame pairing between free-running clocks
> (§8). The owner took the trade and the ingest path now runs a **frame synchronizer**
> (permanent one-frame cushion, like a hardware switcher input): delivery **99%**,
> frames overwritten **0.0%**, repeats ~0, for a deliberate **+16.7 ms** of
> source→program latency. §8 has the numbers and the two refuted attempts that came
> first.
>
> **Correction, recorded deliberately:** the first Windows answer on the §3 follow-up
> was **wrong** — it reported 200 µs as refuted (59.6 fps, 74% delivery). That binary
> had been built from a tree still carrying the "before" edits (`git checkout main`
> carried the uncommitted reverts across), so it was *before + a 200 µs guard*: the
> 74% was the 8 ms poll and the 59.6 fps was the un-anchored pacer, both misattributed
> to the guard. Rebuilt from clean main, the result reversed. Lesson for the next
> before/after on this rig: **`git status` immediately before every measurement build**
> — a stale working tree silently answers a different question than the one asked.

Paste-ready brief for a Claude session on the Windows machine.

The macOS port went through a measurement pass that found three defects in
**shared, platform-unguarded core code**. They are not macOS bugs. Windows
compiles the same files with no `_WIN32` guards on any of these paths, so it has
been shipping all three. **Nothing here has been measured on Windows** — the
point of this brief is to verify, not to assume.

The fixes exist on branch `mac-settings-scenesave-fixes`. Either pull that branch
or re-apply from the descriptions below.

---

## Context: why these were invisible

The headline macOS finding does **not** apply to Windows: the macOS core was
being compiled at `-O0` because every `cmake` configure site omitted
`CMAKE_BUILD_TYPE` and Ninja is a single-config generator. Windows passes
`--config Release` explicitly in `build-native-dev.ps1` and MSVC is multi-config,
so it cannot fall into this. Ignore it except as a cautionary tale.

What *does* apply is everything below, and all three hid behind a healthy-looking
FPS number. That is the theme: **mean fps is not a health metric.**

---

## 1. ~30% of decoded Zoom frames never reach the compositor (highest priority)

`native/src/modules/ZoomEngineRuntime.cpp` → `videoIngestLoop()` polled shared
memory every **8 ms**. The render thread fetches decoded frames on its own
~16.7 ms cadence. At 8 sources × 1080p60 the producer overwrites a participant's
slot before the render thread ever fetches it.

Measured on macOS at 8×1080p60: **672 of 960 decoded frames delivered (70%)**.
Every source silently loses a third of its motion while the FPS readout still
says 60.

Fix — drop the poll to 2 ms:

```cpp
void ZoomEngineRuntime::videoIngestLoop() {
  // Poll interval is pure ADDED LATENCY: a frame written to shared memory sits
  // unseen for up to this long. The locked peek is a 16-byte sequence read per
  // stream and the expensive snapshot only runs when a sequence changed, so
  // polling faster costs almost nothing.
  constexpr auto kVideoIngestPollMs = std::chrono::milliseconds(2);
  while (videoIngestRun_.load(std::memory_order_acquire)) {
    drainVideoStreamsThreePhase();
    std::this_thread::sleep_for(kVideoIngestPollMs);
  }
}
```

macOS result: delivery 70% → **99%**, and p99 ingest→render latency tightened
from ~20 ms to ~13 ms.

**Verify on Windows before/after.** Windows SHM is `CreateFileMapping` +
`MapViewOfFile` rather than POSIX `mmap`; the structural race is identical but
the magnitude may differ.

---

## 2. A 3.1 MB memcpy under the engine mutex

Same file, `publishVideoFrameLocked()`. Inside the block commented
*"Phase 3 (locked, cheap): publish"* it did:

```cpp
decoded.i420 = std::make_shared<const std::vector<std::uint8_t>>(frame.i420);
```

That copy-constructs the whole I420 plane set (3,110,400 bytes at 1080p) **while
holding `mutex_`**. The render thread calls `latestDecodedVideoFrames()` while
holding `coreMutex`, so it blocks behind that copy — a `coreMutex` → `mutex_`
stall on every tick, scaling with source count.

Fix: build the shared buffer in the **already-unlocked Phase 2**, where the
snapshot is taken, and move the pointer in Phase 3. `SnapshotResult` gains
`std::shared_ptr<const std::vector<std::uint8_t>> i420Shared`, populated with
`std::make_shared<...>(std::move(result.frame->i420))`, and
`publishVideoFrameLocked` takes it as a parameter and does `decoded.i420 =
std::move(i420)`.

This violates the repo's own documented law ("no pixel work under shared locks")
and the comment claiming the phase was cheap was simply wrong.

---

## 3. The render pacer drifts by construction — Windows has been compensating for it

`native/src/rpc/JsonRpcServer.cpp`, render loop. The frame deadline was:

```cpp
const auto deadline = t0 + std::chrono::microseconds(kFrameBudgetUs);
```

with `t0` read at the **top of every iteration**. That is a *relative* deadline:
each frame's overshoot becomes the next frame's start and is never reclaimed. It
can only lose time. macOS measured 17.27 ms/frame → **57.9 fps** on a tick with
10 ms of headroom.

**Windows has already been fighting this without naming it.** From the Windows
branch of the pacer:

> *"500us tail: the high-res timer still wakes ~300-400us late under load; a 200us
> guard measured 58.7fps (frames slipping past the deadline). 500us re-locks 60."*

58.7 fps is the same drift. It was masked by spinning 2.5× longer, and this repo
already documents that spinning in hot loops is what made *other applications'
audio glitch* during the virtual-camera work.

Fix — accumulate from a fixed anchor with bounded catch-up:

```cpp
// before the loop
constexpr long long kFrameBudgetUs = 16666;
constexpr int kMaxCatchUpFrames = 3;
auto nextDeadline = std::chrono::steady_clock::now() +
                    std::chrono::microseconds(kFrameBudgetUs);

// inside: use it instead of t0 + budget
const auto deadline = nextDeadline;

// after pacing
nextDeadline += std::chrono::microseconds(kFrameBudgetUs);
const auto afterPace = std::chrono::steady_clock::now();
if (nextDeadline + std::chrono::microseconds(kFrameBudgetUs * kMaxCatchUpFrames) < afterPace) {
  nextDeadline = afterPace + std::chrono::microseconds(kFrameBudgetUs);  // re-anchor
}
```

macOS result: **60.0 fps, 0 dropped frames.**

**The Windows-specific follow-up:** with the drift gone, try lowering
`kSpinGuardUs` from 500 µs back toward 200 µs (or lower) and confirm 60 fps
holds. That reclaims CPU in the exact loop previously implicated in glitching
other apps' audio. Measure it — don't just lower it.

---

## 4. The test rig could never reproduce a real show

`native/zoom-engine/fake/fake-engine.cpp` is shared, so Windows inherits these:

- Roster was **capped at 6 participants** (`if (baseline > 6) baseline = 6;`).
  The owner's Zoom account does **8 × 1080p60** (BO100 flag), so the full wall
  was never expressible. Cap raised to 16.
- Producer was **hardcoded 30 fps** (`next += milliseconds(33)`). Every perf
  number ever taken with this rig was at half the real frame rate. Now
  `COREVIDEO_FAKE_ENGINE_FPS` (default 30 for back-compat with the audio/ISO
  validators; drills should ask for 60).
- Producer repainted all 3.1 MB per frame with per-pixel integer divides, which
  made the *rig* the bottleneck (~690 MB/s ceiling, ~22 fps/source). Now paints
  once and advances a moving band, so it sustains a verified 60.0 ticks/s × 8
  targets = **1.49 GB/s**.

Env knobs: `COREVIDEO_FAKE_ENGINE_PARTICIPANTS` (max 16),
`COREVIDEO_FAKE_ENGINE_RES` (2 = 1080p), `COREVIDEO_FAKE_ENGINE_FPS`,
`COREVIDEO_FAKE_NO_CHURN=1`, `COREVIDEO_FAKE_ENGINE_LOG=<path>` (the log now
reports achieved ticks/s — **always confirm the rig actually delivered what you
asked for**).

---

## 5. Measurement traps — every one of these produced a wrong answer first

1. **Compositor upload counters are not an ingest rate.** They sit behind
   throttles (the multiview composite runs every Nth tick), so a throttle reads
   as a delivery failure. Instrument at the decode boundary instead.
2. **Sample each frame exactly once.** `latestDecodedFrames_` re-serves the held
   frame on ticks where nothing new arrived; sampling every fetch measures
   staleness, not latency. Dedup on `frameId` per participant.
3. **Survivorship bias.** The first "good" p50 (4.3 ms) only sampled frames fast
   enough to survive — the 30% that were overwritten never appeared in the
   sample at all. A flattering latency number was *hiding* the delivery defect.
4. **Mean fps hides judder.** One 33 ms frame plus one 0 ms frame averages a
   perfect 60. Count frames whose interval exceeded 1.5× budget, and track the
   worst.
5. **Profile before theorizing.** Three hypotheses died to inspection here
   (event serialization, buffer deallocation, allocator thrash). `sample <pid>`
   on macOS answered it in one shot; use ETW / Windows Performance Recorder, or
   `dotnet-trace` for the shell side.

---

## 6. Suggested Windows work, in order

1. **Reproduce the delivery loss.** Build `corevideo-zoom-engine-fake.exe`, point
   the core at it via `COREVIDEO_ZOOM_ENGINE_PATH`, run 8 participants at
   1080p60, and instrument how many decoded frames per second reach the
   compositor vs how many the producer wrote. Expect ~70% before the fix.
2. Apply fixes 1–3 and re-measure the same three numbers: sustained fps, dropped
   frames, frame delivery %.
3. Try the `kSpinGuardUs` reduction and measure CPU in that loop.
4. **Port `scripts/mac-show-drill.py`.** It is Python over stdio JSON and mostly
   portable; the real work is the core binary path and the fake-engine path. It
   gates: sustained output rate, zero dropped frames, ≥95% frame delivery,
   latency p50 ≤ 1 frame / p99 ≤ 2 frames, and `coreMutex` over-budget ratio.
   Wire it into the Windows CI job so shared-code perf regressions fail a build
   on both platforms — a macOS-motivated throttle in shared `MediaCore.cpp` would
   otherwise silently degrade the Windows multiview.

---

## 7. Two things worth measuring on Windows that macOS cannot answer

- **`media-core-sync` lock hold.** A comment in `MediaCore.cpp` states this holds
  the core lock **50–100 ms**. If true, that is a spike in the *command* path —
  Take/cut responsiveness — invisible to every frame-path metric above. Worth an
  independent measurement.
- **Capture-card sources.** The SHM capture bridge
  (`WinUiCaptureDeviceAdapter`) is `#ifdef _WIN32`, so Windows is the **only**
  platform that can currently drill synthetic capture-card load. The owner runs
  8–10 capture-card shows. macOS has no headless path for this at all.

---

## Reference numbers (macOS, Apple Silicon, 8 × 1080p60, optimized)

| Metric | Value |
|---|---|
| Ingest into core | 480 frames/s, 1.49 GB/s |
| Sustained render | 60.0 fps, 0 dropped |
| coreMutex hold / tick | ~6 ms of a 16.7 ms budget |
| Frame delivery | 99% |
| Source→render latency | p50 3.9 ms, p99 16.4 ms |

Use these as a sanity reference, **not** as a Windows target — different GPU,
timer, and SHM implementation.

---

## 8. The residual delivery gap, chased (Windows, 2026-08-05)

**It is frame pairing between two free-running ~60 Hz clocks, not lost work.**

`ZoomEngineRuntime` now counts what happens to every decoded frame at the
ingest→render handoff and prints it per 2 s window (`[zoom-slot]`, surfaced by the
drill):

```
[zoom-slot] published=966 fresh=894 overwritten=72 (7.5%) starved=90 over 2.02s
[zoom-slot] published=940 fresh=869 overwritten=71 (7.6%) starved=91 over 2.00s
[zoom-slot] published=967 fresh=879 overwritten=88 (9.1%) starved=81 over 2.00s
```

`overwritten` = a decoded frame destroyed before the compositor took it (real lost
motion). `starved` = a render tick that re-showed a frame it already had. **They are
equal in every window**, which is the whole diagnosis: the producer is not outrunning
the render (59.98–60.09 f/s against 60.0 fps). Two frames land inside one render
interval — so one dies in the latest-wins slot — and the neighbouring interval gets
none, so that tick repeats. ~1 ms of jitter on either clock is enough, on a 16.7 ms
period. The operator sees micro-judder (a repeat plus a skip roughly every 14 frames),
**not** the "a third of every source's motion" that the 8 ms poll was costing.

Two fixes were implemented and **both refuted by measurement**:

1. **Halve the ingest poll (2 ms → 1 ms)** — theory was that poll quantization
   dominated the jitter. Result: 6.6–7.5% overwritten vs 6.6–9.9%. No effect. Poll
   quantization is one of three jitter sources (producer, poll, render), not the
   dominant one. Reverted.
2. **A one-deep catch-up buffer** (park a frame that arrives while the slot is still
   unfetched; promote it on the next starved tick). Result: **no benefit** —
   7.5–9.1% overwritten, unchanged. The starved tick arrives *before* the surplus
   frame, so there is nothing in reserve at the moment it is needed. Reverted; the
   `latestDecodedFrames_` header comment records this so it is not retried blind.

   Worth knowing how that one nearly shipped: the first cut of it measured **97%
   delivery** and looked like a clean win. It was a bug — a newly published frame
   could bypass an occupied pending slot, stranding the parked frame until some later
   tick surfaced it **out of order**, which the latency percentile caught as a
   **796 ms p99**. Delivery alone said "success". Same lesson as §5, one layer up:
   never accept a single metric's verdict on a change that can trade one axis for
   another.

**What actually closed it (owner decision, 2026-08-06): a frame synchronizer.** Always
run one frame behind, so a starved tick has a reserve to draw on. That is what a
hardware switcher input does. It is NOT the catch-up buffer above: the cushion is
built up front (prime to two queued frames, then serve one per fetch) so the reserve
exists *before* it is needed. Net rates being equal, the queue holds steady at one, a
gap draws on the reserve instead of repeating, and a double refills it instead of
dropping. `ZoomEngineRuntime::frameSync_`, capped at 3 deep — on sustained overflow
the OLDEST frame is dropped, so latency can never accumulate.

**Hold the cushion at its target — do not merely inherit it.** Frames pile up before the
render thread starts pulling a newly joined source, and draining one-per-tick from a
deep queue locks that startup backlog in as *permanent* latency. The first cut did
exactly that and measured p50 27 ms on one run and 37 ms on the next, purely on join
timing. The fetch now discards anything beyond the cushion, so a frame sync resyncs
instead of falling behind. This is the whole difference between "one frame, always" and
"one to three frames, depending on when you joined."

Measured at 8 × 1080p60, same rig, 30 s runs (3 runs with frame sync):

| Metric | latest-wins (`COREVIDEO_FRAME_SYNC=0`) | frame sync (default) |
|---|---|---|
| Frames overwritten | 7.5–9.1% | **0.0–0.7%** |
| Starved ticks / 2 s | 80–104 | **0–16** |
| Delivery | 88–92% | **98–100%** |
| Sustained | 59.9 fps, 0 dropped | 60.0 fps, 0 dropped |
| ingest→render p50 / p99 | 10.9 / 20.5 ms | **17.8–26.9 / 29.9–37.0 ms** |
| Drill verdict | FAIL (delivery) | **SHOW DRILL PASSED** |

The added latency is the cushion — one frame — plus whatever sub-frame phase offset the
run happens to start with, which is why p50 varies run to run.
`COREVIDEO_FRAME_SYNC=0` restores the old behaviour and is the A/B control — it is kept
working on purpose.

The drill's latency budgets were raised by one frame to match (16.7/33.3 → 33.3/50 ms).
That is a gate tracking a deliberate architecture change, **not** a red gate being
lowered to go green: `MIN_FRAME_DELIVERY` was left at 0.95 and now passes on its own at
99%. If frame sync is ever disabled by default, put the latency budgets back.

**A/V implication to confirm on the rig.** Video content now reaches the encoder one
frame later while audio is unchanged, so audio leads video by ~16.7 ms more than before
in recordings and live outputs. `validate-record-audio.mjs` stays green (start delta
1.5 ms, duration delta 104 ms) but it measures stream alignment, not content sync — it
would not see a one-frame content shift. The real check is the owed **G2 clap test** on
a live meeting. One frame is inside the 50 ms budget and normally imperceptible, but
audio-lead is the more noticeable direction, so if the clap test shows it mattering the
fix is to compensate the recording PTS by the known cushion.
