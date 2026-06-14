import { describe, expect, it } from "vitest";
import {
  describeSrtConnection,
  effectiveSrtBitrateKbps,
  estimateSrtOverheadPercent,
  parseSrtUrl,
  recommendSrtLatency,
  srtKeyLengthLabel,
} from "./srtOutput";

describe("parseSrtUrl", () => {
  it("rejects empty input", () => {
    expect(parseSrtUrl("").valid).toBe(false);
    expect(parseSrtUrl("").errors[0]).toMatch(/required/i);
  });

  it("rejects non-srt scheme", () => {
    const result = parseSrtUrl("rtmp://server.example.com:1935");
    expect(result.valid).toBe(false);
    expect(result.errors[0]).toMatch(/srt:\/\//);
  });

  it("rejects missing port", () => {
    const result = parseSrtUrl("srt://server.example.com");
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes("port"))).toBe(true);
  });

  it("parses a minimal valid caller URL", () => {
    const result = parseSrtUrl("srt://media.example.com:9001");
    expect(result.valid).toBe(true);
    expect(result.params?.host).toBe("media.example.com");
    expect(result.params?.port).toBe(9001);
    expect(result.params?.mode).toBe("caller");
    expect(result.params?.latencyMs).toBe(120);
    expect(result.params?.passphrase).toBeNull();
    expect(result.params?.keyLength).toBe(16);
  });

  it("parses listener mode from URL", () => {
    const result = parseSrtUrl("srt://0.0.0.0:9001?mode=listener");
    expect(result.valid).toBe(true);
    expect(result.params?.mode).toBe("listener");
    expect(result.warnings.some((w) => w.includes("Listener mode"))).toBe(true);
  });

  it("parses latency from URL parameter", () => {
    const result = parseSrtUrl("srt://server.example.com:9001?latency=500");
    expect(result.valid).toBe(true);
    expect(result.params?.latencyMs).toBe(500);
  });

  it("warns when latency is very low", () => {
    const result = parseSrtUrl("srt://server.example.com:9001?latency=10");
    expect(result.valid).toBe(true);
    expect(result.warnings.some((w) => w.includes("very low"))).toBe(true);
  });

  it("warns when latency is extremely high", () => {
    const result = parseSrtUrl("srt://server.example.com:9001?latency=9000");
    expect(result.valid).toBe(true);
    expect(result.warnings.some((w) => w.includes("8 s"))).toBe(true);
  });

  it("rejects passphrase shorter than 10 characters", () => {
    const result = parseSrtUrl("srt://server.example.com:9001?passphrase=short");
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes("10 characters"))).toBe(true);
  });

  it("rejects passphrase longer than 79 characters", () => {
    const longKey = "a".repeat(80);
    const result = parseSrtUrl(`srt://server.example.com:9001?passphrase=${longKey}`);
    expect(result.valid).toBe(false);
    expect(result.errors.some((e) => e.includes("79"))).toBe(true);
  });

  it("accepts a valid passphrase and defaults AES-128", () => {
    const result = parseSrtUrl("srt://server.example.com:9001?passphrase=securepass123");
    expect(result.valid).toBe(true);
    expect(result.params?.passphrase).toBe("securepass123");
    expect(result.params?.keyLength).toBe(16);
  });

  it("parses AES-256 key length", () => {
    const result = parseSrtUrl("srt://server.example.com:9001?passphrase=securepass123&pbkeylen=32");
    expect(result.valid).toBe(true);
    expect(result.params?.keyLength).toBe(32);
  });

  it("warns and defaults on unknown pbkeylen", () => {
    const result = parseSrtUrl("srt://server.example.com:9001?pbkeylen=7");
    expect(result.valid).toBe(true);
    expect(result.warnings.some((w) => w.includes("pbkeylen"))).toBe(true);
    expect(result.params?.keyLength).toBe(16);
  });
});

describe("recommendSrtLatency", () => {
  it("returns minimum 120 ms for low RTT", () => {
    expect(recommendSrtLatency(10)).toBe(120);
    expect(recommendSrtLatency(40)).toBe(120);
  });

  it("returns 2.5× RTT for higher values", () => {
    expect(recommendSrtLatency(100)).toBe(250);
    expect(recommendSrtLatency(200)).toBe(500);
  });
});

describe("estimateSrtOverheadPercent", () => {
  it("returns baseline 3% at default latency", () => {
    expect(estimateSrtOverheadPercent(120)).toBe(3);
  });

  it("adds 0.5% per 500 ms, capped at 8%", () => {
    expect(estimateSrtOverheadPercent(500)).toBe(3.5);
    expect(estimateSrtOverheadPercent(1000)).toBe(4);
    expect(estimateSrtOverheadPercent(5000)).toBe(8);
  });
});

describe("effectiveSrtBitrateKbps", () => {
  it("subtracts overhead from target", () => {
    // 3% overhead at 120ms, target 6000 kbps → 6000 * 0.97 = 5820
    expect(effectiveSrtBitrateKbps(6000, 120)).toBe(5820);
  });
});

describe("describeSrtConnection", () => {
  it("describes an encrypted caller connection", () => {
    const desc = describeSrtConnection({
      host: "media.example.com",
      port: 9001,
      mode: "caller",
      latencyMs: 120,
      passphrase: "securepass123",
      keyLength: 16,
    });
    expect(desc).toContain("media.example.com:9001");
    expect(desc).toContain("caller");
    expect(desc).toContain("AES-128");
    expect(desc).toContain("120 ms");
  });

  it("describes an unencrypted connection", () => {
    const desc = describeSrtConnection({
      host: "server",
      port: 1234,
      mode: "listener",
      latencyMs: 500,
      passphrase: null,
      keyLength: 16,
    });
    expect(desc).toContain("unencrypted");
    expect(desc).toContain("listener");
  });
});

describe("srtKeyLengthLabel", () => {
  it("formats key length as AES bit label", () => {
    expect(srtKeyLengthLabel(16)).toBe("AES-128");
    expect(srtKeyLengthLabel(24)).toBe("AES-192");
    expect(srtKeyLengthLabel(32)).toBe("AES-256");
  });
});
