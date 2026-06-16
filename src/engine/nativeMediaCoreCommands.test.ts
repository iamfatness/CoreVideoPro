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
      type: "set-active-speaker",
      participantId: "p2"
    });
    expect(commands).toContainEqual({
      type: "set-screen-share-source",
      participantId: "p2"
    });
    expect(commands).toContainEqual({
      type: "set-color-grade",
      ...initialProduction.colorGrade
    });
    expect(commands).toEqual(
      expect.arrayContaining([
        expect.objectContaining({
          type: "set-zoom-source-roster",
          sources: expect.arrayContaining([
            expect.objectContaining({
              sourceId: "participant:p2",
              participantId: "p2",
              displayName: "David Chen",
              isActiveSpeaker: true,
              isScreenSharing: true,
              health: "live"
            })
          ])
        })
      ])
    );
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
      type: "set-output-profile",
      profileId: "1080p60",
      resolution: "1920x1080",
      width: 1920,
      height: 1080,
      fps: 60,
      targetBitrateMbps: 8.2
    });
    expect(commands).toContainEqual({
      type: "prepare-encoder-session",
      reason: "Production outputs armed."
    });
    expect(commands).toContainEqual({
      type: "start-program-output",
      destinations: ["recording", "rtmp", "ndi"],
      isoParticipantIds: ["p1", "p2"]
    });
    expect(commands).toContainEqual({
      type: "start-encoder-session"
    });
    expect(commands).toContainEqual({
      type: "set-recording-targets",
      targetFolder: "Recordings/CoreVideo Pro",
      filenamePrefix: "Q2_Product_Update",
      format: "mp4",
      quality: "high",
      isoParticipantIds: ["p1", "p2"]
    });
    expect(commands).toContainEqual({
      type: "start-recording-session",
      sessionId: "Q2_Product_Update-p1-p2",
      targetFolder: "Recordings/CoreVideo Pro",
      filenamePrefix: "Q2_Product_Update",
      format: "mp4",
      quality: "high",
      isoParticipantIds: ["p1", "p2"]
    });
  });

  it("serializes participant audio mix and caption cues for the native core", () => {
    const commands = buildNativeMediaCoreCommands(initialProduction);

    expect(commands).toContainEqual({
      type: "sync-participant-audio-mix",
      channels: initialProduction.participants.map((participant) => ({
        participantId: participant.id,
        inputLevel: participant.audioLevel,
        muted: participant.isMuted,
        noiseSuppression: false,
        manualGainDb: undefined
      }))
    });
    expect(commands).toContainEqual({
      type: "set-brand-kit",
      name: initialProduction.brandKit.name,
      logoText: initialProduction.brandKit.logoText,
      brandColor: initialProduction.brandKit.brandColor,
      accentColor: initialProduction.brandKit.accentColor,
      backgroundColor: initialProduction.brandKit.backgroundColor,
      fontFamily: initialProduction.brandKit.fontFamily,
      lowerThirdStyle: initialProduction.brandKit.lowerThirdStyle
    });
    expect(commands).toContainEqual({ type: "set-caption-enabled", enabled: true });
    expect(commands).toContainEqual({
      type: "push-caption-cue",
      text: initialProduction.captionOverlay.text.trim(),
      speaker: initialProduction.captionOverlay.speakerName,
      atMs: 0
    });
  });

  it("stops encoder lifecycle when production outputs are idle", () => {
    const commands = buildNativeMediaCoreCommands({
      ...initialProduction,
      recording: false,
      streaming: false
    });

    expect(commands).toContainEqual({
      type: "stop-encoder-session",
      reason: "Outputs disabled in production state."
    });
    expect(commands).toContainEqual({
      type: "stop-recording-session",
      reason: "Recording disabled in production state."
    });
  });
});
