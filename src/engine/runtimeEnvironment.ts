import type { NativeHostBridge } from "./nativeHostBridge";
import type { NativeMediaCoreProfile } from "./nativeMediaCoreProtocol";
import { validateNativeMediaCoreProfile } from "./nativeMediaCoreProtocol";

export type RuntimeEnvironmentStatus = "mock" | "ready" | "degraded" | "unsupported";

export type RuntimeEnvironment = {
  status: RuntimeEnvironmentStatus;
  label: string;
  host: string;
  platform: string;
  warnings: string[];
};

export function describeRuntimeEnvironment(
  bridge: NativeHostBridge | undefined,
  profile?: NativeMediaCoreProfile
): RuntimeEnvironment {
  if (!bridge) {
    return {
      status: "mock",
      label: "Mock studio",
      host: "browser-preview",
      platform: "web",
      warnings: ["Running with simulated Zoom and output engines."]
    };
  }

  if (!profile) {
    return {
      status: "degraded",
      label: "Native shell pending",
      host: bridge.host,
      platform: bridge.platform,
      warnings: ["Native shell is present, but media core capabilities have not been announced."]
    };
  }

  const validation = validateNativeMediaCoreProfile(profile);

  return {
    status: validation.ready ? "ready" : "unsupported",
    label: validation.ready ? "Native media ready" : "Native media incomplete",
    host: bridge.host,
    platform: bridge.platform,
    warnings: [...validation.missingCapabilities.map((capability) => `Missing ${capability}.`), ...validation.warnings]
  };
}
