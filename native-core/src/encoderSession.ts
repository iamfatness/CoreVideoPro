import type {
  MediaCoreDestination,
  MediaCoreEncoderLifecycle,
  MediaCoreEncoderSession,
  MediaCoreEncoderTarget,
  MediaCoreProgramFrame,
  MediaCoreRecordingSession
} from "./protocol.js";

export function buildEncoderSession(input: {
  outputs: MediaCoreDestination[];
  programFrame?: MediaCoreProgramFrame;
  recording?: MediaCoreRecordingSession;
  lifecycle: MediaCoreEncoderLifecycle;
}): MediaCoreEncoderSession {
  const targets: MediaCoreEncoderTarget[] = [];

  input.outputs.forEach((destination) => {
    targets.push({
      targetId: `${destination}:program`,
      destination,
      streamKind: "program",
      status: targetStatus(input.programFrame),
      attachedFrameCount: input.programFrame && input.programFrame.health !== "dropped" ? 1 : 0,
      warning: targetWarning(destination, input.programFrame)
    });
  });

  if (input.recording?.active) {
    input.recording.streams
      .filter((stream) => stream.kind === "iso")
      .forEach((stream) => {
        targets.push({
          targetId: `recording:iso:${stream.participantId}`,
          destination: "recording",
          streamKind: "iso",
          participantId: stream.participantId,
          status: stream.status === "failed" ? "failed" : stream.status === "warning" ? "warning" : "attached",
          attachedFrameCount: stream.framesWritten,
          warning: stream.status === "warning" ? stream.warning ?? input.recording?.warning : undefined
        });
      });
  }

  const warnings = targets.map((target) => target.warning).filter(Boolean) as string[];
  const hasFailure = targets.some((target) => target.status === "failed");
  const hasWarning = targets.some((target) => target.status === "warning");

  return {
    status:
      input.lifecycle.status === "failed"
        ? "failed"
        : hasFailure
          ? "failed"
          : hasWarning
            ? "warning"
            : input.lifecycle.status === "encoding" && targets.length > 0
              ? "encoding"
              : "idle",
    renderPlanId: input.programFrame?.renderPlanId,
    programFrameCount: input.programFrame && input.programFrame.health !== "dropped" ? 1 : 0,
    targets,
    lifecycle: input.lifecycle,
    warnings: [...new Set(warnings)]
  };
}

function targetStatus(programFrame?: MediaCoreProgramFrame): MediaCoreEncoderTarget["status"] {
  if (!programFrame) {
    return "idle";
  }

  if (programFrame.health === "dropped") {
    return "warning";
  }

  return programFrame.health === "degraded" ? "warning" : "attached";
}

function targetWarning(destination: MediaCoreDestination, programFrame?: MediaCoreProgramFrame) {
  if (!programFrame) {
    return `${destination} has no program frame to encode.`;
  }

  if (programFrame.health === "dropped") {
    return `${destination} skipped a dropped program frame.`;
  }

  if (programFrame.health === "degraded") {
    return `${destination} is encoding a degraded program frame.`;
  }

  return undefined;
}
