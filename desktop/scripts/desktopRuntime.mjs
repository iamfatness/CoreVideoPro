import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { buildSync } from "esbuild";

const desktopDir = dirname(dirname(fileURLToPath(import.meta.url)));
export const repoRoot = dirname(desktopDir);

export function resolveElectron(root = repoRoot) {
  if (process.env.ELECTRON_OVERRIDE_BIN && existsSync(process.env.ELECTRON_OVERRIDE_BIN)) {
    return process.env.ELECTRON_OVERRIDE_BIN;
  }
  const roots = [join(root, "node_modules"), join(desktopDir, "node_modules")];
  for (const nodeModules of roots) {
    const binary = join(nodeModules, "electron", "dist", process.platform === "win32" ? "electron.exe" : "electron");
    if (existsSync(binary)) {
      return binary;
    }
    const binNames = process.platform === "win32" ? ["electron.cmd", "electron"] : ["electron"];
    for (const binName of binNames) {
      const shim = join(nodeModules, ".bin", binName);
      if (existsSync(shim)) {
        return shim;
      }
    }
  }
  return undefined;
}

export function ensurePreloadBundle(root = repoRoot) {
  const preloadTs = join(desktopDir, "preload.ts");
  const preloadCjs = join(desktopDir, "preload.cjs");
  if (!existsSync(preloadTs)) {
    return preloadCjs;
  }
  buildSync({
    entryPoints: [preloadTs],
    outfile: preloadCjs,
    bundle: true,
    platform: "node",
    format: "cjs",
    external: ["electron"]
  });
  return preloadCjs;
}

/** Self-contained media-core stub for packaged Electron (asar cannot run .ts imports). */
export function ensureCoreStubBundle(root = repoRoot) {
  const coreStubTs = join(desktopDir, "coreStub.ts");
  const coreStubCjs = join(desktopDir, "coreStub.cjs");
  if (!existsSync(coreStubTs)) {
    return coreStubCjs;
  }
  buildSync({
    entryPoints: [coreStubTs],
    outfile: coreStubCjs,
    bundle: true,
    platform: "node",
    format: "cjs"
  });
  return coreStubCjs;
}

/** Bundled ESM entry for Playwright Electron (tsx `--import` is unreliable under Playwright's loader). */
/** Bundled ESM entry for packaged Electron builds (asar-safe, no tsx loader). */
export function ensurePackagedMainBundle(root = repoRoot) {
  const mainTs = join(desktopDir, "main.ts");
  const mainPackaged = join(desktopDir, "main-packaged.mjs");
  if (!existsSync(mainTs)) {
    return mainPackaged;
  }
  buildSync({
    entryPoints: [mainTs],
    outfile: mainPackaged,
    bundle: true,
    platform: "node",
    format: "esm",
    external: ["electron", "electron-updater"]
  });
  return mainPackaged;
}

export function ensureE2eMainBundle(root = repoRoot) {
  const mainTs = join(desktopDir, "main.ts");
  const mainE2e = join(desktopDir, "main-e2e.mjs");
  if (!existsSync(mainTs)) {
    return mainE2e;
  }
  buildSync({
    entryPoints: [mainTs],
    outfile: mainE2e,
    bundle: true,
    platform: "node",
    format: "esm",
    external: ["electron", "electron-updater"]
  });
  return mainE2e;
}

export function e2eMediaCoreEnv(root = repoRoot) {
  const coreStub = join(desktopDir, "coreStub.ts");
  return {
    COREVIDEO_MEDIA_CORE_COMMAND: process.execPath,
    COREVIDEO_MEDIA_CORE_ARGS: coreStub
  };
}

export function electronMainArgs(root = repoRoot) {
  const tsxLoader = join(root, "node_modules", "tsx", "dist", "loader.mjs");
  const mainTs = join(desktopDir, "main.ts");
  return existsSync(tsxLoader) ? ["--import", tsxLoader, mainTs] : [mainTs];
}