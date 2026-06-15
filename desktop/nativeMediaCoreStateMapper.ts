/**
 * Maps the compact wire state returned by `corevideo-native.exe` into the full
 * `NativeMediaCoreStateSnapshot` shape the renderer expects. The Node stub core
 * already emits the full snapshot; this adapter keeps the C++ core slim while
 * preserving the typed boundary contract.
 */
import type {
  NativeMediaCoreCommand,
  NativeMediaCoreEncoderSession,
  NativeMediaCoreOutputHealth,
  NativeMediaCoreOutputSenderSession,
  NativeMediaCoreProfile,
  NativeMediaCoreRecordingSession,
  NativeMediaCoreStateSnapshot
} from "../src/engine/nativeMediaCoreProtocol";
import { synthesizeSnapshot } from "./syntheticMediaCore.ts";

export type NativeMediaCoreWireState = {
  sceneId?: string;
  routeCount?: number;
  transformCount?: number;
  overlayCount?: number;
  outputs?: string[];
  isoParticipantIds?: string[];
  encoder?: string;
  codec?: string;
  hardwareEncoder?: boolean;
  active?: boolean;
  programFrameCount?: number;
  renderPlanId?: string;
  compositorRenderer?: string;
  encoderSession?: NativeMediaCoreEncoderSession;
  outputSenderSession?: NativeMediaCoreOutputSenderSession;
  recording?: NativeMediaCoreRecordingSession;
  health?: {
    status?: string;
    renderer?: string;
    encoder?: string;
    codec?: string;
    hardwareEncoder?: boolean;
    recordingArtifactPath?: string;
    recordingBytesWritten?: number;
    encodedFrameCount?: number;
    frameCount?: number;
    messages?: string[];
  };
  profile?: NativeMediaCoreProfile;
};

function asDestination(value: string): "rtmp" | "ndi" | "srt" | "webrtc" | "recording" | undefined {
  if (value === "rtmp" || value === "ndi" || value === "srt" || value === "webrtc" || value === "recording") {
    return value;
  }
  return undefined;
}

