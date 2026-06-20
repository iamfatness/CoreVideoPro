/**
 * Cloudflare Worker — CoreVideo Pro AI-director intelligence service (Item 10).
 *
 * Consumes the richer auto-production signals (speaker turns, sentiment,
 * screen-share content, engagement) and asks the latest Claude model to propose
 * a scene/transition. The renderer's `AiProductionEngine` calls `POST /v1/propose`
 * through the typed contract and gates whatever comes back with the always-on
 * heuristic stabilizer — so this service is advisory only.
 *
 * Auth: optional bearer via `env.AI_DIRECTOR_API_KEY` (a Worker secret).
 * Model key: `env.ANTHROPIC_API_KEY` (a Worker secret) — never committed.
 */
import { requestDirectorProposal } from "./lib/claudeClient.mjs";

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method === "GET" && url.pathname === "/v1/health") {
      return Response.json({ ok: true, configured: Boolean(env.ANTHROPIC_API_KEY?.trim()) });
    }

    if (!authorize(request, env)) {
      return new Response("Unauthorized", { status: 401 });
    }

    if (request.method === "POST" && url.pathname === "/v1/propose") {
      const body = await readJson(request);
      if (!body) {
        return new Response("Invalid JSON", { status: 400 });
      }
      try {
        const { proposal, modelId } = await requestDirectorProposal(body, env);
        return Response.json({ proposal, modelId });
      } catch (error) {
        // The renderer treats any non-2xx as "no proposal" and falls back to the
        // heuristic; surface a 502 with a short reason for observability.
        return Response.json(
          { error: String(error?.message ?? error) },
          { status: 502 }
        );
      }
    }

    return new Response("Not found", { status: 404 });
  }
};

function authorize(request, env) {
  const expected = env.AI_DIRECTOR_API_KEY?.trim();
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
