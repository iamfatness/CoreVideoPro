import { describe, expect, it, vi } from "vitest";
import { initialProduction } from "../domain/production";
import { mapCaptureSnapshot } from "./captureSnapshotMapper";
import type { AutoProductionSignals, ZoomSessionSnapshot } from "./contracts";
import {
  HttpDirectorIntelligenceService,
  redactSignalsForPrivacy,
  type AutoProductionProposalRequest
} from "./directorIntelligenceService";

function snapshot(): ZoomSessionSnapshot {
  return mapCaptureSnapshot({
    meetingState: "in_meeting",
    tick: 1,
    activeSpeakerId: "p1",
    participants: initialProduction.participants.slice(0, 3).map((participant, index) => ({
      userId: participant.id,
      displayName: participant.name,
      role: participant.role,
      talking: index === 0,
      networkQuality: "good"
    }))
  });
}

function request(signals?: AutoProductionSignals): AutoProductionProposalRequest {
  return { state: initialProduction, snapshot: snapshot(), signals };
}

const FULL_SIGNALS: AutoProductionSignals = {
  speakerTurns: [{ participantId: "p1", startedAtMs: 0, durationMs: 1200 }],
  sentiment: "positive",
  screenShareContent: "slides",
  engagement: 72
};

describe("redactSignalsForPrivacy", () => {
  it("keeps all signals when content is allowed", () => {
    expect(redactSignalsForPrivacy(FULL_SIGNALS, true)).toEqual(FULL_SIGNALS);
  });

  it("strips content-bearing signals (sentiment, screen-share content) by default", () => {
    const redacted = redactSignalsForPrivacy(FULL_SIGNALS, false);
    expect(redacted).toEqual({
      speakerTurns: FULL_SIGNALS.speakerTurns,
      engagement: 72
    });
    expect(redacted).not.toHaveProperty("sentiment");
    expect(redacted).not.toHaveProperty("screenShareContent");
  });

  it("returns undefined for undefined input", () => {
    expect(redactSignalsForPrivacy(undefined, true)).toBeUndefined();
  });
});

describe("HttpDirectorIntelligenceService", () => {
  it("returns an empty proposal (no service call) when serviceUrl is absent", async () => {
    const fetchImpl = vi.fn();
    const service = new HttpDirectorIntelligenceService({}, fetchImpl as unknown as typeof fetch);
    const result = await service.propose(request());
    expect(result).toEqual({});
    expect(fetchImpl).not.toHaveBeenCalled();
  });

  it("returns an empty proposal on a non-2xx response", async () => {
    const fetchImpl = vi.fn(async () => new Response("nope", { status: 502 }));
    const service = new HttpDirectorIntelligenceService(
      { serviceUrl: "https://director.example" },
      fetchImpl as unknown as typeof fetch
    );
    const result = await service.propose(request());
    expect(result).toEqual({});
  });

  it("returns an empty proposal when the network call throws (timeout/abort)", async () => {
    const fetchImpl = vi.fn(async () => {
      throw new DOMException("aborted", "AbortError");
    });
    const service = new HttpDirectorIntelligenceService(
      { serviceUrl: "https://director.example", timeoutMs: 5 },
      fetchImpl as unknown as typeof fetch
    );
    const result = await service.propose(request());
    expect(result).toEqual({});
  });

  it("sanitizes a valid proposal from the service", async () => {
    const fetchImpl = vi.fn(async () =>
      Response.json({
        proposal: { ruleId: "panel-discussion", recommendedSceneId: "panel", confidence: 250, rationale: "ok" },
        modelId: "claude-opus-4-8"
      })
    );
    const service = new HttpDirectorIntelligenceService(
      { serviceUrl: "https://director.example/" },
      fetchImpl as unknown as typeof fetch
    );
    const result = await service.propose(request());
    expect(result.proposal).toEqual({
      ruleId: "panel-discussion",
      recommendedSceneId: "panel",
      confidence: 100,
      rationale: "ok"
    });
    expect(result.modelId).toBe("claude-opus-4-8");
  });

  it("drops content signals from the request body unless opted in", async () => {
    let sentBody: { signals?: AutoProductionSignals } = {};
    const fetchImpl = vi.fn(async (_url: string, init: RequestInit) => {
      sentBody = JSON.parse(String(init.body));
      return Response.json({ proposal: undefined });
    });
    const service = new HttpDirectorIntelligenceService(
      { serviceUrl: "https://director.example", allowContentSignals: false },
      fetchImpl as unknown as typeof fetch
    );
    await service.propose(request(FULL_SIGNALS));
    expect(sentBody.signals).not.toHaveProperty("sentiment");
    expect(sentBody.signals).not.toHaveProperty("screenShareContent");
    expect(sentBody.signals?.engagement).toBe(72);
  });

  it("attaches the bearer key when configured", async () => {
    let sentHeaders: Record<string, string> = {};
    const fetchImpl = vi.fn(async (_url: string, init: RequestInit) => {
      sentHeaders = (init.headers as Record<string, string>) ?? {};
      return Response.json({ proposal: undefined });
    });
    const service = new HttpDirectorIntelligenceService(
      { serviceUrl: "https://director.example", apiKey: "sek-123" },
      fetchImpl as unknown as typeof fetch
    );
    await service.propose(request());
    expect(sentHeaders.authorization).toBe("Bearer sek-123");
  });
});
