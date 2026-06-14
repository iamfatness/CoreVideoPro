import type { NativeHostBridge } from "./nativeHostBridge";
import type { MediaCoreHealth, NativeMediaCoreCapability, NativeMediaCoreProfile } from "./nativeMediaCoreProtocol";
import { validateNativeMediaCoreProfile } from "./nativeMediaCoreProtocol";

export type RuntimeEnvironmentStatus = "mock" | "ready" | "degraded" | "unsupported";

export type RuntimeEnvironment = {
  status: RuntimeEnvironmentStatus;
  label: string;
  host: string;
  platform: string;
  warnings: string[];
  /** Capabilities announced by the native core. Empty in mock/degraded/pending states. */
  capabilities: NativeMediaCoreCapability[];
};

export function describeRuntimeEnvironment(
  bridge: NativeHostBridge | undefined,
  profile?: NativeMediaCoreProfile,
  health?: MediaCoreHealth
): RuntimeEnvironment {
  if (!bridge) {
    return {
      status: "mock",
      label: "Mock studio",
      host: "browser-preview",
      platform: "web",
      warnings: ["Running with simulated Zoom and output engines."],
      capabilities: []
    };
  }

  if (health?.recovering) {
    const restarts = health.restartCount;
    return {
      status: "degraded",
      label: `Media core recovering (${restarts} restart${restarts === 1 ? "" : "s"})`,
      host: bridge.host,
      platform: bridge.platform,
      warnings: ["The media core child process crashed and is restarting automatically."],
      capabilities: profile?.capabilities ?? []
    };
  }

  if (!profile) {
    return {
      status: "degraded",
      label: "Native shell pending",
      host: bridge.host,
      platform: bridge.platform,
      warnings: ["Native shell is present, but media core capabilities have not been announced."],
      capabilities: []
    };
  }

  const validation = validateNativeMediaCoreProfile(profile);

  return {
    status: validation.ready ? "ready" : "unsupported",
    label: validation.ready ? "Native media ready" : "Native media incomplete",
    host: bridge.host,
    platform: bridge.platform,
    warnings: [...validation.missingCapabilities.map((capability) => `Missing ${capability}.`), ...validation.warnings],
    capabilities: profile.capabilities
  };
}
