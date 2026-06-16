import { describe, expect, it } from "vitest";
import {
  StubCaptionBrokerClient,
  estimateConfidence,
  type CaptionCue
} from "./captionBrokerClient";

describe("estimateConfidence", () => {
  it("scores within the 70–97 range and rewards clean short lines", () => {
    expect(estimateConfidence("Welcome to the show.")).toBeGreaterThanOrEqual(90);
    const veryLong = "a".repeat(800);
    expect(estimateConfidence(veryLong)).toBe(70); // clamped at the floor
    expect(estimateConfidence("hello")).toBeLessThanOrEqual(97);
  });
});

describe("StubCaptionBrokerClient", () => {
  it("starts idle and only streams after start()", async () => {
    const broker = new StubCaptionBrokerClient();
    expect(broker.getStatus()).toBe("idle");

    const cues: CaptionCue[] = [];
    broker.onCue((cue) => cues.push(cue));

    // Observing before start emits nothing.
    broker.observe({ atMs: 0, text: "ignored", speakerName: "Ada" });
    expect(cues).toHaveLength(0);

    await broker.start();
    expect(broker.getStatus()).toBe("streaming");
  });

  it("emits an attributed cue per distinct observation while streaming", async () => {
    const broker = new StubCaptionBrokerClient();
    const cues: CaptionCue[] = [];
    broker.onCue((cue) => cues.push(cue));
    await broker.start({ speakerAttribution: true });

    broker.observe({ atMs: 1000, text: "Hello everyone.", speakerName: "Ada" });
    broker.observe({ atMs: 1000, text: "Hello everyone.", speakerName: "Ada" }); // duplicate ignored
    broker.observe({ atMs: 2000, text: "Let's get started.", speakerName: "Grace" });

    expect(cues).toHaveLength(2);
    expect(cues[0]).toMatchObject({ text: "Hello everyone.", speakerName: "Ada", isFinal: true });
    expect(cues[1].speakerName).toBe("Grace");
    expect(cues[0].id).not.toEqual(cues[1].id);
    expect(cues[0].confidencePct).toBeGreaterThanOrEqual(70);
  });

  it("omits attribution when disabled", async () => {
    const broker = new StubCaptionBrokerClient();
    const cues: CaptionCue[] = [];
    broker.onCue((cue) => cues.push(cue));
    await broker.start({ speakerAttribution: false });

    broker.observe({ atMs: 500, text: "No name here.", speakerName: "Ada" });
    expect(cues[0].speakerName).toBeUndefined();
  });

  it("stops streaming and unsubscribes", async () => {
    const broker = new StubCaptionBrokerClient();
    const cues: CaptionCue[] = [];
    const unsubscribe = broker.onCue((cue) => cues.push(cue));
    await broker.start();

    broker.observe({ atMs: 1, text: "first" });
    unsubscribe();
    broker.observe({ atMs: 2, text: "second" });
    expect(cues).toHaveLength(1);

    await broker.stop();
    expect(broker.getStatus()).toBe("idle");
    broker.onCue((cue) => cues.push(cue));
    broker.observe({ atMs: 3, text: "after stop" });
    expect(cues).toHaveLength(1);
  });
});
