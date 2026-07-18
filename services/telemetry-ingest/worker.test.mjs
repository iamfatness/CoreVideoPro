// @vitest-environment node
import { describe, expect, it } from "vitest";
import worker from "./worker.mjs";
import {
  MAX_CRASH_BYTES,
  MAX_EVENT_BYTES,
  extensionForContentType,
  readBodyWithCap
} from "./lib/handlers.mjs";
import { createIpRateLimiter } from "./lib/rateLimit.mjs";

const API_KEY = "test-telemetry-key";

function makeEnv(overrides = {}) {
  const kv = {
    data: new Map(),
    async put(key, value) {
      this.data.set(key, value);
    },
    async get(key) {
      return this.data.get(key) ?? null;
    }
  };
  const r2 = {
    data: new Map(),
    async put(key, value, options) {
      this.data.set(key, { value, options });
    }
  };
  return { TELEMETRY_API_KEY: API_KEY, REPORTS_KV: kv, REPORTS_BUCKET: r2, ...overrides };
}

let ipCounter = 0;
function uniqueIp() {
  ipCounter += 1;
  return `10.0.${Math.floor(ipCounter / 250)}.${ipCounter % 250}`;
}

function post(path, body, { key = API_KEY, ip = uniqueIp(), contentType = "application/json" } = {}) {
  const headers = { "cf-connecting-ip": ip };
  if (key !== null) headers.authorization = `Bearer ${key}`;
  if (contentType !== null) headers["content-type"] = contentType;
  return new Request(`https://telemetry.example${path}`, { method: "POST", headers, body });
}

describe("auth", () => {
  it("rejects requests without an Authorization header", async () => {
    const res = await worker.fetch(post("/v1/crashes", "{}", { key: null }), makeEnv());
    expect(res.status).toBe(401);
  });

  it("rejects requests with a wrong key", async () => {
    const res = await worker.fetch(post("/v1/crashes", "{}", { key: "wrong" }), makeEnv());
    expect(res.status).toBe(401);
  });

  it("fails loudly (500) when TELEMETRY_API_KEY is not configured", async () => {
    const env = makeEnv({ TELEMETRY_API_KEY: undefined });
    const res = await worker.fetch(post("/v1/crashes", "{}"), env);
    expect(res.status).toBe(500);
    // A misconfigured deploy must never store anything.
    expect(env.REPORTS_KV.data.size).toBe(0);
  });
});

describe("routing", () => {
  it("rejects non-POST methods", async () => {
    const req = new Request("https://telemetry.example/v1/crashes", {
      method: "GET",
      headers: { authorization: `Bearer ${API_KEY}`, "cf-connecting-ip": uniqueIp() }
    });
    const res = await worker.fetch(req, makeEnv());
    expect(res.status).toBe(405);
  });

  it("404s unknown paths", async () => {
    const res = await worker.fetch(post("/v1/nope", "{}"), makeEnv());
    expect(res.status).toBe(404);
  });
});

describe("POST /v1/crashes", () => {
  it("stores the full body in R2 and indexes it in KV", async () => {
    const env = makeEnv();
    const body = JSON.stringify({
      reason: "smoke-test",
      app: { version: "0.1.0", platform: "win32" },
      machineClass: "rtx4090"
    });
    const res = await worker.fetch(post("/v1/crashes", body), env);
    expect(res.status).toBe(202);

    const { reportId, accepted } = await res.json();
    expect(accepted).toBe(true);
    expect(reportId).toMatch(/^cv-\d{8}-[0-9a-f-]{36}$/);

    const day = new Date().toISOString().slice(0, 10);
    const r2Key = `crashes/${day}/${reportId}.json`;
    const stored = env.REPORTS_BUCKET.data.get(r2Key);
    expect(stored).toBeDefined();
    expect(new TextDecoder().decode(stored.value)).toBe(body);
    expect(stored.options.httpMetadata.contentType).toBe("application/json");

    const meta = JSON.parse(await env.REPORTS_KV.get(`report:${reportId}`));
    expect(meta).toMatchObject({
      reportId,
      kind: "crash",
      version: "0.1.0",
      machineClass: "rtx4090",
      reason: "smoke-test",
      size: body.length,
      r2Key
    });
    expect(meta.receivedAt).toMatch(/^\d{4}-\d{2}-\d{2}T/);
  });

  it("rejects invalid JSON with 400 and stores nothing", async () => {
    const env = makeEnv();
    const res = await worker.fetch(post("/v1/crashes", "not-json"), env);
    expect(res.status).toBe(400);
    expect(env.REPORTS_BUCKET.data.size).toBe(0);
    expect(env.REPORTS_KV.data.size).toBe(0);
  });

  it("rejects non-JSON content types with 415 (until S1 archives)", async () => {
    const res = await worker.fetch(
      post("/v1/crashes", "zipzipzip", { contentType: "application/zip" }),
      makeEnv()
    );
    expect(res.status).toBe(415);
  });

  it("rejects bodies over the 25MB cap with 413", async () => {
    const env = makeEnv();
    const big = new Uint8Array(MAX_CRASH_BYTES + 1);
    const res = await worker.fetch(post("/v1/crashes", big), env);
    expect(res.status).toBe(413);
    expect(env.REPORTS_BUCKET.data.size).toBe(0);
  });
});

