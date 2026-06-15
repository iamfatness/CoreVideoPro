import type { CaptureDeviceState } from "../src/domain/production.ts";
import type { CoreCaptureBridgeRequest, CoreResponse } from "./coreProtocol.ts";

const INITIAL_DEVICES: CaptureDeviceState[] = [
  {
    id: "decklink-1",
    vendor: "blackmagic",
    name: "DeckLink Mini Recorder 4K",
    inputs: [
      { id: "sdi-1", label: "SDI 1", hasEmbeddedAudio: true },
      { id: "hdmi-1", label: "HDMI", hasEmbeddedAudio: true }
    ],
    selectedInputId: "sdi-1",
    resolution: { width: 1920, height: 1080 },
    frameRate: 60,
    connectionState: "connected",
    signalPresent: true,
    droppedFrames: 0,
    audioSyncOffsetMs: 0
  },
  {
    id: "aja-io-1",
    vendor: "aja",
    name: "AJA Io 4K Plus",
    inputs: [
      { id: "sdi-1", label: "SDI 1", hasEmbeddedAudio: true },
      { id: "sdi-2", label: "SDI 2", hasEmbeddedAudio: false }
    ],
    selectedInputId: "sdi-1",
    resolution: { width: 1920, height: 1080 },
    frameRate: 30,
    connectionState: "detected",
    signalPresent: false,
    droppedFrames: 0,
    audioSyncOffsetMs: 0
  }
];

class CaptureDeviceSession {
  private devices: CaptureDeviceState[] = INITIAL_DEVICES.map((device) => ({
    ...device,
    inputs: device.inputs.map((input) => ({ ...input }))
  }));

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
  return { id, ok: true, devices };
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