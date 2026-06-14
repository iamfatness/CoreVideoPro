import type {
  MediaCoreCommand,
  MediaCoreColorGrade,
  MediaCoreDestination,
  MediaCoreDiagnosticsSnapshot,
  MediaCoreEncoderLifecycle,
  MediaCoreEncoderSession,
  MediaCoreFrame,
  MediaCoreFrameSourceSnapshot,
  MediaCoreOutputHealth,
  MediaCoreOutputProfile,
  MediaCoreOutputSenderSession,
  MediaCoreProgramFrame,
  MediaCoreProgramFrameTransport,
  MediaCoreRenderPlan,
  MediaCoreRequest,
  MediaCoreResponse,
  MediaCoreStateSnapshot,
  MediaCoreZoomSource
} from "./protocol.js";
import { TestPatternMediaSource, createMediaFrameSource, type MediaCoreFrameSourceRequest, type MediaFrameSource } from "./mediaSource.js";
import { RecordingSink } from "./recordingSink.js";
import { buildRenderPlan } from "./renderPlan.js";
import { ProgramCompositor } from "./compositor.js";
import { buildEncoderSession } from "./encoderSession.js";
import { OutputSenderSessionModel } from "./outputSenderSession.js";

type SceneGraphState = Extract<MediaCoreCommand, { type: "load-scene-graph" }>;
type TransformState = Extract<MediaCoreCommand, { type: "set-participant-transform" }>;
type OverlayState = Extract<MediaCoreCommand, { type: "set-overlay-asset" }>;
type OutputState = Extract<MediaCoreCommand, { type: "start-program-output" }>;

const DEFAULT_OUTPUT_PROFILE: MediaCoreOutputProfile = {
  profileId: "1080p60",
  resolution: "1920x1080",
  width: 1920,
  height: 1080,
  fps: 60,
  targetBitrateMbps: 8
};

const DEFAULT_COLOR_GRADE: MediaCoreColorGrade = {
  lut: "none",
  exposure: 0,
  contrast: 0,
  saturation: 0,
  temperature: 0
};

export class MediaCoreRuntime {
  private mediaSource: MediaFrameSource;

  constructor(mediaSource: MediaFrameSource = new TestPatternMediaSource()) {
    this.mediaSource = mediaSource;
    this.sourceSnapshot = mediaSource.snapshot(0);
  }

  private sceneGraph?: SceneGraphState;
  private sources: MediaCoreZoomSource[] = [];
  private activeSpeakerId?: string;
  private screenShareParticipantId?: string;
  private readonly transforms = new Map<string, TransformState>();
  private readonly overlays = new Map<string, OverlayState>();
  private output?: OutputState;
  private outputProfile = DEFAULT_OUTPUT_PROFILE;
  private colorGrade = DEFAULT_COLOR_GRADE;
  private outputHealth = new Map<MediaCoreDestination, MediaCoreOutputHealth>();
  private lastCommandTypes: string[] = [];
  private readonly compositor = new ProgramCompositor();
  private readonly outputSenderSessionModel = new OutputSenderSessionModel();
  private readonly recordingSink = new RecordingSink();
  private frames: MediaCoreFrame[] = [];
  private sourceSnapshot: MediaCoreFrameSourceSnapshot;
  private programFrame?: MediaCoreProgramFrame;
  private programTransport: MediaCoreProgramFrameTransport = {
    transportId: "in-process-preview",
    status: "idle",
    latencyMs: 0,
    warning: "No program frame has been published."
  };
  private encoderLifecycle: MediaCoreEncoderLifecycle = {
    status: "idle",
    lastTransition: "Encoder session idle."
  };
  private elapsedMs = 0;

  handle(request: MediaCoreRequest): MediaCoreResponse {
    if (request.type === "snapshot") {
      return {
        id: request.id,
        ok: true,
        state: this.snapshot()
      };
    }

    if (request.type === "tick") {
      this.tick(request.elapsedMs);
      return {
        id: request.id,
        ok: true,
        appliedCommandCount: 0,
        state: this.snapshot()
      };
    }

    const warnings = this.apply(request.commands);
    this.tick(this.elapsedMs);

    return {
      id: request.id,
      ok: true,
      appliedCommandCount: request.commands.length,
      state: this.snapshot(warnings)
    };
  }

