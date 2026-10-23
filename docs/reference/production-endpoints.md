# Production endpoints (#434, #436)

Shipped builds bake the production hosts; env vars override for staging only.

| Service | Production host | Worker / dir | Shell default | Override |
|---|---|---|---|---|
| Telemetry + crash upload | `https://telemetry.corevideo.io` | `corevideo-telemetry-ingest`, `services/telemetry-ingest/` | `CrashReportingConfig.DefaultEndpoint` (used by `CrashReportCoordinator` and `TelemetryEventService`) | `COREVIDEO_TELEMETRY_ENDPOINT` |
| Ops monitor | `https://ops.corevideo.io` | `corevideo-ops-monitor`, `services/ops-monitor/` | none (the shell never calls it; the worker probes the others) | `COREVIDEO_OPS_MONITOR_ENDPOINT` (scripts only) |
| Zoom OAuth broker | `https://auth.corevideo.io` (start `/oauth/start`, Zoom redirect `/callback`) | broker lives in the site repo (`site-worker.js`), not here | `ZoomOAuthManifest.DefaultBrokerStartUrl` / `DefaultBrokerCallbackUrl`, `src/config/zoomOAuth.json` | `COREVIDEO_ZOOM_OAUTH_BROKER_START_URL` |

The telemetry **API key is a secret and is NOT baked in**: with no `COREVIDEO_TELEMETRY_API_KEY`
the uploader and heartbeat stay quietly disabled. A shipped build therefore still needs the key
delivered out of band (installer config / machine env) before any upload happens.

`scripts/load-staging-env.ps1` points the current shell at the workers.dev staging Workers.

## Owner checklist (nothing below is done by the PR)

1. Cloudflare: the `corevideo.io` zone must be on the same account as the Workers. Deploy
   `services/telemetry-ingest` and `services/ops-monitor`; the `routes` custom_domain entries in
   each `wrangler.jsonc` create `telemetry.corevideo.io` / `ops.corevideo.io` (DNS + cert are
   automatic). Fill in the KV namespace ids first (see each README).
2. Broker: add the `auth.corevideo.io` custom domain to the site/broker Worker (other repo) and
   make it serve `/oauth/start`, `/callback`, `/oauth/redeem`, `/oauth/refresh`, `/oauth/sdk-jwt`
   (the shell now calls `/callback` as the Zoom redirect; keep `corevideo://oauth/callback` as the
   only allowed `return_uri`). Keep the old `corevideo.iamfatness.us` routes alive until the
   previous beta installs age out.
3. Zoom Marketplace: add `https://auth.corevideo.io/callback` to the OAuth redirect allow-list
   (keep the old entry until the old host is retired).
4. `OPS_BROKER_START_URL` need not be set; the ops-monitor default now probes
   `https://auth.corevideo.io/oauth/start?probe=ops-monitor`.
5. Verify one real upload: with the API key set and telemetry consent ON, launch a build with no
   endpoint env var, send a crash report (or wait for the heartbeat), then confirm the
   `launch.log` line shows `telemetry.corevideo.io`, `curl https://telemetry.corevideo.io/health`
   is 200, and the report appears in the R2 bucket / KV. Also complete one Sign in with Zoom
   against `auth.corevideo.io`.
