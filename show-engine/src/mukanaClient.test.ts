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

  /**
   * Task 3, the abort contract: `MukanaClient` must actually PASS a signal
   * to the injected fetch, not merely accept one in `FetchLike`'s type.
   * `FetchLike`'s own doc comment explains why this matters beyond typing —
   * without a host fetch that honors `signal`, a hung endpoint's promise
   * never settles, `consecutiveFailures` never leaves 0, backoff never
   * engages, and `MukanaPoller`'s hung-poll rule is the only thing left
   * standing between the show and a frozen queue. This test only proves the
   * plumbing half (a signal is actually handed to `fetch`) — honoring it is
   * the host's job, which `FetchLike`'s type cannot enforce.
   */
  it("passes an AbortSignal derived from the endpoint's interval to the injected fetch", async () => {
    let capturedSignal: AbortSignal | undefined;
    const client = new MukanaClient(config, {
      fetch: async (_url, init) => {
        capturedSignal = init?.signal;
        return { ok: true, status: 200, text: async () => panelistsBody };
      }
    });
    await client.fetchPanelists();
    expect(capturedSignal).toBeInstanceOf(AbortSignal);
    expect(capturedSignal?.aborted).toBe(false);
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

  /**
   * Final review, I1: an endpoint that has never answered must NOT report
   * as healthy. Health is written only when a request settles, so the
   * previous optimistic `"ok"` start was a claim of usability the client
   * had no evidence for — and for a `FetchLike` whose promise never
   * settles, one it never revisited: `resolveCapabilities` read `available`
   * for the whole show, `effectiveBoxFill` stayed `"queue"` over an empty
   * queue, and every guest box resolved to `null` while the operator's
   * manual assignments were ignored. `consecutiveFailures` stays 0 (nothing
   * has actually failed), which the delay assertion below pins: a
   * pessimistic start must not back off the first poll of a healthy
   * registry.
   */
  it("starts every endpoint failing-until-proven, without backing off the first poll", () => {
    const client = new MukanaClient(config, { fetch: respondWith(panelistsBody) });
    for (const endpoint of ["panelists", "hands", "question"] as const) {
      expect(client.healthFor(endpoint)).toEqual({
        state: "failing",
        consecutiveFailures: 0,
        detail: "not polled yet"
      });
    }
    expect(client.nextDelayMs("panelists")).toBe(5000);
    expect(client.nextDelayMs("hands")).toBe(2000);
  });

  it("uses each endpoint's own interval for the base delay", () => {
    const client = new MukanaClient(config, { fetch: respondWith(panelistsBody) });
    expect(client.nextDelayMs("panelists")).toBe(5000);
    expect(client.nextDelayMs("hands")).toBe(2000);
    expect(client.nextDelayMs("question")).toBe(2000);
  });

  it("keeps failure state independent per endpoint", async () => {
    let failHands = true;
    const validHandsBody = "4242,5555\n1383\nNONE";
    const client = new MukanaClient(config, {
      fetch: async (url) => {
        const broken = url.includes("req=hands") && failHands;
        if (broken) {
          return { ok: false, status: 503, text: async () => "nope" };
        }
        const body = url.includes("req=hands") ? validHandsBody : panelistsBody;
        return { ok: true, status: 200, text: async () => body };
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

const handsBody = "4242,5555\n1383\nNONE";

describe("MukanaClient per-endpoint parsing", () => {
  it("parses a hands body with the hands parser", async () => {
    const client = new MukanaClient(config, { fetch: respondWith(handsBody) });
    const outcome = await client.fetchHands();
    expect(outcome).toEqual({
      kind: "data",
      queue: { previous: [], current: "1383", upcoming: ["4242", "5555"] }
    });
    expect(client.healthFor("hands").state).toBe("ok");
  });

  it("does not mark a healthy plain-text hands response as failing", async () => {
    const client = new MukanaClient(config, { fetch: respondWith(handsBody) });
    await client.fetchHands();
    expect(client.healthFor("hands").consecutiveFailures).toBe(0);
    expect(client.nextDelayMs("hands")).toBe(2000);
  });

  it("reports a malformed hands body as invalid and counts a failure", async () => {
    const client = new MukanaClient(config, { fetch: respondWith("only one line") });
    const outcome = await client.fetchHands();
    expect(outcome.kind).toBe("invalid");
    expect(client.healthFor("hands").state).toBe("failing");
  });

  it("still treats the off-hours envelope on hands as dormant", async () => {
    const client = new MukanaClient(config, {
      fetch: respondWith(JSON.stringify({ status: 200, detail: "outside show hours" }))
    });
    const outcome = await client.fetchHands();
    expect(outcome).toEqual({ kind: "dormant", detail: "outside show hours" });
    expect(client.healthFor("hands").consecutiveFailures).toBe(0);
  });

  it("parses a question body with the question parser", async () => {
    const body = JSON.stringify({ q: { n: "Ann Lee", q: "Why?", v: 3, ts: 12, tag: "T", key: "k" } });
    const client = new MukanaClient(config, { fetch: respondWith(body) });
    const outcome = await client.fetchQuestion();
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(outcome.question?.askerName).toBe("Ann Lee");
    expect(client.healthFor("question").state).toBe("ok");
  });

  it("keeps using the panelist parser for panelists", async () => {
    const client = new MukanaClient(config, { fetch: respondWith(panelistsBody) });
    const outcome = await client.fetchPanelists();
    expect(outcome.kind).toBe("data");
    if (outcome.kind !== "data") return;
    expect(Object.keys(outcome.db)).toEqual(["4242"]);
  });

  it("keeps endpoint health independent across the three parsers", async () => {
    const client = new MukanaClient(config, {
      fetch: async (url) => {
        if (url.includes("req=hands")) return { ok: true, status: 200, text: async () => "bad" };
        return { ok: true, status: 200, text: async () => panelistsBody };
      }
    });
    await client.fetchHands();
    await client.fetchPanelists();
    expect(client.healthFor("hands").state).toBe("failing");
    expect(client.healthFor("panelists").state).toBe("ok");
  });
});
