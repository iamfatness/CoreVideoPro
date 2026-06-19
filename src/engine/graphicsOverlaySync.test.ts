import { describe, expect, it } from "vitest";
import type { NativeMediaCoreSnapshot } from "./nativeMediaCoreProtocol";
import {
  appendCaptionTranscriptFromSnapshot,
  MAX_TRANSCRIPT_ENTRIES,
  resolveOverlayEnabledFlags
} from "./graphicsOverlaySync";

function buildSnapshot(overlayIds: string[]): NativeMediaCoreSnapshot {
  return {
    overlayCount: overlayIds.length,
    renderPlan: {
      renderPlanId: "test-plan",
      layers: overlayIds.map((overlayId, index) => ({
        layerId: `overlay:${overlayId}`,
        kind: "overlay" as const,
        overlayId,
        order: index + 1
      })),
      routes: []
    }
  } as NativeMediaCoreSnapshot;
}

describe("resolveOverlayEnabledFlags", () => {
  it("preserves local graphics when the engine is off", () => {
    const snapshot = buildSnapshot(["brand-bug"]);
    expect(resolveOverlayEnabledFlags(snapshot, ["brand-bug", "live-banner"], false)).toBeNull();
  });

  it("reflects the render plan when the engine is on", () => {
    const snapshot = buildSnapshot(["brand-bug"]);
    const flags = resolveOverlayEnabledFlags(snapshot, ["brand-bug", "live-banner"], true);

    expect(flags).toEqual({ "brand-bug": true, "live-banner": false });
  });
});

describe("appendCaptionTranscriptFromSnapshot", () => {
  it("appends cues and dedupes repeats", () => {
    const snapshot = {
      captionTrack: {
        status: "live",
        currentCue: {
          text: "Welcome to the show.",
          speaker: "Sophia Martinez",
          atMs: 1200,
          confidence: 94
        }
      }
    } as NativeMediaCoreSnapshot;

    const first = appendCaptionTranscriptFromSnapshot(snapshot, [], { "Sophia Martinez": "Host" });
    const second = appendCaptionTranscriptFromSnapshot(snapshot, first, { "Sophia Martinez": "Host" });

    expect(second).toHaveLength(1);
    expect(second[0]).toMatchObject({
      speakerName: "Sophia Martinez",
      role: "Host",
      text: "Welcome to the show.",
      confidence: 94
    });
    expect(second).toEqual(first);
  });

  it("caps rolling history", () => {
    let transcript: ReturnType<typeof appendCaptionTranscriptFromSnapshot> = [];
    for (let index = 0; index < MAX_TRANSCRIPT_ENTRIES + 2; index++) {
      transcript = appendCaptionTranscriptFromSnapshot(
        {
          captionTrack: {
            status: "live",
            currentCue: {
              text: `Line ${index}`,
              speaker: "Speaker",
              atMs: index * 1000,
              confidence: 90
            }
          }
        } as NativeMediaCoreSnapshot,
        transcript
      );
    }

    expect(transcript).toHaveLength(MAX_TRANSCRIPT_ENTRIES);
    expect(transcript[0]?.text).toBe("Line 2");
    expect(transcript.at(-1)?.text).toBe("Line 7");
  });
});