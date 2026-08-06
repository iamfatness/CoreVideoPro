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
    expect(client.healthFor("panelists")).toEqual({
      state: "ok",
      consecutiveFailures: 0,
      detail: null
    });
    expect(client.nextDelayMs("panelists")).toBe(5000);
  });

  it("reports dormant without counting a failure", async () => {
    const client = new MukanaClient(config, {
      fetch: respondWith(JSON.stringify({ status: 200, detail: "outside show hours" }))
    });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("dormant");
    expect(client.healthFor("panelists")).toEqual({
      state: "dormant",
      consecutiveFailures: 0,
      detail: "outside show hours"
    });
    expect(client.nextDelayMs("panelists")).toBe(5000);
  });

  it("turns a thrown network error into an invalid outcome", async () => {
    const client = new MukanaClient(config, {
      fetch: async () => {
        throw new Error("ECONNREFUSED");
      }
    });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("invalid");
    expect(client.healthFor("panelists").state).toBe("failing");
    expect(client.healthFor("panelists").detail).toMatch(/ECONNREFUSED/);
  });

  it("treats a non-2xx response as a failure", async () => {
    const client = new MukanaClient(config, { fetch: respondWith("nope", false, 503) });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("invalid");
    expect(client.healthFor("panelists").detail).toMatch(/503/);
  });

  it("backs off exponentially and caps at maxBackoffMs", async () => {
    const client = new MukanaClient(config, { fetch: respondWith("nope", false, 503) });
    await client.fetchPanelists();
    expect(client.nextDelayMs("panelists")).toBe(10000);
    await client.fetchPanelists();
    expect(client.nextDelayMs("panelists")).toBe(20000);
    await client.fetchPanelists();
    expect(client.nextDelayMs("panelists")).toBe(40000);
    await client.fetchPanelists();
    expect(client.nextDelayMs("panelists")).toBe(60000);
    await client.fetchPanelists();
    expect(client.nextDelayMs("panelists")).toBe(60000);
  });

  it("resets backoff after a recovery", async () => {
    let body = "nope";
    let ok = false;
    const client = new MukanaClient(config, {
      fetch: async () => ({ ok, status: ok ? 200 : 503, text: async () => body })
    });
    await client.fetchPanelists();
    expect(client.nextDelayMs("panelists")).toBe(10000);

    body = panelistsBody;
    ok = true;
    await client.fetchPanelists();
    expect(client.nextDelayMs("panelists")).toBe(5000);
    expect(client.healthFor("panelists").consecutiveFailures).toBe(0);
  });
});

describe("MukanaClient per-endpoint behaviour", () => {
  it("builds the hands and question URLs", async () => {
    const urls: string[] = [];
    const client = new MukanaClient(config, {
      fetch: async (url) => {
        urls.push(url);
        return { ok: true, status: 200, text: async () => panelistsBody };
      }
    });
    await client.fetchHands();
    await client.fetchQuestion();
    expect(urls).toEqual([
      "https://hoka.example.com/php-panel-rest.php?event=officehours&req=hands",
      "https://hoka.example.com/php-panel-rest.php?event=officehours&req=question"
    ]);
  });

  it("starts every endpoint healthy", () => {
    const client = new MukanaClient(config, { fetch: respondWith(panelistsBody) });
    for (const endpoint of ["panelists", "hands", "question"] as const) {
      expect(client.healthFor(endpoint)).toEqual({
        state: "ok",
        consecutiveFailures: 0,
        detail: null
      });
    }
  });

  it("uses each endpoint's own interval for the base delay", () => {
    const client = new MukanaClient(config, { fetch: respondWith(panelistsBody) });
    expect(client.nextDelayMs("panelists")).toBe(5000);
    expect(client.nextDelayMs("hands")).toBe(2000);
    expect(client.nextDelayMs("question")).toBe(2000);
  });

  it("keeps failure state independent per endpoint", async () => {
    let failHands = true;
    const client = new MukanaClient(config, {
      fetch: async (url) => {
        const broken = url.includes("req=hands") && failHands;
        return {
          ok: !broken,
          status: broken ? 503 : 200,
          text: async () => (broken ? "nope" : panelistsBody)
        };
      }
    });

    await client.fetchHands();
    await client.fetchPanelists();

    expect(client.healthFor("hands").state).toBe("failing");
    expect(client.healthFor("hands").consecutiveFailures).toBe(1);
    expect(client.nextDelayMs("hands")).toBe(4000);

    expect(client.healthFor("panelists").state).toBe("ok");
    expect(client.nextDelayMs("panelists")).toBe(5000);

    failHands = false;
    await client.fetchHands();
    expect(client.healthFor("hands").state).toBe("ok");
    expect(client.nextDelayMs("hands")).toBe(2000);
  });

  it("caps each endpoint's backoff at maxBackoffMs", async () => {
    const client = new MukanaClient(config, { fetch: respondWith("nope", false, 503) });
    for (let i = 0; i < 6; i += 1) await client.fetchHands();
    expect(client.nextDelayMs("hands")).toBe(60000);
  });

  it("treats a dormant hands response as healthy-but-dormant", async () => {
    const client = new MukanaClient(config, {
      fetch: respondWith(JSON.stringify({ status: 200, detail: "outside show hours" }))
    });
    const outcome = await client.fetchHands();
    expect(outcome.kind).toBe("dormant");
    expect(client.healthFor("hands")).toEqual({
      state: "dormant",
      consecutiveFailures: 0,
      detail: "outside show hours"
    });
    expect(client.nextDelayMs("hands")).toBe(2000);
  });

  it("returns copies of health records", () => {
    const client = new MukanaClient(config, { fetch: respondWith(panelistsBody) });
    const record = client.healthFor("panelists");
    record.consecutiveFailures = 99;
    expect(client.healthFor("panelists").consecutiveFailures).toBe(0);
  });
});
