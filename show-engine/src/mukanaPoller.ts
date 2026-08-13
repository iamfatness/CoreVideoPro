/**
 * The Mukana polling loop, carved out of `showEngine.ts` (Task 1). Owns the
 * due-check against `MukanaClient.nextDelayMs`, the per-endpoint busy gate,
 * the fire-and-forget rejection handling, and the hung-poll detection —
 * exactly what `ShowEngine.pollMukana`/`ShowEngine.mukanaHealth` did before,
 * moved verbatim. `ShowEngine` still owns the *sequencing* (calling `poll()`
 * then `drain()` at a fixed point in `tick()`) and applying the drained
 * outcomes to its own state (`mukanaRegistry`, `this.queue`, `this.question`)
 * — this class knows nothing about any of that; it only starts fetches,
 * remembers what settled, and reports which endpoints look hung.
 *
 * This is a move, not a redesign: see each member's doc comment (largely
 * ported from the original `ShowEngine` methods) for the reasoning behind
 * the busy gate, the rejection handlers, and the hung-poll grace window.
 */

import type { Clock } from "./clock.js";
import { MUKANA_ENDPOINTS, MUKANA_HUNG_POLL_INTERVALS } from "./mukanaClient.js";
import type { MukanaClient, MukanaEndpoint } from "./mukanaClient.js";
import type { DormantOutcome, MukanaOutcome, QuestionOutcome } from "./mukanaParse.js";
import type { HandsOutcome } from "./handsQueue.js";
import type { ShowIntegrationsConfig } from "./config.js";

// `MUKANA_HUNG_POLL_INTERVALS` moved to `mukanaClient.ts` in fix round 1: it
// now ALSO sizes `MukanaClient.request`'s own `AbortSignal.timeout`
// deadline, and the two had to share one definition rather than risk
// drifting apart the way they already had once (that drift — the abort
// firing at 1x the interval while this poller's own hung threshold stayed
// at 3x — was the fix round 1 bug: for any host that honors `signal`, the
// fetch always aborted before a poll could ever be reported hung here, so
// the hung path (and the hysteresis built on it) was unreachable). See the
// constant's own doc comment in `mukanaClient.ts` for the full account.

/**
 * Turn a rejection reason from a Mukana fetch promise into the same
 * `string` shape `MukanaClient.fail` uses for a caught transport error —
 * `poll`'s rejection handlers use this so a rejected fetch (a
 * contract-violating `FetchLike`, or anything else outside what
 * `MukanaClient.request`'s own try/catch covers) still records an ordinary
 * `invalid` outcome instead of an unhandled promise rejection.
 */
