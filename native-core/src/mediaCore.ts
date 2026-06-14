import type {
  MediaCoreCommand,
  MediaCoreColorGrade,
  MediaCoreDestination,
  MediaCoreDiagnosticsSnapshot,
  MediaCoreEncoderLifecycle,
  MediaCoreEncoderSession,
  MediaCoreEvent,
  MediaCoreEventArea,
  MediaCoreEventSeverity,
  MediaCoreFrame,
  MediaCoreFrameSourceSnapshot,
  MediaCoreOutputHealth,
  MediaCoreOutputProfile,
  MediaCoreOutputSender,
  MediaCoreOutputSenderSession,
  MediaCoreProgramFrame,
  MediaCoreProgramFrameTransport,
  MediaCoreRecordingStream,
  MediaCoreRenderPlan,
  MediaCoreRequest,
  MediaCoreResponse,
  MediaCoreStateSnapshot,
  MediaCoreSourceHealthIssue,
  MediaCoreZoomSource
} from "./protocol.js";
import { TestPatternMediaSource, createMediaFrameSource, type MediaCoreFrameSourceRequest, type MediaFrameSource } from "./mediaSource.js";
import { RecordingSink } from "./recordingSink.js";
import { buildRenderPlan } from "./renderPlan.js";
import { ProgramCompositor } from "./compositor.js";
import { buildEncoderSession } from "./encoderSession.js";
import { OutputSenderSessionModel } from "./outputSenderSession.js";
import { buildOperatorActions } from "./operatorActions.js";

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

const MAX_EVENT_LOG_LENGTH = 80;

type IsoRecordingReadiness = NonNullable<MediaCoreRecordingStream["readiness"]>;

type IsoRecordingReadinessIssue = {
  participantId: string;
  readiness: Exclude<IsoRecordingReadiness, "ready">;
  warning: string;
};

type ZoomSourceLifecycleState = Pick<
  MediaCoreZoomSource,
  "participantId" | "displayName" | "breakoutRoomName" | "hasVideo" | "isMuted" | "isActiveSpeaker" | "isScreenSharing" | "health"
