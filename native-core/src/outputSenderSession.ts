import type {
  MediaCoreDestination,
  MediaCoreOutputProfile,
  MediaCoreOutputSender,
  MediaCoreOutputSenderSession,
  MediaCoreProgramFrame
} from "./protocol.js";

type NetworkDestination = Exclude<MediaCoreDestination, "recording">;

const NETWORK_DESTINATIONS = new Set<MediaCoreDestination>(["rtmp", "ndi", "srt", "webrtc"]);
const DEFAULT_FPS = 30;

type SenderResultCode = "ok" | "waiting-for-frame" | "degraded-frame" | "dropped-frame" | "failed" | "recovered" | "stopped";
type DestinationHealth = "ok" | "starting" | "warning" | "failed" | "stopped";

type DiagnosticOutputSender = MediaCoreOutputSender & {
  destinationHealth: DestinationHealth;
  lastResultCode: SenderResultCode;
  lastError?: string;
  bytesSent: number;
};

export class OutputSenderSessionModel {
  private readonly senders = new Map<NetworkDestination, DiagnosticOutputSender>();

  fail(destination: NetworkDestination, message: string, elapsedMs: number) {
    const existing = this.senders.get(destination);
    this.senders.set(destination, {
      senderId: existing?.senderId ?? `${destination}:program`,
      destination,
      status: "failed",
      startedAtMs: existing?.startedAtMs,
      stoppedAtMs: elapsedMs,
      lastFrameNumber: existing?.lastFrameNumber,
      framesSent: existing?.framesSent ?? 0,
      retryCount: (existing?.retryCount ?? 0) + 1,
      latencyMs: existing?.latencyMs ?? latencyFor(destination),
      bitrateMbps: existing?.bitrateMbps ?? 0,
      warning: message,
      destinationHealth: "failed",
      lastResultCode: "failed",
      lastError: message,
      bytesSent: existing?.bytesSent ?? 0
    });
    return this.snapshot();
  }

  recover(destination: NetworkDestination, elapsedMs: number, reason = `${destination.toUpperCase()} sender recovered.`) {
    const existing = this.senders.get(destination);
    this.senders.set(destination, {
      senderId: existing?.senderId ?? `${destination}:program`,
      destination,
      status: "starting",
      startedAtMs: elapsedMs,
      stoppedAtMs: undefined,
      lastFrameNumber: existing?.lastFrameNumber,
      framesSent: existing?.framesSent ?? 0,
      retryCount: existing?.retryCount ?? 0,
      latencyMs: existing?.latencyMs ?? latencyFor(destination),
      bitrateMbps: existing?.bitrateMbps ?? 0,
      warning: reason,
      destinationHealth: "starting",
      lastResultCode: "recovered",
      lastError: existing?.lastError,
      bytesSent: existing?.bytesSent ?? 0
    });
    return this.snapshot();
  }

  sync(destinations: MediaCoreDestination[], programFrame: MediaCoreProgramFrame | undefined, outputProfile: MediaCoreOutputProfile, elapsedMs: number) {
    const activeDestinations = destinations.filter((destination): destination is NetworkDestination => NETWORK_DESTINATIONS.has(destination));
    const activeSet = new Set(activeDestinations);

    [...this.senders.entries()].forEach(([destination, sender]) => {
      if (!activeSet.has(destination) && sender.status !== "stopped") {
        this.senders.set(destination, {
          ...sender,
          status: "stopped",
          stoppedAtMs: elapsedMs,
          warning: undefined,
          destinationHealth: "stopped",
          lastResultCode: "stopped"
        });
      }
    });

    activeDestinations.forEach((destination) => {
      const existing = this.senders.get(destination);
      this.senders.set(destination, this.nextSender(destination, existing, programFrame, outputProfile, elapsedMs));
    });

    return this.snapshot();
  }

  snapshot(): MediaCoreOutputSenderSession {
    const senders = [...this.senders.values()].sort((left, right) => left.destination.localeCompare(right.destination));
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

  private nextSender(
    destination: NetworkDestination,
    existing: DiagnosticOutputSender | undefined,
    programFrame: MediaCoreProgramFrame | undefined,
    outputProfile: MediaCoreOutputProfile,
    elapsedMs: number
  ): DiagnosticOutputSender {
    const startedAtMs = existing?.startedAtMs ?? elapsedMs;
    const senderId = existing?.senderId ?? `${destination}:program`;
    const bitrateMbps = bitrateFor(destination, outputProfile);
    const base = {
      senderId,
      destination,
      startedAtMs,
      stoppedAtMs: undefined,
      latencyMs: latencyFor(destination),
      bitrateMbps,
      retryCount: existing?.retryCount ?? 0,
      framesSent: existing?.framesSent ?? 0,
      bytesSent: existing?.bytesSent ?? 0
    };

    if (existing?.status === "failed") {
      return {
        ...base,
        status: "failed",
        stoppedAtMs: existing.stoppedAtMs,
        lastFrameNumber: existing.lastFrameNumber,
        retryCount: existing.retryCount,
        warning: existing.warning,
        destinationHealth: "failed",
        lastResultCode: existing.lastResultCode,
        lastError: existing.lastError
      };
    }

    if (!programFrame) {
      return {
        ...base,
        status: "starting",
        warning: `${destination.toUpperCase()} sender is waiting for a program frame.`,
        destinationHealth: "starting",
        lastResultCode: "waiting-for-frame",
        lastError: existing?.lastError
      };
    }

    if (programFrame.health === "dropped") {
      return {
        ...base,
        status: "warning",
        lastFrameNumber: existing?.lastFrameNumber,
        retryCount: (existing?.retryCount ?? 0) + 1,
        warning: `${destination.toUpperCase()} sender skipped a dropped program frame.`,
        destinationHealth: "warning",
        lastResultCode: "dropped-frame",
        lastError: `${destination.toUpperCase()} sender skipped a dropped program frame.`
      };
    }

    const framesSent = (existing?.framesSent ?? 0) + 1;
    return {
      ...base,
      status: programFrame.health === "degraded" ? "warning" : "live",
      lastFrameNumber: programFrame.frameNumber,
      framesSent,
      retryCount: existing?.retryCount ?? 0,
      warning: programFrame.health === "degraded" ? `${destination.toUpperCase()} sender is publishing degraded program frames.` : undefined,
      destinationHealth: programFrame.health === "degraded" ? "warning" : "ok",
      lastResultCode: programFrame.health === "degraded" ? "degraded-frame" : "ok",
      lastError: programFrame.health === "degraded" ? `${destination.toUpperCase()} sender is publishing degraded program frames.` : existing?.lastError,
      bytesSent: (existing?.bytesSent ?? 0) + estimatedFrameBytes(bitrateMbps, outputProfile.fps)
    };
  }
}

function latencyFor(destination: NetworkDestination) {
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

function bitrateFor(destination: NetworkDestination, outputProfile: MediaCoreOutputProfile) {
  const multiplier = destination === "ndi" ? 1.6 : destination === "webrtc" ? 0.8 : 1;
  return Number((outputProfile.targetBitrateMbps * multiplier).toFixed(1));
}

function estimatedFrameBytes(bitrateMbps: number, fps: number) {
  const frameRate = fps > 0 ? fps : DEFAULT_FPS;
  return Math.round((bitrateMbps * 1_000_000) / 8 / frameRate);
}
