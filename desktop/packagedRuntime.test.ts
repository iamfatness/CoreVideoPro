import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { afterEach, describe, expect, it } from "vitest";
import { mediaCoreSupervisorOptionsForApp, resolvePackagedMediaCoreBinary } from "./packagedRuntime.ts";

describe("packagedRuntime", () => {
  let tempDir = "";

  afterEach(() => {
    tempDir = "";
  });

  it("resolves the packaged native binary when present", () => {
    tempDir = mkdtempSync(join(tmpdir(), "corevideo-native-"));
    const nativeDir = join(tempDir, "native");
    mkdirSync(nativeDir, { recursive: true });
    const binaryPath = join(nativeDir, "corevideo-native.exe");
    writeFileSync(binaryPath, "");

    expect(resolvePackagedMediaCoreBinary(tempDir, "win32")).toBe(binaryPath);
  });

  it("prefers the packaged binary over env overrides when the app is packaged", () => {
    tempDir = mkdtempSync(join(tmpdir(), "corevideo-native-"));
    const nativeDir = join(tempDir, "native");
    mkdirSync(nativeDir, { recursive: true });
    const binaryPath = join(nativeDir, "corevideo-native.exe");
    writeFileSync(binaryPath, "");

    const options = mediaCoreSupervisorOptionsForApp(
      {
        COREVIDEO_MEDIA_CORE_COMMAND: "C:\\stub\\core.exe",
        COREVIDEO_MEDIA_CORE_ARGS: "--stub"
      },
      tempDir,
      "win32",
      true
    );

    expect(options.command).toBe(binaryPath);
    expect(options.args).toEqual(["--stub"]);
  });

  it("enables Electron-as-Node for stub-only packaged builds", () => {
    tempDir = mkdtempSync(join(tmpdir(), "corevideo-stub-"));
    const options = mediaCoreSupervisorOptionsForApp({}, tempDir, "darwin", true);

    expect(options.command).toBeUndefined();
    expect(options.args).toEqual([expect.stringMatching(/coreStub\.cjs$/)]);
    expect(options.env).toMatchObject({ ELECTRON_RUN_AS_NODE: "1" });
  });
});