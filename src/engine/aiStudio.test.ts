import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import { buildAiStudioState } from "./aiStudio";

describe("AI Studio engine", () => {
  it("generates transcript, chapters, highlights, and show notes from production state", () => {
    const studio = buildAiStudioState(initialProduction);

    expect(studio.transcript[0]).toMatchObject({
      speakerName: "Andre Wallace",
      text: "The active speaker is framed automatically while the slides remain readable.",
      confidence: 94
    });
    expect(studio.chapters[0]).toMatchObject({
      title: "Speaker + Slides",
      summary: "Presenter-led section with slides, adaptive captions, and lower-third protection."
    });
    expect(studio.highlights[0]).toMatchObject({
      title: "Presenter plus slides moment",
      speakerName: "Andre Wallace",
      suggestedClipName: "ai-product-launch-webinar-andre-wallace-speaker-slides"
    });
    expect(studio.showNotes.title).toBe("AI Product Launch Webinar show notes");
    expect(studio.showNotes.bullets).toContain("Caption confidence: 94%");
    expect(studio.rundown[2]).toContain("Speaker + Slides - live");
  });

  it("adds publishing guidance when recording is off and feed quality is weak", () => {
    const studio = buildAiStudioState(initialProduction);

    expect(studio.showNotes.nextActions).toContain("Start recording to preserve highlight source media.");
    expect(studio.showNotes.nextActions).toContain("Resolve low-quality participant feeds before clipping.");
  });
});
