import { describe, expect, it } from "vitest";
import { initialProduction, type Participant } from "../domain/production";
import type { ZoomMediaSpineNativeSnapshot } from "./zoomMediaSpineNativeSync";
import { createDeck, showPlate } from "./lowerThird";
import type { ZoomSessionSnapshot } from "./contracts";
import { mapCaptureSnapshot } from "./captureSnapshotMapper";
import {
  applySpineHealthToParticipants,
  buildLiveRosterUpdate,
  participantPlateId,
  resolveProgramLowerThird,
  resolveSceneFocusParticipant,
  showSceneFocusPlate,
  syncLowerThirdDeckFromParticipants
} from "./liveProductionSync";

const participants = initialProduction.participants;

function spineSnapshot(overrides: Partial<ZoomMediaSpineNativeSnapshot> = {}): ZoomMediaSpineNativeSnapshot {
  return {
    meetingState: "in-meeting",
    sdkVersion: "test",
    participantCount: participants.length,
    activeSpeakerId: "p2",
    participants: [],
    subscriptions: [
      {
        participantId: "p2",
        kind: "participant-video",
        purpose: "program",
        priority: 1,
        subscriptionId: "participant-video:p2:program",
        status: "subscribed",
        lastResultCode: "ok",
        framesReceived: 12,
        audioPacketsReceived: 0
      },
      {
        participantId: "p3",
        kind: "participant-video",
        purpose: "program",
        priority: 2,
        subscriptionId: "participant-video:p3:program",
        status: "failed",
        lastResultCode: "video-off",
        framesReceived: 0,
        audioPacketsReceived: 0
      }
    ],
    recording: { evidence: { programFramesWritten: 0, isoFramesWritten: 0, audioPacketsObserved: 0, subscribedVideoFeeds: 1 } },
    warnings: [],
    events: [],
    ...overrides
  };
}

describe("applySpineHealthToParticipants", () => {
  it("maps subscription health and active speaker from the spine snapshot", () => {
    const updated = applySpineHealthToParticipants(participants, spineSnapshot());

    expect(updated.find((participant) => participant.id === "p2")?.isActiveSpeaker).toBe(true);
    expect(updated.find((participant) => participant.id === "p2")?.health).toBe("live");
    expect(updated.find((participant) => participant.id === "p3")?.health).toBe("video-off");
  });
});

describe("syncLowerThirdDeckFromParticipants", () => {
  it("builds plates from live participant names and roles", () => {
    const deck = syncLowerThirdDeckFromParticipants(createDeck(), participants.slice(0, 3));

    expect(deck.plates).toHaveLength(3);
    expect(deck.plates[0]?.name).toBe("David Chen");
    expect(deck.plates[0]?.title).toBe("Chief Product Officer");
    expect(deck.queue).toHaveLength(3);
  });

  it("preserves on-air state when the participant remains in the roster", () => {
    const synced = syncLowerThirdDeckFromParticipants(createDeck(), participants.slice(0, 2));
    const onAir = showPlate(synced, participantPlateId("p1"));

    const refreshed = syncLowerThirdDeckFromParticipants(onAir, participants.slice(0, 2));
    expect(refreshed.onAirId).toBe(participantPlateId("p1"));
    expect(refreshed.plates.find((plate) => plate.id === participantPlateId("p1"))?.name).toBe("Sophia Martinez");
  });
});

describe("resolveSceneFocusParticipant", () => {
  it("prefers the active speaker assigned to the scene routes", () => {
    const focus = resolveSceneFocusParticipant(initialProduction.scenes.find((scene) => scene.id === "interview")!, participants);

    expect(focus?.id).toBe("p2");
    expect(focus?.name).toBe("David Chen");
  });

  it("falls back to the host on intro layouts", () => {
    const roster = participants.map(
      (participant): Participant => ({ ...participant, isActiveSpeaker: false, isScreenSharing: false })
    );
    const focus = resolveSceneFocusParticipant(initialProduction.scenes.find((scene) => scene.id === "intro")!, roster);

    expect(focus?.role).toBe("Host");
    expect(focus?.name).toBe("Sophia Martinez");
  });
});

describe("resolveProgramLowerThird", () => {
  it("returns display metadata for the scene focus participant", () => {
    const lowerThird = resolveProgramLowerThird(initialProduction.scenes.find((scene) => scene.id === "panel")!, participants);

    expect(lowerThird.name).toBe("David Chen");
    expect(lowerThird.title).toBe("Chief Product Officer");
    expect(lowerThird.org).toBe("Main room");
  });
});

describe("buildLiveRosterUpdate", () => {
  it("packages participant, media frame, and lower-third deck updates from a live snapshot", () => {
    const snapshot: ZoomSessionSnapshot = mapCaptureSnapshot({
      meetingState: "in_meeting",
      tick: 3,
      activeSpeakerId: "live-1",
      participants: [
        {
          userId: "live-1",
          displayName: "Live Host",
          role: "Host",
          title: "Executive Producer",
          talking: true,
          videoOn: true,
          networkQuality: "good"
        }
      ]
    });

    const update = buildLiveRosterUpdate(snapshot, createDeck(), {});

    expect(update.productionPatch.participants[0]?.name).toBe("Live Host");
    expect(update.productionPatch.mediaFrames.length).toBeGreaterThan(0);
    expect(update.plateDeck.plates[0]?.name).toBe("Live Host");
    expect(update.plateDeck.plates[0]?.title).toBe("Executive Producer");
  });
});

describe("showSceneFocusPlate", () => {
  it("puts the scene focus plate on air", () => {
    const deck = showSceneFocusPlate(createDeck(), initialProduction.scenes.find((scene) => scene.id === "panel")!, participants);

    expect(deck.onAirId).toBe(participantPlateId("p2"));
    expect(deck.plates.find((plate) => plate.id === deck.onAirId)?.name).toBe("David Chen");
  });
});