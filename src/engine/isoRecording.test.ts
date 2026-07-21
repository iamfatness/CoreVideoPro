import { describe, expect, it } from "vitest";
import {
  estimateIsoTrackBitrateMbps,
  isoOutputPath,
  isoTrackLabel,
  planIsoRecording,
  summarizeIsoPlan,
  validateIsoAgainstDisk,
} from "./isoRecording";

const participants = [
  { id: "p-1", name: "Alice" },
  { id: "p-2", name: "Bob" },
  { id: "p-3", name: "Carol" },
];

describe("estimateIsoTrackBitrateMbps", () => {
  it("returns 6 Mbps for a 1080p participant track", () => {
    expect(estimateIsoTrackBitrateMbps("participant", "1080p")).toBe(6);
  });

  it("returns 4 Mbps for a 720p participant track", () => {
    expect(estimateIsoTrackBitrateMbps("participant", "720p")).toBe(4);
  });

  it("returns 8 Mbps for screen share regardless of tier", () => {
    expect(estimateIsoTrackBitrateMbps("screen-share")).toBe(8);
  });

  it("returns 9 Mbps for a 1080p program mix (1.5x participant rate)", () => {
    expect(estimateIsoTrackBitrateMbps("program-mix", "1080p")).toBe(9);
  });

  it("returns 1 Mbps for ambient", () => {
    expect(estimateIsoTrackBitrateMbps("ambient")).toBe(1);
  });
});

describe("isoTrackLabel", () => {
  it("uses participant name when provided", () => {
    expect(isoTrackLabel("participant", "Alice")).toBe("Alice");
  });

  it("falls back to 'Participant' when name is absent", () => {
    expect(isoTrackLabel("participant")).toBe("Participant");
  });

  it("returns fixed labels for mix, screen-share, and ambient", () => {
    expect(isoTrackLabel("program-mix")).toBe("Program Mix");
    expect(isoTrackLabel("screen-share")).toBe("Screen Share");
    expect(isoTrackLabel("ambient")).toBe("Ambient");
  });
});

describe("planIsoRecording", () => {
  it("assigns a track per participant plus a program mix by default", () => {
    const plan = planIsoRecording(participants);
    // 3 participants + 1 program mix = 4 tracks
    expect(plan.tracks).toHaveLength(4);
    expect(plan.tracks[0].source).toBe("participant");
    expect(plan.tracks[0].participantId).toBe("p-1");
    expect(plan.tracks[3].source).toBe("program-mix");
  });

  it("assigns sequential track indices", () => {
    const plan = planIsoRecording(participants);
    expect(plan.tracks.map((t) => t.trackIndex)).toEqual([0, 1, 2, 3]);
  });

  it("omits program mix when includeProgramMix is false", () => {
    const plan = planIsoRecording(participants, { includeProgramMix: false });
    expect(plan.tracks).toHaveLength(3);
    expect(plan.tracks.every((t) => t.source !== "program-mix")).toBe(true);
  });

  it("adds a screen-share track when requested", () => {
    const plan = planIsoRecording(participants, { includeScreenShare: true });
    const ssTrack = plan.tracks.find((t) => t.source === "screen-share");
    expect(ssTrack).toBeDefined();
    expect(ssTrack?.estimatedBitrateMbps).toBe(8);
  });

  it("caps participant tracks at maxParticipantTracks and warns", () => {
    const many = Array.from({ length: 10 }, (_, i) => ({ id: `p-${i}`, name: `P${i}` }));
    const plan = planIsoRecording(many, { maxParticipantTracks: 4 });
    const participantTracks = plan.tracks.filter((t) => t.source === "participant");
    expect(participantTracks).toHaveLength(4);
    expect(plan.warnings.some((w) => w.includes("6 participant(s) dropped"))).toBe(true);
  });

  it("is valid for non-empty participant list", () => {
    expect(planIsoRecording(participants).valid).toBe(true);
  });

  it("is invalid with no participants and no extra tracks", () => {
    const plan = planIsoRecording([], { includeProgramMix: false });
    expect(plan.valid).toBe(false);
    expect(plan.warnings.some((w) => w.includes("No tracks"))).toBe(true);
  });

  it("warns when combined bitrate exceeds 80 Mbps", () => {
    const many = Array.from({ length: 14 }, (_, i) => ({ id: `p-${i}`, name: `P${i}` }));
    const plan = planIsoRecording(many, { maxParticipantTracks: 14 });
    expect(plan.warnings.some((w) => w.includes("Mbps may exceed"))).toBe(true);
  });

  it("computes estimatedTotalMbps as sum of all track bitrates", () => {
    const plan = planIsoRecording([participants[0]], { includeProgramMix: false });
    expect(plan.estimatedTotalMbps).toBe(6);
  });

  it("builds estimatedFileSizeGbPerHour from total bitrate", () => {
    const plan = planIsoRecording([participants[0]], { includeProgramMix: false });
    // 6 Mbps * 1e6 * 3600 / (8 * 1e9) ≈ 2.7 GB/hr
    expect(plan.estimatedFileSizeGbPerHour).toBeCloseTo(2.7, 0);
  });

  it("uses session name to build output folder slug", () => {
    const plan = planIsoRecording(participants, { sessionName: "Weekly Show" });
    expect(plan.outputFolder).toBe("recordings/weekly-show");
  });

  it("applies 720p bitrate when participantResolution is 720p", () => {
    const plan = planIsoRecording([participants[0]], {
      includeProgramMix: false,
      participantResolution: "720p",
    });
    expect(plan.tracks[0].estimatedBitrateMbps).toBe(4);
    expect(plan.tracks[0].resolutionTier).toBe("720p");
  });
});