  apply(commands: MediaCoreCommand[]) {
    const warnings: string[] = [];
    this.lastCommandTypes = commands.map((command) => command.type);
    const outputCommand = commands.find((command) => command.type === "start-program-output");

    if (!outputCommand) {
      this.output = undefined;
      this.outputHealth.clear();
      if (this.encoderLifecycle.status === "encoding" || this.encoderLifecycle.status === "prepared") {
        this.encoderLifecycle = {
          ...this.encoderLifecycle,
          status: "stopped",
          stoppedAtMs: this.elapsedMs,
          lastTransition: "Encoder session stopped because no program outputs are active."
        };
      }
    }

    commands.forEach((command) => {
      if (command.type === "load-scene-graph") {
        this.sceneGraph = command;
        if (command.routes.length === 0) {
          warnings.push("Scene graph loaded without routes.");
        }
        return;
      }

      if (command.type === "set-participant-transform") {
        this.transforms.set(command.participantId, normalizeTransform(command));
        if (command.crop.width <= 0 || command.crop.height <= 0) {
          warnings.push(`${command.participantId} transform has an empty crop region.`);
        }
        return;
      }

      if (command.type === "set-overlay-asset") {
        this.overlays.set(command.overlayId, command);
        if (!command.text && !command.imageUri) {
          warnings.push(`${command.overlayId} overlay has no text or image asset.`);
        }
        return;
      }

      if (command.type === "set-zoom-source-roster") {
        this.sources = normalizeSources(command.sources);
        this.activeSpeakerId = command.sources.find((source) => source.isActiveSpeaker)?.participantId ?? this.activeSpeakerId;
        this.screenShareParticipantId = command.sources.find((source) => source.isScreenSharing)?.participantId ?? this.screenShareParticipantId;
        if (command.sources.length === 0) {
          warnings.push("Zoom source roster is empty.");
        }
        return;
      }

      if (command.type === "set-media-source-adapter") {
        this.mediaSource = createMediaFrameSource(command.kind, command.adapterId);
        this.frames = [];
        this.sourceSnapshot = this.mediaSource.snapshot(this.elapsedMs);
        return;
      }

      if (command.type === "set-active-speaker") {
        this.activeSpeakerId = command.participantId;
        if (command.participantId && !this.sources.some((source) => source.participantId === command.participantId)) {
          warnings.push(`${command.participantId} active speaker is not present in the Zoom source roster.`);
        }
        return;
      }

      if (command.type === "set-screen-share-source") {
        this.screenShareParticipantId = command.participantId;
        if (command.participantId && !this.sources.some((source) => source.participantId === command.participantId && source.isScreenSharing)) {
          warnings.push(`${command.participantId} is not publishing a screen share source.`);
        }
        return;
      }

      if (command.type === "set-color-grade") {
        this.colorGrade = normalizeColorGrade(command);
        return;
      }

      if (command.type === "set-output-profile") {
        this.outputProfile = normalizeOutputProfile(command);
        if (this.outputProfile.width > 1920 || this.outputProfile.height > 1080) {
          warnings.push(`${this.outputProfile.resolution} output profile requires 4K-capable GPU encoding.`);
        }
        return;
      }

      if (command.type === "start-program-output") {
        this.output = command;
        this.syncOutputHealth(command.destinations);
        if (command.destinations.length > 0 && this.encoderLifecycle.status !== "encoding") {
          this.encoderLifecycle = {
            status: "encoding",
            preparedAtMs: this.encoderLifecycle.preparedAtMs ?? this.elapsedMs,
            startedAtMs: this.elapsedMs,
            lastTransition: "Program output encoder session started."
          };
        }
        if (command.destinations.length === 0) {
          warnings.push("Program output started without destinations.");
        }
        return;
      }

      if (command.type === "prepare-encoder-session") {
        this.encoderLifecycle = {
          status: "prepared",
          preparedAtMs: command.preparedAtMs ?? this.elapsedMs,
          lastTransition: command.reason ?? "Encoder session prepared."
        };
        return;
      }

      if (command.type === "start-encoder-session") {
        this.encoderLifecycle = {
          ...this.encoderLifecycle,
          status: "encoding",
          preparedAtMs: this.encoderLifecycle.preparedAtMs ?? command.startedAtMs ?? this.elapsedMs,
          startedAtMs: command.startedAtMs ?? this.elapsedMs,
          lastTransition: "Encoder session started."
        };
        return;
      }

      if (command.type === "stop-encoder-session") {
        this.encoderLifecycle = {
          ...this.encoderLifecycle,
          status: "stopped",
          stoppedAtMs: command.stoppedAtMs ?? this.elapsedMs,
          lastTransition: command.reason ?? "Encoder session stopped."
        };
        return;
      }

      if (command.type === "fail-output-sender") {
        this.outputSenderSessionModel.fail(command.destination, command.message, command.failedAtMs ?? this.elapsedMs);
        warnings.push(command.message);
        return;
      }

      if (command.type === "recover-output-sender") {
        this.outputSenderSessionModel.recover(command.destination, command.recoveredAtMs ?? this.elapsedMs, command.reason);
        return;
      }

      if (command.type === "set-recording-targets") {
        const recording = this.recordingSink.setTargets(command);
        if (recording?.warning) {
          warnings.push(recording.warning);
        }
        return;
      }

      if (command.type === "start-recording-session") {
        const recording = this.recordingSink.start(command, command.startedAtMs ?? this.elapsedMs, command.sessionId);
        if (recording?.warning) {
          warnings.push(recording.warning);
        }
        this.outputHealth.set("recording", {
          destination: "recording",
          status: recording?.status === "warning" ? "warning" : "live",
          message: recording?.warning ?? "Recording writer active.",
          droppedFrames: recording?.totalDroppedFrames ?? 0
        });
        return;
      }

      if (command.type === "stop-recording-session") {
        const recording = this.recordingSink.stop(this.elapsedMs);
        this.outputHealth.set("recording", {
          destination: "recording",
          status: "idle",
          message: command.reason ?? "Recording stopped.",
          droppedFrames: recording?.totalDroppedFrames ?? 0
        });
        return;
      }

      if (command.type === "fail-recording-session") {
        const recording = this.recordingSink.fail(command.message, this.elapsedMs);
        warnings.push(command.message);
        this.outputHealth.set("recording", {
          destination: "recording",
          status: "failed",
          message: command.message,
          droppedFrames: recording?.totalDroppedFrames ?? 0
        });
        return;
      }

      if (command.type === "recover-recording-session") {
        const recording = this.recordingSink.recover(command.recoveredAtMs ?? this.elapsedMs);
        this.outputHealth.set("recording", {
          destination: "recording",
          status: recording?.status === "warning" ? "warning" : "live",
          message: command.reason ?? recording?.warning ?? "Recording writer recovered.",
          droppedFrames: recording?.totalDroppedFrames ?? 0
        });
      }
    });

    return warnings;
  }

