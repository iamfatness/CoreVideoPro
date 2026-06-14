import type { OutputDestination } from "../domain/production";

export type DestinationReadiness = {
  id: string;
  name: string;
  protocol: OutputDestination["protocol"];
  ready: boolean;
  /** Short status word for the arming pill, e.g. "Ready" or "Source name". */
  statusLabel: string;
  /** Operator-facing detail explaining what is still required, when not ready. */
  detail: string;
};

export type ArmingSummary = {
  armedCount: number;
  readyCount: number;
  /** Counts of armed destinations grouped by protocol. */
  protocolCounts: Record<OutputDestination["protocol"], number>;
  destinations: DestinationReadiness[];
  blockers: string[];
  summary: string;
};

const SRT_MIN_PASSPHRASE_LENGTH = 10;

/**
 * Evaluate a single armed destination for protocol-specific readiness. Each
 * output protocol has different requirements: RTMP/WebRTC need a stream key,
 * NDI just needs a non-empty source name, and SRT in caller mode needs a host
 * and a passphrase long enough for AES.
 */
export function evaluateDestinationReadiness(destination: OutputDestination): DestinationReadiness {
  const endpoint = destination.endpoint.trim();
  const streamKey = destination.streamKey?.trim() ?? "";

  const base = {
    id: destination.id,
    name: destination.name,
    protocol: destination.protocol
  };

  switch (destination.protocol) {
    case "NDI": {
      if (!endpoint) {
        return { ...base, ready: false, statusLabel: "Source name", detail: `${destination.name} needs an NDI source name.` };
      }

      return { ...base, ready: true, statusLabel: "Ready", detail: `${destination.name} will publish on the local network as "${endpoint}".` };
    }
    case "SRT": {
      if (!endpoint) {
        return { ...base, ready: false, statusLabel: "Endpoint", detail: `${destination.name} needs an srt:// endpoint.` };
      }

      if (!endpoint.startsWith("srt://")) {
        return { ...base, ready: false, statusLabel: "Endpoint", detail: `${destination.name} endpoint must start with srt://.` };
      }

      const isCaller = endpoint.includes("mode=caller") || !endpoint.includes("mode=");
      if (isCaller && streamKey.length > 0 && streamKey.length < SRT_MIN_PASSPHRASE_LENGTH) {
        return {
          ...base,
          ready: false,
          statusLabel: "Passphrase",
          detail: `${destination.name} passphrase must be at least ${SRT_MIN_PASSPHRASE_LENGTH} characters for AES.`
        };
      }

      return { ...base, ready: true, statusLabel: "Ready", detail: `${destination.name} is armed${streamKey ? " with an encrypted passphrase" : ""}.` };
    }
    case "WebRTC": {
      if (!endpoint) {
        return { ...base, ready: false, statusLabel: "Endpoint", detail: `${destination.name} is missing an https:// or wss:// endpoint.` };
      }

      if (!streamKey) {
        return { ...base, ready: false, statusLabel: "Stream key", detail: `${destination.name} is missing a stream key.` };
      }

      return { ...base, ready: true, statusLabel: "Ready", detail: `${destination.name} is armed for low-latency monitoring.` };
    }
    case "RTMP":
    default: {
      if (!endpoint) {
        return { ...base, ready: false, statusLabel: "Endpoint", detail: `${destination.name} is missing an rtmp:// endpoint.` };
      }

      if (!streamKey) {
        return { ...base, ready: false, statusLabel: "Stream key", detail: `${destination.name} is missing a stream key.` };
      }

      return { ...base, ready: true, statusLabel: "Ready", detail: `${destination.name} is armed.` };
    }
  }
}

function emptyProtocolCounts(): Record<OutputDestination["protocol"], number> {
  return { RTMP: 0, NDI: 0, SRT: 0, WebRTC: 0 };
}

/**
 * Summarize the arming state across every armed destination so the operator
 * gets a single protocol-aware readiness readout: how many destinations are
 * armed, how many are ready, the protocol mix, and the specific blockers that
 * still need attention before going live.
 */
export function summarizeArming(destinations: OutputDestination[]): ArmingSummary {
  const armed = destinations.filter((destination) => destination.enabled);
  const evaluated = armed.map(evaluateDestinationReadiness);
  const readyCount = evaluated.filter((destination) => destination.ready).length;
  const protocolCounts = emptyProtocolCounts();

  armed.forEach((destination) => {
    protocolCounts[destination.protocol] += 1;
  });

  const blockers = evaluated.filter((destination) => !destination.ready).map((destination) => destination.detail);

  return {
    armedCount: armed.length,
    readyCount,
    protocolCounts,
    destinations: evaluated,
    blockers,
    summary: buildSummary(armed.length, readyCount, protocolCounts)
  };
}

function buildSummary(
  armedCount: number,
  readyCount: number,
  protocolCounts: Record<OutputDestination["protocol"], number>
): string {
  if (armedCount === 0) {
    return "No destinations armed";
  }

  const mix = (Object.keys(protocolCounts) as Array<OutputDestination["protocol"]>)
    .filter((protocol) => protocolCounts[protocol] > 0)
    .map((protocol) => `${protocolCounts[protocol]} ${protocol}`)
    .join(", ");

  return `${readyCount}/${armedCount} ready - ${mix}`;
}
