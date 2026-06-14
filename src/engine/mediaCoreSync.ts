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
  NativeMediaCoreEvent,
  NativeMediaCoreFrame,
  NativeMediaCoreFrameSourceSnapshot,
  NativeMediaCoreMediaSourceKind,
  NativeMediaCoreOperatorAction,
  NativeMediaCoreOutputHealth,
  NativeMediaCoreOutputProfile,
  NativeMediaCoreOutputSender,
  NativeMediaCoreOutputSenderSession,
  NativeMediaCoreProgramFrame,
  NativeMediaCoreProgramFrameTransport,
  NativeMediaCoreRenderPlan,
  NativeMediaCoreRecordingSession,
  NativeMediaCoreRecordingStream,
  NativeMediaCoreRecordingTargets,
  NativeMediaCoreStateSnapshot,
  NativeMediaCoreSourceHealthIssue,
  NativeMediaCoreZoomSource
} from "./nativeMediaCoreProtocol";
import { buildNativeMediaCoreRenderPlan } from "./nativeMediaCoreRenderPlan";
import { buildNativeMediaCoreOperatorActions } from "./mediaCoreOperatorActions";

const MAX_EVENT_LOG_LENGTH = 80;

type IsoRecordingReadiness = NonNullable<NativeMediaCoreRecordingStream["readiness"]>;

type IsoRecordingReadinessIssue = {
  participantId: string;
  readiness: Exclude<IsoRecordingReadiness, "ready">;
  warning: string;
};

