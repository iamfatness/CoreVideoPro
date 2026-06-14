import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import { buildMediaFrames, getFrameForParticipant } from "./mediaFrames";
import { applyVideoEffectToFrame, getVideoEffect, toggleChromaKey, toggleCropMode } from "./videoEffects";

describe("video effects", () => {
  it("toggles chroma key per participant", () => {
    const effects = toggleChromaKey(initialProduction.videoEffects, "p2");
    const effect = getVideoEffect(effects, "p2");

    expect(effect.chromaKeyEnabled).toBe(true);
    expect(effect.chromaKeyColor).toBe("green");
  });

  it("applies manual crop zoom to participant frames", () => {
    const effects = toggleCropMode(initialProduction.videoEffects, "p2");
    const effect = getVideoEffect(effects, "p2");
    const frame = getFrameForParticipant(buildMediaFrames(initialProduction.participants, 0), "p2");
    const adjusted = applyVideoEffectToFrame(frame, effect);

    expect(effect.cropMode).toBe("manual");
    expect(effect.manualZoom).toBeGreaterThan(1);
    expect(adjusted?.crop.width).toBeLessThan(frame?.crop.width ?? 0);
  });
});
