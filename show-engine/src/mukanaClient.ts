/**
 * Mukana REST client.
 * Performs a single fetch per call and reports how long the caller should wait
 * before the next one — the polling loop lives in the orchestrator, which keeps
 * this unit-testable without fake timers. Network failures back off
 * exponentially; a dormant registry is not a failure and does not back off.
 * Each of the three endpoints (panelists, hands, question) keeps its own
 * independent health record and interval, so a failure on one cannot affect
 * the backoff or health of the others. Each endpoint also keeps its own body
 * parser — panelists and question bodies are JSON, hands is a legacy
 * three-line text payload — so the shared `request()` takes the parser as a
 * parameter rather than assuming one shape for every endpoint.
 */

import type { MukanaConfig } from "./config.js";
import {
  detectDormantEnvelope,
  parseMukanaPanelists,
  parseMukanaQuestion,
  type DormantOutcome,
  type MukanaOutcome,
  type QuestionOutcome
} from "./mukanaParse.js";
import { parseHandsPayload, type HandsOutcome } from "./handsQueue.js";

export type FetchResponse = {
  ok: boolean;
  status: number;
  text: () => Promise<string>;
};

/**
 * Consecutive HEALTHY settles a hung-and-since-degraded endpoint must post
 * before `MukanaClient` trusts it as `"ok"` again (Task 3, closing the
 * obligation the previous plan's final review carried forward). Without a
 * hold like this, the instant a hung fetch FINALLY settles with real data,
 * health would flip straight back to `"ok"` on that single answer — and an
 * endpoint that answers correctly around ~6.5s (near the hung threshold at a
 * 2s interval) would flap `boxFill` `queue → manual → queue` on literally
 * every poll cycle, which on air is visible flickering of who is on screen.
 * `MukanaPoller.hungEndpoints()` arms this window the moment it decides a
 * poll is hung (`markHung`, below); `applyHealth` spends it down one healthy
 * settle at a time and resets it to the full window on any settle that
 * ISN'T healthy — the requirement is genuinely CONSECUTIVE good answers, not
 * merely this many somewhere in the endpoint's history.
 *
 * 2 rather than 1: one good answer proves the fetch itself completed, but
 * not that the endpoint has stopped being marginal — a single lucky
 * response is exactly what an endpoint hovering right at the hung threshold
 * would produce on its way to hanging again.
 */
export const MUKANA_RECOVERY_SETTLES = 2;

/**
 * The injected fetch. **A conforming implementation MUST supply a fetch
 * that honors `init.signal`** — abort the request (or otherwise settle its
 * returned promise) once the signal fires — because nothing in this
 * package can cancel it any other way. `MukanaClient.request` derives an
 * `AbortSignal` from the endpoint's OWN interval on every call
 * (`AbortSignal.timeout`, never the backoff-inflated `nextDelayMs`) and
 * passes it through as `init.signal`; the engine itself still holds no
 * timer of its own (its only notion of time is the injected `Clock`, read
 * inside `tick()`), so honouring that signal is entirely the host's
 * responsibility — this package cannot enforce it from inside.
 *
 * Why it matters, concretely: without a fetch that honors `signal`, a hung
 * endpoint's promise never settles, so `consecutiveFailures` never leaves
 * 0, backoff never engages, and `MukanaPoller`'s hung-poll rule
 * (`MUKANA_HUNG_POLL_INTERVALS`) is the ONLY thing standing between the
 * show and a frozen queue. And even that rule can only ever DEGRADE the
 * endpoint (report it `failing`, off the injected clock) — it can never
 * RECOVER it: the one-in-flight-per-endpoint busy gate is cleared only in
 * that SAME fetch's own `.then()`/rejection handler, so a promise that
 * never settles leaves the gate closed for the rest of the process's
 * life — no later fetch for that endpoint is ever even ATTEMPTED again,
 * whether or not the real backend has since recovered. **A host whose
 * fetch ignores `signal` therefore gets degradation but never recovery.**
 *
 * The package defends itself on both sides of that obligation rather than
 * trusting it — every endpoint starts `failing` (`initialHealth`, below) so
 * a never-answered endpoint is never reported as usable, and the engine
 * independently degrades an endpoint whose poll has been outstanding for
 * several of its own intervals (`ShowEngine.mukanaHealth`, fed by
 * `MukanaPoller.hungEndpoints`, which calls `markHung` below). Honouring
 * the signal is still required for genuine recovery: without it the
 * endpoint's health can only ever get stuck, never come back.
 */
