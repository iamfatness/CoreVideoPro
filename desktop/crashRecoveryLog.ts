import { mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname } from "node:path";

export type CrashRecoveryEvent = {
  at: string;
  exitCode: number | null;
  restartCount: number;
};

export type CrashRecoveryLog = {
  events: CrashRecoveryEvent[];
};

const DEFAULT_MAX_EVENTS = 50;

export async function appendCrashRecoveryEvent(
  filePath: string,
  event: CrashRecoveryEvent,
  maxEvents: number = DEFAULT_MAX_EVENTS
): Promise<CrashRecoveryLog> {
  const log = await readCrashRecoveryLog(filePath);
  log.events.push(event);
  if (log.events.length > maxEvents) {
    log.events = log.events.slice(log.events.length - maxEvents);
  }
  await mkdir(dirname(filePath), { recursive: true });
  await writeFile(filePath, `${JSON.stringify(log, null, 2)}\n`, "utf8");
  return log;
}

export async function readCrashRecoveryEvents(filePath: string): Promise<CrashRecoveryEvent[]> {
  return (await readCrashRecoveryLog(filePath)).events;
}

async function readCrashRecoveryLog(filePath: string): Promise<CrashRecoveryLog> {
  try {
    const raw = await readFile(filePath, "utf8");
    const parsed = JSON.parse(raw) as Partial<CrashRecoveryLog>;
    if (!Array.isArray(parsed.events)) {
      return { events: [] };
    }
    return {
      events: parsed.events.filter(isCrashRecoveryEvent)
    };
  } catch {
    return { events: [] };
  }
}

function isCrashRecoveryEvent(value: unknown): value is CrashRecoveryEvent {
  if (typeof value !== "object" || value === null) {
    return false;
  }
  const event = value as Partial<CrashRecoveryEvent>;
  return (
    typeof event.at === "string" &&
    (typeof event.exitCode === "number" || event.exitCode === null) &&
    typeof event.restartCount === "number"
  );
}