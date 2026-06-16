/**
 * Self-contained synthetic media core, owned by Track A.
 *
 * Produces a valid {@link NativeMediaCoreStateSnapshot} from a batch of
 * {@link NativeMediaCoreCommand}s and announces a capability profile. It is the
 * "simulated engine ported to Node" the desktop stub runs until Track B's native
 * binary exists. Deliberately dependency-light (type-only imports) so it can run
 * in a plain `node` child process via type-stripping — no bundler, no SDK.
 */
import type {
  NativeMediaCoreColorGrade,
  NativeMediaCoreCommand,
  NativeMediaCoreEncoderSession,
  NativeMediaCoreFrame,
  NativeMediaCoreFrameSourceSnapshot,
  NativeMediaCoreOutputHealth,
  NativeMediaCoreOutputProfile,
  NativeMediaCoreOutputSenderSession,
  NativeMediaCoreProfile,
  NativeMediaCoreProgramFrame,
  NativeMediaCoreRecordingSession,
  NativeMediaCoreRenderPlan,
  NativeMediaCoreStateSnapshot
} from "../src/engine/nativeMediaCoreProtocol";
import type {
  ZoomMediaSpineNativeSnapshot,
  ZoomMediaSpineNativeSubscription,
  ZoomMediaSpineSubscriptionStatus
} from "../src/engine/zoomMediaSpineNativeSync";
import type { RawCaptureSnapshot } from "../src/engine/captureSnapshotMapper";
import type { ZoomJoinRequest } from "../src/engine/contracts";
import type { ZoomMediaSpineSyncPayload } from "../src/engine/zoomMediaSpineSync";
import { NativeAudioMixSessionSimulator, NativeCaptionTrackSimulator } from "../src/engine/nativeMediaCoreAudioCaption.ts";
import { NativeBrandKitSimulator } from "../src/engine/nativeMediaCoreBrandKit.ts";

const DEFAULT_OUTPUT_PROFILE: NativeMediaCoreOutputProfile = {
  profileId: "1080p60",
  resolution: "1920x1080",
  width: 1920,
  height: 1080,
  fps: 60,
  targetBitrateMbps: 8
};

const NEUTRAL_GRADE: NativeMediaCoreColorGrade = {
  lut: "none",
  exposure: 0,
  contrast: 0,
  saturation: 0,
  temperature: 0
};

/**
 * A fully-capable synthetic profile so `describeRuntimeEnvironment` reports
 * "ready". Drop capabilities here to exercise the "degraded"/"unsupported" path.
 */
export const SYNTHETIC_PROFILE: NativeMediaCoreProfile = {
  name: "CoreVideo synthetic core (Node stub)",
  renderer: "vulkan",
  maxProgramResolution: "3840x2160",
  maxProgramFps: 60,
  maxParticipantFeeds: 8,
  maxIsoRecordings: 4,
  capabilities: [
    "zoom-raw-video",
    "zoom-raw-audio",
    "gpu-compositor",
    "scene-graph-rendering",
    "dynamic-overlays",
    "chroma-key",
    "smart-framing",
    "audio-mixer",
    "program-recording",
    "iso-recording",
    "rtmp-output",
    "ndi-output",
    "srt-output",
    "webrtc-output",
    "decklink-capture",
    "aja-capture"
  ]
};

export type SyntheticZoomCaptureState = {
  joined: boolean;
  tick: number;
  displayName: string;
};

export type SyntheticBreakoutRoomState = {
  breakoutRoomId: string;
  breakoutRoomName: string;
};

const DEFAULT_BREAKOUT_ROOM: SyntheticBreakoutRoomState = {
  breakoutRoomId: "main",
  breakoutRoomName: "Main room"
};

let breakoutRoomState: SyntheticBreakoutRoomState = { ...DEFAULT_BREAKOUT_ROOM };

export function resetSyntheticBreakoutRoomState(): void {
  breakoutRoomState = { ...DEFAULT_BREAKOUT_ROOM };
}

export function createSyntheticZoomCaptureState(): SyntheticZoomCaptureState {
  return { joined: false, tick: 0, displayName: "Guest Producer" };
}

export function synthesizeZoomJoinSnapshot(
  state: SyntheticZoomCaptureState,
  request: ZoomJoinRequest
): RawCaptureSnapshot {
  state.joined = true;
  state.displayName = request.displayName.trim() || state.displayName;
  state.tick += 1;
  return synthesizeZoomSnapshot(state);
}

