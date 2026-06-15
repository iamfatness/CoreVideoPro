import type { CaptionBrokerCue, CaptionBrokerSession } from "./captionBrokerTypes.ts";

export type CaptionBrokerConfig = {
  endpoint?: string;
  apiKey?: string;
};

type FetchLike = typeof fetch;

export function resolveCaptionBrokerConfig(env: NodeJS.ProcessEnv): CaptionBrokerConfig {
  return {
    endpoint: env.COREVIDEO_CAPTION_BROKER_ENDPOINT?.trim() || undefined,
    apiKey: env.COREVIDEO_CAPTION_BROKER_API_KEY?.trim() || undefined
  };
}

export function createCaptionBrokerClient(config: CaptionBrokerConfig, fetchImpl: FetchLike = fetch) {
  const endpoint = config.endpoint?.replace(/\/$/, "");

  async function request(path: string, init?: RequestInit) {
    if (!endpoint) {
      throw new Error("Caption broker endpoint not configured (stub mode).");
    }

    const headers: Record<string, string> = {
      "content-type": "application/json",
      ...(init?.headers as Record<string, string> | undefined)
    };
    if (config.apiKey) {
      headers.authorization = `Bearer ${config.apiKey}`;
    }

    const response = await fetchImpl(`${endpoint}${path}`, {
      ...init,
      headers
    });

    let body: unknown;
    try {
      body = await response.json();
    } catch {
      body = undefined;
    }

    if (!response.ok) {
      const message =
        typeof body === "object" && body && "error" in body && typeof body.error === "string"
          ? body.error
          : `Caption broker failed (${response.status}).`;
      throw new Error(message);
    }

    return body;
  }

  return {
    getConfig: () => config,
    async startSession(productionSessionId?: string, language?: string): Promise<CaptionBrokerSession> {
      const result = (await request("/v1/sessions", {
        method: "POST",
        body: JSON.stringify({ productionSessionId, language })
      })) as CaptionBrokerSession;
      return result;
    },
    async pushChunk(
      brokerSessionId: string,
      payload: {
        atMs: number;
        speakerId?: string;
        speakerName?: string;
        audioLevel?: number;
        audioBase64?: string;
      }
    ): Promise<{ latencyMs: number; cues: CaptionBrokerCue[] }> {
      const result = (await request(`/v1/sessions/${brokerSessionId}/chunk`, {
        method: "POST",
        body: JSON.stringify(payload)
      })) as { latencyMs: number; cues: CaptionBrokerCue[] };
      return { latencyMs: result.latencyMs ?? 180, cues: result.cues ?? [] };
    },
    async endSession(brokerSessionId: string): Promise<{ ended: boolean }> {
      const result = (await request(`/v1/sessions/${brokerSessionId}/end`, {
        method: "POST",
        body: JSON.stringify({})
      })) as { ended: boolean };
      return { ended: result.ended === true };
    }
  };
}

export type CaptionBrokerClient = ReturnType<typeof createCaptionBrokerClient>;