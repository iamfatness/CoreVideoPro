import type { MeetingState, ZoomSessionSnapshot } from "./contracts";
import type { Participant, ProductionState, SceneTemplate, SourceRoute } from "../domain/production";
import { sortParticipantsForProduction } from "../domain/production";
import type { ZoomMediaSpineNativeSnapshot } from "./zoomMediaSpineNativeSync";
import { createDeck, showPlate, type LowerThirdDeck, type Nameplate, type PlateTone } from "./lowerThird";

export type LiveRosterUpdate = {
  meetingState: MeetingState;
  screenShareActive: boolean;
  elapsedSeconds: number;
  productionPatch: Pick<ProductionState, "participants" | "mediaFrames" | "captions">;
  plateDeck: LowerThirdDeck;
};

export type ProgramLowerThird = {
  name: string;
  title: string;
  org: string;
  participantId?: string;
};

export function participantPlateId(participantId: string) {
  return `participant-${participantId}`;
}

export function plateToneForParticipant(participant: Participant): PlateTone {
  if (participant.role === "Host" || participant.role === "Presenter") {
    return "accent";
  }

  if (participant.isActiveSpeaker) {
    return "guest";
  }

  return "neutral";
}

export function participantToPlate(participant: Participant): Nameplate {
  return {
    id: participantPlateId(participant.id),
    name: participant.name,
    title: participant.title.trim() || participant.role,
    role: participant.breakoutRoomName.trim() || participant.role,
    tone: plateToneForParticipant(participant)
  };
}

/**
 * Merge subscription health from a spine tick into the renderer participant roster.
 */
export function buildLiveRosterUpdate(
  snapshot: ZoomSessionSnapshot,
  currentDeck: LowerThirdDeck,
  roleOverrides: Record<string, Participant["role"]>,
  spineSnapshot?: ZoomMediaSpineNativeSnapshot
): LiveRosterUpdate {
  let participants = applyParticipantRoleOverrides(snapshot.participants, roleOverrides);
  if (spineSnapshot) {
    participants = applySpineHealthToParticipants(participants, spineSnapshot);
  }

  const plateDeck =
    snapshot.meetingState === "in_meeting"
      ? syncLowerThirdDeckFromParticipants(currentDeck, participants)
      : createDeck();

  return {
    meetingState: snapshot.meetingState,
    screenShareActive: snapshot.screenShareActive,
    elapsedSeconds: snapshot.elapsedSeconds,
    productionPatch: {
      participants,
      mediaFrames: snapshot.mediaFrames,
      captions: snapshot.caption
    },
    plateDeck
  };
}

function applyParticipantRoleOverrides(participants: Participant[], roleOverrides: Record<string, Participant["role"]>) {
  return participants.map((participant) =>
    roleOverrides[participant.id] ? { ...participant, role: roleOverrides[participant.id] } : participant
  );
}

export function applySpineHealthToParticipants(
  participants: Participant[],
  spine: ZoomMediaSpineNativeSnapshot
): Participant[] {
  const videoSubscriptions = new Map(
    spine.subscriptions
      .filter((subscription) => subscription.kind === "participant-video")
      .map((subscription) => [subscription.participantId, subscription])
  );

  return participants.map((participant) => {
    const subscription = videoSubscriptions.get(participant.id);
    let health = participant.health;

    if (subscription) {
      if (subscription.status === "failed" && subscription.lastResultCode === "video-off") {
        health = "video-off";
      } else if (subscription.status === "failed") {
        health = "recovering";
      } else if (subscription.status === "degraded" || subscription.lastResultCode === "low-resolution") {
        health = "low-resolution";
      } else if (subscription.framesReceived > 0) {
        health = "live";
      } else {
        health = "recovering";
      }
    }

    return {
      ...participant,
      health,
      isActiveSpeaker: spine.activeSpeakerId ? participant.id === spine.activeSpeakerId : participant.isActiveSpeaker,
      isScreenSharing: spine.screenShareParticipantId
        ? participant.id === spine.screenShareParticipantId
        : participant.isScreenSharing
    };
  });
}

/**
 * Keep the lower-third deck aligned with the live Zoom roster (names, titles, roles).
 * Preserves queue/on-air ordering for plates that still exist.
 */
export function syncLowerThirdDeckFromParticipants(deck: LowerThirdDeck, participants: Participant[]): LowerThirdDeck {
  if (participants.length === 0) {
    return createDeck();
  }

  const sortedParticipants = sortParticipantsForProduction(participants);
  const nextPlates = sortedParticipants.map(participantToPlate);
  const nextPlateIds = new Set(nextPlates.map((plate) => plate.id));
  const onAirId = deck.onAirId && nextPlateIds.has(deck.onAirId) ? deck.onAirId : null;
  const preservedQueue = deck.queue.filter((plateId) => nextPlateIds.has(plateId) && plateId !== onAirId);
  const shownIds = deck.shownIds.filter((plateId) => nextPlateIds.has(plateId));
  const newQueueIds = nextPlates
    .map((plate) => plate.id)
    .filter((plateId) => plateId !== onAirId && !preservedQueue.includes(plateId) && !shownIds.includes(plateId));

  return {
    ...deck,
    plates: nextPlates,
    queue: [...preservedQueue, ...newQueueIds],
    onAirId,
    shownIds
  };
}

