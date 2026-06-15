import { describe, expect, it } from "vitest";
import { brokerEndpoint, isBrokerStartUrl, zoomOAuthBrokerConfigured } from "./zoomOAuth";

describe("zoomOAuth config", () => {
  it("ships the CoreVideo broker start URL", () => {
    expect(zoomOAuthBrokerConfigured()).toBe(true);
    expect(isBrokerStartUrl("https://corevideo.iamfatness.us/oauth/start")).toBe(true);
  });

  it("builds broker API endpoints", () => {
    expect(brokerEndpoint("https://corevideo.iamfatness.us/oauth/start", "/oauth/redeem")).toBe(
      "https://corevideo.iamfatness.us/oauth/redeem"
    );
  });
});