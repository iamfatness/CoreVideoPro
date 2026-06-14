import { describe, expect, it } from "vitest";
import {
  addSpeaker,
  buildSpeakerReport,
  computeBalanceScore,
  computeSpeakerAlerts,
  createSpeakerTimer,
  dominantSpeaker,
  formatSec,
  resetSpeakerTimer,
  setActiveSpeaker,
  snapshotTotals,
  speakerSharePct,
} from "./speakerTimer";

const PARTICIPANTS = [
  { id: "p1", name: "Alice" },
  { id: "p2", name: "Bob" },
  { id: "p3", name: "Carol" },
];

describe("createSpeakerTimer", () => {
  it("initialises with all speakers at zero", () => {
    const state = createSpeakerTimer(PARTICIPANTS);
    expect(state.speakers).toHaveLength(3);
    expect(state.speakers.every((s) => s.totalSec === 0)).toBe(true);
    expect(state.speakers.every((s) => !s.isSpeaking)).toBe(true);
  });

  it("sets targetPerSpeakerSec when provided", () => {
    const state = createSpeakerTimer(PARTICIPANTS, 120);
    expect(state.targetPerSpeakerSec).toBe(120);
  });

  it("defaults targetPerSpeakerSec to 0 (no limit)", () => {
    const state = createSpeakerTimer(PARTICIPANTS);
    expect(state.targetPerSpeakerSec).toBe(0);
  });
});

describe("setActiveSpeaker", () => {
  it("marks a speaker as active", () => {
    const state = createSpeakerTimer(PARTICIPANTS);
    const next = setActiveSpeaker(state, 1000, "p1");
    const p1 = next.speakers.find((s) => s.participantId === "p1");
    expect(p1?.isSpeaking).toBe(true);
    expect(p1?.turnStartedAt).toBe(1000);
  });

  it("flushes previous speaker when switching", () => {
    let state = createSpeakerTimer(PARTICIPANTS);
    state = setActiveSpeaker(state, 1000, "p1");
    state = setActiveSpeaker(state, 1030, "p2"); // p1 spoke for 30s
    const p1 = state.speakers.find((s) => s.participantId === "p1");
    expect(p1?.totalSec).toBe(30);
    expect(p1?.isSpeaking).toBe(false);
  });

  it("marks all as not speaking when speakingId is null", () => {
    let state = createSpeakerTimer(PARTICIPANTS);
    state = setActiveSpeaker(state, 1000, "p1");
    state = setActiveSpeaker(state, 1010, null);
    expect(state.speakers.every((s) => !s.isSpeaking)).toBe(true);
  });

  it("accumulates time over multiple turns", () => {
    let state = createSpeakerTimer(PARTICIPANTS);
    state = setActiveSpeaker(state, 0, "p1");
    state = setActiveSpeaker(state, 10, "p2");
    state = setActiveSpeaker(state, 25, "p1"); // p1 again
    state = setActiveSpeaker(state, 40, null); // p1 spoke 15 more seconds
    const p1 = state.speakers.find((s) => s.participantId === "p1");
    expect(p1?.totalSec).toBe(25); // 10 + 15
  });
});

describe("snapshotTotals", () => {
  it("adds in-progress turn time to total without ending the turn", () => {
    let state = createSpeakerTimer(PARTICIPANTS);
    state = setActiveSpeaker(state, 0, "p1");
    const snap = snapshotTotals(state, 20);
    const p1 = snap.speakers.find((s) => s.participantId === "p1");
    expect(p1?.totalSec).toBe(20);
    expect(p1?.isSpeaking).toBe(true); // still speaking
  });

  it("does not change non-speaking speakers", () => {
    const state = createSpeakerTimer(PARTICIPANTS);
    const snap = snapshotTotals(state, 100);
    expect(snap.speakers.every((s) => s.totalSec === 0)).toBe(true);
  });
});

describe("resetSpeakerTimer", () => {
  it("zeros all timers", () => {
    let state = createSpeakerTimer(PARTICIPANTS);
    state = setActiveSpeaker(state, 0, "p1");
    state = setActiveSpeaker(state, 30, null);
    const reset = resetSpeakerTimer(state);
    expect(reset.speakers.every((s) => s.totalSec === 0)).toBe(true);
  });

  it("stops any in-progress turns", () => {
    let state = createSpeakerTimer(PARTICIPANTS);
    state = setActiveSpeaker(state, 0, "p1");
    const reset = resetSpeakerTimer(state);
    expect(reset.speakers.every((s) => !s.isSpeaking)).toBe(true);
  });
});

describe("addSpeaker", () => {
  it("adds a new speaker", () => {
    const state = createSpeakerTimer(PARTICIPANTS);
    const next = addSpeaker(state, { id: "p4", name: "Dave" });
    expect(next.speakers).toHaveLength(4);
    expect(next.speakers.find((s) => s.participantId === "p4")).toBeTruthy();
  });

  it("does not duplicate an existing speaker", () => {
    const state = createSpeakerTimer(PARTICIPANTS);
    const next = addSpeaker(state, { id: "p1", name: "Alice" });
    expect(next.speakers).toHaveLength(3);
  });
});

