import type { ProductionState } from "../domain/production";
import { buildNativeMediaCoreCommands } from "./nativeMediaCoreCommands";
import type { NativeHostBridge } from "./nativeHostBridge";
import type {
  NativeMediaCoreCommand,
  NativeMediaCoreFrame,
  NativeMediaCoreRecordingSession,
  NativeMediaCoreStateSnapshot
} from "./nativeMediaCoreProtocol";

export interface MediaCoreSyncEngine {
  syncProduction(state: ProductionState, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot>;
}

export class InMemoryMediaCoreSyncEngine implements MediaCoreSyncEngine {
  private readonly frameNumbers = new Map<string, number>();
  private recording?: NativeMediaCoreRecordingSession;

  async syncProduction(state: ProductionState, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot> {
    return this.snapshot(buildNativeMediaCoreCommands(state), elapsedMs);
  }

  protected snapshot(commands: NativeMediaCoreCommand[], elapsedMs: number, warnings: string[] = []): NativeMediaCoreStateSnapshot {
    const sceneGraph = commands.find((command) => command.type === "load-scene-graph");
    const output = commands.find((command) => command.type === "start-program-output");
    const transforms = commands.filter((command) => command.type === "set-participant-transform");
    const overlays = commands.filter((command) => command.type === "set-overlay-asset");
    const frames = sceneGraph?.routes.map((route, index) => this.frameFromRoute(route, index, elapsedMs)).filter(Boolean) as NativeMediaCoreFrame[] | undefined;
    const recording = output?.destinations.includes("recording")
      ? this.syncRecording(output.isoParticipantIds, frames ?? [], elapsedMs)
      : this.stopRecording();

    return {
      sceneId: sceneGraph?.sceneId,
      routeCount: sceneGraph?.routes.length ?? 0,
      frameCount: frames?.length ?? 0,
      frames: frames ?? [],
      participantTransformCount: transforms.length,
      overlayCount: overlays.length,
      outputs: output?.destinations ?? [],
      isoParticipantIds: output?.isoParticipantIds ?? [],
      recording,
      lastCommandTypes: commands.map((command) => command.type),
      warnings: [...warnings, recording?.warning].filter(Boolean) as string[]
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

  private syncRecording(isoParticipantIds: string[], frames: NativeMediaCoreFrame[], elapsedMs: number) {
    const isoIds = [...new Set(isoParticipantIds)].sort();
    if (!this.recording || !sameIsoStreams(this.recording, isoIds)) {
      const programPath = `Recordings/CoreVideo Pro/native-core/program-${elapsedMs}.mp4`;
      this.recording = {
        active: true,
        status: isoIds.length > 4 ? "warning" : "recording",
        startedAtMs: elapsedMs,
        elapsedMs: 0,
        estimatedDiskRateMBps: estimateDiskRate(isoIds.length + 1),
        programPath,
        streams: [
          { kind: "program", path: programPath, framesWritten: 0 },
          ...isoIds.map((participantId) => ({
            kind: "iso" as const,
            participantId,
            path: `Recordings/CoreVideo Pro/native-core/iso-${participantId}-${elapsedMs}.mp4`,
            framesWritten: 0
          }))
        ],
        totalFramesWritten: 0,
        warning: isoIds.length > 4 ? "High ISO count may exceed disk bandwidth." : undefined
      };
    }

    this.recording.elapsedMs = Math.max(0, elapsedMs - this.recording.startedAtMs);
    const writableFrames = frames.filter((frame) => frame.health !== "dropped");
    this.recording.streams.forEach((stream) => {
      if (stream.kind === "program") {
        stream.framesWritten += writableFrames.length;
      } else {
        stream.framesWritten += writableFrames.filter((frame) => frame.participantId === stream.participantId).length;
      }
    });
    this.recording.totalFramesWritten = this.recording.streams.reduce((total, stream) => total + stream.framesWritten, 0);

    return {
      ...this.recording,
      streams: this.recording.streams.map((stream) => ({ ...stream }))
    };
  }

  private stopRecording() {
    this.recording = undefined;
    return undefined;
  }
}

function sameIsoStreams(recording: NativeMediaCoreRecordingSession, isoParticipantIds: string[]) {
  const currentIds = recording.streams
    .filter((stream) => stream.kind === "iso")
    .map((stream) => stream.participantId as string)
    .sort();
  return currentIds.length === isoParticipantIds.length && currentIds.every((id, index) => id === isoParticipantIds[index]);
}

function estimateDiskRate(streamCount: number) {
  return Number((Math.max(1, streamCount) * 1.85).toFixed(2));
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
