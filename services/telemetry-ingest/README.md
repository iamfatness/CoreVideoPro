# telemetry-ingest

Cloudflare Worker that receives CoreVideo Pro crash reports and telemetry
events (beta spec `docs/beta-engineering-spec.md` §S0). Since S0 it has real
storage: crash payloads land in R2, every report is indexed in KV, auth is
required, and bodies are size-capped and rate-limited.

## API contract (stable for S1/S3 clients)

Auth: every request needs `Authorization: Bearer <TELEMETRY_API_KEY>`.
Wrong/missing key → `401`. Key not configured on the worker → `500` (loud
misconfiguration — the service never runs open).

### POST `/v1/crashes`

- Body: `application/json`, max **25MB**. (S1 will add zip archives; the R2
  key scheme already handles `application/zip` → `.zip`, anything else →
  `.bin` — the worker returns `415` for non-JSON until S1 lands.)
- Recognized metadata fields (all optional): `reason` (string),
  `app.version`/`version` (string), `machineClass`/`app.machineClass` (string).
  The **full body** is stored verbatim regardless.
- Response `202`:

  ```json
  { "reportId": "cv-20260718-<uuid>", "accepted": true }
  ```

### POST `/v1/events`

- Body: `application/json`, max **64KB**.
- Recognized metadata fields: `name`, `app.version`/`version`,
  `machineClass`/`app.machineClass`.
- Response `202`: same shape as crashes.

### Errors

| Status | Meaning |
|---|---|
| 400 | Body is not valid JSON |
| 401 | Missing/wrong bearer key |
| 404 | Unknown path |
| 405 | Non-POST method |
| 413 | Body over the cap (25MB crashes / 64KB events) |
| 415 | Non-JSON content type (until S1 archives) |
| 429 | Per-IP rate limit (60 req/min); honors `Retry-After` |
| 500 | `TELEMETRY_API_KEY` not set on the worker |

## Storage layout

- **R2 (`REPORTS_BUCKET`)** — crash payloads only, stored verbatim at
  `crashes/<yyyy-mm-dd>/<reportId>.json` (`.zip`/`.bin` reserved for S1).
- **KV (`REPORTS_KV`)** — one index entry per report at `report:<reportId>`
  with `{ reportId, kind, receivedAt, version, machineClass, size, r2Key,
  reason|name }`. Events additionally carry their full `payload` inline —
  they are ≤64KB, so a per-event R2 object would add a write + read for no
  benefit.
- `reportId` embeds the receive date (`cv-<yyyymmdd>-<uuid>`), so KV keys
  sort by day and a bare reportId is enough to find everything.

List a day's reports:

```
npx wrangler kv key list --binding REPORTS_KV --prefix report:cv-20260718
npx wrangler kv key get --binding REPORTS_KV report:<reportId>
npx wrangler r2 object get corevideo-telemetry-reports crashes/2026-07-18/<reportId>.json
```

## Rate limiting (honest limits)

Per-IP token bucket, 60 requests/min, implemented **in-isolate**
(`lib/rateLimit.mjs`): each isolate/colo has its own buckets and a recycle
resets them, so it bounds abuse per isolate, not globally. That stops the
realistic beta failure (a client retry loop) but not a distributed attacker.
Upgrade path: Cloudflare's native rate limiting binding or a Durable Object.

## Deploying (owner)

One-time resource creation (do this before the first S0 deploy — the
`wrangler.jsonc` KV id is a placeholder until then):

```powershell
cd services/telemetry-ingest
npx wrangler r2 bucket create corevideo-telemetry-reports
npx wrangler kv namespace create REPORTS_KV
# paste the returned namespace id into wrangler.jsonc ("kv_namespaces"[0].id)
```

Then the normal flow — `scripts/deploy-staging-workers.ps1` sets the
`TELEMETRY_API_KEY` secret (from `scripts/.staging-secrets.local`,
`COREVIDEO_TELEMETRY_API_KEY=...`) and runs `wrangler deploy`. Verify with
`npm run smoke:staging-services` (posts one crash + one event and asserts a
`reportId` comes back).

## Tests

Vitest (repo root tooling), pure unit tests with in-memory KV/R2 fakes:

```powershell
npx vitest run --config vite.config.ts services/telemetry-ingest
```
