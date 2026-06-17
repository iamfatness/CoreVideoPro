import { describe, expect, it, vi } from "vitest";
import { ZoomOAuthService } from "./zoomOAuthService.ts";
import type { ZoomOAuthTokens, ZoomTokenStore } from "./zoomTokenStore.ts";

function memoryStore(initial?: ZoomOAuthTokens): ZoomTokenStore {
  let tokens = initial;
  return {
    async load() {
      return tokens;
    },
    async save(next) {
      tokens = next;
    },
    async clear() {
      tokens = undefined;
    }
  };
}

describe("ZoomOAuthService", () => {
  it("reports signed-out status when no tokens are stored", async () => {
    const service = new ZoomOAuthService({ tokenStore: memoryStore(), openUrl: () => undefined });
    const status = await service.getStatus();
    expect(status.brokerConfigured).toBe(true);
    expect(status.signedIn).toBe(false);
  });

  it("redeems broker tokens from the OAuth callback", async () => {
    const fetch = vi.fn(async (url: string, init?: RequestInit) => {
      if (url.endsWith("/oauth/redeem")) {
        return new Response(
          JSON.stringify({
            access_token: "access-1",
            refresh_token: "refresh-1",
            expires_in: 3600
          }),
          { status: 200 }
        );
      }
      throw new Error(`unexpected fetch ${url} ${init?.method ?? "GET"}`);
    });
    let capturedState = "";
    const openUrl = vi.fn((url: string) => {
      capturedState = new URL(url).searchParams.get("state") ?? "";
    });
    const store = memoryStore();
    const service = new ZoomOAuthService({ tokenStore: store, fetch, openUrl, nowSeconds: () => 1_000 });
    await service.beginAuthorization();
    await service.handleRedirectUrl(`corevideo://oauth/callback?state=${capturedState}&broker_token=broker-1`);

    const saved = await store.load();
    expect(saved?.accessToken).toBe("access-1");
    expect(saved?.refreshToken).toBe("refresh-1");
    expect((await service.getStatus()).signedIn).toBe(true);
  });

  it("threads broker SDK JWT and ZAK into join credentials", async () => {
    const fetch = vi.fn(async (url: string) => {
      if (url.endsWith("/oauth/sdk-jwt")) {
        return new Response(JSON.stringify({ sdk_jwt: "sdk-jwt-1" }), { status: 200 });
      }
      if (url.includes("/users/me/token")) {
        return new Response(JSON.stringify({ token: "zak-1" }), { status: 200 });
      }
      throw new Error(`unexpected fetch ${url}`);
    });
    const service = new ZoomOAuthService({
      tokenStore: memoryStore({
        accessToken: "access-1",
        refreshToken: "refresh-1",
        expiresAt: 9_999
      }),
      fetch,
      nowSeconds: () => 1_000
    });

    const creds = await service.ensureJoinCredentials();
    expect(creds.sdkJwt).toBe("sdk-jwt-1");
    expect(creds.userZak).toBe("zak-1");
    expect(creds.usePublicAppKey).toBe(false);
  });
});