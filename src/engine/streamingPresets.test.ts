import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import {
  addDestinationFromPreset,
  createDestinationFromPreset,
  getStreamingPreset,
  streamingPresets
} from "./streamingPresets";

describe("streamingPresets catalog", () => {
  it("includes Twitch and a custom RTMP preset with ingest endpoints", () => {
    const twitch = getStreamingPreset("twitch");
    const custom = getStreamingPreset("custom-rtmp");

    expect(twitch?.endpoint).toBe("rtmp://live.twitch.tv/app");
    expect(twitch?.requiresStreamKey).toBe(true);
    expect(custom?.endpoint).toBe("rtmp://");
    expect(streamingPresets.length).toBeGreaterThanOrEqual(4);
  });
});

describe("createDestinationFromPreset", () => {
  it("creates an armed destination with a blank stream key from the preset", () => {
    const preset = getStreamingPreset("twitch")!;
    const destination = createDestinationFromPreset(preset, []);

    expect(destination.id).toBe("twitch");
    expect(destination.name).toBe("Twitch");
    expect(destination.enabled).toBe(true);
    expect(destination.endpoint).toBe("rtmp://live.twitch.tv/app");
    expect(destination.streamKey).toBe("");
  });

  it("numbers a second destination from the same preset family", () => {
    const preset = getStreamingPreset("twitch")!;
    const first = createDestinationFromPreset(preset, []);
    const second = createDestinationFromPreset(preset, [first]);

    expect(second.id).toBe("twitch-2");
    expect(second.name).toBe("Twitch 2");
  });
});

describe("addDestinationFromPreset", () => {
  it("appends a Twitch destination to the existing list", () => {
    const next = addDestinationFromPreset("twitch", initialProduction.outputDestinations);

    expect(next.length).toBe(initialProduction.outputDestinations.length + 1);
    expect(next.at(-1)?.name).toBe("Twitch");
  });

  it("ignores unknown preset ids", () => {
    const next = addDestinationFromPreset("nope", initialProduction.outputDestinations);
    expect(next).toBe(initialProduction.outputDestinations);
  });
});
