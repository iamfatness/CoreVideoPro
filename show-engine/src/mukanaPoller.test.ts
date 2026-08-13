/**
 * Direct unit tests for `MukanaPoller`, carved out of `showEngine.ts`'s
 * "ShowEngine Mukana polling" suite (Task 1). These four tests are a
 * relocation, not new coverage: each pins one of the mechanisms that suite
 * used to exercise indirectly through `ShowEngine.tick()` + a real
 * `MukanaClient` + a fake `fetch` — the due-check against `nextDelayMs`, the
 * non-blocking/never-awaits contract, the per-endpoint busy gate, and the
 * fire-and-forget rejection handling. Testing `MukanaPoller` directly here
 * removes the `MukanaClient.request()`-internals-vs-`flush()` indirection
 * `showEngine.test.ts`'s own doc comment describes, and makes the "never
 * awaits" contract checkable precisely: `poll()` is declared to return
 * `void`, so a mutation that makes it `async` and genuinely awaits a fetch
 * is caught by asserting the call itself returns `undefined`, not a pending
 * `Promise`, regardless of whether any caller happens to await it.
 *
 * `showEngine.test.ts`'s own "ShowEngine Mukana polling" suite keeps the
 * tests that verify INTEGRATION — that `tick()` wires `poll()`/`drain()`'s
 * outcomes into `mukanaRegistry`/`this.queue`/`this.question`/capabilities
 * exactly as before.
 */

import { describe, expect, it } from "vitest";
import type { Clock } from "./clock.js";
import type { MukanaConfig, ShowIntegrationsConfig } from "./config.js";
import { MukanaClient } from "./mukanaClient.js";
import type { FetchResponse, MukanaEndpoint } from "./mukanaClient.js";
import { MukanaPoller } from "./mukanaPoller.js";

/** Matches Task 9's brief: all three integrations enabled, matching the moved tests' original rig. */
const ALL_ON: ShowIntegrationsConfig = { registry: true, handsQueue: true, questionFeed: true };

/** Drains 8 microtask turns — same depth `showEngine.test.ts`'s own `flush()` uses. */
async function flush(): Promise<void> {
  for (let i = 0; i < 8; i += 1) {
    // eslint-disable-next-line no-await-in-loop
    await Promise.resolve();
  }
}

/**
 * A `MukanaClient`-shaped fake whose three fetch methods never settle on
 * their own — each call is recorded and its resolver/rejecter queued, so a
 * test can decide whether/when it ever answers. `nextDelayMs` is fixed by
 * the caller so due-check timing is exact rather than derived from backoff.
 */
function fakeClient(delayMs: number) {
  const calls: Record<MukanaEndpoint, number> = { panelists: 0, hands: 0, question: 0 };
  const pending: Record<MukanaEndpoint, Array<{ resolve: (v: unknown) => void; reject: (e: unknown) => void }>> = {
    panelists: [],
    hands: [],
    question: []
  };

  function makeFetch(endpoint: MukanaEndpoint) {
    return () =>
      new Promise((resolve, reject) => {
        calls[endpoint] += 1;
        pending[endpoint].push({ resolve, reject });
      });
  }

  const client = {
    get health() {
      return {
        panelists: { state: "ok", consecutiveFailures: 0, detail: null },
        hands: { state: "ok", consecutiveFailures: 0, detail: null },
        question: { state: "ok", consecutiveFailures: 0, detail: null }
      };
    },
    healthFor: () => ({ state: "ok", consecutiveFailures: 0, detail: null }),
    nextDelayMs: () => delayMs,
    fetchPanelists: makeFetch("panelists"),
    fetchHands: makeFetch("hands"),
    fetchQuestion: makeFetch("question")
  } as unknown as MukanaClient;

  return {
    client,
    calls,
    totalCalls: (): number => calls.panelists + calls.hands + calls.question,
    /** Settle the oldest still-pending fetch for `endpoint` with a benign `invalid` outcome (never `data` — these tests only care about scheduling/safety, not applied payloads). */
    settle: (endpoint: MukanaEndpoint): void => {
      const resolver = pending[endpoint].shift();
      resolver?.resolve({ kind: "invalid", reason: "test fixture: no real data" });
    },
    reject: (endpoint: MukanaEndpoint, error: unknown): void => {
      const resolver = pending[endpoint].shift();
      resolver?.reject(error);
    }
  };
}

