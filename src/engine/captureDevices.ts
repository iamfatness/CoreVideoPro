import type { CaptureDeviceEngine } from "./contracts";
import type { CaptureDeviceState } from "../domain/production";

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

export class SimulatedCaptureDeviceSession {
  private devices: CaptureDeviceState[] = INITIAL_DEVICES.map((device) => ({ ...device, inputs: [...device.inputs] }));

  listDevices(): CaptureDeviceState[] {
    return this.devices.map((device) => ({ ...device }));
  }

  selectInput(deviceId: string, inputId: string): CaptureDeviceState[] {
    this.devices = this.devices.map((device) => {
      if (device.id !== deviceId) {
        return device;
      }

      const input = device.inputs.find((candidate) => candidate.id === inputId);
      if (!input) {
        return device;
      }

      return { ...device, selectedInputId: inputId };
    });

    return this.listDevices();
  }

  setAudioSyncOffset(deviceId: string, offsetMs: number): CaptureDeviceState[] {
    const clamped = Math.max(-500, Math.min(500, offsetMs));
    this.devices = this.devices.map((device) =>
      device.id === deviceId ? { ...device, audioSyncOffsetMs: clamped } : device
    );

    return this.listDevices();
  }
}

export class MockCaptureDeviceEngine implements CaptureDeviceEngine {
  private session = new SimulatedCaptureDeviceSession();

  async listDevices(): Promise<CaptureDeviceState[]> {
    return this.session.listDevices();
  }

  async selectInput(deviceId: string, inputId: string): Promise<CaptureDeviceState[]> {
    return this.session.selectInput(deviceId, inputId);
  }

  async setAudioSyncOffset(deviceId: string, offsetMs: number): Promise<CaptureDeviceState[]> {
    return this.session.setAudioSyncOffset(deviceId, offsetMs);
  }
}
