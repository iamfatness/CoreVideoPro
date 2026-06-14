import { describe, expect, it } from "vitest";
import {
  evaluateFeedHealth,
  feedHealthBadgeColor,
  summarizeRosterHealth,
  type FeedHealthSignal,
} from "./feedHealth";

const liveSignal: FeedHealthSignal = {
  hasVideo: true,
  hasAudio: true,
  isMuted: false,
  isVideoOff: false,
  resolutionTier: 2,
  msSinceLastFrame: 50,
  isReconnecting: false,
};

describe("evaluateFeedHealth", () => {
  it("returns waiting when no frames received", () => {
    const status = evaluateFeedHealth({ ...liveSignal, msSinceLastFrame: undefined });
    expect(status.state).toBe("waiting");
    expect(status.usable).toBe(false);
    expect(status.needsAttention).toBe(false);
  });

  it("returns live for a healthy 1080p feed", () => {
    const status = evaluateFeedHealth(liveSignal);
    expect(status.state).toBe("live");
    expect(status.usable).toBe(true);
    expect(status.needsAttention).toBe(false);
  });

  it("returns live with 4K detail for tier-3 feed", () => {
    const status = evaluateFeedHealth({ ...liveSignal, resolutionTier: 3 });
    expect(status.state).toBe("live");
    expect(status.detail).toContain("4K");
  });

  it("returns low-resolution for 720p feed", () => {
    const status = evaluateFeedHealth({ ...liveSignal, resolutionTier: 1 });
    expect(status.state).toBe("low-resolution");
    expect(status.usable).toBe(true);
    expect(status.label).toBe("Low res");
  });

  it("returns muted when participant is muted", () => {
    const status = evaluateFeedHealth({ ...liveSignal, isMuted: true });
    expect(status.state).toBe("muted");
    expect(status.usable).toBe(true);
    expect(status.needsAttention).toBe(false);
  });

  it("returns video-off when camera is disabled", () => {
    const status = evaluateFeedHealth({ ...liveSignal, isVideoOff: true });
    expect(status.state).toBe("video-off");
    expect(status.usable).toBe(false);
  });

  it("returns video-off when hasVideo is false", () => {
    const status = evaluateFeedHealth({ ...liveSignal, hasVideo: false, resolutionTier: 0 });
    expect(status.state).toBe("video-off");
  });

  it("returns stale between 3s and 10s without frames", () => {
    const status = evaluateFeedHealth({ ...liveSignal, msSinceLastFrame: 5000 });
    expect(status.state).toBe("stale");
    expect(status.needsAttention).toBe(true);
    expect(status.usable).toBe(false);
    expect(status.detail).toContain("5.0 s");
  });

  it("returns disconnected after 10s without frames", () => {
    const status = evaluateFeedHealth({ ...liveSignal, msSinceLastFrame: 12000 });
    expect(status.state).toBe("disconnected");
    expect(status.needsAttention).toBe(true);
    expect(status.detail).toContain("12 s");
  });

  it("returns recovering when isReconnecting is true", () => {
    const status = evaluateFeedHealth({ ...liveSignal, isReconnecting: true });
    expect(status.state).toBe("recovering");
    expect(status.needsAttention).toBe(true);
    expect(status.usable).toBe(false);
  });

  it("prioritises reconnecting over stale gap", () => {
    const status = evaluateFeedHealth({ ...liveSignal, msSinceLastFrame: 6000, isReconnecting: true });
    expect(status.state).toBe("recovering");
  });
});

describe("summarizeRosterHealth", () => {
  it("returns 'No participants' for empty roster", () => {
    const summary = summarizeRosterHealth([]);
    expect(summary.summary).toBe("No participants");
    expect(summary.worstState).toBeNull();
  });

  it("reports all live when everyone is healthy", () => {
    const statuses = [evaluateFeedHealth(liveSignal), evaluateFeedHealth(liveSignal)];
    const summary = summarizeRosterHealth(statuses);
    expect(summary.live).toBe(2);
    expect(summary.needsAttention).toBe(0);
    expect(summary.summary).toBe("2/2 live");
  });

  it("flags disconnected participant in summary", () => {
    const statuses = [
      evaluateFeedHealth(liveSignal),
      evaluateFeedHealth({ ...liveSignal, msSinceLastFrame: 15000 }),
    ];
    const summary = summarizeRosterHealth(statuses);
    expect(summary.live).toBe(1);
    expect(summary.needsAttention).toBe(1);
    expect(summary.worstState).toBe("disconnected");
    expect(summary.summary).toContain("needs attention");
  });

  it("correctly counts usable feeds", () => {
    const statuses = [
      evaluateFeedHealth(liveSignal), // usable
      evaluateFeedHealth({ ...liveSignal, resolutionTier: 1 }), // usable (low-res)
      evaluateFeedHealth({ ...liveSignal, msSinceLastFrame: 15000 }), // not usable
    ];
    const summary = summarizeRosterHealth(statuses);
    expect(summary.usable).toBe(2);
  });
});

describe("feedHealthBadgeColor", () => {
  it("returns green for live", () => {
    expect(feedHealthBadgeColor("live")).toBe("green");
  });

  it("returns red for disconnected", () => {
    expect(feedHealthBadgeColor("disconnected")).toBe("red");
  });

  it("returns yellow for muted or low-resolution", () => {
    expect(feedHealthBadgeColor("muted")).toBe("yellow");
    expect(feedHealthBadgeColor("low-resolution")).toBe("yellow");
  });
});
