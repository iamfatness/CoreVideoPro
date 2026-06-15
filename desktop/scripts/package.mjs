/**
 * Dev-machine packaging entry point for CoreVideo Pro.
 *
 * Builds the Vite renderer, optionally stages native binaries beside the app,
 * then invokes electron-builder using desktop/electron-builder.yml.
 *
 * Code signing is opt-in via standard electron-builder env vars:
 *   Windows: CSC_LINK, CSC_KEY_PASSWORD
 *   macOS:   CSC_LINK, CSC_KEY_PASSWORD, APPLE_ID, APPLE_APP_SPECIFIC_PASSWORD, APPLE_TEAM_ID
 */
import { spawnSync } from "node:child_process";
import { copyFileSync, existsSync, mkdirSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const desktopDir = dirname(here);
const repoRoot = dirname(desktopDir);

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { stdio: "inherit", cwd: repoRoot, ...options });
  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }
}

function resolveElectronBuilder() {
  const candidates = [
    join(repoRoot, "node_modules", ".bin", "electron-builder"),
    join(desktopDir, "node_modules", ".bin", "electron-builder")
  ];
  return candidates.find((candidate) => existsSync(candidate));
}

const electronBuilder = resolveElectronBuilder();
if (!electronBuilder) {
  console.error(
    [
      "electron-builder is not installed.",
      "Install packaging deps once:",
      "",
      "  npm install --no-save electron@latest electron-builder@latest",
      "",
      "Then re-run: npm run pack:desktop"
    ].join("\n")
  );
  process.exit(1);
}

console.info("[pack] building renderer…");
run("npm", ["run", "build"]);

const nativeSource = process.env.COREVIDEO_NATIVE_BUILD_DIR?.trim() || join(repoRoot, "native", "build-dev", "Release");
const stageDir = join(desktopDir, "build", "native");
if (existsSync(nativeSource)) {
  mkdirSync(stageDir, { recursive: true });
  const names = readdirSync(nativeSource).filter((name) => /^corevideo-/.test(name) && !name.endsWith(".pdb"));
  for (const name of names) {
    copyFileSync(join(nativeSource, name), join(stageDir, name));
    console.info(`[pack] staged native binary: ${name}`);
  }
} else {
  console.warn(`[pack] native build dir not found (${nativeSource}); packaging stub-only media core.`);
}

const target = process.env.COREVIDEO_PACK_TARGET?.trim();
const builderArgs = ["--config", "desktop/electron-builder.yml", "--publish", "never"];
if (target) {
  builderArgs.push(target);
}

console.info("[pack] running electron-builder…");
run(electronBuilder, builderArgs);