import type { ProductionState } from "../domain/production";
import { buildNativeMediaCoreCommands } from "./nativeMediaCoreCommands";
import type { NativeHostBridge } from "./nativeHostBridge";
import type { NativeMediaCoreCommand, NativeMediaCoreFrame, NativeMediaCoreStateSnapshot } from "./nativeMediaCoreProtocol";

export interface MediaCoreSyncEngine {
  syncProduction(state: ProductionState, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot>;
}

export class InMemoryMediaCoreSyncEngine implements MediaCoreSyncEngine {
  private readonly frameNumbers = new Map<string, number>();

  async syncProduction(state: ProductionState, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot> {
    return this.snapshot(buildNativeMediaCoreCommands(state), elapsedMs);
  }

  protected snapshot(commands: NativeMediaCoreCommand[], elapsedMs: number, warnings: string[] = []): NativeMediaCoreStateSnapshot {
    const sceneGraph = commands.find((command) => command.type === "load-scene-graph");
    const output = commands.find((command) => command.type === "start-program-output");
    const transforms = commands.filter((command) => command.type === "set-participant-transform");
    const overlays = commands.filter((command) => command.type === "set-overlay-asset");
    const frames = sceneGraph?.routes.map((route, index) => this.frameFromRoute(route, index, elapsedMs)).filter(Boolean) as NativeMediaCoreFrame[] | undefined;

    return {
      sceneId: sceneGraph?.sceneId,
      routeCount: sceneGraph?.routes.length ?? 0,
      frameCount: frames?.length ?? 0,
      frames: frames ?? [],
      participantTransformCount: transforms.length,
      overlayCount: overlays.length,
      outputs: output?.destinations ?? [],
      isoParticipantIds: output?.isoParticipantIds ?? [],
      lastCommandTypes: commands.map((command) => command.type),
      warnings
    };
  }

  private frameFromRoute(route: Extract<NativeMediaCoreCommand, { type: "load-scene-graph" }>["routes"][number], index: number, elapsedMs: number) {
    if (route.mode === "none") {
      return undefined;
    }

    const isScreenShare = route.mode === "screen-share";
    const sourceId = isScreenShare
      ? `screen-share:${route.routeId}`
      : route.participantId
        ? `participant:${route.participantId}`
        : `${route.mode}:${route.routeId}`;
    const nextFrameNumber = (this.frameNumbers.get(sourceId) ?? 0) + 1;
    this.frameNumbers.set(sourceId, nextFrameNumber);

    return {
      sourceId,
      participantId: route.participantId,
      kind: isScreenShare ? "screen-share" : "participant-video",
      frameNumber: nextFrameNumber,
      timestampMs: elapsedMs,
      width: isScreenShare ? 1920 : index >= 4 ? 960 : 1280,
      height: isScreenShare ? 1080 : index >= 4 ? 540 : 720,
      fps: isScreenShare ? 30 : 60,
      health: nextFrameNumber % 90 === 0 ? "dropped" : index >= 4 && !isScreenShare ? "low-resolution" : "live"
    } satisfies NativeMediaCoreFrame;
  }
}

export class NativeHostMediaCoreSyncEngine extends InMemoryMediaCoreSyncEngine {
  constructor(private readonly bridge: NativeHostBridge) {
    super();
  }

  override async syncProduction(state: ProductionState, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot> {
    const commands = buildNativeMediaCoreCommands(state);

    if (!this.bridge.syncMediaCore) {
      return this.snapshot(commands, elapsedMs, ["Native host has no media-core sync bridge; using renderer-side simulation."]);
    }

    return this.bridge.syncMediaCore(commands, elapsedMs);
  }
}
