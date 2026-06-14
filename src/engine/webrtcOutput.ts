export type WebRtcSignalingProtocol = "whip" | "whep" | "custom";

export type WebRtcIceServer = {
  urls: string;
  username?: string;
  credential?: string;
};

export type WebRtcConnectionParams = {
  signalingUrl: string;
  signalingProtocol: WebRtcSignalingProtocol;
  streamKey: string;
  iceServers: WebRtcIceServer[];
  /** Expected glass-to-glass latency in milliseconds */
  estimatedLatencyMs: number;
};

export type WebRtcValidation = {
  valid: boolean;
  params: WebRtcConnectionParams | null;
  errors: string[];
  warnings: string[];
};

export type WebRtcMonitorStatus = {
  latencyClass: "ultra-low" | "low" | "standard";
  /** Human-readable latency label */
  latencyLabel: string;
  maxViewers: number;
  supportsRecording: boolean;
  summary: string;
};

const DEFAULT_ICE_SERVERS: WebRtcIceServer[] = [
  { urls: "stun:stun.l.google.com:19302" },
  { urls: "stun:stun1.l.google.com:19302" }
];

/**
 * Detect the signaling protocol from the endpoint URL.
 * WHIP (WebRTC-HTTP Ingestion Protocol) is used for publishing;
 * WHEP (WebRTC-HTTP Egress Protocol) is used for playback/monitoring.
 */
export function detectSignalingProtocol(url: string): WebRtcSignalingProtocol {
  const lower = url.toLowerCase();
  if (lower.includes("/whip") || lower.includes("whip")) return "whip";
  if (lower.includes("/whep") || lower.includes("whep")) return "whep";
  return "custom";
}

/**
 * Validate a WebRTC output destination and resolve connection parameters.
 * Accepts https:// or wss:// endpoints for signaling.
 */
export function parseWebRtcEndpoint(endpoint: string, streamKey: string): WebRtcValidation {
  const errors: string[] = [];
  const warnings: string[] = [];

  if (!endpoint) {
    return { valid: false, params: null, errors: ["WebRTC endpoint is required."], warnings };
  }

  const isHttps = endpoint.startsWith("https://");
  const isWss = endpoint.startsWith("wss://");
  if (!isHttps && !isWss) {
    return {
      valid: false,
      params: null,
      errors: ["WebRTC endpoint must use https:// or wss://."],
      warnings
    };
  }

  if (!streamKey || streamKey.trim().length === 0) {
    errors.push("WebRTC stream key is required.");
  }

  if (streamKey.trim().length > 0 && streamKey.trim().length < 8) {
    warnings.push("Short stream keys are easier to guess — consider using a longer token.");
  }

  const signalingProtocol = detectSignalingProtocol(endpoint);
  if (signalingProtocol === "custom") {
    warnings.push("Unknown signaling protocol — ensure your server supports WHIP or WebSocket signaling.");
  }

  if (isWss) {
    warnings.push("WebSocket signaling (wss://) detected — WHIP over https:// is preferred for broader CDN compatibility.");
  }

  if (errors.length > 0) {
    return { valid: false, params: null, errors, warnings };
  }

  return {
    valid: true,
    params: {
      signalingUrl: endpoint,
      signalingProtocol,
      streamKey: streamKey.trim(),
      iceServers: DEFAULT_ICE_SERVERS,
      estimatedLatencyMs: 200
    },
    errors,
    warnings
  };
}

/**
 * Classify WebRTC output latency and describe monitoring capabilities.
 * WebRTC targets sub-500 ms glass-to-glass for program monitoring.
 */
export function describeWebRtcMonitor(params: WebRtcConnectionParams): WebRtcMonitorStatus {
  const { estimatedLatencyMs, signalingProtocol } = params;

  let latencyClass: WebRtcMonitorStatus["latencyClass"];
  let latencyLabel: string;
  if (estimatedLatencyMs <= 200) {
    latencyClass = "ultra-low";
    latencyLabel = `~${estimatedLatencyMs} ms (ultra-low)`;
  } else if (estimatedLatencyMs <= 500) {
    latencyClass = "low";
    latencyLabel = `~${estimatedLatencyMs} ms (low)`;
  } else {
    latencyClass = "standard";
    latencyLabel = `~${estimatedLatencyMs} ms (standard)`;
  }

  const maxViewers = signalingProtocol === "whep" ? 1000 : 50;
  const supportsRecording = false;

  return {
    latencyClass,
    latencyLabel,
    maxViewers,
    supportsRecording,
    summary: `WebRTC ${signalingProtocol.toUpperCase()} — ${latencyLabel}, up to ${maxViewers} viewers`
  };
}

export function defaultIceServers(): WebRtcIceServer[] {
  return [...DEFAULT_ICE_SERVERS];
}

export function formatIceServer(server: WebRtcIceServer): string {
  return server.username ? `${server.urls} (authenticated)` : server.urls;
}
