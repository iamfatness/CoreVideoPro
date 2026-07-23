/**
 * CoreVideo Pro ops-monitor Worker (beta spec docs/beta-engineering-spec.md §S5).
 *
 * A Cron-triggered uptime check for the runtime dependencies beta makes hard:
 * the external OAuth broker (corevideo.iamfatness.us) and the in-repo Cloudflare
 * workers (licensing-api, telemetry-ingest, caption-broker) + itself. On a
 * state CHANGE it alerts a generic webhook (Discord/Slack). It never spams
 * (state-change only, KV-backed flap-dampener) and never crashes on a probe
 * failure — the thing it monitors is expected to fail.
 *
 * Bindings/vars (wrangler.jsonc + secrets — see README.md):
 * - OPS_STATE (KV): last up/down state per target for the flap-dampener.
 * - OPS_ALERT_WEBHOOK_URL (secret, OPTIONAL): Discord/Slack incoming webhook.
 *   Unset => log-only (never crash, never spam).
 * - OPS_BROKER_START_URL / OPS_LICENSING_URL / OPS_TELEMETRY_URL /
 *   OPS_CAPTION_URL / OPS_SELF_URL (vars, OPTIONAL): override any probe target;
 *   "" or "off" skips it.
 */
import { runChecks } from "./lib/checks.mjs";

export default {
  // Cloudflare Cron Trigger entry point (schedule in wrangler.jsonc).
  async scheduled(event, env, ctx) {
    // ctx.waitUntil keeps the isolate alive for the async run; a thrown error
    // here would only log, but we catch anyway so one bad tick is never fatal.
    ctx.waitUntil(
      runChecks(env).catch((err) => {
        console.error(`ops-monitor: scheduled run failed: ${String(err?.message ?? err)}`);
      })
    );
  },

  // HTTP surface: a tiny unauthenticated health endpoint so the monitor is
  // itself monitorable (by this same worker's self-target, or an external
  // uptime service). GET /run triggers an on-demand check for manual verify.
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method === "GET" && (url.pathname === "/health" || url.pathname === "/v1/health")) {
      return Response.json({ status: "ok", service: "ops-monitor", environment: env.ENVIRONMENT ?? "staging" });
    }

    if (request.method === "POST" && url.pathname === "/run") {
      // Manual trigger (owner smoke-test). Same code path as the cron.
      const summary = await runChecks(env).catch((err) => ({
        error: String(err?.message ?? err)
      }));
      return Response.json(summary);
    }

    return new Response("Not found", { status: 404 });
  }
};
