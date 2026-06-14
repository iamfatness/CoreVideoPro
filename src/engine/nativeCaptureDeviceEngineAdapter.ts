import type { CaptureDeviceState } from "../domain/production";
import type { CaptureDeviceEngine } from "./contracts";
import { NativeZoomBridgeError, type NativeCaptureDeviceCommand, type NativeZoomTransport } from "./nativeBridgeProtocol";

export class NativeCaptureDeviceEngineAdapter implements CaptureDeviceEngine {
  private sequence = 0;

  constructor(private readonly transport: NativeZoomTransport) {}

  async listDevices(): Promise<CaptureDeviceState[]> {
    return this.sendForDevices({ id: this.nextId(), type: "list-capture-devices" }, "list-devices-failed");
  }

  async selectInput(deviceId: string, inputId: string): Promise<CaptureDeviceState[]> {
    return this.sendForDevices(
      { id: this.nextId(), type: "select-capture-input", payload: { deviceId, inputId } },
      "select-input-failed"
    );
  }

  async setAudioSyncOffset(deviceId: string, offsetMs: number): Promise<CaptureDeviceState[]> {
    return this.sendForDevices(
      { id: this.nextId(), type: "set-capture-audio-sync-offset", payload: { deviceId, offsetMs } },
      "audio-sync-failed"
    );
  }

  async connectDevice(deviceId: string): Promise<CaptureDeviceState[]> {
    return this.sendForDevices(
      { id: this.nextId(), type: "connect-capture-device", payload: { deviceId } },
      "connect-device-failed"
    );
  }

  private async sendForDevices(
    command: NativeCaptureDeviceCommand,
    fallbackCode: "list-devices-failed" | "select-input-failed" | "audio-sync-failed" | "connect-device-failed"
  ): Promise<CaptureDeviceState[]> {
    const response = await this.transport.request(command);
    if (!response.ok) {
      throw new NativeZoomBridgeError(response.error.code, response.error.message);
    }

    if (!("devices" in response)) {
      throw new NativeZoomBridgeError(fallbackCode, "Native capture bridge returned a non-device response.");
    }

    return response.devices;
  }

  private nextId() {
    this.sequence += 1;
    return `capture:${this.sequence}`;
  }
}
