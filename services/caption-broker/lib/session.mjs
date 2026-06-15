const sessions = new Map();

/**
 * @typedef {object} CaptionBrokerCue
 * @property {string} id
 * @property {string} text
 * @property {number} atMs
 * @property {string | undefined} speaker
 * @property {number | undefined} confidence
 * @property {boolean | undefined} isFinal
 */

/**
 * @typedef {object} CaptionSession
 * @property {string} brokerSessionId
 * @property {string} productionSessionId
 * @property {string | undefined} language
 * @property {number} chunkCount
 * @property {string | undefined} lastSpeakerId
 * @property {string | undefined} lastSpeakerName
 */

/**
 * @param {{ productionSessionId?: string; language?: string }} body
 */
export function startSession(body) {
  const productionSessionId =
    typeof body.productionSessionId === "string" && body.productionSessionId.trim()
      ? body.productionSessionId.trim()
      : `session-${crypto.randomUUID()}`;
  const brokerSessionId = `cb-${crypto.randomUUID()}`;
  /** @type {CaptionSession} */
  const session = {
    brokerSessionId,
    productionSessionId,
    language: typeof body.language === "string" ? body.language : "en-US",
    chunkCount: 0,
    lastSpeakerId: undefined,
    lastSpeakerName: undefined
  };
  sessions.set(brokerSessionId, session);
  return session;
}

/**
 * @param {string} brokerSessionId
 */
export function getSession(brokerSessionId) {
  return sessions.get(brokerSessionId);
}

/**
 * @param {string} brokerSessionId
 */
export function endSession(brokerSessionId) {
  sessions.delete(brokerSessionId);
  return { ended: true, brokerSessionId };
}

/** Test hook */
export function resetCaptionSessions() {
  sessions.clear();
}

export { sessions };