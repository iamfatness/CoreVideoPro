import { endSession, getSession, startSession } from "./session.mjs";
import { transcribeChunk } from "./transcribeStub.mjs";

/**
 * @param {{ productionSessionId?: string; language?: string }} body
 */
export function handleStartSession(body) {
  const session = startSession(body);
  return {
    ok: true,
    brokerSessionId: session.brokerSessionId,
    productionSessionId: session.productionSessionId,
    language: session.language
  };
}

/**
 * @param {string} brokerSessionId
 * @param {{ atMs?: number; speakerId?: string; speakerName?: string; audioLevel?: number; audioBase64?: string }} body
 */
export function handlePushChunk(brokerSessionId, body) {
  const session = getSession(brokerSessionId);
  if (!session) {
    return { ok: false, status: 404, error: "Caption session not found." };
  }

  session.chunkCount += 1;
  const speakerId = typeof body.speakerId === "string" ? body.speakerId.trim() : session.lastSpeakerId;
  const speakerName = typeof body.speakerName === "string" ? body.speakerName.trim() : session.lastSpeakerName;
  if (speakerId) {
    session.lastSpeakerId = speakerId;
  }
  if (speakerName) {
    session.lastSpeakerName = speakerName;
  }

  const atMs = typeof body.atMs === "number" && Number.isFinite(body.atMs) ? body.atMs : Date.now();
  const cues = transcribeChunk({
    chunkCount: session.chunkCount,
    atMs,
    speakerId,
    speakerName,
    audioLevel: typeof body.audioLevel === "number" ? body.audioLevel : undefined
  });

  return {
    ok: true,
    brokerSessionId,
    latencyMs: 180,
    cues
  };
}

/**
 * @param {string} brokerSessionId
 */
export function handleEndSession(brokerSessionId) {
  const session = getSession(brokerSessionId);
  if (!session) {
    return { ok: false, status: 404, error: "Caption session not found." };
  }
  endSession(brokerSessionId);
  return { ok: true, brokerSessionId, ended: true };
}