export function synthesizeZoomLeaveSnapshot(state: SyntheticZoomCaptureState): RawCaptureSnapshot {
  state.joined = false;
  state.tick += 1;
  return synthesizeZoomSnapshot(state);
}

export function synthesizeZoomSnapshot(state: SyntheticZoomCaptureState): RawCaptureSnapshot {
  state.tick += 1;
  if (!state.joined) {
    return { meetingState: "idle", participants: [], tick: state.tick };
  }

  return {
    meetingState: "in_meeting",
    activeSpeakerId: "operator-1",
    caption: "",
    tick: state.tick,
    participants: [
      { userId: "operator-1", displayName: state.displayName, role: "Host", videoOn: true, muted: false, talking: true, audioLevel: 76, networkQuality: "good" },
      { userId: "guest-1", displayName: "Guest 1", role: "Guest", videoOn: true, muted: false, talking: false, audioLevel: 22, networkQuality: "good" }
    ]
  };
}

function emptySourceSnapshot(sourceCount: number, elapsedMs: number): NativeMediaCoreFrameSourceSnapshot {
  return {
    adapterId: "synthetic-core",
    kind: "zoom-sdk",
    status: sourceCount > 0 ? "subscribed" : "idle",
    subscribedSourceCount: sourceCount,
    liveFrameCount: sourceCount,
    staleFrameCount: 0,
    droppedFrameCount: 0,
    lowResolutionFrameCount: 0,
    lastFrameTimestampMs: sourceCount > 0 ? elapsedMs : undefined,
    issues: [],
    warnings: []
  };
}

/**
 * Apply a batch of media-core commands and return synthetic health. `frameNumber`
 * is monotonic across calls so the program/compositor counters advance.
 */
function zoomRosterForSync(state: SyntheticZoomCaptureState | undefined): Pick<RawCaptureSnapshot, "meetingState" | "participants" | "activeSpeakerId"> {
  if (!state?.joined) {
    return { meetingState: "idle", participants: [] };
  }

  return {
    meetingState: "in_meeting",
    activeSpeakerId: "operator-1",
    participants: [
      { userId: "operator-1", displayName: state.displayName, role: "Host", videoOn: true, muted: false, talking: true, audioLevel: 76, networkQuality: "good" },
      { userId: "guest-1", displayName: "Guest 1", role: "Guest", videoOn: true, muted: false, talking: false, audioLevel: 22, networkQuality: "good" }
    ]
  };
}

