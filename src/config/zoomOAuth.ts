import manifest from "./zoomOAuth.json";

export const ZOOM_OAUTH_BROKER_START_URL = manifest.brokerStartUrl;
export const ZOOM_OAUTH_REDIRECT_URI = manifest.redirectUri;
export const ZOOM_OAUTH_SCOPES = manifest.scopes;

export function zoomOAuthBrokerConfigured(): boolean {
  return ZOOM_OAUTH_BROKER_START_URL.trim().length > 0;
}

export function isBrokerStartUrl(url: string): boolean {
  try {
    const parsed = new URL(url);
    return parsed.protocol.startsWith("http") && parsed.pathname.replace(/\/+$/, "") === "/oauth/start";
  } catch {
    return false;
  }
}

export function brokerBaseUrl(authorizationUrl: string): string {
  const parsed = new URL(authorizationUrl);
  parsed.pathname = "/";
  parsed.search = "";
  parsed.hash = "";
  return parsed.toString().replace(/\/$/, "");
}

export function brokerEndpoint(baseUrl: string, path: string): string {
  const parsed = new URL(baseUrl.endsWith("/") ? baseUrl : `${baseUrl}/`);
  parsed.pathname = path.startsWith("/") ? path : `/${path}`;
  parsed.search = "";
  parsed.hash = "";
  return parsed.toString();
}