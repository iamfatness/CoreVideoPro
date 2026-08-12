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
import type { ShowIntegrationsConfig } from "./config.js";
import type { MukanaClient, MukanaEndpoint } from "./mukanaClient.js";
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
