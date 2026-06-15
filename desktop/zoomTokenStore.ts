import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, join } from "node:path";

export type ZoomOAuthTokens = {
  accessToken: string;
  refreshToken: string;
  expiresAt: number;
};

export type ZoomTokenStore = {
  load(): Promise<ZoomOAuthTokens | undefined>;
  save(tokens: ZoomOAuthTokens): Promise<void>;
  clear(): Promise<void>;
};

export type FileZoomTokenStoreOptions = {
  filePath: string;
  encrypt?(value: string): string;
  decrypt?(value: string): string;
};

export function createFileZoomTokenStore(options: FileZoomTokenStoreOptions): ZoomTokenStore {
  const encrypt = options.encrypt ?? ((value: string) => value);
  const decrypt = options.decrypt ?? ((value: string) => value);

  async function ensureParent(): Promise<void> {
    await mkdir(dirname(options.filePath), { recursive: true });
  }

  return {
    async load() {
      try {
        const raw = await readFile(options.filePath, "utf8");
        const parsed = JSON.parse(raw) as {
          accessToken?: string;
          refreshToken?: string;
          expiresAt?: number;
        };
        if (!parsed.accessToken || !parsed.refreshToken) {
          return undefined;
        }
        return {
          accessToken: decrypt(parsed.accessToken),
          refreshToken: decrypt(parsed.refreshToken),
          expiresAt: parsed.expiresAt ?? 0
        };
      } catch {
        return undefined;
      }
    },
    async save(tokens) {
      await ensureParent();
      await writeFile(
        options.filePath,
        JSON.stringify({
          accessToken: encrypt(tokens.accessToken),
          refreshToken: encrypt(tokens.refreshToken),
          expiresAt: tokens.expiresAt
        }),
        "utf8"
      );
    },
    async clear() {
      try {
        await writeFile(options.filePath, "{}", "utf8");
      } catch {
        // ignore missing file
      }
    }
  };
}

export function defaultZoomTokenStorePath(userDataDir: string): string {
  return join(userDataDir, "zoom-oauth.json");
}