describe("POST /v1/events", () => {
  it("indexes the event in KV (payload inline) without touching R2", async () => {
    const env = makeEnv();
    const body = JSON.stringify({ name: "session-end", app: { version: "0.1.0" } });
    const res = await worker.fetch(post("/v1/events", body), env);
    expect(res.status).toBe(202);

    const { reportId, accepted } = await res.json();
    expect(accepted).toBe(true);
    expect(env.REPORTS_BUCKET.data.size).toBe(0);

    const meta = JSON.parse(await env.REPORTS_KV.get(`report:${reportId}`));
    expect(meta).toMatchObject({
      reportId,
      kind: "event",
      name: "session-end",
      version: "0.1.0",
      size: body.length,
      r2Key: null
    });
    expect(meta.payload).toEqual({ name: "session-end", app: { version: "0.1.0" } });
  });

  it("rejects bodies over the 64KB cap with 413", async () => {
    const env = makeEnv();
    const big = "x".repeat(MAX_EVENT_BYTES + 1);
    const res = await worker.fetch(post("/v1/events", big), env);
    expect(res.status).toBe(413);
    expect(env.REPORTS_KV.data.size).toBe(0);
  });
});

describe("rate limiting", () => {
  it("returns 429 after the per-IP budget is exhausted", async () => {
    const env = makeEnv();
    const ip = "192.168.77.77";
    let lastStatus = 0;
    for (let i = 0; i < 61; i += 1) {
      const res = await worker.fetch(post("/v1/events", '{"name":"x"}', { ip }), env);
      lastStatus = res.status;
      if (i < 60) expect(res.status).toBe(202);
    }
    expect(lastStatus).toBe(429);

    // Other IPs are unaffected.
    const other = await worker.fetch(post("/v1/events", '{"name":"y"}'), env);
    expect(other.status).toBe(202);
  });

  it("token bucket refills over time and prunes stale entries", () => {
    let time = 0;
    const limiter = createIpRateLimiter({ limit: 2, windowMs: 1000, now: () => time });
    expect(limiter.allow("a")).toBe(true);
    expect(limiter.allow("a")).toBe(true);
    expect(limiter.allow("a")).toBe(false);
    time += 500; // refills 1 token
    expect(limiter.allow("a")).toBe(true);
    expect(limiter.allow("a")).toBe(false);
  });

  it("bounds memory at maxEntries", () => {
    let time = 0;
    const limiter = createIpRateLimiter({ limit: 1, windowMs: 1000, maxEntries: 3, now: () => time });
    expect(limiter.allow("a")).toBe(true);
    expect(limiter.allow("b")).toBe(true);
    expect(limiter.allow("c")).toBe(true);
    time += 2000; // a/b/c now stale -> pruned when a new IP arrives at capacity
    expect(limiter.allow("d")).toBe(true);
    expect(limiter.allow("d")).toBe(false);
  });
});

describe("body cap streaming", () => {
  it("aborts mid-stream when chunks exceed the cap without a content-length", async () => {
    const chunks = [new Uint8Array(60), new Uint8Array(60)];
    const fakeRequest = {
      headers: new Headers(),
      body: new ReadableStream({
        pull(controller) {
          const next = chunks.shift();
          if (next) controller.enqueue(next);
          else controller.close();
        }
      })
    };
    const result = await readBodyWithCap(fakeRequest, 100);
    expect(result.tooLarge).toBe(true);
  });

  it("concatenates chunks under the cap", async () => {
    const encoder = new TextEncoder();
    const chunks = [encoder.encode("hello "), encoder.encode("world")];
    const fakeRequest = {
      headers: new Headers(),
      body: new ReadableStream({
        pull(controller) {
          const next = chunks.shift();
          if (next) controller.enqueue(next);
          else controller.close();
        }
      })
    };
    const result = await readBodyWithCap(fakeRequest, 100);
    expect(new TextDecoder().decode(result.bytes)).toBe("hello world");
  });
});

describe("S1-ready key scheme", () => {
  it("maps content types to extensions for future binary uploads", () => {
    expect(extensionForContentType("application/json")).toBe(".json");
    expect(extensionForContentType("application/zip")).toBe(".zip");
    expect(extensionForContentType("application/octet-stream")).toBe(".bin");
  });
});