function movingClock(startMs = 0): Clock & { advance: (ms: number) => void } {
  let nowMs = startMs;
  return {
    now: () => nowMs,
    advance: (ms: number) => {
      nowMs += ms;
    }
  };
}

describe("MukanaPoller", () => {
  it("polls an endpoint only once its next delay has elapsed", async () => {
    const { client, totalCalls, settle } = fakeClient(1000);
    const clock = movingClock();
    const poller = new MukanaPoller({ client, clock, integrations: ALL_ON });

    poller.poll();
    const first = totalCalls();
    expect(first).toBeGreaterThan(0);
    // Settle everything that started so the busy gate can't be what's
    // preventing a second fetch below — this test isolates the due-check
    // from the busy gate (that's the next test).
    settle("panelists");
    settle("hands");
    settle("question");
    await flush();

    poller.poll();
    await flush();
    expect(totalCalls()).toBe(first);

    clock.advance(10_000);
    poller.poll();
    await flush();
    expect(totalCalls()).toBeGreaterThan(first);
  });

  /**
   * `forceDue()` — the mechanism behind `ShowEngine.syncAll()` — resets
   * every endpoint's due-check anchor without starting any fetch itself.
   * Pinned with a long interval (an hour) so "nothing is naturally due" is
   * unambiguous: the baseline `poll()` with no clock advance confirms that,
   * then `forceDue()` alone — still no clock advance — is what makes the
   * NEXT `poll()` start every endpoint anyway.
   */
  it("forceDue makes every endpoint due on the next poll without waiting out its interval", async () => {
    const { client, totalCalls, settle } = fakeClient(3_600_000);
    const clock = movingClock();
    const poller = new MukanaPoller({ client, clock, integrations: ALL_ON });

    poller.poll();
    const first = totalCalls();
    expect(first).toBeGreaterThan(0);
    settle("panelists");
    settle("hands");
    settle("question");
    await flush();

    poller.poll(); // no clock advance: nothing is due yet
    await flush();
    expect(totalCalls()).toBe(first);

    poller.forceDue();
    poller.poll(); // still no clock advance
    await flush();
    expect(totalCalls()).toBeGreaterThan(first);
  });

  /**
   * The invariant this must break on: awaiting a fetch inside `poll()`. A
   * hung registry must not stall the caller — spec §2, normative. Asserted
   * precisely: `poll()` is declared to return `void`; a mutation that makes
   * it `async` and genuinely `await`s a fetch makes the call itself return a
   * `Promise` instead of `undefined`, caught here regardless of whether
   * `ShowEngine` happens to await the result.
   */
  it("completes a tick while a fetch is still in flight", () => {
    const { client } = fakeClient(1000);
    const poller = new MukanaPoller({ client, clock: movingClock(), integrations: ALL_ON });

    const returnedFirst = poller.poll();
    expect(returnedFirst).toBeUndefined();
    const returnedSecond = poller.poll();
    expect(returnedSecond).toBeUndefined();
  });

  /**
   * Fix round 1, Finding 3/4/5: the busy gate this test pins is what makes
   * "one in-flight promise per endpoint" a real invariant instead of a
   * comment. Drives a hung `hands` fetch, then advances the clock and calls
   * `poll()` repeatedly — `nextDelayMs` alone would call every one of those
   * calls "due," so without the gate a fresh overlapping fetch starts on
   * each one (the reviewer's probe: 20 concurrent hung fetches over 20
   * ticks, none retired — unbounded concurrent load against an
   * already-struggling registry, and settle-order-not-start-order
   * application that can let a stale fetch overwrite a fresher one). With
   * the gate, exactly one fetch for `hands` is ever outstanding while the
   * first hasn't settled.
   */
  it("never starts a second fetch for an endpoint while one is still in flight", () => {
    const { client, calls } = fakeClient(1000);
    const clock = movingClock();
    const poller = new MukanaPoller({ client, clock, integrations: ALL_ON });

    poller.poll();
    expect(calls.hands).toBe(1);

    for (let i = 0; i < 5; i += 1) {
      clock.advance(10_000);
      poller.poll();
    }
    expect(calls.hands).toBe(1);
  });

  /**
   * Fix round 1, Finding 6: a rejected fetch — a `FetchLike` contract
   * violation, or anything else outside what `MukanaClient.request`'s own
   * try/catch covers — must never become an unhandled promise rejection
   * (spec §2's whole point: a broken third-party integration cannot be
   * allowed to take the process down). Every `.then()` inside `poll()` must
   * supply a rejection handler for every one of the three fetches, or this
   * becomes an unhandled promise rejection — under Node's default
   * `--unhandled-rejections=throw`, that kills the process. Verified by
   * actually listening for `unhandledRejection` for the duration of the
   * test, not by inference.
   *
   * Node defers its "was this rejection ever handled" check past a
   * microtask-queue drain — `flush()` alone is not enough to observe it
   * reliably from inside the test. One real macrotask turn (`setImmediate`)
   * after the flush is what actually lands inside Node's check window
   * before the listener is removed.
   */
  it("never leaves an unhandled promise rejection when a fetch's promise rejects", async () => {
    const { client, reject } = fakeClient(1000);
    const poller = new MukanaPoller({ client, clock: movingClock(), integrations: ALL_ON });

    const rejections: unknown[] = [];
    const onUnhandledRejection = (reason: unknown): void => {
      rejections.push(reason);
    };
    process.on("unhandledRejection", onUnhandledRejection);
    try {
      poller.poll();
      reject("hands", new Error("contract violation: text() did not resolve to a string"));
      await flush();
      await new Promise<void>((resolve) => {
        setImmediate(resolve);
      });
    } finally {
      process.off("unhandledRejection", onUnhandledRejection);
    }

    expect(rejections).toEqual([]);
  });
});

