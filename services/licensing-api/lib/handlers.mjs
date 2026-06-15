import { buildEntitlements, isPaidTier, TRIAL_DAYS } from "./entitlements.mjs";
import { loadLicense, saveLicense } from "./store.mjs";

/**
 * @param {string | undefined} tier
 * @returns {"creator" | "pro" | "team"}
 */
function normalizeCheckoutTier(tier) {
  if (tier === "pro" || tier === "team") {
    return tier;
  }
  return "creator";
}

/**
 * @param {StoredLicense | undefined} stored
 * @returns {import("./types.mjs").LicenseEntitlements}
 */
function entitlementsFromStored(stored) {
  if (!stored) {
    return buildEntitlements("expired", "none", null, "", undefined);
  }

  const now = Date.now();
  const expired = stored.expiresAt ? Date.parse(stored.expiresAt) < now : false;
  const status = expired ? "expired" : "active";
  const tier = expired ? "expired" : stored.tier;
  return buildEntitlements(tier, status, stored.expiresAt, stored.licenseKey, stored.email);
}

/** @typedef {import("./types.mjs").StoredLicense} StoredLicense */

/**
 * @param {{ email?: string; deviceId?: string }} body
 * @param {object} env
 */
export async function handleStartTrial(body, env) {
  const email = typeof body.email === "string" ? body.email.trim() : undefined;
  const licenseKey = `cv-trial-${crypto.randomUUID()}`;
  const createdAt = new Date().toISOString();
  const expiresAt = new Date(Date.now() + TRIAL_DAYS * 24 * 60 * 60 * 1000).toISOString();
  const deviceId = typeof body.deviceId === "string" ? body.deviceId.trim() : undefined;

  /** @type {StoredLicense} */
  const stored = {
    licenseKey,
    email,
    tier: "trial",
    createdAt,
    expiresAt,
    activatedDevices: deviceId ? [deviceId] : []
  };
  await saveLicense(stored, env);

  return {
    licenseKey,
    entitlements: buildEntitlements("trial", "active", expiresAt, licenseKey, email)
  };
}

/**
 * @param {{ licenseKey?: string; deviceId?: string; email?: string }} body
 * @param {object} env
 */
export async function handleActivate(body, env) {
  const licenseKey = typeof body.licenseKey === "string" ? body.licenseKey.trim() : "";
  const deviceId = typeof body.deviceId === "string" ? body.deviceId.trim() : "";
  if (!licenseKey || !deviceId) {
    return { ok: false, status: 400, error: "licenseKey and deviceId are required." };
  }

  const stored = await loadLicense(licenseKey, env);
  if (!stored) {
    return { ok: false, status: 404, error: "License not found." };
  }

  const entitlements = entitlementsFromStored(stored);
  if (entitlements.status === "expired") {
    return { ok: false, status: 403, error: "License expired." };
  }

  if (!stored.activatedDevices.includes(deviceId)) {
    stored.activatedDevices = [...stored.activatedDevices, deviceId];
  }
  if (typeof body.email === "string" && body.email.trim()) {
    stored.email = body.email.trim();
  }
  await saveLicense(stored, env);

  return {
    ok: true,
    activated: true,
    entitlements: entitlementsFromStored(stored)
  };
}

/**
 * @param {URLSearchParams} params
 * @param {object} env
 */
export async function handleVerify(params, env) {
  const licenseKey = params.get("licenseKey")?.trim() ?? "";
  const deviceId = params.get("deviceId")?.trim() ?? "";
  if (!licenseKey) {
    return { ok: false, status: 400, error: "licenseKey is required." };
  }

  const stored = await loadLicense(licenseKey, env);
  if (!stored) {
    return {
      ok: true,
      valid: false,
      entitlements: buildEntitlements("expired", "none", null, licenseKey, undefined)
    };
  }

  const entitlements = entitlementsFromStored(stored);
  const deviceActivated = !deviceId || stored.activatedDevices.includes(deviceId);
  const valid = entitlements.status === "active" && deviceActivated;

  return { ok: true, valid, entitlements };
}

/**
 * @param {{ tier?: string; email?: string; successUrl?: string; cancelUrl?: string }} body
 * @param {object} env
 */