function buildOutputHealth(
  outputs: Array<"rtmp" | "ndi" | "srt" | "webrtc" | "recording">,
  senderSession: NativeMediaCoreOutputSenderSession | undefined,
  recording: NativeMediaCoreRecordingSession | undefined,
  encoderWarnings: string[]
): NativeMediaCoreOutputHealth[] {
  const health: NativeMediaCoreOutputHealth[] = [];

  for (const destination of outputs) {
    if (destination === "recording") {
      const status =
        recording?.status === "failed"
          ? "failed"
          : recording?.status === "warning" || recording?.writerStatus === "warning"
            ? "warning"
            : recording?.active
              ? "live"
              : "idle";
      health.push({
        destination,
        status,
        message:
          recording?.error ?? recording?.warning ?? recording?.programPath ?? "Recording idle.",
        droppedFrames: recording?.totalDroppedFrames ?? 0
      });
      continue;
    }

    const sender = senderSession?.senders.find((item) => item.destination === destination);
    health.push({
      destination,
      status:
        sender?.status === "failed"
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

  encoderWarnings.forEach((warning) => {
    health.push({
      destination: "recording",
      status: "warning",
      message: warning,
      droppedFrames: 0
    });
  });

  return health;
}

export function mapNativeWireStateToSnapshot(
  commands: NativeMediaCoreCommand[],
  elapsedMs: number,
  frameNumber: number,
  wire: NativeMediaCoreWireState
): NativeMediaCoreStateSnapshot {
  const base = synthesizeSnapshot(commands, elapsedMs, frameNumber);
  const outputs = (wire.outputs ?? base.outputs).map(asDestination).filter((value): value is NonNullable<typeof value> => Boolean(value));
  const programFrameCount = Math.max(
    wire.programFrameCount ?? 0,
    wire.health?.frameCount ?? 0,
    wire.encoderSession?.programFrameCount ?? 0,
    base.programFrameCount
  );
  const encoderSession = wire.encoderSession ?? base.encoderSession;
  const outputSenderSession = wire.outputSenderSession ?? base.outputSenderSession;
  const recording = wire.recording
    ? {
        ...wire.recording,
        totalBytesWritten: Math.max(wire.recording.totalBytesWritten, wire.health?.recordingBytesWritten ?? 0)
      }
    : wire.health?.recordingArtifactPath && base.recording
      ? {
          ...base.recording,
          totalBytesWritten: Math.max(base.recording.totalBytesWritten, wire.health.recordingBytesWritten ?? 0)
        }
      : base.recording;

  const compositorRenderer = wire.compositorRenderer ?? wire.health?.renderer ?? wire.profile?.renderer ?? "software";
  const compositorStatus =
    programFrameCount > 0 ? ("live" as const) : compositorRenderer === "software" ? base.compositor.status : ("idle" as const);
  const warnings = [...base.warnings];
  if (wire.health?.recordingArtifactPath && recording?.active) {
    warnings.unshift(`Recording artifact: ${wire.health.recordingArtifactPath}`);
  }
  if (wire.health?.messages?.length) {
    warnings.push(...wire.health.messages.filter((message) => message && !warnings.includes(message)));
  }

  const outputHealth = buildOutputHealth(outputs, outputSenderSession, recording, encoderSession.warnings ?? []);

  const programFrame = base.programFrame
    ? {
        ...base.programFrame,
        frameNumber: programFrameCount || base.programFrame.frameNumber,
        renderPlanId: wire.renderPlanId || base.programFrame.renderPlanId
      }
    : programFrameCount > 0
      ? {
          frameNumber: programFrameCount,
          timestampMs: elapsedMs,
          renderPlanId: wire.renderPlanId ?? `native-plan-${frameNumber}`,
          sceneId: wire.sceneId ?? base.sceneId,
          width: base.outputProfile.width,
          height: base.outputProfile.height,
          fps: base.outputProfile.fps,
          layerCount: base.renderPlan.layers.length,
          colorGrade: base.renderPlan.colorGrade,
          health: "live" as const
        }
      : undefined;

  const diagnostics = {
    ...base.diagnostics,
    generatedAtMs: elapsedMs,
    sceneId: wire.sceneId ?? base.sceneId,
    routeCount: wire.routeCount ?? base.routeCount,
    frameCount: base.frameCount,
    programFrameCount,
    outputs,
    outputHealth,
    outputSenderSession,
    compositor: {
      ...base.compositor,
      status: compositorStatus,
      renderPlanId: programFrame?.renderPlanId,
      programFrameCount,
      lastFrame: programFrame
    },
    programFrame,
    programTransport: {
      ...base.programTransport,
      status: programFrame ? ("publishing" as const) : ("idle" as const),
      frameNumber: programFrame?.frameNumber,
      renderPlanId: programFrame?.renderPlanId,
      timestampMs: programFrame?.timestampMs
    },
    encoderSession,
    recording,
    warnings
  };

  return {
    ...base,
    sceneId: wire.sceneId ?? base.sceneId,
    routeCount: wire.routeCount ?? base.routeCount,
    participantTransformCount: wire.transformCount ?? base.participantTransformCount,
    overlayCount: wire.overlayCount ?? base.overlayCount,
    outputs,
    isoParticipantIds: wire.isoParticipantIds ?? base.isoParticipantIds,
    programFrame,
    programFrameCount,
    compositor: diagnostics.compositor,
    programTransport: diagnostics.programTransport,
    encoderSession,
    outputSenderSession,
    outputHealth,
    recording,
    diagnostics,
    warnings
  };
}

export function isNativeMediaCoreWireState(value: unknown): value is NativeMediaCoreWireState {
  return typeof value === "object" && value !== null && "health" in value && "profile" in value && !("diagnostics" in value);
}