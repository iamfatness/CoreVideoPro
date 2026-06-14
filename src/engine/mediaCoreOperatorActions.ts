import type {
  NativeMediaCoreEncoderSession,
  NativeMediaCoreFrameSourceSnapshot,
  NativeMediaCoreOperatorAction,
  NativeMediaCoreOutputSenderSession,
  NativeMediaCoreProgramFrame,
  NativeMediaCoreProgramFrameTransport,
  NativeMediaCoreRecordingSession,
  NativeMediaCoreRenderPlan
} from "./nativeMediaCoreProtocol";

export type NativeMediaCoreOperatorActionInput = {
  sourceSnapshot: NativeMediaCoreFrameSourceSnapshot;
  renderPlan: NativeMediaCoreRenderPlan;
  programFrame?: NativeMediaCoreProgramFrame;
  programTransport: NativeMediaCoreProgramFrameTransport;
  encoderSession: NativeMediaCoreEncoderSession;
  outputSenderSession: NativeMediaCoreOutputSenderSession;
  recording?: NativeMediaCoreRecordingSession;
};

const severityRank: Record<NativeMediaCoreOperatorAction["severity"], number> = {
  critical: 0,
  warning: 1,
  info: 2
};

export function buildNativeMediaCoreOperatorActions(input: NativeMediaCoreOperatorActionInput): NativeMediaCoreOperatorAction[] {
  const actions: NativeMediaCoreOperatorAction[] = [];

  input.outputSenderSession.senders.forEach((sender) => {
    if (sender.status === "failed") {
      actions.push({
        actionId: `sender:${sender.destination}:recover`,
        severity: "critical",
        area: "sender",
        title: `Recover ${sender.destination.toUpperCase()} sender`,
        detail: sender.warning ?? `${sender.destination.toUpperCase()} sender failed.`,
        command: `recover-output-sender:${sender.destination}`,
        relatedId: sender.senderId
      });
      return;
    }

    if (sender.status === "warning" || sender.status === "starting") {
      actions.push({
        actionId: `sender:${sender.destination}:check`,
        severity: "warning",
        area: "sender",
        title: `Check ${sender.destination.toUpperCase()} sender`,
        detail: sender.warning ?? `${sender.destination.toUpperCase()} sender is ${sender.status}.`,
        relatedId: sender.senderId
      });
    }
  });

  if (input.recording?.status === "failed" || input.recording?.writerStatus === "failed") {
    actions.push({
      actionId: "recording:recover",
      severity: "critical",
      area: "recording",
      title: "Recover recording writer",
      detail: input.recording.error ?? "Recording writer failed.",
      command: "recover-recording-session",
      relatedId: input.recording.sessionId
    });
  } else if (input.recording?.status === "warning" || input.recording?.writerStatus === "warning") {
    const streamWarnings = input.recording.streams.filter((stream) => stream.warning);
    if (streamWarnings.length > 0) {
      streamWarnings.forEach((stream) => {
        const label = stream.kind === "program" ? "program" : `${stream.participantId ?? "unknown"} ISO`;
        const streamId = stream.kind === "program" ? "program" : `iso:${stream.participantId ?? "unknown"}`;
        actions.push({
          actionId: `recording:${streamId}:check`,
          severity: "warning",
          area: "recording",
          title: `Check ${label} recording`,
          detail: stream.warning ?? "Recording stream is warning.",
          relatedId: stream.participantId ?? input.recording?.sessionId
        });
      });
    } else {
      actions.push({
        actionId: "recording:check",
        severity: "warning",
        area: "recording",
        title: "Check recording writer",
        detail: input.recording.warning ?? "Recording writer is warning.",
        relatedId: input.recording.sessionId
      });
    }
  }

  const sourceIssues = input.sourceSnapshot.issues ?? [];

  if (input.sourceSnapshot.status === "failed") {
    actions.push({
      actionId: "source:recover",
      severity: "critical",
      area: "source",
      title: "Recover media source",
      detail: input.sourceSnapshot.warnings[0] ?? "Media source failed.",
      relatedId: input.sourceSnapshot.adapterId
    });
  } else if (sourceIssues.length > 0) {
    sourceIssues.forEach((issue) => {
      const label = issue.displayName ?? issue.participantId ?? issue.sourceId;
      actions.push({
        actionId: `source:${issue.participantId ?? issue.sourceId}:check`,
        severity: issue.severity,
        area: "source",
        title: `Check ${label} feed`,
        detail: issue.detail,
        relatedId: issue.participantId ?? issue.sourceId
      });
    });
  } else if (input.sourceSnapshot.status === "degraded" || input.sourceSnapshot.droppedFrameCount > 0 || input.sourceSnapshot.lowResolutionFrameCount > 0) {
    actions.push({
      actionId: "source:check",
      severity: "warning",
      area: "source",
      title: "Check media source quality",
      detail: input.sourceSnapshot.warnings[0] ?? "Media source is degraded.",
      relatedId: input.sourceSnapshot.adapterId
    });
  }

  input.renderPlan.routes
    .filter((route) => route.status === "missing")
    .forEach((route) => {
      actions.push({
        actionId: `routing:${route.routeId}:resolve`,
        severity: "warning",
        area: "routing",
        title: `Resolve missing route ${route.routeId}`,
        detail: route.warning ?? "A route has no clean source.",
        relatedId: route.routeId
      });
    });

  const programFrame = input.programFrame;

  if (programFrame?.health === "dropped") {
    actions.push({
      actionId: "program:frame-dropped",
      severity: "critical",
      area: "program",
      title: "Recover program frame output",
      detail: programFrame.warning ?? input.programTransport.warning ?? "Program output dropped a frame.",
      relatedId: programFrame.renderPlanId
    });
  } else if (programFrame?.health === "degraded" || input.programTransport.status === "degraded") {
    actions.push({
      actionId: "program:frame-degraded",
      severity: "warning",
      area: "program",
      title: "Review program frame quality",
      detail: programFrame?.warning ?? input.programTransport.warning ?? "Program output is degraded.",
      relatedId: programFrame?.renderPlanId
    });
  }

  if (input.encoderSession.status === "failed" || input.encoderSession.lifecycle.status === "failed") {
    actions.push({
      actionId: "encoder:failed",
      severity: "critical",
      area: "encoder",
      title: "Recover encoder session",
      detail: input.encoderSession.warnings[0] ?? input.encoderSession.lifecycle.lastTransition,
      relatedId: input.encoderSession.renderPlanId
    });
  } else if (input.encoderSession.status === "warning" || (input.encoderSession.lifecycle.status === "stopped" && input.encoderSession.targets.length > 0)) {
    actions.push({
      actionId: "encoder:check",
      severity: "warning",
      area: "encoder",
      title: "Check encoder session",
      detail: input.encoderSession.warnings[0] ?? input.encoderSession.lifecycle.lastTransition,
      relatedId: input.encoderSession.renderPlanId
    });
  }

  return [...new Map(actions.map((action) => [action.actionId, action])).values()].sort(
    (left, right) => severityRank[left.severity] - severityRank[right.severity] || left.actionId.localeCompare(right.actionId)
  );
}
