import type {
  FeedHealth,
  OutputDestinationState,
  Participant,
  ProductionState,
  SceneTemplate,
  SupportBundle,
  SupportBundleMediaCore
} from "../domain/production";
import type { SupportBundleContext } from "./contracts";
import type { NativeMediaCoreStateSnapshot } from "./nativeMediaCoreProtocol";
import { buildRecordingManifest } from "./recordingManifest";

const MOCK_FREE_DISK_BYTES = 86 * 1024 * 1024 * 1024;

export class InMemoryDiagnosticsEngine {
  async createSupportBundle(
    state: ProductionState,
    mediaCore?: NativeMediaCoreStateSnapshot,
    context?: SupportBundleContext
  ): Promise<SupportBundle> {
    return createSupportBundle(state, mediaCore, context);
  }
}

export function createSupportBundle(
  state: ProductionState,
  mediaCore?: NativeMediaCoreStateSnapshot,
  context: SupportBundleContext = {}
): SupportBundle {
  const createdAt = new Date().toISOString();
  const duplicateAssignments = countDuplicateAssignments(state.scenes);
  const unavailableScreenShare = countUnavailableScreenShare(state);
  const lowDeliveredResolution = state.participants.filter((participant) => participant.health === "low-resolution").length;
  const warnings = buildWarnings(state, duplicateAssignments, unavailableScreenShare, lowDeliveredResolution);
  const freeDiskBytes = context.freeDiskBytes ?? MOCK_FREE_DISK_BYTES;
  const isoCapacity = estimateIsoCapacity(state, freeDiskBytes);
  const destinations = getDestinationStates(state);
  const triageLines = buildTriageLines(state, warnings, isoCapacity, context);

  return {
    id: `support-${slugify(state.meetingTitle)}-${createdAt.replace(/[-:.TZ]/g, "").slice(0, 14)}`,
    createdAt,
    app: {
      name: "CoreVideo Pro",
      version: context.version ?? "0.1.0",
      platform: context.platform ?? "mock-desktop"
    },
    summaryText: triageLines.join("\n"),
    triageLines,
    production: {
      meetingTitle: state.meetingTitle,
      mode: state.mode,
      activeSceneId: state.activeSceneId,
      previewSceneId: state.previewSceneId,
      selectedBreakoutRoomId: state.selectedBreakoutRoomId,
      recording: state.recording,
      streaming: state.streaming
    },
    participants: state.participants.map((participant) => ({
      id: participant.id,
      name: participant.name,
      role: participant.role,
      health: participant.health,
      breakoutRoomName: participant.breakoutRoomName,
      isActiveSpeaker: participant.isActiveSpeaker,
      isScreenSharing: participant.isScreenSharing,
      muted: participant.isMuted,
      recommendedAction: getParticipantAction(participant)
    })),
    output: {
      health: { ...state.outputSession.health },
      activeDestinationIds: [...state.outputSession.activeDestinationIds],
      destinations: destinations.map((destination) => ({
        id: destination.id,
        name: destination.name,
        protocol: destination.protocol,
        enabled: destination.enabled,
        active: destination.active,
        health: destination.health,
        bitrateMbps: destination.bitrateMbps,
        endpoint: redactEndpoint(destination.endpoint),
        streamKey: redactSecret(destination.streamKey)
      })),
      statusText: state.outputSession.statusText
    },
    actionCounts: {
      duplicateAssignments,
      unavailableScreenShare,
      lowDeliveredResolution,
      sdkSubscribeErrors: 0
    },
    isoCapacity,
    mediaCore: mediaCore ? summarizeMediaCore(mediaCore, state) : undefined,
    runtime: context.runtime,
    crashRecovery: context.crashEvents?.length ? { events: context.crashEvents } : undefined,
    warnings
  };
}

