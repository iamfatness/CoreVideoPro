import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import { SimulatedOutputSession } from "./outputSession";

function destinations(ids: string[]) {
  return ids.map((id) => initialProduction.outputDestinations.find((destination) => destination.id === id)!);
}

describe("SimulatedOutputSession", () => {
  it("starts idle with safe output health", () => {
    const session = new SimulatedOutputSession();
    const state = session.getSession();

    expect(state.recording).toBe(false);
    expect(state.streaming).toBe(false);
    expect(state.health.bitrateMbps).toBe(0);
    expect(state.statusText).toBe("Outputs idle");
    expect(Object.isFrozen(state)).toBe(true);
  });

  it("tracks recording state and recording file", () => {
    const session = new SimulatedOutputSession();
    const started = session.startRecording({
      ...initialProduction.recordingSettings,
      filenamePrefix: "Weekly_Show",
      format: "mov"
    });

    expect(started.recording).toBe(true);
    expect(started.recordingFile).toBe("Recordings/CoreVideo Pro/Weekly_Show.mov");
    expect(started.recordingSettings?.quality).toBe("high");
    expect(started.statusText).toContain("Recording program + 2 ISOs locally");

    const stopped = session.stopRecording();
    expect(stopped.recording).toBe(false);
    expect(stopped.recordingFile).toBe("Recordings/CoreVideo Pro/Weekly_Show.mov");
  });

  it("uses selected output profile for recording and streaming health", () => {
    const session = new SimulatedOutputSession();
    const profiled = session.setOutputProfile({
      id: "4k30",
      name: "4K 30",
      resolution: "3840x2160",
      fps: 30,
      targetBitrateMbps: 18
    });
    const streaming = session.startStream(destinations(["custom-rtmp"]));

    expect(profiled.health.resolution).toBe("3840x2160");
    expect(streaming.health.fps).toBe(30);
    expect(streaming.health.bitrateMbps).toBe(18);
    expect(streaming.statusText).toContain("18 Mbps");
  });

  it("tracks stream target, bitrate, and combined output status", () => {
    const session = new SimulatedOutputSession();

    session.startRecording(initialProduction.recordingSettings);
    const streaming = session.startStream(destinations(["youtube-primary", "custom-rtmp"]));

    expect(streaming.recording).toBe(true);
    expect(streaming.streaming).toBe(true);
    expect(streaming.streamDestinationId).toBe("youtube-primary");
    expect(streaming.activeDestinationIds).toEqual(["youtube-primary", "custom-rtmp"]);
    expect(streaming.destinations.filter((destination) => destination.active)).toHaveLength(2);
    expect(streaming.health.bitrateMbps).toBeGreaterThan(0);
    expect(streaming.statusText).toContain("Recording program + 2 ISOs and streaming to 2 destinations");
  });

  it("warns when several network destinations are active", () => {
    const session = new SimulatedOutputSession();
    const streaming = session.startStream(destinations(["custom-rtmp", "youtube-primary", "srt-backup"]));

    expect(streaming.health.network).toBe("warning");
    expect(streaming.destinations.find((destination) => destination.id === "srt-backup")?.health).toBe("warning");
  });

  it("preserves editable destination endpoint details in live session state", () => {
    const session = new SimulatedOutputSession();
    const customDestination = {
      ...initialProduction.outputDestinations.find((destination) => destination.id === "custom-rtmp")!,
      endpoint: "rtmp://studio.example.com/live",
      streamKey: "updated-key"
    };

    const streaming = session.startStream([customDestination]);

    expect(streaming.activeDestinationIds).toEqual(["custom-rtmp"]);
    expect(streaming.destinations.find((destination) => destination.id === "custom-rtmp")).toMatchObject({
      endpoint: "rtmp://studio.example.com/live",
      streamKey: "updated-key",
      active: true
    });
  });

  it("increments elapsed time only while outputs are active", () => {
    const session = new SimulatedOutputSession();
    const idle = session.getSession();
    const recording = session.startRecording(initialProduction.recordingSettings);
    const later = session.getSession();

    expect(idle.elapsedSeconds).toBe(0);
    expect(recording.elapsedSeconds).toBeGreaterThan(0);
    expect(later.elapsedSeconds).toBeGreaterThan(recording.elapsedSeconds);
  });
});
