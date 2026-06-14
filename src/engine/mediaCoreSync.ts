import type { ProductionState } from "../domain/production";
import { buildNativeMediaCoreCommands } from "./nativeMediaCoreCommands";
import type { NativeHostBridge } from "./nativeHostBridge";
import type {
  NativeMediaCoreColorGrade,
  NativeMediaCoreCommand,
  NativeMediaCoreCompositorState,
  NativeMediaCoreEncoderLifecycle,
  NativeMediaCoreEncoderSession,
  NativeMediaCoreEncoderTarget,
  NativeMediaCoreFrame,
  NativeMediaCoreFrameSourceSnapshot,
  NativeMediaCoreOutputHealth,
  NativeMediaCoreOutputProfile,
  NativeMediaCoreProgramFrame,
  NativeMediaCoreProgramFrameTransport,
  NativeMediaCoreRenderPlan,
  NativeMediaCoreRecordingSession,
  NativeMediaCoreRecordingTargets,
  NativeMediaCoreStateSnapshot,
  NativeMediaCoreZoomSource
} from "./nativeMediaCoreProtocol";
import { buildNativeMediaCoreRenderPlan } from "./nativeMediaCoreRenderPlan";

export interface MediaCoreSyncEngine {
  syncProduction(state: ProductionState, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot>;
}

export class InMemoryMediaCoreSyncEngine implements MediaCoreSyncEngine {
  private readonly frameNumbers = new Map<string, number>();
  private programFrameNumber = 0;
  private compositorRenderPlanId?: string;
  private compositorDroppedFrameCount = 0;
  private compositorDegradedFrameCount = 0;
  private lastReconfigureReason?: string;
  private programFrame?: NativeMediaCoreProgramFrame;
  private sourceDroppedFrameCount = 0;
  private sourceLowResolutionFrameCount = 0;
  private sourceSnapshot: NativeMediaCoreFrameSourceSnapshot = {
    adapterId: "renderer-test-pattern",
    kind: "test-pattern",
    status: "idle",
    subscribedSourceCount: 0,
    liveFrameCount: 0,
    staleFrameCount: 0,
    droppedFrameCount: 0,
    lowResolutionFrameCount: 0,
    warnings: []
  };
  private programTransport: NativeMediaCoreProgramFrameTransport = {
    transportId: "in-process-preview",
    status: "idle",
    latencyMs: 0,
    warning: "No program frame has been published."
  };
  private encoderLifecycle: NativeMediaCoreEncoderLifecycle = {
    status: "idle",
    lastTransition: "Encoder session idle."
  };
  private sources: NativeMediaCoreZoomSource[] = [];
  private activeSpeakerId?: string;
  private screenShareParticipantId?: string;
  private outputProfile: NativeMediaCoreOutputProfile = {
    profileId: "1080p60",
    resolution: "1920x1080",
    width: 1920,
    height: 1080,
    fps: 60,
    targetBitrateMbps: 8
  };
  private colorGrade: NativeMediaCoreColorGrade = {
    lut: "none",
    exposure: 0,
    contrast: 0,
    saturation: 0,
    temperature: 0
  };
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
    const outputProfile = commands.find((command) => command.type === "set-output-profile");
    const sourceRoster = commands.find((command) => command.type === "set-zoom-source-roster");
    const activeSpeaker = commands.find((command) => command.type === "set-active-speaker");
    const screenShareSource = commands.find((command) => command.type === "set-screen-share-source");
    const colorGrade = commands.find((command) => command.type === "set-color-grade");
    const prepareEncoder = commands.find((command) => command.type === "prepare-encoder-session");
    const startEncoder = commands.find((command) => command.type === "start-encoder-session");
    const stopEncoder = commands.find((command) => command.type === "stop-encoder-session");
    const transforms = commands.filter((command) => command.type === "set-participant-transform");
    const overlays = commands.filter((command) => command.type === "set-overlay-asset");
    if (sourceRoster) {
      this.sources = sourceRoster.sources.map((source) => ({ ...source, hasVideo: source.hasVideo && source.health !== "video-off" }));
      this.activeSpeakerId = sourceRoster.sources.find((source) => source.isActiveSpeaker)?.participantId ?? this.activeSpeakerId;
      this.screenShareParticipantId = sourceRoster.sources.find((source) => source.isScreenSharing)?.participantId ?? this.screenShareParticipantId;
    }
    if (activeSpeaker) {
      this.activeSpeakerId = activeSpeaker.participantId;
    }
    if (screenShareSource) {
      this.screenShareParticipantId = screenShareSource.participantId;
    }
    if (colorGrade) {
      this.colorGrade = {
        lut: colorGrade.lut,
        exposure: clampRange(colorGrade.exposure, -100, 100),
        contrast: clampRange(colorGrade.contrast, -100, 100),
        saturation: clampRange(colorGrade.saturation, -100, 100),
        temperature: clampRange(colorGrade.temperature, -100, 100)
      };
    }
    if (outputProfile) {
      this.outputProfile = normalizeOutputProfile(outputProfile);
    }
    if (!output && (this.encoderLifecycle.status === "encoding" || this.encoderLifecycle.status === "prepared")) {
      this.encoderLifecycle = {
        ...this.encoderLifecycle,
        status: "stopped",
        stoppedAtMs: elapsedMs,
        lastTransition: "Encoder session stopped because no program outputs are active."
      };
    }
    if (output && output.destinations.length > 0 && this.encoderLifecycle.status !== "encoding") {
      this.encoderLifecycle = {
        status: "encoding",
        preparedAtMs: this.encoderLifecycle.preparedAtMs ?? elapsedMs,
        startedAtMs: elapsedMs,
        lastTransition: "Program output encoder session started."
      };
    }
    if (prepareEncoder) {
      this.encoderLifecycle = {
        status: "prepared",
        preparedAtMs: prepareEncoder.preparedAtMs ?? elapsedMs,
        lastTransition: prepareEncoder.reason ?? "Encoder session prepared."
      };
    }
    if (startEncoder) {
      this.encoderLifecycle = {
        ...this.encoderLifecycle,
        status: "encoding",
        preparedAtMs: this.encoderLifecycle.preparedAtMs ?? startEncoder.startedAtMs ?? elapsedMs,
        startedAtMs: startEncoder.startedAtMs ?? elapsedMs,
        lastTransition: "Encoder session started."
      };
    }
    if (stopEncoder) {
      this.encoderLifecycle = {
        ...this.encoderLifecycle,
        status: "stopped",
        stoppedAtMs: stopEncoder.stoppedAtMs ?? elapsedMs,
        lastTransition: stopEncoder.reason ?? "Encoder session stopped."
      };
    }
    const renderPlan = buildNativeMediaCoreRenderPlan({
      sceneGraph,
      sources: this.sources,
      activeSpeakerId: this.activeSpeakerId,
      screenShareParticipantId: this.screenShareParticipantId,
      overlays,
      outputProfile: this.outputProfile,
      colorGrade: this.colorGrade
    });
    const frames = renderPlan.layers
      .filter((layer) => layer.kind === "participant-video" || layer.kind === "screen-share")
      .map((layer, index) => this.frameFromRenderLayer(layer, index, elapsedMs));
    this.sourceSnapshot = this.buildSourceSnapshot(frames, elapsedMs);
    this.programFrame = this.composeProgramFrame(renderPlan, elapsedMs);
    this.programTransport = this.buildProgramTransport(this.programFrame, elapsedMs);
    const recording = this.syncRecordingCommands(commands, frames ?? [], elapsedMs);
    const encoderSession = this.encoderSession(output?.destinations ?? [], this.programFrame, recording);
    const outputHealth = this.outputHealth(output?.destinations ?? [], recording, this.programFrame, encoderSession);
    const compositor = this.compositorSnapshot();
    const allWarnings = [
      ...new Set([
        ...warnings,
        ...this.sourceSnapshot.warnings,
        ...renderPlan.warnings,
        ...encoderSession.warnings,
        recording?.warning,
        recording?.error
      ].filter(Boolean) as string[])
    ];