describe("computeSpeakerAlerts", () => {
  it("returns empty array when no target set", () => {
    const state = createSpeakerTimer(PARTICIPANTS, 0);
    expect(computeSpeakerAlerts(state)).toHaveLength(0);
  });

  it("warns when speaker is at 80–99% of target", () => {
    let state = createSpeakerTimer(PARTICIPANTS, 100);
    state = setActiveSpeaker(state, 0, "p1");
    state = setActiveSpeaker(state, 85, null); // p1 spoke 85s (85% of 100s target)
    const alerts = computeSpeakerAlerts(state);
    expect(alerts.find((a) => a.participantId === "p1")?.severity).toBe("warning");
  });

  it("flags over-time when speaker exceeds target", () => {
    let state = createSpeakerTimer(PARTICIPANTS, 60);
    state = setActiveSpeaker(state, 0, "p1");
    state = setActiveSpeaker(state, 75, null); // 75 > 60
    const alerts = computeSpeakerAlerts(state);
    expect(alerts.find((a) => a.participantId === "p1")?.severity).toBe("over-time");
  });

  it("alert message contains speaker name", () => {
    let state = createSpeakerTimer(PARTICIPANTS, 60);
    state = setActiveSpeaker(state, 0, "p1");
    state = setActiveSpeaker(state, 65, null);
    const alerts = computeSpeakerAlerts(state);
    expect(alerts[0].message).toContain("Alice");
  });
});

describe("computeBalanceScore", () => {
  it("returns 100 for perfectly equal distribution", () => {
    let state = createSpeakerTimer([
      { id: "p1", name: "Alice" },
      { id: "p2", name: "Bob" },
    ]);
    state = setActiveSpeaker(state, 0, "p1");
    state = setActiveSpeaker(state, 30, "p2");
    state = setActiveSpeaker(state, 60, null);
    expect(computeBalanceScore(state)).toBe(100);
  });

  it("returns 0 when one speaker has all the time", () => {
    let state = createSpeakerTimer([
      { id: "p1", name: "Alice" },
      { id: "p2", name: "Bob" },
    ]);
    state = setActiveSpeaker(state, 0, "p1");
    state = setActiveSpeaker(state, 60, null);
    expect(computeBalanceScore(state)).toBe(0);
  });

  it("returns 100 when no one has spoken", () => {
    const state = createSpeakerTimer(PARTICIPANTS);
    expect(computeBalanceScore(state)).toBe(100);
  });

  it("returns 100 for a single speaker", () => {
    const state = createSpeakerTimer([{ id: "p1", name: "Alice" }]);
    expect(computeBalanceScore(state)).toBe(100);
  });
});

describe("dominantSpeaker", () => {
  it("returns the speaker with most time", () => {
    let state = createSpeakerTimer(PARTICIPANTS);
    state = setActiveSpeaker(state, 0, "p2");
    state = setActiveSpeaker(state, 60, null);
    expect(dominantSpeaker(state)?.participantId).toBe("p2");
  });

  it("returns null for empty speaker list", () => {
    const state = createSpeakerTimer([]);
    expect(dominantSpeaker(state)).toBeNull();
  });
});

describe("buildSpeakerReport", () => {
  it("returns no-time summary when all speakers at zero", () => {
    const state = createSpeakerTimer(PARTICIPANTS);
    const report = buildSpeakerReport(state);
    expect(report.summary).toContain("No speaking time");
  });

  it("includes dominant speaker name in summary", () => {
    let state = createSpeakerTimer(PARTICIPANTS);
    state = setActiveSpeaker(state, 0, "p1");
    state = setActiveSpeaker(state, 45, null);
    const report = buildSpeakerReport(state);
    expect(report.summary).toContain("Alice");
  });

  it("returns balanceScore between 0 and 100", () => {
    const state = createSpeakerTimer(PARTICIPANTS);
    const report = buildSpeakerReport(state);
    expect(report.balanceScore).toBeGreaterThanOrEqual(0);
    expect(report.balanceScore).toBeLessThanOrEqual(100);
  });
});

describe("formatSec", () => {
  it("formats under 60 seconds as Ns", () => {
    expect(formatSec(45)).toBe("45s");
  });

  it("formats 60+ seconds as Nm Ns", () => {
    expect(formatSec(90)).toBe("1m 30s");
  });

  it("handles negative values (absolute)", () => {
    expect(formatSec(-30)).toBe("30s");
  });
});

describe("speakerSharePct", () => {
  it("returns 0 when no speaking time", () => {
    const state = createSpeakerTimer(PARTICIPANTS);
    expect(speakerSharePct(state.speakers[0], state.speakers)).toBe(0);
  });

  it("returns 100 when speaker has all the time", () => {
    let state = createSpeakerTimer(PARTICIPANTS);
    state = setActiveSpeaker(state, 0, "p1");
    state = setActiveSpeaker(state, 60, null);
    expect(speakerSharePct(state.speakers[0], state.speakers)).toBe(100);
  });

  it("returns approximate share for two equal speakers", () => {
    let state = createSpeakerTimer([
      { id: "p1", name: "Alice" },
      { id: "p2", name: "Bob" },
    ]);
    state = setActiveSpeaker(state, 0, "p1");
    state = setActiveSpeaker(state, 30, "p2");
    state = setActiveSpeaker(state, 60, null);
    expect(speakerSharePct(state.speakers[0], state.speakers)).toBe(50);
  });
});
