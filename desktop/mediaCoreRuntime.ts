import type { MediaCoreSupervisorOptions } from "./mediaCoreClient.ts";

export function mediaCoreSupervisorOptionsFromEnv(env: NodeJS.ProcessEnv): Pick<MediaCoreSupervisorOptions, "command" | "args" | "cwd"> {
  const command = env.COREVIDEO_MEDIA_CORE_COMMAND?.trim();
  if (!command) {
    return {};
  }

  return {
    command,
    args: parseArgs(env.COREVIDEO_MEDIA_CORE_ARGS),
    cwd: env.COREVIDEO_MEDIA_CORE_CWD?.trim() || undefined
  };
}

function parseArgs(value: string | undefined): string[] {
  if (!value?.trim()) {
    return [];
  }
  return value
    .split(/\s+/)
    .map((arg) => arg.trim())
    .filter(Boolean);
}