function summarizeMediaCore(snapshot: NativeMediaCoreStateSnapshot, state?: ProductionState): SupportBundleMediaCore {
  const recordingManifest = state ? buildRecordingManifest(state, snapshot) : undefined;
  return {
    sceneId: snapshot.sceneId,
    renderPlanId: snapshot.renderPlan.renderPlanId,
    source: {
      adapterId: snapshot.sourceSnapshot.adapterId,
      kind: snapshot.sourceSnapshot.kind,
      status: snapshot.sourceSnapshot.status,
      subscribedSourceCount: snapshot.sourceSnapshot.subscribedSourceCount,
      droppedFrameCount: snapshot.sourceSnapshot.droppedFrameCount,
      lowResolutionFrameCount: snapshot.sourceSnapshot.lowResolutionFrameCount,
      issues: (snapshot.sourceSnapshot.issues ?? []).map((issue) => ({
        sourceId: issue.sourceId,
        participantId: issue.participantId,
        displayName: issue.displayName,
        health: issue.health,
        severity: issue.severity,
        detail: issue.detail
      }))
    },
    compositor: {
      status: snapshot.compositor.status,
      programFrameCount: snapshot.compositor.programFrameCount,
      droppedFrameCount: snapshot.compositor.droppedFrameCount,
      degradedFrameCount: snapshot.compositor.degradedFrameCount
    },
    transport: {
      status: snapshot.programTransport.status,
      frameNumber: snapshot.programTransport.frameNumber,
      latencyMs: snapshot.programTransport.latencyMs
    },
    encoder: {
      status: snapshot.encoderSession.status,
      lifecycle: snapshot.encoderSession.lifecycle.status,
      targetCount: snapshot.encoderSession.targets.length
    },
    senders: {
      status: snapshot.outputSenderSession.status,
      activeSenderCount: snapshot.outputSenderSession.activeSenderCount,
      destinations: snapshot.outputSenderSession.senders.map((sender) => ({
        destination: sender.destination,
        status: sender.status,
        startedAtMs: sender.startedAtMs,
        stoppedAtMs: sender.stoppedAtMs,
        lastFrameNumber: sender.lastFrameNumber,
        framesSent: sender.framesSent,
        retryCount: sender.retryCount,
        latencyMs: sender.latencyMs,
        bitrateMbps: sender.bitrateMbps,
        warning: sender.warning
      }))
    },
    recording: snapshot.recording
      ? {
          status: snapshot.recording.status,
          writerStatus: snapshot.recording.writerStatus,
          totalFramesWritten: snapshot.recording.totalFramesWritten,
          totalDroppedFrames: snapshot.recording.totalDroppedFrames,
          estimatedDiskRateMBps: snapshot.recording.estimatedDiskRateMBps,
          streams: snapshot.recording.streams.map((stream) => ({
            kind: stream.kind,
            participantId: stream.participantId,
            status: stream.status,
            readiness: stream.readiness,
            expectedFrames: stream.expectedFrames,
            framesWritten: stream.framesWritten,
            missingFrames: stream.missingFrames,
            droppedFrames: stream.droppedFrames,
            bytesWritten: stream.bytesWritten,
            warning: stream.warning
          }))
        }
      : undefined,
    recordingManifest: recordingManifest
      ? {
          manifestId: recordingManifest.manifestId,
          sessionId: recordingManifest.sessionId,
          status: recordingManifest.status,
          active: recordingManifest.active,
          sceneId: recordingManifest.sceneId,
          targetFolder: recordingManifest.targetFolder,
          filenamePrefix: recordingManifest.filenamePrefix,
          format: recordingManifest.format,
          quality: recordingManifest.quality,
          outputProfile: recordingManifest.outputProfile,
          trackCount: recordingManifest.tracks.length,
          isoTrackCount: recordingManifest.tracks.filter((track) => track.kind === "iso").length,
          estimatedTotalMbps: recordingManifest.isoPlan.estimatedTotalMbps,
          estimatedFileSizeGbPerHour: recordingManifest.isoPlan.estimatedFileSizeGbPerHour,
          totals: recordingManifest.totals,
          warnings: recordingManifest.warnings
        }
      : undefined,
    operatorActions: snapshot.operatorActions.map((action) => ({
      actionId: action.actionId,
      severity: action.severity,
      area: action.area,
      title: action.title,
      detail: action.detail,
      command: action.command,
      relatedId: action.relatedId
    })),
    eventLog: snapshot.eventLog.map((event) => ({
      eventId: event.eventId,
      atMs: event.atMs,
      severity: event.severity,
      area: event.area,
      title: event.title,
      detail: event.detail,
      relatedId: event.relatedId,
      commandType: event.commandType
    })),
    warnings: snapshot.warnings
  };
}

function getDestinationStates(state: ProductionState): OutputDestinationState[] {
  return state.outputDestinations.map((destination) => {
    const sessionDestination = state.outputSession.destinations.find((item) => item.id === destination.id);
    return sessionDestination ?? { ...destination, active: false, health: "idle", bitrateMbps: 0 };
  });
}

function countDuplicateAssignments(scenes: SceneTemplate[]) {
  return scenes.reduce((count, scene) => {
    const participantSlots = scene.routes
      ? scene.routes
          .filter((route) => (route.mode === "fixed" || route.mode === "spotlight") && route.participantId)
          .map((route) => route.participantId as string)
      : (scene.slots ?? []).filter((slot) => slot !== "screen-share");
    return count + participantSlots.length - new Set(participantSlots).size;
  }, 0);
}

