import { app, safeStorage, shell } from "electron";
import { ZoomOAuthService } from "./zoomOAuthService.ts";
import { createFileZoomTokenStore, defaultZoomTokenStorePath } from "./zoomTokenStore.ts";

export function createDesktopZoomOAuthService(): ZoomOAuthService {
  const filePath = defaultZoomTokenStorePath(app.getPath("userData"));
  const tokenStore = createFileZoomTokenStore({
    filePath,
    encrypt: (value) => (safeStorage.isEncryptionAvailable() ? safeStorage.encryptString(value).toString("base64") : value),
    decrypt: (value) => {
      if (!safeStorage.isEncryptionAvailable()) {
        return value;
      }
      try {
        return safeStorage.decryptString(Buffer.from(value, "base64"));
      } catch {
        return value;
      }
    }
  });

  return new ZoomOAuthService({
    tokenStore,
    openUrl: (url) => shell.openExternal(url)
  });
}