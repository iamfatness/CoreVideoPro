// @vitest-environment node
import { describe, expect, it, vi } from "vitest";
import worker from "./worker.mjs";
import {
  buildTargets,
  statusIsUp,
  probeTarget,
  diffStates,
  buildAlertPayload,
  sendAlert,
  runChecks,
  STATE_KEY
} from "./lib/checks.mjs";

// ---- fakes -----------------------------------------------------------------

function makeKv(initial = null) {
  return {
    store: new Map(initial ? [[STATE_KEY, JSON.stringify(initial)]] : []),
    async get(key, opts) {
      const raw = this.store.get(key) ?? null;
      if (raw === null) return null;
      return opts?.type === "json" ? JSON.parse(raw) : raw;
    },
    async put(key, value) {
      this.store.set(key, value);
    }
  };
}

/**
 * A fetch fake driven by a map of url-substring -> status (or "throw"). The
 * webhook URL is captured so payload-shape assertions can inspect it.
 */
function makeFetch(statusByUrl, capture = {}) {
  return vi.fn(async (url, init) => {
    for (const [needle, outcome] of Object.entries(statusByUrl)) {
      if (String(url).includes(needle)) {
        if (outcome === "throw") throw new Error(`network fail: ${needle}`);
        return { status: outcome };
      }
    }
    // Webhook / unknown POST target: record it.
    if (init?.method === "POST") {
      capture.url = String(url);
      capture.body = init.body;
      capture.headers = init.headers;
      return { status: 204 };
    }
    return { status: 599 };
  });
}

const quietLogger = { log: () => {}, error: () => {} };

// ---- pure helpers ----------------------------------------------------------

describe("statusIsUp", () => {
  it("treats < 500 as up (incl. the broker's expected 4xx), 5xx/0 as down", () => {
    expect(statusIsUp(200)).toBe(true);
    expect(statusIsUp(400)).toBe(true); // broker rejects the no-return_uri probe
    expect(statusIsUp(404)).toBe(true);
    expect(statusIsUp(500)).toBe(false);
    expect(statusIsUp(503)).toBe(false);
    expect(statusIsUp(0)).toBe(false); // transport failure
  });
});

describe("buildTargets", () => {
  it("defaults to the broker + three workers + self", () => {
    const names = buildTargets({}).map((t) => t.name);
    expect(names).toEqual(["oauth-broker", "licensing-api", "telemetry-ingest", "caption-broker", "ops-monitor"]);
  });

  it("flags the broker as a read-only probe and defaults it to a no-return_uri start URL", () => {
    const broker = buildTargets({}).find((t) => t.name === "oauth-broker");
    expect(broker.readOnlyProbe).toBe(true);
    expect(broker.url).toContain("/oauth/start");
    expect(broker.url).not.toContain("return_uri");
  });

  it("honors env overrides and skips targets set to '' or 'off'", () => {
    const targets = buildTargets({
      OPS_BROKER_START_URL: "https://broker.test/probe",
      OPS_CAPTION_URL: "off",
      OPS_SELF_URL: ""
    });
    const byName = Object.fromEntries(targets.map((t) => [t.name, t.url]));
    expect(byName["oauth-broker"]).toBe("https://broker.test/probe");
    expect(byName["caption-broker"]).toBeUndefined();
    expect(byName["ops-monitor"]).toBeUndefined();
  });
});

// ---- probe (never throws) --------------------------------------------------

describe("probeTarget", () => {
  it("reports up for a <500 response, no redirect following", async () => {
    const fetchImpl = makeFetch({ "broker.test": 400 });
    const r = await probeTarget({ name: "b", url: "https://broker.test/oauth/start" }, { fetchImpl });
    expect(r.up).toBe(true);
    expect(r.status).toBe(400);
    // side-effect-free: manual redirect handling
    expect(fetchImpl).toHaveBeenCalledWith("https://broker.test/oauth/start", expect.objectContaining({ redirect: "manual", method: "GET" }));
  });

  it("never throws on a transport failure — resolves to down", async () => {
    const fetchImpl = makeFetch({ "dead.test": "throw" });
    const r = await probeTarget({ name: "d", url: "https://dead.test/health" }, { fetchImpl });
    expect(r.up).toBe(false);
    expect(r.status).toBe(0);
    expect(r.error).toMatch(/network fail/);
  });

  it("reports down for a 5xx", async () => {
    const fetchImpl = makeFetch({ "broken.test": 503 });
    const r = await probeTarget({ name: "x", url: "https://broken.test/health" }, { fetchImpl });
    expect(r.up).toBe(false);
    expect(r.status).toBe(503);
  });
});

