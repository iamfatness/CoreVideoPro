import { describe, expect, it } from "vitest";
import { inspectZoomWindowsSdkPackage, type ZoomWindowsSdkEntry } from "./zoomWindowsSdkPackage";

const sdkRoot = "zoom-sdk-windows-7.0.5.39292";

function entry(fullName: string, length = 1024): ZoomWindowsSdkEntry {
  return { fullName, length };
}

function completeEntries(): ZoomWindowsSdkEntry[] {
  return [
    entry(`${sdkRoot}/version.txt`, 15),
    entry(`${sdkRoot}/x64/bin/sdk.dll`, 2_135_432),
    entry(`${sdkRoot}/x64/bin/sdkExt.dll`, 8_050_048),
    entry(`${sdkRoot}/x64/bin/ssb_sdk.dll`, 6_795_136),
    entry(`${sdkRoot}/x64/bin/libcml.dll`, 15_455_624),
    entry(`${sdkRoot}/x64/bin/libcrypto-3-zm.dll`, 5_873_024),
    entry(`${sdkRoot}/x64/bin/libssl-3-zm.dll`, 1_324_416),
    entry(`${sdkRoot}/x64/bin/libcurl.dll`, 916_352),
    entry(`${sdkRoot}/x64/bin/zVideoApp.dll`, 16_826_752),
    entry(`${sdkRoot}/x64/bin/zVideoUI.dll`, 18_965_896),
    entry(`${sdkRoot}/x64/bin/zVideoUIPlugin.dll`, 15_273_352),
    entry(`${sdkRoot}/x64/bin/zTscoder.exe`, 536_960),
    entry(`${sdkRoot}/x64/bin/zCrashReport64.exe`, 269_696),
    entry(`${sdkRoot}/x64/bin/zWebview2Agent.exe`, 1_286_528),
    ...Array.from({ length: 20 }, (_, index) => entry(`${sdkRoot}/x64/bin/runtime-${index}.dll`, 400_000 + index)),
    entry(`${sdkRoot}/x64/lib/sdk.lib`, 7_340),
    entry(`${sdkRoot}/x64/h/zoom_sdk.h`, 4_737),
    entry(`${sdkRoot}/x64/h/meeting_service_interface.h`, 49_392),
    entry(`${sdkRoot}/x64/h/rawdata/zoom_rawdata_api.h`, 800),
    entry(`${sdkRoot}/x64/h/rawdata/rawdata_renderer_interface.h`, 1_994),
    entry(`${sdkRoot}/x64/h/rawdata/rawdata_audio_helper_interface.h`, 3_471)
  ];
}

describe("inspectZoomWindowsSdkPackage", () => {
  it("marks the real Zoom 7.0.5 x64 package layout ready for helper packaging", () => {
    const report = inspectZoomWindowsSdkPackage({
      entries: completeEntries(),
      architecture: "x64",
      expectedVersion: "7.0.5.39292"
    });

    expect(report).toMatchObject({
      status: "ready",
      architecture: "x64",
      rootFolder: sdkRoot,
      version: "7.0.5.39292",
      runtimeExeCount: 3,
      rawMediaHeaderCount: 3,
      copyPlan: {
        includeRoot: `${sdkRoot}/x64/h`,
        runtimeGlob: `${sdkRoot}/x64/bin/*`,
        includeGlob: `${sdkRoot}/x64/h/**/*`,
        libraryPath: `${sdkRoot}/x64/lib/sdk.lib`,
        helperRuntimeTarget: "native-core/zoom-runtime/windows/x64"
      },
      summary: "Zoom Windows Meeting SDK 7.0.5.39292 x64 package is ready for helper packaging."
    });
    expect(report.runtimeDllCount).toBeGreaterThanOrEqual(20);
    expect(report.requiredFiles.every((file) => file.present)).toBe(true);
  });

  it("blocks when the raw media headers needed for participant feeds are missing", () => {
    const entries = completeEntries().filter((candidate) => !candidate.fullName.includes("/h/rawdata/"));
    const report = inspectZoomWindowsSdkPackage({ entries, expectedVersion: "7.0.5.39292" });

    expect(report.status).toBe("blocked");
    expect(report.blockers).toEqual([
      "x64/h/rawdata/zoom_rawdata_api.h is missing.",
      "x64/h/rawdata/rawdata_renderer_interface.h is missing.",
      "x64/h/rawdata/rawdata_audio_helper_interface.h is missing."
    ]);
    expect(report.warnings).toContain("Raw media headers are incomplete.");
  });

  it("blocks on an unexpected SDK version so helper binaries stay aligned", () => {
    const report = inspectZoomWindowsSdkPackage({
      entries: completeEntries(),
      expectedVersion: "7.0.4.00000"
    });

    expect(report.status).toBe("blocked");
    expect(report.blockers).toEqual(["SDK version 7.0.5.39292 does not match expected 7.0.4.00000."]);
  });

  it("supports arm64 validation using the same package root", () => {
    const armEntries = completeEntries().map((candidate) => ({
      ...candidate,
      fullName: candidate.fullName.replace("/x64/", "/arm64/")
    }));
    const report = inspectZoomWindowsSdkPackage({
      entries: armEntries,
      architecture: "arm64",
      expectedVersion: "7.0.5.39292"
    });

    expect(report.status).toBe("ready");
    expect(report.copyPlan.runtimeGlob).toBe(`${sdkRoot}/arm64/bin/*`);
  });
});
