import type { CaptionOverlayState, CaptionTranscriptEntry, Participant } from "../domain/production";

export const MAX_TRANSCRIPT_ENTRIES = 6;

/**
 * Attribute a caption line to a speaker. The overlay already resolves the
 * active speaker name; we look the participant up to recover their production
 * role so the transcript reads like a labelled interview log.
 */
export function attributeCaption(
  overlay: CaptionOverlayState,
  participants: Participant[],
  atSeconds: number
): CaptionTranscriptEntry {
  const speaker = participants.find((participant) => participant.name === overlay.speakerName);

  return {
    id: `cc-${atSeconds}-${slug(overlay.speakerName)}`,
    speakerName: overlay.speakerName,
    role: speaker?.role ?? "Speaker",
    text: overlay.text,
    confidence: overlay.confidence,
    atSeconds
  };
}

/**
 * Append an attributed caption to the rolling transcript. Consecutive lines
 * from the same speaker with identical text are treated as a repeat and
 * skipped, and the transcript is capped at the most recent entries.
 */
export function appendCaptionEntry(
  transcript: CaptionTranscriptEntry[],
  entry: CaptionTranscriptEntry,
  maxEntries: number = MAX_TRANSCRIPT_ENTRIES
): CaptionTranscriptEntry[] {
  if (entry.text.trim().length === 0) {
    return transcript;
  }

  const last = transcript.at(-1);
  if (last && last.speakerName === entry.speakerName && last.text === entry.text) {
    return transcript;
  }

  return [...transcript, entry].slice(-maxEntries);
}

export function summarizeSpeakers(transcript: CaptionTranscriptEntry[]): Array<{ speakerName: string; lineCount: number }> {
  const counts = new Map<string, number>();

  transcript.forEach((entry) => {
    counts.set(entry.speakerName, (counts.get(entry.speakerName) ?? 0) + 1);
  });

  return [...counts.entries()]
    .map(([speakerName, lineCount]) => ({ speakerName, lineCount }))
    .sort((left, right) => right.lineCount - left.lineCount);
}

function slug(value: string): string {
  return value.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/(^-|-$)/g, "") || "speaker";
}
