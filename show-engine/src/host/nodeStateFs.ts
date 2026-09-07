/**
 * `StateFs` (see `../persistence.js`) as thin wrappers over `node:fs/promises`.
 * No atomic-temp-file logic lives here — `StateStore` already writes to a
 * `.tmp` path and renames over the target itself; `writeFile` here is a
 * plain UTF-8 write. `mkdir` is recursive and swallows `EEXIST` so a
 * pre-existing directory is not an error.
 */

import { mkdir as fsMkdir, readFile as fsReadFile, rename as fsRename, writeFile as fsWriteFile } from "node:fs/promises";
import type { StateFs } from "../persistence.js";

export function nodeStateFs(): StateFs {
  return {
    async readFile(path: string): Promise<string> {
      return fsReadFile(path, "utf8");
    },
    async writeFile(path: string, content: string): Promise<void> {
      await fsWriteFile(path, content, "utf8");
    },
    async rename(from: string, to: string): Promise<void> {
      await fsRename(from, to);
    },
    async mkdir(path: string): Promise<void> {
      try {
        await fsMkdir(path, { recursive: true });
      } catch (err) {
        const code = (err as NodeJS.ErrnoException).code;
        if (code !== "EEXIST") throw err;
      }
    }
  };
}