// ---- flap-dampener (state-change only) -------------------------------------

describe("diffStates (flap-dampener)", () => {
  it("no changes when everything stays up (no alert)", () => {
    const results = [
      { name: "a", url: "u", up: true, status: 200, error: null },
      { name: "b", url: "u", up: true, status: 200, error: null }
    ];
    const { changes, nextStates } = diffStates(results, { a: "up", b: "up" });
    expect(changes).toEqual([]);
    expect(nextStates).toEqual({ a: "up", b: "up" });
  });

  it("emits a change only on the up->down transition, not on subsequent down ticks", () => {
    const down = [{ name: "a", url: "u", up: false, status: 503, error: null }];
    // first observation of down (prior up) -> one change
    const first = diffStates(down, { a: "up" });
    expect(first.changes).toHaveLength(1);
    expect(first.changes[0]).toMatchObject({ name: "a", from: "up", to: "down" });
    // still down next tick (prior now down) -> NO change (no spam)
    const second = diffStates(down, first.nextStates);
    expect(second.changes).toEqual([]);
  });

  it("emits a recovery change on down->up", () => {
    const up = [{ name: "a", url: "u", up: true, status: 200, error: null }];
    const { changes } = diffStates(up, { a: "down" });
    expect(changes).toHaveLength(1);
    expect(changes[0]).toMatchObject({ name: "a", from: "down", to: "up" });
  });

  it("treats missing prior state as 'up' (optimistic baseline): quiet on healthy first run, loud if already down", () => {
    const healthy = diffStates([{ name: "a", url: "u", up: true, status: 200, error: null }], {});
    expect(healthy.changes).toEqual([]);
    const brokenFromStart = diffStates([{ name: "a", url: "u", up: false, status: 0, error: "x" }], {});
    expect(brokenFromStart.changes).toHaveLength(1);
    expect(brokenFromStart.changes[0]).toMatchObject({ from: "up", to: "down" });
  });
});

// ---- alert payload shape ---------------------------------------------------

describe("buildAlertPayload", () => {
  it("returns null when there are no changes", () => {
    expect(buildAlertPayload([])).toBeNull();
    expect(buildAlertPayload(null)).toBeNull();
  });

  it("carries both Discord (content) and Slack (text) keys with the failed target + status", () => {
    const now = () => Date.parse("2026-07-23T12:00:00.000Z");
    const payload = buildAlertPayload(
      [{ name: "licensing-api", url: "u", from: "up", to: "down", status: 503, error: null }],
      { now, environment: "staging" }
    );
    expect(payload.content).toBe(payload.text);
    expect(payload.content).toContain("licensing-api");
    expect(payload.content).toContain("status 503");
    expect(payload.content).toContain("DOWN");
    expect(payload.content).toContain("2026-07-23T12:00:00.000Z");
  });

  it("prefers the transport error text over a status when present, and marks recoveries", () => {
    const payload = buildAlertPayload([
      { name: "oauth-broker", url: "u", from: "up", to: "down", status: 0, error: "timeout after 10000ms" },
      { name: "telemetry-ingest", url: "u", from: "down", to: "up", status: 200, error: null }
    ]);
    expect(payload.content).toContain("error: timeout after 10000ms");
    expect(payload.content).toContain("RECOVERED");
    expect(payload.content).toContain("2 state change(s)");
  });
});

// ---- alert destination (log-only path) -------------------------------------