function countUnavailableScreenShare(state: ProductionState) {
  const activeScene = state.scenes.find((scene) => scene.id === state.activeSceneId);
  const sceneNeedsShare =
    activeScene?.layout === "speaker-slides" ||
    activeScene?.slots?.includes("screen-share") ||
    activeScene?.routes?.some((route) => route.mode === "screen-share");
  const hasShare = state.participants.some((participant) => participant.isScreenSharing);
  return sceneNeedsShare && !hasShare ? 1 : 0;
}

function buildWarnings(
  state: ProductionState,
  duplicateAssignments: number,
  unavailableScreenShare: number,
  lowDeliveredResolution: number
) {
  const warnings: string[] = [];

  if (duplicateAssignments > 0) {
    warnings.push(`${duplicateAssignments} duplicate scene assignment${duplicateAssignments === 1 ? "" : "s"} detected.`);
  }

  if (unavailableScreenShare > 0) {
    warnings.push("Program scene expects screen share, but no participant is sharing.");
  }

  if (lowDeliveredResolution > 0) {
    warnings.push(`${lowDeliveredResolution} participant feed${lowDeliveredResolution === 1 ? "" : "s"} delivered below target resolution.`);
  }

  if (state.outputSession.health.network === "warning") {
    warnings.push("Output network health is warning.");
  }

  state.captionOverlay.warnings.forEach((warning) => warnings.push(warning));

  return warnings;
}

function buildTriageLines(
  state: ProductionState,
  warnings: string[],
  isoCapacity: SupportBundle["isoCapacity"],
  context: SupportBundleContext
) {
  const lines = [
    `Show: ${state.meetingTitle} (${state.mode})`,
    `Program: ${state.activeSceneId}; Preview: ${state.previewSceneId}`,
    `Participants: ${state.participants.length}; low-quality feeds: ${state.participants.filter((participant) => participant.health !== "live").length}`,
    `Outputs: ${state.outputSession.statusText}; active destinations: ${state.outputSession.activeDestinationIds.length}`,
    `Output warnings: ${warnings.length}`,
    `ISO runway: ${isoCapacity.recordingRunwayMinutes} minutes at ${isoCapacity.estimatedPathCount} estimated paths.`,
    "SDK subscribe errors: 0"
  ];

  if (context.runtime) {
    lines.push(`Runtime: ${context.runtime.label} (${context.runtime.status})`);
    if (context.runtime.restartCount > 0) {
      lines.push(`Media core restarts: ${context.runtime.restartCount}`);
    }
  }

  if (context.crashEvents?.length) {
    const latest = context.crashEvents[context.crashEvents.length - 1];
    lines.push(`Latest crash: exit ${latest.exitCode ?? "unknown"} at ${latest.at}`);
  }

  return lines;
}

function estimateIsoCapacity(state: ProductionState, freeDiskBytes: number): SupportBundle["isoCapacity"] {
  const selectedParticipantIds = state.recordingSettings.isoParticipantIds.filter((participantId) =>
    state.participants.some((participant) => participant.id === participantId && participant.health !== "video-off")
  );
  const estimatedPathCount = Math.max(1, selectedParticipantIds.length + 1);
  const estimatedDiskRateMBps = Number((estimatedPathCount * 1.85).toFixed(2));
  const recordingRunwayMinutes = Math.floor(freeDiskBytes / (estimatedDiskRateMBps * 1024 * 1024) / 60);
  const warning = recordingRunwayMinutes < 30 ? "Recording runway under 30 minutes." : undefined;

  return {
    selectedParticipantIds,
    estimatedPathCount,
    estimatedDiskRateMBps,
    freeDiskBytes,
    recordingRunwayMinutes,
    warning
  };
}

function getParticipantAction(participant: Participant) {
  const actions: Record<FeedHealth, string> = {
    live: participant.isMuted ? "Check mute state if the participant should be audible." : "No action needed.",
    "low-resolution": "Ask participant to improve network or reduce competing bandwidth.",
    recovering: "Keep routed only if needed; verify the feed stabilizes before taking live.",
    "video-off": "Keep as audio-only or ask participant to enable video."
  };

  return actions[participant.health];
}

function redactEndpoint(endpoint: string) {
  if (!endpoint) {
    return "";
  }

  try {
    const url = new URL(endpoint);
    if (url.username) {
      url.username = "redacted";
    }
    if (url.password) {
      url.password = "redacted";
    }
    ["key", "token", "passphrase", "password", "secret", "code"].forEach((name) => {
      if (url.searchParams.has(name)) {
        url.searchParams.set(name, "redacted");
      }
    });
    return url.toString();
  } catch {
    return endpoint.replace(/(token|key|passphrase|password|secret|code)=([^&\s]+)/gi, "$1=redacted");
  }
}

function redactSecret(value?: string) {
  return value ? "present-redacted" : "absent";
}

function slugify(value: string) {
  return value.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/(^-|-$)/g, "") || "corevideo";
}
