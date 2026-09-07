import type { OutputDestination, Participant, ProductionState } from "../domain/production";
import type { NativeMediaCoreRecordingSession, NativeMediaCoreStateSnapshot } from "./nativeMediaCoreProtocol";
import { isoOutputPath, planIsoRecording, summarizeIsoPlan } from "./isoRecording";

import { recordingReadModel, type RecordingObservedStatus } from "./recordingReadModel";
import type { OutputLifecycle } from "./generated/lifecycle";

export type RecordingManifestStatus = RecordingObservedStatus;

export type RecordingManifestTrack = {
  kind: "program" | "iso";
  participantId?: string;
  participantName?: string;
  participantRole?: string;
  readiness?: "ready" | "missing" | "video-off" | "unsubscribable";
  path: string;
  status: "writing" | "warning" | "stopped" | "failed";
  expectedFrames: number;
  framesWritten: number;
  missingFrames: number;
  droppedFrames: number;
  bytesWritten: number;
  warning?: string;
};

export type RecordingManifest = {
  manifestId: string;
  sessionId?: string;
  status: RecordingManifestStatus;
  active: boolean;
  lifecycle?: OutputLifecycle;
  finalized: boolean;
  meetingTitle: string;
  sceneId?: string;
  startedAtMs?: number;
  stoppedAtMs?: number;
  elapsedMs: number;
  targetFolder: string;
  filenamePrefix: string;
  format: string;
  quality: string;
  outputProfile: {
    profileId: string;
    resolution: string;
    fps: number;
    targetBitrateMbps: number;
  };
  encoder: {
    codec?: string;
    hardwareAccelerated?: boolean;
    targetBitrateMbps?: number;
  };
  destinations: Array<{
    id: string;
    name: string;
    protocol: OutputDestination["protocol"];
    active: boolean;
    health: "idle" | "live" | "warning";
  }>;
  tracks: RecordingManifestTrack[];
  isoPlan: {
    trackCount: number;
    estimatedTotalMbps: number;
    estimatedFileSizeGbPerHour: number;
    outputFolder: string;
    summary: string;
    warnings: string[];
    valid: boolean;
  };
  totals: {
    framesWritten: number;
    droppedFrames: number;
    missingFrames: number;
    bytesWritten: number;
    estimatedDiskRateMBps?: number;
  };
  warnings: string[];
};

