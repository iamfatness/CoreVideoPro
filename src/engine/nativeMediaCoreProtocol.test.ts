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
        type: "start-program-output",
        destinations: ["recording", "rtmp", "ndi"],
        isoParticipantIds: ["p1", "p2"]
      }
    ];

    expect(commands.map((command) => command.type)).toEqual([
      "load-scene-graph",
      "set-participant-transform",
      "set-overlay-asset",
      "start-program-output"
    ]);
  });
});
