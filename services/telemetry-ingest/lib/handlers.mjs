/**
 * Request handlers for the telemetry-ingest worker (S0 storage).
 *
 * Storage layout:
 * - Crashes: full POSTed body → R2 `REPORTS_BUCKET` at
 *   `crashes/<yyyy-mm-dd>/<reportId><ext>` (ext by content-type; only
 *   application/json is accepted in S0, but the key scheme already handles
 *   the S1 zip/binary uploads), plus a KV index entry.
 * - Events: KV `REPORTS_KV` ONLY — the payload is capped at 64KB, which fits
 *   a KV value trivially, so a per-event R2 object would add a second write
 *   and a second read for no benefit. The event payload rides inline in the
 *   index entry (`payload` field).
 * - KV index key: `report:<reportId>`. reportIds embed the receive date
 *   (`cv-<yyyymmdd>-<uuid>`), so `wrangler kv key list --prefix report:cv-20260718`
 *   lists a day's reports AND a bare reportId lookup stays a single get.
 */

export const MAX_CRASH_BYTES = 25 * 1024 * 1024; // 25MB — S1 crash zips (dump + logs)
export const MAX_EVENT_BYTES = 64 * 1024; // 64KB — S3 telemetry events are tiny

export async function handleCrash(request, env) {
  const contentType = normalizeContentType(request.headers.get("content-type"));
  if (contentType !== "application/json") {
    return new Response(
      "Unsupported content type: S0 accepts application/json (zip archives arrive with S1)",
      { status: 415 }
    );
  }

  const body = await readBodyWithCap(request, MAX_CRASH_BYTES);
  if (body.tooLarge) {
    return new Response(`Payload too large (max ${MAX_CRASH_BYTES} bytes)`, { status: 413 });
  }
  const payload = parseJson(body.bytes);
  if (payload === undefined) {
    return new Response("Invalid JSON", { status: 400 });
  }

  const receivedAt = new Date().toISOString();
  const reportId = makeReportId(receivedAt);
  const r2Key = `crashes/${receivedAt.slice(0, 10)}/${reportId}${extensionForContentType(contentType)}`;

  await env.REPORTS_BUCKET.put(r2Key, body.bytes, {
    httpMetadata: { contentType }
  });
  await env.REPORTS_KV.put(
    kvKeyForReport(reportId),
    JSON.stringify({
      reportId,
      kind: "crash",
      receivedAt,
      version: extractVersion(payload),
      machineClass: extractMachineClass(payload),
      reason: typeof payload?.reason === "string" ? payload.reason : null,
      size: body.bytes.byteLength,
      r2Key
    })
  );

  return Response.json({ reportId, accepted: true }, { status: 202 });
}

export async function handleEvent(request, env) {
  const contentType = normalizeContentType(request.headers.get("content-type"));
  if (contentType !== "application/json") {
    return new Response("Unsupported content type: events must be application/json", {
      status: 415
    });
  }

  const body = await readBodyWithCap(request, MAX_EVENT_BYTES);
  if (body.tooLarge) {
    return new Response(`Payload too large (max ${MAX_EVENT_BYTES} bytes)`, { status: 413 });
  }
  const payload = parseJson(body.bytes);
  if (payload === undefined) {
    return new Response("Invalid JSON", { status: 400 });
  }

  const receivedAt = new Date().toISOString();
  const reportId = makeReportId(receivedAt);

  await env.REPORTS_KV.put(
    kvKeyForReport(reportId),
    JSON.stringify({
      reportId,
      kind: "event",
      receivedAt,
      version: extractVersion(payload),
      machineClass: extractMachineClass(payload),
      name: typeof payload?.name === "string" ? payload.name : null,
      size: body.bytes.byteLength,
      r2Key: null,
      payload
    })
  );

  return Response.json({ reportId, accepted: true }, { status: 202 });
}

/**
 * reportId embeds the receive date so KV index keys sort by day and the id
 * alone is enough to locate both the KV entry and (for crashes) the R2 day
 * prefix. Format: cv-<yyyymmdd>-<uuid>.
 */
export function makeReportId(receivedAtIso) {
  const compactDay = receivedAtIso.slice(0, 10).replace(/-/g, "");
  return `cv-${compactDay}-${crypto.randomUUID()}`;
}

export function kvKeyForReport(reportId) {
  return `report:${reportId}`;
}

/** S1-ready key extensions: json today, zip/bin when binary uploads land. */
export function extensionForContentType(contentType) {
  if (contentType === "application/json") return ".json";
  if (contentType === "application/zip") return ".zip";
  return ".bin";
}

export function normalizeContentType(header) {
  if (!header) return "application/json"; // parse-based fallback, matches pre-S0 behavior
  return header.split(";")[0].trim().toLowerCase();
}

/**
 * Reads the request body while enforcing a byte cap. Rejects early on a
 * declared Content-Length over the cap and aborts mid-stream otherwise, so
 * an over-cap upload never buffers past the limit.
 */
export async function readBodyWithCap(request, cap) {
  const declared = Number(request.headers.get("content-length") ?? "0");
  if (declared > cap) {
    return { tooLarge: true };
  }
  if (!request.body) {
    return { bytes: new Uint8Array(0) };
  }

  const reader = request.body.getReader();
  const chunks = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > cap) {
      await reader.cancel();
      return { tooLarge: true };
    }
    chunks.push(value);
  }

  const bytes = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return { bytes };
}

function parseJson(bytes) {
  try {
    return JSON.parse(new TextDecoder().decode(bytes));
  } catch {
    return undefined;
  }
}

function extractVersion(payload) {
  const version = payload?.app?.version ?? payload?.version;
  return typeof version === "string" ? version : null;
}

function extractMachineClass(payload) {
  const machineClass = payload?.machineClass ?? payload?.app?.machineClass;
  return typeof machineClass === "string" ? machineClass : null;
}
