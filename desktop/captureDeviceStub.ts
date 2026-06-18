import type { CaptureDeviceState } from "../src/domain/production.ts";
import type { CoreCaptureBridgeRequest, CoreResponse } from "./coreProtocol.ts";

class CaptureDeviceSession {
  private devices: CaptureDeviceState[] = [];

  list(): CaptureDeviceState[] {
    return this.devices.map((device) => ({ ...device, inputs: device.inputs.map((input) => ({ ...input })) }));
  }

  selectInput(deviceId: string, inputId: string): CaptureDeviceState[] {
    this.devices = this.devices.map((device) => {
      if (device.id !== deviceId) {
        return device;
      }
      if (!device.inputs.some((input) => input.id === inputId)) {
        return device;
      }
      return { ...device, selectedInputId: inputId };
    });
    return this.list();
  }

  setAudioSyncOffset(deviceId: string, offsetMs: number): CaptureDeviceState[] {
    const clamped = Math.max(-500, Math.min(500, offsetMs));
    this.devices = this.devices.map((device) =>
      device.id === deviceId ? { ...device, audioSyncOffsetMs: clamped } : device
    );
    return this.list();
  }

  connect(deviceId: string): CaptureDeviceState[] {
    this.devices = this.devices.map((device) =>
      device.id === deviceId && device.connectionState !== "connected"
        ? { ...device, connectionState: "connected", signalPresent: true }
        : device
    );
    return this.list();
  }
}

const session = new CaptureDeviceSession();

function captureResponse(id: string, devices: CaptureDeviceState[]): CoreResponse {
  return { id, ok: true, type: "capture-devices", devices };
}

export function handleCaptureDeviceRequest(request: CoreCaptureBridgeRequest): CoreResponse {
  switch (request.type) {
    case "list-capture-devices":
      return captureResponse(request.id, session.list());
    case "select-capture-input":
      return captureResponse(request.id, session.selectInput(request.payload.deviceId, request.payload.inputId));
    case "set-capture-audio-sync-offset":
      return captureResponse(request.id, session.setAudioSyncOffset(request.payload.deviceId, request.payload.offsetMs));
    case "connect-capture-device":
      return captureResponse(request.id, session.connect(request.payload.deviceId));
  }
}