    return {
      sceneId: sceneGraph?.sceneId,
      routeCount: sceneGraph?.routes.length ?? 0,
      frameCount: frames.length,
      frames,
      sourceSnapshot: this.sourceSnapshot,
      programFrame: this.programFrame,
      programFrameCount: compositor.programFrameCount,
      programTransport: this.programTransport,
      compositor,
      participantTransformCount: transforms.length,
      overlayCount: overlays.length,
      outputs: output?.destinations ?? [],
      isoParticipantIds: output?.isoParticipantIds ?? [],
      outputProfile: this.outputProfile,
      outputHealth,
      sourceCount: this.sources.length,
      resolvedRouteCount: renderPlan.resolvedRouteCount,
      renderPlan,
      encoderSession,
      recording,
      diagnostics: {
        generatedAtMs: elapsedMs,
        sceneId: sceneGraph?.sceneId,
        routeCount: sceneGraph?.routes.length ?? 0,
        frameCount: frames.length,
        programFrameCount: compositor.programFrameCount,
        outputs: output?.destinations ?? [],
        outputProfile: this.outputProfile,
        outputHealth,
        sourceSnapshot: this.sourceSnapshot,
        renderPlan,
        compositor,
        programFrame: this.programFrame,
        programTransport: this.programTransport,
        encoderSession,
        recording,
        warnings: allWarnings,
        lastCommandTypes: commands.map((command) => command.type)
      },
      lastCommandTypes: commands.map((command) => command.type),
      warnings: allWarnings
    };
  }

