import type { RuntimeEnvironment } from "../engine/runtimeEnvironment";
import type { ZoomSdkReadinessInput } from "../engine/zoomSdkReadiness";
import manifest from "./zoomMeetingSdk.json";

/** Zoom Meeting SDK Public Client ID — embedded in the shipped app, not user-provided. */
export const ZOOM_MEETING_SDK_PUBLIC_APP_KEY = manifest.publicAppKey;

export function zoomMeetingSdkAppKeyPresent(): boolean {
  return ZOOM_MEETING_SDK_PUBLIC_APP_KEY.trim().length > 0;
}

/** Env vars injected into the native media-core child process. */
export function zoomMeetingSdkChildProcessEnv(
  base: NodeJS.ProcessEnv = process.env
): NodeJS.ProcessEnv {
  const env = { ...base };
  if (!env.COREVIDEO_ZOOM_PUBLIC_APP_KEY?.trim()) {
    env.COREVIDEO_ZOOM_PUBLIC_APP_KEY = ZOOM_MEETING_SDK_PUBLIC_APP_KEY;
  }
  return env;
}

export function createEmbeddedZoomSdkReadinessInput(
  overrides: Partial<ZoomSdkReadinessInput> = {}
): ZoomSdkReadinessInput {
  return {
    platform: "windows",
    sdkRuntimePresent: false,
    appKeyPresent: zoomMeetingSdkAppKeyPresent(),
    oauthConfigured: false,
    jwtBrokerConfigured: false,
    rawVideoEnabled: false,
    rawAudioEnabled: false,
    rawShareEnabled: false,
    ...overrides
  };
}

/** Vendored zoom-engine path: treat native media ready as SDK-ready for Sprint 1. */
export function deriveZoomSdkReadinessInputForRuntime(
  runtime: RuntimeEnvironment | undefined,
  overrides: Partial<ZoomSdkReadinessInput> = {}
): ZoomSdkReadinessInput {
  const base = createEmbeddedZoomSdkReadinessInput(overrides);
  if (runtime?.status !== "ready") {
    return base;
  }

  return {
    ...base,
    sdkRuntimePresent: true,
    sdkVersion: "zoom-engine",
    oauthConfigured: true,
    jwtBrokerConfigured: true,
    rawVideoEnabled: true,
    rawAudioEnabled: true,
    rawShareEnabled: true,
    packagingPath: runtime.host,
    ...overrides
  };
}