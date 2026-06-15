// CI/local entrypoint for the Playwright–Electron smoke gate (Lane C / C2).
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { dirname } from "node:path";
import {
  e2eMediaCoreEnv,
  ensureE2eMainBundle,
  ensurePreloadBundle,
  repoRoot,
  resolveElectron
} from "../desktop/scripts/desktopRuntime.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const root = dirname(here);

if (!resolveElectron(root)) {
  console.error("Electron is not installed. Run: npm install --no-save electron@latest tsx esbuild");
  console.error("Then: node node_modules/electron/install.js");
  process.exit(1);
}

if (!existsSync(join(root, "node_modules", "tsx", "dist", "loader.mjs"))) {
  console.error("tsx is not installed. Run: npm install --no-save tsx");
  process.exit(1);
}

ensurePreloadBundle(root);
const mainE2e = ensureE2eMainBundle(root);
const rendererPort = process.env.COREVIDEO_E2E_PORT ?? "5173";
const rendererUrl = process.env.COREVIDEO_RENDERER_URL ?? `http://127.0.0.1:${rendererPort}`;

const result = spawnSync("npx", ["playwright", "test", "tests/e2e/smoke.test.ts"], {
  cwd: root,
  stdio: "inherit",
  env: {
    ...process.env,
    COREVIDEO_E2E: "1",
    COREVIDEO_E2E_MAIN: mainE2e,
    COREVIDEO_RENDERER_URL: rendererUrl,
    ...e2eMediaCoreEnv(root)
  },
  shell: process.platform === "win32"
});

process.exit(result.status ?? 1);