import type {
  MediaCoreDestination,
  MediaCoreOutputProfile,
  MediaCoreOutputSender,
  MediaCoreOutputSenderSession,
  MediaCoreProgramFrame
} from "./protocol.js";

type NetworkDestination = Exclude<MediaCoreDestination, "recording">;

const NETWORK_DESTINATIONS = new Set<MediaCoreDestination>(["rtmp", "ndi", "srt", "webrtc"]);

export class OutputSenderSessionModel {
  private readonly senders = new Map<NetworkDestination, MediaCoreOutputSender>();

  sync(destinations: MediaCoreDestination[], programFrame: MediaCoreProgramFrame | undefined, outputProfile: MediaCoreOutputProfile, elapsedMs: number) {
    const activeDestinations = destinations.filter((destination): destination is NetworkDestination => NETWORK_DESTINATIONS.has(destination));
    const activeSet = new Set(activeDestinations);

    [...this.senders.entries()].forEach(([destination, sender]) => {
      if (!activeSet.has(destination) && sender.status !== "stopped") {
        this.senders.set(destination, {
          ...sender,
          status: "stopped",
          stoppedAtMs: elapsedMs,
          warning: undefined
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
    existing: MediaCoreOutputSender | undefined,
    programFrame: MediaCoreProgramFrame | undefined,
    outputProfile: MediaCoreOutputProfile,
    elapsedMs: number
  ): MediaCoreOutputSender {
    const startedAtMs = existing?.startedAtMs ?? elapsedMs;
    const senderId = existing?.senderId ?? `${destination}:program`;
    const base = {
      senderId,
      destination,
      startedAtMs,
      stoppedAtMs: undefined,
      latencyMs: latencyFor(destination),
      bitrateMbps: bitrateFor(destination, outputProfile),
      retryCount: existing?.retryCount ?? 0,
      framesSent: existing?.framesSent ?? 0
    };

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
      retryCount: existing?.retryCount ?? 0,
      warning: programFrame.health === "degraded" ? `${destination.toUpperCase()} sender is publishing degraded program frames.` : undefined
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