export function synthesizeSnapshot(
  commands: NativeMediaCoreCommand[],
  elapsedMs: number,
  frameNumber: number,
  zoomState?: SyntheticZoomCaptureState
): NativeMediaCoreStateSnapshot {
  const sceneGraph = commands.find((command) => command.type === "load-scene-graph");
  const roster = commands.find((command) => command.type === "set-zoom-source-roster");
  const output = commands.find((command) => command.type === "start-program-output");
  const recordingCommand = commands.find((command) => command.type === "start-recording-session");
  const overlays = commands.filter((command) => command.type === "set-overlay-asset");
  const transforms = commands.filter((command) => command.type === "set-participant-transform");
  const audioMixCommand = commands.find((command) => command.type === "sync-participant-audio-mix");
  const captionCue = commands.find((command) => command.type === "push-caption-cue");
  const captionEnabled = commands.find((command) => command.type === "set-caption-enabled");
  const brandKitCommand = commands.find((command) => command.type === "set-brand-kit");
  const breakoutRoomChange = commands.find((command) => command.type === "simulate-breakout-room-change");
  if (breakoutRoomChange && breakoutRoomChange.type === "simulate-breakout-room-change") {
    breakoutRoomState = {
      breakoutRoomId: breakoutRoomChange.breakoutRoomId.trim() || DEFAULT_BREAKOUT_ROOM.breakoutRoomId,
      breakoutRoomName: breakoutRoomChange.breakoutRoomName.trim() || DEFAULT_BREAKOUT_ROOM.breakoutRoomName
    };
  }

  const sourceCount = roster ? roster.sources.length : 0;
  const routeCount = sceneGraph ? sceneGraph.routes.length : 0;
  const outputs = output ? [...new Set(output.destinations)] : [];
  const sceneId = sceneGraph?.sceneId;

  const renderPlan: NativeMediaCoreRenderPlan = {
    renderPlanId: `synthetic-plan-${frameNumber}`,
    sceneId,
    outputProfile: DEFAULT_OUTPUT_PROFILE,
    colorGrade: NEUTRAL_GRADE,
    sourceCount,
    resolvedRouteCount: routeCount,
    layers: [],
    routes: [],
    warnings: []
  };

  const frames: NativeMediaCoreFrame[] = roster
    ? roster.sources.map((source) => ({
        sourceId: source.sourceId,
        participantId: source.participantId,
        kind: "participant-video",
        frameNumber,
        timestampMs: elapsedMs,
        width: 1280,
        height: 720,
        fps: 60,
        health: "live"
      }))
    : [];

  const programFrame: NativeMediaCoreProgramFrame | undefined = sceneId
    ? {
        frameNumber,
        timestampMs: elapsedMs,
        renderPlanId: renderPlan.renderPlanId,
        sceneId,
        width: DEFAULT_OUTPUT_PROFILE.width,
        height: DEFAULT_OUTPUT_PROFILE.height,
        fps: DEFAULT_OUTPUT_PROFILE.fps,
        layerCount: sourceCount + overlays.length,
        colorGrade: NEUTRAL_GRADE,
        health: "live"
      }
    : undefined;

  const encoding = outputs.length > 0;
  const sourceSnapshot = emptySourceSnapshot(sourceCount, elapsedMs);
  const lastCommandTypes = commands.map((command) => command.type);

  const audioMixSession = new NativeAudioMixSessionSimulator();
  const captionTrack = new NativeCaptionTrackSimulator();
  const brandKitSimulator = new NativeBrandKitSimulator();
  if (audioMixCommand) {
    audioMixSession.sync(audioMixCommand.channels);
  }
  if (captionEnabled) {
    captionTrack.setEnabled(captionEnabled.enabled);
  }
  if (captionCue) {
    captionTrack.pushCue(captionCue.text, captionCue.atMs, captionCue.speaker);
  }
  audioMixSession.mix(frames.length > 0 ? 1 : 0);
  if (brandKitCommand) {
    brandKitSimulator.apply(
      {
        name: brandKitCommand.name,
        logoText: brandKitCommand.logoText,
        brandColor: brandKitCommand.brandColor,
        accentColor: brandKitCommand.accentColor,
        backgroundColor: brandKitCommand.backgroundColor,
        backgroundImageUrl: "",
        fontFamily: brandKitCommand.fontFamily,
        lowerThirdStyle: brandKitCommand.lowerThirdStyle
      },
      overlays.length
    );
  }
  const audioMixSnapshot = audioMixSession.snapshot();
  const captionTrackSnapshot = captionTrack.snapshot();
  const brandKitSnapshot = brandKitSimulator.snapshot();
  const programFrameCount = programFrame ? frameNumber : 0;

  const recording = buildRecordingSession(recordingCommand, elapsedMs, programFrameCount);
  const outputSenderSession = buildOutputSenderSession(outputs, programFrameCount);
  const encoderSession: NativeMediaCoreEncoderSession = {
    status: encoding ? "encoding" : "idle",
    renderPlanId: programFrame?.renderPlanId,
    programFrameCount: programFrame ? 1 : 0,
    targets: outputs.map((destination) => ({
      targetId: `${destination}:program`,
      destination,
      streamKind: "program",
      status: "attached",
      attachedFrameCount: programFrame ? 1 : 0
    })),
    lifecycle: {
      status: encoding ? "encoding" : "idle",
      lastTransition: encoding ? "Program output encoding." : "Encoder idle."
    },
    warnings: []
  };
  const outputHealth = buildOutputHealth(outputs, outputSenderSession, recording, encoderSession.warnings);

  const diagnostics = {
    generatedAtMs: elapsedMs,
    sceneId,
    routeCount,
    frameCount: frames.length,
    programFrameCount,
    outputs,
    outputProfile: DEFAULT_OUTPUT_PROFILE,
    outputHealth,
    outputSenderSession,
    sourceSnapshot,
    renderPlan,
    compositor: {
      status: programFrame ? ("live" as const) : ("idle" as const),
      renderPlanId: programFrame ? renderPlan.renderPlanId : undefined,
      programFrameCount: programFrame ? frameNumber : 0,
      droppedFrameCount: 0,
      degradedFrameCount: 0
    },
    programFrame,
    programTransport: {
      transportId: "in-process-preview" as const,
      status: programFrame ? ("publishing" as const) : ("idle" as const),
      frameNumber: programFrame?.frameNumber,
      renderPlanId: programFrame?.renderPlanId,
      timestampMs: programFrame?.timestampMs,
      latencyMs: 0
    },
    encoderSession,
    recording,
    audioMixSession: audioMixSnapshot,
    captionTrack: captionTrackSnapshot,
    brandKit: brandKitSnapshot,
    operatorActions: [],
    eventLog: [],
    warnings: [...audioMixSnapshot.warnings, ...captionTrackSnapshot.warnings, ...brandKitSnapshot.warnings],
    lastCommandTypes
  };

  return {
    sceneId,
    routeCount,
    frameCount: frames.length,
    frames,
    sourceSnapshot,
    programFrame,
    programFrameCount: diagnostics.programFrameCount,
    programTransport: diagnostics.programTransport,
    compositor: diagnostics.compositor,
    participantTransformCount: transforms.length,
    overlayCount: overlays.length,
    outputs,
    isoParticipantIds: output ? output.isoParticipantIds : [],
    outputProfile: DEFAULT_OUTPUT_PROFILE,
    outputHealth,
    outputSenderSession,
    sourceCount,
    resolvedRouteCount: routeCount,
    renderPlan,
    encoderSession,
    recording,
    audioMixSession: audioMixSnapshot,
    captionTrack: captionTrackSnapshot,
    brandKit: brandKitSnapshot,
    operatorActions: [],
    eventLog: [],
    diagnostics,
    lastCommandTypes,
    warnings: diagnostics.warnings,
    ...(() => {
      const zoomRoster = zoomRosterForSync(zoomState);
      const inMeeting = zoomRoster.meetingState === "in_meeting" || Boolean(sceneId) || sourceCount > 0;
      return {
        meetingState: inMeeting ? "in_meeting" : "idle",
        activeSpeakerId:
          zoomRoster.activeSpeakerId == null ? undefined : String(zoomRoster.activeSpeakerId),
        participants: zoomRoster.participants,
        breakoutRoomId: breakoutRoomState.breakoutRoomId,
        breakoutRoomName: breakoutRoomState.breakoutRoomName
      };
    })()
  };
}

