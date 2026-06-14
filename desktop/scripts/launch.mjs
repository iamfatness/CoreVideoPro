// Launches the CoreVideo Pro desktop shell. Builds the Vite renderer (unless a
// dev server URL is provided via COREVIDEO_RENDERER_URL), then starts Electron
// against desktop/main.ts. Electron is an optional, on-demand dependency so the
// stubs-only CI gate (npm run typecheck && npm run test) never has to fetch it.
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const desktopDir = dirname(dirname(fileURLToPath(import.meta.url)));
const repoRoot = dirname(desktopDir);

function resolveElectron() {
  const candidates = [
    join(repoRoot, "node_modules", ".bin", "electron"),
    join(desktopDir, "node_modules", ".bin", "electron")
  ];
  return candidates.find((candidate) => existsSync(candidate));
}

const electronBin = resolveElectron();
if (!electronBin) {
  console.error(
    [
      "Electron is not installed (kept out of the default install so CI stays light).",
      "Install it once to run the desktop shell:",
      "",
      "  npm install --no-save electron@latest electron-builder@latest",
      "",
      "Then re-run: npm run desktop",
      "",
      "The headless integration gate runs without Electron:  npm run test -- desktop/integration.test.ts"
    ].join("\n")
  );
  process.exit(1);
}

const useDevServer = Boolean(process.env.COREVIDEO_RENDERER_URL);
if (!useDevServer) {
  console.info("[desktop] building renderer (vite build)…");
  const build = spawnSync("npm", ["run", "build"], { cwd: repoRoot, stdio: "inherit" });
  if (build.status !== 0) {
    process.exit(build.status ?? 1);
  }
}

console.info("[desktop] starting Electron…");
const electron = spawnSync(electronBin, [join(desktopDir, "main.ts")], { cwd: repoRoot, stdio: "inherit" });
process.exit(electron.status ?? 0);
