import type { Participant } from "../domain/production";
import { mapCaptureSnapshot } from "./captureSnapshotMapper";
import type { ZoomCaptureEngine, ZoomJoinRequest, ZoomSessionSnapshot } from "./contracts";
import {
  NativeZoomBridgeError,
  type NativeZoomCommand,
  type NativeZoomTransport
} from "./nativeBridgeProtocol";

export class NativeZoomEngineAdapter implements ZoomCaptureEngine {
  private lastSnapshot: ZoomSessionSnapshot | null = null;
  private sequence = 0;

  constructor(private readonly transport: NativeZoomTransport) {}

  async join(request: ZoomJoinRequest): Promise<ZoomSessionSnapshot> {
    return this.send({ id: this.nextId(), type: "join", payload: request });
  }

  async leave(): Promise<ZoomSessionSnapshot> {
    return this.send({ id: this.nextId(), type: "leave" });
  }

  async getParticipants(): Promise<Participant[]> {
    return (await this.getSnapshot()).participants;
  }

  async getSnapshot(): Promise<ZoomSessionSnapshot> {
    return this.send({ id: this.nextId(), type: "snapshot" });
  }

  getCachedSnapshot(): ZoomSessionSnapshot | null {
    return this.lastSnapshot;
  }

  private async send(command: NativeZoomCommand): Promise<ZoomSessionSnapshot> {
    const response = await this.transport.request(command);
    if (!response.ok) {
      throw new NativeZoomBridgeError(response.error.code, response.error.message);
    }

    if (!("snapshot" in response)) {
      throw new NativeZoomBridgeError("snapshot-failed", "Native Zoom bridge returned a non-snapshot response.");
    }

    const snapshot = mapCaptureSnapshot(response.snapshot);
    this.lastSnapshot = snapshot;
    return snapshot;
  }

  private nextId() {
    this.sequence += 1;
    return `zoom:${this.sequence}`;
  }
}