/**
 * Task 3: closes the hung-endpoint obligation the previous plan carried
 * forward. Two defects in that plan's fix:
 *
 *   1. No hysteresis — one good answer right after a hang flipped
 *      `boxFill`/availability straight back, so an endpoint hovering near
 *      the hung threshold could flap the guest boxes every poll cycle.
 *   2. The `MUKANA_HUNG_POLL_INTERVALS` constant's lower bound was unpinned
 *      — the existing "ordinary slowness is not an outage" test asserted at
 *      `outstanding == 0` (the instant a fetch STARTS), which stays green
 *      even at `MUKANA_HUNG_POLL_INTERVALS = 1`.
 *
 * These tests use a REAL `MukanaClient` + `MukanaPoller` pair (not the
 * `fakeClient()` double above) because the hysteresis bookkeeping under
 * test — `MukanaClient.markHung`/`applyHealth` — lives inside
 * `MukanaClient` itself; a fake here would just re-test a hand-written copy
 * of the production logic instead of the logic.
 */

/**
 * A REAL `MukanaClient` + `MukanaPoller` pair wired to a controllable
 * `hands` fetch. Only `hands` is enabled (`registry`/`questionFeed` off) so
 * the shared `fetch` only ever needs to answer one endpoint, and every
 * settle below is deliberately routed through this ONE endpoint so a test
 * can track its capability as a simple `"ok" -> available` / anything else
 * `-> unavailable` binary (mirroring `resolveCapability` in
 * `capabilities.ts`, which this file deliberately does not import — the
 * property below is about `MukanaPoller`/`MukanaClient`'s OWN published
 * health, not `ShowEngine`'s wiring of it, which stays untouched by Task 3
 * by design: see `MukanaPoller.detectHungEndpoints`'s own doc comment).
 */
function realHandsRig(intervalMs: number) {
  const config: MukanaConfig = {
    baseUrl: "https://example.com/rest.php",
    event: "test",
    panelistsIntervalMs: intervalMs,
    handsIntervalMs: intervalMs,
    questionIntervalMs: intervalMs,
    maxBackoffMs: 60_000
  };
  const pending: Array<{
    resolve: (response: FetchResponse) => void;
    reject: (error: unknown) => void;
  }> = [];
  const fetch = (): Promise<FetchResponse> =>
    new Promise((resolve, reject) => {
      pending.push({ resolve, reject });
    });
  const client = new MukanaClient(config, { fetch });
  const clock = movingClock();
  const integrations: ShowIntegrationsConfig = {
    registry: false,
    handsQueue: true,
    questionFeed: false
  };
  const poller = new MukanaPoller({ client, clock, integrations });

  return {
    client,
    poller,
    clock,
    pendingCount: (): number => pending.length,
    // Both throw loudly on an empty queue (fix round 1, Minor 5) rather
    // than silently no-op via `?.` — `runCycle` below already models the
    // right shape: a broken due-check upstream must fail the test that
    // called this, not make the settle silently vanish and leave the next
    // assertion passing for the wrong reason (health simply never changes).
    settleHealthy: (): void => {
      const next = pending.shift();
      if (next === undefined) {
        throw new Error("realHandsRig.settleHealthy: no hands fetch is pending");
      }
      next.resolve({ ok: true, status: 200, text: async () => "4242,5555\n1383\nNONE" });
    },
    settleInvalid: (): void => {
      const next = pending.shift();
      if (next === undefined) {
        throw new Error("realHandsRig.settleInvalid: no hands fetch is pending");
      }
      next.resolve({ ok: false, status: 503, text: async () => "nope" });
    }
  };
}