export interface MediaCoreSyncEngine {
  syncProduction(state: ProductionState, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot>;
  executeOperatorAction(state: ProductionState, action: NativeMediaCoreOperatorAction, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot>;
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
  private mediaSourceKind: NativeMediaCoreMediaSourceKind = "test-pattern";
  private mediaSourceAdapterId = "renderer-test-pattern";
  private sourceSnapshot: NativeMediaCoreFrameSourceSnapshot = {
    adapterId: "renderer-test-pattern",
    kind: "test-pattern",
    status: "idle",
    subscribedSourceCount: 0,
    liveFrameCount: 0,
    staleFrameCount: 0,
    droppedFrameCount: 0,
    lowResolutionFrameCount: 0,
    issues: [],
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
  private readonly outputSenders = new Map<"rtmp" | "ndi" | "srt" | "webrtc", NativeMediaCoreOutputSender>();
  private readonly eventLog: NativeMediaCoreEvent[] = [];
  private readonly seenEventKeys = new Set<string>();
  private readonly activeSourceIssues = new Map<string, NativeMediaCoreSourceHealthIssue>();
  private readonly outputSenderEventStates = new Map<string, { status: NativeMediaCoreOutputSender["status"]; warning?: string }>();
  private readonly recordingStreamWarnings = new Map<string, string>();
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
    return this.syncCommands(buildNativeMediaCoreCommands(state), elapsedMs);
  }

  async executeOperatorAction(state: ProductionState, action: NativeMediaCoreOperatorAction, elapsedMs: number): Promise<NativeMediaCoreStateSnapshot> {
    const command = mediaCoreCommandFromOperatorAction(action, elapsedMs);
    const commands = command ? [...buildNativeMediaCoreCommands(state), command] : buildNativeMediaCoreCommands(state);

    return this.syncCommands(commands, elapsedMs);
  }

  protected async syncCommands(commands: NativeMediaCoreCommand[], elapsedMs: number, warnings: string[] = []): Promise<NativeMediaCoreStateSnapshot> {
    return this.snapshot(commands, elapsedMs, warnings);
  }

  protected snapshot(commands: NativeMediaCoreCommand[], elapsedMs: number, warnings: string[] = []): NativeMediaCoreStateSnapshot {
    const sceneGraph = commands.find((command) => command.type === "load-scene-graph");
    const output = commands.find((command) => command.type === "start-program-output");
    const outputProfile = commands.find((command) => command.type === "set-output-profile");
    const sourceRoster = commands.find((command) => command.type === "set-zoom-source-roster");
    const sourceAdapter = commands.find((command) => command.type === "set-media-source-adapter");
    const activeSpeaker = commands.find((command) => command.type === "set-active-speaker");
    const screenShareSource = commands.find((command) => command.type === "set-screen-share-source");
    const colorGrade = commands.find((command) => command.type === "set-color-grade");
    const prepareEncoder = commands.find((command) => command.type === "prepare-encoder-session");
    const startEncoder = commands.find((command) => command.type === "start-encoder-session");
    const stopEncoder = commands.find((command) => command.type === "stop-encoder-session");
    const failedSender = commands.find((command) => command.type === "fail-output-sender");
    const recoveredSender = commands.find((command) => command.type === "recover-output-sender");
    const transforms = commands.filter((command) => command.type === "set-participant-transform");
    const overlays = commands.filter((command) => command.type === "set-overlay-asset");
    if (sourceRoster) {
      this.sources = sourceRoster.sources.map((source) => ({ ...source, hasVideo: source.hasVideo && source.health !== "video-off" }));
      this.activeSpeakerId = sourceRoster.sources.find((source) => source.isActiveSpeaker)?.participantId ?? this.activeSpeakerId;
      this.screenShareParticipantId = sourceRoster.sources.find((source) => source.isScreenSharing)?.participantId ?? this.screenShareParticipantId;
    }
    if (sourceAdapter) {
      this.mediaSourceKind = sourceAdapter.kind;
      this.mediaSourceAdapterId = sourceAdapter.adapterId ?? (sourceAdapter.kind === "test-pattern" ? "renderer-test-pattern" : `renderer-${sourceAdapter.kind}`);
      this.frameNumbers.clear();
      this.sourceDroppedFrameCount = 0;
      this.sourceLowResolutionFrameCount = 0;
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
    if (failedSender) {
      this.failOutputSender(failedSender.destination, failedSender.message, failedSender.failedAtMs ?? elapsedMs);
    }
    if (recoveredSender) {
      this.recoverOutputSender(recoveredSender.destination, recoveredSender.recoveredAtMs ?? elapsedMs, recoveredSender.reason);
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
    const isoParticipantIds = isoParticipantIdsFromCommands(commands, output?.isoParticipantIds ?? this.recordingTargets.isoParticipantIds);
    const isoReadinessIssues = buildIsoRecordingReadinessIssues(isoParticipantIds, this.sources);
    const frameSources = buildFrameSourceLayers(renderPlan, this.sources, isoParticipantIds);
    const frames = frameSources.map((layer, index) => this.frameFromRenderLayer(layer, index, elapsedMs));
    this.sourceSnapshot = this.buildSourceSnapshot(frames, elapsedMs);
    this.programFrame = this.composeProgramFrame(renderPlan, elapsedMs);
    this.programTransport = this.buildProgramTransport(this.programFrame, elapsedMs);
    const recording = this.syncRecordingCommands(commands, frames ?? [], elapsedMs, isoReadinessIssues);
    const encoderSession = this.encoderSession(output?.destinations ?? [], this.programFrame, recording);
    const outputSenderSession = this.outputSenderSession(output?.destinations ?? [], this.programFrame, elapsedMs);
    this.syncOutputSenderEvents(outputSenderSession.senders, elapsedMs);
    const outputHealth = this.outputHealth(output?.destinations ?? [], recording, this.programFrame, encoderSession, outputSenderSession);
    const compositor = this.compositorSnapshot();
    const operatorActions = buildNativeMediaCoreOperatorActions({
      sourceSnapshot: this.sourceSnapshot,
      renderPlan,
      programFrame: this.programFrame,
      programTransport: this.programTransport,
      encoderSession,
      outputSenderSession,
      recording
    });
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
    const sourceIssueDetails = new Set((this.sourceSnapshot.issues ?? []).map((issue) => issue.detail));
    this.syncSourceIssueEvents(this.sourceSnapshot.issues ?? [], elapsedMs);
    const recordingStreamWarningDetails = new Set((recording?.streams ?? []).map((stream) => stream.warning).filter(Boolean) as string[]);
    this.syncRecordingStreamEvents(recording?.streams ?? [], elapsedMs);
    this.addWarningsAsEvents(allWarnings.filter((warning) => !sourceIssueDetails.has(warning) && !recordingStreamWarningDetails.has(warning)), elapsedMs);

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
      outputSenderSession,
      sourceCount: this.sources.length,
      resolvedRouteCount: renderPlan.resolvedRouteCount,
      renderPlan,
      encoderSession,
      recording,
      operatorActions,
      eventLog: [...this.eventLog],
      diagnostics: {
        generatedAtMs: elapsedMs,
        sceneId: sceneGraph?.sceneId,
        routeCount: sceneGraph?.routes.length ?? 0,
        frameCount: frames.length,
        programFrameCount: compositor.programFrameCount,
        outputs: output?.destinations ?? [],
        outputProfile: this.outputProfile,
        outputHealth,
        outputSenderSession,
        sourceSnapshot: this.sourceSnapshot,
        renderPlan,
        compositor,
        programFrame: this.programFrame,
        programTransport: this.programTransport,
        encoderSession,
        recording,
        operatorActions,
        eventLog: [...this.eventLog],
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
    const zoomSource = findZoomSourceForLayer(this.sources, layer);
    const nextFrameNumber = (this.frameNumbers.get(sourceId) ?? 0) + 1;
    this.frameNumbers.set(sourceId, nextFrameNumber);
    const isStressPattern = this.mediaSourceKind !== "local-camera";
    const isLowResolution = (isStressPattern && index >= 4 && !isScreenShare) || (!isScreenShare && zoomSource?.health === "low-resolution");

    return {
      sourceId,
      participantId: layer.participantId,
      kind: isScreenShare ? "screen-share" : "participant-video",
      frameNumber: nextFrameNumber,
      timestampMs: elapsedMs,
      width: isScreenShare ? 1920 : this.mediaSourceKind === "local-camera" ? 1920 : isLowResolution ? 960 : 1280,
      height: isScreenShare ? 1080 : this.mediaSourceKind === "local-camera" ? 1080 : isLowResolution ? 540 : 720,
      fps: isScreenShare ? 30 : 60,
      health: isStressPattern && nextFrameNumber % 90 === 0 ? "dropped" : isLowResolution ? "low-resolution" : "live"
    } satisfies NativeMediaCoreFrame;
  }

  private buildSourceSnapshot(frames: NativeMediaCoreFrame[], elapsedMs: number): NativeMediaCoreFrameSourceSnapshot {
    const droppedThisTick = frames.filter((frame) => frame.health === "dropped").length;
    const lowResolutionThisTick = frames.filter((frame) => frame.health === "low-resolution").length;
    const issues = buildZoomSourceHealthIssues(frames, this.sources);
    this.sourceDroppedFrameCount += droppedThisTick;
    this.sourceLowResolutionFrameCount += lowResolutionThisTick;
    const warnings = [
      lowResolutionThisTick > 0 ? `${lowResolutionThisTick} source frame${lowResolutionThisTick === 1 ? "" : "s"} below target resolution.` : undefined,
      droppedThisTick > 0 ? `${droppedThisTick} source frame${droppedThisTick === 1 ? "" : "s"} dropped by media source.` : undefined,
      ...issues.map((issue) => issue.detail)
    ].filter(Boolean) as string[];

    return {
      adapterId: this.mediaSourceAdapterId,
      kind: this.mediaSourceKind,
      status: warnings.length ? "degraded" : frames.length > 0 ? "subscribed" : "idle",
      subscribedSourceCount: frames.length,
      liveFrameCount: frames.filter((frame) => frame.health === "live").length,
      staleFrameCount: frames.filter((frame) => elapsedMs - frame.timestampMs > 250).length,
      droppedFrameCount: this.sourceDroppedFrameCount,
      lowResolutionFrameCount: this.sourceLowResolutionFrameCount,
      lastFrameTimestampMs: frames.length ? elapsedMs : this.sourceSnapshot.lastFrameTimestampMs,
      issues,
      warnings
    };
  }

  private syncRecordingCommands(commands: NativeMediaCoreCommand[], frames: NativeMediaCoreFrame[], elapsedMs: number, isoReadinessIssues: IsoRecordingReadinessIssue[] = []) {
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

      if (command.type === "recover-recording-session") {
        this.recoverRecording(command.recoveredAtMs ?? elapsedMs);
        this.addEvent("info", "recording", "Recording writer recovery requested", command.reason ?? "Recording writer recovered.", command.type, this.recording?.sessionId, command.recoveredAtMs ?? elapsedMs);
      }
    });

    if (this.recording?.active) {
      this.writeRecordingFrames(frames, elapsedMs, this.programFrame);
    }

    return applyIsoRecordingReadiness(this.snapshotRecording(elapsedMs), isoReadinessIssues);
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
      const expectedFrames = stream.kind === "program" ? (programFrame ? 1 : 0) : 1;
      const droppedFrames =
        stream.kind === "program"
          ? programFrame?.health === "dropped"
            ? 1
            : 0
          : frames.filter((frame) => frame.kind === "participant-video" && frame.health === "dropped" && frame.participantId === stream.participantId).length;
      const writtenFrames =
        stream.kind === "program"
          ? programFrame && programFrame.health !== "dropped"
            ? 1
            : 0
          : writableFrames.filter((frame) => frame.kind === "participant-video" && frame.participantId === stream.participantId).length;
      const missingFrames = Math.max(0, expectedFrames - writtenFrames - droppedFrames);

      stream.expectedFrames = (stream.expectedFrames ?? 0) + expectedFrames;
      stream.missingFrames = (stream.missingFrames ?? 0) + missingFrames;
      if (stream.kind === "program") {
        stream.framesWritten += writtenFrames;
        stream.droppedFrames += droppedFrames;
        stream.bytesWritten += writtenFrames * 260_000;
      } else {
        stream.framesWritten += writtenFrames;
        stream.droppedFrames += droppedFrames;
        stream.bytesWritten += writtenFrames * 140_000;
      }
      stream.lastFrameTimestampMs = writtenFrames > 0 ? elapsedMs : stream.lastFrameTimestampMs;
      stream.warning = recordingStreamWarning(stream);
      stream.status = stream.warning ? "warning" : "writing";
    });
    this.recording.totalFramesWritten = this.recording.streams.reduce((total, stream) => total + stream.framesWritten, 0);
    this.recording.totalDroppedFrames = this.recording.streams.reduce((total, stream) => total + stream.droppedFrames, 0);
    this.recording.totalBytesWritten = this.recording.streams.reduce((total, stream) => total + stream.bytesWritten, 0);
    const streamWarning = this.recording.streams.map(recordingStreamWarning).find(Boolean);
    if (this.recording.totalDroppedFrames > 0 || streamWarning) {
      this.recording.status = "warning";
      this.recording.writerStatus = "warning";
      this.recording.warning = this.recording.totalDroppedFrames > 0 ? "Recording writer is skipping dropped frames." : streamWarning;
      this.recording.streams.forEach((stream) => {
        stream.warning = recordingStreamWarning(stream);
        stream.status = stream.warning ? "warning" : "writing";
      });
    } else {
      this.recording.status = "recording";
      this.recording.writerStatus = "writing";
      this.recording.warning = undefined;
      this.recording.streams.forEach((stream) => {
        stream.warning = undefined;
        stream.status = "writing";
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
      this.addEvent("critical", "recording", "Recording writer failed", message, "fail-recording-session", this.recording.sessionId, elapsedMs);
    }
  }

  private recoverRecording(elapsedMs: number) {
    if (!this.recording) {
      this.startRecording(this.recordingTargets, elapsedMs);
    }

    if (this.recording) {
      this.recording.active = true;
      this.recording.status = "recording";
      this.recording.writerStatus = "writing";
      this.recording.error = undefined;
      this.recording.warning = undefined;
      this.recording.stoppedAtMs = undefined;
      this.recording.elapsedMs = Math.max(0, elapsedMs - this.recording.startedAtMs);
      this.recording.streams.forEach((stream) => {
        stream.status = "writing";
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
    encoderSession: NativeMediaCoreEncoderSession,
    outputSenderSession: NativeMediaCoreOutputSenderSession
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

    outputSenderSession.senders
      .filter((sender) => encoderSession.lifecycle.status === "encoding" && sender.status !== "stopped" && sender.status !== "idle")
      .forEach((sender) => {
        const index = health.findIndex((item) => item.destination === sender.destination);
        const senderHealth: NativeMediaCoreOutputHealth = {
          destination: sender.destination,
          status: sender.status === "failed" ? "failed" : sender.status === "warning" || sender.status === "starting" ? "warning" : sender.status === "live" ? "live" : "idle",
          message: sender.warning ?? `${sender.destination.toUpperCase()} sender ${sender.status}.`,
          droppedFrames: 0
        };
        if (index >= 0) {
          health[index] = senderHealth;
        } else {
          health.push(senderHealth);
        }
      });

    return health;
  }

  private outputSenderSession(
    destinations: Array<"rtmp" | "ndi" | "srt" | "webrtc" | "recording">,
    programFrame: NativeMediaCoreProgramFrame | undefined,
    elapsedMs: number
  ): NativeMediaCoreOutputSenderSession {
    const networkDestinations = destinations.filter((destination): destination is "rtmp" | "ndi" | "srt" | "webrtc" => destination !== "recording");
    const activeSet = new Set(networkDestinations);

    [...this.outputSenders.entries()].forEach(([destination, sender]) => {
      if (!activeSet.has(destination) && sender.status !== "stopped") {
        this.outputSenders.set(destination, { ...sender, status: "stopped", stoppedAtMs: elapsedMs, warning: undefined });
      }
    });

    networkDestinations.forEach((destination) => {
      const existing = this.outputSenders.get(destination);
      this.outputSenders.set(destination, nextOutputSender(destination, existing, programFrame, this.outputProfile, elapsedMs));
    });

    const senders = [...this.outputSenders.values()].sort((left, right) => left.destination.localeCompare(right.destination));
    const warnings = [...new Set(senders.map((sender) => sender.warning).filter(Boolean) as string[])];
    const activeSenderCount = senders.filter((sender) => sender.status === "live" || sender.status === "warning" || sender.status === "starting").length;
    const hasFailure = senders.some((sender) => sender.status === "failed");
    const hasWarning = senders.some((sender) => sender.status === "warning");

    return {
      status: hasFailure ? "failed" : hasWarning ? "warning" : activeSenderCount > 0 ? "live" : "idle",
      activeSenderCount,
      senders,
      warnings
    };
  }

  private failOutputSender(destination: "rtmp" | "ndi" | "srt" | "webrtc", message: string, elapsedMs: number) {
    const existing = this.outputSenders.get(destination);
    this.outputSenders.set(destination, {
      senderId: existing?.senderId ?? `${destination}:program`,
      destination,
      status: "failed",
      startedAtMs: existing?.startedAtMs,
      stoppedAtMs: elapsedMs,
      lastFrameNumber: existing?.lastFrameNumber,
      framesSent: existing?.framesSent ?? 0,
      retryCount: (existing?.retryCount ?? 0) + 1,
      latencyMs: existing?.latencyMs ?? senderLatency(destination),
      bitrateMbps: existing?.bitrateMbps ?? 0,
      warning: message
    });
    this.addEvent("critical", "sender", `${destination.toUpperCase()} sender failed`, message, "fail-output-sender", destination, elapsedMs);
  }

  private recoverOutputSender(destination: "rtmp" | "ndi" | "srt" | "webrtc", elapsedMs: number, reason = `${destination.toUpperCase()} sender recovered.`) {
    const existing = this.outputSenders.get(destination);
    this.outputSenders.set(destination, {
      senderId: existing?.senderId ?? `${destination}:program`,
      destination,
      status: "starting",
      startedAtMs: elapsedMs,
      stoppedAtMs: undefined,
      lastFrameNumber: existing?.lastFrameNumber,
      framesSent: existing?.framesSent ?? 0,
      retryCount: existing?.retryCount ?? 0,
      latencyMs: existing?.latencyMs ?? senderLatency(destination),
      bitrateMbps: existing?.bitrateMbps ?? 0,
      warning: reason
    });
    this.addEvent("info", "sender", `${destination.toUpperCase()} sender recovery requested`, reason, "recover-output-sender", destination, elapsedMs);
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
            warning: stream.status === "warning" ? stream.warning ?? recording.warning : undefined
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

  private addWarningsAsEvents(warnings: string[], elapsedMs: number) {
    warnings.forEach((warning) => this.addEvent("warning", "system", "Media core warning", warning, undefined, undefined, elapsedMs));
  }

  private syncSourceIssueEvents(issues: NativeMediaCoreSourceHealthIssue[], elapsedMs: number) {
    const currentKeys = new Set(issues.map(sourceIssueKey));
    issues.forEach((issue) => {
      this.activeSourceIssues.set(sourceIssueKey(issue), issue);
      this.addEvent(issue.severity, "source", sourceIssueTitle(issue), issue.detail, undefined, issue.participantId ?? issue.sourceId, elapsedMs);
    });
    [...this.activeSourceIssues.entries()]
      .filter(([key]) => !currentKeys.has(key))
      .forEach(([key, issue]) => {
        this.activeSourceIssues.delete(key);
        this.addEvent("info", "source", "Zoom feed recovered", sourceIssueRecoveryDetail(issue), undefined, issue.participantId ?? issue.sourceId, elapsedMs);
      });
  }

  private syncOutputSenderEvents(senders: NativeMediaCoreOutputSender[], elapsedMs: number) {
    senders.forEach((sender) => {
      const previous = this.outputSenderEventStates.get(sender.destination);
      if (previous?.status === sender.status && previous.warning === sender.warning) {
        return;
      }

      this.outputSenderEventStates.set(sender.destination, { status: sender.status, warning: sender.warning });
      const event = outputSenderTransitionEvent(sender, previous?.status, this.outputProfile);
      this.addEvent(event.severity, "sender", event.title, event.detail, undefined, sender.senderId, elapsedMs);
    });
  }

  private syncRecordingStreamEvents(streams: NativeMediaCoreRecordingStream[], elapsedMs: number) {
    const currentWarningKeys = new Set<string>();
    streams.forEach((stream) => {
      const key = recordingStreamKey(stream);
      if (!stream.warning) {
        return;
      }

      currentWarningKeys.add(key);
      if (this.recordingStreamWarnings.get(key) === stream.warning) {
        return;
      }

      this.recordingStreamWarnings.set(key, stream.warning);
      this.addEvent("warning", "recording", recordingStreamWarningTitle(stream), stream.warning, undefined, stream.participantId ?? stream.kind, elapsedMs);
    });

    [...this.recordingStreamWarnings.entries()]
      .filter(([key]) => !currentWarningKeys.has(key))
      .forEach(([key, warning]) => {
        this.recordingStreamWarnings.delete(key);
        this.addEvent("info", "recording", "Recording stream recovered", recordingStreamRecoveryDetail(key, warning), undefined, key, elapsedMs);
      });
  }

  private addEvent(
    severity: NativeMediaCoreEvent["severity"],
    area: NativeMediaCoreEvent["area"],
    title: string,
    detail: string,
    commandType?: NativeMediaCoreCommand["type"],
    relatedId?: string,
    atMs = 0
  ) {
    const eventKey = `${severity}:${area}:${title}:${detail}:${relatedId ?? ""}`;
    if (this.seenEventKeys.has(eventKey)) {
      return;
    }

    this.seenEventKeys.add(eventKey);
    this.eventLog.push({
      eventId: `${atMs}-${this.eventLog.length + 1}-${area}`,
      atMs,
      severity,
      area,
      title,
      detail,
      relatedId,
      commandType
    });

    if (this.eventLog.length > MAX_EVENT_LOG_LENGTH) {
      const removed = this.eventLog.splice(0, this.eventLog.length - MAX_EVENT_LOG_LENGTH);
      removed.forEach((event) => this.seenEventKeys.delete(`${event.severity}:${event.area}:${event.title}:${event.detail}:${event.relatedId ?? ""}`));
    }
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
    expectedFrames: 0,
    framesWritten: 0,
    missingFrames: 0,
    droppedFrames: 0,
    bytesWritten: 0
  };
}

function recordingStreamWarning(stream: NativeMediaCoreRecordingStream) {
  if (stream.kind === "iso" && (stream.missingFrames ?? 0) > 0) {
    return `${stream.participantId ?? "Unknown participant"} ISO has no clean participant frames.`;
  }

  if (stream.droppedFrames > 0) {
    return stream.kind === "program" ? "Program recording is skipping dropped frames." : `${stream.participantId ?? "Unknown participant"} ISO is skipping dropped frames.`;
  }

  return undefined;
}

function applyIsoRecordingReadiness(recording: NativeMediaCoreRecordingSession | undefined, issues: IsoRecordingReadinessIssue[]) {
  if (!recording || issues.length === 0 || recording.status === "failed" || recording.status === "stopped") {
    return recording;
  }

  const issuesByParticipantId = new Map(issues.map((issue) => [issue.participantId, issue]));
  let readinessWarning: string | undefined;
  const streams = recording.streams.map((stream) => {
    if (stream.kind !== "iso" || !stream.participantId) {
      return stream;
    }

    const issue = issuesByParticipantId.get(stream.participantId);
    if (!issue) {
      return {
        ...stream,
        readiness: "ready" as const
      };
    }

    readinessWarning ??= issue.warning;
    return {
      ...stream,
      readiness: issue.readiness,
      status: "warning" as const,
      warning: issue.warning
    };
  });

  if (!readinessWarning) {
    return {
      ...recording,
      streams
    };
  }

  return {
    ...recording,
    status: "warning" as const,
    writerStatus: "warning" as const,
    warning: readinessWarning,
    streams
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

function nextOutputSender(
  destination: "rtmp" | "ndi" | "srt" | "webrtc",
  existing: NativeMediaCoreOutputSender | undefined,
  programFrame: NativeMediaCoreProgramFrame | undefined,
  outputProfile: NativeMediaCoreOutputProfile,
  elapsedMs: number
): NativeMediaCoreOutputSender {
  const base = {
    senderId: existing?.senderId ?? `${destination}:program`,
    destination,
    startedAtMs: existing?.startedAtMs ?? elapsedMs,
    stoppedAtMs: undefined,
    framesSent: existing?.framesSent ?? 0,
    retryCount: existing?.retryCount ?? 0,
    latencyMs: senderLatency(destination),
    bitrateMbps: senderBitrate(destination, outputProfile)
  };

  if (existing?.status === "failed") {
    return {
      ...base,
      status: "failed",
      stoppedAtMs: existing.stoppedAtMs,
      lastFrameNumber: existing.lastFrameNumber,
      retryCount: existing.retryCount,
      warning: existing.warning
    };
  }

  if (!programFrame) {
    return {
      ...base,
      status: "starting",
      warning: `${destination.toUpperCase()} sender is waiting for a program frame.`
    };
  }

  if (programFrame.health === "dropped") {
    return {
      ...base,
      status: "warning",
      lastFrameNumber: existing?.lastFrameNumber,
      retryCount: (existing?.retryCount ?? 0) + 1,
      warning: `${destination.toUpperCase()} sender skipped a dropped program frame.`
    };
  }

  return {
    ...base,
    status: programFrame.health === "degraded" ? "warning" : "live",
    lastFrameNumber: programFrame.frameNumber,
    framesSent: (existing?.framesSent ?? 0) + 1,
    warning: programFrame.health === "degraded" ? `${destination.toUpperCase()} sender is publishing degraded program frames.` : undefined
  };
}

function senderLatency(destination: "rtmp" | "ndi" | "srt" | "webrtc") {
  if (destination === "ndi") {
    return 80;
  }
  if (destination === "webrtc") {
    return 220;
  }
  if (destination === "srt") {
    return 420;
  }
  return 2100;
}

function senderBitrate(destination: "rtmp" | "ndi" | "srt" | "webrtc", outputProfile: NativeMediaCoreOutputProfile) {
  const multiplier = destination === "ndi" ? 1.6 : destination === "webrtc" ? 0.8 : 1;
  return Number((outputProfile.targetBitrateMbps * multiplier).toFixed(1));
}

function recordingStreamKey(stream: Pick<NativeMediaCoreRecordingStream, "kind" | "participantId">) {
  return stream.kind === "program" ? "program" : `iso:${stream.participantId ?? "unknown"}`;
}

function recordingStreamWarningTitle(stream: Pick<NativeMediaCoreRecordingStream, "kind">) {
  return stream.kind === "program" ? "Program recording warning" : "ISO recording warning";
}

function recordingStreamRecoveryDetail(key: string, warning: string) {
  const label = key.startsWith("iso:") ? `${key.slice(4)} ISO` : "Program recording";
  return `${label} recovered after warning: ${warning}`;
}

function outputSenderTransitionEvent(
  sender: NativeMediaCoreOutputSender,
  previousStatus: NativeMediaCoreOutputSender["status"] | undefined,
  outputProfile: NativeMediaCoreOutputProfile
): Pick<NativeMediaCoreEvent, "severity" | "title" | "detail"> {
  const label = sender.destination.toUpperCase();

  if (sender.status === "failed") {
    return {
      severity: "critical",
      title: `${label} sender failed`,
      detail: sender.warning ?? `${label} sender failed.`
    };
  }

  if (sender.status === "warning") {
    return {
      severity: "warning",
      title: `${label} sender warning`,
      detail: sender.warning ?? `${label} sender is degraded.`
    };
  }

  if (sender.status === "stopped") {
    return {
      severity: "info",
      title: `${label} sender stopped`,
      detail: `${label} sender stopped after ${sender.framesSent} frame${sender.framesSent === 1 ? "" : "s"}.`
    };
  }

  if (sender.status === "starting") {
    return {
      severity: "info",
      title: `${label} sender starting`,
      detail: sender.warning ?? `${label} sender is starting.`
    };
  }

  if (previousStatus && previousStatus !== "live") {
    return {
      severity: "info",
      title: `${label} sender recovered`,
      detail: `${label} sender recovered at ${formatOutputProfileLabel(outputProfile)}.`
    };
  }

  return {
    severity: "info",
    title: `${label} sender live`,
    detail: `${label} sender live at ${formatOutputProfileLabel(outputProfile)}; ${sender.bitrateMbps} Mbps target.`
  };
}

function formatOutputProfileLabel(outputProfile: NativeMediaCoreOutputProfile) {
  return `${outputProfile.resolution} ${outputProfile.fps}fps`;
}

function findZoomSourceForLayer(sources: NativeMediaCoreZoomSource[], layer: NativeMediaCoreRenderPlan["layers"][number]) {
  if (layer.participantId) {
    return sources.find((source) => source.participantId === layer.participantId);
  }

  if (layer.sourceId) {
    return sources.find((source) => source.sourceId === layer.sourceId);
  }

  return undefined;
}

function buildFrameSourceLayers(
  renderPlan: NativeMediaCoreRenderPlan,
  sources: NativeMediaCoreZoomSource[],
  isoParticipantIds: string[]
): NativeMediaCoreRenderPlan["layers"] {
  const layers = renderPlan.layers.filter((layer) => layer.kind === "participant-video" || layer.kind === "screen-share");
  const seenSourceIds = new Set(layers.map((layer) => layer.sourceId ?? layer.layerId));
  const isoLayers: NativeMediaCoreRenderPlan["layers"] = [];

  isoParticipantIds.forEach((participantId, index) => {
    const source = sources.find((item) => item.participantId === participantId && item.hasVideo && item.health !== "video-off");
    if (!source || seenSourceIds.has(source.sourceId)) {
      return;
    }

    seenSourceIds.add(source.sourceId);
    isoLayers.push({
      layerId: `recording:iso:${participantId}`,
      kind: "participant-video",
      sourceId: source.sourceId,
      participantId,
      order: renderPlan.layers.length + index
    });
  });

  return [...layers, ...isoLayers];
}

function buildIsoRecordingReadinessIssues(isoParticipantIds: string[], sources: NativeMediaCoreZoomSource[]): IsoRecordingReadinessIssue[] {
  return isoParticipantIds
    .map((participantId) => {
      const source = sources.find((item) => item.participantId === participantId);
      if (!source) {
        return {
          participantId,
          readiness: "missing" as const,
          warning: `${participantId} ISO participant is not present in the Zoom roster.`
        };
      }

      if (source.health === "video-off") {
        return {
          participantId,
          readiness: "video-off" as const,
          warning: `${source.displayName} ISO video is off.`
        };
      }

      if (!source.hasVideo) {
        return {
          participantId,
          readiness: "unsubscribable" as const,
          warning: `${source.displayName} ISO video cannot be subscribed by the Zoom SDK.`
        };
      }

      return undefined;
    })
    .filter((issue): issue is IsoRecordingReadinessIssue => Boolean(issue));
}

function isoParticipantIdsFromCommands(commands: NativeMediaCoreCommand[], fallbackIds: string[]) {
  const commandIds = commands.flatMap((command) => {
    if (command.type === "start-program-output" || command.type === "set-recording-targets" || command.type === "start-recording-session") {
      return command.isoParticipantIds ?? [];
    }

    return [];
  });

  return [...new Set(commandIds.length > 0 ? commandIds : fallbackIds)].sort();
}

function buildZoomSourceHealthIssues(frames: NativeMediaCoreFrame[], sources: NativeMediaCoreZoomSource[]): NativeMediaCoreSourceHealthIssue[] {
  const renderedParticipantIds = new Set(frames.map((frame) => frame.participantId).filter(Boolean) as string[]);
  const renderedSourceIds = new Set(frames.map((frame) => frame.sourceId));

  return sources
    .filter((source) => renderedParticipantIds.has(source.participantId) || renderedSourceIds.has(source.sourceId))
    .map(sourceHealthIssue)
    .filter((issue): issue is NativeMediaCoreSourceHealthIssue => Boolean(issue));
}

function sourceHealthIssue(source: NativeMediaCoreZoomSource): NativeMediaCoreSourceHealthIssue | undefined {
  if (source.health === "live") {
    return undefined;
  }

  const detail =
    source.health === "low-resolution"
      ? `${source.displayName} feed is below target resolution.`
      : source.health === "recovering"
        ? `${source.displayName} feed is recovering.`
        : `${source.displayName} camera is off.`;

  return {
    sourceId: source.sourceId,
    participantId: source.participantId,
    displayName: source.displayName,
    health: source.health,
    severity: source.health === "video-off" ? "critical" : "warning",
    detail
  };
}

function sourceIssueTitle(issue: NativeMediaCoreSourceHealthIssue) {
  if (issue.health === "recovering") {
    return "Zoom feed recovering";
  }

  if (issue.health === "video-off") {
    return "Zoom feed unavailable";
  }

  return "Zoom feed quality warning";
}

function sourceIssueKey(issue: Pick<NativeMediaCoreSourceHealthIssue, "participantId" | "sourceId">) {
  return issue.participantId ?? issue.sourceId;
}

function sourceIssueRecoveryDetail(issue: NativeMediaCoreSourceHealthIssue) {
  const label = issue.displayName ?? issue.participantId ?? issue.sourceId;
  return `${label} feed recovered.`;
}

export class NativeHostMediaCoreSyncEngine extends InMemoryMediaCoreSyncEngine {
  constructor(private readonly bridge: NativeHostBridge) {
    super();
  }

  protected override async syncCommands(commands: NativeMediaCoreCommand[], elapsedMs: number, warnings: string[] = []): Promise<NativeMediaCoreStateSnapshot> {
    if (!this.bridge.syncMediaCore) {
      return this.snapshot(commands, elapsedMs, [...warnings, "Native host has no media-core sync bridge; using renderer-side simulation."]);
    }

    return this.bridge.syncMediaCore(commands, elapsedMs);
  }
}

function mediaCoreCommandFromOperatorAction(action: NativeMediaCoreOperatorAction, elapsedMs: number): NativeMediaCoreCommand | undefined {
  if (!action.command) {
    return undefined;
  }

  if (action.command === "recover-recording-session") {
    return {
      type: "recover-recording-session",
      recoveredAtMs: elapsedMs,
      reason: "Recording writer recovered from operator action."
    };
  }

  const senderMatch = /^recover-output-sender:(rtmp|ndi|srt|webrtc)$/.exec(action.command);
  if (senderMatch) {
    const destination = senderMatch[1] as "rtmp" | "ndi" | "srt" | "webrtc";
    return {
      type: "recover-output-sender",
      destination,
      recoveredAtMs: elapsedMs,
      reason: `${destination.toUpperCase()} sender recovered from operator action.`
    };
  }

  return undefined;
}
