import { describe, expect, it } from "vitest";
import { MukanaClient, type FetchLike } from "./mukanaClient.js";
import type { MukanaConfig } from "./config.js";

const config: MukanaConfig = {
  baseUrl: "https://hoka.example.com/php-panel-rest.php",
  event: "officehours",
  panelistsIntervalMs: 5000,
  handsIntervalMs: 2000,
  questionIntervalMs: 2000,
  maxBackoffMs: 60000
};

function respondWith(body: string, ok = true, status = 200): FetchLike {
  return async () => ({ ok, status, text: async () => body });
}

const panelistsBody = JSON.stringify({
  uidA: { displayName: "Ann Lee", loc: "Austin, TX", pin: 4242, role: "panelist", online: true }
});

describe("MukanaClient", () => {
  it("requests the panelists endpoint with the configured event", async () => {
    const urls: string[] = [];
    const client = new MukanaClient(config, {
      fetch: async (url) => {
        urls.push(url);
        return { ok: true, status: 200, text: async () => panelistsBody };
      }
    });
    await client.fetchPanelists();
    expect(urls).toEqual([
      "https://hoka.example.com/php-panel-rest.php?event=officehours&req=panelists"
    ]);
  });

  it("reports healthy after a successful fetch", async () => {
    const client = new MukanaClient(config, { fetch: respondWith(panelistsBody) });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("data");
    expect(client.health).toEqual({ state: "ok", consecutiveFailures: 0, detail: null });
    expect(client.nextDelayMs()).toBe(5000);
  });

  it("reports dormant without counting a failure", async () => {
    const client = new MukanaClient(config, {
      fetch: respondWith(JSON.stringify({ status: 200, detail: "outside show hours" }))
    });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("dormant");
    expect(client.health).toEqual({
      state: "dormant",
      consecutiveFailures: 0,
      detail: "outside show hours"
    });
    expect(client.nextDelayMs()).toBe(5000);
  });

  it("turns a thrown network error into an invalid outcome", async () => {
    const client = new MukanaClient(config, {
      fetch: async () => {
        throw new Error("ECONNREFUSED");
      }
    });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("invalid");
    expect(client.health.state).toBe("failing");
    expect(client.health.detail).toMatch(/ECONNREFUSED/);
  });

  it("treats a non-2xx response as a failure", async () => {
    const client = new MukanaClient(config, { fetch: respondWith("nope", false, 503) });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("invalid");
    expect(client.health.detail).toMatch(/503/);
  });

  it("backs off exponentially and caps at maxBackoffMs", async () => {
    const client = new MukanaClient(config, { fetch: respondWith("nope", false, 503) });
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(10000);
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(20000);
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(40000);
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(60000);
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(60000);
  });

  it("resets backoff after a recovery", async () => {
    let body = "nope";
    let ok = false;
    const client = new MukanaClient(config, {
      fetch: async () => ({ ok, status: ok ? 200 : 503, text: async () => body })
    });
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(10000);

    body = panelistsBody;
    ok = true;
    await client.fetchPanelists();
    expect(client.nextDelayMs()).toBe(5000);
    expect(client.health.consecutiveFailures).toBe(0);
  });
});
