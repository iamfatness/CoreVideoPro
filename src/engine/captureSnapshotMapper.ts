import type { FeedHealth, Participant, ParticipantRole } from "../domain/production";
import type { MeetingState, ZoomSessionSnapshot } from "./contracts";
import { buildMediaFrames } from "./mediaFrames";

export type RawParticipantEvent = {
  userId: string | number;
  displayName: string;
  role?: ParticipantRole;
  title?: string;
  breakoutRoomId?: string;
  breakoutRoomName?: string;
  muted?: boolean;
  videoOn?: boolean;
  talking?: boolean;
  sharingScreen?: boolean;
  audioLevel?: number;
  networkQuality?: "good" | "low" | "recovering";
};

export type RawCaptureSnapshot = {
  meetingState: MeetingState;
  participants: RawParticipantEvent[];
  activeSpeakerId?: string | number;
  caption?: string;
  tick: number;
};

export function isRawCaptureSnapshot(value: unknown): value is RawCaptureSnapshot {
  return (
    typeof value === "object" &&
    value !== null &&
    "meetingState" in value &&
    "tick" in value &&
    Array.isArray((value as RawCaptureSnapshot).participants)
  );
}

export function mapCaptureSnapshot(raw: RawCaptureSnapshot): ZoomSessionSnapshot {
  const participants = raw.meetingState === "in_meeting" ? normalizeParticipants(raw) : [];
  const activeSpeaker = participants.find((participant) => participant.isActiveSpeaker && participant.health !== "video-off");

  return freezeSnapshot({
    meetingState: raw.meetingState,
    participants,
    mediaFrames: raw.meetingState === "in_meeting" ? buildMediaFrames(participants, raw.tick) : [],
    activeSpeakerName: activeSpeaker?.name ?? "None",
    screenShareActive: participants.some((participant) => participant.isScreenSharing),
    caption: raw.caption ?? "",
    elapsedSeconds: Math.max(0, raw.tick) * 8
  });
}

function normalizeParticipants(raw: RawCaptureSnapshot): Participant[] {
  const activeSpeakerId = raw.activeSpeakerId == null ? "" : String(raw.activeSpeakerId);

  return raw.participants.map((participant) => {
    const id = String(participant.userId);
    const health = normalizeHealth(participant);

    return {
      id,
      name: participant.displayName.trim() || `Zoom User ${id}`,
      title: participant.title ?? participant.role ?? "Guest",
      role: participant.role ?? "Guest",
      breakoutRoomId: participant.breakoutRoomId ?? "main",
      breakoutRoomName: participant.breakoutRoomName ?? "Main room",
      isActiveSpeaker: id === activeSpeakerId || Boolean(participant.talking),
      isMuted: Boolean(participant.muted),
      isScreenSharing: Boolean(participant.sharingScreen),
      audioLevel: clampNumber(participant.audioLevel ?? (participant.talking ? 78 : 22), 0, 100),
      gainDb: participant.talking ? -0.5 : 1.5,
      noiseSuppression: true,
      cropConfidence: health === "video-off" ? 0 : health === "low-resolution" ? 82 : 94,
      health
    };
  });
}

function normalizeHealth(participant: RawParticipantEvent): FeedHealth {
  if (participant.videoOn === false) {
    return "video-off";
  }

  if (participant.networkQuality === "low") {
    return "low-resolution";
  }

  if (participant.networkQuality === "recovering") {
    return "recovering";
  }

  return "live";
}

function freezeSnapshot(snapshot: ZoomSessionSnapshot): ZoomSessionSnapshot {
  snapshot.participants.forEach(Object.freeze);
  snapshot.mediaFrames.forEach((frame) => {
    Object.freeze(frame.crop);
    Object.freeze(frame);
  });
  Object.freeze(snapshot.participants);
  Object.freeze(snapshot.mediaFrames);
  return Object.freeze(snapshot);
}

function clampNumber(value: number, min: number, max: number) {
  return Math.max(min, Math.min(max, value));
}
