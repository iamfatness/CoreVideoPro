import { describe, expect, it } from "vitest";
import { initialProduction, type ProductionState } from "../domain/production";
import { mapCaptureSnapshot } from "./captureSnapshotMapper";
import type { ZoomSessionSnapshot } from "./contracts";
import { buildDirectorIntelligenceSignals } from "./directorSignals";
import type { DirectorSignals } from "./directorStrategy";

function signals(snap: ZoomSessionSnapshot, state: ProductionState = initialProduction): DirectorSignals {
  return {
    state,
    snapshot: snap,
    liveParticipants: snap.participants.filter((participant) => participant.health !== "video-off"),
    elapsedMs: snap.elapsedSeconds * 1000
  };
}

describe("buildDirectorIntelligenceSignals", () => {
  it("derives speaker-turn, screen-share, engagement and feed-health summaries from the snapshot", () => {
    const snap = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: initialProduction.participants.slice(0, 4).map((participant, index) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: participant.id === "p1",
        sharingScreen: false,
        muted: index >= 2,
        networkQuality: index === 3 ? "recovering" : "good"
      }))
    });

    const bundle = buildDirectorIntelligenceSignals(signals(snap));

    expect(bundle.schemaVersion).toBe(1);
    expect(bundle.meetingState).toBe("in_meeting");
    expect(bundle.speakerTurns.activeSpeakerIds).toEqual(["p1"]);
    expect(bundle.speakerTurns.dominantSpeakerId).toBe("p1");
    expect(bundle.speakerTurns.crossTalk).toBe(false);
    expect(bundle.speakerTurns.recentTurns).toHaveLength(1);
    expect(bundle.screenShare.active).toBe(false);
    expect(bundle.feedHealth.liveCount).toBe(4);
    expect(bundle.feedHealth.degradedCount).toBe(1);
    expect(bundle.feedHealth.byHealth.recovering).toBe(1);
    expect(bundle.engagement.liveRatio).toBe(1);
    // p1 talking + p2 unmuted => 2 plausible contributors (p3/p4 muted, p4 not talking).
    expect(bundle.engagement.activeContributorCount).toBe(2);
  });

  it("flags cross-talk when multiple feeds are simultaneously active", () => {
    const snap = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: initialProduction.participants.slice(0, 3).map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: ["p1", "p2"].includes(participant.id),
        networkQuality: "good"
      }))
    });

    const bundle = buildDirectorIntelligenceSignals(signals(snap));
    expect(bundle.speakerTurns.crossTalk).toBe(true);
    expect(bundle.speakerTurns.dominantSpeakerId).toBeUndefined();
    expect(bundle.speakerTurns.activeSpeakerIds.length).toBeGreaterThanOrEqual(2);
  });

  it("reports screen-share presence and the sharer without decoding content", () => {
    const snap = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p2",
      participants: initialProduction.participants.slice(0, 3).map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: participant.id === "p2",
        sharingScreen: participant.id === "p2",
        networkQuality: "good"
      }))
    });

    const bundle = buildDirectorIntelligenceSignals(signals(snap));
    expect(bundle.screenShare.active).toBe(true);
    expect(bundle.screenShare.sharerId).toBe("p2");
    expect(bundle.screenShare.sharerName).toBeTruthy();
    // Privacy posture: no content classification is ever produced by the scaffold.
    expect("contentClassification" in bundle.screenShare).toBe(false);
  });

  it("is pure and JSON-serializable (deterministic for identical signals)", () => {
    const snap = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 1,
      activeSpeakerId: "p1",
      participants: initialProduction.participants.slice(0, 5).map((participant) => ({
        userId: participant.id,
        displayName: participant.name,
        role: participant.role,
        talking: participant.id === "p1",
        networkQuality: "good"
      }))
    });
    const a = buildDirectorIntelligenceSignals(signals(snap));
    const b = buildDirectorIntelligenceSignals(signals(snap));
    expect(a).toEqual(b);
    expect(JSON.parse(JSON.stringify(a))).toEqual(a);
  });
});
