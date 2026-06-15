/**
 * Cloudflare Worker ingest stub for CoreVideo Pro crash + telemetry uploads.
 * Deploy separately from the demo site worker; bind KV in production if persistence is needed.
 */
export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method !== "POST") {
      return new Response("Method not allowed", { status: 405 });
    }

    const authorized = authorize(request, env);
    if (!authorized) {
      return new Response("Unauthorized", { status: 401 });
    }

    let payload;
    try {
      payload = await request.json();
    } catch {
      return new Response("Invalid JSON", { status: 400 });
    }

    const reportId = `cv-${crypto.randomUUID()}`;
    if (url.pathname === "/v1/crashes") {
      console.log("crash", reportId, payload?.reason ?? "unknown");
      return Response.json({ reportId, accepted: true }, { status: 202 });
    }

    if (url.pathname === "/v1/events") {
      console.log("event", reportId, payload?.name ?? "unknown");
      return Response.json({ reportId, accepted: true }, { status: 202 });
    }

    return new Response("Not found", { status: 404 });
  }
};

function authorize(request, env) {
  const expected = env.TELEMETRY_API_KEY?.trim();
  if (!expected) {
    return true;
  }
  const header = request.headers.get("authorization") ?? "";
  return header === `Bearer ${expected}`;
}