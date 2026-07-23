/**
 * Pure + orchestration logic for the ops-monitor worker (beta spec §S5).
 *
 * The worker periodically probes the OAuth broker + the in-repo Cloudflare
 * workers, and alerts ONLY when a target changes state (up->down / down->up)
 * so a persistent outage never spams the destination. Everything here is
 * dependency-injected (fetch / KV / clock) so it is unit-testable without a
 * live network or a real Cloudflare runtime.
 *
 * INVARIANTS (see README):
 * - Never throw out of a probe: the thing we monitor is expected to fail, so a
 *   fetch/timeout/DNS error is a normal "down" result, never an exception.
 * - Never alert-spam: alerts fire on STATE CHANGE only (flap-dampener backed by
 *   KV last-state).
 * - The broker is monitored READ-ONLY with a side-effect-free probe (see
 *   probeTarget / the README "Broker probe" section).
 */

const DEFAULT_TIMEOUT_MS = 10_000;

/**
 * A target is "up" when it answers with any HTTP status below 500. That covers:
 * - the in-repo workers' `GET /health` (200), and
 * - the broker's `/oauth/start` probe, which by design 4xx-rejects the missing
 *   return_uri BEFORE generating any OAuth state (a 400 proves the worker is up
 *   and routing, with zero side effects).
 * "Down" is a transport failure (DNS/timeout/reset) or a 5xx — the signals that
 * actually mean "the dependency is unavailable".
 */
export function statusIsUp(status) {
  return typeof status === "number" && status > 0 && status < 500;
}

/**
 * Build the probe target list from env, with sensible staging defaults. Every
 * URL is overridable so nothing is hardcoded to one deployment; set any target
 * to an empty string or "off" to skip it.
 *
 * The broker target is flagged `readOnlyProbe` purely for documentation/telemetry
 * — the actual side-effect-free behavior comes from the URL we default to (no
 * return_uri) + `redirect: "manual"` in probeTarget.
 */
export function buildTargets(env = {}) {
  const raw = [
    {
      name: "oauth-broker",
      // Default: the known broker start URL (reality doc) with NO return_uri, so
      // the broker rejects it before any OAuth state/redirect. Overridable.
      url: env.OPS_BROKER_START_URL ?? "https://corevideo.iamfatness.us/oauth/start?probe=ops-monitor",
      readOnlyProbe: true
    },
    {
      name: "licensing-api",
      url: env.OPS_LICENSING_URL ?? "https://corevideo-licensing-api.wallace-john-w.workers.dev/health"
    },
    {
      name: "telemetry-ingest",
      url: env.OPS_TELEMETRY_URL ?? "https://corevideo-telemetry-ingest.wallace-john-w.workers.dev/health"
    },
    {
      name: "caption-broker",
      url: env.OPS_CAPTION_URL ?? "https://corevideo-caption-broker.wallace-john-w.workers.dev/health"
    },
    {
      name: "ops-monitor",
      url: env.OPS_SELF_URL ?? "https://corevideo-ops-monitor.wallace-john-w.workers.dev/health"
    }
  ];
  return raw.filter((t) => {
    const u = (t.url ?? "").trim();
    return u.length > 0 && u.toLowerCase() !== "off";
  });
}

/**
 * Probe one target. GET, redirect: "manual" (never follow a redirect off to an
 * external site — keeps the broker probe side-effect-free even if it 302s), with
 * a hard timeout. NEVER throws: a transport failure resolves to `{ up: false }`.
 */
export async function probeTarget(target, { fetchImpl = fetch, timeoutMs = DEFAULT_TIMEOUT_MS, now = Date.now } = {}) {
  const startedAt = now();
  const controller = typeof AbortController === "function" ? new AbortController() : null;
  const timer = controller ? setTimeout(() => controller.abort(), timeoutMs) : null;
  try {
    const res = await fetchImpl(target.url, {
      method: target.method ?? "GET",
      redirect: "manual",
      headers: { "user-agent": "corevideo-ops-monitor/1" },
      signal: controller ? controller.signal : undefined
    });
    const status = res?.status ?? 0;
    return {
      name: target.name,
      url: target.url,
      up: statusIsUp(status),
      status,
      latencyMs: now() - startedAt,
      error: null
    };
  } catch (err) {
    return {
      name: target.name,
      url: target.url,
      up: false,
      status: 0,
      latencyMs: now() - startedAt,
      error: err && err.name === "AbortError" ? `timeout after ${timeoutMs}ms` : String(err?.message ?? err)
    };
  } finally {
    if (timer) clearTimeout(timer);
  }
}

/** Probe every target concurrently. Order of results follows the target list. */
export async function probeAll(targets, opts = {}) {
  return Promise.all(targets.map((t) => probeTarget(t, opts)));
}

/**
 * Flap-dampener core (PURE). Given the fresh probe results and the prior state
 * map (name -> "up"|"down"), return the state CHANGES plus the full next-state
 * map to persist. A target with no prior state is treated as previously "up"
 * (optimistic baseline): a healthy first run stays quiet, but a target already
 * down on first observation is surfaced immediately.
 */