export type FetchLike = (url: string, init?: { signal?: AbortSignal }) => Promise<FetchResponse>;

export type MukanaHealth = {
  state: "ok" | "dormant" | "failing";
  consecutiveFailures: number;
  detail: string | null;
};

export type MukanaEndpoint = "panelists" | "hands" | "question";

/** Every endpoint, in a fixed order — exported so a consumer can iterate health without re-listing the union. */
export const MUKANA_ENDPOINTS: readonly MukanaEndpoint[] = ["panelists", "hands", "question"];

/**
 * The three kinds every endpoint parser's outcome can take. Each concrete
 * parser's return type (`MukanaOutcome`, `QuestionOutcome`, `HandsOutcome`)
 * is structurally a member of this union — `data` carries endpoint-specific
 * payload, `invalid` always carries `reason`, and `dormant` (when present)
 * always carries `detail`.
 */
type ParseResult = { kind: "data" } | DormantOutcome | { kind: "invalid"; reason: string };

/**
 * Health for an endpoint that has never answered. **Pessimistic on
 * purpose** (final review, I1): health is written only when a request
 * SETTLES, so an optimistic `"ok"` start is a claim of usability the client
 * has no evidence for — and for an endpoint whose very first fetch hangs
 * (see `FetchLike`), a claim it never revisits. The measured consequence of
 * the optimistic version was not stale data but a degradation path that
 * never engaged: `resolveCapabilities` read `ok` → `available`,
 * `effectiveBoxFill` stayed `"queue"`, the queue was empty because nothing
 * ever arrived, and every guest box resolved to `null` for the whole show
 * while the operator's manual assignments sat ignored. Starting `failing`
 * makes an unanswered endpoint resolve to `unavailable`, which is the state
 * the rest of the package already knows how to fall back from.
 *
 * `consecutiveFailures: 0` is deliberate: nothing has actually failed yet,
 * and `nextDelayMs` reads that field, so a non-zero value here would
 * back-off the very first poll of a perfectly healthy registry.
 */
function initialHealth(): MukanaHealth {
  return { state: "failing", consecutiveFailures: 0, detail: "not polled yet" };
}

export class MukanaClient {
  private readonly config: MukanaConfig;
  private readonly fetch: FetchLike;
  private readonly state: Record<MukanaEndpoint, MukanaHealth>;

  /**
   * Consecutive HEALTHY settles still owed before an endpoint degraded by
   * `markHung` is trusted `"ok"` again — 0 for an endpoint that's never
   * been marked hung, or that has already paid off its hold. See
   * `MUKANA_RECOVERY_SETTLES`'s own doc comment for why this exists;
   * `armRecoveryHold`/`breakRecoveryStreak` are the only two places that
   * INCREASE it (arm to the full window), `applyHealth` is the only place
   * that decreases it (one healthy settle at a time).
   */
  private readonly recoverySettlesRemaining: Record<MukanaEndpoint, number>;

  constructor(config: MukanaConfig, deps: { fetch: FetchLike }) {
    this.config = config;
    this.fetch = deps.fetch;
    this.state = {
      panelists: initialHealth(),
      hands: initialHealth(),
      question: initialHealth()
    };
    this.recoverySettlesRemaining = { panelists: 0, hands: 0, question: 0 };
  }

  get health(): Record<MukanaEndpoint, MukanaHealth> {
    const copy = {} as Record<MukanaEndpoint, MukanaHealth>;
    for (const endpoint of MUKANA_ENDPOINTS) {
      copy[endpoint] = { ...this.state[endpoint] };
    }
    return copy;
  }

  /** Convenience accessor for a single endpoint's health, returned as a copy. */
  healthFor(endpoint: MukanaEndpoint): MukanaHealth {
    return { ...this.state[endpoint] };
  }

  /** Milliseconds to wait before the next fetch of this endpoint. */
  nextDelayMs(endpoint: MukanaEndpoint): number {
    const { consecutiveFailures } = this.state[endpoint];
    const interval = this.intervalFor(endpoint);
    if (consecutiveFailures === 0) return interval;

    const backoff = interval * 2 ** consecutiveFailures;
    return Math.min(backoff, this.config.maxBackoffMs);
  }

