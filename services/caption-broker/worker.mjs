/**
 * Cloudflare Worker caption transcription broker (cloud-first stub for staging).
 * Env: CAPTION_BROKER_API_KEY (optional bearer).
 */
import { handleEndSession, handlePushChunk, handleStartSession } from "./lib/handlers.mjs";

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (!authorize(request, env)) {
      return new Response("Unauthorized", { status: 401 });
    }

    if (request.method === "POST" && url.pathname === "/v1/sessions") {
      const body = await readJson(request);
      if (!body) {
        return new Response("Invalid JSON", { status: 400 });
      }
      return Response.json(handleStartSession(body));
    }

    const chunkMatch = url.pathname.match(/^\/v1\/sessions\/([^/]+)\/chunk$/);
    if (request.method === "POST" && chunkMatch) {
      const body = await readJson(request);
      if (!body) {
        return new Response("Invalid JSON", { status: 400 });
      }
      const result = handlePushChunk(chunkMatch[1], body);
      return Response.json(result, { status: result.status ?? 200 });
    }

    const endMatch = url.pathname.match(/^\/v1\/sessions\/([^/]+)\/end$/);
    if (request.method === "POST" && endMatch) {
      const result = handleEndSession(endMatch[1]);
      return Response.json(result, { status: result.status ?? 200 });
    }

    return new Response("Not found", { status: 404 });
  }
};

function authorize(request, env) {
  const expected = env.CAPTION_BROKER_API_KEY?.trim();
  if (!expected) {
    return true;
  }
  const header = request.headers.get("authorization") ?? "";
  return header === `Bearer ${expected}`;
}

async function readJson(request) {
  try {
    return await request.json();
  } catch {
    return undefined;
  }
}