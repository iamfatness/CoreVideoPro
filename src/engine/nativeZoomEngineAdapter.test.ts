import { describe, expect, it } from "vitest";
import type { RawCaptureSnapshot } from "./captureSnapshotMapper";
import { NativeZoomEngineAdapter } from "./nativeZoomEngineAdapter";
import {
  NativeZoomBridgeError,
  type NativeBridgeCommand,
  type NativeBridgeResponse,
  type NativeZoomResponse,
  type NativeZoomTransport
} from "./nativeBridgeProtocol";

class MemoryNativeZoomTransport implements NativeZoomTransport {
  public readonly commands: NativeBridgeCommand[] = [];
  private failNext: NativeZoomResponse | null = null;
  private tick = 0;
  private meetingState: RawCaptureSnapshot["meetingState"] = "idle";

  failOnce(response: NativeZoomResponse) {
    this.failNext = response;
  }

  async request(command: NativeBridgeCommand): Promise<NativeBridgeResponse> {
    this.commands.push(command);
    if (this.failNext) {
      const response = this.failNext;
      this.failNext = null;
      return response;
    }

    if (command.type === "join") {
      this.meetingState = "in_meeting";
      this.tick = 0;
    } else if (command.type === "leave") {
      this.meetingState = "idle";
    } else {
      this.tick += 1;
    }

    return {
      id: command.id,
      ok: true,
      snapshot: this.buildSnapshot()
    };
  }

  private buildSnapshot(): RawCaptureSnapshot {
    return {
      meetingState: this.meetingState,
      activeSpeakerId: "native-presenter",
      caption: this.meetingState === "in_meeting" ? "Native bridge caption." : "",
      tick: this.tick,
      participants: [
        {
          userId: "native-presenter",
          displayName: "Native Presenter",
          role: "Presenter",
          title: "SDK Feed",
          talking: true,
          sharingScreen: this.meetingState === "in_meeting",
          audioLevel: 82,
          networkQuality: "good"
        }
      ]
    };
  }
}

describe("NativeZoomEngineAdapter", () => {
  it("sends join command and maps native snapshot into app state", async () => {
    const transport = new MemoryNativeZoomTransport();
    const adapter = new NativeZoomEngineAdapter(transport);

    const snapshot = await adapter.join({
      meetingUrl: "https://zoom.us/j/native",
      displayName: "Producer",
      webinar: false
    });

    expect(transport.commands[0]).toMatchObject({
      type: "join",
      payload: {
        meetingUrl: "https://zoom.us/j/native",
        displayName: "Producer",
        webinar: false
      }
    });
    expect(snapshot.meetingState).toBe("in_meeting");
    expect(snapshot.participants[0].name).toBe("Native Presenter");
    expect(snapshot.mediaFrames.find((frame) => frame.kind === "screen-share")?.participantId).toBe("native-presenter");
    expect(adapter.getCachedSnapshot()).toBe(snapshot);
  });

  it("requests fresh snapshots and participants through the transport", async () => {
    const transport = new MemoryNativeZoomTransport();
    const adapter = new NativeZoomEngineAdapter(transport);

    await adapter.join({ meetingUrl: "x", displayName: "Producer", webinar: false });
    const snapshot = await adapter.getSnapshot();
    const participants = await adapter.getParticipants();

    expect(transport.commands.map((command) => command.type)).toEqual(["join", "snapshot", "snapshot"]);
    expect(snapshot.elapsedSeconds).toBe(8);
    expect(participants).toHaveLength(1);
  });

  it("maps leave responses to idle snapshots", async () => {
    const transport = new MemoryNativeZoomTransport();
    const adapter = new NativeZoomEngineAdapter(transport);

    await adapter.join({ meetingUrl: "x", displayName: "Producer", webinar: false });
    const snapshot = await adapter.leave();

    expect(snapshot.meetingState).toBe("idle");
    expect(snapshot.participants).toHaveLength(0);
    expect(snapshot.mediaFrames).toHaveLength(0);
  });

  it("throws typed bridge errors from native failures", async () => {
    const transport = new MemoryNativeZoomTransport();
    const adapter = new NativeZoomEngineAdapter(transport);
    transport.failOnce({
      id: "zoom:1",
      ok: false,
      error: {
        code: "native-unavailable",
        message: "Native process is not running."
      }
    });

    await expect(adapter.getSnapshot()).rejects.toMatchObject({
      name: "NativeZoomBridgeError",
      code: "native-unavailable",
      message: "Native process is not running."
    } satisfies Partial<NativeZoomBridgeError>);
  });
});
