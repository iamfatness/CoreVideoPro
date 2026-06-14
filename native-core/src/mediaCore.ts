import type {
  MediaCoreCommand,
  MediaCoreDestination,
  MediaCoreDiagnosticsSnapshot,
  MediaCoreOutputHealth,
  MediaCoreOutputProfile,
  MediaCoreRequest,
  MediaCoreResponse,
  MediaCoreStateSnapshot
} from "./protocol.js";
import { FakeFrameProducer, type FakeFrameSource } from "./fakeFrameProducer.js";
import { RecordingSink } from "./recordingSink.js";

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

export class MediaCoreRuntime {
  private sceneGraph?: SceneGraphState;
  private readonly transforms = new Map<string, TransformState>();
  private readonly overlays = new Map<string, OverlayState>();
  private output?: OutputState;
  private outputProfile = DEFAULT_OUTPUT_PROFILE;
  private outputHealth = new Map<MediaCoreDestination, MediaCoreOutputHealth>();
  private lastCommandTypes: string[] = [];
  private readonly frameProducer = new FakeFrameProducer();
  private readonly recordingSink = new RecordingSink();
  private frames = this.frameProducer.render([], 0);
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
    const outputHealth = this.buildOutputHealth(recording);
    const allWarnings = [...new Set([...warnings, recording?.warning, recording?.error].filter(Boolean) as string[])];

    return {
      sceneId: this.sceneGraph?.sceneId,
      routeCount: this.sceneGraph?.routes.length ?? 0,
      frameCount: this.frames.length,
      frames: this.frames,
      participantTransformCount: this.transforms.size,
      overlayCount: this.overlays.size,
      outputs: this.output?.destinations ?? [],
      isoParticipantIds: this.output?.isoParticipantIds ?? [],
      outputProfile: this.outputProfile,
      outputHealth,
      recording,
      diagnostics: this.diagnostics(outputHealth, allWarnings, recording),
      lastCommandTypes: this.lastCommandTypes,
      warnings: allWarnings
    };
  }

  private tick(elapsedMs: number) {
    this.elapsedMs = Math.max(0, elapsedMs);
    this.frames = this.frameProducer.render(this.getFrameSources(), this.elapsedMs);
    this.recordingSink.writeFrames(this.frames, this.elapsedMs);
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

  private buildOutputHealth(recording: MediaCoreStateSnapshot["recording"]) {
    const health = new Map(this.outputHealth);

    if (recording) {
      health.set("recording", {
        destination: "recording",
        status: recording.status === "failed" ? "failed" : recording.status === "warning" ? "warning" : recording.active ? "live" : "idle",
        message: recording.error ?? recording.warning ?? (recording.active ? "Recording writer active." : "Recording stopped."),
        droppedFrames: recording.totalDroppedFrames
      });
    }

    return [...health.values()];
  }

  private diagnostics(
    outputHealth: MediaCoreOutputHealth[],
    warnings: string[],
    recording: MediaCoreStateSnapshot["recording"]
  ): MediaCoreDiagnosticsSnapshot {
    return {
      generatedAtMs: this.elapsedMs,
      sceneId: this.sceneGraph?.sceneId,
      routeCount: this.sceneGraph?.routes.length ?? 0,
      frameCount: this.frames.length,
      outputs: this.output?.destinations ?? [],
      outputProfile: this.outputProfile,
      outputHealth,
      recording,
      warnings,
      lastCommandTypes: this.lastCommandTypes
    };
  }

  private getFrameSources(): FakeFrameSource[] {
    if (!this.sceneGraph) {
      return [];
    }

    return this.sceneGraph.routes
      .map((route): FakeFrameSource | undefined => {
        if (route.mode === "screen-share") {
          return {
            sourceId: `screen-share:${route.routeId}`,
            kind: "screen-share"
          };
        }

        if (route.participantId) {
          return {
            sourceId: `participant:${route.participantId}`,
            participantId: route.participantId,
            kind: "participant-video"
          };
        }

        if (route.mode === "active-speaker") {
          return {
            sourceId: `active-speaker:${route.routeId}`,
            kind: "participant-video"
          };
        }

        return undefined;
      })
      .filter(Boolean) as FakeFrameSource[];
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
