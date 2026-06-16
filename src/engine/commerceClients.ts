import { StubCaptionBrokerClient, type CaptionBrokerClient } from "./captionBrokerClient";
import { StubLicenseClient, type LicenseClient } from "./licenseClient";
import { getNativeHostBridge } from "./nativeHostBridge";
import { isCaptionBridge, RemoteCaptionBrokerClient } from "./remoteCaptionBrokerClient";
import { isLicenseBridge, RemoteLicenseClient } from "./remoteLicenseClient";

export function createLicenseClient(): LicenseClient {
  const bridge = getNativeHostBridge();
  if (isLicenseBridge(bridge)) {
    return new RemoteLicenseClient(bridge);
  }
  return new StubLicenseClient();
}

export function createCaptionBrokerClient(): CaptionBrokerClient {
  const bridge = getNativeHostBridge();
  if (isCaptionBridge(bridge)) {
    return new RemoteCaptionBrokerClient(bridge);
  }
  return new StubCaptionBrokerClient();
}