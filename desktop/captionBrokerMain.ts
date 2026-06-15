import { ipcMain } from "electron";
import { createCaptionBrokerClient, resolveCaptionBrokerConfig } from "./captionBrokerApiClient.ts";
import type { CaptionBrokerCue } from "./captionBrokerTypes.ts";

export type CaptionBrokerService = {
  isConfigured(): boolean;
};

export function createCaptionBrokerService(env: NodeJS.ProcessEnv = process.env): CaptionBrokerService {
  const config = resolveCaptionBrokerConfig(env);
  const client = createCaptionBrokerClient(config);
  let activeSessionId: string | undefined;

  ipcMain.handle("corevideo:caption-broker-start", async (_event, payload?: { productionSessionId?: string; language?: string }) => {
    if (!config.endpoint) {
      return { ok: false, message: "Caption broker not configured (stub mode)." };
    }
    const session = await client.startSession(payload?.productionSessionId, payload?.language);
    activeSessionId = session.brokerSessionId;
    return { ok: true, session };
  });

  ipcMain.handle(
    "corevideo:caption-broker-push-chunk",
    async (
      _event,
      payload: {
        brokerSessionId?: string;
        atMs: number;
        speakerId?: string;
        speakerName?: string;
        audioLevel?: number;
        audioBase64?: string;
      }
    ) => {
      if (!config.endpoint) {
        return { ok: false, message: "Caption broker not configured (stub mode).", cues: [] as CaptionBrokerCue[] };
      }
      const brokerSessionId = payload.brokerSessionId ?? activeSessionId;
      if (!brokerSessionId) {
        return { ok: false, message: "No active caption session.", cues: [] as CaptionBrokerCue[] };
      }
      const result = await client.pushChunk(brokerSessionId, payload);
      return { ok: true, ...result };
    }
  );

  ipcMain.handle("corevideo:caption-broker-end", async (_event, brokerSessionId?: string) => {
    if (!config.endpoint) {
      return { ok: false, message: "Caption broker not configured (stub mode)." };
    }
    const sessionId = brokerSessionId ?? activeSessionId;
    if (!sessionId) {
      return { ok: false, message: "No active caption session." };
    }
    const result = await client.endSession(sessionId);
    if (activeSessionId === sessionId) {
      activeSessionId = undefined;
    }
    return { ok: true, ...result };
  });

  return {
    isConfigured: () => Boolean(config.endpoint)
  };
}