export async function handleCheckoutSession(body, env) {
  const tier = normalizeCheckoutTier(body.tier);
  const email = typeof body.email === "string" ? body.email.trim() : undefined;
  const stripeSecret = env.STRIPE_SECRET_KEY?.trim();

  if (!stripeSecret) {
    const sessionId = `cs_stub_${crypto.randomUUID()}`;
    const checkoutUrl =
      typeof body.successUrl === "string" && body.successUrl.trim()
        ? `${body.successUrl.trim()}?session_id=${sessionId}&tier=${tier}`
        : `https://checkout.stripe.com/stub/${sessionId}?tier=${tier}`;
    return { ok: true, sessionId, checkoutUrl, mode: "stub", tier, email };
  }

  const successUrl = body.successUrl?.trim() || "https://corevideo.pro/billing/success";
  const cancelUrl = body.cancelUrl?.trim() || "https://corevideo.pro/billing/cancel";
  const priceMap = {
    creator: env.STRIPE_PRICE_CREATOR,
    pro: env.STRIPE_PRICE_PRO,
    team: env.STRIPE_PRICE_TEAM
  };
  const priceId = priceMap[tier];
  if (!priceId) {
    return { ok: false, status: 500, error: `Stripe price id missing for tier ${tier}.` };
  }

  const form = new URLSearchParams();
  form.set("mode", "subscription");
  form.set("success_url", successUrl);
  form.set("cancel_url", cancelUrl);
  form.set("line_items[0][price]", priceId);
  form.set("line_items[0][quantity]", "1");
  if (email) {
    form.set("customer_email", email);
  }
  form.set("metadata[tier]", tier);

  const response = await fetch("https://api.stripe.com/v1/checkout/sessions", {
    method: "POST",
    headers: {
      authorization: `Bearer ${stripeSecret}`,
      "content-type": "application/x-www-form-urlencoded"
    },
    body: form.toString()
  });

  if (!response.ok) {
    return { ok: false, status: 502, error: `Stripe checkout failed (${response.status}).` };
  }

  const session = await response.json();
  return {
    ok: true,
    sessionId: session.id,
    checkoutUrl: session.url,
    mode: "stripe",
    tier,
    email
  };
}

/**
 * @param {Request} request
 * @param {object} env
 */
export async function handleStripeWebhook(request, env) {
  let event;
  try {
    event = await request.json();
  } catch {
    return { ok: false, status: 400, error: "Invalid JSON." };
  }

  if (event.type === "checkout.session.completed") {
    const session = event.data?.object ?? {};
    const tier = normalizeCheckoutTier(session.metadata?.tier);
    const email = typeof session.customer_email === "string" ? session.customer_email : session.customer_details?.email;
    const licenseKey = `cv-${tier}-${crypto.randomUUID()}`;

    /** @type {StoredLicense} */
    const stored = {
      licenseKey,
      email,
      tier,
      createdAt: new Date().toISOString(),
      expiresAt: null,
      activatedDevices: [],
      stripeCustomerId: session.customer,
      stripeSubscriptionId: session.subscription
    };
    await saveLicense(stored, env);

    return { ok: true, accepted: true, licenseKey, tier };
  }

  return { ok: true, accepted: true, ignored: event.type ?? "unknown" };
}

/**
 * Upgrade an existing license key to a paid tier (used by stub checkout success flow).
 * @param {{ licenseKey?: string; tier?: string }} body
 * @param {object} env
 */
export async function handleUpgrade(body, env) {
  const licenseKey = typeof body.licenseKey === "string" ? body.licenseKey.trim() : "";
  const tier = normalizeCheckoutTier(body.tier);
  if (!licenseKey) {
    return { ok: false, status: 400, error: "licenseKey is required." };
  }

  const stored = await loadLicense(licenseKey, env);
  if (!stored) {
    return { ok: false, status: 404, error: "License not found." };
  }

  if (!isPaidTier(tier)) {
    return { ok: false, status: 400, error: "Invalid paid tier." };
  }

  stored.tier = tier;
  stored.expiresAt = null;
  await saveLicense(stored, env);

  return { ok: true, entitlements: entitlementsFromStored(stored) };
}