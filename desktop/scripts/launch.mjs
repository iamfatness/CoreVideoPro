// Launches the CoreVideo Pro desktop shell. Builds the Vite renderer (unless a
// dev server URL is provided via COREVIDEO_RENDERER_URL), then starts Electron
// against desktop/main.ts. Electron is an optional, on-demand dependency so the
// stubs-only CI gate (npm run typecheck && npm run test) never has to fetch it.
import { spawn, spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import {
  electronMainArgs,
  ensurePreloadBundle,
  repoRoot,
  resolveElectron
} from "./desktopRuntime.mjs";

const desktopDir = dirname(dirname(fileURLToPath(import.meta.url)));

const electronBin = resolveElectron();
if (!electronBin) {
  console.error(
    [
      "Electron is not installed (kept out of the default install so CI stays light).",
      "Install it once to run the desktop shell:",
      "",
      "  npm install --no-save electron@latest tsx",
      "",
      "Launch the desktop app (no dev server):  npm run launch",
      "Developer hot-reload only:              npm run dev:desktop",
      "",
      "The headless integration gate runs without Electron:  npm run test -- desktop/integration.test.ts"
    ].join("\n")
  );
  process.exit(1);
}

const useDevServer = Boolean(process.env.COREVIDEO_RENDERER_URL);
if (!useDevServer) {
  console.info("[desktop] building renderer (vite build)…");
  const build = spawnSync("npm", ["run", "build"], { cwd: repoRoot, stdio: "inherit", shell: process.platform === "win32" });
  if (build.status !== 0) {
    process.exit(build.status ?? 1);
  }
}

process.env.ELECTRON_ENABLE_LOGGING = "1";

ensurePreloadBundle();
console.info("[desktop] starting Electron…");

const electronArgs = electronMainArgs();
const useShell = process.platform === "win32" && electronBin.endsWith(".cmd");
const child = spawn(electronBin, electronArgs, {
  cwd: repoRoot,
  stdio: "inherit",
  env: { ...process.env, ELECTRON_ENABLE_LOGGING: "1" },
  shell: useShell
});

for (const sig of ["SIGINT", "SIGTERM", "SIGHUP"]) {
  process.on(sig, () => {
    child.kill(sig);
  });
}

child.on("close", (code) => {
  process.exit(code ?? 0);
});