  /**
   * Told by `MukanaPoller.hungEndpoints()` the moment it decides a poll has
   * been outstanding long enough to call hung (Task 3, closing the previous
   * plan's carried-forward obligation). Two things happen together, so the
   * hysteresis DECISION and the health record it produces can never drift
   * apart the way they would if a caller tried to layer this on top of
   * `health` from outside:
   *
   * 1. The endpoint's health is written `failing`, with the SAME
   *    operator-facing string `ShowEngine.mukanaHealth`'s own (unchanged,
   *    redundant-while-in-flight) override builds from the identical
   *    `outstandingMs` — Task 1's review already caught this exact string
   *    being collapsed once during a carve-out, so it is deliberately
   *    duplicated here rather than centralized, to avoid reaching back into
   *    `showEngine.ts` for this task (see `MukanaPoller.hungEndpoints`'s own
   *    doc comment). `consecutiveFailures` is preserved, not incremented —
   *    nothing has actually failed a settle here, the fetch is still
   *    outstanding.
   * 2. `recoverySettlesRemaining[endpoint]` is armed to the full
   *    `MUKANA_RECOVERY_SETTLES` window. This is what survives past the
   *    moment the hung fetch finally settles — which a busy-gated view like
   *    `hungEndpoints()` cannot see once the promise resolves and the
   *    endpoint stops being "in flight." `applyHealth` (below) consults it
   *    on every subsequent settle.
   *
   * Idempotent while already degraded: calling this again for the SAME
   * still-hung fetch (which happens every time a caller re-checks
   * `hungEndpoints()` before it settles) never REDUCES the hold — only a
   * genuine settle in `applyHealth` does that — but it also never leaves a
   * PARTIALLY-recovered hold (`recoverySettlesRemaining` < the full window)
   * sitting there: a fresh hang detection re-arms it to the full window,
   * because a hang that recurs before recovery finished is not evidence the
   * endpoint has actually settled down.
   */
  markHung(endpoint: MukanaEndpoint, outstandingMs: number): void {
    this.state[endpoint] = {
      state: "failing",
      consecutiveFailures: this.state[endpoint].consecutiveFailures,
      detail: `no response after ${outstandingMs}ms with a poll still in flight`
    };
    this.armRecoveryHold(endpoint);
  }

  /** Arm (or refresh) `endpoint`'s hang-recovery hold to the full window. Never reduces it. */
  private armRecoveryHold(endpoint: MukanaEndpoint): void {
    if (this.recoverySettlesRemaining[endpoint] < MUKANA_RECOVERY_SETTLES) {
      this.recoverySettlesRemaining[endpoint] = MUKANA_RECOVERY_SETTLES;
    }
  }

  /**
   * A settle FAILED (a thrown/non-2xx transport error via `fail`, or a
   * parsed `invalid` outcome via `applyHealth`) while `endpoint` was mid a
   * hang-recovery hold. `MUKANA_RECOVERY_SETTLES` requires GENUINELY
   * CONSECUTIVE healthy settles, so a failure here breaks the streak — the
   * full window is re-armed rather than any partial progress being kept.
   * No-op when nothing is mid-hold (the ordinary case).
   */
  private breakRecoveryStreak(endpoint: MukanaEndpoint): void {
    if (this.recoverySettlesRemaining[endpoint] > 0) {
      this.recoverySettlesRemaining[endpoint] = MUKANA_RECOVERY_SETTLES;
    }
  }

  async fetchPanelists(): Promise<MukanaOutcome> {
    return this.request("panelists", parseMukanaPanelists);
  }

  async fetchHands(): Promise<HandsOutcome | DormantOutcome> {
    return this.request("hands", parseHandsPayload, { detectDormant: true });
  }

  async fetchQuestion(): Promise<QuestionOutcome> {
    return this.request("question", parseMukanaQuestion);
  }

  private intervalFor(endpoint: MukanaEndpoint): number {
    switch (endpoint) {
      case "panelists":
        return this.config.panelistsIntervalMs;
      case "hands":
        return this.config.handsIntervalMs;
      case "question":
        return this.config.questionIntervalMs;
    }
  }

