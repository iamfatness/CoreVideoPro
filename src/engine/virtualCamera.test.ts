import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import type { VirtualCameraState } from "../domain/production";
import { describeVirtualCamera, toggleVirtualCamera } from "./virtualCamera";

const output = initialProduction.output;

function state(overrides: Partial<VirtualCameraState> = {}): VirtualCameraState {
  return { enabled: false, deviceName: "CoreVideo Pro Camera", mirrored: false, ...overrides };
}

describe("describeVirtualCamera", () => {
  it("reports an offline camera when disabled", () => {
    const status = describeVirtualCamera(state(), output);
    expect(status.active).toBe(false);
    expect(status.label).toBe("Virtual camera offline");
    expect(status.summary).toBe("CoreVideo Pro Camera - idle");
  });

  it("publishes the program format when enabled", () => {
    const status = describeVirtualCamera(state({ enabled: true }), output);
    expect(status.active).toBe(true);
    expect(status.label).toBe("Publishing program to CoreVideo Pro Camera");
    expect(status.outputFormat).toBe("1920x1080 60fps");
  });

  it("notes a mirrored feed and a custom device name", () => {
    const status = describeVirtualCamera(state({ enabled: true, mirrored: true, deviceName: "Studio Cam" }), output);
    expect(status.outputFormat).toBe("1920x1080 60fps mirrored");
    expect(status.summary).toBe("Studio Cam - 1920x1080 60fps mirrored");
  });

  it("falls back to a default device name when blank", () => {
    const status = describeVirtualCamera(state({ deviceName: "  " }), output);
    expect(status.summary).toBe("CoreVideo Pro Camera - idle");
  });
});

describe("toggleVirtualCamera", () => {
  it("flips the enabled flag without mutating the input", () => {
    const start = state();
    const next = toggleVirtualCamera(start);
    expect(next.enabled).toBe(true);
    expect(start.enabled).toBe(false);
  });
});
