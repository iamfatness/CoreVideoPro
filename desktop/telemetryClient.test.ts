import { describe, expect, it, vi } from "vitest";
import { initialProduction } from "../src/domain/production.ts";
import { createSupportBundle } from "../src/engine/supportBundle.ts";
import { createTelemetryClient, resolveTelemetryConfig } from "./telemetryClient.ts";

describe("resolveTelemetryConfig", () => {
  it("reads endpoint and consent from env", () => {
    const config = resolveTelemetryConfig(
      { COREVIDEO_TELEMETRY_ENDPOINT: "https://telemetry.example.com", COREVIDEO_TELEMETRY_API_KEY: "secret" },
      { analyticsEnabled: true },
      "0.2.0",
      "win32"
    );
    expect(config.endpoint).toBe("https://telemetry.example.com");
    expect(config.apiKey).toBe("secret");
    expect(config.analyticsEnabled).toBe(true);
  });
});

describe("createTelemetryClient", () => {
  it("stubs uploads when no endpoint is configured", async () => {
    const client = createTelemetryClient({
      appVersion: "0.1.0",
      platform: "mock",
      analyticsEnabled: true
    });
    const result = await client.uploadCrashReport({ reason: "test" });
    expect(result.uploaded).toBe(false);
    expect(result.message).toContain("stub mode");
  });

  it("respects telemetry opt-out for feature events", async () => {
    const fetchMock = vi.fn();
    const client = createTelemetryClient(
      {
        endpoint: "https://telemetry.example.com",
        appVersion: "0.1.0",
        platform: "win32",
        analyticsEnabled: false
      },
      fetchMock
    );
    const result = await client.recordEvent({ kind: "feature", name: "tab_opened", detail: "studio" });
    expect(result.uploaded).toBe(false);
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it("uploads redacted crash bundles with bearer auth", async () => {
    const fetchMock = vi.fn(async () => new Response(JSON.stringify({ reportId: "crash-123" }), { status: 202 }));
    const bundle = createSupportBundle(initialProduction);
    const client = createTelemetryClient(
      {
        endpoint: "https://telemetry.example.com",
        apiKey: "secret",
        appVersion: "0.1.0",
        platform: "win32",
        analyticsEnabled: true
      },
      fetchMock
    );

    const result = await client.uploadCrashReport({ reason: "forced", bundle });
    expect(result.uploaded).toBe(true);
    expect(result.reportId).toBe("crash-123");

    const [, init] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(init.headers).toMatchObject({ authorization: "Bearer secret" });
    const body = JSON.parse(String(init.body)) as { bundle: { participants: Array<{ name: string }> } };
    expect(body.bundle.participants.every((participant) => participant.name.startsWith("participant-"))).toBe(true);
  });
});