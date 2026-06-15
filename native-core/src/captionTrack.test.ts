import { describe, expect, it } from "vitest";
import { CaptionTrackModel } from "./captionTrack.js";

describe("CaptionTrackModel", () => {
  it("stays idle until a cue is pushed", () => {
    const model = new CaptionTrackModel();

    expect(model.snapshot()).toMatchObject({
      enabled: true,
      status: "idle",
      latencyMs: 180
    });
  });

  it("publishes caption cues with confidence and speaker attribution", () => {
    const model = new CaptionTrackModel();
    const track = model.pushCue("Welcome to the webinar.", 1200, "Maya Chen");

    expect(track.status).toBe("live");
    expect(track.currentCue).toMatchObject({
      text: "Welcome to the webinar.",
      speaker: "Maya Chen",
      atMs: 1200,
      confidence: expect.any(Number)
    });
  });

  it("reports disabled state and ignores empty cues", () => {
    const model = new CaptionTrackModel();
    model.pushCue("   ", 0);

    expect(model.setEnabled(false)).toMatchObject({
      enabled: false,
      warnings: ["Caption track disabled."]
    });
  });
});