describe("validateIsoAgainstDisk", () => {
  it("returns no errors when disk space is ample", () => {
    const plan = planIsoRecording(participants);
    expect(validateIsoAgainstDisk(plan, 500, 60)).toHaveLength(0);
  });

  it("returns an error when disk space is insufficient", () => {
    const plan = planIsoRecording(participants);
    const errors = validateIsoAgainstDisk(plan, 1, 60);
    expect(errors.some((e) => e.includes("only 1.0 GB available"))).toBe(true);
  });

  it("warns when recording will consume more than 80% of disk", () => {
    const plan = planIsoRecording([participants[0]], { includeProgramMix: false });
    // 6 Mbps → ~2.7 GB/hr; 60 min → ~2.7 GB; disk = 3.2 GB → ~84% used (>80% threshold)
    const errors = validateIsoAgainstDisk(plan, 3.2, 60);
    expect(errors.some((e) => e.includes("% of available disk"))).toBe(true);
  });
});

describe("isoOutputPath", () => {
  it("builds a zero-padded ISO track path (spec §5 scheme)", () => {
    expect(isoOutputPath("recordings/show", "Alice", 0, "participant")).toBe(
      "recordings/show/ISO-01-Alice.mp4"
    );
    expect(isoOutputPath("recordings/show", "Bob Jones", 9, "participant")).toBe(
      "recordings/show/ISO-10-Bob-Jones.mp4"
    );
  });

  it("names the program mix Program.mp4", () => {
    expect(isoOutputPath("recordings/show", "Program Mix", 9, "program-mix")).toBe(
      "recordings/show/Program.mp4"
    );
  });

  it("sanitizes label with spaces and special chars, preserving case", () => {
    expect(isoOutputPath("recordings/show", "Screen Share!", 2, "screen-share")).toBe(
      "recordings/show/ISO-03-Screen-Share.mp4"
    );
  });
});

describe("summarizeIsoPlan", () => {
  it("summarises track count, bitrate and file size", () => {
    const plan = planIsoRecording(participants);
    const summary = summarizeIsoPlan(plan);
    expect(summary).toMatch(/4 ISO tracks/);
    expect(summary).toMatch(/Mbps/);
    expect(summary).toMatch(/GB\/hr/);
  });

  it("returns a short message for an invalid plan", () => {
    const plan = planIsoRecording([], { includeProgramMix: false });
    expect(summarizeIsoPlan(plan)).toBe("No ISO tracks configured");
  });
});