/** `"ok"` is `available`, anything else (`"failing"`/`"dormant"`) is `unavailable` — `resolveCapability`'s own binary, mirrored here for `hands` only (every rig above force-enables it, nothing else). */
function handsCapability(client: MukanaClient): "available" | "unavailable" {
  return client.healthFor("hands").state === "ok" ? "available" : "unavailable";
}

/**
 * Runs one full hands poll cycle: starts the (assumed due) fetch, then
 * waits `latencyMultiplier` x `intervalMs` before settling it healthily —
 * checking `poller.detectHungEndpoints()` (and sampling capability) every
 * `checkpointMs` along the way, the way a real `ShowEngine.tick()` would
 * re-check health on every tick regardless of whether THIS poll just
 * started. A single check right at the end would miss a hang that both
 * crosses the threshold AND settles within the same cycle (`multiplier`
 * just over 3, e.g. 3.1x) — the exact marginal case defect 1 is about.
 *
 * Pads the clock back up to a full `intervalMs` since THIS cycle's poll
 * started before returning, so the NEXT `runCycle` call's own `poll()`
 * always finds its due-check satisfied, independent of how fast this one
 * settled (`latencyMultiplier < 1` settles before a full interval has
 * elapsed since the poll started).
 *
 * Throws (loudly, not a silent no-op) if `poll()` didn't actually start a
 * fetch — the due-check failing here would otherwise make the whole cycle
 * vacuous: `settleHealthy()` would resolve nothing, health would never
 * change, and every assertion downstream would pass for the wrong reason.
 */
async function runCycle(
  rig: ReturnType<typeof realHandsRig>,
  intervalMs: number,
  latencyMultiplier: number,
  checkpointMs: number,
  onSample: () => void
): Promise<void> {
  const before = rig.pendingCount();
  rig.poller.poll();
  if (rig.pendingCount() !== before + 1) {
    throw new Error("runCycle: poll() did not start a new hands fetch — due-check was not satisfied");
  }
  const pollStartedAt = rig.clock.now();
  const settleAtMs = latencyMultiplier * intervalMs;
  let elapsed = 0;
  while (elapsed < settleAtMs) {
    const step = Math.min(checkpointMs, settleAtMs - elapsed);
    rig.clock.advance(step);
    elapsed += step;
    rig.poller.detectHungEndpoints();
    onSample();
  }
  rig.settleHealthy();
  await flush();
  onSample();
  const sincePollStart = rig.clock.now() - pollStartedAt;
  if (sincePollStart < intervalMs) {
    rig.clock.advance(intervalMs - sincePollStart);
  }
}

describe("MukanaPoller hung-poll threshold pinning (Task 3, fix round 2 obligation 2)", () => {
  /**
   * The lower-bound pin. The previous plan's own "ordinary slowness is not
   * an outage" test checked this at `outstanding == 0` — the instant the
   * fetch STARTS, before any real time has passed — which cannot tell
   * `MUKANA_HUNG_POLL_INTERVALS = 3` from `= 1`: both leave a
   * zero-outstanding poll looking fine. This test waits a REAL 2500ms
   * (1.25x a 2000ms interval — genuinely slow, but well under 3x) before
   * checking, so it reds exactly when the constant collapses to 1 and no
   * longer covers that wait.
   */
  it("keeps hands available across ordinary slowness well under the hung threshold", async () => {
    const rig = realHandsRig(2000);
    rig.poller.poll();
    rig.settleHealthy();
    await flush();
    expect(handsCapability(rig.client)).toBe("available");

    rig.clock.advance(2000); // due
    rig.poller.poll();
    rig.clock.advance(2500); // outstanding = 2500ms, 1.25x the interval — still < 3x
    const hung = rig.poller.detectHungEndpoints();

    expect(hung).toEqual([]);
    expect(handsCapability(rig.client)).toBe("available");
  });

  /**
   * The upper-bound pin (companion to the lower-bound test above): at the
   * shipped threshold of 3, a poll outstanding 6001ms against a 2000ms
   * interval (3x + 1ms) IS reported hung. Reds at
   * `MUKANA_HUNG_POLL_INTERVALS = 30` (6001ms is nowhere near 30x2000ms =
   * 60000ms), pinning the constant can't drift upward unnoticed either.
   */
  it("reports hands hung and unavailable once outstanding for the hung threshold", async () => {
    const rig = realHandsRig(2000);
    rig.poller.poll();
    rig.settleHealthy();
    await flush();
    expect(handsCapability(rig.client)).toBe("available");

    rig.clock.advance(2000); // due
    rig.poller.poll();
    rig.clock.advance(6001); // 3 x 2000ms + 1ms
    const hung = rig.poller.detectHungEndpoints();

    expect(hung).toEqual([{ endpoint: "hands", outstandingMs: 6001 }]);
    expect(rig.client.healthFor("hands")).toEqual({
      state: "failing",
      consecutiveFailures: 0,
      detail: "no response after 6001ms with a poll still in flight"
    });
    expect(handsCapability(rig.client)).toBe("unavailable");
  });
});

