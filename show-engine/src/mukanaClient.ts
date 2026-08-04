/**
 * Mukana REST client.
 * Performs a single fetch per call and reports how long the caller should wait
 * before the next one — the polling loop lives in the orchestrator, which keeps
 * this unit-testable without fake timers. Network failures back off
 * exponentially; a dormant registry is not a failure and does not back off.
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

export class MukanaClient {
  private readonly config: MukanaConfig;
  private readonly fetch: FetchLike;
  private state: MukanaHealth = { state: "ok", consecutiveFailures: 0, detail: null };

  constructor(config: MukanaConfig, deps: { fetch: FetchLike }) {
    this.config = config;
    this.fetch = deps.fetch;
  }

  get health(): MukanaHealth {
    return { ...this.state };
  }

  /** Milliseconds to wait before the next panelists fetch. */
  nextDelayMs(): number {
    const { consecutiveFailures } = this.state;
    if (consecutiveFailures === 0) return this.config.panelistsIntervalMs;

    const backoff = this.config.panelistsIntervalMs * 2 ** consecutiveFailures;
    return Math.min(backoff, this.config.maxBackoffMs);
  }

  async fetchPanelists(): Promise<MukanaOutcome> {
    return this.request("panelists");
  }

  private async request(req: string): Promise<MukanaOutcome> {
    const url = `${this.config.baseUrl}?event=${encodeURIComponent(this.config.event)}&req=${req}`;

    let body: string;
    try {
      const response = await this.fetch(url);
      if (!response.ok) {
        return this.fail(`HTTP ${response.status} from ${req}`);
      }
      body = await response.text();
    } catch (error) {
      const detail = error instanceof Error ? error.message : String(error);
      return this.fail(detail);
    }

    const outcome = parseMukanaPanelists(body);
    if (outcome.kind === "invalid") {
      return this.fail(outcome.reason);
    }

    if (outcome.kind === "dormant") {
      this.state = { state: "dormant", consecutiveFailures: 0, detail: outcome.detail };
      return outcome;
    }

    this.state = { state: "ok", consecutiveFailures: 0, detail: null };
    return outcome;
  }

  private fail(detail: string): MukanaOutcome {
    this.state = {
      state: "failing",
      consecutiveFailures: this.state.consecutiveFailures + 1,
      detail
    };
    return { kind: "invalid", reason: detail };
  }
}
