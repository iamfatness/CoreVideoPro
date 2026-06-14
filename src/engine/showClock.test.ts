import { describe, expect, it } from "vitest";
import {
  buildRundownFromScenes,
  computeShowClock,
  formatClock,
  suggestShowDurationMinutes,
  type RundownSegment,
} from "./showClock";

const segments: RundownSegment[] = [
  { id: "seg-1", name: "Intro", plannedSeconds: 300 },
  { id: "seg-2", name: "Main presentation", plannedSeconds: 1800 },
  { id: "seg-3", name: "Q&A", plannedSeconds: 900 }
];

describe("formatClock", () => {
  it("formats seconds as mm:ss", () => {
    expect(formatClock(0)).toBe("00:00");
    expect(formatClock(65)).toBe("01:05");
    expect(formatClock(3600)).toBe("1:00:00");
    expect(formatClock(3661)).toBe("1:01:01");
  });

  it("formats remaining time with sign (negative = over)", () => {
    expect(formatClock(120, true)).toBe("-02:00");
    expect(formatClock(-60, true)).toBe("+01:00");
    expect(formatClock(0, true)).toBe("-00:00");
  });
});

describe("computeShowClock", () => {
  it("returns not-started state when index is -1", () => {
    const state = computeShowClock(segments, -1, 0, 0);
    expect(state.elapsedSeconds).toBe(0);
    expect(state.elapsedLabel).toBe("00:00");
    expect(state.activeSegmentIndex).toBe(-1);
    expect(state.segments.every((s) => s.status === "upcoming")).toBe(true);
  });

  it("computes planned total from segments", () => {
    const state = computeShowClock(segments, 0, 0, 0);
    expect(state.totalPlannedSeconds).toBe(3000);
  });

  it("marks completed segments as done", () => {
    const state = computeShowClock(segments, 1, 120, 420);
    expect(state.segments[0].status).toBe("done");
    expect(state.segments[1].status).toBe("running");
    expect(state.segments[2].status).toBe("upcoming");
  });

  it("marks active segment as over when elapsed exceeds planned", () => {
    const state = computeShowClock(segments, 0, 400, 400);
    expect(state.segments[0].status).toBe("over");
    expect(state.segments[0].remainingSeconds).toBe(-100);
  });

  it("shows positive remaining when under time", () => {
    const state = computeShowClock(segments, 1, 600, 900);
    const active = state.segments[1];
    expect(active.remainingSeconds).toBe(1200);
    expect(active.progress).toBeCloseTo(0.333, 2);
  });

  it("marks show as over when total elapsed exceeds planned", () => {
    const state = computeShowClock(segments, 2, 950, 3050);
    expect(state.showOver).toBe(true);
    expect(state.remainingSeconds).toBe(-50);
    expect(state.remainingLabel).toContain("+");
  });

  it("formats elapsed and remaining labels", () => {
    const state = computeShowClock(segments, 1, 300, 600);
    expect(state.elapsedLabel).toBe("10:00");
    expect(state.remainingLabel).toBe("-40:00");
  });
});

describe("buildRundownFromScenes", () => {
  it("returns empty for no scenes", () => {
    expect(buildRundownFromScenes([], 60)).toHaveLength(0);
  });

  it("creates equal-duration segments summing to total", () => {
    const rundown = buildRundownFromScenes(["Intro", "Main", "Q&A"], 60);
    expect(rundown).toHaveLength(3);
    expect(rundown[0].plannedSeconds).toBe(1200);
    const total = rundown.reduce((sum, s) => sum + s.plannedSeconds, 0);
    expect(total).toBe(3600);
  });

  it("assigns scene names and sequential ids", () => {
    const rundown = buildRundownFromScenes(["Intro", "Closing"], 30);
    expect(rundown[0].id).toBe("seg-1");
    expect(rundown[0].name).toBe("Intro");
    expect(rundown[1].id).toBe("seg-2");
    expect(rundown[1].name).toBe("Closing");
  });
});

describe("suggestShowDurationMinutes", () => {
  it("suggests 30 min for small shows", () => {
    expect(suggestShowDurationMinutes(2)).toBe(30);
    expect(suggestShowDurationMinutes(3)).toBe(30);
  });

  it("suggests 45 min for medium shows", () => {
    expect(suggestShowDurationMinutes(5)).toBe(45);
  });

  it("suggests 60 min for standard shows", () => {
    expect(suggestShowDurationMinutes(8)).toBe(60);
  });

  it("suggests 90 min for large shows", () => {
    expect(suggestShowDurationMinutes(12)).toBe(90);
  });
});