  private frameFromRenderLayer(layer: NativeMediaCoreRenderPlan["layers"][number], index: number, elapsedMs: number) {
    const isScreenShare = layer.kind === "screen-share";
    const sourceId = layer.sourceId ?? layer.layerId;
    const nextFrameNumber = (this.frameNumbers.get(sourceId) ?? 0) + 1;
    this.frameNumbers.set(sourceId, nextFrameNumber);

    return {
      sourceId,
      participantId: layer.participantId,
      kind: isScreenShare ? "screen-share" : "participant-video",
      frameNumber: nextFrameNumber,
      timestampMs: elapsedMs,
      width: isScreenShare ? 1920 : index >= 4 ? 960 : 1280,
      height: isScreenShare ? 1080 : index >= 4 ? 540 : 720,
      fps: isScreenShare ? 30 : 60,
      health: nextFrameNumber % 90 === 0 ? "dropped" : index >= 4 && !isScreenShare ? "low-resolution" : "live"
    } satisfies NativeMediaCoreFrame;
  }

  private buildSourceSnapshot(frames: NativeMediaCoreFrame[], elapsedMs: number): NativeMediaCoreFrameSourceSnapshot {
    const droppedThisTick = frames.filter((frame) => frame.health === "dropped").length;
    const lowResolutionThisTick = frames.filter((frame) => frame.health === "low-resolution").length;
    this.sourceDroppedFrameCount += droppedThisTick;
    this.sourceLowResolutionFrameCount += lowResolutionThisTick;
    const warnings = [
      lowResolutionThisTick > 0 ? `${lowResolutionThisTick} source frame${lowResolutionThisTick === 1 ? "" : "s"} below target resolution.` : undefined,
      droppedThisTick > 0 ? `${droppedThisTick} source frame${droppedThisTick === 1 ? "" : "s"} dropped by media source.` : undefined
    ].filter(Boolean) as string[];

    return {
      adapterId: "renderer-test-pattern",
      kind: "test-pattern",
      status: warnings.length ? "degraded" : frames.length > 0 ? "subscribed" : "idle",
      subscribedSourceCount: frames.length,
      liveFrameCount: frames.filter((frame) => frame.health === "live").length,
      staleFrameCount: frames.filter((frame) => elapsedMs - frame.timestampMs > 250).length,
      droppedFrameCount: this.sourceDroppedFrameCount,
      lowResolutionFrameCount: this.sourceLowResolutionFrameCount,
      lastFrameTimestampMs: frames.length ? elapsedMs : this.sourceSnapshot.lastFrameTimestampMs,
      warnings
    };
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
      this.writeRecordingFrames(frames, elapsedMs, this.programFrame);
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

  private writeRecordingFrames(frames: NativeMediaCoreFrame[], elapsedMs: number, programFrame?: NativeMediaCoreProgramFrame) {
    if (!this.recording || !this.recording.active) {
      return;
    }

    this.recording.elapsedMs = Math.max(0, elapsedMs - this.recording.startedAtMs);
    const writableFrames = frames.filter((frame) => frame.health !== "dropped");
    this.recording.streams.forEach((stream) => {
      const droppedFrames =
        stream.kind === "program"
          ? programFrame?.health === "dropped"
            ? 1
            : 0
          : frames.filter((frame) => frame.kind === "participant-video" && frame.health === "dropped" && frame.participantId === stream.participantId).length;
      if (stream.kind === "program") {
        const programFrames = programFrame && programFrame.health !== "dropped" ? 1 : 0;
        stream.framesWritten += programFrames;
        stream.droppedFrames += droppedFrames;
        stream.bytesWritten += programFrames * 260_000;
      } else {
        const isoFrames = writableFrames.filter((frame) => frame.kind === "participant-video" && frame.participantId === stream.participantId).length;
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

  private outputHealth(
    destinations: Array<"rtmp" | "ndi" | "srt" | "webrtc" | "recording">,
    recording: NativeMediaCoreRecordingSession | undefined,
    programFrame: NativeMediaCoreProgramFrame | undefined,
    encoderSession: NativeMediaCoreEncoderSession
  ) {
    const health = destinations.map(
      (destination): NativeMediaCoreOutputHealth => ({
        destination,
        status: "live",
        message:
          destination === "recording"
            ? `Recording output armed at ${this.outputProfile.resolution}${this.outputProfile.fps}.`
            : `${destination.toUpperCase()} output active at ${this.outputProfile.resolution}${this.outputProfile.fps}.`,
        droppedFrames: 0
      })
    );

    if (recording) {
      const index = health.findIndex((item) => item.destination === "recording");
      const recordingHealth: NativeMediaCoreOutputHealth = {
        destination: "recording",
        status:
          recording.status === "failed" || encoderSession.status === "failed"
            ? "failed"
            : recording.status === "warning" ||
                encoderSession.status === "warning" ||
                programFrame?.health === "degraded" ||
                (destinations.length > 0 && encoderSession.lifecycle.status !== "encoding")
              ? "warning"
              : recording.active
                ? "live"
                : "idle",
        message:
          recording.error ??
          recording.warning ??
          encoderSession.warnings[0] ??
          (destinations.length > 0 && encoderSession.lifecycle.status !== "encoding" ? encoderSession.lifecycle.lastTransition : undefined) ??
          (recording.active ? "Recording writer active." : "Recording stopped."),
        droppedFrames: recording.totalDroppedFrames
      };

      if (index >= 0) {
        health[index] = recordingHealth;
      } else {
        health.push(recordingHealth);
      }
    }

    if (programFrame?.health === "degraded") {
      return health.map((item) => ({
        ...item,
        status: item.status === "failed" ? item.status : ("warning" as const),
        message: programFrame.warning ?? "Program frame is degraded."
      }));
    }

    if (destinations.length > 0 && encoderSession.lifecycle.status !== "encoding") {
      return health.map((item) => ({
        ...item,
        status: item.status === "failed" ? item.status : ("warning" as const),
        message: encoderSession.lifecycle.lastTransition
      }));
    }

    return health;
  }

  private composeProgramFrame(renderPlan: NativeMediaCoreRenderPlan, elapsedMs: number) {
    if (!renderPlan.sceneId) {
      this.programFrame = undefined;
      return undefined;
    }

    if (this.compositorRenderPlanId !== renderPlan.renderPlanId) {
      this.lastReconfigureReason = this.compositorRenderPlanId
        ? `Render plan changed from ${this.compositorRenderPlanId} to ${renderPlan.renderPlanId}.`
        : `Initial render plan ${renderPlan.renderPlanId}.`;
      this.compositorRenderPlanId = renderPlan.renderPlanId;
    }

    this.programFrameNumber += 1;
    const hasMissingRoutes = renderPlan.routes.some((route) => route.status === "missing");
    const health = this.programFrameNumber % 120 === 0 ? "dropped" : hasMissingRoutes || renderPlan.warnings.length > 0 ? "degraded" : "live";
    if (health === "dropped") {
      this.compositorDroppedFrameCount += 1;
    }
    if (health === "degraded") {
      this.compositorDegradedFrameCount += 1;
    }

    return {
      frameNumber: this.programFrameNumber,
      timestampMs: elapsedMs,
      renderPlanId: renderPlan.renderPlanId,
      sceneId: renderPlan.sceneId,
      width: renderPlan.outputProfile.width,
      height: renderPlan.outputProfile.height,
      fps: renderPlan.outputProfile.fps,
      layerCount: renderPlan.layers.length,
      colorGrade: renderPlan.colorGrade,
      health,
      warning: health === "degraded" ? renderPlan.warnings[0] ?? "Program frame degraded by incomplete render plan." : undefined
    } satisfies NativeMediaCoreProgramFrame;
  }

  private compositorSnapshot(): NativeMediaCoreCompositorState {
    return {
      status: this.programFrame ? (this.programFrame.health === "dropped" ? "degraded" : this.programFrame.health) : "idle",
      renderPlanId: this.compositorRenderPlanId,
      programFrameCount: this.programFrameNumber,
      droppedFrameCount: this.compositorDroppedFrameCount,
      degradedFrameCount: this.compositorDegradedFrameCount,
      lastReconfigureReason: this.lastReconfigureReason,
      lastFrame: this.programFrame ? { ...this.programFrame, colorGrade: { ...this.programFrame.colorGrade } } : undefined
    };
  }

  private buildProgramTransport(programFrame: NativeMediaCoreProgramFrame | undefined, elapsedMs: number): NativeMediaCoreProgramFrameTransport {
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
      latencyMs: Math.max(0, elapsedMs - programFrame.timestampMs),
      warning:
        programFrame.health === "dropped"
          ? "Program transport skipped a dropped frame."
          : programFrame.health === "degraded"
            ? programFrame.warning ?? "Program transport is publishing degraded frames."
            : undefined
    };
  }

  private encoderSession(
    outputs: Array<"rtmp" | "ndi" | "srt" | "webrtc" | "recording">,
    programFrame?: NativeMediaCoreProgramFrame,
    recording?: NativeMediaCoreRecordingSession
  ): NativeMediaCoreEncoderSession {
    const targets: NativeMediaCoreEncoderTarget[] = outputs.map((destination) => ({
      targetId: `${destination}:program`,
      destination,
      streamKind: "program" as const,
      status: !programFrame ? ("idle" as const) : programFrame.health === "live" ? ("attached" as const) : ("warning" as const),
      attachedFrameCount: programFrame && programFrame.health !== "dropped" ? 1 : 0,
      warning: !programFrame
        ? `${destination} has no program frame to encode.`
        : programFrame.health === "dropped"
          ? `${destination} skipped a dropped program frame.`
          : programFrame.health === "degraded"
            ? `${destination} is encoding a degraded program frame.`
            : undefined
    }));

    if (recording?.active) {
      recording.streams
        .filter((stream) => stream.kind === "iso")
        .forEach((stream) => {
          targets.push({
            targetId: `recording:iso:${stream.participantId}`,
            destination: "recording",
            streamKind: "iso" as const,
            participantId: stream.participantId,
            status: stream.status === "failed" ? "failed" : stream.status === "warning" ? "warning" : "attached",
            attachedFrameCount: stream.framesWritten,
            warning: stream.status === "warning" ? recording.warning : undefined
          });
        });
    }

    const warnings = targets.map((target) => target.warning).filter(Boolean) as string[];
    const hasFailure = targets.some((target) => target.status === "failed");
    const hasWarning = targets.some((target) => target.status === "warning");

    return {
      status:
        this.encoderLifecycle.status === "failed"
          ? "failed"
          : hasFailure
            ? "failed"
            : hasWarning
              ? "warning"
              : this.encoderLifecycle.status === "encoding" && targets.length > 0
                ? "encoding"
                : "idle",
      renderPlanId: programFrame?.renderPlanId,
      programFrameCount: programFrame && programFrame.health !== "dropped" ? 1 : 0,
      targets,
      lifecycle: this.encoderLifecycle,
      warnings: [...new Set(warnings)]
    };
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

function normalizeOutputProfile(profile: NativeMediaCoreOutputProfile): NativeMediaCoreOutputProfile {
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

function clampRange(value: number, min: number, max: number) {
  return Math.min(max, Math.max(min, value));
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
