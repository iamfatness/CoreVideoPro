import { describe, expect, it } from "vitest";
import { initialParticipants } from "../domain/production";
import { planMultitrackAudio } from "./multitrackAudio";

describe("planMultitrackAudio", () => {
  it("always includes a stereo program master track", () => {
    const plan = planMultitrackAudio(initialParticipants, []);

    expect(plan.tracks).toHaveLength(1);
    expect(plan.tracks[0]).toMatchObject({ id: "master", kind: "master", channels: 2 });
    expect(plan.totalChannels).toBe(2);
    expect(plan.warnings).toHaveLength(0);
  });

  it("adds a mono ISO track per selected participant and totals channels", () => {
    const plan = planMultitrackAudio(initialParticipants, ["p1", "p2"]);

    expect(plan.trackCount).toBe(3);
    expect(plan.totalChannels).toBe(4);
    expect(plan.tracks.filter((track) => track.kind === "iso").map((track) => track.label)).toEqual([
      "Maya Chen ISO",
      "Andre Wallace ISO"
    ]);
    expect(plan.summary).toBe("3 tracks - 4 ch - 0.58 MB/s");
  });

  it("warns when an ISO source is muted", () => {
    const plan = planMultitrackAudio(initialParticipants, ["p7"]);

    expect(plan.tracks.find((track) => track.participantId === "p7")?.muted).toBe(true);
    expect(plan.warnings).toContain("Michael Thompson's ISO track will be silent while muted.");
  });

  it("warns when an ISO id has no matching source", () => {
    const plan = planMultitrackAudio(initialParticipants, ["ghost"]);

    expect(plan.warnings).toContain("ISO track for ghost has no matching source.");
    expect(plan.trackCount).toBe(1);
  });
});
