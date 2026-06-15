import {
  ZOOM_OAUTH_BROKER_START_URL,
  ZOOM_OAUTH_REDIRECT_URI,
  ZOOM_OAUTH_SCOPES,
  brokerEndpoint,
  brokerBaseUrl,
  isBrokerStartUrl,
  zoomOAuthBrokerConfigured
} from "../src/config/zoomOAuth.ts";
import { pkceChallenge, randomBase64Url } from "./zoomOAuthCrypto.ts";
import type { ZoomOAuthTokens, ZoomTokenStore } from "./zoomTokenStore.ts";

export type ZoomOAuthSessionStatus = {
  brokerConfigured: boolean;
  signedIn: boolean;
  expiresAt: number;
  pendingAuthorization: boolean;
};

export type ZoomJoinCredentials = {
  sdkJwt?: string;
  userZak?: string;
  usePublicAppKey: boolean;
};

export type ZoomOAuthServiceOptions = {
  tokenStore: ZoomTokenStore;
  fetch?: typeof fetch;
  openUrl?: (url: string) => Promise<void> | void;
  nowSeconds?: () => number;
};

type PendingAuthorization = {
  state: string;
  verifier: string;
  brokerBaseUrl: string;
};

function oauthErrorMessage(body: string, fallback: string): string {
  try {
    const parsed = JSON.parse(body) as {
      error?: string;
      reason?: string;
      message?: string;
    };
    if (parsed.error === "invalid_client") {
      return "Zoom rejected the OAuth client. Confirm the Marketplace app is configured for Public Client OAuth (PKCE).";
    }
    if (parsed.reason) return parsed.reason;
    if (parsed.message) return parsed.message;
    if (parsed.error) return parsed.error;
  } catch {
    // ignore
  }
  return body || fallback;
}

function parseTokenResponse(body: string): ZoomOAuthTokens {
  const parsed = JSON.parse(body) as {
    access_token?: string;
    refresh_token?: string;
    expires_in?: number;
    message?: string;
  };
  if (!parsed.access_token) {
    throw new Error(parsed.message ?? "Zoom did not return an access token.");
  }
  const now = Math.floor(Date.now() / 1000);
  return {
    accessToken: parsed.access_token,
    refreshToken: parsed.refresh_token ?? "",
    expiresAt: now + (parsed.expires_in ?? 3600) - 60
  };
}

export class ZoomOAuthService {
  private readonly tokenStore: ZoomTokenStore;
  private readonly fetchImpl: typeof fetch;
  private readonly openUrl: (url: string) => Promise<void> | void;
  private readonly nowSeconds: () => number;
  private pending: PendingAuthorization | undefined;

  constructor(options: ZoomOAuthServiceOptions) {
    this.tokenStore = options.tokenStore;
    this.fetchImpl = options.fetch ?? fetch;
    this.openUrl = options.openUrl ?? (() => undefined);
    this.nowSeconds = options.nowSeconds ?? (() => Math.floor(Date.now() / 1000));
  }

  async getStatus(): Promise<ZoomOAuthSessionStatus> {
    const tokens = await this.tokenStore.load();
    return {
      brokerConfigured: zoomOAuthBrokerConfigured(),
      signedIn: Boolean(tokens?.refreshToken || tokens?.accessToken),
      expiresAt: tokens?.expiresAt ?? 0,
      pendingAuthorization: this.pending !== undefined
    };
  }

  async beginAuthorization(): Promise<void> {
    if (!zoomOAuthBrokerConfigured()) {
      throw new Error("Zoom OAuth broker is not configured for this build.");
    }

    const authorizationUrl = ZOOM_OAUTH_BROKER_START_URL;
    const url = new URL(authorizationUrl);
    if (!url.protocol.startsWith("http") || !url.host) {
      throw new Error("The Zoom authorization URL is not valid.");
    }

    const useBroker = isBrokerStartUrl(authorizationUrl);
    const state = randomBase64Url(32);
    const verifier = useBroker ? "" : randomBase64Url(64);
    const brokerBase = useBroker ? brokerBaseUrl(authorizationUrl) : "";

    url.searchParams.set("state", state);
    if (useBroker) {
      url.searchParams.set("return_uri", ZOOM_OAUTH_REDIRECT_URI);
    } else {
      url.searchParams.set("response_type", "code");
      url.searchParams.set("redirect_uri", ZOOM_OAUTH_REDIRECT_URI);
      url.searchParams.set("scope", ZOOM_OAUTH_SCOPES);
      url.searchParams.set("code_challenge", pkceChallenge(verifier));
      url.searchParams.set("code_challenge_method", "S256");
    }

    this.pending = { state, verifier, brokerBaseUrl: brokerBase };
    await this.openUrl(url.toString());
  }

