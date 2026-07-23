# ops-monitor

Cron-triggered Cloudflare Worker that watches the runtime dependencies beta
makes hard (beta spec `docs/beta-engineering-spec.md` §S5): the external OAuth
broker and the in-repo workers (`licensing-api`, `telemetry-ingest`,
`caption-broker`) plus itself. On a **state change** it alerts a generic webhook
(Discord/Slack). It never alert-spams (state-change only), never crashes on a
probe failure (the thing it monitors is expected to fail), and probes the broker
**read-only**.

## What it does

Every 5 minutes (Cron Trigger `*/5 * * * *`) it:

1. Loads the last up/down state of each target from KV (`OPS_STATE`).
2. Probes every target concurrently (`GET`, 10s timeout, `redirect: "manual"`).
   A target is **up** when it answers with any status `< 500`, **down** on a
   transport failure (DNS/timeout/reset) or a `5xx`.
3. Diffs against the stored state (**flap-dampener**): a target that stays down
   for hours produces exactly ONE alert. A missing prior state is treated as
   `up`, so a healthy first run is silent but a target already down is surfaced.
4. Persists the new state and, if anything changed, posts ONE aggregated alert.

It also serves an unauthenticated `GET /health` (so it is itself monitorable)
and `POST /run` (an on-demand check for manual verification).

## Broker probe — why it is side-effect-free

The OAuth broker (`corevideo.iamfatness.us`) lives in a **different repo**; we
can only monitor it, not edit it. Hitting `/oauth/start` normally would generate
OAuth state and redirect to Zoom — a side effect we must not trigger every 5
minutes.

The safe probe: `GET /oauth/start?probe=ops-monitor` **with no `return_uri`**.
The deployed broker (`site-worker.js handleOauthStart`) hard-rejects any
`return_uri` other than `corevideo://oauth/callback` **before** it generates any
state or redirect (this is the S4-documented behavior — see the repo CLAUDE.md
"Secrets at rest + OAuth return URI"). So a missing `return_uri` gets a `4xx`
rejection with **zero side effects**, and that `4xx` (being `< 500`) proves the
worker is up and routing. `redirect: "manual"` guarantees we never follow a
redirect off to Zoom even if the broker's behavior changes. The probe URL is
fully overridable via `OPS_BROKER_START_URL` if the broker later grows a
dedicated health endpoint — point it there and delete the query marker.

## In-repo worker probes

Each in-repo worker now serves an unauthenticated `GET /health` -> `200
{status:"ok", service, environment}` (added in this same change, placed before
the auth gate). The monitor hits `/health` on each. Because "up" only requires a
`< 500` response, the monitor still reports **up** even against a worker that
hasn't been redeployed with `/health` yet (it returns `404`, which is `< 500` =
the worker is reachable). Once redeployed, `/health` returns a clean `200`.

## Env contract

| Key | Kind | Default | Meaning |
|---|---|---|---|
| `OPS_ALERT_WEBHOOK_URL` | secret | *(unset)* | Discord/Slack incoming webhook. **Unset => log-only** (never crash, never spam). |
| `OPS_BROKER_START_URL` | var | `https://corevideo.iamfatness.us/oauth/start?probe=ops-monitor` | Broker probe URL (no `return_uri`). |
| `OPS_LICENSING_URL` | var | `…/corevideo-licensing-api.<acct>.workers.dev/health` | licensing-api probe. |
| `OPS_TELEMETRY_URL` | var | `…/corevideo-telemetry-ingest.<acct>.workers.dev/health` | telemetry-ingest probe. |
| `OPS_CAPTION_URL` | var | `…/corevideo-caption-broker.<acct>.workers.dev/health` | caption-broker probe. |
| `OPS_SELF_URL` | var | `…/corevideo-ops-monitor.<acct>.workers.dev/health` | self probe. |

Any target URL set to `""` or `"off"` is skipped. Set the `.workers.dev`
defaults to your account subdomain via the `OPS_*_URL` vars if it differs from
`wallace-john-w`.

**Alert destination is deliberately deferred to the owner.** Set
`OPS_ALERT_WEBHOOK_URL` whenever a Discord or Slack channel exists — no decision
is needed now. A generic incoming-webhook URL works for either: the payload
carries BOTH `content` (Discord) and `text` (Slack) and each service ignores the
key it doesn't use. There is **no email path** in this repo (no Cloudflare Email
binding exists on any service), so this is webhook-or-log-only, which §S5 allows.

## Deploying (owner)

One-time KV namespace creation (the `wrangler.jsonc` id is a placeholder until
then):

```powershell
cd services/ops-monitor
npx wrangler kv namespace create OPS_STATE
# paste the returned namespace id into wrangler.jsonc ("kv_namespaces"[0].id)
```

Optional: set the alert webhook (skip for log-only):

```powershell
# either directly:
"https://discord.com/api/webhooks/XXX/YYY" | npx wrangler secret put OPS_ALERT_WEBHOOK_URL
# or via the staging deploy flow: add to scripts/.staging-secrets.local:
#   COREVIDEO_OPS_ALERT_WEBHOOK_URL=https://discord.com/api/webhooks/XXX/YYY
```

Then deploy with the rest of the staging workers:

```powershell
npm run deploy:staging-workers   # includes ops-monitor (optional webhook secret)
```

Verify: `curl https://corevideo-ops-monitor.<account>.workers.dev/health`, or
force a check with `curl -X POST …/run` and read the JSON summary. Cron runs are
visible in `npx wrangler tail` (each tick logs the checked/down/changed counts).

## Tests

Pure unit tests (vitest, node env, injected fetch/KV/clock — no live network):

```powershell
npx vitest run --config vite.config.ts services/ops-monitor
```

Coverage: the flap-dampener (alerts only on state change, silent while a target
stays down), multi-target aggregation (one webhook for N changed targets), the
no-destination log-only path, the failed-target alert payload shape, the
never-throw probe on transport failure, and KV-read-failure resilience.
