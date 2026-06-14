import type { ProductionState } from "../domain/production";
import { buildNativeMediaCoreCommands } from "./nativeMediaCoreCommands";
import type { NativeHostBridge } from "./nativeHostBridge";
import type {
  NativeMediaCoreCommand,
  NativeMediaCoreFrame,
  NativeMediaCoreOutputHealth,
  NativeMediaCoreRecordingSession,
  NativeMediaCoreRecordingTargets,
  NativeMediaCoreStateSnapshot
} from "./nativeMediaCoreProtocol";

export interface MediaCoreSyncEngine {
  syncProduction(state: ProductionState, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot>;
}

export class InMemoryMediaCoreSyncEngine implements MediaCoreSyncEngine {
  private readonly frameNumbers = new Map<string, number>();
  private recording?: NativeMediaCoreRecordingSession;
  private recordingTargets: NativeMediaCoreRecordingTargets = {
    targetFolder: "Recordings/CoreVideo Pro/native-core",
    filenamePrefix: "program",
    format: "mp4",
    quality: "high",
    isoParticipantIds: []
  };

  async syncProduction(state: ProductionState, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot> {
    return this.snapshot(buildNativeMediaCoreCommands(state), elapsedMs);
  }

  protected snapshot(commands: NativeMediaCoreCommand[], elapsedMs: number, warnings: string[] = []): NativeMediaCoreStateSnapshot {
    const sceneGraph = commands.find((command) => command.type === "load-scene-graph");
    const output = commands.find((command) => command.type === "start-program-output");
    const transforms = commands.filter((command) => command.type === "set-participant-transform");
    const overlays = commands.filter((command) => command.type === "set-overlay-asset");
    const frames = sceneGraph?.routes.map((route, index) => this.frameFromRoute(route, index, elapsedMs)).filter(Boolean) as NativeMediaCoreFrame[] | undefined;
    const recording = this.syncRecordingCommands(commands, frames ?? [], elapsedMs);
    const outputHealth = this.outputHealth(output?.destinations ?? [], recording);
    const allWarnings = [...new Set([...warnings, recording?.warning, recording?.error].filter(Boolean) as string[])];

    return {
      sceneId: sceneGraph?.sceneId,
      routeCount: sceneGraph?.routes.length ?? 0,
      frameCount: frames?.length ?? 0,
      frames: frames ?? [],
      participantTransformCount: transforms.length,
      overlayCount: overlays.length,
      outputs: output?.destinations ?? [],
      isoParticipantIds: output?.isoParticipantIds ?? [],
      outputHealth,
      recording,
      diagnostics: {
        generatedAtMs: elapsedMs,
        sceneId: sceneGraph?.sceneId,
        routeCount: sceneGraph?.routes.length ?? 0,
        frameCount: frames?.length ?? 0,
        outputs: output?.destinations ?? [],
        outputHealth,
        recording,
        warnings: allWarnings,
        lastCommandTypes: commands.map((command) => command.type)
      },
      lastCommandTypes: commands.map((command) => command.type),
      warnings: allWarnings
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

  private syncRecordingCommands(commands: NativeMediaCoreCommand[], frames: NativeMediaCoreFrame[], elapsedMs: number) {
    commands.forEach((command) => {
      if (command.type === "set-recording-targets") {
        this.recordingTargets = normalizeTargets(command);
      }

      if (command.type === "start-recording-session") {
        const targets = normalizeTargets({ ...this.recordingTargets, ...command });
        this.startRecording(targets, command.startedAtMs ?? elapsedMs, command.sessionId);
      }

      if (command.type === "stop-recording-session") {
        this.stopRecording(elapsedMs);
      }

      if (command.type === "fail-recording-session") {
        this.failRecording(command.message, elapsedMs);
      }
    });

    if (this.recording?.active) {
      this.writeRecordingFrames(frames, elapsedMs);
    }

    return this.snapshotRecording(elapsedMs);
  }

  private startRecording(targets: NativeMediaCoreRecordingTargets, elapsedMs: number, sessionId = `rec-${elapsedMs}`) {
    if (!this.recording || !this.recording.active || !sameTargets(this.recording, targets)) {
      const programPath = `${targets.targetFolder}/${targets.filenamePrefix}-program-${elapsedMs}.${targets.format}`;
      this.recording = {
        sessionId,
        active: true,
        status: targets.isoParticipantIds.length > 4 ? "warning" : "recording",
        writerStatus: targets.isoParticipantIds.length > 4 ? "warning" : "writing",
        startedAtMs: elapsedMs,
        elapsedMs: 0,
        targetFolder: targets.targetFolder,
        filenamePrefix: targets.filenamePrefix,
        format: targets.format,
        quality: targets.quality,
        encoder: encoderForQuality(targets.quality),
        estimatedDiskRateMBps: estimateDiskRate(targets.isoParticipantIds.length + 1, targets.quality),
        programPath,
        streams: [
          createRecordingStream("program", programPath),
          ...targets.isoParticipantIds.map((participantId) =>
            createRecordingStream("iso", `${targets.targetFolder}/${targets.filenamePrefix}-iso-${participantId}-${elapsedMs}.${targets.format}`, participantId)
          )
        ],
        totalFramesWritten: 0,
        totalDroppedFrames: 0,
        totalBytesWritten: 0,
        warning: targets.isoParticipantIds.length > 4 ? "High ISO count may exceed disk bandwidth." : undefined
      };
    }

  }

  private writeRecordingFrames(frames: NativeMediaCoreFrame[], elapsedMs: number) {
    if (!this.recording || !this.recording.active) {
      return;
    }

    this.recording.elapsedMs = Math.max(0, elapsedMs - this.recording.startedAtMs);
    const writableFrames = frames.filter((frame) => frame.health !== "dropped");
    this.recording.streams.forEach((stream) => {
      const droppedFrames =
        stream.kind === "program" ? frames.length - writableFrames.length : frames.filter((frame) => frame.health === "dropped" && frame.participantId === stream.participantId).length;
      if (stream.kind === "program") {
        stream.framesWritten += writableFrames.length;
        stream.droppedFrames += droppedFrames;
        stream.bytesWritten += writableFrames.length * 260_000;
      } else {
        const isoFrames = writableFrames.filter((frame) => frame.participantId === stream.participantId).length;
        stream.framesWritten += isoFrames;
        stream.droppedFrames += droppedFrames;
        stream.bytesWritten += isoFrames * 140_000;
      }
    });
    this.recording.totalFramesWritten = this.recording.streams.reduce((total, stream) => total + stream.framesWritten, 0);
    this.recording.totalDroppedFrames = this.recording.streams.reduce((total, stream) => total + stream.droppedFrames, 0);
    this.recording.totalBytesWritten = this.recording.streams.reduce((total, stream) => total + stream.bytesWritten, 0);
    if (this.recording.totalDroppedFrames > 0) {
      this.recording.status = "warning";
      this.recording.writerStatus = "warning";
      this.recording.warning = "Recording writer is skipping dropped frames.";
      this.recording.streams.forEach((stream) => {
        stream.status = "warning";
      });
    }
  }

  private stopRecording(elapsedMs: number) {
    if (!this.recording) {
      return undefined;
    }

    this.recording.active = false;
    this.recording.status = "stopped";
    this.recording.writerStatus = "stopped";
    this.recording.stoppedAtMs = elapsedMs;
    this.recording.streams.forEach((stream) => {
      stream.status = "stopped";
    });
    return this.snapshotRecording(elapsedMs);
  }

  private failRecording(message: string, elapsedMs: number) {
    if (!this.recording) {
      this.startRecording(this.recordingTargets, elapsedMs);
    }

    if (this.recording) {
      this.recording.active = false;
      this.recording.status = "failed";
      this.recording.writerStatus = "failed";
      this.recording.error = message;
      this.recording.warning = undefined;
      this.recording.stoppedAtMs = elapsedMs;
      this.recording.streams.forEach((stream) => {
        stream.status = "failed";
      });
    }
  }

  private snapshotRecording(elapsedMs: number) {
    if (!this.recording) {
      return undefined;
    }

    return {
      ...this.recording,
      elapsedMs: Math.max(this.recording.elapsedMs, elapsedMs - this.recording.startedAtMs),
      streams: this.recording.streams.map((stream) => ({ ...stream })),
      encoder: { ...this.recording.encoder }
    };
  }

  private outputHealth(destinations: Array<"rtmp" | "ndi" | "srt" | "webrtc" | "recording">, recording?: NativeMediaCoreRecordingSession) {
    const health = destinations.map(
      (destination): NativeMediaCoreOutputHealth => ({
        destination,
        status: "live",
        message: destination === "recording" ? "Recording output armed." : `${destination.toUpperCase()} output active.`,
        droppedFrames: 0
      })
    );

    if (recording) {
      const index = health.findIndex((item) => item.destination === "recording");
      const recordingHealth: NativeMediaCoreOutputHealth = {
        destination: "recording",
        status: recording.status === "failed" ? "failed" : recording.status === "warning" ? "warning" : recording.active ? "live" : "idle",
        message: recording.error ?? recording.warning ?? (recording.active ? "Recording writer active." : "Recording stopped."),
        droppedFrames: recording.totalDroppedFrames
      };

      if (index >= 0) {
        health[index] = recordingHealth;
      } else {
        health.push(recordingHealth);
      }
    }

    return health;
  }
}

function sameTargets(recording: NativeMediaCoreRecordingSession, targets: NativeMediaCoreRecordingTargets) {
  const currentIds = recording.streams
    .filter((stream) => stream.kind === "iso")
    .map((stream) => stream.participantId as string)
    .sort();
  return (
    recording.targetFolder === targets.targetFolder &&
    recording.filenamePrefix === targets.filenamePrefix &&
    recording.format === targets.format &&
    recording.quality === targets.quality &&
    currentIds.length === targets.isoParticipantIds.length &&
    currentIds.every((id, index) => id === targets.isoParticipantIds[index])
  );
}

function normalizeTargets(targets: NativeMediaCoreRecordingTargets): NativeMediaCoreRecordingTargets {
  return {
    targetFolder: targets.targetFolder.trim() || "Recordings/CoreVideo Pro/native-core",
    filenamePrefix: sanitizeFilename(targets.filenamePrefix || "program"),
    format: targets.format,
    quality: targets.quality,
    isoParticipantIds: [...new Set(targets.isoParticipantIds)].sort()
  };
}

function sanitizeFilename(value: string) {
  return value.trim().replace(/[^a-zA-Z0-9._-]+/g, "_") || "program";
}

function createRecordingStream(kind: "program" | "iso", path: string, participantId?: string) {
  return {
    kind,
    participantId,
    path,
    status: "writing" as const,
    framesWritten: 0,
    droppedFrames: 0,
    bytesWritten: 0
  };
}

function encoderForQuality(quality: "standard" | "high" | "archive") {
  if (quality === "archive") {
    return { codec: "hevc" as const, hardwareAccelerated: true, targetBitrateMbps: 32 };
  }

  if (quality === "high") {
    return { codec: "h264" as const, hardwareAccelerated: true, targetBitrateMbps: 18 };
  }

  return { codec: "h264" as const, hardwareAccelerated: true, targetBitrateMbps: 10 };
}

function estimateDiskRate(streamCount: number, quality: "standard" | "high" | "archive") {
  const qualityMultiplier = quality === "archive" ? 2.2 : quality === "high" ? 1.35 : 1;
  return Number((Math.max(1, streamCount) * 1.85 * qualityMultiplier).toFixed(2));
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
