import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import { applyShowPreset, createShowPreset, InMemoryPresetEngine } from "./presets";

describe("show presets", () => {
  it("serializes repeatable show setup without runtime output state", () => {
    const preset = createShowPreset({
      ...initialProduction,
      streaming: true,
      outputDestinations: initialProduction.outputDestinations.map((destination) =>
        destination.id === "ndi-program" ? { ...destination, enabled: true } : destination
      )
    });

    expect(preset.id).toBe("ai-product-launch-webinar");
    expect(preset.scenes).toHaveLength(initialProduction.scenes.length);
    expect(preset.selectedOutputProfileId).toBe("1080p60");
    expect(preset.outputProfiles).toHaveLength(initialProduction.outputProfiles.length);
    expect(preset.recordingSettings).toEqual(initialProduction.recordingSettings);
    expect(preset.graphics.find((graphic) => graphic.id === "brand-bug")?.enabled).toBe(true);
    expect(preset.videoEffects).toHaveLength(initialProduction.participants.length);
    expect(preset.outputDestinations.filter((destination) => destination.enabled)).toHaveLength(3);
  });

  it("saves, lists, and loads presets from the engine", async () => {
    const engine = new InMemoryPresetEngine();
    const saved = await engine.savePreset(initialProduction);
    const summaries = await engine.listPresets();
    const loaded = await engine.loadPreset(saved.id);

    expect(summaries).toEqual([
      expect.objectContaining({
        id: saved.id,
        sceneCount: initialProduction.scenes.length,
        armedDestinationCount: 2,
        enabledGraphicCount: 1
      })
    ]);
    expect(loaded?.meetingTitle).toBe(initialProduction.meetingTitle);
  });

  it("restores show setup and stops active streaming state", () => {
    const preset = createShowPreset({
      ...initialProduction,
      mode: "manual",
      activeSceneId: "panel",
      previewSceneId: "panel"
    });

    const restored = applyShowPreset(
      {
        ...initialProduction,
        streaming: true,
        outputSession: {
          ...initialProduction.outputSession,
          streaming: true,
          activeDestinationIds: ["custom-rtmp"]
        }
      },
      preset
    );

    expect(restored.mode).toBe("manual");
    expect(restored.activeSceneId).toBe("panel");
    expect(restored.selectedOutputProfileId).toBe("1080p60");
    expect(restored.recordingSettings).toEqual(initialProduction.recordingSettings);
    expect(restored.videoEffects).toHaveLength(initialProduction.videoEffects.length);
    expect(restored.streaming).toBe(false);
    expect(restored.outputSession.activeDestinationIds).toEqual([]);
    expect(restored.transition.statusText).toContain("loaded");
  });
});
