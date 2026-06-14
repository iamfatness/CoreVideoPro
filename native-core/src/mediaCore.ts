import type { MediaCoreCommand, MediaCoreRequest, MediaCoreResponse, MediaCoreStateSnapshot } from "./protocol.js";
import { FakeFrameProducer, type FakeFrameSource } from "./fakeFrameProducer.js";

type SceneGraphState = Extract<MediaCoreCommand, { type: "load-scene-graph" }>;
type TransformState = Extract<MediaCoreCommand, { type: "set-participant-transform" }>;
type OverlayState = Extract<MediaCoreCommand, { type: "set-overlay-asset" }>;
type OutputState = Extract<MediaCoreCommand, { type: "start-program-output" }>;

export class MediaCoreRuntime {
  private sceneGraph?: SceneGraphState;
  private readonly transforms = new Map<string, TransformState>();
  private readonly overlays = new Map<string, OverlayState>();
  private output?: OutputState;
  private lastCommandTypes: string[] = [];
  private readonly frameProducer = new FakeFrameProducer();
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

      if (command.type === "start-program-output") {
        this.output = command;
        if (command.destinations.length === 0) {
          warnings.push("Program output started without destinations.");
        }
      }
    });

    return warnings;
  }

  snapshot(warnings: string[] = []): MediaCoreStateSnapshot {
    return {
      sceneId: this.sceneGraph?.sceneId,
      routeCount: this.sceneGraph?.routes.length ?? 0,
      frameCount: this.frames.length,
      frames: this.frames,
      participantTransformCount: this.transforms.size,
      overlayCount: this.overlays.size,
      outputs: this.output?.destinations ?? [],
      isoParticipantIds: this.output?.isoParticipantIds ?? [],
      lastCommandTypes: this.lastCommandTypes,
      warnings
    };
  }

  private tick(elapsedMs: number) {
    this.elapsedMs = Math.max(0, elapsedMs);
    this.frames = this.frameProducer.render(this.getFrameSources(), this.elapsedMs);
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
