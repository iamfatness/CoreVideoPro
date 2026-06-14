import { describe, expect, it } from "vitest";
import {
  ZOOM_MEETING_SDK_PUBLIC_APP_KEY,
  createEmbeddedZoomSdkReadinessInput,
  zoomMeetingSdkAppKeyPresent,
  zoomMeetingSdkChildProcessEnv
} from "./zoomMeetingSdk";

describe("zoomMeetingSdk", () => {
  it("ships an embedded public app key", () => {
    expect(ZOOM_MEETING_SDK_PUBLIC_APP_KEY.length).toBeGreaterThan(0);
    expect(zoomMeetingSdkAppKeyPresent()).toBe(true);
  });

  it("injects the embedded key into child-process env when unset", () => {
    const env = zoomMeetingSdkChildProcessEnv({});
    expect(env.COREVIDEO_ZOOM_PUBLIC_APP_KEY).toBe(ZOOM_MEETING_SDK_PUBLIC_APP_KEY);
  });

  it("does not override an explicit env override", () => {
    const env = zoomMeetingSdkChildProcessEnv({ COREVIDEO_ZOOM_PUBLIC_APP_KEY: "override-key" });
    expect(env.COREVIDEO_ZOOM_PUBLIC_APP_KEY).toBe("override-key");
  });

  it("marks app-key readiness from the embedded config", () => {
    expect(createEmbeddedZoomSdkReadinessInput().appKeyPresent).toBe(true);
  });
});