  snapshot(warnings: string[] = []): MediaCoreStateSnapshot {
    const recording = this.recordingSink.snapshot();
    const renderPlan = this.renderPlan();
    const compositor = this.compositor.snapshot();
    const encoderSession = this.encoderSession(recording);
    const outputSenderSession = this.outputSenderSession();
    const outputHealth = this.buildOutputHealth(recording, this.programFrame, encoderSession, outputSenderSession);
    const allWarnings = [
      ...new Set([
        ...warnings,
        ...this.sourceSnapshot.warnings,
        ...renderPlan.warnings,
        ...encoderSession.warnings,
        ...outputSenderSession.warnings,
        recording?.warning,
        recording?.error
      ].filter(Boolean) as string[])
    ];

    return {
      sceneId: this.sceneGraph?.sceneId,
      routeCount: this.sceneGraph?.routes.length ?? 0,
      frameCount: this.frames.length,
      frames: this.frames,
      sourceSnapshot: this.sourceSnapshot,
      programFrame: this.programFrame,
      programFrameCount: compositor.programFrameCount,
      programTransport: this.programTransport,
      compositor,
      participantTransformCount: this.transforms.size,
      overlayCount: this.overlays.size,
      outputs: this.output?.destinations ?? [],
      isoParticipantIds: this.output?.isoParticipantIds ?? [],
      outputProfile: this.outputProfile,
      outputHealth,
      outputSenderSession,
      sourceCount: this.sources.length,
      resolvedRouteCount: renderPlan.resolvedRouteCount,
      renderPlan,
      encoderSession,
      recording,
      diagnostics: this.diagnostics(outputHealth, allWarnings, recording, renderPlan, compositor, encoderSession, outputSenderSession),
      lastCommandTypes: this.lastCommandTypes,
      warnings: allWarnings
    };
  }

