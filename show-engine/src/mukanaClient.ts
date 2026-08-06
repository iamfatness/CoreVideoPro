/**
 * Mukana REST client.
 * Performs a single fetch per call and reports how long the caller should wait
 * before the next one — the polling loop lives in the orchestrator, which keeps
 * this unit-testable without fake timers. Network failures back off
 * exponentially; a dormant registry is not a failure and does not back off.
 * Each of the three endpoints (panelists, hands, question) keeps its own
 * independent health record and interval, so a failure on one cannot affect
 * the backoff or health of the others.
 */

import type { MukanaConfig } from "./config.js";
import { parseMukanaPanelists, type MukanaOutcome } from "./mukanaParse.js";

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
    return this.request("panelists");
  }

  async fetchHands(): Promise<MukanaOutcome> {
    return this.request("hands");
  }

  async fetchQuestion(): Promise<MukanaOutcome> {
    return this.request("question");
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

  private async request(endpoint: MukanaEndpoint): Promise<MukanaOutcome> {
    const url = `${this.config.baseUrl}?event=${encodeURIComponent(this.config.event)}&req=${endpoint}`;

    let body: string;
    try {
      const response = await this.fetch(url);
      if (!response.ok) {
        return this.fail(endpoint, `HTTP ${response.status} from ${endpoint}`);
      }
      body = await response.text();
    } catch (error) {
      const detail = error instanceof Error ? error.message : String(error);
      return this.fail(endpoint, detail);
    }

    const outcome = parseMukanaPanelists(body);
    if (outcome.kind === "invalid") {
      return this.fail(endpoint, outcome.reason);
    }

    if (outcome.kind === "dormant") {
      this.state[endpoint] = { state: "dormant", consecutiveFailures: 0, detail: outcome.detail };
      return outcome;
    }

    this.state[endpoint] = { state: "ok", consecutiveFailures: 0, detail: null };
    return outcome;
  }

  private fail(endpoint: MukanaEndpoint, detail: string): MukanaOutcome {
    this.state[endpoint] = {
      state: "failing",
      consecutiveFailures: this.state[endpoint].consecutiveFailures + 1,
      detail
    };
    return { kind: "invalid", reason: detail };
  }
}