export function buildRecordingManifest(state: ProductionState, mediaCore?: NativeMediaCoreStateSnapshot): RecordingManifest {
  const recording = mediaCore?.recording;
  const observed = recordingReadModel(recording, state.recording);
  const tracks = buildTracks(state.participants, recording);
  const outputProfile = mediaCore?.outputProfile ?? selectedOutputProfile(state);
  const isoParticipants = state.recordingSettings.isoParticipantIds
    .map((participantId) => state.participants.find((participant) => participant.id === participantId))
    .filter((participant): participant is Participant => Boolean(participant));
  const isoPlan = planIsoRecording(
    isoParticipants.map((participant) => ({ id: participant.id, name: participant.name })),
    {
      includeProgramMix: true,
      includeScreenShare: state.participants.some((participant) => participant.isScreenSharing),
      participantResolution: outputProfile.height >= 1080 ? "1080p" : outputProfile.height >= 720 ? "720p" : "480p",
      sessionName: state.recordingSettings.filenamePrefix
    }
  );
  const warnings = buildWarnings(recording, tracks, isoPlan.warnings);

  return {
    manifestId: `manifest-${slugify(state.meetingTitle)}-${recording?.sessionId ?? "idle"}`,
    sessionId: recording?.sessionId,
    status: observed.status,
    active: observed.active,
    lifecycle: observed.lifecycle,
    finalized: observed.finalized,
    meetingTitle: state.meetingTitle,
    sceneId: mediaCore?.sceneId ?? state.activeSceneId,
    startedAtMs: recording?.startedAtMs,
    stoppedAtMs: recording?.stoppedAtMs,
    elapsedMs: recording?.elapsedMs ?? state.outputSession.elapsedSeconds * 1000,
    targetFolder: recording?.targetFolder ?? state.recordingSettings.folder,
    filenamePrefix: recording?.filenamePrefix ?? state.recordingSettings.filenamePrefix,
    format: recording?.format ?? state.recordingSettings.format,
    quality: recording?.quality ?? state.recordingSettings.quality,
    outputProfile: {
      profileId: outputProfile.profileId,
      resolution: outputProfile.resolution,
      fps: outputProfile.fps,
      targetBitrateMbps: outputProfile.targetBitrateMbps
    },
    encoder: {
      codec: recording?.encoder.codec,
      hardwareAccelerated: recording?.encoder.hardwareAccelerated,
      targetBitrateMbps: recording?.encoder.targetBitrateMbps
    },
    destinations: state.outputSession.destinations.map((destination) => ({
      id: destination.id,
      name: destination.name,
      protocol: destination.protocol,
      active: destination.active,
      health: destination.health
    })),
    tracks,
    isoPlan: {
      trackCount: isoPlan.tracks.length,
      estimatedTotalMbps: isoPlan.estimatedTotalMbps,
      estimatedFileSizeGbPerHour: Number(isoPlan.estimatedFileSizeGbPerHour.toFixed(2)),
      outputFolder: isoPlan.outputFolder,
      summary: summarizeIsoPlan(isoPlan),
      warnings: isoPlan.warnings,
      valid: isoPlan.valid
    },
    totals: {
      framesWritten: recording?.totalFramesWritten ?? tracks.reduce((total, track) => total + track.framesWritten, 0),
      droppedFrames: recording?.totalDroppedFrames ?? tracks.reduce((total, track) => total + track.droppedFrames, 0),
      missingFrames: tracks.reduce((total, track) => total + track.missingFrames, 0),
      bytesWritten: recording?.totalBytesWritten ?? tracks.reduce((total, track) => total + track.bytesWritten, 0),
      estimatedDiskRateMBps: recording?.estimatedDiskRateMBps
    },
    warnings
  };
}

function buildTracks(participants: Participant[], recording?: NativeMediaCoreRecordingSession): RecordingManifestTrack[] {
  if (!recording) {
    return [];
  }

  return recording.streams.map((stream, index) => {
    const participant = stream.participantId ? participants.find((item) => item.id === stream.participantId) : undefined;
    return {
      kind: stream.kind,
      participantId: stream.participantId,
      participantName: participant?.name,
      participantRole: participant?.role,
      readiness: stream.kind === "iso" ? stream.readiness ?? "ready" : stream.readiness,
      path: stream.path || isoOutputPath(recording.targetFolder, participant?.name ?? stream.participantId ?? stream.kind, index),
      status: stream.status,
      expectedFrames: stream.expectedFrames ?? 0,
      framesWritten: stream.framesWritten,
      missingFrames: stream.missingFrames ?? 0,
      droppedFrames: stream.droppedFrames,
      bytesWritten: stream.bytesWritten,
      warning: stream.warning
    };
  });
}

function selectedOutputProfile(state: ProductionState) {
  const profile = state.outputProfiles.find((candidate) => candidate.id === state.selectedOutputProfileId) ?? state.outputProfiles[0];
  const [width, height] = profile.resolution.split("x").map((part) => Number(part));
  return {
    profileId: profile.id,
    resolution: profile.resolution,
    width,
    height,
    fps: profile.fps,
    targetBitrateMbps: profile.targetBitrateMbps
  };
}

function buildWarnings(recording: NativeMediaCoreRecordingSession | undefined, tracks: RecordingManifestTrack[], isoPlanWarnings: string[]) {
  return [
    recording?.warning,
    recording?.error,
    recording?.lifecycle?.error,
    ...tracks.map((track) => track.warning),
    ...isoPlanWarnings
  ].filter(Boolean) as string[];
}

function slugify(value: string) {
  return value.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/(^-|-$)/g, "") || "corevideo";
}
