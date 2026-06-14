import type { Participant, ParticipantRole, ProductionState, RecordingSettings } from "../domain/production";
import {
  assessZoomSdkReadiness,
  buildZoomRawMediaSubscriptionPlan,
  type ZoomRawMediaKind,
  type ZoomRawMediaPurpose,
  type ZoomSdkReadinessInput,
  type ZoomSdkReadinessReport
} from "./zoomSdkReadiness";

export type ZoomMediaSpineParticipantRole = "host" | "cohost" | "panelist" | "attendee" | "guest";
export type ZoomMediaSpineNetworkQuality = "good" | "low" | "recovering";

export type ZoomMediaSpineParticipantPayload = {
  sdkUserId: string;
  displayName: string;
  role: ZoomMediaSpineParticipantRole;
  videoOn: boolean;
  muted: boolean;
  talking: boolean;
  sharingScreen: boolean;
  audioLevel: number;
  networkQuality: ZoomMediaSpineNetworkQuality;
};

export type ZoomMediaSpineSubscriptionRequestPayload = {
  participantId: string;
  kind: ZoomRawMediaKind;
  purpose: ZoomRawMediaPurpose;
  priority: number;
};

export type ZoomMediaSpineRecordingTargetPayload = {
  targetFolder: string;
  filenamePrefix: string;
  format: RecordingSettings["format"];
  quality: RecordingSettings["quality"];
  isoParticipantIds: string[];
};

export type ZoomMediaSpineSyncPayload = {
  readiness: ZoomSdkReadinessReport;
  participants: ZoomMediaSpineParticipantPayload[];
  subscriptions: ZoomMediaSpineSubscriptionRequestPayload[];
  recording?: ZoomMediaSpineRecordingTargetPayload;
  blocked: boolean;
  warnings: string[];
  summary: string;
};

export type ZoomMediaSpineSyncOptions = {
  maxVideoSubscriptions?: number;
  selectedBreakoutRoomId?: string;
};

export function buildZoomMediaSpineSyncPayload(
  state: ProductionState,
  readinessInput: ZoomSdkReadinessInput,
  options: ZoomMediaSpineSyncOptions = {}
): ZoomMediaSpineSyncPayload {
  const readiness = assessZoomSdkReadiness(readinessInput);
  const subscriptionPlan = buildZoomRawMediaSubscriptionPlan(state, options);
  const selectedBreakoutRoomId = options.selectedBreakoutRoomId ?? state.selectedBreakoutRoomId;
  const participants = filterParticipantsForRoom(state.participants, selectedBreakoutRoomId).map(mapParticipantToSpinePayload);
  const availableParticipantIds = new Set(participants.map((participant) => participant.sdkUserId));
  const subscriptions = subscriptionPlan.subscriptions
    .filter((subscription) => availableParticipantIds.has(subscription.participantId))
    .map((subscription) => ({
      participantId: subscription.participantId,
      kind: subscription.kind,
      purpose: subscription.purpose,
      priority: subscription.priority
    }));
  const recording = state.recording ? mapRecordingTargets(state.recordingSettings, availableParticipantIds) : undefined;
  const warnings = [
    ...readiness.blockers,
    ...readiness.warnings,
    ...subscriptionPlan.warnings,
    ...(recording && recording.isoParticipantIds.length < state.recordingSettings.isoParticipantIds.length
      ? ["Some requested ISO participants are outside the selected Zoom room or unavailable."]
      : [])
  ];
  const blocked = readiness.status === "blocked" || subscriptionPlan.blocked;

  return {
    readiness,
    participants,
    subscriptions,
    recording,
    blocked,
    warnings,
    summary: blocked
      ? `Zoom media spine sync blocked; ${warnings.length} warning${warnings.length === 1 ? "" : "s"} require attention.`
      : `${participants.length} Zoom participant${participants.length === 1 ? "" : "s"}, ${subscriptionPlan.summary}`
  };
}

function filterParticipantsForRoom(participants: Participant[], selectedBreakoutRoomId: string) {
  if (selectedBreakoutRoomId === "all") {
    return participants;
  }

  return participants.filter((participant) => participant.breakoutRoomId === selectedBreakoutRoomId);
}

function mapParticipantToSpinePayload(participant: Participant): ZoomMediaSpineParticipantPayload {
  return {
    sdkUserId: participant.id,
    displayName: participant.name,
    role: mapParticipantRole(participant.role),
    videoOn: participant.health !== "video-off",
    muted: participant.isMuted,
    talking: participant.isActiveSpeaker,
    sharingScreen: participant.isScreenSharing,
    audioLevel: participant.audioLevel,
    networkQuality: mapNetworkQuality(participant.health)
  };
}

function mapParticipantRole(role: ParticipantRole): ZoomMediaSpineParticipantRole {
  if (role === "Host") {
    return "host";
  }

  if (role === "Presenter" || role === "Panelist") {
    return "panelist";
  }

  return "guest";
}

function mapNetworkQuality(health: Participant["health"]): ZoomMediaSpineNetworkQuality {
  if (health === "low-resolution") {
    return "low";
  }

  if (health === "recovering") {
    return "recovering";
  }

  return "good";
}

function mapRecordingTargets(settings: RecordingSettings, availableParticipantIds: Set<string>): ZoomMediaSpineRecordingTargetPayload {
  return {
    targetFolder: settings.folder,
    filenamePrefix: settings.filenamePrefix,
    format: settings.format,
    quality: settings.quality,
    isoParticipantIds: settings.isoParticipantIds.filter((participantId) => availableParticipantIds.has(participantId))
  };
}