describe("sendAlert", () => {
  it("log-only when no webhook is configured (never throws, never posts)", async () => {
    const logs = [];
    const fetchImpl = vi.fn();
    const res = await sendAlert(
      { content: "x", text: "x" },
      {},
      { fetchImpl, logger: { log: (m) => logs.push(m), error: () => {} } }
    );
    expect(res).toMatchObject({ sent: false, reason: "no-destination", logged: true });
    expect(fetchImpl).not.toHaveBeenCalled();
    expect(logs.join("\n")).toContain("no OPS_ALERT_WEBHOOK_URL");
  });

  it("posts the JSON payload to the configured webhook", async () => {
    const capture = {};
    const fetchImpl = makeFetch({}, capture);
    const res = await sendAlert(
      { content: "hello", text: "hello" },
      { OPS_ALERT_WEBHOOK_URL: "https://hooks.test/abc" },
      { fetchImpl, logger: quietLogger }
    );
    expect(res).toMatchObject({ sent: true, status: 204 });
    expect(capture.url).toBe("https://hooks.test/abc");
    expect(JSON.parse(capture.body)).toEqual({ content: "hello", text: "hello" });
  });

  it("does not throw when the webhook itself fails", async () => {
    const fetchImpl = vi.fn(async () => {
      throw new Error("webhook down");
    });
    const res = await sendAlert(
      { content: "x", text: "x" },
      { OPS_ALERT_WEBHOOK_URL: "https://hooks.test/abc" },
      { fetchImpl, logger: quietLogger }
    );
    expect(res).toMatchObject({ sent: false, reason: "webhook-error" });
  });
});

// ---- runChecks aggregation (end to end with fakes) -------------------------

describe("runChecks (multi-target aggregation)", () => {
  it("probes all targets, persists next state, and alerts once for the changed targets", async () => {
    const kv = makeKv({ "oauth-broker": "up", "licensing-api": "up", "telemetry-ingest": "up", "caption-broker": "up", "ops-monitor": "up" });
    const capture = {};
    const fetchImpl = makeFetch(
      {
        "oauth-broker": 400, // broker up (expected 4xx)
        "corevideo.iamfatness.us": 400,
        "licensing-api": 503, // DOWN
        "telemetry-ingest": 200,
        "caption-broker": 200,
        "ops-monitor": 200
      },
      capture
    );
    const env = { OPS_ALERT_WEBHOOK_URL: "https://hooks.test/abc", ENVIRONMENT: "staging" };
    const summary = await runChecks(env, { fetchImpl, kv, logger: quietLogger });

    expect(summary.results).toHaveLength(5);
    // exactly one change: licensing-api up->down
    expect(summary.changes).toHaveLength(1);
    expect(summary.changes[0]).toMatchObject({ name: "licensing-api", to: "down" });
    expect(summary.alert).toMatchObject({ sent: true });
    // one aggregated webhook POST
    const body = JSON.parse(capture.body);
    expect(body.content).toContain("licensing-api");
    // next state persisted
    const persisted = JSON.parse(kv.store.get(STATE_KEY));
    expect(persisted["licensing-api"]).toBe("down");
    expect(persisted["telemetry-ingest"]).toBe("up");
  });

  it("no state change => no webhook call (spam-proof)", async () => {
    const kv = makeKv({ "oauth-broker": "up", "licensing-api": "up", "telemetry-ingest": "up", "caption-broker": "up", "ops-monitor": "up" });
    const capture = {};
    const fetchImpl = makeFetch(
      { "iamfatness": 400, "licensing": 200, "telemetry": 200, "caption": 200, "ops-monitor": 200 },
      capture
    );
    const summary = await runChecks({ OPS_ALERT_WEBHOOK_URL: "https://hooks.test/abc" }, { fetchImpl, kv, logger: quietLogger });
    expect(summary.changes).toEqual([]);
    expect(capture.url).toBeUndefined(); // never POSTed
  });

  it("survives a KV read failure and still probes + alerts (log-only)", async () => {
    const brokenKv = {
      async get() {
        throw new Error("kv unavailable");
      },
      async put() {}
    };
    const fetchImpl = makeFetch({ "iamfatness": "throw", "licensing": 200, "telemetry": 200, "caption": 200, "ops-monitor": 200 });
    const summary = await runChecks({}, { fetchImpl, kv: brokenKv, logger: quietLogger });
    // broker down; missing prior => baseline up => one down change; log-only (no webhook)
    expect(summary.changes.some((c) => c.name === "oauth-broker" && c.to === "down")).toBe(true);
    expect(summary.alert).toMatchObject({ sent: false, reason: "no-destination" });
  });
});

// ---- worker surface --------------------------------------------------------

describe("worker.fetch", () => {
  it("serves an unauthenticated /health 200", async () => {
    const res = await worker.fetch(new Request("https://ops.example/health"), { ENVIRONMENT: "staging" });
    expect(res.status).toBe(200);
    const body = await res.json();
    expect(body).toMatchObject({ status: "ok", service: "ops-monitor" });
  });

  it("404s unknown paths", async () => {
    const res = await worker.fetch(new Request("https://ops.example/nope"), {});
    expect(res.status).toBe(404);
  });
});
