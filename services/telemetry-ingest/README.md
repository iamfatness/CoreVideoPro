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

- Body: `application/json` **or** `application/zip` (S1 crash archives), max
  **25MB**. Any other content type → `415`.
- JSON metadata fields (all optional): `reason` (string),
  `app.version`/`version` (string), `machineClass`/`app.machineClass` (string).
  The **full body** is stored verbatim regardless.
- Zip bodies are **opaque** — never parsed server-side (an empty body is
  `400`). Metadata rides in headers (preferred) or query params, headers win:
  `X-CoreVideo-Version` / `?version=`, `X-CoreVideo-Machine-Class` /
  `?machineClass=`, `X-CoreVideo-Reason` / `?reason=` (trimmed, capped at 200
  chars each).
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
| 400 | Body is not valid JSON / zip body is empty |
| 401 | Missing/wrong bearer key |
| 404 | Unknown path |
| 405 | Non-POST method |
| 413 | Body over the cap (25MB crashes / 64KB events) — do NOT retry the same payload |
| 415 | Content type other than application/json (or application/zip on crashes) |
| 429 | Per-IP rate limit (60 req/min); honors `Retry-After` |
| 500 | `TELEMETRY_API_KEY` not set on the worker (client: retry later, never block the app) |

## Storage layout

- **R2 (`REPORTS_BUCKET`)** — crash payloads only, stored verbatim at
  `crashes/<yyyy-mm-dd>/<reportId>.json` (JSON) or `…/<reportId>.zip` (S1
  archives).
- **KV (`REPORTS_KV`)** — one index entry per report at `report:<reportId>`
  with `{ reportId, kind, receivedAt, contentType, version, machineClass,
  size, r2Key, reason|name }`. Events additionally carry their full `payload`
  inline — they are ≤64KB, so a per-event R2 object would add a write + read
  for no benefit.
- `reportId` embeds the receive date (`cv-<yyyymmdd>-<uuid>`), so KV keys
  sort by day and a bare reportId is enough to find everything.

List a day's reports:

```
npx wrangler kv key list --binding REPORTS_KV --prefix report:cv-20260718
npx wrangler kv key get --binding REPORTS_KV report:<reportId>
npx wrangler r2 object get corevideo-telemetry-reports crashes/2026-07-18/<reportId>.json
```

## Shell client (S1 crash pipeline)

The WinUI shell's crash reporter (`CrashReportCoordinator` /
`CrashReportUploader`, spec §S1) posts `application/zip` archives to
`/v1/crashes` with the `X-CoreVideo-*` metadata headers. It is configured via
two environment/settings keys — **both empty by default, which quietly
disables crash reporting** (dev builds); the beta config ships them:

| Key | Meaning |
|---|---|
| `COREVIDEO_TELEMETRY_ENDPOINT` | Base URL of this worker, e.g. `https://corevideo-telemetry-ingest.<account>.workers.dev` |
| `COREVIDEO_TELEMETRY_API_KEY` | The same value as the worker's `TELEMETRY_API_KEY` secret |

Consent posture: dumps are detected against an offer-once watermark
(`%LOCALAPPDATA%\CoreVideoPro\crash-watermark.json`), nothing is uploaded
without the operator clicking Send, and the assembled zip stays under
`%LOCALAPPDATA%\CoreVideoPro\support-bundles\` before and after upload
(accepted reportIds are recorded in the watermark file).

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
