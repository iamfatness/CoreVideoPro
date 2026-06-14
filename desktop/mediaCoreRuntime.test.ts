import { describe, expect, it } from "vitest";
import { mediaCoreSupervisorOptionsFromEnv } from "./mediaCoreRuntime.ts";

describe("mediaCoreSupervisorOptionsFromEnv", () => {
  it("uses the bundled Node stub by default", () => {
    expect(mediaCoreSupervisorOptionsFromEnv({})).toEqual({});
  });

  it("selects a dev media-core command when configured", () => {
    expect(
      mediaCoreSupervisorOptionsFromEnv({
        COREVIDEO_MEDIA_CORE_COMMAND: "native/build/corevideo-native.exe",
        COREVIDEO_MEDIA_CORE_ARGS: "--dev --trace",
        COREVIDEO_MEDIA_CORE_CWD: "native/build"
      })
    ).toEqual({
      command: "native/build/corevideo-native.exe",
      args: ["--dev", "--trace"],
      cwd: "native/build"
    });
  });
});
