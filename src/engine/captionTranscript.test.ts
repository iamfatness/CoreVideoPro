import { describe, expect, it } from "vitest";
import { initialProduction, type CaptionOverlayState, type CaptionTranscriptEntry } from "../domain/production";
import { appendCaptionEntry, attributeCaption, summarizeSpeakers } from "./captionTranscript";

const overlay: CaptionOverlayState = {
  ...initialProduction.captionOverlay,
  speakerName: "David Chen",
  text: "Here is the launch timeline.",
  confidence: 91
};

function entry(overrides: Partial<CaptionTranscriptEntry>): CaptionTranscriptEntry {
  return {
    id: "x",
    speakerName: "Sophia Martinez",
    role: "Host",
    text: "Line",
    confidence: 90,
    atSeconds: 0,
    ...overrides
  };
}

describe("attributeCaption", () => {
  it("recovers the speaker's production role from the roster", () => {
    const result = attributeCaption(overlay, initialProduction.participants, 20);
    expect(result.speakerName).toBe("David Chen");
    expect(result.role).toBe("Presenter");
    expect(result.confidence).toBe(91);
    expect(result.atSeconds).toBe(20);
  });

  it("falls back to a generic role for unknown speakers", () => {
    const result = attributeCaption({ ...overlay, speakerName: "Program audio" }, initialProduction.participants, 5);
    expect(result.role).toBe("Speaker");
  });
});

describe("appendCaptionEntry", () => {
  it("appends new lines and caps the transcript length", () => {
    let transcript: CaptionTranscriptEntry[] = [];
    for (let index = 0; index < 8; index += 1) {
      transcript = appendCaptionEntry(transcript, entry({ id: `e${index}`, text: `Line ${index}`, atSeconds: index }), 6);
    }

    expect(transcript).toHaveLength(6);
    expect(transcript[0].text).toBe("Line 2");
    expect(transcript.at(-1)?.text).toBe("Line 7");
  });

  it("skips a consecutive repeat from the same speaker", () => {
    const first = appendCaptionEntry([], entry({ text: "Same" }));
    const second = appendCaptionEntry(first, entry({ text: "Same" }));
    expect(second).toBe(first);
  });

  it("ignores blank caption text", () => {
    const result = appendCaptionEntry([], entry({ text: "   " }));
    expect(result).toHaveLength(0);
  });
});

describe("summarizeSpeakers", () => {
  it("counts lines per speaker, most frequent first", () => {
    const transcript = [
      entry({ speakerName: "Sophia Martinez" }),
      entry({ speakerName: "David Chen" }),
      entry({ speakerName: "David Chen" })
    ];

    expect(summarizeSpeakers(transcript)).toEqual([
      { speakerName: "David Chen", lineCount: 2 },
      { speakerName: "Sophia Martinez", lineCount: 1 }
    ]);
  });
});
