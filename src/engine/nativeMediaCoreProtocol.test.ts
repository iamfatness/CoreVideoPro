import { describe, expect, it } from "vitest";
import {
  requiredMvpMediaCoreCapabilities,
  validateNativeMediaCoreProfile,
  type NativeMediaCoreCommand,
  type NativeMediaCoreProfile
} from "./nativeMediaCoreProtocol";

describe("native media core protocol", () => {
  const productionProfile: NativeMediaCoreProfile = {
    name: "CoreVideo Native GPU Core",
    renderer: "direct3d12",
    maxProgramResolution: "3840x2160",
    maxProgramFps: 60,
    maxParticipantFeeds: 8,
    maxIsoRecordings: 4,
    capabilities: [
      ...requiredMvpMediaCoreCapabilities,
      "ndi-output",
      "srt-output",
      "webrtc-output"
    ]
  };

  it("accepts a native GPU media core that owns real-time video work", () => {
    const validation = validateNativeMediaCoreProfile(productionProfile);

    expect(validation.ready).toBe(true);
    expect(validation.missingCapabilities).toEqual([]);
    expect(validation.warnings).toEqual([]);
  });

  it("rejects a shell-only or software-rendered profile for production switching", () => {
    const validation = validateNativeMediaCoreProfile({
      ...productionProfile,
      renderer: "software",
      maxParticipantFeeds: 4,
      capabilities: ["dynamic-overlays", "program-recording"]
    });

    expect(validation.ready).toBe(false);
    expect(validation.missingCapabilities).toContain("gpu-compositor");
    expect(validation.missingCapabilities).toContain("zoom-raw-video");
    expect(validation.warnings).toContain("Software rendering is not suitable for production 1080p/4K switching.");
    expect(validation.warnings).toContain("MVP target expects at least 6 clean Zoom participant feeds.");
  });

  it("models direct scene graph, transform, overlay, and output commands outside the UI shell", () => {
    const commands: NativeMediaCoreCommand[] = [
      {
        type: "set-zoom-source-roster",
        sources: [
          {
            sourceId: "participant:p2",
            participantId: "p2",
            displayName: "Andre Wallace",
            role: "Presenter",
            breakoutRoomId: "main",
            breakoutRoomName: "Main room",
            hasVideo: true,
            hasAudio: true,
            isMuted: false,
            isActiveSpeaker: true,
            isScreenSharing: true,
            audioLevel: 82,
            health: "live"
          }
        ]
      },
      { type: "set-active-speaker", participantId: "p2" },
      { type: "set-screen-share-source", participantId: "p2" },
      {
        type: "load-scene-graph",
        sceneId: "speaker-slides",
        routes: [
          { routeId: "speaker", mode: "fixed", participantId: "p2", audioRole: "isolated" },
          { routeId: "slides", mode: "screen-share", audioRole: "audience" }
        ]
      },
      {
        type: "set-participant-transform",
        participantId: "p2",
        crop: { x: 0.1, y: 0.08, width: 0.8, height: 0.84 },
        scale: 1.18,
        chromaKey: { enabled: true, color: "green", spillSuppression: 42 }
      },
      {
        type: "set-overlay-asset",
        overlayId: "lower-third",
        text: "Andre Wallace - Product Lead",
        position: "lower-third"
      },
      {
        type: "set-color-grade",
        lut: "warm-film",
        exposure: 2,
        contrast: 6,
        saturation: 8,
        temperature: 5
      },
      {
        type: "set-output-profile",
        profileId: "1080p60",
        resolution: "1920x1080",
        width: 1920,
        height: 1080,
        fps: 60,
        targetBitrateMbps: 8.2
      },
      {
        type: "start-program-output",
        destinations: ["recording", "rtmp", "ndi"],
        isoParticipantIds: ["p1", "p2"]
      },
      {
        type: "set-recording-targets",
        targetFolder: "Recordings/CoreVideo Pro",
        filenamePrefix: "launch-show",
        format: "mp4",
        quality: "high",
        isoParticipantIds: ["p1", "p2"]
      },
      {
        type: "start-recording-session",
        sessionId: "launch-show-p1-p2",
        isoParticipantIds: ["p1", "p2"]
      }
    ];

    expect(commands.map((command) => command.type)).toEqual([
      "set-zoom-source-roster",
      "set-active-speaker",
      "set-screen-share-source",
      "load-scene-graph",
      "set-participant-transform",
      "set-overlay-asset",
      "set-color-grade",
      "set-output-profile",
      "start-program-output",
      "set-recording-targets",
      "start-recording-session"
    ]);
  });
});
