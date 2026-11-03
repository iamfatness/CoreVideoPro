# Production endpoints (#434, #436)

> **Owner ruling 2026-10-25: the Zoom OAuth broker stays on `https://corevideo.iamfatness.us`.**
> Its `/oauth/callback` is the redirect registered with the Zoom app. #1034 had moved the shell's
> default to `auth.corevideo.io`, which does not resolve; beta-2026-10-25-ebc974d shipped that and
> Sign in with Zoom failed on the owner's machine (`zoom-join: credential error No such host is
> known. (auth.corevideo.io:443)`). The default is back on the live host. `telemetry.corevideo.io`
> and `ops.corevideo.io` still do not resolve (checklist steps 1 and 5), so uploads stay disabled.
> Known gap (not changed here): `ZoomOAuthService.cs` (~line 108) leaves `_pending` set with no
> timeout when the browser hits a dead host.

Shipped builds bake the production hosts; env vars override for staging only.

| Service | Production host | Worker / dir | Shell default | Override |
|---|---|---|---|---|
| Telemetry + crash upload | `https://telemetry.corevideo.io` | `corevideo-telemetry-ingest`, `services/telemetry-ingest/` | `CrashReportingConfig.DefaultEndpoint` (used by `CrashReportCoordinator` and `TelemetryEventService`) | `COREVIDEO_TELEMETRY_ENDPOINT` |
| Ops monitor | `https://ops.corevideo.io` | `corevideo-ops-monitor`, `services/ops-monitor/` | none (the shell never calls it; the worker probes the others) | `COREVIDEO_OPS_MONITOR_ENDPOINT` (scripts only) |
| Zoom OAuth broker | `https://corevideo.iamfatness.us` (start `/oauth/start`, Zoom redirect `/oauth/callback`; owner ruling 2026-10-25, do not move) | broker lives in the site repo (`site-worker.js`), not here | `ZoomOAuthManifest.DefaultBrokerStartUrl` / `DefaultBrokerCallbackUrl`, `src/config/zoomOAuth.json` | `COREVIDEO_ZOOM_OAUTH_BROKER_START_URL` |

The telemetry **API key is a secret and is NOT baked in**: with no `COREVIDEO_TELEMETRY_API_KEY`
the uploader and heartbeat stay quietly disabled. A shipped build therefore still needs the key
delivered out of band (installer config / machine env) before any upload happens.

`scripts/load-staging-env.ps1` points the current shell at the workers.dev staging Workers.

## Owner checklist (nothing below is done by the PR)

1. Cloudflare: the `corevideo.io` zone must be on the same account as the Workers. Deploy
   `services/telemetry-ingest` and `services/ops-monitor`; the `routes` custom_domain entries in
   each `wrangler.jsonc` create `telemetry.corevideo.io` / `ops.corevideo.io` (DNS + cert are
   automatic). Fill in the KV namespace ids first (see each README).
2. Broker: nothing to do. The broker stays on `corevideo.iamfatness.us` (owner ruling 2026-10-25);
   `corevideo://oauth/callback` remains the only allowed `return_uri`. Any future host move needs
   the owner, a new Zoom redirect allow-list entry, and the old routes kept alive until installed
   betas age out. The `DefaultBrokerCallbackUrl` in the shell is informational only: the deployed
   broker (site repo `site-worker.js`) routes `/oauth/callback` and builds its own `redirect_uri`
   from `ZOOM_OAUTH_REDIRECT_URI` or `${origin}/oauth/callback`, ignoring the shell's query param.
3. Zoom Marketplace: the allow-list entry is `https://corevideo.iamfatness.us/oauth/callback`; leave it.
4. `OPS_BROKER_START_URL` need not be set; the ops-monitor default probes
   `https://corevideo.iamfatness.us/oauth/start?probe=ops-monitor`.
5. Verify one real upload: with the API key set and telemetry consent ON, launch a build with no
   endpoint env var, send a crash report (or wait for the heartbeat), then confirm the
   `launch.log` startup line reads `crash-report: telemetry endpoint host=telemetry.corevideo.io apiKeyPresent=True`, `curl https://telemetry.corevideo.io/health`
   is 200, and the report appears in the R2 bucket / KV. Also complete one Sign in with Zoom
   against `corevideo.iamfatness.us`.
