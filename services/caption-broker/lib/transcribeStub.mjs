/**
 * @typedef {import("./session.mjs").CaptionBrokerCue} CaptionBrokerCue
 */

/**
 * Deterministic low-latency stub transcription for CI and staging.
 * @param {{
 *   chunkCount: number;
 *   atMs: number;
 *   speakerId?: string;
 *   speakerName?: string;
 *   audioLevel?: number;
 * }} input
 * @returns {CaptionBrokerCue[]}
 */
export function transcribeChunk(input) {
  const speaker = input.speakerName?.trim() || input.speakerId?.trim() || "Program audio";
  const level = typeof input.audioLevel === "number" ? Math.round(input.audioLevel) : 0;
  const phrase = level > 0 ? `Audio level ${level}` : "Listening";
  const text = `${speaker}: ${phrase} — caption chunk ${input.chunkCount}.`;
  const isFinal = input.chunkCount % 2 === 0;

  return [
    {
      id: `cue-${input.chunkCount}`,
      text,
      atMs: input.atMs,
      speaker,
      confidence: 0.9,
      isFinal
    }
  ];
}