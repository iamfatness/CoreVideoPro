import { describe, expect, it } from "vitest";
import type { CaptureDeviceState } from "../domain/production";
import { estimateDeviceBandwidthMbps, summarizeCaptureFleet } from "./captureFleet";

function device(overrides: Partial<CaptureDeviceState>): CaptureDeviceState {
  return {
    id: "dev",
    vendor: "blackmagic",
    name: "Device",
    inputs: [{ id: "sdi-1", label: "SDI 1", hasEmbeddedAudio: true }],
    selectedInputId: "sdi-1",
    resolution: { width: 1920, height: 1080 },
    frameRate: 60,
    connectionState: "connected",
    signalPresent: true,
    droppedFrames: 0,
    audioSyncOffsetMs: 0,
    ...overrides
  };
}

describe("estimateDeviceBandwidthMbps", () => {
  it("scores a 1080p60 source at the ~10 Mbps reference", () => {
    expect(estimateDeviceBandwidthMbps(device({}))).toBe(10);
  });

  it("halves bandwidth for 1080p30", () => {
    expect(estimateDeviceBandwidthMbps(device({ frameRate: 30 }))).toBe(5);
  });
});

describe("summarizeCaptureFleet", () => {
  it("reports a single live source without a dual-capture warning", () => {
    const summary = summarizeCaptureFleet([
      device({ id: "a", name: "DeckLink" }),
      device({ id: "b", name: "AJA Io", connectionState: "detected", signalPresent: false })
    ]);

    expect(summary.connectedCount).toBe(1);
    expect(summary.liveSourceCount).toBe(1);
    expect(summary.dualCapture).toBe(false);
    expect(summary.summary).toMatch(/1 live \/ 1 connected/);
    expect(summary.warnings).toHaveLength(0);
  });

  it("flags dual capture and sums bandwidth when two devices are live", () => {
    const summary = summarizeCaptureFleet([
      device({ id: "a", name: "DeckLink" }),
      device({ id: "b", name: "AJA Io", vendor: "aja", frameRate: 30 })
    ]);

    expect(summary.dualCapture).toBe(true);
    expect(summary.liveSourceCount).toBe(2);
    expect(summary.estimatedBandwidthMbps).toBe(15);
    expect(summary.warnings).toContain(
      "Two capture sources are live; confirm each device's A/V sync offset against Zoom audio."
    );
  });

  it("warns about connected-but-silent and format-mismatch devices", () => {
    const summary = summarizeCaptureFleet([
      device({ id: "a", name: "DeckLink", signalPresent: false }),
      device({ id: "b", name: "AJA Io", connectionState: "format-mismatch", signalPresent: false })
    ]);

    expect(summary.warnings).toContain("DeckLink is connected but has no signal.");
    expect(summary.warnings).toContain("AJA Io has a format mismatch; set the input format manually.");
  });
});
