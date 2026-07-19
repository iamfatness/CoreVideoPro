/**
 * Cloudflare Worker ingest for CoreVideo Pro crash + telemetry uploads (S0).
 *
 * Bindings (wrangler.jsonc): REPORTS_BUCKET (R2, crash payloads),
 * REPORTS_KV (KV, report index). Secret: TELEMETRY_API_KEY — REQUIRED;
 * an unset key rejects everything loudly instead of running open.
 * See README.md for the API contract, storage layout, and deploy steps.
 */
import { handleCrash, handleEvent } from "./lib/handlers.mjs";
import { createIpRateLimiter } from "./lib/rateLimit.mjs";

// Per-isolate token bucket (honest limits documented in lib/rateLimit.mjs).
const limiter = createIpRateLimiter({ limit: 60, windowMs: 60_000 });

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method !== "POST") {
      return new Response("Method not allowed", { status: 405 });
    }

    // Rate-limit before auth so the bucket also blunts key brute-forcing.
    const ip = request.headers.get("cf-connecting-ip") ?? "unknown";
    if (!limiter.allow(ip)) {
      return new Response("Too many requests", {
        status: 429,
        headers: { "retry-after": "60" }
      });
    }

    const expected = env.TELEMETRY_API_KEY?.trim();
    if (!expected) {
      // Misconfigured deploy: fail LOUD, never silently accept unauthenticated uploads.
      console.error("telemetry-ingest: TELEMETRY_API_KEY is not set; rejecting all requests");
      return new Response("Service misconfigured: TELEMETRY_API_KEY is not set", {
        status: 500
      });
    }
    const header = request.headers.get("authorization") ?? "";
    if (header !== `Bearer ${expected}`) {
      return new Response("Unauthorized", { status: 401 });
    }

    if (url.pathname === "/v1/crashes") {
      return handleCrash(request, env);
    }
    if (url.pathname === "/v1/events") {
      return handleEvent(request, env);
    }
    return new Response("Not found", { status: 404 });
  }
};
