/**
 * Output latency budget engine.
 * Models the end-to-end production pipeline latency from Zoom capture
 * through compositing, encoding, and delivery protocol to the viewer.
 * Helps operators understand and minimise glass-to-glass delay.
 */

export type LatencyStage =
  | "zoom-capture"
  | "compositor"
  | "encoder"
  | "protocol"
  | "cdn-buffer"
  | "player-buffer";

export type ProtocolLatencyProfile =
  | "rtmp-hls"     // 10–30 s
  | "rtmp-dash"    // 6–15 s
  | "srt"          // 0.12–2 s
  | "webrtc-whip"  // 0.05–0.3 s
  | "ndi"          // <0.005 s
  | "recording";   // no delivery latency

export type LatencyStageBudget = {
  stage: LatencyStage;
  label: string;
  minMs: number;
  maxMs: number;
  /** Best estimate for the current settings */
  estimatedMs: number;
};

export type LatencyBudget = {
  stages: LatencyStageBudget[];
  totalMinMs: number;
  totalMaxMs: number;
  totalEstimatedMs: number;
  /** Human-readable category */
  latencyClass: "ultra-low" | "low" | "standard" | "broadcast";
  summary: string;
};

export type LatencyBudgetOptions = {
  protocol: ProtocolLatencyProfile;
  /** SRT latency setting in ms (only applies to "srt" protocol) */
  srtLatencyMs?: number;
  /** WebRTC estimated RTT in ms */
  webrtcRttMs?: number;
  /** Encoder frame buffer in frames at the output frame rate */
  encoderBufferFrames?: number;
  outputFps?: number;
};

const ZOOM_CAPTURE_MS = { min: 100, max: 300, est: 180 };
const COMPOSITOR_MS = { min: 16, max: 50, est: 33 };

function encoderMs(bufferFrames: number, fps: number) {
  const estFrameMs = 1000 / fps;
  const estMs = bufferFrames * estFrameMs;
  return { min: estFrameMs, max: estMs * 2, est: estMs };
}

const PROTOCOL_RANGES: Record<ProtocolLatencyProfile, { min: number; max: number; est: number }> = {
  "rtmp-hls":    { min: 10000, max: 30000, est: 20000 },
  "rtmp-dash":   { min: 6000,  max: 15000, est: 10000 },
  srt:           { min: 120,   max: 2000,  est: 500 },
  "webrtc-whip": { min: 50,    max: 300,   est: 150 },
  ndi:           { min: 1,     max: 5,     est: 3 },
  recording:     { min: 0,     max: 0,     est: 0 },
};

const CDN_BUFFER: Record<ProtocolLatencyProfile, { min: number; max: number; est: number }> = {
  "rtmp-hls":    { min: 2000, max: 6000, est: 4000 },
  "rtmp-dash":   { min: 1000, max: 3000, est: 2000 },
  srt:           { min: 0,    max: 200,  est: 50 },
  "webrtc-whip": { min: 0,    max: 50,   est: 20 },
  ndi:           { min: 0,    max: 0,    est: 0 },
  recording:     { min: 0,    max: 0,    est: 0 },
};

const PLAYER_BUFFER: Record<ProtocolLatencyProfile, { min: number; max: number; est: number }> = {
  "rtmp-hls":    { min: 3000, max: 10000, est: 6000 },
  "rtmp-dash":   { min: 2000, max: 6000,  est: 3000 },
  srt:           { min: 50,   max: 500,   est: 100 },
  "webrtc-whip": { min: 20,   max: 200,   est: 50 },
  ndi:           { min: 0,    max: 0,     est: 0 },
  recording:     { min: 0,    max: 0,     est: 0 },
};

function stageLabel(stage: LatencyStage): string {
  switch (stage) {
    case "zoom-capture":  return "Zoom capture";
    case "compositor":    return "Compositor";
    case "encoder":       return "Encoder buffer";
    case "protocol":      return "Protocol / network";
    case "cdn-buffer":    return "CDN buffer";
    case "player-buffer": return "Player buffer";
  }
}

export function computeLatencyBudget(options: LatencyBudgetOptions): LatencyBudget {
  const {
    protocol,
    srtLatencyMs,
    webrtcRttMs,
    encoderBufferFrames = 2,
    outputFps = 30,
  } = options;

  // Override protocol range for SRT and WebRTC when specific values are given
  let protocolRange = { ...PROTOCOL_RANGES[protocol] };
  if (protocol === "srt" && srtLatencyMs !== undefined) {
    protocolRange = { min: srtLatencyMs * 0.8, max: srtLatencyMs * 1.2, est: srtLatencyMs };
  }
  if (protocol === "webrtc-whip" && webrtcRttMs !== undefined) {
    const halfRtt = webrtcRttMs / 2;
    protocolRange = { min: halfRtt * 0.8, max: halfRtt * 1.5, est: halfRtt };
  }

  const enc = encoderMs(encoderBufferFrames, outputFps);

  const rawStages: Array<{ stage: LatencyStage; range: { min: number; max: number; est: number } }> = [
    { stage: "zoom-capture",  range: ZOOM_CAPTURE_MS },
    { stage: "compositor",    range: COMPOSITOR_MS },
    { stage: "encoder",       range: enc },
    { stage: "protocol",      range: protocolRange },
    { stage: "cdn-buffer",    range: CDN_BUFFER[protocol] },
    { stage: "player-buffer", range: PLAYER_BUFFER[protocol] },
  ];

  const stages: LatencyStageBudget[] = rawStages.map(({ stage, range }) => ({
    stage,
    label: stageLabel(stage),
    minMs: Math.round(range.min),
    maxMs: Math.round(range.max),
    estimatedMs: Math.round(range.est),
  }));

  const totalMinMs = stages.reduce((s, t) => s + t.minMs, 0);
  const totalMaxMs = stages.reduce((s, t) => s + t.maxMs, 0);
  const totalEstimatedMs = stages.reduce((s, t) => s + t.estimatedMs, 0);

  let latencyClass: LatencyBudget["latencyClass"];
  if (totalEstimatedMs < 300) {
    latencyClass = "ultra-low";
  } else if (totalEstimatedMs < 2000) {
    latencyClass = "low";
  } else if (totalEstimatedMs < 10000) {
    latencyClass = "standard";
  } else {
    latencyClass = "broadcast";
  }

  const summary = `${latencyClass.replace("-", " ")} — ~${formatLatencyMs(totalEstimatedMs)} glass-to-glass`;

  return { stages, totalMinMs, totalMaxMs, totalEstimatedMs, latencyClass, summary };
}

export function formatLatencyMs(ms: number): string {
  if (ms < 1000) return `${ms} ms`;
  if (ms < 60000) return `${(ms / 1000).toFixed(1)} s`;
  return `${(ms / 60000).toFixed(1)} min`;
}

export function latencyClassLabel(cls: LatencyBudget["latencyClass"]): string {
  switch (cls) {
    case "ultra-low": return "Ultra-low (<300ms)";
    case "low":       return "Low (<2s)";
    case "standard":  return "Standard (<10s)";
    case "broadcast": return "Broadcast (10s+)";
  }
}
