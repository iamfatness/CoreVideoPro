import { describe, expect, it } from "vitest";
import { initialProduction, type ProductionState } from "../domain/production";
import { buildNativeMediaCoreCommands } from "./nativeMediaCoreCommands";

describe("native media core command builder", () => {
  it("serializes the active scene routes for the native scene graph", () => {
    const state: ProductionState = {
      ...initialProduction,
      activeSceneId: "interview",
      scenes: initialProduction.scenes.map((scene) =>
        scene.id === "interview"
          ? {
              ...scene,
              routes: [
                { id: "interview-1", mode: "active-speaker", audioRole: "mix" },
                { id: "interview-2", mode: "fixed", participantId: "p1", audioRole: "isolated" }
              ]
            }
          : scene
      )
    };

    const sceneGraph = buildNativeMediaCoreCommands(state).find((command) => command.type === "load-scene-graph");

    expect(sceneGraph).toEqual({
      type: "load-scene-graph",
      sceneId: "interview",
      routes: [
        { routeId: "interview-1", mode: "active-speaker", participantId: undefined, audioRole: "mix" },
        { routeId: "interview-2", mode: "fixed", participantId: "p1", audioRole: "isolated" }
      ]
    });
  });

  it("derives speaker plus slides routes when a template has no explicit routes", () => {
    const sceneGraph = buildNativeMediaCoreCommands(initialProduction).find((command) => command.type === "load-scene-graph");

    expect(sceneGraph).toMatchObject({
      type: "load-scene-graph",
      sceneId: "speaker-slides",
      routes: [
        { mode: "screen-share", audioRole: "audience" },
        { mode: "fixed", audioRole: "isolated" }
      ]
    });
  });

  it("converts participant video effects into native transform commands", () => {
    const commands = buildNativeMediaCoreCommands({
      ...initialProduction,
      videoEffects: [
        {
          participantId: "p2",
          cropMode: "manual",
          manualZoom: 1.4,
          chromaKeyEnabled: true,
          chromaKeyColor: "green",
          spillSuppression: 48
        }
      ]
    });

    expect(commands).toContainEqual({
      type: "set-participant-transform",
      participantId: "p2",
      crop: { x: 0.1, y: 0.1, width: 0.8, height: 0.8 },
      scale: 1.4,
      chromaKey: { enabled: true, color: "green", spillSuppression: 48 }
    });
  });

  it("sends enabled graphics and live outputs to the media core", () => {
    const commands = buildNativeMediaCoreCommands({
      ...initialProduction,
      recording: true,
      streaming: true,
      graphics: initialProduction.graphics.map((graphic) => ({ ...graphic, enabled: graphic.id !== "question-cta" })),
      outputDestinations: initialProduction.outputDestinations.map((destination) => ({
        ...destination,
        enabled: destination.protocol !== "SRT"
      }))
    });

    expect(commands).toContainEqual({
      type: "set-overlay-asset",
      overlayId: "brand-bug",
      text: "CoreVideo Pro",
      position: "top-right"
    });
    expect(commands).toContainEqual({
      type: "set-overlay-asset",
      overlayId: "live-banner",
      text: "Live webinar",
      position: "bottom-right"
    });
    expect(commands).toContainEqual({
      type: "start-program-output",
      destinations: ["recording", "rtmp", "ndi"],
      isoParticipantIds: ["p1", "p2"]
    });
  });
});