function buildRecordingSession(
  recordingCommand: Extract<NativeMediaCoreCommand, { type: "start-recording-session" }> | undefined,
  elapsedMs: number,
  programFrameCount: number
): NativeMediaCoreRecordingSession | undefined {
  if (!recordingCommand) {
    return undefined;
  }

  const targetFolder = recordingCommand.targetFolder ?? "Recordings";
  const filenamePrefix = recordingCommand.filenamePrefix ?? "program";

  return {
    sessionId: recordingCommand.sessionId ?? "recording",
    active: true,
    status: "recording",
    writerStatus: "writing",
    startedAtMs: Math.max(0, elapsedMs - 1000),
    elapsedMs: Math.max(0, elapsedMs - 1000),
    targetFolder,
    filenamePrefix,
    format: recordingCommand.format ?? "mp4",
    quality: recordingCommand.quality ?? "high",
    encoder: {
      codec: "h264",
      hardwareAccelerated: true,
      targetBitrateMbps: 18
    },
    estimatedDiskRateMBps: 4.99,
    programPath: `${targetFolder}/${filenamePrefix}-program-0.mp4`,
    streams: [],
    totalFramesWritten: programFrameCount,
    totalDroppedFrames: 0,
    totalBytesWritten: programFrameCount > 0 ? 4096 : 0
  };
}

function buildOutputSenderSession(
  outputs: string[],
  programFrameCount: number
): NativeMediaCoreOutputSenderSession {
  const streamDestinations = outputs.filter(
    (destination) => destination !== "recording"
  ) as Array<"rtmp" | "ndi" | "srt" | "webrtc">;

  const senders = streamDestinations.map((destination) => ({
    senderId: `${destination}:program`,
    destination,
    status: "live" as const,
    framesSent: programFrameCount,
    retryCount: 0,
    latencyMs: 2100,
    bitrateMbps: 6
  }));

  return {
    status: senders.length > 0 ? "live" : "idle",
    activeSenderCount: senders.length,
    senders,
    warnings: []
  };
}