function mukanaRejectionReason(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

/** What settled since the last `drain()`, one slot per endpoint, `null` when nothing settled for that endpoint. */
export type PollOutcomes = {
  panelists: MukanaOutcome | null;
  hands: HandsOutcome | DormantOutcome | null;
  question: QuestionOutcome | null;
};

/**
 * One hung endpoint, as reported by `detectHungEndpoints()` — the endpoint
 * name PLUS how long (per the injected `Clock`) its in-flight poll has been
 * outstanding. `ShowEngine.mukanaHealth` needs the millisecond figure, not
 * just the fact of hanging, to publish the same operator-facing detail text
 * ("no response after ${outstandingMs}ms...") this reported before the
 * carve-out — collapsing it to a bare endpoint list would have quietly
 * dropped real diagnostic information (fix round 1).
 */
export type HungMukanaPoll = { endpoint: MukanaEndpoint; outstandingMs: number };

export class MukanaPoller {
  private readonly client: MukanaClient;
  private readonly clock: Clock;
  private readonly integrations: ShowIntegrationsConfig;

  /**
   * Wall-clock time (per the injected `Clock`) each endpoint's poll was last
   * STARTED, keyed so `nextDelayMs`'s own interval/backoff math (which this
   * poller deliberately adds no policy on top of) is checked against a fresh
   * anchor every time a fetch is kicked off — never against when it
   * SETTLED, since a poll that's still in flight must not look perpetually
   * "not due yet" once its interval has genuinely elapsed. `-Infinity`
   * makes every endpoint due on the very first `poll()` call.
   */
  private readonly lastMukanaPollAt: Record<MukanaEndpoint, number> = {
    panelists: Number.NEGATIVE_INFINITY,
    hands: Number.NEGATIVE_INFINITY,
    question: Number.NEGATIVE_INFINITY
  };

  /**
   * Whether a fetch for this endpoint is currently in flight — gates
   * starting a NEW one in `poll`. Set the moment a fetch starts, cleared in
   * that SAME fetch's `.then()`/rejection handler, never anywhere else.
   * This is what makes "one in-flight promise per endpoint" a real
   * invariant rather than a comment: without it, a slow or hung endpoint
   * gets a fresh overlapping fetch every time its interval elapses
   * (unbounded concurrent requests against an already-struggling registry,
   * and — because outcomes are applied in SETTLE order, not START order — a
   * later-settling but earlier-started fetch can overwrite a fresher one
   * with stale data, e.g. the hands queue jumping backwards to an older
   * guest). `nextDelayMs`'s backoff-after-failure ceiling bounds the RATE a
   * healthy-but-slow endpoint gets hit; this bounds the COUNT in flight at
   * once to exactly one, which backoff alone does not.
   */
  private readonly mukanaPollBusy: Record<MukanaEndpoint, boolean> = {
    panelists: false,
    hands: false,
    question: false
  };

  /**
   * The outcome of a hands/question/panelists fetch that has settled since
   * the last `drain()`, waiting for the next `drain()` call to notice and
   * return it — `null` once drained, or while no fetch for that endpoint
   * has settled yet. `poll()` never `await`s the fetch itself (spec §2: no
   * external integration failure may block a tick); a background `.then()`
   * attached the moment the fetch is started writes here as its ONLY job,
   * so writing it can never race a caller's own synchronous read-and-clear
   * in `drain()` — both run on the same single JS thread, and a `.then()`
   * callback body itself never runs concurrently with `drain()`'s own
   * execution.
   */
  private panelistsPollSettled: MukanaOutcome | null = null;
  private handsPollSettled: HandsOutcome | DormantOutcome | null = null;
  private questionPollSettled: QuestionOutcome | null = null;

  constructor(deps: { client: MukanaClient; clock: Clock; integrations: ShowIntegrationsConfig }) {
    this.client = deps.client;
    this.clock = deps.clock;
    this.integrations = deps.integrations;
  }

  /**
   * Start any endpoint's fetch whose poll is due AND not already in flight.
   *
   * Non-blocking by construction, never by discipline someone could get
   * wrong later: this method contains no `await` at all. Every fetch is
   * started and immediately `.then()`-attached without awaiting the
   * returned promise, so a hung registry can only ever delay when its OWN
   * outcome gets returned from `drain()` — never delay this or any other
   * tick from returning (spec §2, normative).
   *
   * `mukanaPollBusy[endpoint]` gates starting a NEW fetch while one is
   * still outstanding — see that field's own doc comment for what breaks
   * without it (unbounded concurrent requests to a hung endpoint, and
   * settle-order-not-start-order data landing stale-over-fresh). An earlier
   * revision of this method shipped WITHOUT the gate, reasoning that
   * settling a fetch takes several real microtask turns and each `tick()`
   * call is itself only one or two of those turns, so a busy gate could
   * leave an endpoint marked busy across ticks whose clock delta alone
   * would call it due. That reasoning was fitted to the test rig, not to
   * production: it measured true only because the ORIGINAL test rig never
   * let the microtask queue drain between `tick()` calls. In real time a
   * fetch settles in milliseconds against a multi-second polling interval —
   * millions of microtask turns of headroom — so the busy flag is cleared
   * long before the next poll is ever due. The rig now drains explicitly
   * (`flush()` in `showEngine.test.ts`) instead of the engine papering over
   * a fixture gap with a correctness gap of its own.
   *
   * A rejected fetch — a `FetchLike` that resolves `text()` to something
   * other than a string, or any other contract violation `MukanaClient`
   * doesn't already catch internally — must never become an unhandled
   * promise rejection (spec §2's whole point: a broken third-party
   * integration cannot be allowed to take the process down). Every
   * `.then()` below supplies BOTH handlers; the rejection handler clears
   * `mukanaPollBusy` exactly like the success path and records an
   * `invalid` outcome so a rejecting endpoint still surfaces as a normal
   * failure rather than silently going quiet or crashing the show.
   *
   * Endpoint gating mirrors `config.integrations`: an endpoint whose
   * integration is off is never polled, matching the rest of this
   * package's rule that an unconfigured integration must never silently
   * reach a URL nobody set (`config.ts`'s own `parseMukana` doc comment).
   */
  poll(): void {
    const client = this.client;
    const now = this.clock.now();

    if (
      this.integrations.registry &&
      !this.mukanaPollBusy.panelists &&
      this.isMukanaPollDue("panelists", now)
    ) {
      this.lastMukanaPollAt.panelists = now;
      this.mukanaPollBusy.panelists = true;
      client.fetchPanelists().then(
        (outcome) => {
          this.mukanaPollBusy.panelists = false;
          this.panelistsPollSettled = outcome;
        },
        (error: unknown) => {
          this.mukanaPollBusy.panelists = false;
          this.panelistsPollSettled = { kind: "invalid", reason: mukanaRejectionReason(error) };
        }
      );
    }
    if (
      this.integrations.handsQueue &&
      !this.mukanaPollBusy.hands &&
      this.isMukanaPollDue("hands", now)
    ) {
      this.lastMukanaPollAt.hands = now;
      this.mukanaPollBusy.hands = true;
      client.fetchHands().then(
        (outcome) => {
          this.mukanaPollBusy.hands = false;
          this.handsPollSettled = outcome;
        },
        (error: unknown) => {
          this.mukanaPollBusy.hands = false;
          this.handsPollSettled = { kind: "invalid", reason: mukanaRejectionReason(error) };
        }
      );
    }
    if (
      this.integrations.questionFeed &&
      !this.mukanaPollBusy.question &&
      this.isMukanaPollDue("question", now)
    ) {
      this.lastMukanaPollAt.question = now;
      this.mukanaPollBusy.question = true;
      client.fetchQuestion().then(
        (outcome) => {
          this.mukanaPollBusy.question = false;
          this.questionPollSettled = outcome;
        },
        (error: unknown) => {
          this.mukanaPollBusy.question = false;
          this.questionPollSettled = { kind: "invalid", reason: mukanaRejectionReason(error) };
        }
      );
    }
  }

  /**
   * Return whatever settled since the last `drain()` call, then clear it —
   * each endpoint's outcome is returned at most once. Called by `ShowEngine`
   * right after `poll()`, every tick, whether or not anything actually
   * settled (all three fields are `null` when nothing did).
   */
  drain(): PollOutcomes {
    const panelists = this.panelistsPollSettled;
    this.panelistsPollSettled = null;
    const hands = this.handsPollSettled;
    this.handsPollSettled = null;
    const question = this.questionPollSettled;
    this.questionPollSettled = null;
    return { panelists, hands, question };
  }

  /**
   * Endpoints whose in-flight poll has been outstanding for
   * `MUKANA_HUNG_POLL_INTERVALS` of their own intervals — the engine-side
   * correction `MukanaClient`'s own health cannot make for itself (final
   * review, I1 — see `MUKANA_HUNG_POLL_INTERVALS`'s own doc comment for why,
   * and `FetchLike`'s for the timeout obligation this backstops). Each entry
   * carries `outstandingMs` (fix round 1) so a caller can report exactly how
   * stale the hang is, not just that it's hung — the same operator-facing
   * detail (`"no response after ${outstandingMs}ms..."`) this reported
   * before the carve-out. That detail is now built and recorded by
   * `MukanaClient.markHung` itself (Task 3) — called here, the ONE place a
   * fresh hang is ever detected — rather than left for a caller to
   * reconstruct: `markHung` both writes the SAME string into `client.health`
   * (so a caller reading `client.health` directly, without going through
   * this method's own return value, sees the identical answer) and arms the
   * hang-recovery hysteresis hold (`MUKANA_RECOVERY_SETTLES`) that outlives
   * this endpoint's busy window — the part this method's own busy-gated view
   * can never see once the fetch finally settles. This method's own return
   * shape and busy-gated behavior are otherwise UNCHANGED from before Task 3.
   *
   * NAMED `detect...`, not a bare noun like `hungEndpoints` (fix round 1,
   * Minor 3), because this is a QUERY THAT MUTATES: every call can write
   * health and arm a fresh recovery hold via `markHung`, not just read
   * state. That is reachable from outside the tick loop too —
   * `ShowEngine.adjustPage` (behind `nextGuest`/`prevGuest`) calls
   * `mukanaHealth()`, which calls this — so an operator paging through
   * guests can, as a side effect, arm a recovery hold slightly earlier than
   * the next tick would have. Harmless today (the hold's effect is
   * identical either way, and paging happens far less often than ticking),
   * but the name says what the method actually does rather than implying a
   * plain getter.
   */
  detectHungEndpoints(): readonly HungMukanaPoll[] {
    const now = this.clock.now();
    const hung: HungMukanaPoll[] = [];
    for (const endpoint of MUKANA_ENDPOINTS) {
      if (!this.mukanaPollBusy[endpoint]) continue;
      const outstandingMs = now - this.lastMukanaPollAt[endpoint];
      if (outstandingMs < this.client.nextDelayMs(endpoint) * MUKANA_HUNG_POLL_INTERVALS) continue;
      this.client.markHung(endpoint, outstandingMs);
      hung.push({ endpoint, outstandingMs });
    }
    return hung;
  }

  /** Whether `endpoint`'s next poll is due: `nextDelayMs` already folds interval + backoff; this adds no policy of its own. */
  private isMukanaPollDue(endpoint: MukanaEndpoint, now: number): boolean {
    return now - this.lastMukanaPollAt[endpoint] >= this.client.nextDelayMs(endpoint);
  }

  /**
   * Force every endpoint to read as due on the very next `poll()` call,
   * regardless of its own interval/backoff or how recently it last polled —
   * the mechanism behind an operator's explicit "sync now" control
   * (`ShowEngine.syncAll`). Resets `lastMukanaPollAt` rather than calling
   * any fetch itself: this method takes no `Clock` reading and starts
   * nothing, it only clears the due-check's anchor so the NEXT `poll()`,
   * whenever it runs, starts every endpoint immediately instead of waiting
   * out its interval. An endpoint currently in flight is unaffected — the
   * busy gate in `poll()` still refuses a second overlapping fetch for it;
   * a forced sync on a hung endpoint takes effect the moment its current
   * fetch finally settles and `poll()` next runs.
   */
  forceDue(): void {
    for (const endpoint of MUKANA_ENDPOINTS) {
      this.lastMukanaPollAt[endpoint] = Number.NEGATIVE_INFINITY;
    }
  }
}