  private tick(elapsedMs: number) {
    this.elapsedMs = Math.max(0, elapsedMs);
    const renderPlan = this.renderPlan();
    const sourceResult = this.mediaSource.render(this.getFrameSources(renderPlan), this.elapsedMs);
    this.frames = sourceResult.frames;
    this.sourceSnapshot = sourceResult.snapshot;
    this.programFrame = this.compositor.compose(renderPlan, this.elapsedMs);
    this.programTransport = this.buildProgramTransport(this.programFrame);
    this.recordingSink.writeFrames(this.frames, this.elapsedMs, this.programFrame);
  }

  private renderPlan() {
    return buildRenderPlan({
      sceneGraph: this.sceneGraph,
      sources: this.sources,
      activeSpeakerId: this.activeSpeakerId,
      screenShareParticipantId: this.screenShareParticipantId,
      overlays: [...this.overlays.values()],
      outputProfile: this.outputProfile,
      colorGrade: this.colorGrade
    });
  }

  private syncOutputHealth(destinations: MediaCoreDestination[]) {
    this.outputHealth.clear();
    destinations.forEach((destination) => {
      this.outputHealth.set(destination, {
        destination,
        status: "live",
        message:
          destination === "recording"
            ? `Recording output armed at ${this.outputProfile.resolution}${this.outputProfile.fps}.`
            : `${destination.toUpperCase()} output active at ${this.outputProfile.resolution}${this.outputProfile.fps}.`,
        droppedFrames: 0
      });
    });
  }

  private buildOutputHealth(
    recording: MediaCoreStateSnapshot["recording"],
    programFrame: MediaCoreProgramFrame | undefined,
    encoderSession: MediaCoreEncoderSession,
    outputSenderSession: MediaCoreOutputSenderSession
  ) {
    const health = new Map(this.outputHealth);

    if (programFrame?.health === "degraded") {
      health.forEach((value, destination) => {
        health.set(destination, {
          ...value,
          status: value.status === "failed" ? "failed" : "warning",
          message: programFrame.warning ?? "Program frame is degraded."
        });
      });
    }

    if ((this.output?.destinations.length ?? 0) > 0 && encoderSession.lifecycle.status !== "encoding") {
      health.forEach((value, destination) => {
        health.set(destination, {
          ...value,
          status: value.status === "failed" ? "failed" : "warning",
          message: encoderSession.lifecycle.lastTransition
        });
      });
    }

    outputSenderSession.senders
      .filter((sender) => encoderSession.lifecycle.status === "encoding" && sender.status !== "stopped" && sender.status !== "idle")
      .forEach((sender) => {
        health.set(sender.destination, {
          destination: sender.destination,
          status: sender.status === "failed" ? "failed" : sender.status === "warning" || sender.status === "starting" ? "warning" : sender.status === "live" ? "live" : "idle",
          message: sender.warning ?? `${sender.destination.toUpperCase()} sender ${sender.status}.`,
          droppedFrames: 0
        });
      });

    if (recording) {
      health.set("recording", {
        destination: "recording",
        status:
          recording.status === "failed" || encoderSession.status === "failed"
            ? "failed"
            : recording.status === "warning" ||
                encoderSession.status === "warning" ||
                programFrame?.health === "degraded" ||
                ((this.output?.destinations.length ?? 0) > 0 && encoderSession.lifecycle.status !== "encoding")
              ? "warning"
              : recording.active
                ? "live"
                : "idle",
        message:
          recording.error ??
          recording.warning ??
          encoderSession.warnings[0] ??
          (encoderSession.lifecycle.status !== "encoding" && (this.output?.destinations.length ?? 0) > 0 ? encoderSession.lifecycle.lastTransition : undefined) ??
          (recording.active ? "Recording writer active." : "Recording stopped."),
        droppedFrames: recording.totalDroppedFrames
      });
    }

    return [...health.values()];
  }

