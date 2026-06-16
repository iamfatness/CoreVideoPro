import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import type { MediaCoreSupervisorOptions } from "./mediaCoreClient.ts";
import { mediaCoreSupervisorOptionsFromEnv } from "./mediaCoreRuntime.ts";

const desktopDir = dirname(fileURLToPath(import.meta.url));
const PACKAGED_STUB_PATH = join(desktopDir, "coreStub.cjs");

export function resolvePackagedMediaCoreBinary(resourcesPath: string, platform: string): string | undefined {
  const binaryName = platform === "win32" ? "corevideo-native.exe" : "corevideo-native";
  const command = join(resourcesPath, "native", binaryName);
  return existsSync(command) ? command : undefined;
}

export function mediaCoreSupervisorOptionsForApp(
  env: NodeJS.ProcessEnv,
  resourcesPath: string | undefined,
  platform: string,
  packaged: boolean
): Pick<MediaCoreSupervisorOptions, "command" | "args" | "cwd" | "env"> {
  const fromEnv = mediaCoreSupervisorOptionsFromEnv(env);
  if (!packaged || !resourcesPath) {
    return fromEnv;
  }

  const packagedCommand = resolvePackagedMediaCoreBinary(resourcesPath, platform);
  if (packagedCommand) {
    return {
      ...fromEnv,
      command: packagedCommand,
      args: fromEnv.args ?? [],
      cwd: fromEnv.cwd ?? dirname(packagedCommand)
    };
  }

  // Stub-only package: spawn the bundled media-core stub via Electron-as-Node.
  // The .ts stub imports src/ modules that are not shipped in the asar.
  return {
    ...fromEnv,
    args: [PACKAGED_STUB_PATH],
    env: {
      ...fromEnv.env,
      ELECTRON_RUN_AS_NODE: "1"
    }
  };
}