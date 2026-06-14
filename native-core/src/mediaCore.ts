import type {
  MediaCoreCommand,
  MediaCoreColorGrade,
  MediaCoreDestination,
  MediaCoreDiagnosticsSnapshot,
  MediaCoreEncoderSession,
  MediaCoreOutputHealth,
  MediaCoreOutputProfile,
  MediaCoreProgramFrame,
  MediaCoreRenderPlan,
  MediaCoreRequest,
  MediaCoreResponse,
  MediaCoreStateSnapshot,
  MediaCoreZoomSource
} from "./protocol.js";
import { FakeFrameProducer, type FakeFrameSource } from "./fakeFrameProducer.js";
import { RecordingSink } from "./recordingSink.js";
import { buildRenderPlan } from "./renderPlan.js";
import { ProgramCompositor } from "./compositor.js";
import { buildEncoderSession } from "./encoderSession.js";

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
  private readonly frameProducer = new FakeFrameProducer();
  private readonly compositor = new ProgramCompositor();
  private readonly recordingSink = new RecordingSink();
  private frames = this.frameProducer.render([], 0);
  private programFrame?: MediaCoreProgramFrame;
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
        if (command.destinations.length === 0) {
          warnings.push("Program output started without destinations.");
        }
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
      }
    });

    return warnings;
  }

  snapshot(warnings: string[] = []): MediaCoreStateSnapshot {
    const recording = this.recordingSink.snapshot();
    const renderPlan = this.renderPlan();
    const compositor = this.compositor.snapshot();
    const encoderSession = this.encoderSession(recording);
    const outputHealth = this.buildOutputHealth(recording, this.programFrame, encoderSession);
    const allWarnings = [
      ...new Set([...warnings, ...renderPlan.warnings, ...encoderSession.warnings, recording?.warning, recording?.error].filter(Boolean) as string[])
    ];

    return {
      sceneId: this.sceneGraph?.sceneId,
      routeCount: this.sceneGraph?.routes.length ?? 0,
      frameCount: this.frames.length,
      frames: this.frames,
      programFrame: this.programFrame,
      programFrameCount: compositor.programFrameCount,
      compositor,
      participantTransformCount: this.transforms.size,
      overlayCount: this.overlays.size,
      outputs: this.output?.destinations ?? [],
      isoParticipantIds: this.output?.isoParticipantIds ?? [],
      outputProfile: this.outputProfile,
      outputHealth,
      sourceCount: this.sources.length,
      resolvedRouteCount: renderPlan.resolvedRouteCount,
      renderPlan,
      encoderSession,
      recording,
      diagnostics: this.diagnostics(outputHealth, allWarnings, recording, renderPlan, compositor, encoderSession),
      lastCommandTypes: this.lastCommandTypes,
      warnings: allWarnings
    };
  }

  private tick(elapsedMs: number) {
    this.elapsedMs = Math.max(0, elapsedMs);
    const renderPlan = this.renderPlan();
    this.frames = this.frameProducer.render(this.getFrameSources(renderPlan), this.elapsedMs);
    this.programFrame = this.compositor.compose(renderPlan, this.elapsedMs);
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

  private buildOutputHealth(recording: MediaCoreStateSnapshot["recording"], programFrame: MediaCoreProgramFrame | undefined, encoderSession: MediaCoreEncoderSession) {
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

    if (recording) {
      health.set("recording", {
        destination: "recording",
        status:
          recording.status === "failed" || encoderSession.status === "failed"
            ? "failed"
            : recording.status === "warning" || encoderSession.status === "warning" || programFrame?.health === "degraded"
              ? "warning"
              : recording.active
                ? "live"
                : "idle",
        message: recording.error ?? recording.warning ?? encoderSession.warnings[0] ?? (recording.active ? "Recording writer active." : "Recording stopped."),
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
    encoderSession: MediaCoreEncoderSession
  ): MediaCoreDiagnosticsSnapshot {
    return {
      generatedAtMs: this.elapsedMs,
      sceneId: this.sceneGraph?.sceneId,
      routeCount: this.sceneGraph?.routes.length ?? 0,
      frameCount: this.frames.length,
      outputs: this.output?.destinations ?? [],
      outputProfile: this.outputProfile,
      outputHealth,
      renderPlan,
      compositor,
      programFrame: this.programFrame,
      programFrameCount: compositor.programFrameCount,
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
      recording
    });
  }

  private getFrameSources(renderPlan: MediaCoreRenderPlan): FakeFrameSource[] {
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
