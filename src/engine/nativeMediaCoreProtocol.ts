export type NativeMediaCoreCapability =
  | "zoom-raw-video"
  | "zoom-raw-audio"
  | "gpu-compositor"
  | "scene-graph-rendering"
  | "dynamic-overlays"
  | "chroma-key"
  | "smart-framing"
  | "audio-mixer"
  | "program-recording"
  | "iso-recording"
  | "rtmp-output"
  | "ndi-output"
  | "srt-output"
  | "webrtc-output";

export type NativeMediaCoreProfile = {
  name: string;
  renderer: "metal" | "direct3d11" | "direct3d12" | "vulkan" | "opengl" | "software";
  maxProgramResolution: "1920x1080" | "3840x2160";
  maxProgramFps: 30 | 60;
  maxParticipantFeeds: number;
  maxIsoRecordings: number;
  capabilities: NativeMediaCoreCapability[];
};

export type NativeMediaCoreCommand =
  | {
      type: "load-scene-graph";
      sceneId: string;
      routes: Array<{
        routeId: string;
        mode: "fixed" | "active-speaker" | "spotlight" | "screen-share" | "none";
        participantId?: string;
        audioRole: "mix" | "isolated" | "audience";
      }>;
    }
  | {
      type: "set-participant-transform";
      participantId: string;
      crop: { x: number; y: number; width: number; height: number };
      scale: number;
      chromaKey?: { enabled: boolean; color: "green" | "blue"; spillSuppression: number };
    }
  | {
      type: "set-overlay-asset";
      overlayId: string;
      text?: string;
      imageUri?: string;
      position: "top-right" | "bottom-right" | "center" | "lower-third";
    }
  | {
      type: "start-program-output";
      destinations: Array<"rtmp" | "ndi" | "srt" | "webrtc" | "recording">;
      isoParticipantIds: string[];
    };

export type NativeMediaCoreValidation = {
  ready: boolean;
  missingCapabilities: NativeMediaCoreCapability[];
  warnings: string[];
};

export const requiredMvpMediaCoreCapabilities: NativeMediaCoreCapability[] = [
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
];

export function validateNativeMediaCoreProfile(profile: NativeMediaCoreProfile): NativeMediaCoreValidation {
  const missingCapabilities = requiredMvpMediaCoreCapabilities.filter((capability) => !profile.capabilities.includes(capability));
  const warnings: string[] = [];

  if (profile.renderer === "software") {
    warnings.push("Software rendering is not suitable for production 1080p/4K switching.");
  }

  if (profile.maxParticipantFeeds < 6) {
    warnings.push("MVP target expects at least 6 clean Zoom participant feeds.");
  }

  if (profile.maxIsoRecordings < 2) {
    warnings.push("MVP target expects program recording plus selected ISO recovery paths.");
  }

  if (profile.maxProgramResolution !== "3840x2160") {
    warnings.push("4K output will be unavailable on this media core profile.");
  }

  return {
    ready: missingCapabilities.length === 0 && profile.renderer !== "software" && profile.maxParticipantFeeds >= 6,
    missingCapabilities,
    warnings
  };
}