  async handleRedirectUrl(callbackUrl: string): Promise<void> {
    const url = new URL(callbackUrl);
    const oauthError = url.searchParams.get("error");
    if (oauthError) {
      throw new Error(`Zoom OAuth failed: ${oauthError}`);
    }

    const pending = this.pending;
    if (!pending || (!url.searchParams.get("broker_token") && !pending.verifier)) {
      throw new Error("Zoom returned a callback but no sign-in is in progress. Start sign-in again.");
    }

    const state = url.searchParams.get("state") ?? "";
    if (!state || state !== pending.state) {
      throw new Error("Zoom OAuth callback state did not match the active sign-in.");
    }

    const brokerToken = url.searchParams.get("broker_token");
    if (brokerToken) {
      const response = await this.fetchImpl(brokerEndpoint(pending.brokerBaseUrl || brokerBaseUrl(ZOOM_OAUTH_BROKER_START_URL), "/oauth/redeem"), {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({ broker_token: brokerToken })
      });
      const body = await response.text();
      if (!response.ok) {
        throw new Error(`Zoom broker token redeem failed: ${oauthErrorMessage(body, response.statusText)}`);
      }
      await this.saveTokenResponse(body);
      this.pending = undefined;
      return;
    }

    const code = url.searchParams.get("code");
    if (!code) {
      throw new Error("OAuth callback did not include an authorization code.");
    }

    const response = await this.fetchImpl("https://zoom.us/oauth/token", {
      method: "POST",
      headers: { "content-type": "application/x-www-form-urlencoded" },
      body: new URLSearchParams({
        grant_type: "authorization_code",
        code,
        redirect_uri: ZOOM_OAUTH_REDIRECT_URI,
        code_verifier: pending.verifier
      }).toString()
    });
    const body = await response.text();
    if (!response.ok) {
      throw new Error(`Zoom token exchange failed: ${oauthErrorMessage(body, response.statusText)}`);
    }
    await this.saveTokenResponse(body);
    this.pending = undefined;
  }

  async signOut(): Promise<void> {
    this.pending = undefined;
    await this.tokenStore.clear();
  }

  async ensureJoinCredentials(): Promise<ZoomJoinCredentials> {
    const tokens = await this.tokenStore.load();
    if (!tokens?.accessToken && !tokens?.refreshToken) {
      return { usePublicAppKey: true };
    }

    await this.ensureAccessToken();
    const refreshed = await this.tokenStore.load();
    if (!refreshed?.accessToken) {
      return { usePublicAppKey: true };
    }

    if (zoomOAuthBrokerConfigured()) {
      const sdkJwt = await this.fetchSdkJwt(refreshed.accessToken);
      const userZak = await this.fetchZak(refreshed.accessToken);
      return { sdkJwt, userZak, usePublicAppKey: false };
    }

    const userZak = await this.fetchZak(refreshed.accessToken);
    return { userZak, usePublicAppKey: true };
  }

  private async ensureAccessToken(): Promise<void> {
    const tokens = await this.tokenStore.load();
    if (!tokens) {
      throw new Error("Sign in with Zoom before joining.");
    }
    if (tokens.expiresAt > this.nowSeconds()) {
      return;
    }
    if (!tokens.refreshToken) {
      throw new Error("Zoom OAuth session expired. Sign in again.");
    }
    await this.refreshAccessToken(tokens.refreshToken);
  }

  private async refreshAccessToken(refreshToken: string): Promise<void> {
    if (zoomOAuthBrokerConfigured()) {
      const response = await this.fetchImpl(brokerEndpoint(brokerBaseUrl(ZOOM_OAUTH_BROKER_START_URL), "/oauth/refresh"), {
        method: "POST",
        headers: { "content-type": "application/json", accept: "application/json" },
        body: JSON.stringify({ refresh_token: refreshToken })
      });
      const body = await response.text();
      if (!response.ok) {
        throw new Error(`Zoom broker token refresh failed: ${oauthErrorMessage(body, response.statusText)}`);
      }
      await this.saveTokenResponse(body);
      return;
    }

    const response = await this.fetchImpl("https://zoom.us/oauth/token", {
      method: "POST",
      headers: { "content-type": "application/x-www-form-urlencoded" },
      body: new URLSearchParams({
        grant_type: "refresh_token",
        refresh_token: refreshToken
      }).toString()
    });
    const body = await response.text();
    if (!response.ok) {
      throw new Error(`Zoom token refresh failed: ${oauthErrorMessage(body, response.statusText)}`);
    }
    await this.saveTokenResponse(body);
  }

  private async saveTokenResponse(body: string): Promise<void> {
    const parsed = parseTokenResponse(body);
    if (!parsed.refreshToken) {
      const existing = await this.tokenStore.load();
      if (existing?.refreshToken) {
        parsed.refreshToken = existing.refreshToken;
      }
    }
    await this.tokenStore.save(parsed);
  }

  private async fetchZak(accessToken: string): Promise<string> {
    const response = await this.fetchImpl("https://api.zoom.us/v2/users/me/token?type=zak", {
      headers: {
        authorization: `Bearer ${accessToken}`,
        accept: "application/json"
      }
    });
    const body = await response.text();
    if (!response.ok) {
      throw new Error(`Could not fetch Zoom ZAK: ${oauthErrorMessage(body, response.statusText)}`);
    }
    const parsed = JSON.parse(body) as { token?: string };
    if (!parsed.token) {
      throw new Error("Zoom did not return a ZAK token.");
    }
    return parsed.token;
  }

  private async fetchSdkJwt(accessToken: string): Promise<string> {
    const response = await this.fetchImpl(brokerEndpoint(brokerBaseUrl(ZOOM_OAUTH_BROKER_START_URL), "/oauth/sdk-jwt"), {
      method: "POST",
      headers: { "content-type": "application/json", accept: "application/json" },
      body: JSON.stringify({ access_token: accessToken })
    });
    const body = await response.text();
    if (!response.ok) {
      throw new Error(`Meeting SDK auth broker failed: ${oauthErrorMessage(body, response.statusText)}`);
    }
    const parsed = JSON.parse(body) as { sdk_jwt?: string };
    if (!parsed.sdk_jwt) {
      throw new Error("Meeting SDK auth broker did not return an SDK JWT.");
    }
    return parsed.sdk_jwt;
  }
}