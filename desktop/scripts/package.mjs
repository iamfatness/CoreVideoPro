/**
 * Dev-machine / CI packaging entry point for CoreVideo Pro.
 *
 * Builds the Vite renderer, stages optional native binaries, bundles the
 * Electron main + preload entries, generates icons, then invokes electron-builder.
 *
 * Code signing is opt-in via standard electron-builder env vars:
 *   Windows: CSC_LINK, CSC_KEY_PASSWORD
 *   macOS:   CSC_LINK, CSC_KEY_PASSWORD, APPLE_ID, APPLE_APP_SPECIFIC_PASSWORD, APPLE_TEAM_ID
 */
import { spawnSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import {
  ensureCoreStubBundle,
  ensurePackagedMainBundle,
  ensurePreloadBundle,
  repoRoot
} from "./desktopRuntime.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const desktopDir = dirname(here);

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    stdio: "inherit",
    cwd: repoRoot,
    shell: process.platform === "win32",
    ...options
  });
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}

function resolveElectronBuilder() {
  const candidates = [
    join(repoRoot, "node_modules", ".bin", process.platform === "win32" ? "electron-builder.cmd" : "electron-builder"),
    join(desktopDir, "node_modules", ".bin", process.platform === "win32" ? "electron-builder.cmd" : "electron-builder")
  ];
  return candidates.find((candidate) => existsSync(candidate));
}

if (!resolveElectronBuilder()) {
  console.info("[pack] installing packaging deps (electron + electron-updater + electron-builder)…");
  run("npm", ["install", "--no-save", "electron@latest", "electron-updater@6.8.9", "electron-builder@latest", "tsx"]);
  run("node", ["node_modules/electron/install.js"]);
}

const electronBuilder = resolveElectronBuilder();
if (!electronBuilder) {
  console.error("[pack] electron-builder is still missing after install. Re-run: npm run pack:desktop");
  process.exit(1);
}

console.info("[pack] generating icons…");
run("node", ["desktop/scripts/generate-icons.mjs"]);

console.info("[pack] building renderer…");
run("npm", ["run", "build"]);

console.info("[pack] bundling Electron entries…");
ensurePreloadBundle();
ensureCoreStubBundle();
ensurePackagedMainBundle();

const nativeSource = process.env.COREVIDEO_NATIVE_BUILD_DIR?.trim() || join(repoRoot, "native", "build-dev", "Release");
const stageDir = join(desktopDir, "build", "native");
mkdirSync(stageDir, { recursive: true });
if (existsSync(nativeSource)) {
  const names = readdirSync(nativeSource).filter((name) => /^corevideo-/.test(name) && !name.endsWith(".pdb"));
  for (const name of names) {
    copyFileSync(join(nativeSource, name), join(stageDir, name));
    console.info(`[pack] staged native binary: ${name}`);
  }
} else {
  console.warn(`[pack] native build dir not found (${nativeSource}); packaging stub-only media core.`);
}

const target = process.env.COREVIDEO_PACK_TARGET?.trim();
const publish = process.env.COREVIDEO_PUBLISH?.trim() || "never";
const builderArgs = ["--config", "desktop/electron-builder.yml", "--publish", publish];
if (target) {
  builderArgs.push(target);
}

if (publish !== "never" && !process.env.GH_TOKEN?.trim()) {
  console.error("[pack] COREVIDEO_PUBLISH requires GH_TOKEN (GitHub Releases upload).");
  process.exit(1);
}

const signingConfigured = Boolean(process.env.CSC_LINK?.trim());
if (signingConfigured) {
  console.info("[pack] CSC_LINK detected — electron-builder will sign (and notarize on macOS when Apple creds are set).");
} else {
  console.info("[pack] no CSC_LINK — producing unsigned installers.");
}

console.info("[pack] running electron-builder…");
run(electronBuilder, builderArgs);