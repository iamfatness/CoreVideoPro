import { describe, expect, it } from "vitest";
import { initialProduction } from "../domain/production";
import { runOutputPreflight, runRecordingPreflight } from "./outputPreflight";

describe("output preflight", () => {
  it("passes for valid armed destinations", () => {
    const result = runOutputPreflight(initialProduction.outputDestinations);

    expect(result.ready).toBe(true);
    expect(result.destinations.map((destination) => destination.id)).toEqual(["custom-rtmp", "youtube-primary"]);
    expect(result.summary).toBe("Ready to stream to 2 destinations.");
  });

  it("blocks streaming when no destination is armed", () => {
    const result = runOutputPreflight(initialProduction.outputDestinations.map((destination) => ({ ...destination, enabled: false })));

    expect(result.ready).toBe(false);
    expect(result.errors).toEqual(["Arm at least one destination before streaming."]);
  });

  it("requires RTMP endpoints and stream keys", () => {
    const result = runOutputPreflight([
      {
        ...initialProduction.outputDestinations.find((destination) => destination.id === "custom-rtmp")!,
        endpoint: "https://not-rtmp.example.com/live",
        streamKey: ""
      }
    ]);

    expect(result.ready).toBe(false);
    expect(result.errors).toEqual([
      "Custom RTMP needs a stream key.",
      "Custom RTMP endpoint must start with rtmp:// or rtmps://."
    ]);
  });

  it("warns when several network destinations are armed", () => {
    const result = runOutputPreflight(
      initialProduction.outputDestinations.map((destination) =>
        ["custom-rtmp", "youtube-primary", "srt-backup"].includes(destination.id) ? { ...destination, enabled: true } : destination
      )
    );

    expect(result.ready).toBe(true);
    expect(result.warnings).toEqual(["Three or more network destinations may increase encoder and network load."]);
  });

  it("passes valid recording settings", () => {
    const result = runRecordingPreflight(initialProduction.recordingSettings);

    expect(result.ready).toBe(true);
    expect(result.summary).toBe("Ready to record MP4 at high quality with 2 ISO feeds.");
  });

  it("warns when several ISO feeds are selected", () => {
    const result = runRecordingPreflight({
      ...initialProduction.recordingSettings,
      isoParticipantIds: ["p1", "p2", "p3", "p4", "p5"]
    });

    expect(result.ready).toBe(true);
    expect(result.warnings).toEqual(["Recording more than 4 ISO feeds may increase disk and encoder load."]);
  });

  it("blocks recording when folder or filename is missing", () => {
    const result = runRecordingPreflight({
      ...initialProduction.recordingSettings,
      folder: "",
      filenamePrefix: ""
    });

    expect(result.ready).toBe(false);
    expect(result.errors).toEqual([
      "Choose a recording folder before recording.",
      "Enter a recording filename before recording."
    ]);
  });

  it("blocks unsafe recording paths and filenames", () => {
    const result = runRecordingPreflight({
      ...initialProduction.recordingSettings,
      folder: "../Recordings",
      filenamePrefix: "Client:Panel"
    });

    expect(result.ready).toBe(false);
    expect(result.errors).toEqual([
      "Recording filename cannot contain < > : \" | ? * characters.",
      "Recording folder cannot include parent-directory traversal."
    ]);
  });
});