export function showSceneFocusPlate(deck: LowerThirdDeck, scene: SceneTemplate, participants: Participant[]): LowerThirdDeck {
  const focus = resolveSceneFocusParticipant(scene, participants);
  if (!focus) {
    return deck;
  }

  const synced = syncLowerThirdDeckFromParticipants(deck, participants);
  return showPlate(synced, participantPlateId(focus.id));
}

/**
 * Pick the participant that should drive the on-program lower third for a layout.
 */
export function resolveSceneFocusParticipant(scene: SceneTemplate, participants: Participant[]): Participant | undefined {
  if (participants.length === 0) {
    return undefined;
  }

  const sortedParticipants = sortParticipantsForProduction(participants);
  const routeParticipants = getSceneRouteParticipants(scene, sortedParticipants);

  if (routeParticipants.length > 0) {
    return routeParticipants.find((participant) => participant.isActiveSpeaker) ?? routeParticipants[0];
  }

  if (scene.layout === "host-focus" || scene.layout === "outro") {
    return sortedParticipants.find((participant) => participant.role === "Host") ?? sortedParticipants[0];
  }

  return sortedParticipants.find((participant) => participant.isActiveSpeaker) ?? sortedParticipants[0];
}

export function resolveProgramLowerThird(scene: SceneTemplate, participants: Participant[]): ProgramLowerThird {
  const focus = resolveSceneFocusParticipant(scene, participants);

  if (!focus) {
    return {
      name: "CoreVideo Pro",
      title: "Zoom-native production",
      org: "Program"
    };
  }

  return {
    name: focus.name,
    title: focus.title.trim() || focus.role,
    org: focus.breakoutRoomName.trim() || focus.role,
    participantId: focus.id
  };
}

function getSceneRouteParticipants(scene: SceneTemplate, participants: Participant[]) {
  return getRouteDefaults(scene, participants)
    .map((route) => resolveRouteParticipant(route, participants))
    .filter((participant): participant is Participant => Boolean(participant))
    .filter((participant, index, list) => list.findIndex((item) => item.id === participant.id) === index);
}

function getRouteDefaults(scene: SceneTemplate, participants: Participant[]): SourceRoute[] {
  const slotCount = getRouteSlotCount(scene, participants);
  const slotDefaults =
    scene.layout === "speaker-slides"
      ? [...getSlotDefaults(scene, participants), "screen-share"]
      : getSlotDefaults(scene, participants);
  const existingRoutes = scene.routes ?? [];

  return Array.from({ length: slotCount }, (_, index) => existingRoutes[index] ?? routeFromSlot(scene.id, index, slotDefaults[index]));
}

function getRouteSlotCount(scene: SceneTemplate, participants: Participant[]) {
  if (scene.layout === "speaker-slides") {
    return 2;
  }

  return getSceneSlotCount(scene, participants);
}

function getSceneSlotCount(scene: SceneTemplate, participants: Participant[]) {
  if (scene.layout === "two-up") {
    return Math.min(2, participants.length);
  }

  if (scene.layout === "smart-grid") {
    return Math.min(6, participants.length);
  }

  return Math.min(1, participants.length);
}

function getSlotDefaults(scene: SceneTemplate, participants: Participant[]) {
  const existingSlots = (scene.slots ?? []).filter((slot) => slot !== "screen-share");
  const sortedParticipants = sortParticipantsForProduction(participants);
  const slotCount = getSceneSlotCount(scene, participants);
  const defaults = [...existingSlots];

  sortedParticipants.forEach((participant) => {
    if (defaults.length < slotCount && !defaults.includes(participant.id)) {
      defaults.push(participant.id);
    }
  });

  return defaults.slice(0, slotCount);
}

function routeFromSlot(sceneId: string, index: number, slot?: string): SourceRoute {
  if (slot === "screen-share") {
    return { id: `${sceneId}-${index + 1}`, mode: "screen-share", audioRole: "audience" };
  }

  return {
    id: `${sceneId}-${index + 1}`,
    mode: slot ? "fixed" : "active-speaker",
    participantId: slot,
    audioRole: slot ? "isolated" : "mix"
  };
}

function resolveRouteParticipant(route: SourceRoute, participants: Participant[]) {
  if (route.mode === "fixed") {
    return participants.find((participant) => participant.id === route.participantId);
  }

  if (route.mode === "active-speaker") {
    return participants.find((participant) => participant.isActiveSpeaker && participant.health !== "video-off");
  }

  if (route.mode === "spotlight") {
    return (
      participants.find((participant) => participant.id === route.participantId) ??
      participants[route.spotlightIndex ?? 0]
    );
  }

  return undefined;
}