  private diagnostics(
    outputHealth: MediaCoreOutputHealth[],
    warnings: string[],
    recording: MediaCoreStateSnapshot["recording"],
    renderPlan: MediaCoreRenderPlan,
    compositor: MediaCoreStateSnapshot["compositor"],
    encoderSession: MediaCoreEncoderSession,
    outputSenderSession: MediaCoreOutputSenderSession
  ): MediaCoreDiagnosticsSnapshot {
    return {
      generatedAtMs: this.elapsedMs,
      sceneId: this.sceneGraph?.sceneId,
      routeCount: this.sceneGraph?.routes.length ?? 0,
      frameCount: this.frames.length,
      outputs: this.output?.destinations ?? [],
      outputProfile: this.outputProfile,
      outputHealth,
      outputSenderSession,
      sourceSnapshot: this.sourceSnapshot,
      renderPlan,
      compositor,
      programFrame: this.programFrame,
      programFrameCount: compositor.programFrameCount,
      programTransport: this.programTransport,
      encoderSession,
      recording,
      warnings,
      lastCommandTypes: this.lastCommandTypes
    };
  }

  private encoderSession(recording: MediaCoreStateSnapshot["recording"]) {
    return buildEncoderSession({
      outputs: this.output?.destinations ?? [],
      programFrame: this.programFrame,
      recording,
      lifecycle: this.encoderLifecycle
    });
  }

  private outputSenderSession() {
    return this.outputSenderSessionModel.sync(this.output?.destinations ?? [], this.programFrame, this.outputProfile, this.elapsedMs);
  }

  private buildProgramTransport(programFrame?: MediaCoreProgramFrame): MediaCoreProgramFrameTransport {
    if (!programFrame) {
      return {
        transportId: "in-process-preview",
        status: "idle",
        latencyMs: 0,
        warning: "No program frame has been published."
      };
    }

    return {
      transportId: "in-process-preview",
      status: programFrame.health === "live" ? "publishing" : "degraded",
      frameNumber: programFrame.frameNumber,
      renderPlanId: programFrame.renderPlanId,
      timestampMs: programFrame.timestampMs,
      latencyMs: Math.max(0, this.elapsedMs - programFrame.timestampMs),
      warning:
        programFrame.health === "dropped"
          ? "Program transport skipped a dropped frame."
          : programFrame.health === "degraded"
            ? programFrame.warning ?? "Program transport is publishing degraded frames."
            : undefined
    };
  }

  private getFrameSources(renderPlan: MediaCoreRenderPlan): MediaCoreFrameSourceRequest[] {
    return renderPlan.layers
      .filter((layer): layer is typeof layer & { kind: "participant-video" | "screen-share" } => layer.kind === "participant-video" || layer.kind === "screen-share")
      .map((layer) => ({
        sourceId: layer.sourceId ?? layer.layerId,
        participantId: layer.participantId,
        kind: layer.kind
      }));
  }
}

function normalizeOutputProfile(profile: MediaCoreOutputProfile): MediaCoreOutputProfile {
  const [parsedWidth, parsedHeight] = profile.resolution.split("x").map((part) => Number(part));
  const width = Number.isFinite(parsedWidth) && parsedWidth > 0 ? parsedWidth : profile.width;
  const height = Number.isFinite(parsedHeight) && parsedHeight > 0 ? parsedHeight : profile.height;

  return {
    ...profile,
    width,
    height,
    fps: Math.max(1, profile.fps),
    targetBitrateMbps: Math.max(0, profile.targetBitrateMbps)
  };
}

function normalizeColorGrade(colorGrade: MediaCoreColorGrade): MediaCoreColorGrade {
  return {
    lut: colorGrade.lut,
    exposure: clampRange(colorGrade.exposure, -100, 100),
    contrast: clampRange(colorGrade.contrast, -100, 100),
    saturation: clampRange(colorGrade.saturation, -100, 100),
    temperature: clampRange(colorGrade.temperature, -100, 100)
  };
}

function normalizeSources(sources: MediaCoreZoomSource[]) {
  return sources.map((source) => ({
    ...source,
    hasVideo: source.hasVideo && source.health !== "video-off",
    hasAudio: source.hasAudio,
    audioLevel: clampRange(source.audioLevel, 0, 100)
  }));
}

function clampRange(value: number, min: number, max: number) {
  return Math.min(max, Math.max(min, value));
}

function normalizeTransform(command: TransformState): TransformState {
  return {
    ...command,
    crop: {
      x: clamp01(command.crop.x),
      y: clamp01(command.crop.y),
      width: clamp01(command.crop.width),
      height: clamp01(command.crop.height)
    },
    scale: Math.max(0.1, command.scale)
  };
}

function clamp01(value: number) {
  return Math.min(1, Math.max(0, value));
}
