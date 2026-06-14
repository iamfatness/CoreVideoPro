import { describe, expect, it } from "vitest";
import type { OutputDestination } from "../domain/production";
import { evaluateDestinationReadiness, summarizeArming } from "./outputArming";

function destination(overrides: Partial<OutputDestination>): OutputDestination {
  return {
    id: "dest",
    name: "Destination",
    protocol: "RTMP",
    enabled: true,
    latencyMs: 2100,
    endpoint: "rtmp://live.example.com/app",
    streamKey: "key",
    ...overrides
  };
}

describe("evaluateDestinationReadiness", () => {
  it("arms a complete RTMP destination", () => {
    const result = evaluateDestinationReadiness(destination({}));
    expect(result.ready).toBe(true);
    expect(result.statusLabel).toBe("Ready");
  });

  it("flags an RTMP destination missing its stream key", () => {
    const result = evaluateDestinationReadiness(destination({ streamKey: "" }));
    expect(result.ready).toBe(false);
    expect(result.statusLabel).toBe("Stream key");
    expect(result.detail).toMatch(/is missing a stream key/);
  });

  it("treats NDI as ready once a source name is present, key not required", () => {
    const named = evaluateDestinationReadiness(
      destination({ id: "ndi", name: "NDI Program", protocol: "NDI", endpoint: "CoreVideo Pro Program", streamKey: undefined })
    );
    expect(named.ready).toBe(true);
    expect(named.detail).toMatch(/local network/);

    const unnamed = evaluateDestinationReadiness(
      destination({ id: "ndi", name: "NDI Program", protocol: "NDI", endpoint: "  ", streamKey: undefined })
    );
    expect(unnamed.ready).toBe(false);
    expect(unnamed.statusLabel).toBe("Source name");
  });

  it("requires an srt:// endpoint and a long enough passphrase for SRT", () => {
    const wrongScheme = evaluateDestinationReadiness(
      destination({ id: "srt", protocol: "SRT", endpoint: "udp://host:9000", streamKey: undefined })
    );
    expect(wrongScheme.ready).toBe(false);
    expect(wrongScheme.detail).toMatch(/must start with srt:\/\//);

    const shortPassphrase = evaluateDestinationReadiness(
      destination({ id: "srt", protocol: "SRT", endpoint: "srt://backup.example.com:9000?mode=caller", streamKey: "short" })
    );
    expect(shortPassphrase.ready).toBe(false);
    expect(shortPassphrase.statusLabel).toBe("Passphrase");

    const armed = evaluateDestinationReadiness(
      destination({ id: "srt", protocol: "SRT", endpoint: "srt://backup.example.com:9000?mode=caller", streamKey: "longpassphrase" })
    );
    expect(armed.ready).toBe(true);
    expect(armed.detail).toMatch(/encrypted passphrase/);
  });

  it("allows an SRT caller without a passphrase (unencrypted)", () => {
    const result = evaluateDestinationReadiness(
      destination({ id: "srt", protocol: "SRT", endpoint: "srt://backup.example.com:9000?mode=caller", streamKey: undefined })
    );
    expect(result.ready).toBe(true);
  });

  it("requires an endpoint and stream key for WebRTC", () => {
    const result = evaluateDestinationReadiness(
      destination({ id: "webrtc", protocol: "WebRTC", endpoint: "https://monitor.example.com", streamKey: "" })
    );
    expect(result.ready).toBe(false);
    expect(result.statusLabel).toBe("Stream key");
  });
});

describe("summarizeArming", () => {
  it("reports no destinations armed when all are disabled", () => {
    const summary = summarizeArming([destination({ enabled: false })]);
    expect(summary.armedCount).toBe(0);
    expect(summary.summary).toBe("No destinations armed");
  });

  it("counts ready destinations and the protocol mix", () => {
    const summary = summarizeArming([
      destination({ id: "rtmp", protocol: "RTMP" }),
      destination({ id: "ndi", name: "NDI Program", protocol: "NDI", endpoint: "CoreVideo Pro Program", streamKey: undefined }),
      destination({ id: "srt", name: "SRT Backup", protocol: "SRT", endpoint: "srt://backup.example.com:9000?mode=caller", streamKey: "longpassphrase" }),
      destination({ id: "off", enabled: false })
    ]);

    expect(summary.armedCount).toBe(3);
    expect(summary.readyCount).toBe(3);
    expect(summary.protocolCounts).toEqual({ RTMP: 1, NDI: 1, SRT: 1, WebRTC: 0 });
    expect(summary.summary).toBe("3/3 ready - 1 RTMP, 1 NDI, 1 SRT");
    expect(summary.blockers).toHaveLength(0);
  });

  it("collects blockers for destinations that are not ready", () => {
    const summary = summarizeArming([
      destination({ id: "rtmp", protocol: "RTMP", streamKey: "" }),
      destination({ id: "srt", protocol: "SRT", endpoint: "srt://host:9000?mode=caller", streamKey: "short" })
    ]);

    expect(summary.readyCount).toBe(0);
    expect(summary.blockers).toHaveLength(2);
    expect(summary.summary).toBe("0/2 ready - 1 RTMP, 1 SRT");
  });
});