>;

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
  private readonly eventLog: MediaCoreEvent[] = [];
  private readonly seenEventKeys = new Set<string>();
  private readonly activeSourceIssues = new Map<string, MediaCoreSourceHealthIssue>();
  private readonly outputSenderEventStates = new Map<string, { status: MediaCoreOutputSender["status"]; warning?: string }>();
  private readonly recordingStreamWarnings = new Map<string, string>();
  private readonly zoomSourceLifecycleStates = new Map<string, ZoomSourceLifecycleState>();
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
          this.warn(warnings, "routing", "Scene graph has no routes", "Scene graph loaded without routes.", command.type);
        }
        return;
      }

      if (command.type === "set-participant-transform") {
        this.transforms.set(command.participantId, normalizeTransform(command));
        if (command.crop.width <= 0 || command.crop.height <= 0) {
          this.warn(warnings, "program", "Transform has empty crop", `${command.participantId} transform has an empty crop region.`, command.type, command.participantId);
        }
        return;
      }

      if (command.type === "set-overlay-asset") {
        this.overlays.set(command.overlayId, command);
        if (!command.text && !command.imageUri) {
          this.warn(warnings, "program", "Overlay has no asset", `${command.overlayId} overlay has no text or image asset.`, command.type, command.overlayId);
        }
        return;
      }

      if (command.type === "set-zoom-source-roster") {
        this.sources = normalizeSources(command.sources);
        this.syncZoomSourceLifecycleEvents(this.sources);
        this.activeSpeakerId = command.sources.find((source) => source.isActiveSpeaker)?.participantId ?? this.activeSpeakerId;
        this.screenShareParticipantId = command.sources.find((source) => source.isScreenSharing)?.participantId ?? this.screenShareParticipantId;
        if (command.sources.length === 0) {
          this.warn(warnings, "source", "Zoom roster is empty", "Zoom source roster is empty.", command.type);
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
          this.warn(warnings, "routing", "Active speaker missing", `${command.participantId} active speaker is not present in the Zoom source roster.`, command.type, command.participantId);
        }
        return;
      }

      if (command.type === "set-screen-share-source") {
        this.screenShareParticipantId = command.participantId;
        if (command.participantId && !this.sources.some((source) => source.participantId === command.participantId && source.isScreenSharing)) {
          this.warn(warnings, "routing", "Screen share source missing", `${command.participantId} is not publishing a screen share source.`, command.type, command.participantId);
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
          this.warn(warnings, "encoder", "4K encoder required", `${this.outputProfile.resolution} output profile requires 4K-capable GPU encoding.`, command.type, this.outputProfile.profileId);
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
          this.warn(warnings, "sender", "Output has no destinations", "Program output started without destinations.", command.type);
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
        this.warn(warnings, "sender", `${command.destination.toUpperCase()} sender failed`, command.message, command.type, command.destination, "critical");
        return;
      }

      if (command.type === "recover-output-sender") {
        this.outputSenderSessionModel.recover(command.destination, command.recoveredAtMs ?? this.elapsedMs, command.reason);
        this.addEvent(
          "info",
          "sender",
          `${command.destination.toUpperCase()} sender recovery requested`,
          command.reason ?? `${command.destination.toUpperCase()} sender recovered.`,
          command.type,
          command.destination,
          command.recoveredAtMs ?? this.elapsedMs
        );
        return;
      }

      if (command.type === "set-recording-targets") {
        const recording = this.recordingSink.setTargets(command);
        if (recording?.warning) {
          this.warn(warnings, "recording", "Recording target warning", recording.warning, command.type, recording.sessionId);
        }
        return;
      }

      if (command.type === "start-recording-session") {
        const recording = this.recordingSink.start(command, command.startedAtMs ?? this.elapsedMs, command.sessionId);
        if (recording?.warning) {
          this.warn(warnings, "recording", "Recording writer warning", recording.warning, command.type, recording.sessionId);
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
        this.warn(warnings, "recording", "Recording writer failed", command.message, command.type, recording?.sessionId, "critical");
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
        this.addEvent(
          "info",
          "recording",
          "Recording writer recovery requested",
          command.reason ?? recording?.warning ?? "Recording writer recovered.",
          command.type,
          recording?.sessionId,
          command.recoveredAtMs ?? this.elapsedMs
        );
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
    const recording = applyIsoRecordingReadiness(
      this.recordingSink.snapshot(),
      buildIsoRecordingReadinessIssues(this.recordingIsoParticipantIds(), this.sources)
    );
    const renderPlan = this.renderPlan();
    const compositor = this.compositor.snapshot();
    const encoderSession = this.encoderSession(recording);
    const outputSenderSession = this.outputSenderSession();
    this.syncOutputSenderEvents(outputSenderSession.senders);
    const outputHealth = this.buildOutputHealth(recording, this.programFrame, encoderSession, outputSenderSession);
    const operatorActions = buildOperatorActions({
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
    this.syncSourceIssueEvents(this.sourceSnapshot.issues ?? []);
    const recordingStreamWarningDetails = new Set((recording?.streams ?? []).map((stream) => stream.warning).filter(Boolean) as string[]);
    this.syncRecordingStreamEvents(recording?.streams ?? []);
    this.addWarningsAsEvents(allWarnings.filter((warning) => !sourceIssueDetails.has(warning) && !recordingStreamWarningDetails.has(warning)));

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
      operatorActions,
      eventLog: [...this.eventLog],
      diagnostics: this.diagnostics(outputHealth, allWarnings, recording, renderPlan, compositor, encoderSession, outputSenderSession, operatorActions),
      lastCommandTypes: this.lastCommandTypes,
      warnings: allWarnings
    };
  }

  private tick(elapsedMs: number) {
    this.elapsedMs = Math.max(0, elapsedMs);
    const renderPlan = this.renderPlan();
    const sourceResult = this.mediaSource.render(this.getFrameSources(renderPlan, this.recordingIsoParticipantIds()), this.elapsedMs);
    this.frames = sourceResult.frames;
    this.sourceSnapshot = enrichSourceSnapshotWithZoomIssues(sourceResult.snapshot, this.frames, this.sources);
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
    outputSenderSession: MediaCoreOutputSenderSession,
    operatorActions: MediaCoreStateSnapshot["operatorActions"]
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
      operatorActions,
      eventLog: [...this.eventLog],
      warnings,
      lastCommandTypes: this.lastCommandTypes
    };
  }

  private addWarningsAsEvents(warnings: string[]) {
    warnings.forEach((warning) => this.addEvent("warning", "system", "Media core warning", warning));
  }

  private syncSourceIssueEvents(issues: MediaCoreSourceHealthIssue[]) {
    const currentKeys = new Set(issues.map(sourceIssueKey));
    issues.forEach((issue) => {
      this.activeSourceIssues.set(sourceIssueKey(issue), issue);
      this.addEvent(issue.severity, "source", sourceIssueTitle(issue), issue.detail, undefined, issue.participantId ?? issue.sourceId);
    });
    [...this.activeSourceIssues.entries()]
      .filter(([key]) => !currentKeys.has(key))
      .forEach(([key, issue]) => {
        this.activeSourceIssues.delete(key);
        this.addEvent("info", "source", "Zoom feed recovered", sourceIssueRecoveryDetail(issue), undefined, issue.participantId ?? issue.sourceId);
      });
  }

  private syncZoomSourceLifecycleEvents(sources: MediaCoreZoomSource[]) {
    const nextStates = new Map(sources.map((source) => [source.participantId, zoomSourceLifecycleState(source)]));
    if (this.zoomSourceLifecycleStates.size === 0) {
      replaceMap(this.zoomSourceLifecycleStates, nextStates);
      return;
    }

    nextStates.forEach((state, participantId) => {
      const previous = this.zoomSourceLifecycleStates.get(participantId);
      if (!previous) {
        this.addEvent("info", "source", "Zoom participant joined", `${state.displayName} joined ${state.breakoutRoomName}.`, undefined, participantId);
        return;
      }

      zoomSourceLifecycleEvents(previous, state).forEach((event) => {
        this.addEvent(event.severity, "source", event.title, event.detail, undefined, participantId);
      });
    });

    this.zoomSourceLifecycleStates.forEach((state, participantId) => {
      if (!nextStates.has(participantId)) {
        this.addEvent("info", "source", "Zoom participant left", `${state.displayName} left ${state.breakoutRoomName}.`, undefined, participantId);
      }
    });

    replaceMap(this.zoomSourceLifecycleStates, nextStates);
  }

  private syncOutputSenderEvents(senders: MediaCoreOutputSender[]) {
    senders.forEach((sender) => {
      const previous = this.outputSenderEventStates.get(sender.destination);
      if (previous?.status === sender.status && previous?.warning === sender.warning) {
        return;
      }

      this.outputSenderEventStates.set(sender.destination, { status: sender.status, warning: sender.warning });
      const event = outputSenderTransitionEvent(sender, previous?.status, this.outputProfile);
      this.addEvent(event.severity, "sender", event.title, event.detail, undefined, sender.senderId);
    });
  }

  private syncRecordingStreamEvents(streams: MediaCoreRecordingStream[]) {
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
      this.addEvent("warning", "recording", recordingStreamWarningTitle(stream), stream.warning, undefined, stream.participantId ?? stream.kind);
    });

    [...this.recordingStreamWarnings.entries()]
      .filter(([key]) => !currentWarningKeys.has(key))
      .forEach(([key, warning]) => {
        this.recordingStreamWarnings.delete(key);
        this.addEvent("info", "recording", "Recording stream recovered", recordingStreamRecoveryDetail(key, warning), undefined, key);
      });
  }

  private warn(
    warnings: string[],
    area: MediaCoreEventArea,
    title: string,
    detail: string,
    commandType?: MediaCoreCommand["type"],
    relatedId?: string,
    severity: MediaCoreEventSeverity = "warning"
  ) {
    warnings.push(detail);
    this.addEvent(severity, area, title, detail, commandType, relatedId);
  }

  private addEvent(
    severity: MediaCoreEventSeverity,
    area: MediaCoreEventArea,
    title: string,
    detail: string,
    commandType?: MediaCoreCommand["type"],
    relatedId?: string,
    atMs = this.elapsedMs
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

  private getFrameSources(renderPlan: MediaCoreRenderPlan, isoParticipantIds: string[] = []): MediaCoreFrameSourceRequest[] {
    const frameSources = renderPlan.layers
      .filter((layer): layer is typeof layer & { kind: "participant-video" | "screen-share" } => layer.kind === "participant-video" || layer.kind === "screen-share")
      .map((layer) => ({
        sourceId: layer.sourceId ?? layer.layerId,
        participantId: layer.participantId,
        kind: layer.kind
      }));
    const seenSourceIds = new Set(frameSources.map((source) => source.sourceId));
    const isoSources: MediaCoreFrameSourceRequest[] = [];
    isoParticipantIds.forEach((participantId) => {
      const source = this.sources.find((item) => item.participantId === participantId && item.hasVideo && item.health !== "video-off");
      if (!source || seenSourceIds.has(source.sourceId)) {
        return;
      }

      seenSourceIds.add(source.sourceId);
      isoSources.push({
        sourceId: source.sourceId,
        participantId,
        kind: "participant-video"
      });
    });

    return [...frameSources, ...isoSources];
  }

  private recordingIsoParticipantIds() {
    const recordingIds = this.recordingSink
      .snapshot()
      ?.streams.filter((stream) => stream.kind === "iso" && stream.participantId)
      .map((stream) => stream.participantId as string);

    return [...new Set(recordingIds && recordingIds.length > 0 ? recordingIds : this.output?.isoParticipantIds ?? [])].sort();
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

function enrichSourceSnapshotWithZoomIssues(
  snapshot: MediaCoreFrameSourceSnapshot,
  frames: MediaCoreFrame[],
  sources: MediaCoreZoomSource[]
): MediaCoreFrameSourceSnapshot {
  const issues = buildZoomSourceHealthIssues(frames, sources);
  if (issues.length === 0) {
    return {
      ...snapshot,
      issues: snapshot.issues ?? []
    };
  }

  const warnings = [...new Set([...snapshot.warnings, ...issues.map((issue) => issue.detail)])];

  return {
    ...snapshot,
    status: snapshot.status === "failed" ? "failed" : "degraded",
    issues,
    warnings
  };
}

function applyIsoRecordingReadiness(recording: MediaCoreStateSnapshot["recording"], issues: IsoRecordingReadinessIssue[]) {
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

function buildIsoRecordingReadinessIssues(isoParticipantIds: string[], sources: MediaCoreZoomSource[]): IsoRecordingReadinessIssue[] {
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

function buildZoomSourceHealthIssues(frames: MediaCoreFrame[], sources: MediaCoreZoomSource[]): MediaCoreSourceHealthIssue[] {
  const renderedParticipantIds = new Set(frames.map((frame) => frame.participantId).filter(Boolean) as string[]);
  const renderedSourceIds = new Set(frames.map((frame) => frame.sourceId));

  return sources
    .filter((source) => renderedParticipantIds.has(source.participantId) || renderedSourceIds.has(source.sourceId))
    .map(sourceHealthIssue)
    .filter((issue): issue is MediaCoreSourceHealthIssue => Boolean(issue));
}

function sourceHealthIssue(source: MediaCoreZoomSource): MediaCoreSourceHealthIssue | undefined {
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

function sourceIssueTitle(issue: MediaCoreSourceHealthIssue) {
  if (issue.health === "recovering") {
    return "Zoom feed recovering";
  }

  if (issue.health === "video-off") {
    return "Zoom feed unavailable";
  }

  return "Zoom feed quality warning";
}

function sourceIssueKey(issue: Pick<MediaCoreSourceHealthIssue, "participantId" | "sourceId">) {
  return issue.participantId ?? issue.sourceId;
}

function sourceIssueRecoveryDetail(issue: MediaCoreSourceHealthIssue) {
  const label = issue.displayName ?? issue.participantId ?? issue.sourceId;
  return `${label} feed recovered.`;
}

function zoomSourceLifecycleState(source: MediaCoreZoomSource): ZoomSourceLifecycleState {
  return {
    participantId: source.participantId,
    displayName: source.displayName,
    breakoutRoomName: source.breakoutRoomName,
    hasVideo: source.hasVideo,
    isMuted: source.isMuted,
    isActiveSpeaker: source.isActiveSpeaker,
    isScreenSharing: source.isScreenSharing,
    health: source.health
  };
}

function replaceMap<TKey, TValue>(target: Map<TKey, TValue>, source: Map<TKey, TValue>) {
  target.clear();
  source.forEach((value, key) => target.set(key, value));
}

function zoomSourceLifecycleEvents(previous: ZoomSourceLifecycleState, current: ZoomSourceLifecycleState) {
  const events: Array<Pick<MediaCoreEvent, "severity" | "title" | "detail">> = [];

  if (previous.hasVideo && (!current.hasVideo || current.health === "video-off")) {
    events.push({
      severity: "critical",
      title: "Zoom participant video off",
      detail: `${current.displayName} turned video off.`
    });
  } else if ((!previous.hasVideo || previous.health === "video-off") && current.hasVideo && current.health !== "video-off") {
    events.push({
      severity: "info",
      title: "Zoom participant video restored",
      detail: `${current.displayName} video is available.`
    });
  }

  if (!previous.isMuted && current.isMuted) {
    events.push({
      severity: "info",
      title: "Zoom participant muted",
      detail: `${current.displayName} muted their microphone.`
    });
  } else if (previous.isMuted && !current.isMuted) {
    events.push({
      severity: "info",
      title: "Zoom participant unmuted",
      detail: `${current.displayName} unmuted their microphone.`
    });
  }

  if (!previous.isScreenSharing && current.isScreenSharing) {
    events.push({
      severity: "info",
      title: "Zoom screen share started",
      detail: `${current.displayName} started screen sharing.`
    });
  } else if (previous.isScreenSharing && !current.isScreenSharing) {
    events.push({
      severity: "info",
      title: "Zoom screen share stopped",
      detail: `${current.displayName} stopped screen sharing.`
    });
  }

  if (!previous.isActiveSpeaker && current.isActiveSpeaker) {
    events.push({
      severity: "info",
      title: "Zoom active speaker changed",
      detail: `${current.displayName} is now the active speaker.`
    });
  }

  return events;
}

function recordingStreamKey(stream: Pick<MediaCoreRecordingStream, "kind" | "participantId">) {
  return stream.kind === "program" ? "program" : `iso:${stream.participantId ?? "unknown"}`;
}

function recordingStreamWarningTitle(stream: Pick<MediaCoreRecordingStream, "kind">) {
  return stream.kind === "program" ? "Program recording warning" : "ISO recording warning";
}

function recordingStreamRecoveryDetail(key: string, warning: string) {
  const label = key.startsWith("iso:") ? `${key.slice(4)} ISO` : "Program recording";
  return `${label} recovered after warning: ${warning}`;
}

function outputSenderTransitionEvent(
  sender: MediaCoreOutputSender,
  previousStatus: MediaCoreOutputSender["status"] | undefined,
  outputProfile: MediaCoreOutputProfile
): Pick<MediaCoreEvent, "severity" | "title" | "detail"> {
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

function formatOutputProfileLabel(outputProfile: MediaCoreOutputProfile) {
  return `${outputProfile.resolution} ${outputProfile.fps}fps`;
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
