import type { NativeMediaCoreRecordingSession } from "./nativeMediaCoreProtocol";
import { validateOutputLifecycle, type OutputLifecycle } from "./generated/lifecycle";

export type RecordingObservedStatus = "idle" | "recording" | "warning" | "stopped" | "failed" |
  "starting" | "stopping" | "finalizing" | "completed" | "interrupted";

/** Render writer evidence separately from the local Record command's intent. */
export function recordingReadModel(
  recording?: Pick<NativeMediaCoreRecordingSession, "lifecycle" | "active" | "status" | "totalFramesWritten">,
  requested = false
) {
  const lifecycle = recording?.lifecycle;
  if (lifecycle !== undefined) {
    if (!validateOutputLifecycle(lifecycle)) {
      return { active: false, status: "failed" as const, label: "Recording status unavailable", finalized: false };
    }
    const status: RecordingObservedStatus = lifecycle.state === "live"
      ? (lifecycle.health === "failed" ? "failed" : lifecycle.health === "unknown" ? "starting"
        : lifecycle.health === "degraded" ? "warning" : "recording")
      : lifecycle.state;
    return {
      active: lifecycle.state === "live" && (lifecycle.health === "healthy" || lifecycle.health === "degraded"),
      status,
      label: recordingLabel(status),
      finalized: lifecycle.state === "completed" && lifecycle.finalized,
      lifecycle: lifecycle as OutputLifecycle
    };
  }
  // Compatibility with simulated/older snapshots: an active bit alone cannot
  // prove that the writer has produced media. Missing snapshots never do.
  const active = Boolean(recording?.active && recording.totalFramesWritten > 0 &&
    (recording.status === "recording" || recording.status === "warning"));
  const status: RecordingObservedStatus = recording?.status === "failed" ? "failed"
    : active ? recording!.status
    : recording?.active || requested ? "starting"
    : recording?.status === "stopped" ? "stopped" : "idle";
  return { active, status, label: recordingLabel(status), finalized: false };
}

function recordingLabel(status: RecordingObservedStatus): string {
  switch (status) {
    case "idle": return "Record";
    case "recording": return "Recording";
    case "warning": return "Recording warning";
    case "starting": return "Starting recording";
    case "stopping": return "Stopping recording";
    case "finalizing": return "Finalizing recording";
    case "completed": return "Recording completed";
    case "interrupted": return "Recording interrupted";
    case "failed": return "Recording failed";
    case "stopped": return "Record";
  }
}
