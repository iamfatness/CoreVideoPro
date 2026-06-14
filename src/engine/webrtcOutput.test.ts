import { describe, expect, it } from "vitest";
import {
  defaultIceServers,
  describeWebRtcMonitor,
  detectSignalingProtocol,
  formatIceServer,
  parseWebRtcEndpoint,
} from "./webrtcOutput";

describe("detectSignalingProtocol", () => {
  it("detects WHIP from URL", () => {
    expect(detectSignalingProtocol("https://live.example.com/whip/stream")).toBe("whip");
    expect(detectSignalingProtocol("https://server.com/api/whip")).toBe("whip");
  });

  it("detects WHEP from URL", () => {
    expect(detectSignalingProtocol("https://cdn.example.com/whep/view")).toBe("whep");
  });

  it("returns custom for unknown signaling", () => {
    expect(detectSignalingProtocol("wss://server.example.com/ws/signal")).toBe("custom");
    expect(detectSignalingProtocol("https://server.example.com/broadcast")).toBe("custom");
  });
});

describe("parseWebRtcEndpoint", () => {
  it("rejects empty endpoint", () => {
    const result = parseWebRtcEndpoint("", "token");
    expect(result.valid).toBe(false);
    expect(result.errors[0]).toMatch(/required/i);
  });

  it("rejects non-https/wss scheme", () => {
    const result = parseWebRtcEndpoint("http://insecure.example.com/whip", "token");
    expect(result.valid).toBe(false);
    expect(result.errors[0]).toMatch(/https/);
  });

  it("rejects missing stream key", () => {
    const result = parseWebRtcEndpoint("https://live.example.com/whip", "");
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes("stream key"))).toBe(true);
  });

  it("warns on short stream key but still valid", () => {
    const result = parseWebRtcEndpoint("https://live.example.com/whip", "abc1234");
    expect(result.valid).toBe(true);
    expect(result.warnings.some((w) => w.includes("Short stream keys"))).toBe(true);
  });

  it("parses a valid WHIP endpoint", () => {
    const result = parseWebRtcEndpoint("https://live.example.com/whip/stream", "my-secure-token-123");
    expect(result.valid).toBe(true);
    expect(result.params?.signalingProtocol).toBe("whip");
    expect(result.params?.signalingUrl).toBe("https://live.example.com/whip/stream");
    expect(result.params?.streamKey).toBe("my-secure-token-123");
    expect(result.params?.iceServers.length).toBeGreaterThan(0);
    expect(result.warnings).toHaveLength(0);
  });

  it("parses a valid WHEP endpoint", () => {
    const result = parseWebRtcEndpoint("https://cdn.example.com/whep/view", "viewer-token-xyz");
    expect(result.valid).toBe(true);
    expect(result.params?.signalingProtocol).toBe("whep");
  });

  it("warns on wss:// endpoint", () => {
    const result = parseWebRtcEndpoint("wss://server.example.com/ws/broadcast", "secure-token-abc");
    expect(result.valid).toBe(true);
    expect(result.warnings.some((w) => w.includes("wss://"))).toBe(true);
  });

  it("warns on custom signaling protocol", () => {
    const result = parseWebRtcEndpoint("https://server.example.com/broadcast", "secure-token-abc");
    expect(result.valid).toBe(true);
    expect(result.warnings.some((w) => w.includes("WHIP"))).toBe(true);
  });

});

describe("describeWebRtcMonitor", () => {
  const baseParams = {
    signalingUrl: "https://live.example.com/whip/stream",
    signalingProtocol: "whip" as const,
    streamKey: "token",
    iceServers: defaultIceServers(),
    estimatedLatencyMs: 200
  };

  it("classifies ultra-low latency at 200 ms", () => {
    const status = describeWebRtcMonitor(baseParams);
    expect(status.latencyClass).toBe("ultra-low");
    expect(status.latencyLabel).toContain("ultra-low");
    expect(status.summary).toContain("WHIP");
  });

  it("classifies low latency at 350 ms", () => {
    const status = describeWebRtcMonitor({ ...baseParams, estimatedLatencyMs: 350 });
    expect(status.latencyClass).toBe("low");
  });

  it("classifies standard latency above 500 ms", () => {
    const status = describeWebRtcMonitor({ ...baseParams, estimatedLatencyMs: 800 });
    expect(status.latencyClass).toBe("standard");
  });

  it("reports higher viewer limit for WHEP", () => {
    const whepStatus = describeWebRtcMonitor({ ...baseParams, signalingProtocol: "whep" });
    const whipStatus = describeWebRtcMonitor(baseParams);
    expect(whepStatus.maxViewers).toBeGreaterThan(whipStatus.maxViewers);
  });

  it("does not support recording", () => {
    const status = describeWebRtcMonitor(baseParams);
    expect(status.supportsRecording).toBe(false);
  });
});

describe("defaultIceServers", () => {
  it("returns at least one STUN server", () => {
    const servers = defaultIceServers();
    expect(servers.length).toBeGreaterThan(0);
    expect(servers[0].urls).toContain("stun:");
  });
});

describe("formatIceServer", () => {
  it("formats unauthenticated server", () => {
    expect(formatIceServer({ urls: "stun:stun.l.google.com:19302" })).toBe("stun:stun.l.google.com:19302");
  });

  it("formats authenticated server", () => {
    expect(formatIceServer({ urls: "turn:turn.example.com:3478", username: "user", credential: "pass" })).toBe(
      "turn:turn.example.com:3478 (authenticated)"
    );
  });
});
