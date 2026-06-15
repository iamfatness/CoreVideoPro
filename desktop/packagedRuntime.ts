import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import type { MediaCoreSupervisorOptions } from "./mediaCoreClient.ts";
import { mediaCoreSupervisorOptionsFromEnv } from "./mediaCoreRuntime.ts";

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

  // Stub-only package: spawn the Node media-core stub via Electron-as-Node.
  return {
    ...fromEnv,
    env: {
      ...fromEnv.env,
      ELECTRON_RUN_AS_NODE: "1"
    }
  };
}