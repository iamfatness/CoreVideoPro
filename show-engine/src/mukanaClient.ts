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

export type FetchLike = (url: string) => Promise<FetchResponse>;

export type MukanaHealth = {
  state: "ok" | "dormant" | "failing";
  consecutiveFailures: number;
  detail: string | null;
};

export type MukanaEndpoint = "panelists" | "hands" | "question";

const ENDPOINTS: readonly MukanaEndpoint[] = ["panelists", "hands", "question"];

/**
 * The three kinds every endpoint parser's outcome can take. Each concrete
 * parser's return type (`MukanaOutcome`, `QuestionOutcome`, `HandsOutcome`)
 * is structurally a member of this union — `data` carries endpoint-specific
 * payload, `invalid` always carries `reason`, and `dormant` (when present)
 * always carries `detail`.
 */
type ParseResult = { kind: "data" } | DormantOutcome | { kind: "invalid"; reason: string };

function initialHealth(): MukanaHealth {
  return { state: "ok", consecutiveFailures: 0, detail: null };
}

export class MukanaClient {
  private readonly config: MukanaConfig;
  private readonly fetch: FetchLike;
  private readonly state: Record<MukanaEndpoint, MukanaHealth>;

  constructor(config: MukanaConfig, deps: { fetch: FetchLike }) {
    this.config = config;
    this.fetch = deps.fetch;
    this.state = {
      panelists: initialHealth(),
      hands: initialHealth(),
      question: initialHealth()
    };
  }

  get health(): Record<MukanaEndpoint, MukanaHealth> {
    const copy = {} as Record<MukanaEndpoint, MukanaHealth>;
    for (const endpoint of ENDPOINTS) {
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

    let body: string;
    try {
      const response = await this.fetch(url);
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

  /** Classify a parsed outcome and update the endpoint's health record accordingly. */
  private applyHealth<T extends ParseResult>(endpoint: MukanaEndpoint, outcome: T): T {
    const result: ParseResult = outcome;
    if (result.kind === "invalid") {
      this.state[endpoint] = {
        state: "failing",
        consecutiveFailures: this.state[endpoint].consecutiveFailures + 1,
        detail: result.reason
      };
    } else if (result.kind === "dormant") {
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
    this.state[endpoint] = {
      state: "failing",
      consecutiveFailures: this.state[endpoint].consecutiveFailures + 1,
      detail
    };
    return { kind: "invalid", reason: detail } as T;
  }
}
