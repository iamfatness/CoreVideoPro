/**
 * Cloudflare Worker for CoreVideo Pro licensing + billing.
 * Env: LICENSE_API_KEY (optional bearer), LICENSE_KV, STRIPE_SECRET_KEY, STRIPE_PRICE_*.
 */
import {
  handleActivate,
  handleCheckoutSession,
  handleStartTrial,
  handleStripeWebhook,
  handleUpgrade,
  handleVerify
} from "./lib/handlers.mjs";

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (url.pathname === "/v1/webhooks/stripe" && request.method === "POST") {
      const result = await handleStripeWebhook(request, env);
      return json(result, result.status ?? (result.ok ? 200 : 500));
    }

    if (!authorize(request, env)) {
      return new Response("Unauthorized", { status: 401 });
    }

    if (request.method === "POST" && url.pathname === "/v1/trial/start") {
      const body = await readJson(request);
      if (!body) {
        return new Response("Invalid JSON", { status: 400 });
      }
      return json(await handleStartTrial(body, env));
    }

    if (request.method === "POST" && url.pathname === "/v1/license/activate") {
      const body = await readJson(request);
      if (!body) {
        return new Response("Invalid JSON", { status: 400 });
      }
      const result = await handleActivate(body, env);
      return json(result, result.status ?? 200);
    }

    if (request.method === "GET" && url.pathname === "/v1/license/verify") {
      const result = await handleVerify(url.searchParams, env);
      return json(result, result.status ?? 200);
    }

    if (request.method === "POST" && url.pathname === "/v1/checkout/session") {
      const body = await readJson(request);
      if (!body) {
        return new Response("Invalid JSON", { status: 400 });
      }
      const result = await handleCheckoutSession(body, env);
      return json(result, result.status ?? 200);
    }

    if (request.method === "POST" && url.pathname === "/v1/license/upgrade") {
      const body = await readJson(request);
      if (!body) {
        return new Response("Invalid JSON", { status: 400 });
      }
      const result = await handleUpgrade(body, env);
      return json(result, result.status ?? 200);
    }

    return new Response("Not found", { status: 404 });
  }
};

function authorize(request, env) {
  const expected = env.LICENSE_API_KEY?.trim();
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

function json(payload, status = 200) {
  return Response.json(payload, { status });
}