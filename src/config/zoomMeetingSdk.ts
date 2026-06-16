import type { RuntimeEnvironment } from "../engine/runtimeEnvironment";
import type { ZoomSdkReadinessInput } from "../engine/zoomSdkReadiness";
import { zoomOAuthBrokerConfigured } from "./zoomOAuth";
import manifest from "./zoomMeetingSdk.json";

export type ZoomOAuthReadiness = {
  signedIn?: boolean;
};

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
    /** True when the embedded OAuth PKCE broker URL is present (not a user JWT). */
    jwtBrokerConfigured: false,
    rawVideoEnabled: false,
    rawAudioEnabled: false,
    rawShareEnabled: false,
    ...overrides
  };
}

/** Vendored zoom-engine path: native runtime + embedded app key; OAuth when signed in. */
export function deriveZoomSdkReadinessInputForRuntime(
  runtime: RuntimeEnvironment | undefined,
  overrides: Partial<ZoomSdkReadinessInput> = {},
  oauth: ZoomOAuthReadiness = {}
): ZoomSdkReadinessInput {
  const base = createEmbeddedZoomSdkReadinessInput(overrides);
  if (runtime?.status !== "ready") {
    return base;
  }

  const oauthBrokerConfigured = zoomOAuthBrokerConfigured();
  const oauthConfigured = Boolean(oauth.signedIn) || oauthBrokerConfigured || zoomMeetingSdkAppKeyPresent();

  return {
    ...base,
    sdkRuntimePresent: true,
    sdkVersion: "zoom-engine",
    oauthConfigured,
    jwtBrokerConfigured: oauthBrokerConfigured,
    rawVideoEnabled: true,
    rawAudioEnabled: true,
    rawShareEnabled: true,
    packagingPath: runtime.host,
    ...overrides
  };
}