import { mkdtemp, readFile } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { afterEach, describe, expect, it } from "vitest";
import { appendCrashRecoveryEvent, readCrashRecoveryEvents } from "./crashRecoveryLog.ts";

describe("crashRecoveryLog", () => {
  let tempDir = "";

  afterEach(() => {
    tempDir = "";
  });

  async function makeLogPath() {
    tempDir = await mkdtemp(join(tmpdir(), "corevideo-crash-"));
    return join(tempDir, "crash-recovery.json");
  }

  it("appends crash events and trims to the configured maximum", async () => {
    const filePath = await makeLogPath();

    for (let restartCount = 1; restartCount <= 3; restartCount += 1) {
      await appendCrashRecoveryEvent(filePath, {
        at: `2026-06-14T12:00:0${restartCount}.000Z`,
        exitCode: 1,
        restartCount
      });
    }

    const events = await readCrashRecoveryEvents(filePath);
    expect(events).toHaveLength(3);
    expect(events[2]).toMatchObject({ restartCount: 3, exitCode: 1 });

    const raw = await readFile(filePath, "utf8");
    expect(raw).toContain("restartCount");
  });

  it("returns an empty list when the log file does not exist", async () => {
    const filePath = await makeLogPath();
    await expect(readCrashRecoveryEvents(filePath)).resolves.toEqual([]);
  });
});