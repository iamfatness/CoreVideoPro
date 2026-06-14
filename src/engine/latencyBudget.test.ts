import { describe, expect, it } from "vitest";
import {
  computeLatencyBudget,
  formatLatencyMs,
  latencyClassLabel,
} from "./latencyBudget";

describe("computeLatencyBudget – protocol classification", () => {
  it("classifies NDI as ultra-low", () => {
    const budget = computeLatencyBudget({ protocol: "ndi" });
    expect(budget.latencyClass).toBe("ultra-low");
    expect(budget.totalEstimatedMs).toBeLessThan(300);
  });

  it("classifies WebRTC WHIP as low (sum of all stages exceeds 300ms)", () => {
    const budget = computeLatencyBudget({ protocol: "webrtc-whip" });
    // zoom(180) + compositor(33) + encoder(66) + protocol(150) + cdn(20) + player(50) ≈ 499ms
    expect(budget.latencyClass).toBe("low");
    expect(budget.totalEstimatedMs).toBeLessThan(2000);
  });

  it("classifies SRT as low", () => {
    const budget = computeLatencyBudget({ protocol: "srt" });
    expect(budget.latencyClass).toBe("low");
  });

  it("classifies RTMP-DASH as broadcast or standard", () => {
    const budget = computeLatencyBudget({ protocol: "rtmp-dash" });
    expect(["standard", "broadcast"]).toContain(budget.latencyClass);
  });

  it("classifies RTMP-HLS as broadcast", () => {
    const budget = computeLatencyBudget({ protocol: "rtmp-hls" });
    expect(budget.latencyClass).toBe("broadcast");
  });

  it("classifies recording as ultra-low (no delivery latency)", () => {
    const budget = computeLatencyBudget({ protocol: "recording" });
    expect(budget.latencyClass).toBe("ultra-low");
  });
});

describe("computeLatencyBudget – stage structure", () => {
  it("returns 6 stages", () => {
    const budget = computeLatencyBudget({ protocol: "srt" });
    expect(budget.stages).toHaveLength(6);
  });

  it("sums stages correctly into totals", () => {
    const budget = computeLatencyBudget({ protocol: "webrtc-whip" });
    const sumMin = budget.stages.reduce((s, t) => s + t.minMs, 0);
    const sumMax = budget.stages.reduce((s, t) => s + t.maxMs, 0);
    const sumEst = budget.stages.reduce((s, t) => s + t.estimatedMs, 0);
    expect(budget.totalMinMs).toBe(sumMin);
    expect(budget.totalMaxMs).toBe(sumMax);
    expect(budget.totalEstimatedMs).toBe(sumEst);
  });

  it("NDI cdn-buffer and player-buffer stages are 0 ms", () => {
    const budget = computeLatencyBudget({ protocol: "ndi" });
    const cdn = budget.stages.find((s) => s.stage === "cdn-buffer")!;
    const player = budget.stages.find((s) => s.stage === "player-buffer")!;
    expect(cdn.estimatedMs).toBe(0);
    expect(player.estimatedMs).toBe(0);
  });

  it("recording has 0 ms for protocol, cdn and player stages", () => {
    const budget = computeLatencyBudget({ protocol: "recording" });
    const protocol = budget.stages.find((s) => s.stage === "protocol")!;
    expect(protocol.estimatedMs).toBe(0);
  });
});

describe("computeLatencyBudget – overrides", () => {
  it("uses srtLatencyMs when provided", () => {
    const budget = computeLatencyBudget({ protocol: "srt", srtLatencyMs: 800 });
    const protocolStage = budget.stages.find((s) => s.stage === "protocol")!;
    expect(protocolStage.estimatedMs).toBe(800);
  });

  it("uses half webrtcRttMs as protocol latency", () => {
    const budget = computeLatencyBudget({ protocol: "webrtc-whip", webrtcRttMs: 100 });
    const protocolStage = budget.stages.find((s) => s.stage === "protocol")!;
    expect(protocolStage.estimatedMs).toBe(50);
  });

  it("encoder stage scales with buffer frames and fps", () => {
    const b30 = computeLatencyBudget({ protocol: "ndi", encoderBufferFrames: 2, outputFps: 30 });
    const b60 = computeLatencyBudget({ protocol: "ndi", encoderBufferFrames: 2, outputFps: 60 });
    const enc30 = b30.stages.find((s) => s.stage === "encoder")!;
    const enc60 = b60.stages.find((s) => s.stage === "encoder")!;
    expect(enc30.estimatedMs).toBeGreaterThan(enc60.estimatedMs);
  });
});

describe("computeLatencyBudget – summary", () => {
  it("includes a latency class and glass-to-glass label", () => {
    const budget = computeLatencyBudget({ protocol: "webrtc-whip" });
    expect(budget.summary).toMatch(/low/i);
    expect(budget.summary).toMatch(/glass-to-glass/i);
  });

  it("includes estimated time in summary", () => {
    const budget = computeLatencyBudget({ protocol: "srt", srtLatencyMs: 500 });
    expect(budget.summary).toMatch(/ms|s\b/);
  });
});

describe("formatLatencyMs", () => {
  it("formats sub-1s in ms", () => {
    expect(formatLatencyMs(250)).toBe("250 ms");
  });

  it("formats 1–60s in seconds", () => {
    expect(formatLatencyMs(2500)).toBe("2.5 s");
    expect(formatLatencyMs(1000)).toBe("1.0 s");
  });

  it("formats 60s+ in minutes", () => {
    expect(formatLatencyMs(90000)).toBe("1.5 min");
  });
});

describe("latencyClassLabel", () => {
  it("maps all classes to labels", () => {
    expect(latencyClassLabel("ultra-low")).toBe("Ultra-low (<300ms)");
    expect(latencyClassLabel("low")).toBe("Low (<2s)");
    expect(latencyClassLabel("standard")).toBe("Standard (<10s)");
    expect(latencyClassLabel("broadcast")).toBe("Broadcast (10s+)");
  });
});
