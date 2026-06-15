import { Buffer } from "node:buffer";
import { app, safeStorage, shell } from "electron";
import { ZoomOAuthService } from "./zoomOAuthService.ts";
import { createFileZoomTokenStore, defaultZoomTokenStorePath } from "./zoomTokenStore.ts";

export function createDesktopZoomOAuthService(): ZoomOAuthService {
  const filePath = defaultZoomTokenStorePath(app.getPath("userData"));
  const tokenStore = createFileZoomTokenStore({
    filePath,
    encrypt: (value) => (safeStorage.isEncryptionAvailable() ? Buffer.from(safeStorage.encryptString(value)).toString("base64") : value),
    decrypt: (value) => {
      if (!safeStorage.isEncryptionAvailable()) {
        return value;
      }
      try {
        return safeStorage.decryptString(Buffer.from(value, "base64") as unknown as Uint8Array);
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
