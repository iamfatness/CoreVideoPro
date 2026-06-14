import type { OutputDestination, OutputHealth, OutputProfile, OutputSessionState, RecordingSettings } from "../domain/production";
import type { OutputEngine } from "./contracts";
import { NativeZoomBridgeError, type NativeOutputCommand, type NativeZoomTransport } from "./nativeBridgeProtocol";

export class NativeOutputEngineAdapter implements OutputEngine {
  private sequence = 0;

  constructor(private readonly transport: NativeZoomTransport) {}

  async setOutputProfile(profile: OutputProfile): Promise<OutputSessionState> {
    return this.sendForSession({ id: this.nextId(), type: "set-output-profile", payload: profile }, "output-profile-failed");
  }

  async startRecording(settings: RecordingSettings): Promise<OutputSessionState> {
    return this.sendForSession({ id: this.nextId(), type: "start-recording", payload: settings }, "recording-failed");
  }

  async stopRecording(): Promise<OutputSessionState> {
    return this.sendForSession({ id: this.nextId(), type: "stop-recording" }, "recording-failed");
  }

  async startStream(destinations: OutputDestination[]): Promise<OutputSessionState> {
    return this.sendForSession({ id: this.nextId(), type: "start-stream", payload: { destinations } }, "stream-failed");
  }

  async stopStream(): Promise<OutputSessionState> {
    return this.sendForSession({ id: this.nextId(), type: "stop-stream" }, "stream-failed");
  }

  async getHealth(): Promise<OutputHealth> {
    const response = await this.transport.request({ id: this.nextId(), type: "get-output-health" });
    if (!response.ok) {
      throw new NativeZoomBridgeError(response.error.code, response.error.message);
    }

    if (!("health" in response)) {
      throw new NativeZoomBridgeError("protocol-error", "Native output bridge returned a non-health response.");
    }

    return response.health;
  }

  async getSession(): Promise<OutputSessionState> {
    return this.sendForSession({ id: this.nextId(), type: "get-output-session" }, "output-session-failed");
  }

  private async sendForSession(
    command: NativeOutputCommand,
    fallbackCode: "output-profile-failed" | "recording-failed" | "stream-failed" | "output-session-failed"
  ): Promise<OutputSessionState> {
    const response = await this.transport.request(command);
    if (!response.ok) {
      throw new NativeZoomBridgeError(response.error.code, response.error.message);
    }

    if (!("session" in response)) {
      throw new NativeZoomBridgeError(fallbackCode, "Native output bridge returned a non-session response.");
    }

    return response.session;
  }

  private nextId() {
    this.sequence += 1;
    return `output:${this.sequence}`;
  }
}