describe("MukanaClient hang-recovery hysteresis (Task 3, fix round 2 obligation 1)", () => {
  /**
   * The direct pin for `MUKANA_RECOVERY_SETTLES = 2`. Reds at
   * `MUKANA_RECOVERY_SETTLES = 1`: with that mutation, the hung fetch's own
   * eventual healthy answer (the FIRST settle after degradation) would
   * already be enough, and `handsCapability` would read `"available"`
   * immediately — contradicting the `"unavailable"` assertion right after
   * that settle, below.
   *
   * Also pins the mid-hold detail string exactly (fix round 1, Minor 1 —
   * it was unpinned; mutating it to garbage left the full suite green,
   * even though it is the ONLY explanation an operator gets for why the
   * queue is still sitting in manual fallback after the feed has already
   * answered once).
   */
  it("holds hands degraded through exactly one healthy settle, and clears it on the second", async () => {
    const rig = realHandsRig(2000);
    rig.poller.poll();
    rig.settleHealthy();
    await flush();
    expect(handsCapability(rig.client)).toBe("available");

    rig.clock.advance(2000);
    rig.poller.poll();
    rig.clock.advance(6001);
    rig.poller.detectHungEndpoints(); // detects the hang, arms the recovery hold
    expect(handsCapability(rig.client)).toBe("unavailable");

    // The hung fetch FINALLY answers — settle #1 of the recovery streak.
    rig.settleHealthy();
    await flush();
    expect(rig.client.healthFor("hands")).toEqual({
      state: "failing",
      consecutiveFailures: 0,
      detail: "recovered from a hang but not yet trusted — 1 more healthy poll needed"
    });
    expect(handsCapability(rig.client)).toBe("unavailable");

    // A second, on-time, healthy poll cycle — settle #2. ONLY now is the
    // endpoint trusted again.
    rig.clock.advance(2000);
    rig.poller.poll();
    rig.clock.advance(2000);
    rig.settleHealthy();
    await flush();
    expect(rig.client.healthFor("hands")).toEqual({
      state: "ok",
      consecutiveFailures: 0,
      detail: null
    });
    expect(handsCapability(rig.client)).toBe("available");
  });

  /** A settle that FAILS mid-hold breaks the streak: the next healthy settle after it is only settle #1 again, not a continuation. */
  it("resets the recovery streak on a failed settle mid-hold", async () => {
    const rig = realHandsRig(2000);
    rig.poller.poll();
    rig.settleHealthy();
    await flush();

    rig.clock.advance(2000);
    rig.poller.poll();
    rig.clock.advance(6001);
    rig.poller.detectHungEndpoints();
    rig.settleHealthy(); // settle #1 of 2
    await flush();
    expect(handsCapability(rig.client)).toBe("unavailable");

    rig.clock.advance(2000);
    rig.poller.poll();
    rig.settleInvalid(); // breaks the streak — NOT settle #2
    await flush();
    expect(handsCapability(rig.client)).toBe("unavailable");
    // The failed settle also bumped `consecutiveFailures` to 1, so the NEXT
    // due-check needs the backed-off delay (2000 x 2^1 = 4000ms), not the
    // plain interval — `nextDelayMs` backoff is orthogonal to, and stacks
    // with, the recovery hold under test here.
    rig.clock.advance(4000);
    rig.poller.poll();

    // Two MORE consecutive healthy settles are required from here, not one.
    rig.settleHealthy();
    await flush();
    expect(handsCapability(rig.client)).toBe("unavailable"); // only 1 of 2 since the reset

    rig.clock.advance(2000);
    rig.poller.poll();
    rig.settleHealthy();
    await flush();
    expect(handsCapability(rig.client)).toBe("available"); // 2 of 2 since the reset
  });
});