  /**
   * Shared request path for every endpoint: builds the URL, runs the
   * injected fetch, classifies thrown errors and non-2xx responses as
   * `invalid`, and updates health/backoff bookkeeping. The only thing that
   * varies per endpoint is `parse` — how to turn the raw body into that
   * endpoint's outcome type. `options.detectDormant` lets a caller opt into
   * recognizing the shared off-hours envelope (via `detectDormantEnvelope`,
   * the same classifier the JSON endpoints' own parsers use) before `parse`
   * ever sees the body, for endpoints whose own outcome type has no
   * `dormant` arm. Either way, `applyHealth` is the only place that writes
   * a health record, so "shared bookkeeping" is true of the code, not just
   * the intent.
   */
  private async request<T extends ParseResult>(
    endpoint: MukanaEndpoint,
    parse: (body: string) => T,
    options?: { detectDormant?: boolean }
  ): Promise<T | DormantOutcome> {
    const url = `${this.config.baseUrl}?event=${encodeURIComponent(this.config.event)}&req=${endpoint}`;
    // Derived from the endpoint's OWN interval, never the backoff-inflated
    // `nextDelayMs` — see `FetchLike`'s doc comment for the obligation this
    // places on the host (a fetch that doesn't honor `signal` gets
    // degradation but never recovery) and why: this package holds no timer
    // of its own to fall back on.
    const signal = AbortSignal.timeout(this.intervalFor(endpoint));

    let body: string;
    try {
      const response = await this.fetch(url, { signal });
      if (!response.ok) {
        return this.fail<T>(endpoint, `HTTP ${response.status} from ${endpoint}`);
      }
      body = await response.text();
    } catch (error) {
      const detail = error instanceof Error ? error.message : String(error);
      return this.fail<T>(endpoint, detail);
    }

    if (options?.detectDormant) {
      const dormant = detectDormantEnvelope(body);
      if (dormant) {
        return this.applyHealth(endpoint, dormant);
      }
    }

    return this.applyHealth(endpoint, parse(body));
  }

  /**
   * Classify a parsed outcome and update the endpoint's health record
   * accordingly — and, when `endpoint` is mid a hang-recovery hold (Task 3,
   * `markHung`/`MUKANA_RECOVERY_SETTLES`), keep it reported `failing` until
   * enough CONSECUTIVE healthy settles have paid the hold off. "Healthy"
   * here means anything that isn't `invalid` — `data` and `dormant` both
   * count, because both are genuine evidence the endpoint answered rather
   * than staying silent; `invalid` breaks the streak instead
   * (`breakRecoveryStreak`).
   */
  private applyHealth<T extends ParseResult>(endpoint: MukanaEndpoint, outcome: T): T {
    const result: ParseResult = outcome;
    if (result.kind === "invalid") {
      this.breakRecoveryStreak(endpoint);
      this.state[endpoint] = {
        state: "failing",
        consecutiveFailures: this.state[endpoint].consecutiveFailures + 1,
        detail: result.reason
      };
      return outcome;
    }

    const remaining = this.recoverySettlesRemaining[endpoint];
    if (remaining > 0) {
      const next = remaining - 1;
      this.recoverySettlesRemaining[endpoint] = next;
      if (next > 0) {
        // Still mid-hold: one good answer right after a hang must not
        // immediately restore `"ok"` (review finding 1 — that flip is what
        // let a marginal endpoint flap the guest boxes every poll cycle).
        // `consecutiveFailures: 0` because nothing failed — this settle WAS
        // healthy, it just isn't the last one owed yet.
        this.state[endpoint] = {
          state: "failing",
          consecutiveFailures: 0,
          detail: `recovered from a hang but not yet trusted — ${next} more healthy poll${next === 1 ? "" : "s"} needed`
        };
        return outcome;
      }
      // next === 0: the hold just paid off on THIS settle — fall through
      // and record the real outcome below, same as an endpoint that was
      // never degraded.
    }

    if (result.kind === "dormant") {
      this.state[endpoint] = { state: "dormant", consecutiveFailures: 0, detail: result.detail };
    } else {
      this.state[endpoint] = { state: "ok", consecutiveFailures: 0, detail: null };
    }
    return outcome;
  }

  /**
   * Records a transport-level failure (thrown fetch, non-2xx) and returns it
   * as an `invalid` outcome. Every endpoint's outcome type is constrained by
   * `ParseResult` to include this exact `{ kind: "invalid", reason }` shape,
   * so building it generically and asserting it as `T` is safe.
   */
  private fail<T extends ParseResult>(endpoint: MukanaEndpoint, detail: string): T {
    this.breakRecoveryStreak(endpoint);
    this.state[endpoint] = {
      state: "failing",
      consecutiveFailures: this.state[endpoint].consecutiveFailures + 1,
      detail
    };
    return { kind: "invalid", reason: detail } as T;
  }
}