export function diffStates(results, priorStates = {}) {
  const changes = [];
  const nextStates = {};
  for (const r of results) {
    const current = r.up ? "up" : "down";
    const prior = priorStates[r.name] ?? "up";
    nextStates[r.name] = current;
    if (current !== prior) {
      changes.push({
        name: r.name,
        url: r.url,
        from: prior,
        to: current,
        status: r.status,
        error: r.error
      });
    }
  }
  return { changes, nextStates };
}

/**
 * Build the alert payload for a set of state changes, or `null` when there is
 * nothing to report. The body carries BOTH `content` (Discord) and `text`
 * (Slack) so ONE generic webhook URL works for either — each service ignores
 * the key it doesn't use.
 */
export function buildAlertPayload(changes, { now = Date.now, environment = "staging" } = {}) {
  if (!changes || changes.length === 0) return null;
  const ts = new Date(now()).toISOString();
  const lines = changes.map((c) => {
    if (c.to === "down") {
      const detail = c.error ? `error: ${c.error}` : `status ${c.status}`;
      return `:red_circle: DOWN  ${c.name} (${detail})`;
    }
    return `:large_green_circle: RECOVERED  ${c.name} (status ${c.status})`;
  });
  const anyDown = changes.some((c) => c.to === "down");
  const heading = `${anyDown ? ":rotating_light:" : ":white_check_mark:"} CoreVideo Pro ops-monitor [${environment}] — ${changes.length} state change(s) at ${ts}`;
  const message = [heading, ...lines].join("\n");
  return { content: message, text: message };
}

/**
 * POST the alert to the generic webhook. Returns a small result object and NEVER
 * throws — a webhook outage must not crash the monitor. When no destination is
 * configured we log-only (the beta posture: an unconfigured owner still gets
 * signal in `wrangler tail`, never a crash).
 */
export async function sendAlert(payload, env = {}, { fetchImpl = fetch, logger = console } = {}) {
  if (!payload) return { sent: false, reason: "no-changes" };

  const webhook = (env.OPS_ALERT_WEBHOOK_URL ?? "").trim();
  if (!webhook) {
    logger.log?.(`ops-monitor: no OPS_ALERT_WEBHOOK_URL configured; state change (log-only):\n${payload.content}`);
    return { sent: false, reason: "no-destination", logged: true };
  }

  try {
    const res = await fetchImpl(webhook, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(payload)
    });
    if (!res || res.status >= 300) {
      logger.error?.(`ops-monitor: alert webhook returned ${res?.status ?? "no response"}`);
      return { sent: false, reason: `webhook-status-${res?.status ?? "none"}` };
    }
    return { sent: true, status: res.status };
  } catch (err) {
    // The alerter itself failing must never crash the run.
    logger.error?.(`ops-monitor: alert webhook threw: ${String(err?.message ?? err)}`);
    return { sent: false, reason: "webhook-error", error: String(err?.message ?? err) };
  }
}

const STATE_KEY = "ops-monitor:last-state";

/**
 * One full monitoring pass: load prior state from KV, probe all targets, diff,
 * persist next state, and alert on changes. Wrapped so a failure in any single
 * step (KV read, a probe, the webhook) is logged and the pass still completes.
 * Returns a summary for tests / observability.
 */
export async function runChecks(env = {}, { fetchImpl = fetch, kv = env.OPS_STATE, now = Date.now, logger = console, timeoutMs } = {}) {
  const targets = buildTargets(env);

  let priorStates = {};
  try {
    if (kv) {
      const stored = await kv.get(STATE_KEY, { type: "json" });
      if (stored && typeof stored === "object") priorStates = stored;
    }
  } catch (err) {
    logger.error?.(`ops-monitor: failed to read last-state from KV: ${String(err?.message ?? err)}`);
  }

  const results = await probeAll(targets, { fetchImpl, now, timeoutMs });
  const { changes, nextStates } = diffStates(results, priorStates);

  // Persist next state (best-effort; a KV write failure must not crash the run).
  try {
    if (kv) await kv.put(STATE_KEY, JSON.stringify(nextStates));
  } catch (err) {
    logger.error?.(`ops-monitor: failed to persist last-state to KV: ${String(err?.message ?? err)}`);
  }

  const payload = buildAlertPayload(changes, { now, environment: env.ENVIRONMENT ?? "staging" });
  const alert = await sendAlert(payload, env, { fetchImpl, logger });

  const downNow = results.filter((r) => !r.up).map((r) => r.name);
  logger.log?.(
    `ops-monitor: checked ${results.length} target(s), ${downNow.length} down [${downNow.join(", ")}], ${changes.length} change(s)`
  );

  return { results, changes, nextStates, alert };
}

export { STATE_KEY, DEFAULT_TIMEOUT_MS };