/**
 * THE property: the published `handsQueue` capability never changes state
 * more than once per `MUKANA_RECOVERY_SETTLES`-worth of settles. Quantified
 * — deliberately, because the previous plan's version of this guarantee
 * held latency constant (either "settles instantly" or "never settles at
 * all") and so never exercised the marginal band right around the hung
 * threshold where the false-positive flapping (defect 1) actually lives.
 *
 * Ranged over, per the brief:
 *   - every latency multiplier named there — 0.5x/1x/2x (comfortably under
 *     the 3x hung threshold), 2.9x (just under), 3.1x (just over), 10x (a
 *     genuine hang) — as the latency of ONE poll cycle that eventually
 *     answers CORRECTLY, just slowly (the exact shape of defect 1: "An
 *     endpoint answering correctly after ~6.5s... flips boxFill
 *     queue -> manual -> queue");
 *   - crossed with recovering after exactly 1 total healthy settle and
 *     exactly 2 (the shipped `MUKANA_RECOVERY_SETTLES`) — the first
 *     settle after degradation IS the slow cycle's own eventual answer, so
 *     "recover after 1" runs no further cycles and "recover after 2" runs
 *     exactly one more, on-time, healthy one.
 *
 * The invariant under test: for a genuinely slow-but-correct endpoint
 * (latency < 3x), the capability must NEVER go unavailable at all — zero
 * transitions, the false-positive half of defect 1. For a genuinely hung
 * one (>= 3x), it goes unavailable EXACTLY once, and comes back available
 * only once `MUKANA_RECOVERY_SETTLES` total healthy settles have landed —
 * never earlier, never oscillating in between.
 */
describe("published hands capability never flaps faster than the recovery window (Task 3 property)", () => {
  const INTERVAL_MS = 2000;
  const CHECKPOINT_MS = 250;
  const LATENCY_MULTIPLIERS = [0.5, 1, 2, 2.9, 3.1, 10];

  for (const latencyMultiplier of LATENCY_MULTIPLIERS) {
    for (const totalRecoverySettles of [1, 2] as const) {
      it(`latency ${latencyMultiplier}x the interval, recovering after ${totalRecoverySettles} total settle(s)`, async () => {
        const rig = realHandsRig(INTERVAL_MS);
        const log: Array<"available" | "unavailable"> = [];
        const sample = (): void => {
          log.push(handsCapability(rig.client));
        };

        // Warm up to a genuinely healthy baseline before the cycle under test.
        rig.poller.poll();
        rig.settleHealthy();
        await flush();
        sample();
        expect(log[0]).toBe("available");

        // The cycle under test — settle #1 of the recovery streak, if this
        // one degrades the endpoint at all.
        rig.clock.advance(INTERVAL_MS); // due
        await runCycle(rig, INTERVAL_MS, latencyMultiplier, CHECKPOINT_MS, sample);

        // Additional on-time, healthy cycles up to `totalRecoverySettles`
        // total (the first settle was the cycle above).
        for (let settled = 1; settled < totalRecoverySettles; settled += 1) {
          // eslint-disable-next-line no-await-in-loop
          await runCycle(rig, INTERVAL_MS, 1, CHECKPOINT_MS, sample);
        }

        let transitions = 0;
        for (let i = 1; i < log.length; i += 1) {
          if (log[i] !== log[i - 1]) transitions += 1;
        }
        const wentUnavailable = log.includes("unavailable");

        if (latencyMultiplier < 3) {
          expect(wentUnavailable).toBe(false);
          expect(transitions).toBe(0);
          expect(log[log.length - 1]).toBe("available");
        } else {
          // MUKANA_RECOVERY_SETTLES is 2 — hardcoded here (not imported)
          // deliberately, so a mutation to that constant changes what
          // ACTUALLY happens without changing what this test EXPECTS.
          const recovered = totalRecoverySettles >= 2;
          expect(wentUnavailable).toBe(true);
          expect(transitions).toBe(recovered ? 2 : 1);
          expect(log[log.length - 1]).toBe(recovered ? "available" : "unavailable");
        }
      });
    }
  }
});
