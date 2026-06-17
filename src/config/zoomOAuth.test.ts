import { describe, expect, it } from "vitest";
import {
  brokerEndpoint,
  isBrokerStartUrl,
  ZOOM_OAUTH_BROKER_CALLBACK_URL,
  ZOOM_OAUTH_BROKER_START_URL,
  ZOOM_OAUTH_PUBLIC_CLIENT_ID,
  ZOOM_OAUTH_REDIRECT_URI,
  zoomOAuthBrokerConfigured
} from "./zoomOAuth";

describe("zoomOAuth config", () => {
  it("ships the CoreVideo broker start URL", () => {
    expect(zoomOAuthBrokerConfigured()).toBe(true);
    expect(ZOOM_OAUTH_BROKER_START_URL).toBe("https://corevideo.iamfatness.us/oauth/start");
    expect(ZOOM_OAUTH_BROKER_CALLBACK_URL).toBe("https://corevideo.iamfatness.us/oauth/callback");
    expect(ZOOM_OAUTH_REDIRECT_URI).toBe("corevideo://oauth/callback");
    expect(ZOOM_OAUTH_PUBLIC_CLIENT_ID).toBe("y6sIWSwiTZe1JygMx4C9EQ");
    expect(isBrokerStartUrl("https://corevideo.iamfatness.us/oauth/start")).toBe(true);
  });

  it("builds broker API endpoints", () => {
    expect(brokerEndpoint("https://corevideo.iamfatness.us/oauth/start", "/oauth/redeem")).toBe(
      "https://corevideo.iamfatness.us/oauth/redeem"
    );
  });
});