// Launches the CoreVideo Pro desktop shell. Builds the Vite renderer (unless a
// dev server URL is provided via COREVIDEO_RENDERER_URL), then starts Electron
// against desktop/main.ts. Electron is an optional, on-demand dependency so the
// stubs-only CI gate (npm run typecheck && npm run test) never has to fetch it.
import { spawn, spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { buildSync } from "esbuild";

const desktopDir = dirname(dirname(fileURLToPath(import.meta.url)));
const repoRoot = dirname(desktopDir);

function resolveElectron() {
  // Allow the caller to override the electron binary (e.g. from a Playwright
  // test that already resolved the path via the `electron` npm package).
  if (process.env.ELECTRON_OVERRIDE_BIN && existsSync(process.env.ELECTRON_OVERRIDE_BIN)) {
    return process.env.ELECTRON_OVERRIDE_BIN;
  }
  const roots = [join(repoRoot, "node_modules"), join(desktopDir, "node_modules")];
  for (const root of roots) {
    const binary = join(root, "electron", "dist", process.platform === "win32" ? "electron.exe" : "electron");
    if (existsSync(binary)) {
      return binary;
    }
    const binNames = process.platform === "win32" ? ["electron.cmd", "electron"] : ["electron"];
    for (const binName of binNames) {
      const shim = join(root, ".bin", binName);
      if (existsSync(shim)) {
        return shim;
      }
    }
  }
  return undefined;
}

const electronBin = resolveElectron();
if (!electronBin) {
  console.error(
    [
      "Electron is not installed (kept out of the default install so CI stays light).",
      "Install it once to run the desktop shell:",
      "",
      "  npm install --no-save electron@latest tsx",
      "  npm run dev:desktop",
      "",
      "Then re-run: npm run desktop",
      "",
      "The headless integration gate runs without Electron:  npm run test -- desktop/integration.test.ts"
    ].join("\n")
  );
  process.exit(1);
}

function ensurePreloadBundle() {
  const preloadTs = join(desktopDir, "preload.ts");
  const preloadCjs = join(desktopDir, "preload.cjs");
  if (!existsSync(preloadTs)) {
    return;
  }
  buildSync({
    entryPoints: [preloadTs],
    outfile: preloadCjs,
    bundle: true,
    platform: "node",
    format: "cjs",
    external: ["electron"]
  });
}

const useDevServer = Boolean(process.env.COREVIDEO_RENDERER_URL);
if (!useDevServer) {
  console.info("[desktop] building renderer (vite build)…");
  const build = spawnSync("npm", ["run", "build"], { cwd: repoRoot, stdio: "inherit" });
  if (build.status !== 0) {
    process.exit(build.status ?? 1);
  }
}

// Enable Electron verbose logging so crash info surfaces during development and
// in Playwright smoke runs.
process.env.ELECTRON_ENABLE_LOGGING = "1";

ensurePreloadBundle();
console.info("[desktop] starting Electron…");

// Use async spawn so we can forward OS signals to the child process — this
// ensures Ctrl-C and SIGTERM from process managers reach Electron cleanly.
const tsxLoader = join(repoRoot, "node_modules", "tsx", "dist", "loader.mjs");
const electronArgs = existsSync(tsxLoader) ? ["--import", tsxLoader, join(desktopDir, "main.ts")] : [join(desktopDir, "main.ts")];
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
