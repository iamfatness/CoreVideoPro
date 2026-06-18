import { describe, expect, it } from "vitest";
import type { CaptureDeviceState } from "../domain/production";
import { createTestCaptureDevices, SimulatedCaptureDeviceSession } from "./captureDevices";
import { NativeCaptureDeviceEngineAdapter } from "./nativeCaptureDeviceEngineAdapter";
import {
  NativeZoomBridgeError,
  type NativeBridgeCommand,
  type NativeBridgeResponse,
  type NativeZoomTransport
} from "./nativeBridgeProtocol";

class MemoryNativeCaptureTransport implements NativeZoomTransport {
  public readonly commands: NativeBridgeCommand[] = [];
  private readonly session = new SimulatedCaptureDeviceSession();

  constructor() {
    this.session.seedDevices(createTestCaptureDevices());
  }
  private failNext: NativeBridgeResponse | null = null;

  failOnce(response: NativeBridgeResponse) {
    this.failNext = response;
  }

  async request(command: NativeBridgeCommand): Promise<NativeBridgeResponse> {
    this.commands.push(command);
    if (this.failNext) {
      const response = this.failNext;
      this.failNext = null;
      return response;
    }

    switch (command.type) {
      case "list-capture-devices":
        return this.devicesResponse(command.id, this.session.listDevices());
      case "select-capture-input":
        return this.devicesResponse(command.id, this.session.selectInput(command.payload.deviceId, command.payload.inputId));
      case "set-capture-audio-sync-offset":
        return this.devicesResponse(
          command.id,
          this.session.setAudioSyncOffset(command.payload.deviceId, command.payload.offsetMs)
        );
      case "connect-capture-device":
        return this.devicesResponse(command.id, this.session.connectDevice(command.payload.deviceId));
      default:
        return {
          id: command.id,
          ok: false,
          error: {
            code: "native-unavailable",
            message: "Capture transport received a non-capture command."
          }
        };
    }
  }

  private devicesResponse(id: string, devices: CaptureDeviceState[]): NativeBridgeResponse {
    return { id, ok: true, devices };
  }
}

describe("NativeCaptureDeviceEngineAdapter", () => {
  it("lists capture devices through the native bridge", async () => {
    const transport = new MemoryNativeCaptureTransport();
    const adapter = new NativeCaptureDeviceEngineAdapter(transport);

    const devices = await adapter.listDevices();

    expect(transport.commands.map((command) => command.type)).toEqual(["list-capture-devices"]);
    expect(devices.map((device) => device.id)).toEqual(["decklink-1", "aja-io-1"]);
    expect(devices[0].selectedInputId).toBe("sdi-1");
  });

  it("selects an input and sets the audio sync offset", async () => {
    const transport = new MemoryNativeCaptureTransport();
    const adapter = new NativeCaptureDeviceEngineAdapter(transport);

    const afterSelect = await adapter.selectInput("decklink-1", "hdmi-1");
    const afterOffset = await adapter.setAudioSyncOffset("decklink-1", 1200);

    expect(transport.commands.map((command) => command.type)).toEqual([
      "select-capture-input",
      "set-capture-audio-sync-offset"
    ]);
    expect((transport.commands[0] as Extract<NativeBridgeCommand, { type: "select-capture-input" }>).payload).toEqual({
      deviceId: "decklink-1",
      inputId: "hdmi-1"
    });
    expect(afterSelect.find((device) => device.id === "decklink-1")?.selectedInputId).toBe("hdmi-1");
    expect(afterOffset.find((device) => device.id === "decklink-1")?.audioSyncOffsetMs).toBe(500);
  });

  it("connects a detected capture device", async () => {
    const transport = new MemoryNativeCaptureTransport();
    const adapter = new NativeCaptureDeviceEngineAdapter(transport);

    const devices = await adapter.connectDevice("aja-io-1");

    expect(transport.commands.map((command) => command.type)).toEqual(["connect-capture-device"]);
    const device = devices.find((candidate) => candidate.id === "aja-io-1");
    expect(device?.connectionState).toBe("connected");
    expect(device?.signalPresent).toBe(true);
  });

  it("throws typed bridge errors from native capture failures", async () => {
    const transport = new MemoryNativeCaptureTransport();
    const adapter = new NativeCaptureDeviceEngineAdapter(transport);
    transport.failOnce({
      id: "capture:1",
      ok: false,
      error: {
        code: "connect-device-failed",
        message: "Capture card did not acknowledge the connect request."
      }
    });

    await expect(adapter.connectDevice("decklink-1")).rejects.toMatchObject({
      name: "NativeZoomBridgeError",
      code: "connect-device-failed",
      message: "Capture card did not acknowledge the connect request."
    } satisfies Partial<NativeZoomBridgeError>);
  });
});