function buildOutputHealth(
  outputs: string[],
  senderSession: NativeMediaCoreOutputSenderSession,
  recording: NativeMediaCoreRecordingSession | undefined,
  encoderWarnings: string[]
): NativeMediaCoreOutputHealth[] {
  const health: NativeMediaCoreOutputHealth[] = [];

  for (const destination of outputs) {
    if (destination === "recording") {
      const status = recording?.status === "failed"
        ? "failed"
        : recording?.status === "warning"
          ? "warning"
          : recording?.active
            ? "live"
            : "idle";
      health.push({
        destination: "recording",
        status,
        message: recording?.programPath ?? "Recording idle.",
        droppedFrames: recording?.totalDroppedFrames ?? 0
      });
      continue;
    }

    const sender = senderSession.senders.find((item) => item.destination === destination);
    health.push({
      destination: destination as NativeMediaCoreOutputHealth["destination"],
      status: sender?.status === "failed"
        ? "failed"
        : sender?.status === "warning"
          ? "warning"
          : sender?.status === "live" || sender?.status === "starting"
            ? "live"
            : "idle",
      message: sender?.warning ?? `${destination.toUpperCase()} sender ${sender?.status ?? "idle"}.`,
      droppedFrames: 0
    });
  }

  for (const warning of encoderWarnings) {
    health.push({
      destination: "recording",
      status: "warning",
      message: warning,
      droppedFrames: 0
    });
  }

  return health;
}

/**
 * Synthetic Zoom media spine snapshot for the Node stub core. Mirrors the
 * structure of buildFallbackZoomMediaSpineSnapshot but originates from the
 * child-process side so the renderer knows it came from the native path.
 */
export function synthesizeSpineSnapshot(
  payload: ZoomMediaSpineSyncPayload,
  elapsedMs: number
): ZoomMediaSpineNativeSnapshot {
  const participantsById = new Map(payload.participants.map((p) => [p.sdkUserId, p]));
  const subscriptions: ZoomMediaSpineNativeSubscription[] = payload.subscriptions.map((sub) => {
    const participant = participantsById.get(sub.participantId);
    const videoOff =
      participant && (sub.kind === "participant-video" || sub.kind === "screen-share") && !participant.videoOn;
    const status: ZoomMediaSpineSubscriptionStatus = videoOff ? "failed" : "subscribed";
    const isAudio = sub.kind === "participant-audio";
    return {
      ...sub,
      subscriptionId: `${sub.kind}:${sub.participantId}:${sub.purpose}`,
      displayName: participant?.displayName,
      status,
      lastResultCode: videoOff ? "video-off" : "ok",
      deliveredWidth: sub.kind === "screen-share" ? 1920 : 1280,
      deliveredHeight: sub.kind === "screen-share" ? 1080 : 720,
      deliveredFps: 30,
      framesReceived: isAudio || videoOff ? 0 : Math.max(1, Math.floor(elapsedMs / 33)),
      audioPacketsReceived: isAudio && !videoOff ? Math.max(1, Math.floor(elapsedMs / 20)) : 0
    };
  });

  const activeSpeaker = payload.participants.find((p) => p.talking);
  const screenShare = payload.participants.find((p) => p.sharingScreen);
  const subscribedFeeds = subscriptions.filter(
    (s) => s.status !== "failed" && s.kind === "participant-video"
  ).length;
  const totalAudioPackets = subscriptions.reduce((t, s) => t + s.audioPacketsReceived, 0);

  return {
    meetingState: payload.blocked ? "error" : "in-meeting",
    sdkVersion: payload.readiness.sdkVersion,
    participantCount: payload.participants.length,
    activeSpeakerId: activeSpeaker?.sdkUserId,
    screenShareParticipantId: screenShare?.sdkUserId,
    participants: payload.participants,
    subscriptions,
    recording: {
      evidence: {
        programFramesWritten: subscribedFeeds > 0 ? Math.max(1, Math.floor(elapsedMs / 33)) : 0,
        isoFramesWritten: 0,
        audioPacketsObserved: totalAudioPackets,
        subscribedVideoFeeds: subscribedFeeds
      }
    },
    warnings: [...payload.warnings],
    events: ["zoom-media-spine-sync accepted by Node stub core.", payload.summary]
  };
}
