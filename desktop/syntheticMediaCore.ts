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
  NativeMediaCoreFrame,
  NativeMediaCoreFrameSourceSnapshot,
  NativeMediaCoreOutputProfile,
  NativeMediaCoreProfile,
  NativeMediaCoreProgramFrame,
  NativeMediaCoreRenderPlan,
  NativeMediaCoreStateSnapshot
} from "../src/engine/nativeMediaCoreProtocol";

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
    "webrtc-output"
  ]
};

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
export function synthesizeSnapshot(
  commands: NativeMediaCoreCommand[],
  elapsedMs: number,
  frameNumber: number
): NativeMediaCoreStateSnapshot {
  const sceneGraph = commands.find((command) => command.type === "load-scene-graph");
  const roster = commands.find((command) => command.type === "set-zoom-source-roster");
  const output = commands.find((command) => command.type === "start-program-output");
  const overlays = commands.filter((command) => command.type === "set-overlay-asset");
  const transforms = commands.filter((command) => command.type === "set-participant-transform");

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

  const diagnostics = {
    generatedAtMs: elapsedMs,
    sceneId,
    routeCount,
    frameCount: frames.length,
    programFrameCount: programFrame ? frameNumber : 0,
    outputs,
    outputProfile: DEFAULT_OUTPUT_PROFILE,
    outputHealth: [],
    outputSenderSession: { status: "idle" as const, activeSenderCount: 0, senders: [], warnings: [] },
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
    encoderSession: {
      status: encoding ? ("encoding" as const) : ("idle" as const),
      renderPlanId: programFrame?.renderPlanId,
      programFrameCount: programFrame ? 1 : 0,
      targets: outputs.map((destination) => ({
        targetId: `${destination}:program`,
        destination,
        streamKind: "program" as const,
        status: "attached" as const,
        attachedFrameCount: programFrame ? 1 : 0
      })),
      lifecycle: {
        status: encoding ? ("encoding" as const) : ("idle" as const),
        lastTransition: encoding ? "Program output encoding." : "Encoder idle."
      },
      warnings: []
    },
    operatorActions: [],
    eventLog: [],
    warnings: [],
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
    outputHealth: diagnostics.outputHealth,
    outputSenderSession: diagnostics.outputSenderSession,
    sourceCount,
    resolvedRouteCount: routeCount,
    renderPlan,
    encoderSession: diagnostics.encoderSession,
    operatorActions: [],
    eventLog: [],
    diagnostics,
    lastCommandTypes,
    warnings: []
  };
}
