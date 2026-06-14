import { describe, expect, it } from "vitest";
import { describeRuntimeEnvironment } from "./runtimeEnvironment";
import type { NativeHostBridge } from "./nativeHostBridge";
import type { NativeMediaCoreProfile } from "./nativeMediaCoreProtocol";

const bridge: NativeHostBridge = {
  host: "native-shell",
  platform: "win32",
  async request(command) {
    return {
      id: command.id,
      ok: false,
      error: {
        code: "native-unavailable",
        message: "Test bridge does not execute commands."
      }
    };
  }
};

const readyProfile: NativeMediaCoreProfile = {
  name: "Direct3D 12 production core",
  renderer: "direct3d12",
  maxProgramResolution: "3840x2160",
  maxProgramFps: 60,
  maxParticipantFeeds: 8,
  maxIsoRecordings: 4,
  capabilities: [
    "zoom-raw-video",
    "zoom-raw-audio",
    "gpu-compositor",
    "scene-graph-rendering",
    "dynamic-overlays",
    "chroma-key",
    "smart-framing",
    "audio-mixer",
    "program-recording",
    "iso-recording",
    "rtmp-output"
  ]
};

describe("runtime environment", () => {
  it("describes browser development as a mock studio", () => {
    expect(describeRuntimeEnvironment(undefined)).toMatchObject({
      status: "mock",
      label: "Mock studio",
      host: "browser-preview",
      platform: "web"
    });
  });

  it("marks a native shell without a media profile as degraded", () => {
    expect(describeRuntimeEnvironment(bridge)).toMatchObject({
      status: "degraded",
      label: "Native shell pending",
      host: "native-shell",
      platform: "win32"
    });
  });

  it("marks a capable native media profile as ready", () => {
    expect(describeRuntimeEnvironment(bridge, readyProfile)).toMatchObject({
      status: "ready",
      label: "Native media ready",
      warnings: []
    });
  });

  it("surfaces missing native media capabilities", () => {
    const runtime = describeRuntimeEnvironment(bridge, {
      ...readyProfile,
      capabilities: readyProfile.capabilities.filter((capability) => capability !== "iso-recording")
    });

    expect(runtime.status).toBe("unsupported");
    expect(runtime.warnings).toContain("Missing iso-recording.");
  });
});
