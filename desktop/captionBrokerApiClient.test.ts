import { describe, expect, it, vi } from "vitest";
import { createCaptionBrokerClient, resolveCaptionBrokerConfig } from "./captionBrokerApiClient.ts";

describe("resolveCaptionBrokerConfig", () => {
  it("reads endpoint from env", () => {
    const config = resolveCaptionBrokerConfig({
      COREVIDEO_CAPTION_BROKER_ENDPOINT: "https://captions.example.com",
      COREVIDEO_CAPTION_BROKER_API_KEY: "secret"
    });
    expect(config.endpoint).toBe("https://captions.example.com");
    expect(config.apiKey).toBe("secret");
  });
});

describe("createCaptionBrokerClient", () => {
  it("throws in stub mode when endpoint is missing", async () => {
    const client = createCaptionBrokerClient({});
    await expect(client.startSession("show-1")).rejects.toThrow(/stub mode/i);
  });

  it("streams attributed caption cues from chunks", async () => {
    const fetchMock = vi.fn(async (url: string) => {
      if (url.endsWith("/v1/sessions")) {
        return new Response(JSON.stringify({ brokerSessionId: "cb-1", productionSessionId: "show-1" }), { status: 200 });
      }
      if (url.endsWith("/chunk")) {
        return new Response(
          JSON.stringify({
            latencyMs: 180,
            cues: [{ id: "cue-1", text: "Maya Chen: Audio level 42 — caption chunk 1.", speaker: "Maya Chen", atMs: 1200, confidence: 0.9 }]
          }),
          { status: 200 }
        );
      }
      if (url.endsWith("/end")) {
        return new Response(JSON.stringify({ ended: true }), { status: 200 });
      }
      return new Response("not found", { status: 404 });
    });

    const client = createCaptionBrokerClient({ endpoint: "https://captions.example.com" }, fetchMock);
    const session = await client.startSession("show-1", "en-US");
    expect(session.brokerSessionId).toBe("cb-1");

    const chunk = await client.pushChunk(session.brokerSessionId, {
      atMs: 1200,
      speakerId: "p1",
      speakerName: "Maya Chen",
      audioLevel: 42
    });
    expect(chunk.cues[0]?.speaker).toBe("Maya Chen");
    expect(chunk.latencyMs).toBeLessThanOrEqual(250);

    const ended = await client.endSession(session.brokerSessionId);
    expect(ended.ended).toBe(true);
  });
});