import { describe, expect, it } from "vitest";
import { createTestCaptureDevices, MockCaptureDeviceEngine } from "./captureDevices";

describe("MockCaptureDeviceEngine", () => {
  it("starts with no fabricated capture hardware", async () => {
    const engine = new MockCaptureDeviceEngine();
    const devices = await engine.listDevices();

    expect(devices).toEqual([]);
  });

  it("switches the selected input for a seeded device", async () => {
    const engine = new MockCaptureDeviceEngine();
    engine.seedDevices(createTestCaptureDevices());
    const devices = await engine.selectInput("decklink-1", "hdmi-1");

    const decklink = devices.find((device) => device.id === "decklink-1");
    expect(decklink?.selectedInputId).toBe("hdmi-1");
  });

  it("ignores an unknown input id", async () => {
    const engine = new MockCaptureDeviceEngine();
    engine.seedDevices(createTestCaptureDevices());
    const devices = await engine.selectInput("decklink-1", "nonexistent");

    const decklink = devices.find((device) => device.id === "decklink-1");
    expect(decklink?.selectedInputId).toBe("sdi-1");
  });

  it("sets and clamps the audio sync offset", async () => {
    const engine = new MockCaptureDeviceEngine();
    engine.seedDevices(createTestCaptureDevices());

    const updated = await engine.setAudioSyncOffset("aja-io-1", 120);
    expect(updated.find((device) => device.id === "aja-io-1")?.audioSyncOffsetMs).toBe(120);

    const clamped = await engine.setAudioSyncOffset("aja-io-1", 9000);
    expect(clamped.find((device) => device.id === "aja-io-1")?.audioSyncOffsetMs).toBe(500);
  });

  it("brings a detected second device online as a live source", async () => {
    const engine = new MockCaptureDeviceEngine();
    engine.seedDevices(createTestCaptureDevices());

    const devices = await engine.connectDevice("aja-io-1");
    const aja = devices.find((device) => device.id === "aja-io-1");

    expect(aja?.connectionState).toBe("connected");
    expect(aja?.signalPresent).toBe(true);
  });
});