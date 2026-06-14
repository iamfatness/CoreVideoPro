import { describe, expect, it } from "vitest";
import { initialProduction, type OutputHealth, type OutputProfile, type OutputSessionState } from "../domain/production";
import { NativeOutputEngineAdapter } from "./nativeOutputEngineAdapter";
import { NativeZoomBridgeError, type NativeBridgeCommand, type NativeBridgeResponse, type NativeZoomTransport } from "./nativeBridgeProtocol";
import { SimulatedOutputSession } from "./outputSession";

class MemoryNativeOutputTransport implements NativeZoomTransport {
  public readonly commands: NativeBridgeCommand[] = [];
  private readonly session = new SimulatedOutputSession();
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
      case "set-output-profile":
        return this.sessionResponse(command.id, this.session.setOutputProfile(command.payload));
      case "start-recording":
        return this.sessionResponse(command.id, this.session.startRecording(command.payload));
      case "stop-recording":
        return this.sessionResponse(command.id, this.session.stopRecording());
      case "start-stream":
        return this.sessionResponse(command.id, this.session.startStream(command.payload.destinations));
      case "stop-stream":
        return this.sessionResponse(command.id, this.session.stopStream());
      case "get-output-health":
        return { id: command.id, ok: true, health: this.session.getHealth() };
      case "get-output-session":
        return this.sessionResponse(command.id, this.session.getSession());
      default:
        return {
          id: command.id,
          ok: false,
          error: {
            code: "native-unavailable",
            message: "Output transport received a Zoom capture command."
          }
        };
    }
  }

  private sessionResponse(id: string, session: OutputSessionState): NativeBridgeResponse {
    return { id, ok: true, session };
  }
}

describe("NativeOutputEngineAdapter", () => {
  it("sends recording and streaming commands through the native bridge", async () => {
    const transport = new MemoryNativeOutputTransport();
    const adapter = new NativeOutputEngineAdapter(transport);

    const recordingSession = await adapter.startRecording(initialProduction.recordingSettings);
    const streamSession = await adapter.startStream(
      initialProduction.outputDestinations.filter((destination) => ["youtube-primary", "ndi-program"].includes(destination.id))
    );
    const idleSession = await adapter.stopStream();

    expect(transport.commands.map((command) => command.type)).toEqual(["start-recording", "start-stream", "stop-stream"]);
    expect(recordingSession.recording).toBe(true);
    expect((transport.commands[0] as Extract<NativeBridgeCommand, { type: "start-recording" }>).payload).toEqual(initialProduction.recordingSettings);
    expect(streamSession.streaming).toBe(true);
    expect(streamSession.activeDestinationIds).toEqual(["youtube-primary", "ndi-program"]);
    expect((transport.commands[1] as Extract<NativeBridgeCommand, { type: "start-stream" }>).payload.destinations[0].endpoint).toContain("youtube");
    expect(idleSession.streaming).toBe(false);
    expect(idleSession.recording).toBe(true);
  });

  it("sets output profile and exposes matching health", async () => {
    const transport = new MemoryNativeOutputTransport();
    const adapter = new NativeOutputEngineAdapter(transport);
    const profile = initialProduction.outputProfiles.find((item) => item.id === "4k30") as OutputProfile;

    const session = await adapter.setOutputProfile(profile);
    await adapter.startStream(initialProduction.outputDestinations.filter((destination) => destination.id === "custom-rtmp"));
    const health = await adapter.getHealth();

    expect(transport.commands.map((command) => command.type)).toEqual(["set-output-profile", "start-stream", "get-output-health"]);
    expect(session.health.resolution).toBe("3840x2160");
    expect(health).toMatchObject({
      resolution: "3840x2160",
      fps: 30,
      bitrateMbps: 18
    } satisfies Partial<OutputHealth>);
  });

  it("gets the current output session", async () => {
    const transport = new MemoryNativeOutputTransport();
    const adapter = new NativeOutputEngineAdapter(transport);

    await adapter.startRecording(initialProduction.recordingSettings);
    const session = await adapter.getSession();

    expect(transport.commands.map((command) => command.type)).toEqual(["start-recording", "get-output-session"]);
    expect(session.recording).toBe(true);
    expect(session.statusText).toContain("Recording program + 2 ISOs locally");
  });

  it("throws typed bridge errors from native output failures", async () => {
    const transport = new MemoryNativeOutputTransport();
    const adapter = new NativeOutputEngineAdapter(transport);
    transport.failOnce({
      id: "output:1",
      ok: false,
      error: {
        code: "stream-failed",
        message: "Encoder could not reserve the selected destination."
      }
    });

    await expect(adapter.startStream(initialProduction.outputDestinations.filter((destination) => destination.id === "youtube-primary"))).rejects.toMatchObject({
      name: "NativeZoomBridgeError",
      code: "stream-failed",
      message: "Encoder could not reserve the selected destination."
    } satisfies Partial<NativeZoomBridgeError>);
  });
});
